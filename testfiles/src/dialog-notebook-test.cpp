// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/window.h>
#include <gtkmm/notebook.h>
#include <giomm/simpleaction.h>
#include <glibmm/fileutils.h>
#include <glib/gstdio.h>

#include <cstdlib>
#include <algorithm>
#include <csignal>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "desktop.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "io/resource.h"
#include "actions/actions-undo-document.h"
#include "ui/dialog/dialog-base.h"
#include "ui/dialog/dialog-notebook.h"
#include "ui/dialog/dialog-multipaned.h"
#include "ui/dialog/dialog-window.h"
#include "ui/dialog/inkscape-preferences.h"
#include "util/scope_exit.h"

#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/scale.h>
#include <gtkmm/settings.h>
#include <glibmm/i18n.h>

#include "inkscape.h"
#include "preferences.h"

namespace {

class TestApplication : public InkscapeApplication
{
public:
    bool ownsWindow(InkscapeWindow const *window) const
    {
        for (auto const &owned : _windows) {
            if (owned.get() == window) return true;
        }
        return false;
    }
};

TestApplication &testApplication()
{
    static auto application = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "dialognotebooktest", true);
        auto result = new TestApplication();
        result->gio_app()->register_application();
        // Preserve fatal failures (including shutdown) as test failures rather
        // than waiting in the application's interactive emergency dialog.
        for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
            std::signal(signal, SIG_DFL);
        }
#ifndef _WIN32
        std::signal(SIGBUS, SIG_DFL);
#endif
        return result;
    }();
    return *application;
}

void drainMainContext()
{
    for (unsigned i = 0; i < 10000 && g_main_context_pending(nullptr); ++i) {
        g_main_context_iteration(nullptr, false);
    }
}

void pumpFrames()
{
    // GTK relocates focus after unparenting during the next after-paint phase,
    // not necessarily in an already-pending idle. Keep the live window running.
    auto loop = g_main_loop_new(nullptr, false);
    g_timeout_add(100, [](gpointer data) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop *>(data));
        return G_SOURCE_REMOVE;
    }, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

std::string focusPath(Gtk::Window &window)
{
    std::string path;
    for (auto widget = gtk_root_get_focus(GTK_ROOT(window.gobj())); widget;
         widget = gtk_widget_get_parent(widget)) {
        if (!path.empty()) path += " -> ";
        path += G_OBJECT_TYPE_NAME(widget);
        path += ':';
        path += gtk_widget_get_name(widget);
        path += "[mapped=" + std::to_string(gtk_widget_get_mapped(widget));
        path += ",visible=" + std::to_string(gtk_widget_is_visible(widget)) + ']';
    }
    return path.empty() ? "<none>" : path;
}

class WindowLifecycleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        auto &application = testApplication();
        document = application.document_add(SPDocument::createNewDocFromMem(
            std::string_view{"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"/>"}));
        ASSERT_TRUE(document);
        desktop = application.createDesktop(document, false, true);
        ASSERT_TRUE(desktop);
        drainMainContext();
    }

    void TearDown() override
    {
        if (document) document->setModifiedSinceSave(false);
        if (desktop) testApplication().destroyDesktop(desktop);
        else if (document) testApplication().document_close(document);
        desktop = nullptr;
        document = nullptr;
        drainMainContext();
        dialog_state_restore.reset(); // Closing windows can save dialog state.
    }

    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    std::shared_ptr<void> dialog_state_restore;
};

class DialogNotebookTest : public WindowLifecycleTest,
                           public ::testing::WithParamInterface<std::tuple<bool, int, bool>> {};
class FloatingDialogLifecycleTest : public WindowLifecycleTest,
                                    public ::testing::WithParamInterface<bool> {};

template <typename T>
T *findChild(Gtk::Widget &root)
{
    if (auto found = dynamic_cast<T *>(&root)) return found;
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto found = findChild<T>(*child)) return found;
    }
    return nullptr;
}

// Minimal test-local traversal that matches a real labeled child (buttons and
// check buttons) without adding a production test seam.
template <typename T>
T *findChildByLabel(Gtk::Widget &root, Glib::ustring const &label)
{
    if (auto found = dynamic_cast<T *>(&root)) {
        if (found->get_label() == label) return found;
    }
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto found = findChildByLabel<T>(*child, label)) return found;
    }
    return nullptr;
}

TEST_F(WindowLifecycleTest, RestoringDockedDialogsDoesNotPresentHiddenRoot)
{
    using namespace Inkscape::UI::Dialog;
    Gtk::Window host;
    host.set_default_size(700, 500);
    DialogContainer container(desktop->getInkscapeWindow());
    host.set_child(container);
    gtk_widget_realize(GTK_WIDGET(host.gobj()));
    ASSERT_FALSE(host.get_visible());
    ASSERT_FALSE(host.get_mapped());
    unsigned visible_events = 0;
    unsigned map_events = 0;
    sigc::scoped_connection visible = host.property_visible().signal_changed().connect([&] {
        if (host.get_visible()) ++visible_events;
    });
    sigc::scoped_connection mapped = host.signal_map().connect([&] { ++map_events; });

    // Exercise the production factory, icon-bearing tabs and saved active-tab
    // path. The KeyFile is in memory: never read/write the user's dialog state.
    auto state = Glib::KeyFile::create();
    state->load_from_data(
        "[Windows]\nCount=1\n"
        "[Window0]\nColumnCount=1\nFloating=false\n"
        "[Window0Column0]\nNotebookCount=1\nBeforeCanvas=false\n"
        "Notebook0Dialogs=FillStroke;AlignDistribute;\nNotebook0ActiveTab=0\n");
    container.load_container_state(state.get(), false);
    EXPECT_EQ(visible_events, 0u);
    EXPECT_EQ(map_events, 0u);
    EXPECT_FALSE(host.get_visible());
    EXPECT_FALSE(host.get_mapped());
    auto notebook = findChild<DialogNotebook>(container);
    ASSERT_TRUE(notebook);
    auto pages = notebook->get_notebook();
    auto fill = container.get_dialog("FillStroke");
    auto align = container.get_dialog("AlignDistribute");
    ASSERT_TRUE(fill && align);
    ASSERT_EQ(pages->get_n_pages(), 2);
    EXPECT_EQ(pages->get_nth_page(0), fill);
    EXPECT_EQ(pages->get_nth_page(1), align);
    EXPECT_EQ(pages->get_current_page(), 0);
    auto tabs = findChild<Inkscape::UI::Widget::TabStrip>(container);
    ASSERT_TRUE(tabs && tabs->get_tab_at(0));
    EXPECT_TRUE(tabs->is_tab_active(*tabs->get_tab_at(0)));

    // Hidden page selection still synchronizes tabs without raising the root.
    pages->set_current_page(1);
    EXPECT_TRUE(tabs->is_tab_active(*tabs->get_tab_at(1)));
    EXPECT_FALSE(host.get_visible());
    EXPECT_EQ(visible_events, 0u);

    // A deliberate open is different from restoration: it must still present
    // the hidden root and focus a usable control in the requested dialog.
    container.new_dialog("FillStroke");
    pumpFrames();
    EXPECT_TRUE(host.get_visible());
    EXPECT_TRUE(host.get_mapped());
    EXPECT_GT(map_events, 0u);
    EXPECT_EQ(pages->get_current_page(), 0);
    auto focus = gtk_root_get_focus(GTK_ROOT(host.gobj()));
    ASSERT_TRUE(focus);
    EXPECT_TRUE(gtk_widget_is_ancestor(focus, GTK_WIDGET(fill->gobj())));

    // The notebook-menu overload must reveal a collapsed docked destination.
    ASSERT_TRUE(notebook->get_parent());
    notebook->get_parent()->set_visible(false);
    container.new_dialog("AlignDistribute", notebook, true);
    pumpFrames();
    EXPECT_TRUE(notebook->get_mapped());
    EXPECT_EQ(pages->get_current_page(), 1);
    focus = gtk_root_get_focus(GTK_ROOT(host.gobj()));
    ASSERT_TRUE(focus);
    EXPECT_TRUE(gtk_widget_is_ancestor(focus, GTK_WIDGET(align->gobj())));

    // Actual tab-strip signal (not a private callback) exercises mapped focus.
    tabs->signal_select_tab().emit(*tabs->get_tab_at(1));
    pumpFrames();
    EXPECT_EQ(pages->get_current_page(), 1);
    focus = gtk_root_get_focus(GTK_ROOT(host.gobj()));
    ASSERT_TRUE(focus);
    EXPECT_TRUE(gtk_widget_is_ancestor(focus, GTK_WIDGET(align->gobj())));

    // Exercise the real scroll controller connected to the tab strip.
    auto controllers = gtk_widget_observe_controllers(tabs->Gtk::Widget::gobj());
    bool scrolled = false;
    for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_SCROLL(controller)) {
            gboolean handled = false;
            g_signal_emit_by_name(controller, "scroll", 0.0, -1.0, &handled);
            EXPECT_TRUE(handled);
            scrolled = true;
        }
        g_object_unref(controller);
    }
    g_object_unref(controllers);
    ASSERT_TRUE(scrolled);
    EXPECT_EQ(pages->get_current_page(), 0);
    focus = gtk_root_get_focus(GTK_ROOT(host.gobj()));
    ASSERT_TRUE(focus);
    EXPECT_TRUE(gtk_widget_is_ancestor(focus, GTK_WIDGET(fill->gobj())));

    host.unset_child();
    host.close();
}

TEST_P(FloatingDialogLifecycleTest, ReopeningHiddenDialogsRestoresVisibilityAndFocus)
{
    using namespace Inkscape::UI::Dialog;
    Gtk::Window host;
    host.set_default_size(700, 500);
    DialogContainer container(desktop->getInkscapeWindow());
    host.set_child(container);
    auto state = Glib::KeyFile::create();
    state->load_from_data(
        "[Windows]\nCount=1\n"
        "[Window0]\nColumnCount=1\nFloating=false\n"
        "[Window0Column0]\nNotebookCount=1\nBeforeCanvas=false\n"
        "Notebook0Dialogs=FillStroke;AlignDistribute;\nNotebook0ActiveTab=0\n");
    container.load_container_state(state.get(), false);
    host.present();
    pumpFrames();
    auto notebook = findChild<DialogNotebook>(container);
    auto fill = container.get_dialog("FillStroke");
    auto align = container.get_dialog("AlignDistribute");
    ASSERT_TRUE(notebook && fill && align);
    // Explicit undocking still shows the new floating window. A deliberate
    // reopen must raise it again even if its notebook is currently unmapped.
    std::unique_ptr<DialogWindow> floating(notebook->float_tab(*fill));
    ASSERT_TRUE(floating);
    pumpFrames();
    EXPECT_TRUE(floating->get_mapped());
    auto &manager = DialogManager::singleton();
    manager.set_floating_dialog_visibility(floating.get(), false);
    EXPECT_FALSE(floating->get_mapped());
    if (GetParam()) pumpFrames(); // Test settled and same-event-loop reopening.
    container.new_dialog("FillStroke");
    pumpFrames();
    EXPECT_TRUE(floating->get_mapped());
    EXPECT_EQ(fill->get_root(), floating.get());
    manager.set_floating_dialog_visibility(floating.get(), true);
    auto windows = manager.get_all_floating_dialog_windows();
    EXPECT_EQ(std::count(windows.begin(), windows.end(), floating.get()), 1);

    auto floating_notebook = findChild<DialogNotebook>(*floating->get_container());
    ASSERT_TRUE(floating_notebook);
    floating_notebook->move_page(*align);
    EXPECT_EQ(floating_notebook->get_notebook()->get_current_page(), 1);
    manager.set_floating_dialog_visibility(floating.get(), false);
    if (GetParam()) pumpFrames();
    container.new_dialog("FillStroke", nullptr, true); // notebook-menu route
    auto const focus_before_frame = focusPath(*floating);
    pumpFrames();
    EXPECT_TRUE(floating->get_mapped());
    EXPECT_EQ(floating_notebook->get_notebook()->get_current_page(), 0);
    auto focus = gtk_root_get_focus(GTK_ROOT(floating->gobj()));
    EXPECT_TRUE(focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(fill->gobj())))
        << "Before frame: " << focus_before_frame << "; after frame: " << focusPath(*floating);
    manager.set_floating_dialog_visibility(floating.get(), true);
    windows = manager.get_all_floating_dialog_windows();
    EXPECT_EQ(std::count(windows.begin(), windows.end(), floating.get()), 1);
    host.unset_child();
    host.close();
}

TEST_F(WindowLifecycleTest, DestroyingHiddenFloatingWindowRemovesManagerEntry)
{
    using namespace Inkscape::UI::Dialog;
    auto &manager = DialogManager::singleton();
    auto floating = std::make_unique<DialogWindow>(desktop->getInkscapeWindow());
    floating->set_visible(true);
    pumpFrames();
    auto pointer = floating.get();
    manager.set_floating_dialog_visibility(pointer, false);
    auto windows = manager.get_all_floating_dialog_windows();
    EXPECT_EQ(std::count(windows.begin(), windows.end(), pointer), 1);
    floating.reset();
    windows = manager.get_all_floating_dialog_windows();
    EXPECT_EQ(std::count(windows.begin(), windows.end(), pointer), 0);
}

TEST_F(WindowLifecycleTest, FocusSkipsUnavailableFirstControl)
{
    using namespace Inkscape::UI::Dialog;
    Gtk::Window host;
    Gtk::Box layout(Gtk::Orientation::VERTICAL);
    Gtk::Entry outside, disabled, target;
    DialogBase dialog("/dialogs/test-focus-available", "FillStroke");
    disabled.set_sensitive(false);
    dialog.append(disabled);
    dialog.append(target);
    layout.append(outside);
    layout.append(dialog);
    host.set_child(layout);
    auto cleanup = scope_exit{[&] {
        host.unset_child();
        host.close();
        pumpFrames();
    }};
    host.present();
    pumpFrames();
    ASSERT_TRUE(outside.grab_focus());
    ASSERT_EQ(gtk_widget_get_focus_child(GTK_WIDGET(dialog.gobj())), nullptr);

    dialog.focus_dialog();
    auto focus = gtk_root_get_focus(GTK_ROOT(host.gobj()));
    EXPECT_TRUE(focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(target.gobj())));
}

TEST_F(WindowLifecycleTest, FocusStopsWhenShowCallbackDestroysDialog)
{
    using namespace Inkscape::UI::Dialog;
    Gtk::Window host;
    auto dialog = std::make_unique<DialogBase>("/dialogs/test-focus", "FillStroke");
    dialog->append(*Gtk::make_managed<Gtk::Entry>());
    host.set_child(*dialog);
    sigc::scoped_connection showing = host.signal_show().connect([&] {
        host.unset_child();
        dialog.reset();
    });
    dialog->focus_dialog();
    EXPECT_FALSE(dialog);
    host.close();
}

TEST_F(WindowLifecycleTest, MenuOpenStopsWhenUncollapsingDestroysDestinationNotebook)
{
    using namespace Inkscape::UI::Dialog;
    Gtk::Window host;
    DialogContainer container(desktop->getInkscapeWindow());
    host.set_child(container);
    auto column_owner = container.create_column();
    auto column = column_owner.get();
    container.get_columns()->append(std::move(column_owner));
    auto target_owner = std::make_unique<DialogNotebook>(&container);
    auto target = target_owner.get();
    column->append(std::move(target_owner));
    auto survivor_owner = std::make_unique<DialogNotebook>(&container);
    auto survivor = survivor_owner.get();
    column->append(std::move(survivor_owner));
    column->set_visible(false);
    bool removed = false;
    sigc::scoped_connection visible = column->property_visible().signal_changed().connect([&] {
        if (!column->get_visible() || removed) return;
        removed = true;
        column->remove(*target);
    });
    container.new_dialog("FillStroke", target, true);
    EXPECT_TRUE(removed);
    EXPECT_EQ(container.get_dialog("FillStroke"), nullptr);
    EXPECT_EQ(survivor->get_parent(), column);
    visible.disconnect();
    host.unset_child();
    host.close();
}

TEST_F(WindowLifecycleTest, BlinkStopsWhenPageNotificationDestroysDialog)
{
    using namespace Inkscape::UI::Dialog;
    Gtk::Window host;
    DialogContainer container(desktop->getInkscapeWindow());
    auto notebook = std::make_unique<DialogNotebook>(&container);
    auto first = std::make_unique<DialogBase>("/dialogs/test-blink", "FillStroke");
    auto second = std::make_unique<DialogBase>("/dialogs/test-blink-other", "AlignDistribute");
    notebook->add_page(*first);
    notebook->add_page(*second);
    host.set_child(*notebook);
    auto pages = notebook->get_notebook();
    bool destroyed = false;
    sigc::scoped_connection changed = pages->property_page().signal_changed().connect([&] {
        if (destroyed || pages->get_current_page() != 0) return;
        destroyed = true;
        pages->remove_page(*first);
        first.reset();
    });
    first->blink();
    EXPECT_TRUE(destroyed);
    EXPECT_FALSE(first);
    EXPECT_EQ(pages->get_n_pages(), 1);
    changed.disconnect();
    pages->remove_page(*second);
    host.unset_child();
    host.close();
}

TEST_F(WindowLifecycleTest, RestoredMainWindowIsOwnedBeforeItIsShown)
{
    // CTest supplies this isolated profile. Scope the synthetic saved state and
    // restore its exact previous bytes even on an assertion's early return.
    auto const profile = std::getenv("INKSCAPE_PROFILE_DIR");
    ASSERT_TRUE(profile);
    ASSERT_TRUE(std::string_view(profile).ends_with("dialog-notebook-profile"));
    ASSERT_EQ(Inkscape::IO::Resource::profile_path(), profile);
    struct SavedState {
        std::string path;
        bool existed;
        std::string previous;
        explicit SavedState(std::string p)
            : path(std::move(p)), existed(Glib::file_test(path, Glib::FileTest::EXISTS))
            , previous(existed ? Glib::file_get_contents(path) : std::string{}) {}
        ~SavedState()
        {
            if (existed) {
                EXPECT_NO_THROW(Glib::file_set_contents(path, previous));
            } else {
                EXPECT_EQ(g_unlink(path.c_str()), 0);
            }
        }
    };
    auto saved = std::make_shared<SavedState>(Inkscape::IO::Resource::profile_path("dialogs-state-ex.ini"));
    dialog_state_restore = saved; // Survive the test body and all desktop closes.
    Glib::file_set_contents(saved->path,
        "[Windows]\nCount=1\n"
        "[Window0]\nColumnCount=1\nFloating=false\n"
        "[Window0Column0]\nNotebookCount=1\nBeforeCanvas=false\n"
        "Notebook0Dialogs=FillStroke;AlignDistribute;\nNotebook0ActiveTab=1\n");
    auto &application = testApplication();
    unsigned shows = 0;
    unsigned maps = 0;
    unsigned premature = 0;
    std::vector<sigc::scoped_connection> observations;
    sigc::scoped_connection added = application.gtk_app()->signal_window_added().connect([&](Gtk::Window *root) {
        if (auto window = dynamic_cast<InkscapeWindow *>(root)) {
            observations.emplace_back(window->signal_show().connect([&, window] {
                ++shows;
                if (!application.ownsWindow(window)) ++premature;
            }));
            observations.emplace_back(window->signal_map().connect([&, window] {
                ++maps;
                if (!application.ownsWindow(window)) ++premature;
            }));
        }
    });
    auto const original = desktop;
    desktop = application.createDesktop(document, false, true);
    application.desktopClose(original);
    ASSERT_TRUE(desktop);
    EXPECT_GT(shows, 0u);
    EXPECT_GT(maps, 0u);
    EXPECT_EQ(premature, 0u);
    auto fill = desktop->getContainer()->get_dialog("FillStroke");
    auto align = desktop->getContainer()->get_dialog("AlignDistribute");
    ASSERT_TRUE(fill && align);
    auto pages = Inkscape::UI::Dialog::DialogNotebook::get_page_notebook(*fill);
    ASSERT_TRUE(pages);
    EXPECT_EQ(pages->get_n_pages(), 2);
    EXPECT_EQ(pages->get_nth_page(0), fill);
    EXPECT_EQ(pages->get_nth_page(1), align);
    EXPECT_EQ(pages->get_current_page(), 1);
}

TEST_F(WindowLifecycleTest, DocumentActionsRemainUsableAfterClosingItsWindow)
{
    auto group = document->getActionGroup();
    auto undo = std::dynamic_pointer_cast<Gio::SimpleAction>(group->lookup_action("undo"));
    auto redo = std::dynamic_pointer_cast<Gio::SimpleAction>(group->lookup_action("redo"));
    ASSERT_TRUE(undo && redo);
    testApplication().desktopClose(desktop); // Keep the document alive deliberately.
    desktop = nullptr;
    enable_undo_actions(document, true, false);
    EXPECT_TRUE(undo->get_enabled());
    EXPECT_FALSE(redo->get_enabled());
    enable_undo_actions(document, false, true);
    EXPECT_FALSE(undo->get_enabled());
    EXPECT_TRUE(redo->get_enabled());
    pumpFrames();
}

TEST_F(WindowLifecycleTest, ClosingOneViewPreservesOtherViewsDocumentActions)
{
    auto closing = desktop;
    desktop = testApplication().createDesktop(document, false, true);
    ASSERT_TRUE(desktop);
    ASSERT_NE(desktop->getInkscapeWindow(), closing->getInkscapeWindow());
    auto group = document->getActionGroup();
    auto undo = std::dynamic_pointer_cast<Gio::SimpleAction>(group->lookup_action("undo"));
    ASSERT_TRUE(undo);
    testApplication().desktopClose(closing);
    enable_undo_actions(document, true, false);
    EXPECT_TRUE(undo->get_enabled());
#ifdef __APPLE__
    auto mirror = desktop->getInkscapeWindow()->lookup_action("undo");
    EXPECT_EQ(mirror, undo);
    EXPECT_TRUE(mirror->get_enabled());
#endif
    enable_undo_actions(document, false, true);
    EXPECT_FALSE(undo->get_enabled());
    pumpFrames();
}

// The desktop-independence policy is exercised through real Dialogs and GTK
// widgets rather than a boolean helper. A default panel must be disabled
// without a desktop, while Preferences (and anything overriding
// requiresDesktop()) stays usable. Real desktop destruction drives the same
// path as closing the last document.
class SelfDestroyingDialog : public Inkscape::UI::Dialog::DialogBase
{
public:
    SelfDestroyingDialog() : DialogBase("/dialogs/test-self-destroy", "FillStroke") {}

    void documentReplaced() override
    {
        if (owner) {
            auto *target = owner;
            owner = nullptr;
            target->reset();
        }
    }

    std::unique_ptr<SelfDestroyingDialog> *owner = nullptr;
};

TEST_F(WindowLifecycleTest, DesktopIndependencePolicyKeepsPreferencesUsable)
{
    using namespace Inkscape::UI::Dialog;

    DialogBase dependent("/dialogs/test-policy-dependent", "FillStroke");
    ASSERT_TRUE(dependent.requiresDesktop());
    EXPECT_TRUE(dependent.get_sensitive());

    // Explicitly setting the already-null desktop must still apply the policy.
    dependent.setDesktop(nullptr);
    EXPECT_FALSE(dependent.get_sensitive());
    // Repeating the null transition is stable.
    dependent.setDesktop(nullptr);
    EXPECT_FALSE(dependent.get_sensitive());
    // A valid desktop restores a dependent panel.
    dependent.setDesktop(desktop);
    EXPECT_TRUE(dependent.get_sensitive());
    // A repeated non-null desktop must not disturb a deliberately disabled panel.
    dependent.set_sensitive(false);
    dependent.setDesktop(desktop);
    EXPECT_FALSE(dependent.get_sensitive());
    dependent.set_sensitive(true);
    // Losing the desktop disables it again.
    dependent.setDesktop(nullptr);
    EXPECT_FALSE(dependent.get_sensitive());

    InkscapePreferences preferences;
    ASSERT_FALSE(preferences.requiresDesktop());
    EXPECT_TRUE(preferences.get_sensitive());
    preferences.setDesktop(nullptr);
    EXPECT_TRUE(preferences.get_sensitive());
    EXPECT_TRUE(gtk_widget_is_sensitive(GTK_WIDGET(preferences.gobj())));

    // Real desktop destruction: a bound dependent panel is disabled while the
    // bound desktop-independent Preferences panel stays usable.
    DialogBase extra_dependent("/dialogs/test-policy-extra-dependent", "AlignDistribute");
    auto *extra = testApplication().createDesktop(document, false, true);
    ASSERT_TRUE(extra);
    extra_dependent.setDesktop(extra);
    preferences.setDesktop(extra);
    ASSERT_TRUE(extra_dependent.get_sensitive());
    ASSERT_TRUE(preferences.get_sensitive());
    testApplication().destroyDesktop(extra);
    drainMainContext();
    EXPECT_FALSE(extra_dependent.get_sensitive());
    EXPECT_TRUE(preferences.get_sensitive());
    EXPECT_TRUE(gtk_widget_is_sensitive(GTK_WIDGET(preferences.gobj())));
}

// A dialog that destroys itself from documentReplaced() while detaching must not
// be dereferenced by the null-desktop policy path.
TEST_F(WindowLifecycleTest, NullDesktopTransitionSurvivesSelfDestruction)
{
    using namespace Inkscape::UI::Dialog;

    auto owner = std::make_unique<SelfDestroyingDialog>();
    owner->setDesktop(desktop);
    ASSERT_TRUE(owner);
    owner->owner = &owner;
    owner->setDesktop(nullptr);
    EXPECT_FALSE(owner);
}

// A floating DialogWindow must enable itself without a desktop only when one of
// its panels is desktop-independent; dependent panels stay insensitive, and
// reattaching restores the normal bindings.
TEST_F(WindowLifecycleTest, FloatingDialogWindowSensitivityFollowsDesktopIndependence)
{
    using namespace Inkscape::UI::Dialog;
    auto *inkscape_window = desktop->getInkscapeWindow();
    ASSERT_TRUE(inkscape_window);

    InkscapePreferences preferences;
    DialogBase dependent("/dialogs/test-policy-floating-dependent", "FillStroke");

    DialogWindow window(inkscape_window);
    auto *container = window.get_container();
    ASSERT_TRUE(container);
    auto column_owner = container->create_column();
    auto *column = column_owner.get();
    container->get_columns()->append(std::move(column_owner));
    auto notebook_owner = std::make_unique<DialogNotebook>(container);
    auto *notebook = notebook_owner.get();
    column->append(std::move(notebook_owner));
    notebook->add_page(preferences);
    notebook->add_page(dependent);

    // Native on_page_added already linked both panels; no manual link.
    ASSERT_EQ(preferences.get_root(), &window);
    ASSERT_NE(DialogNotebook::get_page_notebook(preferences), nullptr);
    ASSERT_EQ(container->get_dialog(preferences.get_type()), &preferences);
    ASSERT_EQ(container->get_dialog(dependent.get_type()), &dependent);

    // Attached: normal sensitivity.
    EXPECT_TRUE(window.get_sensitive());
    EXPECT_TRUE(preferences.get_sensitive());
    EXPECT_TRUE(dependent.get_sensitive());
    EXPECT_TRUE(gtk_widget_is_sensitive(GTK_WIDGET(preferences.gobj())));
    EXPECT_TRUE(gtk_widget_is_sensitive(GTK_WIDGET(dependent.gobj())));

    auto const windows_before = testApplication().gtk_app()->get_windows().size();
    auto *document_before = desktop->getDocument();
    window.set_inkscape_window(nullptr);

    // The floating parent remains usable only because Preferences is present;
    // the dependent panel is explicitly left insensitive.
    EXPECT_TRUE(window.get_sensitive());
    EXPECT_TRUE(preferences.get_sensitive());
    EXPECT_FALSE(dependent.get_sensitive());
    EXPECT_TRUE(gtk_widget_is_sensitive(GTK_WIDGET(preferences.gobj())));
    EXPECT_FALSE(gtk_widget_is_sensitive(GTK_WIDGET(dependent.gobj())));
    // The policy itself fabricates neither a window nor a document.
    EXPECT_EQ(testApplication().gtk_app()->get_windows().size(), windows_before);
    EXPECT_EQ(desktop->getDocument(), document_before);

    // Reattaching restores the dependent panel.
    window.set_inkscape_window(inkscape_window);
    EXPECT_TRUE(window.get_sensitive());
    EXPECT_TRUE(preferences.get_sensitive());
    EXPECT_TRUE(dependent.get_sensitive());

    // A floating window containing only dependent panels stays insensitive.
    DialogBase dependent_only("/dialogs/test-policy-floating-dependent-only", "AlignDistribute");
    DialogWindow dependent_window(inkscape_window);
    auto *dependent_container = dependent_window.get_container();
    ASSERT_TRUE(dependent_container);
    auto dep_column_owner = dependent_container->create_column();
    auto *dep_column = dep_column_owner.get();
    dependent_container->get_columns()->append(std::move(dep_column_owner));
    auto dep_notebook_owner = std::make_unique<DialogNotebook>(dependent_container);
    auto *dep_notebook = dep_notebook_owner.get();
    dep_column->append(std::move(dep_notebook_owner));
    dep_notebook->add_page(dependent_only);
    ASSERT_EQ(dependent_only.get_root(), &dependent_window);
    ASSERT_NE(DialogNotebook::get_page_notebook(dependent_only), nullptr);
    ASSERT_EQ(dependent_container->get_dialog(dependent_only.get_type()), &dependent_only);
    dependent_window.set_inkscape_window(nullptr);
    EXPECT_FALSE(dependent_window.get_sensitive());
    EXPECT_FALSE(dependent_only.get_sensitive());
}

// Both native action groups inserted by DialogWindow must stop routing when the
// window is detached, and must route again after a valid reattach. Injected
// probe actions make the assertions platform-independent while still exercising
// real GTK widget action routing.
TEST_F(WindowLifecycleTest, FloatingWindowDropsAndRestoresActionGroups)
{
    using namespace Inkscape::UI::Dialog;
    auto *inkscape_window = desktop->getInkscapeWindow();
    ASSERT_TRUE(inkscape_window);
    auto document_group = document->getActionGroup();
    ASSERT_TRUE(document_group);

    auto win_probe = Gio::SimpleAction::create("policy-probe");
    auto doc_probe = Gio::SimpleAction::create("policy-probe");
    inkscape_window->add_action(win_probe);
    document_group->add_action(doc_probe);
    auto cleanup = scope_exit{[&] {
        inkscape_window->remove_action("policy-probe");
        document_group->remove_action("policy-probe");
    }};

    // Mount an actual desktop-independent panel so the floating window remains
    // sensitive after detaching; the group policy must then be observable.
    InkscapePreferences preferences;
    DialogWindow window(inkscape_window);
    auto *container = window.get_container();
    ASSERT_TRUE(container);
    auto column_owner = container->create_column();
    auto *column = column_owner.get();
    container->get_columns()->append(std::move(column_owner));
    auto notebook_owner = std::make_unique<DialogNotebook>(container);
    auto *notebook = notebook_owner.get();
    column->append(std::move(notebook_owner));
    notebook->add_page(preferences);
    ASSERT_EQ(container->get_dialog(preferences.get_type()), &preferences);
    window.update_dialogs();

    int win_count = 0;
    int doc_count = 0;
    win_probe->signal_activate().connect([&](const Glib::VariantBase&) { ++win_count; });
    doc_probe->signal_activate().connect([&](const Glib::VariantBase&) { ++doc_count; });

    auto activate = [&](const char *name) {
        return gtk_widget_activate_action(GTK_WIDGET(window.gobj()), name, nullptr);
    };

    // Attached: both groups route exactly once.
    EXPECT_TRUE(activate("win.policy-probe"));
    EXPECT_TRUE(activate("doc.policy-probe"));
    EXPECT_EQ(win_count, 1);
    EXPECT_EQ(doc_count, 1);

    window.set_inkscape_window(nullptr);
    // Detached but still sensitive because Preferences remains mounted; the
    // stale groups must not route and must not fire callbacks again.
    EXPECT_TRUE(window.get_sensitive());
    EXPECT_FALSE(activate("win.policy-probe"));
    EXPECT_FALSE(activate("doc.policy-probe"));
    EXPECT_EQ(win_count, 1);
    EXPECT_EQ(doc_count, 1);

    window.set_inkscape_window(inkscape_window);
    // Reattached: both groups route again.
    EXPECT_TRUE(activate("win.policy-probe"));
    EXPECT_TRUE(activate("doc.policy-probe"));
    EXPECT_EQ(win_count, 2);
    EXPECT_EQ(doc_count, 2);
}

// CTest runs this executable with G_DEBUG=fatal-criticals: a GTK root/focus or
// lifetime critical is a failed test, not merely a warning hidden by exit 0.
TEST_P(DialogNotebookTest, DestroyFocusedPagesWithAndWithoutRoot)
{
    auto const [unroot_first, page_count, external_focus] = GetParam();
    Gtk::Window host;
    host.set_default_size(500, 300);
    Gtk::Box content(Gtk::Orientation::VERTICAL);
    Gtk::Entry survivor;
    auto notebook = std::make_unique<Inkscape::UI::Dialog::DialogNotebook>(desktop->getContainer());
    auto pages = notebook->get_notebook();
    Gtk::Entry *last_entry = nullptr;
    for (int i = 0; i < page_count; ++i) {
        auto page = Gtk::make_managed<Gtk::Box>();
        auto entry = Gtk::make_managed<Gtk::Entry>();
        entry->set_text("Focus must not move to another page during destruction");
        page->append(*entry);
        pages->append_page(*page);
        last_entry = entry;
    }
    content.append(*notebook);
    content.append(survivor);
    host.set_child(content);
    host.present();
    drainMainContext();

    if (last_entry) {
        pages->set_current_page(page_count - 1);
        ASSERT_TRUE(last_entry->grab_focus());
        auto focus = gtk_root_get_focus(GTK_ROOT(host.gobj()));
        ASSERT_TRUE(focus);
        EXPECT_TRUE(focus == GTK_WIDGET(last_entry->gobj()) ||
                    gtk_widget_is_ancestor(focus, GTK_WIDGET(last_entry->gobj())));
        ASSERT_TRUE(gtk_widget_get_focus_child(GTK_WIDGET(pages->gobj())));
    }

    if (external_focus || !last_entry) ASSERT_TRUE(survivor.grab_focus());
    auto const previous_focus = gtk_root_get_focus(GTK_ROOT(host.gobj()));
    // A rooted C++ wrapper's destruction does not remove the parent's GObject
    // reference. Keep an explicit reference to detach that shell afterwards.
    auto widget = GTK_WIDGET(g_object_ref(notebook->gobj()));
    if (unroot_first) {
        content.remove(*notebook);
        EXPECT_EQ(gtk_widget_get_root(GTK_WIDGET(pages->gobj())), nullptr);
    } else {
        EXPECT_EQ(gtk_widget_get_root(GTK_WIDGET(pages->gobj())), GTK_ROOT(host.gobj()));
    }
    notebook.reset();
    if (!unroot_first) gtk_box_remove(content.gobj(), widget);
    g_object_unref(widget);
    pumpFrames();
    EXPECT_EQ(content.get_first_child(), &survivor);
    auto const focus = gtk_root_get_focus(GTK_ROOT(host.gobj()));
    EXPECT_TRUE(!focus || gtk_widget_get_root(focus) == GTK_ROOT(host.gobj()));
    if (external_focus) EXPECT_EQ(focus, previous_focus);
    host.unset_child();
    host.close();
}

// Test-local snapshot of the raw preference strings the two theme/style tests
// may mutate, so every original value and its set/unset state is restored even
// when an ASSERT returns early.
struct PrefSnapshot {
    Glib::ustring path;
    bool was_set = false;
    Glib::ustring raw;
};

std::vector<PrefSnapshot> snapshotPreferences(Inkscape::Preferences &prefs,
                                              std::vector<Glib::ustring> const &paths)
{
    std::vector<PrefSnapshot> snapshots;
    snapshots.reserve(paths.size());
    for (auto const &path : paths) {
        snapshots.push_back(PrefSnapshot{path, prefs.getEntry(path).isSet(), prefs.getString(path)});
    }
    return snapshots;
}

void restorePreferences(Inkscape::Preferences &prefs, std::vector<PrefSnapshot> const &snapshots)
{
    for (auto const &snapshot : snapshots) {
        if (snapshot.was_set) prefs.setString(snapshot.path, snapshot.raw);
        else prefs.remove(snapshot.path);
    }
}

// Without a bound desktop, the per-tool "Take from selection" button must stay
// disabled no matter what the own-style radio says, while the rest of the
// desktop-independent Preferences panel remains usable. Binding a desktop
// enables it only for the own-style choice; detaching disables it again.
TEST_F(WindowLifecycleTest, PreferencesTakeFromSelectionRequiresDesktopAndOwnStyle)
{
    using namespace Inkscape::UI::Dialog;

    auto *prefs = Inkscape::Preferences::get();
    Glib::ustring const own_path = "/tools/shapes/rect/usecurrent";
    std::vector<Glib::ustring> const mutated_paths = {
        "/dialogs/preferences/page",
        own_path,
    };
    auto const prefs_before = snapshotPreferences(*prefs, mutated_paths);

    // Restore the exact raw values (including the page's unset state) after the
    // Preferences panel and its observers are destroyed.
    auto restore = scope_exit([&] { restorePreferences(*prefs, prefs_before); });

    Glib::ustring const own_tooltip = _("Remember the style of the (first) selected object as this tool's style");
    Glib::ustring const no_document_tooltip = _("Open a document and select an object to use its style.");

    InkscapePreferences preferences;
    Gtk::Window host;
    host.set_default_size(900, 700);
    host.set_child(preferences);
    host.present();
    pumpFrames();

    // Bind, then explicitly detach after mapping because DialogBase::on_map can
    // bind the application's legacy active desktop.
    preferences.setDesktop(nullptr);
    EXPECT_EQ(preferences.getDesktop(), nullptr);

    prefs->setInt("/dialogs/preferences/page", PREFS_PAGE_TOOLS_SHAPES_RECT);
    preferences.showPage();
    pumpFrames();

    auto *own = findChildByLabel<Gtk::CheckButton>(preferences, _("This tool's own style:"));
    auto *last_used = findChildByLabel<Gtk::CheckButton>(preferences, _("Last used style"));
    auto *take = findChildByLabel<Gtk::Button>(preferences, _("Take from selection"));
    ASSERT_TRUE(own);
    ASSERT_TRUE(last_used);
    ASSERT_TRUE(take);
    EXPECT_EQ(preferences.get_root(), static_cast<Gtk::Root *>(&host));

    // Global controls on the mounted page stay sensitive while Take cannot be.
    EXPECT_TRUE(preferences.get_sensitive());
    EXPECT_TRUE(own->get_sensitive());

    // Detached: the exact no-document explanation replaces the original tooltip.
    EXPECT_EQ(take->get_tooltip_text(), no_document_tooltip);

    // own-style false -> true -> false cannot enable Take without a desktop.
    last_used->set_active(true);
    EXPECT_FALSE(own->get_active());
    EXPECT_FALSE(take->get_sensitive());
    own->set_active(true);
    EXPECT_FALSE(take->get_sensitive());
    last_used->set_active(true);
    EXPECT_FALSE(own->get_active());
    EXPECT_FALSE(take->get_sensitive());
    EXPECT_EQ(take->get_tooltip_text(), no_document_tooltip);

    // Binding the fixture desktop enables Take only for the own-style choice
    // and restores the exact original tooltip.
    preferences.setDesktop(desktop);
    ASSERT_EQ(preferences.getDesktop(), desktop);
    EXPECT_EQ(take->get_tooltip_text(), own_tooltip);
    last_used->set_active(true);
    EXPECT_FALSE(take->get_sensitive());
    own->set_active(true);
    EXPECT_TRUE(take->get_sensitive());
    EXPECT_EQ(take->get_tooltip_text(), own_tooltip);

    // Detaching while own-style is active disables it, and toggling the radio
    // cannot re-enable it until a desktop is bound again.
    preferences.setDesktop(nullptr);
    EXPECT_FALSE(take->get_sensitive());
    EXPECT_EQ(take->get_tooltip_text(), no_document_tooltip);
    last_used->set_active(true);
    own->set_active(true);
    EXPECT_FALSE(take->get_sensitive());
    EXPECT_EQ(take->get_tooltip_text(), no_document_tooltip);

    // A fresh instance with no explicit binding is disabled without any mapping.
    InkscapePreferences fresh;
    Gtk::Window fresh_host;
    fresh_host.set_child(fresh);
    prefs->setInt("/dialogs/preferences/page", PREFS_PAGE_TOOLS_SHAPES_RECT);
    fresh.showPage();
    auto *fresh_take = findChildByLabel<Gtk::Button>(fresh, _("Take from selection"));
    ASSERT_TRUE(fresh_take);
    EXPECT_EQ(fresh.getDesktop(), nullptr);
    EXPECT_FALSE(fresh_take->get_sensitive());
    EXPECT_EQ(fresh_take->get_tooltip_text(), no_document_tooltip);

    fresh_host.unset_child();
    host.unset_child();
}

// With the last desktop really destroyed (not merely unbound), the global
// theme callbacks must still run, stay sensitive and write their preferences
// without fabricating a document.
TEST_F(WindowLifecycleTest, PreferencesThemeCallbacksRemainUsableWithoutDocuments)
{
    using namespace Inkscape::UI::Dialog;

    auto *prefs = Inkscape::Preferences::get();

    // Resolve the dynamic icon color paths exactly as production does.
    Glib::ustring const icon_theme =
        prefs->getString("/theme/iconTheme", prefs->getString("/theme/defaultIconTheme", ""));
    std::vector<Glib::ustring> const mutated_paths = {
        "/dialogs/preferences/page",
        "/theme/symbolicIcons",
        "/theme/preferDarkTheme",
        "/theme/darkTheme",
        "/theme/contrast",
        "/theme/symbolicDefaultBaseColors",
        "/options/boot/theme",
        "/theme/" + icon_theme + "/symbolicBaseColor",
        "/theme/" + icon_theme + "/symbolicSuccessColor",
        "/theme/" + icon_theme + "/symbolicWarningColor",
        "/theme/" + icon_theme + "/symbolicErrorColor",
    };
    auto const prefs_before = snapshotPreferences(*prefs, mutated_paths);

    auto gtk_settings = Gtk::Settings::get_default();
    Glib::ustring gtk_theme_before;
    bool gtk_prefer_dark_before = false;
    if (gtk_settings) {
        gtk_theme_before = gtk_settings->property_gtk_theme_name().get_value();
        gtk_prefer_dark_before = gtk_settings->property_gtk_application_prefer_dark_theme().get_value();
    }

    // Restore both the preferences and the GTK settings after the panel and its
    // observers are destroyed, even when an ASSERT returns early.
    auto restore = scope_exit([&] {
        restorePreferences(*prefs, prefs_before);
        if (gtk_settings) {
            gtk_settings->property_gtk_theme_name() = gtk_theme_before;
            gtk_settings->property_gtk_application_prefer_dark_theme() = gtk_prefer_dark_before;
        }
    });

    InkscapePreferences preferences;
    Gtk::Window host;
    host.set_default_size(900, 700);
    host.set_child(preferences);
    host.present();
    pumpFrames();

    prefs->setInt("/dialogs/preferences/page", PREFS_PAGE_UI_THEME);
    preferences.showPage();
    pumpFrames();
    EXPECT_EQ(preferences.get_root(), static_cast<Gtk::Root *>(&host));

    auto *symbolic = findChildByLabel<Gtk::CheckButton>(preferences, _("Use symbolic icons"));
    auto *dark = findChildByLabel<Gtk::CheckButton>(preferences, _("Use dark theme"));
    ASSERT_TRUE(symbolic);
    ASSERT_TRUE(dark);

    // Destroy the only fixture desktop; the application must really lose its
    // active desktop rather than keep a stale legacy pointer or a document.
    document->setModifiedSinceSave(false);
    ASSERT_TRUE(testApplication().destroyDesktop(desktop));
    desktop = nullptr;
    document = nullptr;
    drainMainContext();
    ASSERT_EQ(SP_ACTIVE_DESKTOP, nullptr);
    ASSERT_TRUE(testApplication().get_documents().empty());

    // Apply the null desktop after mapping (on_map may have bound the legacy one).
    preferences.setDesktop(nullptr);
    EXPECT_EQ(preferences.getDesktop(), nullptr);
    EXPECT_TRUE(preferences.get_sensitive());

    // Symbolic icons still drives its native callback and writes its preference.
    bool const symbolic_target = !prefs->getBool("/theme/symbolicIcons", false);
    symbolic->set_active(symbolic_target);
    drainMainContext();
    EXPECT_EQ(prefs->getBool("/theme/symbolicIcons", false), symbolic_target);
    EXPECT_EQ(SP_ACTIVE_DESKTOP, nullptr);
    EXPECT_TRUE(preferences.get_sensitive());

    // Dark theme preference still drives its native callback too.
    bool const dark_target = !prefs->getBool("/theme/preferDarkTheme", false);
    dark->set_active(dark_target);
    drainMainContext();
    EXPECT_EQ(prefs->getBool("/theme/preferDarkTheme", false), dark_target);
    EXPECT_EQ(SP_ACTIVE_DESKTOP, nullptr);
    EXPECT_TRUE(preferences.get_sensitive());

    // The native contrast slider exercises themeChange through the public API.
    // Both the native widget and its scale must exist; a missing widget is a
    // real failure, never an optional skip.
    auto *contrast = findChild<Inkscape::UI::Widget::PrefSlider>(preferences);
    ASSERT_TRUE(contrast);
    auto *slider = contrast->getSlider();
    ASSERT_TRUE(slider);
    double const contrast_target = slider->get_value() == 10.0 ? 1.0 : 10.0;
    slider->set_value(contrast_target);
    drainMainContext();
    EXPECT_EQ(slider->get_value(), contrast_target);
    EXPECT_EQ(prefs->getDouble("/theme/contrast"), contrast_target);
    EXPECT_EQ(SP_ACTIVE_DESKTOP, nullptr);
    EXPECT_TRUE(preferences.get_sensitive());

    // None of these global callbacks may fabricate a desktop or a document.
    EXPECT_EQ(SP_ACTIVE_DESKTOP, nullptr);
    EXPECT_TRUE(testApplication().get_documents().empty());

    host.unset_child();
}

INSTANTIATE_TEST_SUITE_P(EmptySingleAndMultiple, DialogNotebookTest,
                        ::testing::Combine(::testing::Bool(), ::testing::Values(0, 1, 3), ::testing::Bool()));

INSTANTIATE_TEST_SUITE_P(SettledAndImmediate, FloatingDialogLifecycleTest,
                        ::testing::Values(true, false));

} // namespace

TEST_F(WindowLifecycleTest, BlinkTimeoutAfterUnparentingIsSafe)
{
    Inkscape::UI::Dialog::DialogBase dialog("/dialogs/test-blink-lifetime", "FillStroke");
    Gtk::Notebook notebook;
    notebook.append_page(dialog, "Blink test");
    dialog.blink();
    ASSERT_TRUE(notebook.has_css_class("blink"));
    notebook.remove_page(dialog);
    ASSERT_EQ(dialog.get_parent(), nullptr);
    auto loop = g_main_loop_new(nullptr, false);
    g_timeout_add(1100, [](gpointer data) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop *>(data));
        return G_SOURCE_REMOVE;
    }, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}
