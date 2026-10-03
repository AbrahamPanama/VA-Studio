// SPDX-License-Identifier: GPL-2.0-or-later
//
// Isolated app-owned Preferences admission/restore/lifetime integration test
// executable, with the unique application id tag "preferencesadmissiontest". It
// runs in its own process and is gated behind INKSCAPE_TEST_GUI. It exercises
// real native widget routing through the single app-owned native Preferences
// host: docked/floating dialog entry points, the native page-list selection,
// filtered restore of saved dialog state, and host/slot lifetime when an
// initiating floating container is destroyed during its own request.
//
// Non-owned behavior is covered by the existing preferences-presenter target.

#include "config.h"

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <gtkmm/treemodel.h>
#include <gtkmm/treeselection.h>
#include <gtkmm/treeview.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include <glibmm/i18n.h>
#include <glibmm/keyfile.h>
#include <sigc++/adaptors/track_obj.h>
#include <sigc++/scoped_connection.h>

#include "actions/actions-tools.h"
#include "desktop.h"
#include "document.h"
#include "enums.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "preferences.h"
#include "ui/dialog/dialog-base.h"
#include "ui/dialog/dialog-container.h"
#include "ui/dialog/dialog-manager.h"
#include "ui/dialog/dialog-notebook.h"
#include "ui/dialog/dialog-window.h"
#include "ui/dialog/inkscape-preferences.h"

using namespace Inkscape;
using namespace Inkscape::UI::Dialog;

namespace {

// Reused real application, unique tag for the separate test process.
InkscapeApplication &testApplication()
{
    static auto application = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "preferencesadmissiontest", true);
        auto result = new InkscapeApplication();
        result->gio_app()->register_application();
        // Preserve fatal failures (including shutdown) as test failures instead
        // of waiting in an interactive emergency dialog.
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

// Test-local snapshot of the raw preference strings routing mutates, so the
// exact original value and set/unset state are restored after the fixture has
// destroyed its real desktop. `Preferences::getEntry(path).isSet()` is the
// actual API (there is no `Preferences::isSet(path)`).
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

// Saves and restores selected preference values around a case, including when an
// ASSERT returns early: the destructor always runs on scope exit.
struct ScopedPreferenceValues {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    std::vector<PrefSnapshot> saved;

    explicit ScopedPreferenceValues(std::initializer_list<Glib::ustring> paths)
    {
        if (!prefs) return;
        for (auto const &path : paths) {
            saved.push_back(snapshotPreference(*prefs, path));
        }
    }

    ~ScopedPreferenceValues()
    {
        if (!prefs) return;
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            restorePreference(*prefs, *it);
        }
    }
};

// Build one saved window group in an in-memory KeyFile. Group and key names
// follow DialogContainer::save_container_state(); nothing is read from disk.
void addRestoreWindow(Glib::KeyFile &state, int window_idx, bool floating,
                      std::vector<Glib::ustring> const &dialogs, int active)
{
    auto const window = Glib::ustring::compose("Window%1", window_idx);
    auto const column = Glib::ustring::compose("Window%1Column0", window_idx);
    state.set_integer(window, "ColumnCount", 1);
    state.set_boolean(window, "Floating", floating);
    state.set_integer(column, "NotebookCount", 1);
    state.set_boolean(column, "BeforeCanvas", false);
    state.set_string_list(column, "Notebook0Dialogs", dialogs);
    state.set_integer(column, "Notebook0ActiveTab", active);
}

// Count actual native page-list notebooks under a widget, used only to show that
// a filtered owned-only restore adds none.
std::size_t countDialogNotebooks(Gtk::Widget &root)
{
    std::size_t count = dynamic_cast<DialogNotebook *>(&root) ? 1 : 0;
    for (auto *child = root.get_first_child(); child; child = child->get_next_sibling()) {
        count += countDialogNotebooks(*child);
    }
    return count;
}

// The DialogManager transient map is process-wide; clear only the states this
// isolated executable's cases own. Never touches an app-owned host.
void clearTestTransientStates()
{
    auto &manager = DialogManager::singleton();
    manager.remove_dialog_floating_state("Preferences");
    manager.remove_dialog_floating_state("FillStroke");
    manager.remove_dialog_floating_state("AlignDistribute");
}

// Parameter for the mixed saved-tab restore cases.
struct MixedSavedTabsParam {
    std::vector<Glib::ustring> tabs;
    int active = 0;
    Glib::ustring expected;
    std::string name;
};

std::string mixedSavedTabsName(::testing::TestParamInfo<MixedSavedTabsParam> const &info)
{
    return info.param.name;
}

std::string inlineSvg()
{
    return "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20' "
           "viewBox='0 0 20 20'><rect id='admission-rect' width='5' height='5'/></svg>";
}

// Native floating DialogWindows created by this exact application. The owned
// Preferences host is an ordinary Gtk::Window and is deliberately NOT in this
// manager list, so it is never counted or closed here.
std::size_t ownedFloatingDialogCount(InkscapeApplication &app)
{
    std::size_t count = 0;
    for (auto *window : DialogManager::singleton().get_all_floating_dialog_windows()) {
        if (window && window->get_application_owner() == &app) {
            ++count;
        }
    }
    return count;
}

// Cleanup only actual native floating DialogWindows owned by this exact
// application, before the real desktop is destroyed. Never touches an owned
// ordinary Gtk::Window and performs no manual unmount.
void closeOwnedFloatingDialogs(InkscapeApplication &app)
{
    for (auto *window : DialogManager::singleton().get_all_floating_dialog_windows()) {
        if (window && window->get_application_owner() == &app) {
            window->close();
        }
    }
    drainMainContext();
}

// Test-local collection of native page-list TreeViews: 3-column model with
// exactly one displayed column titled "name". Found by widget descent because
// the production member is protected; no production getter and no direct
// selection mutation. Callers assert exactly one match before use.
std::vector<Gtk::TreeView *> collectMatchingNativeTreeViews(Gtk::Widget &root)
{
    std::vector<Gtk::TreeView *> views;
    if (auto *view = dynamic_cast<Gtk::TreeView *>(&root)) {
        auto model = view->get_model();
        if (model && model->get_n_columns() == 3) {
            auto const columns = view->get_columns();
            if (columns.size() == 1 && columns[0] && columns[0]->get_title() == "name") {
                views.push_back(view);
            }
        }
    }
    for (auto *child = root.get_first_child(); child; child = child->get_next_sibling()) {
        auto found = collectMatchingNativeTreeViews(*child);
        views.insert(views.end(), found.begin(), found.end());
    }
    return views;
}

// Actual native selection oracle. The model column record registers
// name(0), page(1), id(2); declaration order differs but registration order
// fixes index 2 as the int page id.
int selectedPageId(Gtk::TreeView &view)
{
    auto selection = view.get_selection();
    if (!selection) return -1;
    auto iter = selection->get_selected();
    if (!iter) return -1;
    int id = -1;
    iter->get_value(2, id);
    return id;
}

class PreferencesAdmissionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        auto &app = testApplication();
        initialized = true;
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
        routing_pref_guard->snapshots.push_back(snapshotPreference(*prefs, "/dialogs/preferences/page"));
        prefs->setInt("/options/savedialogposition/value", PREFS_DIALOGS_STATE_NONE);
        prefs->setInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE);
        drainMainContext();

        // Cold admission must happen BEFORE any real document exists, and only
        // when the application does not already own a host. The retained host is
        // then kept hidden between tests.
        ASSERT_TRUE(app.get_documents().empty());
        if (!app.ownedPreferencesWindow()) {
            app.presentPreferences();
        }
        drainMainContext();

        owned_host = app.ownedPreferencesWindow();
        ASSERT_TRUE(owned_host);
        owned_panel = dynamic_cast<InkscapePreferences *>(owned_host->get_child());
        ASSERT_TRUE(owned_panel);
        owned_host->hide();
        drainMainContext();
        ASSERT_FALSE(owned_host->get_visible());

        // Process-wide transient dialog state must not leak into a case; clear
        // only the states this executable's cases own.
        clearTestTransientStates();

        // One real inline-SVG document + native desktop for actual container
        // requests. No reset API, mocks or dummy document for Preferences.
        document = app.document_add(SPDocument::createNewDocFromMem(inlineSvg()));
        ASSERT_TRUE(document);
        desktop = app.createDesktop(document, false, true);
        ASSERT_TRUE(desktop);
        document->ensureUpToDate();
        drainMainContext();
    }

    void TearDown() override
    {
        // GUI-gated skip leaves SetUp before the application exists; never
        // initialize it here.
        if (!initialized) return;
        auto &app = testApplication();
        // Cleanup actual native DialogWindows created in this isolated process
        // before destroying the real desktop; only this exact owner app.
        closeOwnedFloatingDialogs(app);

        // Closing floating windows stores new transient state; drop only the
        // states this executable owns before the next case.
        clearTestTransientStates();

        // The real document/desktop is destroyed last so no owned panel can
        // outlive it. No arbitrary panel is torn down from here.
        if (document) document->setModifiedSinceSave(false);
        if (desktop) app.destroyDesktop(desktop);
        else if (document) app.document_close(document);
        desktop = nullptr;
        document = nullptr;
        drainMainContext();

        // Retained app-owned host stays hidden between tests.
        if (auto *host = app.ownedPreferencesWindow()) {
            host->hide();
            drainMainContext();
        }

        // Restore routing preferences only after all desktop closes.
        routing_pref_guard.reset();
    }

    InkscapeApplication &app() { return testApplication(); }

    bool initialized = false;
    Gtk::Window *owned_host = nullptr;
    InkscapePreferences *owned_panel = nullptr;
    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    std::unique_ptr<RoutingPreferenceGuard> routing_pref_guard;
};

// 1. A silent docked admission (ensure_visibility=false) must not reveal the
//    retained hidden host and must not create any native Preferences duplicate.
TEST_F(PreferencesAdmissionTest, SilentAdmissionDoesNotRevealOwnedHost)
{
    auto &application = this->app();
    ASSERT_TRUE(owned_host);
    ASSERT_TRUE(owned_panel);
    ASSERT_FALSE(owned_host->get_visible());

    unsigned visible_changes = 0;
    unsigned mapped_changes = 0;
    sigc::scoped_connection visible_conn = owned_host->property_visible().signal_changed().connect([&] {
        ++visible_changes;
    });
    sigc::scoped_connection mapped_conn = owned_host->signal_map().connect([&] {
        ++mapped_changes;
    });

    auto *container = desktop->getContainer();
    ASSERT_TRUE(container);
    ASSERT_EQ(container->get_dialog("Preferences"), nullptr);

    container->new_dialog("Preferences", nullptr, false);
    drainMainContext();

    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);
    EXPECT_EQ(visible_changes, 0u);
    EXPECT_EQ(mapped_changes, 0u);
    EXPECT_FALSE(owned_host->get_visible());
    EXPECT_FALSE(owned_host->get_mapped());
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(ownedFloatingDialogCount(application), 0u);

    visible_conn.disconnect();
    mapped_conn.disconnect();
}

// 2. Each explicit public native entry point reuses the retained owned host and
//    never creates a duplicate native Preferences panel.
TEST_F(PreferencesAdmissionTest, ExplicitNativeEntriesReuseOwnedHost)
{
    auto &application = this->app();
    auto *container = desktop->getContainer();
    ASSERT_TRUE(container);

    // Route A: simple docked new_dialog.
    owned_host->hide();
    drainMainContext();
    ASSERT_FALSE(owned_host->get_visible());
    container->new_dialog("Preferences");
    drainMainContext();
    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);
    EXPECT_TRUE(owned_host->get_visible());
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(ownedFloatingDialogCount(application), 0u);

    // Route B: explicit notebook/null overload with ensure_visibility=true.
    owned_host->hide();
    drainMainContext();
    ASSERT_FALSE(owned_host->get_visible());
    container->new_dialog("Preferences", nullptr, true);
    drainMainContext();
    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);
    EXPECT_TRUE(owned_host->get_visible());
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(ownedFloatingDialogCount(application), 0u);

    // Route C: floating request must reuse the owned host and return nullptr.
    owned_host->hide();
    drainMainContext();
    ASSERT_FALSE(owned_host->get_visible());
    auto *floating = container->new_floating_dialog("Preferences");
    drainMainContext();
    EXPECT_EQ(floating, nullptr);
    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);
    EXPECT_TRUE(owned_host->get_visible());
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(ownedFloatingDialogCount(application), 0u);
}

// 3. An already-visible owned host is used by production tool_preferences and
//    the native page-list selection (not the persisted preference) reports the
//    requested tool page.
TEST_F(PreferencesAdmissionTest, AlreadyVisibleOwnedToolPageUsesNativeSelection)
{
    auto &application = this->app();
    auto *realwin = desktop->getInkscapeWindow();
    ASSERT_TRUE(realwin);
    ASSERT_EQ(realwin->get_application_owner(), &application);

    application.presentPreferences(realwin);
    drainMainContext();

    ASSERT_TRUE(owned_host);
    ASSERT_TRUE(owned_panel);
    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);
    EXPECT_TRUE(owned_host->get_visible());
    EXPECT_TRUE(owned_host->get_mapped());

    auto views = collectMatchingNativeTreeViews(*owned_panel);
    ASSERT_EQ(views.size(), 1u);
    auto *view = views[0];
    ASSERT_TRUE(view);
    auto model = view->get_model();
    ASSERT_TRUE(model);
    ASSERT_EQ(model->get_n_columns(), 3u);
    ASSERT_EQ(model->get_column_type(2), G_TYPE_INT);
    auto const columns = view->get_columns();
    ASSERT_EQ(columns.size(), 1u);
    EXPECT_EQ(columns[0]->get_title(), "name");

    tool_preferences("Node", realwin);
    drainMainContext();
    EXPECT_EQ(selectedPageId(*view), static_cast<int>(PREFS_PAGE_TOOLS_NODE));
    EXPECT_TRUE(owned_host->get_visible());
    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);

    tool_preferences("Rect", realwin);
    drainMainContext();
    EXPECT_EQ(selectedPageId(*view), static_cast<int>(PREFS_PAGE_TOOLS_SHAPES_RECT));
    EXPECT_TRUE(owned_host->get_visible());
    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);
}

// A. A docked restore whose only saved panel is the owned native Preferences
//    host must not reveal the host, must not add a Preferences panel and must
//    not allocate a notebook. The empty native column is allowed.
TEST_F(PreferencesAdmissionTest, OwnedOnlyDockedRestoreLeavesNoNotebook)
{
    auto &application = this->app();
    ASSERT_TRUE(owned_host);
    ASSERT_FALSE(owned_host->get_visible());

    auto *container = desktop->getContainer();
    ASSERT_TRUE(container);
    ASSERT_TRUE(container->get_dialogs().empty());
    auto const notebooks_before = countDialogNotebooks(*container);

    unsigned visible_changes = 0;
    unsigned mapped_changes = 0;
    sigc::scoped_connection visible_conn = owned_host->property_visible().signal_changed().connect([&] {
        ++visible_changes;
    });
    sigc::scoped_connection mapped_conn = owned_host->signal_map().connect([&] {
        ++mapped_changes;
    });

    auto state = Glib::KeyFile::create();
    state->set_integer("Windows", "Count", 1);
    addRestoreWindow(*state, 0, false, {"Preferences"}, 0);

    container->load_container_state(state.get(), false);
    drainMainContext();

    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_FALSE(owned_host->get_visible());
    EXPECT_EQ(visible_changes, 0u);
    EXPECT_EQ(mapped_changes, 0u);
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(countDialogNotebooks(*container), notebooks_before);
    EXPECT_EQ(ownedFloatingDialogCount(application), 0u);

    visible_conn.disconnect();
    mapped_conn.disconnect();
}

// B. A floating saved window whose only panel is the owned Preferences host is
//    dropped, and restore CONTINUES to a later saved window instead of stopping.
TEST_F(PreferencesAdmissionTest, OwnedOnlyFloatingWindowDoesNotBlockLaterWindow)
{
    auto &application = this->app();
    ASSERT_TRUE(owned_host);
    ASSERT_FALSE(owned_host->get_visible());

    auto *container = desktop->getContainer();
    ASSERT_TRUE(container);
    ASSERT_TRUE(container->get_dialogs().empty());

    unsigned visible_changes = 0;
    unsigned mapped_changes = 0;
    sigc::scoped_connection visible_conn = owned_host->property_visible().signal_changed().connect([&] {
        ++visible_changes;
    });
    sigc::scoped_connection mapped_conn = owned_host->signal_map().connect([&] {
        ++mapped_changes;
    });

    auto state = Glib::KeyFile::create();
    state->set_integer("Windows", "Count", 2);
    addRestoreWindow(*state, 0, true, {"Preferences"}, 0);
    addRestoreWindow(*state, 1, true, {"FillStroke"}, 0);

    container->load_container_state(state.get(), true);
    drainMainContext();

    auto *fill_window = DialogManager::singleton().find_floating_dialog_window("FillStroke");
    ASSERT_TRUE(fill_window);
    EXPECT_EQ(fill_window->get_application_owner(), &application);
    EXPECT_TRUE(fill_window->get_visible());
    ASSERT_TRUE(fill_window->get_container());
    EXPECT_NE(fill_window->get_container()->get_dialog("FillStroke"), nullptr);
    EXPECT_EQ(fill_window->get_container()->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog("Preferences"), nullptr);
    EXPECT_EQ(ownedFloatingDialogCount(application), 1u);

    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_FALSE(owned_host->get_visible());
    EXPECT_EQ(visible_changes, 0u);
    EXPECT_EQ(mapped_changes, 0u);

    visible_conn.disconnect();
    mapped_conn.disconnect();
}

// C. Mixed saved tabs: after the owned Preferences tab is filtered, the native
//    active tab is chosen by identity (next before previous), never by a copied
//    index. The last parameter is the no-owned-removal control.
class PreferencesMixedSavedTabsTest : public PreferencesAdmissionTest,
                                      public ::testing::WithParamInterface<MixedSavedTabsParam>
{
};

TEST_P(PreferencesMixedSavedTabsTest, MixedSavedTabsPreserveNativeSelection)
{
    auto &application = this->app();
    auto *container = desktop->getContainer();
    ASSERT_TRUE(container);
    ASSERT_TRUE(container->get_dialogs().empty());

    auto state = Glib::KeyFile::create();
    state->set_integer("Windows", "Count", 1);
    addRestoreWindow(*state, 0, false, GetParam().tabs, GetParam().active);

    container->load_container_state(state.get(), false);
    drainMainContext();

    auto *fill = container->get_dialog("FillStroke");
    auto *align = container->get_dialog("AlignDistribute");
    ASSERT_TRUE(fill);
    ASSERT_TRUE(align);
    auto *fill_notebook = DialogNotebook::get_page_notebook(*fill);
    auto *align_notebook = DialogNotebook::get_page_notebook(*align);
    ASSERT_TRUE(fill_notebook);
    EXPECT_EQ(fill_notebook, align_notebook);

    auto *expected = container->get_dialog(GetParam().expected);
    ASSERT_TRUE(expected);
    auto const current = fill_notebook->get_current_page();
    ASSERT_GE(current, 0);
    EXPECT_EQ(fill_notebook->get_nth_page(current), static_cast<Gtk::Widget *>(expected));
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);

    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_FALSE(owned_host->get_visible());
}

INSTANTIATE_TEST_SUITE_P(
    MixedSavedTabs, PreferencesMixedSavedTabsTest,
    ::testing::Values(
        MixedSavedTabsParam{{"Preferences", "FillStroke", "AlignDistribute"}, 2, "AlignDistribute", "PrefsFirstActive2"},
        MixedSavedTabsParam{{"FillStroke", "Preferences", "AlignDistribute"}, 2, "AlignDistribute", "PrefsMiddleActive2"},
        MixedSavedTabsParam{{"Preferences", "FillStroke", "AlignDistribute"}, 0, "FillStroke", "PrefsFirstActive0"},
        MixedSavedTabsParam{{"FillStroke", "Preferences", "AlignDistribute"}, 1, "AlignDistribute", "PrefsMiddleActive1"},
        MixedSavedTabsParam{{"FillStroke", "AlignDistribute", "Preferences"}, 2, "AlignDistribute", "PrefsLastActive2"},
        MixedSavedTabsParam{{"FillStroke", "AlignDistribute"}, 1, "AlignDistribute", "NoOwnedActive1"}),
    mixedSavedTabsName);

// D. With docking disabled, a mixed saved window still restores the native panel
//    floating and never re-adds the owned Preferences host or an empty wrapper.
TEST_F(PreferencesAdmissionTest, ForcedFloatingRestoreSuppressesOwnedPreferences)
{
    auto &application = this->app();
    ASSERT_TRUE(owned_host);
    ASSERT_FALSE(owned_host->get_visible());

    auto *container = desktop->getContainer();
    ASSERT_TRUE(container);
    ASSERT_TRUE(container->get_dialogs().empty());

    auto *prefs = Inkscape::Preferences::get();
    ASSERT_TRUE(prefs);
    ScopedPreferenceValues guard({"/options/dialogtype/value"});
    prefs->setInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_FLOATING);

    auto state = Glib::KeyFile::create();
    state->set_integer("Windows", "Count", 1);
    addRestoreWindow(*state, 0, false, {"Preferences", "FillStroke"}, 0);

    container->load_container_state(state.get(), false);
    drainMainContext();

    auto *fill_window = DialogManager::singleton().find_floating_dialog_window("FillStroke");
    ASSERT_TRUE(fill_window);
    EXPECT_EQ(fill_window->get_application_owner(), &application);
    EXPECT_TRUE(fill_window->get_visible());
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog("Preferences"), nullptr);
    EXPECT_EQ(ownedFloatingDialogCount(application), 1u);
    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_FALSE(owned_host->get_visible());
}

// E. A transient state whose first floating window is the owned-only
//    Preferences must not abort restore of the later native panel. The private
//    recreate path is exercised only through public native operations, and the
//    transient state is populated in memory.
TEST_F(PreferencesAdmissionTest, TransientOwnedOnlyWindowDoesNotAbortLaterNativePanel)
{
    auto &application = this->app();
    ASSERT_TRUE(owned_host);
    ASSERT_FALSE(owned_host->get_visible());

    auto *container = desktop->getContainer();
    ASSERT_TRUE(container);
    ASSERT_TRUE(container->get_dialogs().empty());

    auto *real_window = container->new_floating_dialog("FillStroke");
    ASSERT_TRUE(real_window);
    ASSERT_EQ(real_window->get_application_owner(), &application);
    ASSERT_EQ(DialogManager::singleton().find_floating_dialog_window("FillStroke"), real_window);

    DialogManager::singleton().store_state(*real_window);
    drainMainContext();

    // Close the real fixture window BEFORE mutating its state: the native close
    // handler stores fresh state and deletes the window, so re-find afterwards
    // to mutate the live transient state rather than a clobbered pointer.
    real_window->close();
    drainMainContext();
    real_window = nullptr;

    // The seed window must really be gone before its state is reused: no live
    // FillStroke floating window and no owned floating window remain. This
    // proves the closed seed cannot be mistaken for an existing-panel reuse.
    ASSERT_EQ(DialogManager::singleton().find_floating_dialog_window("FillStroke"), nullptr);
    ASSERT_EQ(ownedFloatingDialogCount(application), 0u);

    auto state = DialogManager::singleton().find_dialog_state("FillStroke");
    ASSERT_TRUE(state);

    auto transient = Glib::KeyFile::create();
    transient->set_integer("Windows", "Count", 2);
    addRestoreWindow(*transient, 0, true, {"Preferences"}, 0);
    addRestoreWindow(*transient, 1, true, {"FillStroke"}, 0);
    ASSERT_TRUE(state->load_from_data(transient->to_data()));

    auto *prefs = Inkscape::Preferences::get();
    ASSERT_TRUE(prefs);
    ScopedPreferenceValues guard({"/options/savedialogposition/value", "/options/dialogtype/value"});
    prefs->setInt("/options/savedialogposition/value", PREFS_DIALOGS_STATE_SAVE);
    prefs->setInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE);

    unsigned visible_changes = 0;
    unsigned mapped_changes = 0;
    sigc::scoped_connection visible_conn = owned_host->property_visible().signal_changed().connect([&] {
        ++visible_changes;
    });
    sigc::scoped_connection mapped_conn = owned_host->signal_map().connect([&] {
        ++mapped_changes;
    });

    auto *returned = container->new_floating_dialog("FillStroke");
    EXPECT_EQ(returned, nullptr);
    drainMainContext();

    auto *restored_window = DialogManager::singleton().find_floating_dialog_window("FillStroke");
    ASSERT_TRUE(restored_window);
    EXPECT_EQ(restored_window->get_application_owner(), &application);
    EXPECT_TRUE(restored_window->get_visible());
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog("Preferences"), nullptr);
    EXPECT_EQ(ownedFloatingDialogCount(application), 1u);
    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_FALSE(owned_host->get_visible());
    EXPECT_EQ(visible_changes, 0u);
    EXPECT_EQ(mapped_changes, 0u);

    visible_conn.disconnect();
    mapped_conn.disconnect();
}

// Public native entry route used by the destruction-during-reveal case.
enum class RevealRoute {
    SimpleNewDialog,
    ExplicitNewDialog,
    FloatingNewDialog,
};

struct RevealRouteParam {
    RevealRoute route;
    std::string name;
};

std::string revealRouteName(::testing::TestParamInfo<RevealRouteParam> const &info)
{
    return info.param.name;
}

// F. A DialogWindow whose attached InkscapeWindow was cleared must still route
//    the owned Preferences request through its fixed creating application owner
//    (the DialogWindow root), not through the absent attached window.
TEST_F(PreferencesAdmissionTest, DetachedFloatingContainerUsesCreatingOwner)
{
    auto &application = this->app();
    auto *realwin = desktop->getInkscapeWindow();
    ASSERT_TRUE(realwin);
    ASSERT_EQ(realwin->get_application_owner(), &application);
    ASSERT_TRUE(owned_host);
    ASSERT_TRUE(owned_panel);
    owned_host->hide();
    drainMainContext();
    ASSERT_FALSE(owned_host->get_visible());

    // Real native DialogWindow with no attached InkscapeWindow; only its fixed
    // creating owner can route the owned Preferences host.
    std::unique_ptr<DialogWindow> initiator = std::make_unique<DialogWindow>(realwin, nullptr);
    ASSERT_TRUE(initiator);
    initiator->set_inkscape_window(nullptr);
    ASSERT_EQ(initiator->get_inkscape_window(), nullptr);
    ASSERT_TRUE(initiator->get_container());
    ASSERT_EQ(initiator->get_container()->get_inkscape_window(), nullptr);
    ASSERT_EQ(initiator->get_application_owner(), &application);

    auto *container = initiator->get_container();

    auto const native_before = ownedFloatingDialogCount(application);
    container->new_dialog("Preferences", nullptr, true);
    drainMainContext();

    EXPECT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);
    EXPECT_TRUE(owned_host->get_visible());
    EXPECT_EQ(container->get_dialog("Preferences"), nullptr);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog("Preferences"), nullptr);

    // The initiator itself is one native DialogWindow; the owned route must add
    // no other. Its RAII scope ends here, before fixture teardown.
    EXPECT_EQ(ownedFloatingDialogCount(application), native_before);
}

// G. Destroying the initiating floating DialogWindow while its own dialog
//    request is still on the stack must not damage the retained owned host or
//    leave dangling tracked state.
class PreferencesDestructionDuringRevealTest : public PreferencesAdmissionTest,
                                               public ::testing::WithParamInterface<RevealRouteParam>
{
};

TEST_P(PreferencesDestructionDuringRevealTest, InitiatingWindowDestroyedDuringReveal)
{
    auto &application = this->app();
    auto *realwin = desktop->getInkscapeWindow();
    ASSERT_TRUE(realwin);
    ASSERT_EQ(realwin->get_application_owner(), &application);
    ASSERT_TRUE(owned_host);
    ASSERT_TRUE(owned_panel);
    owned_host->hide();
    drainMainContext();
    ASSERT_FALSE(owned_host->get_visible());

    auto const native_baseline = ownedFloatingDialogCount(application);

    auto initiator = std::make_unique<DialogWindow>(realwin, nullptr);
    ASSERT_TRUE(initiator);
    auto *borrowed_container = initiator->get_container();
    ASSERT_TRUE(borrowed_container);

    // Independent tracked lifetime oracles for the initiating window and its
    // native container, captured before the request.
    sigc::slot<bool()> window_alive = sigc::track_object([] { return true; }, *initiator);
    sigc::slot<bool()> container_alive = sigc::track_object([] { return true; }, *borrowed_container);
    ASSERT_FALSE(window_alive.empty());
    ASSERT_FALSE(container_alive.empty());

    bool in_request = false;
    bool called = false;
    bool callback_inside_request = false;
    unsigned reset_count = 0;
    sigc::scoped_connection visible_conn = owned_host->property_visible().signal_changed().connect([&] {
        if (!owned_host->get_visible()) return;
        if (called) return;
        called = true;
        ++reset_count;
        callback_inside_request = in_request;
        // Destroy ONLY the initiating window while its own request member call
        // is on the stack. No manual unmount and no production change.
        initiator.reset();
    });

    in_request = true;
    DialogWindow *returned = nullptr;
    switch (GetParam().route) {
    case RevealRoute::SimpleNewDialog:
        borrowed_container->new_dialog("Preferences");
        break;
    case RevealRoute::ExplicitNewDialog:
        borrowed_container->new_dialog("Preferences", nullptr, true);
        break;
    case RevealRoute::FloatingNewDialog:
        returned = borrowed_container->new_floating_dialog("Preferences");
        break;
    }
    in_request = false;
    // After return the initiating window and its container are destroyed; never
    // dereference either borrowed pointer again.
    borrowed_container = nullptr;

    EXPECT_TRUE(called);
    EXPECT_TRUE(callback_inside_request);
    EXPECT_TRUE(window_alive.empty());
    EXPECT_TRUE(container_alive.empty());
    if (GetParam().route == RevealRoute::FloatingNewDialog) {
        EXPECT_EQ(returned, nullptr);
    }

    // The retained owned host/panel must still be the exact registered host; the
    // pointer-identity check precedes any child access.
    ASSERT_EQ(application.ownedPreferencesWindow(), owned_host);
    ASSERT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);
    EXPECT_TRUE(owned_host->get_visible());
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog("Preferences"), nullptr);
    EXPECT_EQ(ownedFloatingDialogCount(application), native_baseline);

    // The host can still be hidden and presented again through the real
    // application and real InkscapeWindow; the callback guard prevents a repeat
    // reset of the already-destroyed initiator.
    owned_host->hide();
    drainMainContext();
    ASSERT_FALSE(owned_host->get_visible());

    application.presentPreferences(realwin);
    drainMainContext();
    ASSERT_EQ(application.ownedPreferencesWindow(), owned_host);
    EXPECT_EQ(dynamic_cast<InkscapePreferences *>(owned_host->get_child()), owned_panel);
    EXPECT_TRUE(owned_host->get_visible());
    EXPECT_EQ(reset_count, 1u);

    visible_conn.disconnect();
}

INSTANTIATE_TEST_SUITE_P(
    PreferencesDestructionDuringReveal, PreferencesDestructionDuringRevealTest,
    ::testing::Values(
        RevealRouteParam{RevealRoute::SimpleNewDialog, "SimpleNewDialog"},
        RevealRouteParam{RevealRoute::ExplicitNewDialog, "ExplicitNewDialog"},
        RevealRouteParam{RevealRoute::FloatingNewDialog, "FloatingNewDialog"}),
    revealRouteName);

} // namespace
