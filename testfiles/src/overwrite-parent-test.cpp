// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Real GUI outcome coverage for the explicit-parent overwrite confirmation
 * interface:
 *
 *   bool sp_ui_overwrite_file(std::string const &filename, Gtk::Window *parent);
 *
 * with the legacy one-argument overload routed through the actual active
 * desktop. The tests exercise the owned native toplevel model and destroy watch
 * against real GtkMessageDialog instances and the native NO/YES response
 * buttons, never against a guessed title (the production helper sets none) and
 * never by selecting the dialog through the expected parent - the actual
 * transient_for is inspected after the dialog has been found.
 *
 * Six enabled (no platform skips) GUI cases:
 *   - an absent destination needs no dialog for an explicit, null or legacy
 *     parent, while an existing target with a null parent refuses without one;
 *   - an explicit parent wins over the actual active desktop window;
 *   - a native Cancel response and a native window close both refuse;
 *   - removing the native parent while the dialog is modal overrides a late
 *     YES emitted after the native destroy returns;
 *   - the legacy one-argument call prompts transient for the actual active
 *     desktop;
 *   - with no active desktop left, an existing target refuses and an absent
 *     target still succeeds.
 *
 * The fixture reuses the process-lifetime registered GUI app, fatal-signal
 * reset, tracked document/desktop destruction, INKSCAPE_TEST_GUI gate and
 * bounded main-context drain from testfiles/src/desktop-origin-lifetime-test.cpp.
 * All files live in an owned g_dir_make_tmp directory and only generated
 * artifacts are removed.
 */

#include <gtest/gtest.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <giomm/listmodel.h>
#include <glibmm/main.h>
#include <sigc++/scoped_connection.h>

#include <csignal>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "desktop.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "ui/interface.h"

namespace {

class TestApplication : public InkscapeApplication {};

InkscapeApplication &testApplication()
{
    static auto application = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "overwriteparenttest", true);
        auto result = new TestApplication();
        result->gio_app()->register_application();
        // Preserve fatal failures as test failures rather than the application's
        // interactive emergency dialog.
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

// One synthetic registered document with a titled, non-empty SVG.
constexpr char kSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
    "<title>Overwrite parent fixture</title>"
    "<rect id='rect' x='10' y='10' width='5' height='5'/>"
    "</svg>";

// Fixed marker bytes; the confirmation helper must never rewrite the target.
constexpr char kMarker[] = "vacards-overwrite-parent-marker-0123456789abcdef";

constexpr guint kWatchdogMs = 5000;

// The production helper deliberately sets no dialog title, so the two native
// response buttons are the stable identity. Selecting by the expected parent
// would hide a wrong-parent finding.
bool isOverwriteDialog(GtkWidget *widget)
{
    if (!widget || !GTK_IS_MESSAGE_DIALOG(widget)) {
        return false;
    }
    auto *const dialog = GTK_DIALOG(widget);
    return gtk_dialog_get_widget_for_response(dialog, GTK_RESPONSE_NO) != nullptr
        && gtk_dialog_get_widget_for_response(dialog, GTK_RESPONSE_YES) != nullptr;
}

// Finds the actual new GtkMessageDialog in the native toplevel model and returns
// it with a reference held by the caller.
GtkWidget *findOverwriteDialog()
{
    GListModel *const model = gtk_window_get_toplevels();
    guint const n = g_list_model_get_n_items(model);
    for (guint i = 0; i < n; ++i) {
        auto *const widget = GTK_WIDGET(g_list_model_get_item(model, i));
        if (isOverwriteDialog(widget)) {
            return widget;
        }
        if (widget) {
            g_object_unref(widget);
        }
    }
    return nullptr;
}

unsigned countMessageDialogs()
{
    unsigned count = 0;
    GListModel *const model = gtk_window_get_toplevels();
    guint const n = g_list_model_get_n_items(model);
    for (guint i = 0; i < n; ++i) {
        auto *const widget = GTK_WIDGET(g_list_model_get_item(model, i));
        if (GTK_IS_MESSAGE_DIALOG(widget)) {
            ++count;
        }
        if (widget) {
            g_object_unref(widget);
        }
    }
    return count;
}

bool toplevelContains(gpointer identity)
{
    GListModel *const model = gtk_window_get_toplevels();
    guint const n = g_list_model_get_n_items(model);
    for (guint i = 0; i < n; ++i) {
        gpointer const item = g_list_model_get_item(model, i);
        bool const match = item == identity;
        if (item) {
            g_object_unref(item);
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// Scoped idle responder in the spirit of
// testfiles/src/artwork-library-controller-test.cpp: it enumerates the native
// toplevels, refcounts the actual dialog, records the observation and then
// executes the caller's action. The action observes actual properties (such as
// transient_for) after the dialog has been found.
sigc::connection observeOverwriteDialog(bool &seen, std::function<void(GtkWidget *)> action)
{
    return Glib::signal_idle().connect(
        [&seen, action = std::move(action)] {
            GtkWidget *const dialog = findOverwriteDialog();
            if (!dialog) {
                return true; // keep waiting until the modal dialog exists
            }
            seen = true;
            action(dialog);
            g_object_unref(dialog);
            return false;
        });
}

// Bounded fail-safe: if the responder never dismissed a relevant dialog, the
// watchdog closes it and records a failure so a cancel case cannot pass merely
// because the watchdog terminated the modal loop.
sigc::connection armDialogWatchdog(bool &timed_out)
{
    return Glib::signal_timeout().connect(
        [&timed_out]() -> bool {
            GtkWidget *const dialog = findOverwriteDialog();
            if (!dialog) {
                return true; // a modal dialog may still be about to appear
            }
            timed_out = true;
            ADD_FAILURE() << "overwrite dialog watchdog dismissed a lingering dialog";
            gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_NO);
            g_object_unref(dialog);
            return false;
        },
        kWatchdogMs);
}

class OverwriteParentTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        // Only a test that passed the gate may touch the application or files.
        started = true;

        auto &application = testApplication();
        ASSERT_TRUE(application.gtk_app());

        doc = application.document_add(SPDocument::createNewDocFromMem(std::string_view{kSvg}));
        ASSERT_TRUE(doc);
        doc_destroy = doc->connectDestroy([this] { doc = nullptr; });

        // One registered document, two real desktops in two separate native
        // windows.
        desktopA = application.createDesktop(doc, false, true);
        ASSERT_TRUE(desktopA);
        desktopA_destroy = desktopA->connectDestroy([this](SPDesktop *) { desktopA = nullptr; });

        desktopB = application.createDesktop(doc, false, true);
        ASSERT_TRUE(desktopB);
        desktopB_destroy = desktopB->connectDestroy([this](SPDesktop *) { desktopB = nullptr; });

        // The two views must be distinct native windows; a shared window would
        // make the parent-identity oracle meaningless.
        auto *const windowA = desktopA->getInkscapeWindow();
        auto *const windowB = desktopB->getInkscapeWindow();
        ASSERT_TRUE(windowA);
        ASSERT_TRUE(windowB);
        ASSERT_NE(windowA, windowB);

        // Owned temporary directory; only generated artifacts are touched, never
        // user paths.
        char *const raw_directory = g_dir_make_tmp("vacards-overwrite-parent-XXXXXX", nullptr);
        ASSERT_TRUE(raw_directory);
        directory = raw_directory;
        g_free(raw_directory);

        existing = directory + "/existing-marker.bin";
        absent = directory + "/absent-marker.bin";
        ASSERT_TRUE(g_file_set_contents(existing.c_str(), kMarker, sizeof(kMarker) - 1, nullptr));
        ASSERT_FALSE(g_file_test(absent.c_str(), G_FILE_TEST_EXISTS));

        // Drain the initial window/desktop setup before making B the actual
        // active desktop, so activation is the last observed state change.
        drainMainContext();
        INKSCAPE.activate_desktop(desktopB);
        ASSERT_EQ(SP_ACTIVE_DESKTOP, desktopB);
    }

    void TearDown() override
    {
        // A skipped SetUp must not initialize the application or pump events.
        if (!started) {
            return;
        }

        if (doc) {
            doc->setModifiedSinceSave(false);
        }

        // Native desktop-close path first; registered documents follow.
        if (desktopB) {
            testApplication().desktopClose(desktopB);
        }
        if (desktopA) {
            testApplication().desktopClose(desktopA);
        }
        drainMainContext();

        if (doc) {
            testApplication().document_close(doc);
        }
        drainMainContext();

        if (!existing.empty()) {
            g_remove(existing.c_str());
        }
        if (!absent.empty()) {
            g_remove(absent.c_str());
        }
        if (!directory.empty()) {
            g_rmdir(directory.c_str());
        }
    }

    void expectMarkerUnchanged() const
    {
        gchar *contents = nullptr;
        gsize length = 0;
        ASSERT_TRUE(g_file_get_contents(existing.c_str(), &contents, &length, nullptr));
        std::string const actual(contents ? contents : "", length);
        g_free(contents);
        EXPECT_EQ(length, sizeof(kMarker) - 1);
        EXPECT_EQ(actual, std::string(kMarker, sizeof(kMarker) - 1));
    }

    SPDocument *doc = nullptr;
    SPDesktop *desktopA = nullptr;
    SPDesktop *desktopB = nullptr;
    std::string directory;
    std::string existing;
    std::string absent;
    bool started = false;
    sigc::scoped_connection doc_destroy;
    sigc::scoped_connection desktopA_destroy;
    sigc::scoped_connection desktopB_destroy;
};

// 1. An absent destination never needs a dialog, whatever the parent, while an
// existing target without an explicit parent refuses and creates none. The
// native toplevel model is watched positively during the calls, not just
// counted at the end.
TEST_F(OverwriteParentTest, AbsentDestinationNeedsNoDialog)
{
    ASSERT_TRUE(doc);
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    auto *const parentA = desktopA->getInkscapeWindow();
    ASSERT_TRUE(parentA);
    ASSERT_EQ(SP_ACTIVE_DESKTOP, desktopB);

    Glib::RefPtr<Gio::ListModel> const toplevels = Gtk::Window::get_toplevels();
    ASSERT_TRUE(toplevels);
    unsigned dialog_additions = 0;
    sigc::scoped_connection watch = toplevels->signal_items_changed().connect(
        [&dialog_additions](guint, guint, guint added) {
            if (added > 0 && countMessageDialogs() > 0) {
                ++dialog_additions;
            }
        });
    bool timed_out = false;
    sigc::scoped_connection watchdog = armDialogWatchdog(timed_out);

    EXPECT_EQ(countMessageDialogs(), 0u);
    EXPECT_TRUE(sp_ui_overwrite_file(absent, parentA));
    EXPECT_TRUE(sp_ui_overwrite_file(absent, nullptr));
    EXPECT_TRUE(sp_ui_overwrite_file(absent));
    drainMainContext();
    EXPECT_EQ(countMessageDialogs(), 0u);
    EXPECT_EQ(dialog_additions, 0u);

    EXPECT_FALSE(sp_ui_overwrite_file(existing, nullptr));
    drainMainContext();
    EXPECT_EQ(countMessageDialogs(), 0u);
    EXPECT_EQ(dialog_additions, 0u);
    watch.disconnect();
    watchdog.disconnect();

    EXPECT_FALSE(timed_out);
    expectMarkerUnchanged();
    EXPECT_EQ(SP_ACTIVE_DESKTOP, desktopB);
}

// 2. The explicit parent A owns the dialog even though B is the actual active
// desktop; the actual transient parent is inspected, not assumed.
TEST_F(OverwriteParentTest, ExplicitParentWinsOverActiveWindow)
{
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    auto *const parentA = desktopA->getInkscapeWindow();
    ASSERT_TRUE(parentA);
    ASSERT_EQ(SP_ACTIVE_DESKTOP, desktopB);

    bool seen = false;
    bool timed_out = false;
    GtkWindow *observed_transient = nullptr;
    sigc::scoped_connection responder = observeOverwriteDialog(seen, [&](GtkWidget *dialog) {
        observed_transient = gtk_window_get_transient_for(GTK_WINDOW(dialog));
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
    });
    sigc::scoped_connection watchdog = armDialogWatchdog(timed_out);

    EXPECT_TRUE(sp_ui_overwrite_file(existing, parentA));

    responder.disconnect();
    watchdog.disconnect();

    EXPECT_TRUE(seen) << "overwrite dialog not observed";
    EXPECT_FALSE(timed_out);
    EXPECT_EQ(observed_transient, GTK_WINDOW(parentA->gobj()));
    expectMarkerUnchanged();
}

// 3. The native Cancel button and a native window close both refuse, with the
// dialog positively observed for each.
TEST_F(OverwriteParentTest, CancelAndCloseRefuse)
{
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    auto *const parentA = desktopA->getInkscapeWindow();
    ASSERT_TRUE(parentA);
    ASSERT_EQ(SP_ACTIVE_DESKTOP, desktopB);

    {
        bool seen = false;
        bool timed_out = false;
        sigc::scoped_connection responder = observeOverwriteDialog(seen, [](GtkWidget *dialog) {
            gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_NO);
        });
        sigc::scoped_connection watchdog = armDialogWatchdog(timed_out);

        EXPECT_FALSE(sp_ui_overwrite_file(existing, parentA));

        responder.disconnect();
        watchdog.disconnect();
        EXPECT_TRUE(seen) << "Cancel dialog not observed";
        EXPECT_FALSE(timed_out);
    }
    expectMarkerUnchanged();

    {
        bool seen = false;
        bool timed_out = false;
        sigc::scoped_connection responder = observeOverwriteDialog(seen, [](GtkWidget *dialog) {
            gtk_window_close(GTK_WINDOW(dialog));
        });
        sigc::scoped_connection watchdog = armDialogWatchdog(timed_out);

        EXPECT_FALSE(sp_ui_overwrite_file(existing, parentA));

        responder.disconnect();
        watchdog.disconnect();
        EXPECT_TRUE(seen) << "close dialog not observed";
        EXPECT_FALSE(timed_out);
    }
    expectMarkerUnchanged();
}

// 4. An ordinary retained toplevel with no GtkApplication association is the
// parent. Destroying that native parent while the dialog is modal is observed
// through the native toplevel model, and a late YES emitted on the still-owned
// dialog after the destroy returns must still refuse. The C++ wrapper stays in
// scope and is never dereferenced after the native destroy.
TEST_F(OverwriteParentTest, NativeParentRemovalOverridesLateYes)
{
    ASSERT_TRUE(doc);
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    ASSERT_EQ(SP_ACTIVE_DESKTOP, desktopB);

    Gtk::Window parent;
    parent.present();
    drainMainContext();

    // Presented but deliberately not associated with any GtkApplication: a
    // production shortcut that watched application windows would miss it.
    ASSERT_EQ(gtk_window_get_application(GTK_WINDOW(parent.gobj())), nullptr);
    GtkWindow *const parent_window = GTK_WINDOW(parent.gobj());
    gpointer const parent_native = parent_window;
    ASSERT_TRUE(toplevelContains(parent_native));

    unsigned parent_removals = 0;
    bool parent_removed = false;
    Glib::RefPtr<Gio::ListModel> const toplevels = Gtk::Window::get_toplevels();
    ASSERT_TRUE(toplevels);
    sigc::scoped_connection model_watch = toplevels->signal_items_changed().connect(
        [&](guint, guint removed, guint) {
            if (removed == 0) {
                return;
            }
            if (!toplevelContains(parent_native)) {
                ++parent_removals;
                parent_removed = true;
            }
        });

    bool seen = false;
    bool timed_out = false;
    GtkWindow *observed_transient = nullptr;
    sigc::scoped_connection responder = observeOverwriteDialog(seen, [&](GtkWidget *dialog) {
        observed_transient = gtk_window_get_transient_for(GTK_WINDOW(dialog));
        // Native removal of the parent while the modal dialog is observed.
        gtk_window_destroy(parent_window);
        // Late YES on the still-owned dialog after the native destroy returned.
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
    });
    sigc::scoped_connection watchdog = armDialogWatchdog(timed_out);

    EXPECT_FALSE(sp_ui_overwrite_file(existing, &parent));

    responder.disconnect();
    watchdog.disconnect();
    model_watch.disconnect();

    EXPECT_TRUE(seen) << "overwrite dialog not observed";
    EXPECT_FALSE(timed_out);
    EXPECT_TRUE(parent_removed) << "native parent removal not observed";
    EXPECT_GE(parent_removals, 1u);
    // Identity was captured before the native destroy; only membership was
    // observed, the parent wrapper is never dereferenced afterwards.
    EXPECT_EQ(static_cast<gpointer>(observed_transient), parent_native);
    expectMarkerUnchanged();
    // `parent` remains in scope here and is intentionally not reset.

    // Second independent subcase: the parent is destroyed from the native
    // toplevel-model callback that fires when the dialog itself is removed,
    // after the responder has already emitted YES. The YES must not be latched
    // before the dialog's destruction completes, so the helper must refuse.
    Gtk::Window cleanup_parent;
    cleanup_parent.present();
    drainMainContext();

    ASSERT_EQ(gtk_window_get_application(GTK_WINDOW(cleanup_parent.gobj())), nullptr);
    GtkWindow *const cleanup_native = GTK_WINDOW(cleanup_parent.gobj());
    ASSERT_TRUE(toplevelContains(static_cast<gpointer>(cleanup_native)));

    bool cleanup_seen = false;
    bool cleanup_timed_out = false;
    bool dialog_removal_trigger = false;
    bool cleanup_parent_removed = false;
    unsigned dialog_removal_count = 0;
    gpointer cleanup_dialog_native = nullptr;
    gpointer cleanup_transient = nullptr;

    // Installed before the helper so removal of the recorded dialog identity can
    // drive parent teardown.
    sigc::scoped_connection cleanup_model_watch = toplevels->signal_items_changed().connect(
        [&](guint, guint removed, guint) {
            if (removed == 0 || !cleanup_dialog_native) {
                return;
            }
            if (toplevelContains(cleanup_dialog_native)) {
                return;
            }
            ++dialog_removal_count;
            // The one-shot flag is set before the parent is destroyed so the
            // parent's own removal event cannot re-enter the destroy below.
            if (dialog_removal_trigger) {
                return;
            }
            dialog_removal_trigger = true;
            if (toplevelContains(static_cast<gpointer>(cleanup_native))) {
                gtk_window_destroy(cleanup_native);
                cleanup_parent_removed = true;
            }
        });

    sigc::scoped_connection cleanup_responder = observeOverwriteDialog(cleanup_seen, [&](GtkWidget *dialog) {
        cleanup_dialog_native = dialog;
        cleanup_transient = gtk_window_get_transient_for(GTK_WINDOW(dialog));
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
    });
    sigc::scoped_connection cleanup_watchdog = armDialogWatchdog(cleanup_timed_out);

    EXPECT_FALSE(sp_ui_overwrite_file(existing, &cleanup_parent));

    cleanup_responder.disconnect();
    cleanup_watchdog.disconnect();
    cleanup_model_watch.disconnect();
    drainMainContext();

    EXPECT_TRUE(cleanup_seen) << "cleanup dialog not observed";
    EXPECT_FALSE(cleanup_timed_out);
    EXPECT_TRUE(dialog_removal_trigger) << "dialog removal did not trigger parent teardown";
    EXPECT_GE(dialog_removal_count, 1u);
    EXPECT_TRUE(cleanup_parent_removed);
    EXPECT_FALSE(toplevelContains(static_cast<gpointer>(cleanup_native)));
    EXPECT_EQ(cleanup_transient, static_cast<gpointer>(cleanup_native));
    expectMarkerUnchanged();
    // `cleanup_parent` remains in scope and is intentionally not reset or
    // dereferenced after its native window was removed.
}

// 5. The legacy one-argument call prompts transient for the actual active
// desktop B.
TEST_F(OverwriteParentTest, LegacyUsesActiveParent)
{
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    auto *const active_window = desktopB->getInkscapeWindow();
    ASSERT_TRUE(active_window);
    ASSERT_EQ(SP_ACTIVE_DESKTOP, desktopB);

    bool seen = false;
    bool timed_out = false;
    GtkWindow *observed_transient = nullptr;
    sigc::scoped_connection responder = observeOverwriteDialog(seen, [&](GtkWidget *dialog) {
        observed_transient = gtk_window_get_transient_for(GTK_WINDOW(dialog));
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
    });
    sigc::scoped_connection watchdog = armDialogWatchdog(timed_out);

    EXPECT_TRUE(sp_ui_overwrite_file(existing));

    responder.disconnect();
    watchdog.disconnect();

    EXPECT_TRUE(seen) << "overwrite dialog not observed";
    EXPECT_FALSE(timed_out);
    EXPECT_EQ(observed_transient, GTK_WINDOW(active_window->gobj()));
    expectMarkerUnchanged();
}

// 6. With both real views natively closed there is no active desktop, so an
// existing target refuses while an absent target still succeeds. The registered
// document survives and remains closable.
TEST_F(OverwriteParentTest, MissingActiveDesktopRefusesExisting)
{
    ASSERT_TRUE(doc);
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);

    testApplication().desktopClose(desktopA);
    testApplication().desktopClose(desktopB);
    drainMainContext();

    EXPECT_EQ(desktopA, nullptr);
    EXPECT_EQ(desktopB, nullptr);
    ASSERT_EQ(SP_ACTIVE_DESKTOP, nullptr);
    ASSERT_NE(doc, nullptr);

    Glib::RefPtr<Gio::ListModel> const toplevels = Gtk::Window::get_toplevels();
    ASSERT_TRUE(toplevels);
    unsigned dialog_additions = 0;
    sigc::scoped_connection watch = toplevels->signal_items_changed().connect(
        [&dialog_additions](guint, guint, guint added) {
            if (added > 0 && countMessageDialogs() > 0) {
                ++dialog_additions;
            }
        });
    bool timed_out = false;
    sigc::scoped_connection watchdog = armDialogWatchdog(timed_out);

    EXPECT_FALSE(sp_ui_overwrite_file(existing));
    EXPECT_TRUE(sp_ui_overwrite_file(absent));
    EXPECT_FALSE(sp_ui_overwrite_file(existing, nullptr));
    drainMainContext();

    EXPECT_EQ(countMessageDialogs(), 0u);
    EXPECT_EQ(dialog_additions, 0u);
    watch.disconnect();
    watchdog.disconnect();
    EXPECT_FALSE(timed_out);
    expectMarkerUnchanged();
}

} // namespace
