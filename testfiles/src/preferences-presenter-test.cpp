// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native cold/owned lifetime and native existing-panel routing for
// Inkscape::UI::Dialog::PreferencesPresenter. Root startup/action/close-last/
// restore hooks are not qualified by these direct API tests. The fixture
// deliberately keeps no document, no desktop and a reused application handle.
//
// GUI-gated source test. Compile, registration and execution belong to the root
// workstream; nothing here claims a runtime pass.

#include "config.h"

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtk/gtktestatcontext.h>
#include <gtkmm/application.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <glibmm/i18n.h>
#include <sigc++/adaptors/track_obj.h>
#include <sigc++/scoped_connection.h>

#include "desktop.h"
#include "document.h"
#include "enums.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "io/stream/bufferstream.h"
#include "preferences.h"
#include "ui/dialog/dialog-base.h"
#include "ui/dialog/dialog-container.h"
#include "ui/dialog/dialog-manager.h"
#include "ui/dialog/dialog-multipaned.h"
#include "ui/dialog/dialog-notebook.h"
#include "ui/dialog/dialog-window.h"
#include "ui/dialog/inkscape-preferences.h"
#include "ui/dialog/preferences-presenter.h"
#include "ui/dialog/startup.h"

#include <giomm/file.h>
#include <gtkmm/recentmanager.h>
#include <glibmm/main.h>
#include <glib/gstdio.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <filesystem>
#include <cstring>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

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
        g_setenv("INKSCAPE_APP_ID_TAG", "preferencespresentertest", true);
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

// Compares raw window identity only; never dereferences its argument, so it is
// safe to call with a pointer that a native callback already destroyed.
bool applicationContains(Gtk::Application &app, Gtk::Window *window)
{
    auto const windows = app.get_windows();
    return std::find(windows.begin(), windows.end(), window) != windows.end();
}

// The exact branded title production gives the owned host.
Glib::ustring expectedOwnedTitle()
{
    Glib::ustring title = VACARDS_PRODUCT_NAME;
    title += " - ";
    title += _("Preferences");
    return title;
}

// Test-local snapshot of the raw preference strings the routing setup mutates,
// so the exact original value and its set/unset state are restored after the
// fixture has destroyed its real desktop. `Preferences::getEntry(path).isSet()`
// is the actual API (there is no `Preferences::isSet(path)`).
struct PrefSnapshot {
    Glib::ustring path;
    bool was_set = false;
    Glib::ustring raw;
};

PrefSnapshot snapshotPreference(Inkscape::Preferences &prefs, Glib::ustring path)
{
    return PrefSnapshot{path, prefs.getEntry(path).isSet(), prefs.getString(path)};
}

void restorePreference(Inkscape::Preferences &prefs, PrefSnapshot const &snapshot)
{
    if (snapshot.was_set) prefs.setString(snapshot.path, snapshot.raw);
    else prefs.remove(snapshot.path);
}

// Owns the routing-preference snapshots; reset AFTER the fixture desktop is
// destroyed so the exact previous set/unset raw values are restored last.
struct RoutingPreferenceGuard {
    std::vector<PrefSnapshot> snapshots;

    ~RoutingPreferenceGuard()
    {
        auto *prefs = Inkscape::Preferences::get();
        if (!prefs) return;
        for (auto it = snapshots.rbegin(); it != snapshots.rend(); ++it) {
            restorePreference(*prefs, *it);
        }
    }
};

class PreferencesPresenterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        (void)testApplication();
        drainMainContext();

        // Deterministic native routing for every case: never load or save docked
        // / floating dialog layout while a real desktop exists, and always dock
        // new dialogs. The exact previous raw values and set/unset state are
        // restored after the fixture has destroyed its desktop.
        auto *prefs = Inkscape::Preferences::get();
        ASSERT_TRUE(prefs);
        routing_pref_guard = std::make_unique<RoutingPreferenceGuard>();
        routing_pref_guard->snapshots.push_back(snapshotPreference(*prefs, "/options/dialogtype/value"));
        routing_pref_guard->snapshots.push_back(snapshotPreference(*prefs, "/options/savedialogposition/value"));
        prefs->setInt("/options/savedialogposition/value", PREFS_DIALOGS_STATE_NONE);
        prefs->setInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE);
        drainMainContext();
    }

    void TearDown() override
    {
        // Every local presenter/window in the test body is destroyed by now.
        // The real document/desktop is destroyed last so no owned panel can
        // outlive it. No arbitrary panel is torn down from here.
        if (document) document->setModifiedSinceSave(false);
        if (desktop) testApplication().destroyDesktop(desktop);
        else if (document) testApplication().document_close(document);
        desktop = nullptr;
        document = nullptr;
        drainMainContext();

        // Restore the routing preferences only after all desktop closes.
        routing_pref_guard.reset();
    }

    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    std::unique_ptr<RoutingPreferenceGuard> routing_pref_guard;
};

TEST_F(PreferencesPresenterTest, ColdNoDocumentsRetainsNativePanelAcrossClose)
{
    using namespace Inkscape::UI::Dialog;
    auto &app = testApplication();

    ASSERT_TRUE(app.get_documents().empty());
    ASSERT_EQ(app.get_active_desktop(), nullptr);
    ASSERT_EQ(SP_ACTIVE_DESKTOP, nullptr);

    PreferencesPresenter presenter(app);
    presenter.present();
    drainMainContext();

    auto *window = presenter.ownedWindow();
    ASSERT_TRUE(window);
    auto *panel = dynamic_cast<InkscapePreferences *>(window->get_child());
    ASSERT_TRUE(panel);

    // One ordinary host, branded title, native retention semantics, no document.
    EXPECT_EQ(window->get_title(), expectedOwnedTitle());
    EXPECT_TRUE(window->get_hide_on_close());
    EXPECT_FALSE(window->get_destroy_with_parent());
    EXPECT_TRUE(window->get_visible());
    ASSERT_TRUE(applicationContains(*app.gtk_app(), window));
    ASSERT_TRUE(app.get_documents().empty());

    // Tracked lifetime oracle: the retained host and its panel must still be
    // alive before any post-close dereference.
    sigc::slot<bool()> host_alive = sigc::track_object([] { return true; }, *window, *panel);
    ASSERT_FALSE(host_alive.empty());

    // An ordinary native close hides the host: exact host and panel survive,
    // while the hidden window leaves the application's window list.
    window->close();
    drainMainContext();
    ASSERT_FALSE(host_alive.empty());
    EXPECT_FALSE(window->get_visible());
    EXPECT_FALSE(applicationContains(*app.gtk_app(), window));
    EXPECT_EQ(presenter.ownedWindow(), window);
    ASSERT_EQ(dynamic_cast<InkscapePreferences *>(window->get_child()), panel);
    ASSERT_TRUE(app.get_documents().empty());

    // Re-presenting re-registers the exact same host/panel, visible.
    presenter.present();
    drainMainContext();
    EXPECT_EQ(presenter.ownedWindow(), window);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(window->get_child()), panel);
    EXPECT_TRUE(window->get_visible());
    EXPECT_TRUE(applicationContains(*app.gtk_app(), window));
    ASSERT_TRUE(app.get_documents().empty());

    // Close once more and reopen through the focus path.
    window->close();
    drainMainContext();
    ASSERT_FALSE(host_alive.empty());
    EXPECT_FALSE(window->get_visible());
    EXPECT_TRUE(presenter.focusOwnedIfPresent());
    drainMainContext();
    EXPECT_TRUE(window->get_visible());
    EXPECT_EQ(presenter.ownedWindow(), window);
    EXPECT_TRUE(applicationContains(*app.gtk_app(), window));

    presenter.shutdown();
    drainMainContext();
}

TEST_F(PreferencesPresenterTest, ShutdownIsTerminalAndReleasesHost)
{
    using namespace Inkscape::UI::Dialog;
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());

    PreferencesPresenter presenter(app);
    presenter.present();
    drainMainContext();

    auto *window = presenter.ownedWindow();
    ASSERT_TRUE(window);
    auto *panel = dynamic_cast<InkscapePreferences *>(window->get_child());
    ASSERT_TRUE(panel);

    // Track destruction through sigc; never dereference the host afterwards.
    sigc::slot<bool()> host_alive = sigc::track_object([] { return true; }, *window, *panel);
    ASSERT_FALSE(host_alive.empty());

    presenter.shutdown();

    EXPECT_TRUE(host_alive.empty());
    EXPECT_EQ(presenter.ownedWindow(), nullptr);
    EXPECT_FALSE(applicationContains(*app.gtk_app(), window));
    EXPECT_TRUE(app.gtk_app()->get_windows().empty());
    EXPECT_TRUE(app.get_documents().empty());

    // Terminal: repeated shutdown/present/focus cannot resurrect a host.
    presenter.shutdown();
    presenter.present();
    drainMainContext();
    EXPECT_EQ(presenter.ownedWindow(), nullptr);
    EXPECT_FALSE(presenter.focusOwnedIfPresent());
    EXPECT_FALSE(applicationContains(*app.gtk_app(), window));
    EXPECT_TRUE(app.get_documents().empty());
}

TEST_F(PreferencesPresenterTest, TransientParentDestructionRetainsOwnedHost)
{
    using namespace Inkscape::UI::Dialog;
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());

    PreferencesPresenter presenter(app);
    auto parent = std::make_unique<Gtk::Window>();
    parent->set_title("preferences-presenter-test-parent");
    parent->present();
    drainMainContext();

    presenter.present(parent.get());
    drainMainContext();

    auto *window = presenter.ownedWindow();
    ASSERT_TRUE(window);
    auto *panel = dynamic_cast<InkscapePreferences *>(window->get_child());
    ASSERT_TRUE(panel);
    EXPECT_FALSE(window->get_destroy_with_parent());
    EXPECT_EQ(window->get_transient_for(), parent.get());
    ASSERT_TRUE(app.get_documents().empty());

    // Tracked lifetime oracle: destroying the transient parent must not destroy
    // the retained host, so check the slot before any post-reset dereference.
    sigc::slot<bool()> host_alive = sigc::track_object([] { return true; }, *window, *panel);
    ASSERT_FALSE(host_alive.empty());

    // Destroying the transient parent must not destroy the retained host.
    parent.reset();
    drainMainContext();
    ASSERT_FALSE(host_alive.empty());
    EXPECT_EQ(presenter.ownedWindow(), window);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(window->get_child()), panel);
    EXPECT_TRUE(window->get_visible());
    ASSERT_TRUE(app.get_documents().empty());

    // No live tracked parent: present(nullptr) drops the stale relationship.
    presenter.present(nullptr);
    drainMainContext();
    EXPECT_EQ(presenter.ownedWindow(), window);
    EXPECT_EQ(window->get_transient_for(), nullptr);
    ASSERT_TRUE(app.get_documents().empty());

    presenter.shutdown();
    drainMainContext();
}

TEST_F(PreferencesPresenterTest, ShutdownDuringRevealStopsOwnedContinuation)
{
    using namespace Inkscape::UI::Dialog;
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());

    PreferencesPresenter presenter(app);
    presenter.present();
    drainMainContext();

    auto *window = presenter.ownedWindow();
    ASSERT_TRUE(window);

    sigc::slot<bool()> host_alive = sigc::track_object([] { return true; }, *window);
    ASSERT_FALSE(host_alive.empty());

    window->hide();
    drainMainContext();
    ASSERT_FALSE(window->get_visible());

    bool callback_ran = false;
    bool callback_inside_present = false;
    bool in_present = false;
    sigc::scoped_connection visible = window->property_visible().signal_changed().connect([&] {
        if (window->get_visible()) {
            callback_ran = true;
            callback_inside_present = in_present;
            presenter.shutdown();
        }
    });

    in_present = true;
    presenter.present();
    in_present = false;
    drainMainContext();

    EXPECT_TRUE(callback_ran);
    EXPECT_TRUE(callback_inside_present);
    EXPECT_TRUE(host_alive.empty());
    EXPECT_EQ(presenter.ownedWindow(), nullptr);
    EXPECT_FALSE(applicationContains(*app.gtk_app(), window));
    EXPECT_TRUE(app.gtk_app()->get_windows().empty());
    EXPECT_TRUE(app.get_documents().empty());

    // Terminal after the mid-reveal shutdown.
    presenter.present();
    EXPECT_FALSE(presenter.focusOwnedIfPresent());
    EXPECT_TRUE(app.get_documents().empty());

    visible.disconnect();
}

TEST_F(PreferencesPresenterTest, PresenterDestructionDuringRevealKeepsNativeFrameAlive)
{
    using namespace Inkscape::UI::Dialog;
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());

    auto presenter = std::make_unique<PreferencesPresenter>(app);
    presenter->present();
    drainMainContext();

    auto *window = presenter->ownedWindow();
    ASSERT_TRUE(window);

    sigc::slot<bool()> host_alive = sigc::track_object([] { return true; }, *window);
    ASSERT_FALSE(host_alive.empty());

    window->hide();
    drainMainContext();
    ASSERT_FALSE(window->get_visible());

    bool callback_ran = false;
    bool callback_inside_present = false;
    bool in_present = false;
    sigc::scoped_connection visible = window->property_visible().signal_changed().connect([&] {
        if (window->get_visible()) {
            callback_ran = true;
            callback_inside_present = in_present;
            presenter.reset();
        }
    });

    // This call expression may return after the callback freed the presenter
    // and, once the native frame unwinds, the host. Nothing below may use the
    // presenter or dereference the host.
    in_present = true;
    presenter->present();
    in_present = false;
    drainMainContext();

    EXPECT_TRUE(callback_ran);
    EXPECT_TRUE(callback_inside_present);
    EXPECT_FALSE(presenter);
    EXPECT_TRUE(host_alive.empty());
    EXPECT_TRUE(app.gtk_app()->get_windows().empty());
    EXPECT_TRUE(app.get_documents().empty());

    visible.disconnect();
}

TEST_F(PreferencesPresenterTest, OwnedBindingFollowsExplicitDesktopHooks)
{
    using namespace Inkscape::UI::Dialog;
    auto &app = testApplication();

    ASSERT_TRUE(app.get_documents().empty());
    ASSERT_EQ(SP_ACTIVE_DESKTOP, nullptr);

    // Owned panel first, while the application is genuinely cold.
    PreferencesPresenter presenter(app);
    presenter.present();
    drainMainContext();
    auto *window = presenter.ownedWindow();
    ASSERT_TRUE(window);
    auto *panel = dynamic_cast<InkscapePreferences *>(window->get_child());
    ASSERT_TRUE(panel);

    // Then a real native document + desktop owned by the application.
    auto svg = std::string_view{
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"/>"};
    document = app.document_add(SPDocument::createNewDocFromMem(svg));
    ASSERT_TRUE(document);
    desktop = app.createDesktop(document, false, true);
    ASSERT_TRUE(desktop);
    drainMainContext();

    presenter.desktopChanged(desktop);
    EXPECT_EQ(panel->getDesktop(), desktop);

    // An unrelated (null) close notification must not detach a live binding.
    presenter.desktopWillClose(nullptr);
    EXPECT_EQ(panel->getDesktop(), desktop);
    EXPECT_TRUE(panel->get_sensitive());
    EXPECT_TRUE(gtk_widget_is_sensitive(GTK_WIDGET(panel->gobj())));

    // The matching desktop close detaches to null; Preferences stays usable.
    presenter.desktopWillClose(desktop);
    EXPECT_EQ(panel->getDesktop(), nullptr);
    EXPECT_TRUE(panel->get_sensitive());
    EXPECT_TRUE(gtk_widget_is_sensitive(GTK_WIDGET(panel->gobj())));
    EXPECT_EQ(app.get_documents().size(), 1u);

    // Root's close-last hook is not integrated: shut the presenter down before
    // the fixture destroys the real desktop.
    presenter.shutdown();
    drainMainContext();
    EXPECT_EQ(presenter.ownedWindow(), nullptr);
}

// An already-docked native Preferences panel is reused as-is: the presenter must
// neither create an owned host nor retarget the external panel's native binding.
TEST_F(PreferencesPresenterTest, ExistingDockedPanelIsReused)
{
    using namespace Inkscape::UI::Dialog;
    auto &app = testApplication();

    auto svg = std::string_view{
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"/>"};
    document = app.document_add(SPDocument::createNewDocFromMem(svg));
    ASSERT_TRUE(document);
    desktop = app.createDesktop(document, false, true);
    ASSERT_TRUE(desktop);
    drainMainContext();

    auto *container = desktop->getContainer();
    ASSERT_TRUE(container);
    container->new_dialog("Preferences");
    drainMainContext();

    auto *panel = dynamic_cast<InkscapePreferences *>(container->get_dialog("Preferences"));
    ASSERT_TRUE(panel);
    ASSERT_NE(DialogNotebook::get_page_notebook(*panel), nullptr);
    ASSERT_EQ(panel->get_root(), desktop->getInkscapeWindow());

    // Focus/settle the native panel before the presenter observes it.
    panel->focus_dialog();
    drainMainContext();

    PreferencesPresenter presenter(app);
    presenter.present();
    drainMainContext();

    // Reused native panel: no owned host, same panel/root/registry, same document.
    EXPECT_EQ(presenter.ownedWindow(), nullptr);
    EXPECT_EQ(container->get_dialog("Preferences"), panel);
    EXPECT_EQ(panel->get_root(), desktop->getInkscapeWindow());
    ASSERT_NE(DialogNotebook::get_page_notebook(*panel), nullptr);
    EXPECT_EQ(app.get_documents().size(), 1u);

    // The presenter holds no owned state, so its explicit desktop hooks must not
    // retarget the external panel away from its settled native binding.
    auto *bound = panel->getDesktop();
    ASSERT_EQ(bound, desktop);
    presenter.desktopChanged(nullptr);
    drainMainContext();
    EXPECT_EQ(panel->getDesktop(), bound);
    presenter.desktopWillClose(bound);
    drainMainContext();
    EXPECT_EQ(panel->getDesktop(), bound);
    EXPECT_EQ(presenter.ownedWindow(), nullptr);
}

// A hidden, detached floating Preferences window created by this exact
// application is reused through native focus_dialog(), not through a new owner.
TEST_F(PreferencesPresenterTest, HiddenDetachedFloatingPanelUsesCreatingApplication)
{
    using namespace Inkscape::UI::Dialog;
    auto &app = testApplication();

    auto svg = std::string_view{
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"/>"};
    document = app.document_add(SPDocument::createNewDocFromMem(svg));
    ASSERT_TRUE(document);
    desktop = app.createDesktop(document, false, true);
    ASSERT_TRUE(desktop);
    drainMainContext();

    auto *inkscape_window = desktop->getInkscapeWindow();
    ASSERT_TRUE(inkscape_window);

    // Ordinary native floating wrapper; the panel is mounted through a native
    // DialogNotebook so on_page_added registers it, with no registry injection.
    DialogWindow window(inkscape_window);
    auto *container = window.get_container();
    ASSERT_TRUE(container);
    auto column_owner = container->create_column();
    auto *column = column_owner.get();
    container->get_columns()->append(std::move(column_owner));
    auto notebook_owner = std::make_unique<DialogNotebook>(container);
    auto *notebook = notebook_owner.get();
    column->append(std::move(notebook_owner));
    auto *prefs = Gtk::make_managed<InkscapePreferences>();
    notebook->add_page(*prefs);

    EXPECT_EQ(prefs->get_root(), &window);
    ASSERT_NE(DialogNotebook::get_page_notebook(*prefs), nullptr);
    ASSERT_EQ(container->get_dialog("Preferences"), prefs);

    window.present();
    drainMainContext();
    ASSERT_TRUE(window.get_visible());

    // Hidden, detached floating panel: still owned by this exact application.
    window.set_inkscape_window(nullptr);
    DialogManager::singleton().set_floating_dialog_visibility(&window, false);
    drainMainContext();
    EXPECT_FALSE(window.get_visible());
    EXPECT_EQ(window.get_inkscape_window(), nullptr);
    EXPECT_EQ(window.get_application_owner(), &app);
    EXPECT_EQ(container->get_dialog("Preferences"), prefs);
    EXPECT_EQ(prefs->get_root(), &window);
    ASSERT_NE(DialogNotebook::get_page_notebook(*prefs), nullptr);

    auto const documents_before = app.get_documents().size();

    PreferencesPresenter presenter(app);
    presenter.present();
    drainMainContext();

    // Routed to the same hidden floating panel, re-shown through native
    // focus_dialog(): no owned host, unchanged documents, same owner getter.
    EXPECT_EQ(presenter.ownedWindow(), nullptr);
    EXPECT_TRUE(window.get_visible());
    EXPECT_TRUE(window.get_mapped());
    EXPECT_EQ(container->get_dialog("Preferences"), prefs);
    EXPECT_EQ(prefs->get_root(), &window);
    ASSERT_NE(DialogNotebook::get_page_notebook(*prefs), nullptr);
    EXPECT_EQ(window.get_application_owner(), &app);
    EXPECT_EQ(app.get_documents().size(), documents_before);
}

// With a real active desktop and no existing panel, the presenter must create
// the native Preferences panel in that desktop's existing container.
TEST_F(PreferencesPresenterTest, ActiveDesktopUsesNativeCreation)
{
    using namespace Inkscape::UI::Dialog;
    auto &app = testApplication();

    auto svg = std::string_view{
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"/>"};
    document = app.document_add(SPDocument::createNewDocFromMem(svg));
    ASSERT_TRUE(document);
    desktop = app.createDesktop(document, false, true);
    ASSERT_TRUE(desktop);
    drainMainContext();

    // Deterministic active desktop via the public application API.
    app.set_active_desktop(desktop);
    ASSERT_EQ(app.get_active_desktop(), desktop);

    auto *container = desktop->getContainer();
    ASSERT_TRUE(container);
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog("Preferences"), nullptr);

    PreferencesPresenter presenter(app);
    presenter.present();
    drainMainContext();

    auto *panel = dynamic_cast<InkscapePreferences *>(container->get_dialog("Preferences"));
    ASSERT_TRUE(panel);
    EXPECT_EQ(presenter.ownedWindow(), nullptr);
    ASSERT_NE(DialogNotebook::get_page_notebook(*panel), nullptr);
    EXPECT_EQ(panel->get_root(), desktop->getInkscapeWindow());
    EXPECT_EQ(desktop->getDocument(), document);
    EXPECT_EQ(app.get_documents().size(), 1u);
}

} // namespace

namespace {

using Inkscape::UI::Dialog::StartScreen;
using Inkscape::UI::Dialog::welcome_default_size;

TEST(WelcomeSizeTest, FitsLogicalSmallScreenAndClampsMinimum)
{
    // 1366x768 at 125% is 1093x614 logical pixels; macOS reserves 24+80.
    auto const size = welcome_default_size({1093, 614 - 104}, {640, 480});
    EXPECT_EQ(size.width, 929);
    EXPECT_EQ(size.height, 480);
    EXPECT_LE(size.width, 1093);
    EXPECT_LE(size.height, 510);
    auto const tiny = welcome_default_size({500, 360}, {640, 480});
    EXPECT_EQ(tiny.width, 500);
    EXPECT_EQ(tiny.height, 360);
    auto const large = welcome_default_size({2560, 1440 - 104}, {640, 480});
    EXPECT_EQ(large.width, 1180);
    EXPECT_EQ(large.height, 800);
}

bool welcomeGuiEnabled()
{
    auto const gui = std::getenv("INKSCAPE_TEST_GUI");
    return gui && std::string(gui) == "1";
}

// Every Recent case re-executes in a fresh process. GTK and the application
// singleton see the temporary roots before either can construct a manager.
bool isolatedRecentChild()
{
    auto const *root = g_getenv("WELCOME_RECENT_CHILD_ROOT");
    if (root) {
        auto *manager = gtk_recent_manager_get_default();
        gchar *filename = nullptr;
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(manager), "filename")) {
            g_object_get(manager, "filename", &filename, nullptr);
        }
        bool const isolated = filename && g_str_has_prefix(filename, root) &&
            (filename[std::strlen(root)] == '/' || filename[std::strlen(root)] == '\\');
        EXPECT_TRUE(isolated) << "GTK RecentManager storage must be inside the child profile";
        g_free(filename);
        return isolated;
    }

    std::string executable;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    executable.resize(size);
    if (_NSGetExecutablePath(executable.data(), &size) != 0) {
        ADD_FAILURE() << "Cannot resolve test executable";
        return false;
    }
#elif defined(_WIN32)
    char path[MAX_PATH];
    auto const length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!length || length == MAX_PATH) {
        ADD_FAILURE() << "Cannot resolve test executable";
        return false;
    }
    executable.assign(path, length);
#else
    executable = "/proc/self/exe";
#endif
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("vacards-recent-child-XXXXXX", &error);
    if (!tmp) {
        ADD_FAILURE() << "Cannot create isolated profile: " << (error ? error->message : "unknown");
        if (error) g_error_free(error);
        return false;
    }
    std::string const root_path(tmp);
    g_free(tmp);
    for (auto const *subdir : {"data", "config", "profile"}) {
        std::filesystem::create_directories(root_path + "/" + subdir);
    }
    auto *env = g_get_environ();
    env = g_environ_setenv(env, "WELCOME_RECENT_CHILD_ROOT", root_path.c_str(), true);
    env = g_environ_setenv(env, "XDG_DATA_HOME", (root_path + "/data").c_str(), true);
    env = g_environ_setenv(env, "XDG_CONFIG_HOME", (root_path + "/config").c_str(), true);
    env = g_environ_setenv(env, "INKSCAPE_PROFILE_DIR", (root_path + "/profile").c_str(), true);
    auto const *info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string filter = std::string("--gtest_filter=") + info->test_suite_name() + "." + info->name();
    gchar *args[] = {executable.data(), filter.data(), nullptr};
    gchar *output = nullptr;
    gchar *errors = nullptr;
    int status = -1;
    bool const spawned = g_spawn_sync(nullptr, args, env, G_SPAWN_DEFAULT, nullptr, nullptr,
                                      &output, &errors, &status, &error);
    bool const passed = spawned && g_spawn_check_wait_status(status, &error);
    EXPECT_TRUE(passed) << "Isolated Recent child failed: " << (output ? output : "")
                        << (errors ? errors : "") << (error ? error->message : "");
    g_free(output);
    g_free(errors);
    if (error) g_error_free(error);
    g_strfreev(env);
    std::filesystem::remove_all(root_path);
    return false; // parent completed its assertion; only the child runs GUI code
}

bool waitUntil(std::function<bool()> const &ready, int max_ms = 5000)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
}

std::string absentUri()
{
    std::unique_ptr<char, decltype(&g_free)> uuid(g_uuid_string_random(), &g_free);
    auto const path = std::string(g_get_tmp_dir()) + "/vacards-welcome-missing-" +
                      std::string(uuid.get()) + ".svg";
    return Gio::File::create_for_path(path)->get_uri();
}

void addRecent(Gtk::RecentManager &manager, std::string const &uri, char const *name)
{
    GtkRecentData metadata{};
    metadata.display_name = const_cast<char *>(name);
    metadata.mime_type = const_cast<char *>("image/svg+xml");
    metadata.app_name = const_cast<char *>("inkscape");
    metadata.app_exec = const_cast<char *>("inkscape %u");
    ASSERT_TRUE(gtk_recent_manager_add_full(manager.gobj(), uri.c_str(), &metadata));
    ASSERT_TRUE(waitUntil([&] { return manager.has_item(uri); }));
}

GtkWidget *findCardPlaceholder(GtkWidget *widget)
{
    if (!widget) return nullptr;
    if (gtk_widget_has_css_class(widget, "welcome-card-placeholder")) return widget;
    for (auto *child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) {
        if (auto *found = findCardPlaceholder(child)) return found;
    }
    return nullptr;
}

GtkWidget *cardPreviewImage(StartScreen &screen, std::string const &uri)
{
    auto *button = screen.recent_action_for_test(uri, false);
    if (!button) return nullptr;
    auto *placeholder = findCardPlaceholder(gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(button->gobj()))));
    return placeholder ? gtk_widget_get_next_sibling(placeholder) : nullptr;
}

void expectAccessibleLabel(GtkWidget *widget, std::string const &expected)
{
    ASSERT_NE(widget, nullptr);
    char *actual = gtk_test_accessible_check_property(GTK_ACCESSIBLE(widget),
                                                       GTK_ACCESSIBLE_PROPERTY_LABEL, expected.c_str());
    EXPECT_EQ(actual, nullptr) << "Accessible label mismatch: " << (actual ? actual : "");
    g_free(actual);
}

GtkWidget *welcomeCssNode(GtkWidget *root, char const *name)
{
    if (!root) return nullptr;
    if (g_strcmp0(gtk_widget_get_css_name(root), name) == 0) return root;
    for (auto *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child))
        if (auto *found = welcomeCssNode(child, name)) return found;
    return nullptr;
}

GtkWidget *welcomeWidget(GtkWidget *root, char const *id)
{
    if (!root) return nullptr;
    if (GTK_IS_BUILDABLE(root) && g_strcmp0(gtk_buildable_get_buildable_id(GTK_BUILDABLE(root)), id) == 0)
        return root;
    for (auto *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child))
        if (auto *found = welcomeWidget(child, id)) return found;
    return nullptr;
}

void expectSelected(GtkWidget *widget, bool selected)
{
    ASSERT_NE(widget, nullptr);
    char *error = gtk_test_accessible_check_state(GTK_ACCESSIBLE(widget),
                                                   GTK_ACCESSIBLE_STATE_SELECTED, selected);
    EXPECT_EQ(error, nullptr) << (error ? error : "");
    g_free(error);
}

struct ActionBindingTracker {
    struct Binding {
        std::string uri;
        GtkListItem *item = nullptr;
        bool bound = true;
    };
    struct Rebound {
        std::string source;
        std::string target;
        GtkWidget *button = nullptr;
    };
    StartScreen &screen;
    std::vector<std::string> const &uris;
    bool retry;
    std::map<GtkWidget *, Binding> widgets;
    Rebound first_rebound;
    GtkWidget *first_unbound = nullptr;
    bool unbind_disconnected = true;
    std::size_t binds = 0, unbinds = 0;
    gulong bind_handler = 0, unbind_handler = 0;
    GtkListItemFactory *factory = nullptr;
    GtkWidget *grid = nullptr;

    static GtkWidget *action(GtkListItem *item, bool retry)
    {
        auto *box = gtk_list_item_get_child(item);
        auto *child = box ? gtk_widget_get_first_child(box) : nullptr;
        for (int i = 0; child && i < 4; ++i) child = gtk_widget_get_next_sibling(child);
        auto *button = child ? gtk_widget_get_first_child(child) : nullptr;
        return retry ? button : button ? gtk_widget_get_next_sibling(button) : nullptr;
    }
    void record(GtkWidget *button, std::string const &uri, GtkListItem *item = nullptr)
    {
        if (auto it = widgets.find(button); it != widgets.end()) {
            if (first_rebound.button == nullptr && it->second.uri != uri)
                first_rebound = {it->second.uri, uri, button};
            it->second = {uri, item, true};
        } else {
            g_object_ref(button);
            widgets.emplace(button, Binding{uri, item, true});
        }
    }
    static void on_bind(GtkListItemFactory *, GtkListItem *item, gpointer data)
    {
        auto &self = *static_cast<ActionBindingTracker *>(data);
        ++self.binds;
        auto *button = action(item, self.retry);
        if (!button) return;
        for (auto const &uri : self.uris) {
            auto *current = self.screen.recent_action_for_test(uri, self.retry);
            if (current && GTK_WIDGET(current->gobj()) == button) {
                self.record(button, uri, item);
                return;
            }
        }
    }
    static void on_unbind(GtkListItemFactory *, GtkListItem *item, gpointer data)
    {
        auto &self = *static_cast<ActionBindingTracker *>(data);
        ++self.unbinds;
        self.unbind_disconnected &= self.screen.recent_last_unbind_live_connections_for_test() == 0;
        auto *button = action(item, self.retry);
        if (auto it = self.widgets.find(button); it != self.widgets.end()) {
            it->second.bound = false;
            if (!self.first_unbound) self.first_unbound = button;
        }
    }
    ActionBindingTracker(StartScreen &screen, std::vector<std::string> const &uris, bool retry)
        : screen(screen), uris(uris), retry(retry)
    {
        for (auto const &uri : uris) {
            if (auto *button = screen.recent_action_for_test(uri, retry)) {
                auto *widget = GTK_WIDGET(button->gobj());
                record(widget, uri);
                if (!grid) grid = gtk_widget_get_ancestor(widget, GTK_TYPE_GRID_VIEW);
            }
        }
        if (!grid) return;
        factory = gtk_grid_view_get_factory(GTK_GRID_VIEW(grid));
        bind_handler = g_signal_connect(factory, "bind", G_CALLBACK(on_bind), this);
        unbind_handler = g_signal_connect(factory, "unbind", G_CALLBACK(on_unbind), this);
    }
    ~ActionBindingTracker()
    {
        if (bind_handler) g_signal_handler_disconnect(factory, bind_handler);
        if (unbind_handler) g_signal_handler_disconnect(factory, unbind_handler);
        for (auto const &[button, binding] : widgets) g_object_unref(button);
    }
    void scroll()
    {
        if (!grid) return;
        auto *scroll = gtk_widget_get_ancestor(grid, GTK_TYPE_SCROLLED_WINDOW);
        if (!scroll) return;
        auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        if (!waitUntil([&] { return gtk_adjustment_get_upper(adjustment) >
                                     gtk_adjustment_get_page_size(adjustment); })) return;
        std::cout << "Widget initial: tracked=" << widgets.size()
                  << " binds=" << screen.recent_bind_count_for_test()
                  << " unbinds=" << screen.recent_unbind_count_for_test()
                  << " upper=" << gtk_adjustment_get_upper(adjustment)
                  << " page=" << gtk_adjustment_get_page_size(adjustment)
                  << " value=" << gtk_adjustment_get_value(adjustment) << "\n";
        auto const maximum = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
        for (int step = 1; step <= 40 && !first_rebound.button; ++step) {
            gtk_adjustment_set_value(adjustment, maximum * step / 40);
            waitUntil([&] { return first_rebound.button != nullptr; }, 150);
        }
        std::cout << "Widget reuse: binds=" << binds << " unbinds=" << unbinds
                  << " path=" << (first_rebound.button ? "rebind" : "disconnect") << "\n";
    }
    GtkWidget *unbound_action() const
    {
        for (auto const &[button, binding] : widgets) if (!binding.bound) return button;
        return nullptr;
    }
};

TEST(WelcomeRecentGuiTest, TemplatesFitSmallLogicalScreen)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto screen = std::make_unique<StartScreen>();
    auto *root = GTK_WIDGET(screen->gobj());
    gtk_window_set_default_size(GTK_WINDOW(root), 929, 480);
    screen->present();
    auto *templates = welcomeWidget(root, "welcome-templates-button");
    ASSERT_NE(templates, nullptr);
    g_signal_emit_by_name(templates, "clicked");
    auto *holder = welcomeWidget(root, "template-list-holder");
    auto *create = welcomeWidget(root, "welcome-template-create");
    ASSERT_NE(holder, nullptr);
    ASSERT_NE(create, nullptr);
    ASSERT_TRUE(waitUntil([&] { return gtk_widget_get_allocated_height(holder) > 0; }));
    auto *scroller = welcomeCssNode(holder, "scrolledwindow");
    ASSERT_NE(scroller, nullptr); // TemplateList already owns a vertical scroller.
    ASSERT_TRUE(GTK_IS_SCROLLED_WINDOW(scroller));
    auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
    ASSERT_NE(adjustment, nullptr);
    EXPECT_GT(gtk_adjustment_get_page_size(adjustment), 0);
    EXPECT_LE(gtk_widget_get_allocated_height(root), 510);
    EXPECT_LE(gtk_widget_get_allocated_width(root), 1093);
    EXPECT_TRUE(gtk_widget_get_visible(create));
    EXPECT_GT(gtk_widget_get_allocated_height(create), 0);
    EXPECT_LE(gtk_widget_get_allocated_height(scroller), gtk_widget_get_allocated_height(holder));
    if (gtk_adjustment_get_upper(adjustment) > gtk_adjustment_get_page_size(adjustment)) {
        gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment));
        EXPECT_GT(gtk_adjustment_get_value(adjustment), 0);
    }
}

TEST(WelcomeRecentGuiTest, NavigationMnemonicsAndSelectedState)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    auto *root = GTK_WIDGET(screen->gobj());
    std::set<guint> keys;
    for (auto const *id : {"welcome-new-label", "welcome-open-label", "welcome-recent-label",
                           "welcome-templates-label", "welcome-preferences-label", "welcome-about-label"}) {
        auto *label = welcomeWidget(root, id);
        ASSERT_NE(label, nullptr);
        auto key = gtk_label_get_mnemonic_keyval(GTK_LABEL(label));
        EXPECT_NE(key, GDK_KEY_VoidSymbol);
        EXPECT_TRUE(keys.insert(key).second) << id;
        EXPECT_NE(gtk_label_get_mnemonic_widget(GTK_LABEL(label)), nullptr);
    }
    auto *recent = welcomeWidget(root, "welcome-recent-button");
    auto *templates = welcomeWidget(root, "welcome-templates-button");
    expectSelected(recent, true);
    expectSelected(templates, false);
    EXPECT_TRUE(gtk_widget_has_css_class(recent, "welcome-nav-active"));
    EXPECT_TRUE(gtk_widget_mnemonic_activate(welcomeWidget(root, "welcome-templates-label"), false));
    ASSERT_TRUE(waitUntil([&] { return gtk_widget_has_css_class(templates, "welcome-nav-active"); }));
    expectSelected(recent, false);
    expectSelected(templates, true);
    EXPECT_TRUE(gtk_widget_has_css_class(templates, "welcome-nav-active"));
    for (auto const *id : {"welcome-empty-new", "welcome-empty-open", "welcome-no-match-new",
                           "welcome-no-match-open", "welcome-template-create"}) {
        auto *button = welcomeWidget(root, id);
        ASSERT_NE(button, nullptr);
        EXPECT_TRUE(gtk_button_get_use_underline(GTK_BUTTON(button)));
    }
}

TEST(WelcomeRecentGuiTest, SearchLabelsKeysAndEmptyFocus)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    auto *root = GTK_WIDGET(screen->gobj());
    auto *search = welcomeWidget(root, "welcome-search");
    auto *category = welcomeWidget(root, "template-nav-holder");
    ASSERT_NE(search, nullptr);
    ASSERT_NE(category, nullptr);
    expectAccessibleLabel(search, "Search recent documents");
    auto *dropdown = gtk_widget_get_first_child(category);
    ASSERT_NE(dropdown, nullptr);
    expectAccessibleLabel(dropdown, "Template category");
    auto *new_button = welcomeWidget(root, "welcome-new-button");
    ASSERT_TRUE(waitUntil([&] { return gtk_window_get_focus(GTK_WINDOW(root)) == new_button; }));
#ifdef __APPLE__
    auto const primary = Gdk::ModifierType::META_MASK;
#else
    auto const primary = Gdk::ModifierType::CONTROL_MASK;
#endif
    EXPECT_TRUE(screen->press_key_for_test(GDK_KEY_f, primary));
    EXPECT_EQ(gtk_widget_get_ancestor(gtk_window_get_focus(GTK_WINDOW(root)), GTK_TYPE_SEARCH_ENTRY), search);
}

TEST(WelcomeRecentGuiTest, CardNameLocationAndTypeToSearch)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const uri = absentUri();
    auto const duplicate = absentUri();
    addRecent(*manager, uri, "A11y-card.svg");
    addRecent(*manager, duplicate, "A11y-card.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, false); }));
    auto *root = GTK_WIDGET(screen->gobj());
    auto *remove = GTK_WIDGET(screen->recent_action_for_test(uri, false)->gobj());
    auto *card = gtk_widget_get_parent(gtk_widget_get_parent(remove));
    ASSERT_NE(card, nullptr);
    auto const location = std::string(manager->lookup_item(uri)->get_uri_display());
    expectAccessibleLabel(card, "A11y-card.svg — " + location);
    auto *other_remove = screen->recent_action_for_test(duplicate, false);
    ASSERT_NE(other_remove, nullptr);
    auto *other_card = gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(other_remove->gobj())));
    auto const other_location = std::string(manager->lookup_item(duplicate)->get_uri_display());
    EXPECT_NE(location, other_location);
    expectAccessibleLabel(other_card, "A11y-card.svg — " + other_location);
    auto *grid = welcomeWidget(root, "welcome-recent-grid");
    ASSERT_NE(grid, nullptr);
    ASSERT_TRUE(waitUntil([&] {
        auto *focus = gtk_window_get_focus(GTK_WINDOW(root));
        return focus && focus != grid && gtk_widget_get_ancestor(focus, GTK_TYPE_GRID_VIEW) == grid;
    }));
    gtk_widget_grab_focus(grid);
    EXPECT_TRUE(screen->press_key_for_test(GDK_KEY_a, Gdk::ModifierType{}));
    auto *search = welcomeWidget(root, "welcome-search");
    ASSERT_NE(search, nullptr);
    EXPECT_EQ(gtk_widget_get_ancestor(gtk_window_get_focus(GTK_WINDOW(root)), GTK_TYPE_SEARCH_ENTRY), search);
    EXPECT_STREQ(gtk_editable_get_text(GTK_EDITABLE(search)), "a");
    EXPECT_TRUE(gtk_button_get_use_underline(GTK_BUTTON(screen->recent_action_for_test(uri, false)->gobj())));
    EXPECT_TRUE(gtk_button_get_use_underline(GTK_BUTTON(screen->recent_action_for_test(uri, true)->gobj())));
}

TEST(WelcomeRecentGuiTest, SecondaryTextContrastInBothThemes)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const uri = absentUri();
    addRecent(*manager, uri, "contrast.svg");
    auto *settings = gtk_settings_get_default();
    gboolean original = false;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &original, nullptr);
    auto luminance = [](GdkRGBA const &color) {
        auto channel = [](double value) {
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return .2126 * channel(color.red) + .7152 * channel(color.green) + .0722 * channel(color.blue);
    };
    for (bool dark : {false, true}) {
        g_object_set(settings, "gtk-application-prefer-dark-theme", dark, nullptr);
        auto screen = std::make_unique<StartScreen>();
        screen->present();
        drainMainContext();
        auto *root = GTK_WIDGET(screen->gobj());
        GdkRGBA background{};
        ASSERT_TRUE(gtk_style_context_lookup_color(gtk_widget_get_style_context(root),
                                                   "theme_bg_color", &background));
        auto *search = welcomeWidget(root, "welcome-search");
        auto *placeholder = welcomeCssNode(search, "placeholder");
        ASSERT_NE(placeholder, nullptr);
        GdkRGBA entry_background{};
        ASSERT_TRUE(gtk_style_context_lookup_color(gtk_widget_get_style_context(search),
                                                   "theme_base_color", &entry_background));
        GdkRGBA placeholder_color{};
        gtk_style_context_get_color(gtk_widget_get_style_context(placeholder), &placeholder_color);
        auto const placeholder_light = std::max(luminance(placeholder_color), luminance(entry_background));
        auto const placeholder_shade = std::min(luminance(placeholder_color), luminance(entry_background));
        EXPECT_GE((placeholder_light + .05) / (placeholder_shade + .05), 4.5) << "placeholder dark=" << dark;
        auto *remove = screen->recent_action_for_test(uri, false);
        ASSERT_NE(remove, nullptr);
        auto *card = gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(remove->gobj())));
        auto *preview = gtk_widget_get_first_child(card);
        GdkRGBA preview_color{}, theme_foreground{};
        gtk_style_context_get_color(gtk_widget_get_style_context(preview), &preview_color);
        ASSERT_TRUE(gtk_style_context_lookup_color(gtk_widget_get_style_context(root),
                                                   "theme_fg_color", &theme_foreground));
        GdkRGBA preview_background{};
        preview_background.red = .08 * theme_foreground.red + .92 * background.red;
        preview_background.green = .08 * theme_foreground.green + .92 * background.green;
        preview_background.blue = .08 * theme_foreground.blue + .92 * background.blue;
        preview_background.alpha = 1;
        auto const preview_light = std::max(luminance(preview_color), luminance(preview_background));
        auto const preview_shade = std::min(luminance(preview_color), luminance(preview_background));
        EXPECT_GE((preview_light + .05) / (preview_shade + .05), 4.5) << "card placeholder dark=" << dark;
        for (auto const *id : {"welcome-subtitle", "welcome-version-label"}) {
            auto *widget = welcomeWidget(root, id);
            ASSERT_NE(widget, nullptr);
            GdkRGBA foreground{};
            gtk_style_context_get_color(gtk_widget_get_style_context(widget), &foreground);
            auto light = std::max(luminance(foreground), luminance(background));
            auto shade = std::min(luminance(foreground), luminance(background));
            EXPECT_GE((light + .05) / (shade + .05), 4.5) << id << " dark=" << dark;
        }
    }
    g_object_set(settings, "gtk-application-prefer-dark-theme", original, nullptr);
}

TEST(WelcomeRecentGuiTest, MissingLocalDoesNotOpen)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    auto &app = testApplication();
    auto const before = app.get_documents().size();
    auto screen = std::make_unique<StartScreen>();
    screen->activate_recent_for_test(absentUri());
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Missing);
    EXPECT_EQ(app.get_documents().size(), before);
}

TEST(WelcomeRecentGuiTest, InjectedUnreachableDoesNotOpen)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    auto &app = testApplication();
    auto const before = app.get_documents().size();
    auto screen = std::make_unique<StartScreen>();
    screen->set_recent_probe_for_test([](auto const &, auto completion) {
        Glib::signal_idle().connect_once([completion] {
            completion(StartScreen::RecentProbeResult::Unreachable);
        });
    });
    screen->activate_recent_for_test(absentUri());
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Unreachable);
    EXPECT_EQ(app.get_documents().size(), before);
}

TEST(WelcomeRecentGuiTest, TimeoutKeepsHeartbeatResponsive)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    auto &app = testApplication();
    auto const before = app.get_documents().size();
    auto screen = std::make_unique<StartScreen>();
    screen->set_recent_probe_for_test([](auto const &, auto) {}); // delayed forever
    std::vector<double> intervals;
    auto previous = std::chrono::steady_clock::now();
    auto heartbeat = Glib::signal_timeout().connect([&] {
        auto const now = std::chrono::steady_clock::now();
        intervals.push_back(std::chrono::duration<double, std::milli>(now - previous).count());
        previous = now;
        return true;
    }, 10);
    screen->activate_recent_for_test(absentUri());
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }, 5000));
    heartbeat.disconnect();
    ASSERT_GE(intervals.size(), 100u);
    std::sort(intervals.begin(), intervals.end());
    auto const p95 = intervals[static_cast<std::size_t>(intervals.size() * 0.95)];
    std::cout << "Welcome delayed-check heartbeat p95: " << p95 << " ms\n";
    EXPECT_LT(p95, 50.0);
    EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Unreachable);
    EXPECT_EQ(app.get_documents().size(), before);
}

TEST(WelcomeRecentGuiTest, TimeoutAnnouncesOnlyUnreachable)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const uri = absentUri();
    addRecent(*manager, uri, "timeout.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    screen->set_recent_probe_for_test([](auto const &, auto) {});
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, false); }));
    auto const position = screen->recent_position_for_test(uri);
    auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(screen->recent_action_for_test(uri, false)->gobj()),
                                         GTK_TYPE_GRID_VIEW);
    ASSERT_NE(grid, nullptr);
    g_signal_emit_by_name(grid, "activate", position);
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }, 5000));
    EXPECT_EQ(screen->recent_announcement_count_for_test(), 2u);
}

TEST(WelcomeRecentGuiTest, SecondActivationCancelsPending)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    auto &app = testApplication();
    auto const before = app.get_documents().size();
    auto screen = std::make_unique<StartScreen>();
    std::function<void(StartScreen::RecentProbeResult)> delayed;
    screen->set_recent_probe_for_test([&](auto const &, auto completion) { delayed = completion; });
    auto const uri = absentUri();
    screen->activate_recent_for_test(uri);
    ASSERT_TRUE(screen->recent_pending_for_test());
    screen->activate_recent_for_test(uri);
    EXPECT_FALSE(screen->recent_pending_for_test());
    delayed(StartScreen::RecentProbeResult::Available);
    drainMainContext();
    EXPECT_EQ(app.get_documents().size(), before);
}

TEST(WelcomeRecentGuiTest, DestroyDuringCheckDoesNotOpen)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    auto &app = testApplication();
    auto const before = app.get_documents().size();
    std::function<void(StartScreen::RecentProbeResult)> delayed;
    {
        auto screen = std::make_unique<StartScreen>();
        screen->set_recent_probe_for_test([&](auto const &, auto completion) { delayed = completion; });
        screen->activate_recent_for_test(absentUri());
    }
    delayed(StartScreen::RecentProbeResult::Available);
    drainMainContext();
    EXPECT_EQ(app.get_documents().size(), before);
}

TEST(WelcomeRecentGuiTest, RemoveOnlyHistory)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    ASSERT_TRUE(manager);
    char *path = nullptr;
    int fd = g_file_open_tmp("vacards-welcome-remove-XXXXXX", &path, nullptr);
    ASSERT_GE(fd, 0);
    g_close(fd, nullptr);
    std::unique_ptr<char, decltype(&g_free)> owned_path(path, &g_free);
    auto const uri = Gio::File::create_for_path(path)->get_uri();
    addRecent(*manager, uri, "remove-only.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, false) != nullptr; }));
    ASSERT_TRUE(manager->has_item(uri));
    g_signal_emit_by_name(screen->recent_action_for_test(uri, false)->gobj(), "clicked");
    EXPECT_FALSE(manager->has_item(uri));
    EXPECT_TRUE(g_file_test(path, G_FILE_TEST_EXISTS));
    g_unlink(path);
}

TEST(WelcomeRecentGuiTest, RecoveryFilterClearsWhenNoRecoveryRemains)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const uri = absentUri();
    addRecent(*manager, uri, "ordinary.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->set_recovery_only_for_test(true);
    ASSERT_TRUE(screen->recovery_only_for_test());
    screen->refresh_recent_for_test();
    EXPECT_FALSE(screen->recovery_only_for_test());
    EXPECT_NE(screen->recent_position_for_test(uri), GTK_INVALID_LIST_POSITION);
}

TEST(WelcomeRecentGuiTest, GridRebuildDuringPendingCheck)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    auto &app = testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const a = absentUri(), b = absentUri(), c = absentUri();
    addRecent(*manager, a, "A-grid.svg");
    addRecent(*manager, b, "B-grid.svg");
    addRecent(*manager, c, "C-grid.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(a, false) &&
                                      screen->recent_action_for_test(c, false); }));
    auto const bound = screen->recent_bind_count_for_test();
    auto const unbound = screen->recent_unbind_count_for_test();
    auto const before = app.get_documents().size();
    std::function<void(StartScreen::RecentProbeResult)> delayed;
    screen->set_recent_probe_for_test([&](auto const &uri, auto completion) {
        if (uri == c) delayed = completion;
    });
    auto const position = screen->recent_position_for_test(c);
    ASSERT_NE(position, GTK_INVALID_LIST_POSITION);
    g_signal_emit_by_name(gtk_widget_get_ancestor(GTK_WIDGET(screen->recent_action_for_test(c, false)->gobj()),
                                               GTK_TYPE_GRID_VIEW), "activate", position);
    ASSERT_TRUE(screen->recent_pending_for_test());
    ASSERT_TRUE(delayed);
    g_signal_emit_by_name(screen->recent_action_for_test(a, false)->gobj(), "clicked");
    ASSERT_TRUE(waitUntil([&] { return screen->recent_unbind_count_for_test() > unbound; }));
    EXPECT_GT(screen->recent_bind_count_for_test(), bound);
    delayed(StartScreen::RecentProbeResult::Available);
    drainMainContext();
    EXPECT_FALSE(manager->has_item(a));
    EXPECT_TRUE(manager->has_item(b));
    EXPECT_TRUE(manager->has_item(c));
    EXPECT_EQ(app.get_documents().size(), before);
    EXPECT_FALSE(screen->recent_pending_for_test());
}

TEST(WelcomeRecentGuiTest, RemoveAfterWidgetReuse)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const source = absentUri(), target = absentUri();
    addRecent(*manager, source, "B-reuse.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(source, false); }));
    auto *button = GTK_WIDGET(screen->recent_action_for_test(source, false)->gobj());
    gtk_widget_grab_focus(welcomeWidget(GTK_WIDGET(screen->gobj()), "welcome-new-button"));
    auto const unbound = screen->recent_unbind_count_for_test();
    addRecent(*manager, target, "B-reuse.svg");
    screen->replace_recent_for_test(source, target); // _recent_store->splice
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(target, false); }));
    ASSERT_GT(screen->recent_unbind_count_for_test(), unbound);
    std::cout << "RemoveAfterWidgetReuse: new_binding == old_binding: "
              << screen->recent_reused_binding_for_test() << '\n';
    EXPECT_FALSE(screen->recent_reused_binding_for_test()); // GTK used a new binding; helper moved the old child
    EXPECT_TRUE(screen->recent_old_unbind_clean_for_test());
    EXPECT_EQ(screen->recent_last_unbind_live_connections_for_test(), 0u);
    ASSERT_EQ(GTK_WIDGET(screen->recent_action_for_test(target, false)->gobj()), button);
    auto *placeholder = findCardPlaceholder(gtk_widget_get_parent(gtk_widget_get_parent(button)));
    ASSERT_NE(placeholder, nullptr);
    auto *label = gtk_widget_get_next_sibling(gtk_widget_get_first_child(placeholder));
    ASSERT_TRUE(GTK_IS_LABEL(label));
    auto const label_before = std::string(gtk_label_get_text(GTK_LABEL(label)));
    screen->emit_replaced_recent_changed_for_test();
    EXPECT_EQ(std::string(gtk_label_get_text(GTK_LABEL(label))), label_before);
    auto const refreshes = screen->recent_refresh_count_for_test();
    g_signal_emit_by_name(button, "clicked");
    EXPECT_TRUE(manager->has_item(source));
    EXPECT_FALSE(manager->has_item(target));
    EXPECT_EQ(screen->recent_refresh_count_for_test(), refreshes + 1);
}

TEST(WelcomeRecentGuiTest, PermissionDeniedHasDistinctState)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    auto &app = testApplication();
    auto const before = app.get_documents().size();
    auto screen = std::make_unique<StartScreen>();
    screen->set_recent_probe_for_test([](auto const &, auto complete) {
        Glib::signal_idle().connect_once([complete] { complete(StartScreen::RecentProbeResult::PermissionDenied); });
    });
    screen->activate_recent_for_test(absentUri());
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::PermissionDenied);
    EXPECT_EQ(app.get_documents().size(), before);
}

TEST(WelcomeRecentGuiTest, ReadableEmptyAndNonemptyFilesOpen)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    unsigned opened = 0;
    for (auto const *contents : {"", "<svg xmlns='http://www.w3.org/2000/svg'/>"}) {
        gchar *path = nullptr;
        int fd = g_file_open_tmp("vacards-welcome-readable-XXXXXX.svg", &path, nullptr);
        ASSERT_GE(fd, 0);
        g_close(fd, nullptr);
        std::unique_ptr<char, decltype(&g_free)> owned(path, &g_free);
        ASSERT_TRUE(g_file_set_contents(path, contents, -1, nullptr));
        auto screen = std::make_unique<StartScreen>();
        screen->set_recent_open_for_test([&](auto const &) { ++opened; });
        screen->activate_recent_for_test(Gio::File::create_for_path(path)->get_uri());
        ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
        EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Available);
        g_unlink(path);
    }
    EXPECT_EQ(opened, 2u);
}

TEST(WelcomeRecentGuiTest, MetadataSuccessThenDelayedReadNeverOpens)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    auto &app = testApplication();
    auto screen = std::make_unique<StartScreen>();
    auto const uri = absentUri();
    std::function<void(StartScreen::RecentProbeResult)> complete;
    screen->set_recent_probe_for_test([&](auto const &, auto callback) { complete = callback; });
    auto const before = app.get_documents().size();
    screen->activate_recent_for_test(uri);
    ASSERT_TRUE(complete);
    complete(StartScreen::RecentProbeResult::MetadataReady);
    EXPECT_TRUE(screen->recent_pending_for_test());
    EXPECT_EQ(app.get_documents().size(), before);
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }, 5000));
    EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Unreachable);
    complete(StartScreen::RecentProbeResult::Available);
    drainMainContext();
    EXPECT_EQ(app.get_documents().size(), before);
}

TEST(WelcomeRecentGuiTest, RealQueryThenCancelledReadNeverOpens)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    gchar *path = nullptr;
    int fd = g_file_open_tmp("vacards-welcome-cancel-XXXXXX.svg", &path, nullptr);
    ASSERT_GE(fd, 0);
    g_close(fd, nullptr);
    std::unique_ptr<char, decltype(&g_free)> owned(path, &g_free);
    ASSERT_TRUE(g_file_set_contents(path, "<svg/>", -1, nullptr));
    auto screen = std::make_unique<StartScreen>();
    auto const uri = Gio::File::create_for_path(path)->get_uri();
    unsigned metadata = 0, opened = 0;
    screen->set_recent_open_for_test([&](auto const &) { ++opened; });
    screen->set_recent_metadata_for_test([&] { ++metadata; screen->activate_recent_for_test(uri); });
    screen->activate_recent_for_test(uri);
    ASSERT_TRUE(waitUntil([&] { return metadata == 1 && !screen->recent_pending_for_test(); }));
    drainMainContext();
    EXPECT_EQ(opened, 0u);
    g_unlink(path);
}

TEST(WelcomeRecentGuiTest, RealPermissionDeniedDoesNotOpen)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
#ifndef _WIN32
    (void)testApplication();
    gchar *path = nullptr;
    int fd = g_file_open_tmp("vacards-welcome-denied-XXXXXX.svg", &path, nullptr);
    ASSERT_GE(fd, 0);
    g_close(fd, nullptr);
    std::unique_ptr<char, decltype(&g_free)> owned(path, &g_free);
    ASSERT_EQ(g_chmod(path, 0000), 0);
    if (g_access(path, R_OK) == 0) {
        g_chmod(path, 0600);
        g_unlink(path);
        GTEST_SKIP() << "Process can read mode-0000 files";
    }
    auto screen = std::make_unique<StartScreen>();
    screen->activate_recent_for_test(Gio::File::create_for_path(path)->get_uri());
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::PermissionDenied);
    g_chmod(path, 0600);
    g_unlink(path);
#endif
}

TEST(WelcomeRecentGuiTest, HungBackendDoesNotAccumulate)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto const uri = absentUri();
    unsigned starts = 0;
    std::function<void(StartScreen::RecentProbeResult)> complete;
    auto screen = std::make_unique<StartScreen>();
    screen->set_recent_probe_for_test([&](auto const &, auto callback) {
        ++starts;
        complete = callback;
    });
    screen->activate_recent_for_test(uri);
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }, 5000));
    for (int i = 0; i < 5; ++i) screen->activate_recent_for_test(uri);
    EXPECT_EQ(starts, 1u);
    screen.reset();
    auto another = std::make_unique<StartScreen>();
    another->set_recent_probe_for_test([&](auto const &, auto) { ++starts; });
    another->activate_recent_for_test(uri);
    EXPECT_EQ(starts, 1u);
    complete(StartScreen::RecentProbeResult::Unreachable);
    another->activate_recent_for_test(uri);
    EXPECT_EQ(starts, 2u);
}

TEST(WelcomeRecentGuiTest, RetryBudgetTwoPerSession)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto screen = std::make_unique<StartScreen>();
    unsigned starts = 0;
    screen->set_recent_probe_for_test([&](auto const &, auto complete) {
        ++starts;
        Glib::signal_idle().connect_once([complete] { complete(StartScreen::RecentProbeResult::Unreachable); });
    });
    auto const uri = absentUri();
    for (int i = 0; i < 4; ++i) {
        screen->activate_recent_for_test(uri);
        ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    }
    EXPECT_EQ(starts, 4u); // card activation always probes, even after two failed retries
}

TEST(WelcomeRecentGuiTest, MissingClicksDoNotSpendRetries)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto screen = std::make_unique<StartScreen>();
    unsigned starts = 0;
    screen->set_recent_probe_for_test([&](auto const &, auto complete) {
        ++starts;
        Glib::signal_idle().connect_once([complete] { complete(StartScreen::RecentProbeResult::Missing); });
    });
    auto const uri = absentUri();
    for (int i = 0; i < 4; ++i) {
        screen->activate_recent_for_test(uri);
        ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
        EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Missing);
    }
    EXPECT_EQ(starts, 4u);
}

TEST(WelcomeRecentGuiTest, RetryBudgetSurvivesWelcomeReopen)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto const uri = absentUri();
    unsigned starts = 0;
    for (int i = 0; i < 4; ++i) {
        auto screen = std::make_unique<StartScreen>();
        screen->set_recent_probe_for_test([&](auto const &, auto complete) {
            ++starts;
            Glib::signal_idle().connect_once([complete] { complete(StartScreen::RecentProbeResult::Unreachable); });
        });
        screen->activate_recent_for_test(uri);
        ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    }
    EXPECT_EQ(starts, 4u);
}

TEST(WelcomeRecentGuiTest, ExhaustedRetryButtonIsDisabled)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const uri = absentUri();
    addRecent(*manager, uri, "retry-budget.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    unsigned starts = 0;
    screen->set_recent_probe_for_test([&](auto const &, auto complete) {
        ++starts;
        Glib::signal_idle().connect_once([complete] { complete(StartScreen::RecentProbeResult::Unreachable); });
    });
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, true); }));
    auto *button = screen->recent_action_for_test(uri, true);
    auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(button->gobj()), GTK_TYPE_GRID_VIEW);
    ASSERT_NE(grid, nullptr);
    auto const position = screen->recent_position_for_test(uri);
    ASSERT_NE(position, GTK_INVALID_LIST_POSITION);
    for (int i = 0; i < 3; ++i) {
        g_signal_emit_by_name(grid, "activate", position);
        ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    }
    EXPECT_FALSE(button->get_sensitive());
    EXPECT_TRUE(button->get_visible());
    EXPECT_EQ(button->get_tooltip_text(), "Retry limit reached for this session");
    EXPECT_EQ(starts, 3u);
    g_signal_emit_by_name(grid, "activate", position);
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    EXPECT_EQ(starts, 4u);
}

TEST(WelcomeRecentGuiTest, CancelledRetryDoesNotSpendBudget)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const uri = absentUri(), other = absentUri();
    addRecent(*manager, uri, "cancelled-retry.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    std::function<void(StartScreen::RecentProbeResult)> pending;
    screen->set_recent_probe_for_test([&](auto const &, auto complete) { pending = complete; });
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, true); }));
    auto *button = screen->recent_action_for_test(uri, true);
    auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(button->gobj()), GTK_TYPE_GRID_VIEW);
    auto const position = screen->recent_position_for_test(uri);
    ASSERT_NE(grid, nullptr);
    ASSERT_NE(position, GTK_INVALID_LIST_POSITION);
    g_signal_emit_by_name(grid, "activate", position);
    ASSERT_TRUE(pending);
    pending(StartScreen::RecentProbeResult::Unreachable);
    ASSERT_TRUE(button->get_sensitive());
    g_signal_emit_by_name(button->gobj(), "clicked");
    ASSERT_TRUE(screen->recent_pending_for_test());
    auto cancelled = pending;
    screen->activate_recent_for_test(other); // cancels the retry before completion
    cancelled(StartScreen::RecentProbeResult::Unreachable);
    pending(StartScreen::RecentProbeResult::Missing);
    for (int i = 0; i < 2; ++i) {
        g_signal_emit_by_name(grid, "activate", position);
        ASSERT_TRUE(screen->recent_pending_for_test());
        pending(StartScreen::RecentProbeResult::Unreachable);
        EXPECT_EQ(button->get_sensitive(), i == 0);
    }
}

TEST(WelcomeRecentGuiTest, BusyAfterUnreachableKeepsRetry)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const uri = absentUri();
    addRecent(*manager, uri, "busy-retry.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    screen->set_recent_probe_for_test([](auto const &, auto complete) {
        complete(StartScreen::RecentProbeResult::Unreachable);
    });
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, true); }));
    auto *button = screen->recent_action_for_test(uri, true);
    auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(button->gobj()), GTK_TYPE_GRID_VIEW);
    auto const position = screen->recent_position_for_test(uri);
    ASSERT_NE(grid, nullptr);
    ASSERT_NE(position, GTK_INVALID_LIST_POSITION);
    g_signal_emit_by_name(grid, "activate", position);
    ASSERT_TRUE(button->get_visible());
    auto blocker = std::make_unique<StartScreen>();
    std::function<void(StartScreen::RecentProbeResult)> release;
    blocker->set_recent_probe_for_test([&](auto const &, auto complete) { release = complete; });
    blocker->activate_recent_for_test(uri);
    ASSERT_TRUE(release);
    g_signal_emit_by_name(grid, "activate", position);
    EXPECT_TRUE(button->get_visible());
    EXPECT_TRUE(button->get_sensitive());
    auto *placeholder = findCardPlaceholder(gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(button->gobj()))));
    ASSERT_NE(placeholder, nullptr);
    EXPECT_TRUE(std::string(gtk_widget_get_tooltip_text(placeholder)).find("Still checking this location") == 0);
    release(StartScreen::RecentProbeResult::Missing);
}

TEST(WelcomeRecentGuiTest, BusyCardDoesNotCancelCurrentCheck)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto const uri = absentUri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "busy-current.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, false); }));
    std::function<void(StartScreen::RecentProbeResult)> complete;
    screen->set_recent_probe_for_test([&](auto const &, auto callback) { complete = callback; });
    screen->activate_recent_for_test(uri);
    ASSERT_TRUE(screen->recent_pending_for_test());
    auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(screen->recent_action_for_test(uri, false)->gobj()), GTK_TYPE_GRID_VIEW);
    g_signal_emit_by_name(grid, "activate", screen->recent_position_for_test(uri));
    EXPECT_TRUE(screen->recent_pending_for_test());
    complete(StartScreen::RecentProbeResult::Missing);
}

TEST(WelcomeRecentGuiTest, BusyCardRestoresWhenCurrentCheckFinishes)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto const uri = absentUri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "busy-finish.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, false); }));
    std::function<void(StartScreen::RecentProbeResult)> complete;
    screen->set_recent_probe_for_test([&](auto const &, auto callback) { complete = callback; });
    screen->activate_recent_for_test(uri);
    auto *button = screen->recent_action_for_test(uri, false);
    auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(button->gobj()), GTK_TYPE_GRID_VIEW);
    g_signal_emit_by_name(grid, "activate", screen->recent_position_for_test(uri));
    auto *placeholder = findCardPlaceholder(gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(button->gobj()))));
    ASSERT_NE(placeholder, nullptr);
    ASSERT_NE(gtk_widget_get_tooltip_text(placeholder), nullptr);
    EXPECT_TRUE(std::string(gtk_widget_get_tooltip_text(placeholder)).find("Still checking") == 0);
    complete(StartScreen::RecentProbeResult::Missing);
    EXPECT_EQ(std::string(gtk_widget_get_tooltip_text(placeholder)), "Preview unavailable");
}

TEST(WelcomeRecentGuiTest, CancelledRetryRestoresUnreachableCard)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto const uri = absentUri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "cancel-state.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, true); }));
    std::function<void(StartScreen::RecentProbeResult)> complete;
    screen->set_recent_probe_for_test([&](auto const &, auto callback) { complete = callback; });
    auto *retry = screen->recent_action_for_test(uri, true);
    auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(retry->gobj()), GTK_TYPE_GRID_VIEW);
    g_signal_emit_by_name(grid, "activate", screen->recent_position_for_test(uri));
    complete(StartScreen::RecentProbeResult::Unreachable);
    ASSERT_TRUE(retry->get_visible());
    g_signal_emit_by_name(retry->gobj(), "clicked");
    ASSERT_TRUE(screen->recent_pending_for_test());
    screen->activate_recent_for_test(absentUri());
    EXPECT_TRUE(retry->get_visible());
}

TEST(WelcomeRecentGuiTest, RefusedRetryRefreshesButton)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto const uri = absentUri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "refused-retry.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, true); }));
    auto *retry = screen->recent_action_for_test(uri, true);
    screen->set_recent_probe_for_test([](auto const &, auto complete) { complete(StartScreen::RecentProbeResult::Unreachable); });
    auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(retry->gobj()), GTK_TYPE_GRID_VIEW);
    g_signal_emit_by_name(grid, "activate", screen->recent_position_for_test(uri));
    ASSERT_TRUE(retry->get_sensitive());
    auto other = std::make_unique<StartScreen>();
    other->set_recent_probe_for_test([](auto const &, auto complete) { complete(StartScreen::RecentProbeResult::Unreachable); });
    other->activate_recent_for_test(uri);
    other->activate_recent_for_test(uri);
    ASSERT_TRUE(retry->get_sensitive());
    g_signal_emit_by_name(retry->gobj(), "clicked");
    EXPECT_FALSE(retry->get_sensitive());
    EXPECT_EQ(retry->get_tooltip_text(), "Retry limit reached for this session");
}

TEST(WelcomeRecentGuiTest, AvailableResetsRetryBudget)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto const uri = absentUri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "available-reset.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, true); }));
    auto *retry = screen->recent_action_for_test(uri, true);
    auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(retry->gobj()), GTK_TYPE_GRID_VIEW);
    auto const position = screen->recent_position_for_test(uri);
    screen->set_recent_open_for_test([](auto const &) {});
    int probes = 0;
    screen->set_recent_probe_for_test([&](auto const &, auto complete) {
        ++probes;
        complete(probes == 4 ? StartScreen::RecentProbeResult::Available
                             : StartScreen::RecentProbeResult::Unreachable);
    });
    for (int i = 0; i < 5; ++i) g_signal_emit_by_name(grid, "activate", position);
    ASSERT_EQ(probes, 5);
    EXPECT_TRUE(retry->get_visible());
    EXPECT_TRUE(retry->get_sensitive());
}

TEST(WelcomeRecentGuiTest, FourDistinctBackendLimit)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    std::vector<std::unique_ptr<StartScreen>> screens;
    std::vector<std::function<void(StartScreen::RecentProbeResult)>> pending;
    unsigned starts = 0;
    for (int i = 0; i < 5; ++i) {
        auto screen = std::make_unique<StartScreen>();
        screen->set_recent_probe_for_test([&](auto const &, auto complete) {
            ++starts;
            pending.push_back(complete);
        });
        screen->activate_recent_for_test(absentUri());
        screens.push_back(std::move(screen));
    }
    EXPECT_EQ(starts, 4u);
    EXPECT_EQ(pending.size(), 4u);
    EXPECT_EQ(screens.back()->recent_result_for_test(), StartScreen::RecentProbeResult::Busy);
    for (auto &complete : pending) complete(StartScreen::RecentProbeResult::Unreachable);
}

TEST(WelcomeRecentGuiTest, MissingMacVolumeRootIsUnreachable)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
#ifdef __APPLE__
    (void)testApplication();
    auto screen = std::make_unique<StartScreen>();
    std::unique_ptr<char, decltype(&g_free)> uuid(g_uuid_string_random(), &g_free);
    auto const path = std::string("/Volumes/vacards-absent-") + uuid.get() + "/file.svg";
    screen->activate_recent_for_test(Gio::File::create_for_path(path)->get_uri());
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Unreachable);
#endif
}

TEST(WelcomeRecentGuiTest, EmptyMountRootUsesAsyncFilesystemIdentity)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    GError *error = nullptr;
    char *path = g_dir_make_tmp("vacards-empty-mount-XXXXXX", &error);
    ASSERT_NE(path, nullptr) << (error ? error->message : "unknown");
    std::unique_ptr<char, decltype(&g_free)> owned(path, &g_free);
    auto screen = std::make_unique<StartScreen>();
    screen->set_recent_root_for_test(path);
    screen->activate_recent_for_test(Gio::File::create_for_path(std::string(path) + "/missing.svg")->get_uri());
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Unreachable);
    g_rmdir(path);
}

TEST(WelcomeRecentGuiTest, DifferentFilesystemRootMeansMissing)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
#ifdef __APPLE__
    auto screen = std::make_unique<StartScreen>();
    screen->set_recent_root_for_test("/dev");
    screen->activate_recent_for_test(Gio::File::create_for_path("/dev/vacards-absent/missing.svg")->get_uri());
    ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Missing);
#endif
}

#ifdef _WIN32
TEST(WelcomeRecentGuiTest, MissingWindowsUncAndDriveRootsAreUnreachable)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    for (auto const *path : {"\\\\vacards-absent-host\\absent-share\\file.svg",
                             "Z:\\vacards-absent-dir\\file.svg"}) {
        auto screen = std::make_unique<StartScreen>();
        screen->activate_recent_for_test(Gio::File::create_for_path(path)->get_uri());
        ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }, 5000));
        EXPECT_EQ(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Unreachable);
    }
}
#endif

TEST(WelcomeRecentGuiTest, RetryAfterWidgetReuseTargetsCurrentCard)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    std::vector<std::string> candidates;
    for (int i = 0; i < 90; ++i) {
        auto uri = absentUri();
        addRecent(*manager, uri, "B-retry.svg");
        candidates.push_back(std::move(uri));
    }
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    std::vector<std::string> probes;
    screen->set_recent_probe_for_test([&](auto const &uri, auto complete) {
        probes.push_back(uri);
        Glib::signal_idle().connect_once([complete] { complete(StartScreen::RecentProbeResult::Unreachable); });
    });
    auto activate = [&](std::string const &uri) {
        auto const position = screen->recent_position_for_test(uri);
        ASSERT_NE(position, GTK_INVALID_LIST_POSITION);
        auto *button = screen->recent_action_for_test(uri, false);
        ASSERT_NE(button, nullptr);
        auto *grid = gtk_widget_get_ancestor(GTK_WIDGET(button->gobj()), GTK_TYPE_GRID_VIEW);
        ASSERT_NE(grid, nullptr);
        g_signal_emit_by_name(grid, "activate", position);
        ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
    };
    ASSERT_TRUE(waitUntil([&] { return screen->recent_bind_count_for_test() >= 3; }));
    ActionBindingTracker tracker(*screen, candidates, true);
    ASSERT_GE(tracker.widgets.size(), 3u);
    tracker.scroll();
    auto const rebound = tracker.first_rebound;
    if (rebound.button) {
        ASSERT_EQ(GTK_WIDGET(screen->recent_action_for_test(rebound.target, true)->gobj()), rebound.button);
        activate(rebound.target);
        auto const before = probes.size();
        g_signal_emit_by_name(rebound.button, "clicked");
        ASSERT_TRUE(waitUntil([&] { return !screen->recent_pending_for_test(); }));
        ASSERT_EQ(probes.size(), before + 1);
        EXPECT_EQ(probes.back(), rebound.target);
    } else {
        screen->search_recent_for_test("__vacards_no_matching_recent_card__");
        ASSERT_TRUE(waitUntil([&] { return tracker.unbinds > 0; }));
        std::cout << "Widget fallback: binds=" << tracker.binds << " unbinds=" << tracker.unbinds
                  << " live=0=" << tracker.unbind_disconnected << "\n";
        auto *button = tracker.unbound_action();
        ASSERT_NE(button, nullptr);
        ASSERT_TRUE(tracker.unbind_disconnected);
        auto const before = probes.size();
        g_signal_emit_by_name(button, "clicked");
        drainMainContext();
        EXPECT_EQ(probes.size(), before);
    }
}

TEST(WelcomeRecentGuiTest, CardAccessibleStates)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    auto const uri = absentUri();
    addRecent(*manager, uri, "Accessible.svg");
    auto screen = std::make_unique<StartScreen>();
    std::function<void(StartScreen::RecentProbeResult)> complete;
    screen->set_recent_probe_for_test([&](auto const &, auto callback) { complete = callback; });
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, false); }));
    auto *remove = GTK_WIDGET(screen->recent_action_for_test(uri, false)->gobj());
    auto *placeholder = findCardPlaceholder(gtk_widget_get_parent(gtk_widget_get_parent(remove)));
    ASSERT_NE(placeholder, nullptr);
    auto const location = std::string(manager->lookup_item(uri)->get_uri_display());
    auto check_status = [&](std::string const &message) {
        EXPECT_EQ(std::string(gtk_widget_get_tooltip_text(placeholder)), message);
        expectAccessibleLabel(placeholder, message);
    };
    check_status("Preview unavailable");
    EXPECT_EQ(screen->recent_announcement_count_for_test(), 0u);
    screen->set_recent_open_for_test([](auto const &) {});
    auto activate = [&] {
        auto const position = screen->recent_position_for_test(uri);
        ASSERT_NE(position, GTK_INVALID_LIST_POSITION);
        auto *grid = gtk_widget_get_ancestor(remove, GTK_TYPE_GRID_VIEW);
        ASSERT_NE(grid, nullptr);
        g_signal_emit_by_name(grid, "activate", position);
        ASSERT_TRUE(complete);
        check_status("Checking location…");
    };
    activate();
    complete(StartScreen::RecentProbeResult::Missing);
    check_status("File not found\n" + location);
    EXPECT_FALSE(screen->recent_action_for_test(uri, true)->get_visible());
    activate();
    complete(StartScreen::RecentProbeResult::PermissionDenied);
    check_status("Permission denied\n" + location);
    EXPECT_FALSE(screen->recent_action_for_test(uri, true)->get_visible());
    activate();
    complete(StartScreen::RecentProbeResult::Unreachable);
    check_status("Location not reachable\n" + location);
    EXPECT_TRUE(screen->recent_action_for_test(uri, true)->get_visible());
    EXPECT_EQ(screen->recent_announcement_count_for_test(), 6u);
}

TEST(WelcomeRecentGuiTest, PassiveSvgPreviewShowsImageAndCancelsOnSearch)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    Inkscape::IO::enable_file_io_test_hooks();
    (void)testApplication();
    auto const path = std::string(g_getenv("WELCOME_RECENT_CHILD_ROOT")) + "/preview.svg";
    char const *svg = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='30'>"
                      "<rect width='40' height='30' fill='red'/></svg>";
    ASSERT_TRUE(g_file_set_contents(path.c_str(), svg, -1, nullptr));
    auto const uri = Gio::File::create_for_path(path)->get_uri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "preview.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return cardPreviewImage(*screen, uri) != nullptr; }));
    auto *image = cardPreviewImage(*screen, uri);
    bool const rendered = waitUntil([&] { return gtk_widget_get_visible(image); }, 12000);
    auto *placeholder = gtk_widget_get_prev_sibling(image);
    EXPECT_TRUE(rendered) << "status=" << gtk_widget_get_tooltip_text(placeholder)
                          << " renders=" << screen->recent_preview_renders_for_test();
    if (!rendered) return;
    EXPECT_NE(gtk_picture_get_paintable(GTK_PICTURE(image)), nullptr);
    EXPECT_EQ(gtk_picture_get_content_fit(GTK_PICTURE(image)), GTK_CONTENT_FIT_CONTAIN);
    EXPECT_TRUE(waitUntil([&] { return gtk_widget_get_width(image) >= 150 &&
                                     gtk_widget_get_height(image) >= 113; }));
    auto *visual = gtk_widget_get_parent(image);
    ASSERT_NE(visual, nullptr);
    EXPECT_EQ(gtk_widget_get_width(image), gtk_widget_get_width(visual));
    EXPECT_EQ(gtk_widget_get_height(image), gtk_widget_get_height(visual));
    screen->search_recent_for_test("no matching preview card");
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(uri, false) == nullptr; }));
    EXPECT_EQ(screen->recent_retry_used_for_test(uri), 0u);
    screen.reset();
    std::filesystem::remove(path);
}

TEST(WelcomeRecentGuiTest, PassiveUnavailableDoesNotRenderOrSpendRetry)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto const uri = absentUri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "absent.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return cardPreviewImage(*screen, uri) != nullptr; }));
    ASSERT_TRUE(waitUntil([&] {
        auto *image = cardPreviewImage(*screen, uri);
        auto *placeholder = image ? gtk_widget_get_prev_sibling(image) : nullptr;
        auto *text = placeholder ? gtk_widget_get_tooltip_text(placeholder) : nullptr;
        bool const missing = text && std::string(text).find("File not found") != std::string::npos;
        return missing;
    }));
    EXPECT_EQ(screen->recent_preview_renders_for_test(), 0u);
    EXPECT_EQ(screen->recent_retry_used_for_test(uri), 0u);
}

TEST(WelcomeRecentGuiTest, ClickDuringPendingPassiveProbeOpens)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto path = std::string(g_getenv("WELCOME_RECENT_CHILD_ROOT")) + "/passive-click.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(),
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='30'/>", -1, nullptr));
    auto uri = Gio::File::create_for_path(path)->get_uri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "passive-click.svg");
    auto screen = std::make_unique<StartScreen>();
    unsigned passive = 0;
    bool opened = false;
    screen->set_recent_open_for_test([&](auto const &) { opened = true; });
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return cardPreviewImage(*screen, uri) != nullptr; }));
    screen->set_recent_passive_probe_for_test([&](auto const &, auto) { ++passive; });
    ASSERT_TRUE(waitUntil([&] { return passive > 0 && cardPreviewImage(*screen, uri); }));
    screen->activate_recent_for_test(uri);
    EXPECT_NE(screen->recent_result_for_test(), StartScreen::RecentProbeResult::Busy);
    EXPECT_TRUE(waitUntil([&] { return opened; }));
    screen.reset();
    std::filesystem::remove(path);
}

TEST(WelcomeRecentGuiTest, RecycledPreviewClearsPriorImage)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    Inkscape::IO::enable_file_io_test_hooks();
    (void)testApplication();
    auto const path = std::string(g_getenv("WELCOME_RECENT_CHILD_ROOT")) + "/first.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(),
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='30'><rect width='40' height='30'/></svg>",
        -1, nullptr));
    auto const first = Gio::File::create_for_path(path)->get_uri();
    auto const second = absentUri();
    addRecent(*Gtk::RecentManager::get_default(), first, "first.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return cardPreviewImage(*screen, first) != nullptr; }));
    ASSERT_TRUE(waitUntil([&] { return gtk_widget_get_visible(cardPreviewImage(*screen, first)); }, 12000));
    screen->replace_recent_for_test(first, second);
    ASSERT_TRUE(waitUntil([&] { return cardPreviewImage(*screen, second) != nullptr; }));
    auto *image = cardPreviewImage(*screen, second);
    EXPECT_FALSE(gtk_widget_get_visible(image));
    EXPECT_EQ(gtk_picture_get_paintable(GTK_PICTURE(image)), nullptr);
    screen.reset();
    std::filesystem::remove(path);
}

TEST(WelcomeRecentGuiTest, PreviewFailureKeepsOpenAndSearchCancelsDemand)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    Inkscape::IO::enable_file_io_test_hooks();
    (void)testApplication();
    auto const path = std::string(g_getenv("WELCOME_RECENT_CHILD_ROOT")) + "/failing.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(),
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='30'><rect width='40' height='30'/></svg>",
        -1, nullptr));
    auto const uri = Gio::File::create_for_path(path)->get_uri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "failing.svg");
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "abort", true);
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_preview_finished_for_test(uri); }, 12000));
    auto *image = cardPreviewImage(*screen, uri);
    ASSERT_NE(image, nullptr);
    EXPECT_FALSE(gtk_widget_get_visible(image));
    auto *placeholder = gtk_widget_get_prev_sibling(image);
    ASSERT_NE(placeholder, nullptr);
    EXPECT_STREQ(gtk_widget_get_tooltip_text(placeholder), "Preview unavailable");
    bool opened = false;
    screen->set_recent_open_for_test([&](auto const &) { opened = true; });
    auto *grid = gtk_widget_get_ancestor(image, GTK_TYPE_GRID_VIEW);
    ASSERT_NE(grid, nullptr);
    g_signal_emit_by_name(grid, "activate", screen->recent_position_for_test(uri));
    EXPECT_TRUE(waitUntil([&] { return opened; }));
    screen->search_recent_for_test("no matching failing card");
    ASSERT_TRUE(waitUntil([&] { return screen->recent_preview_demands_for_test() == 0; }));
    screen.reset();
    g_unsetenv("VACARDS_PREVIEW_TEST_ACTION");
    std::filesystem::remove(path);
}

TEST(WelcomeRecentGuiTest, SearchAndCloseCancelOutstandingPreview)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    Inkscape::IO::enable_file_io_test_hooks();
    (void)testApplication();
    auto const path = std::string(g_getenv("WELCOME_RECENT_CHILD_ROOT")) + "/slow.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(),
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='30'><rect width='40' height='30'/></svg>",
        -1, nullptr));
    auto const uri = Gio::File::create_for_path(path)->get_uri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "slow.svg");
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "sleep", true);
    auto screen = std::make_unique<StartScreen>();
    unsigned cancels = 0;
    screen->set_recent_preview_cancel_for_test([&](auto const &cancelled) {
        if (cancelled == uri) ++cancels;
    });
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_preview_renders_for_test() >= 1; }));
    EXPECT_EQ(screen->recent_preview_demands_for_test(), 1u);
    screen->search_recent_for_test("no matching slow card");
    ASSERT_TRUE(waitUntil([&] { return screen->recent_preview_demands_for_test() == 0; }));
    EXPECT_EQ(cancels, 1u);
    screen->search_recent_for_test("");
    ASSERT_TRUE(waitUntil([&] { return screen->recent_preview_demands_for_test() == 1; }));
    screen.reset();
    EXPECT_EQ(cancels, 2u);
    g_unsetenv("VACARDS_PREVIEW_TEST_ACTION");
    std::filesystem::remove(path);
}

TEST(WelcomeRecentGuiTest, ScrollCancelsOutstandingPreview)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    Inkscape::IO::enable_file_io_test_hooks();
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    for (int i = 0; i < 90; ++i) {
        auto other = absentUri();
        addRecent(*manager, other, "older.svg");
    }
    auto const path = std::string(g_getenv("WELCOME_RECENT_CHILD_ROOT")) + "/scroll.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(),
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='30'><rect width='40' height='30'/></svg>",
        -1, nullptr));
    auto const uri = Gio::File::create_for_path(path)->get_uri();
    addRecent(*manager, uri, "scroll.svg");
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "sleep", true);
    auto screen = std::make_unique<StartScreen>();
    bool cancelled = false;
    screen->set_recent_preview_cancel_for_test([&](auto const &path) {
        if (path == uri) cancelled = true;
    });
    screen->present();
    EXPECT_TRUE(screen->recent_scroll_connected_for_test());
    ASSERT_TRUE(waitUntil([&] { return cardPreviewImage(*screen, uri) != nullptr; }));
    auto *scroll = gtk_widget_get_ancestor(cardPreviewImage(*screen, uri), GTK_TYPE_SCROLLED_WINDOW);
    ASSERT_NE(scroll, nullptr);
    auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    ASSERT_TRUE(waitUntil([&] { return gtk_adjustment_get_upper(adjustment) >
                                       gtk_adjustment_get_page_size(adjustment); }));
    ASSERT_TRUE(waitUntil([&] { return screen->recent_preview_renders_for_test() >= 1; }));
    auto const maximum = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
    gtk_adjustment_set_value(adjustment, maximum);
    drainMainContext();
    EXPECT_TRUE(waitUntil([&] { return cancelled; }));
    screen.reset();
    g_unsetenv("VACARDS_PREVIEW_TEST_ACTION");
    std::filesystem::remove(path);
}

#ifdef __APPLE__
TEST(WelcomeRecentGuiTest, PassiveUnreachableNeverRenders)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto const uri = Gio::File::create_for_path("/Volumes/vacards-absent-preview-root/file.svg")->get_uri();
    addRecent(*Gtk::RecentManager::get_default(), uri, "offline.svg");
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return cardPreviewImage(*screen, uri) != nullptr; }));
    ASSERT_TRUE(waitUntil([&] {
        auto *placeholder = gtk_widget_get_prev_sibling(cardPreviewImage(*screen, uri));
        auto *text = gtk_widget_get_tooltip_text(placeholder);
        return text && std::string(text).find("Location not reachable") != std::string::npos;
    }, 7000));
    EXPECT_EQ(screen->recent_preview_renders_for_test(), 0u);
    EXPECT_EQ(screen->recent_retry_used_for_test(uri), 0u);
}
#endif

TEST(WelcomeRecentGuiTest, RemoveAllDisplayLocationVariants)
{
    if (!welcomeGuiEnabled()) GTEST_SKIP() << "GUI testing not enabled";
    if (!isolatedRecentChild()) return;
    (void)testApplication();
    auto manager = Gtk::RecentManager::get_default();
    std::unique_ptr<char, decltype(&g_free)> uuid(g_uuid_string_random(), &g_free);
    auto const path = std::string(g_get_tmp_dir()) + "/vacards-variant;" + uuid.get() + ".svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(), "<svg xmlns='http://www.w3.org/2000/svg'/>", -1, nullptr));
    auto const encoded = std::string(Gio::File::create_for_path(path)->get_uri());
    auto alternate = encoded;
    if (auto const index = alternate.find("%3B"); index != std::string::npos) alternate.replace(index, 3, ";");
    else if (auto const index = alternate.find(';'); index != std::string::npos) alternate.replace(index, 1, "%3B");
    ASSERT_NE(encoded, alternate);
    auto const unrelated = absentUri();
    addRecent(*manager, encoded, "variant.svg");
    addRecent(*manager, alternate, "variant.svg");
    addRecent(*manager, unrelated, "unrelated.svg");
    auto one = manager->lookup_item(encoded);
    auto two = manager->lookup_item(alternate);
    ASSERT_TRUE(one && two) << "Fixture requires two stored raw URIs";
    ASSERT_EQ(one->get_uri_display(), two->get_uri_display()) << "Unsupported GTK URI variant fixture";
    auto screen = std::make_unique<StartScreen>();
    screen->present();
    ASSERT_TRUE(waitUntil([&] { return screen->recent_action_for_test(encoded, false) ||
                                      screen->recent_action_for_test(alternate, false); }));
    auto *button = screen->recent_action_for_test(encoded, false);
    if (!button) button = screen->recent_action_for_test(alternate, false);
    auto const refreshes = screen->recent_refresh_count_for_test();
    g_signal_emit_by_name(button->gobj(), "clicked");
    EXPECT_FALSE(manager->has_item(encoded));
    EXPECT_FALSE(manager->has_item(alternate));
    EXPECT_TRUE(manager->has_item(unrelated));
    EXPECT_TRUE(g_file_test(path.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(screen->recent_refresh_count_for_test(), refreshes + 1);
    g_unlink(path.c_str());
}

} // namespace
