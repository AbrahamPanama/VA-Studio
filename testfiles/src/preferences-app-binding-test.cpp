// SPDX-License-Identifier: GPL-2.0-or-later
//
// W7 application-owned Preferences binding: focused native-GTK outcome tests.
//
// These tests drive the real InkscapeApplication / PreferencesPresenter pair
// against live Gtk windows. They are modeled on the adjacent WindowLifecycleTest
// fixture in dialog-notebook-test.cpp (process-singleton TestApplication,
// drainMainContext, pumpFrames) rather than a new mock framework, and they skip
// explicitly when INKSCAPE_TEST_GUI is not 1. A skipped run is never reported as
// a pass: testfiles/CMakeLists.txt registers both suites with the
// dialog-notebook-style failure guards.
//
// Suite split for CTest isolation: `PreferencesAppBindingTest` holds the three
// ordinary GUI cases; `PreferencesAppBindingQuitTest` reaches an explicit Quit
// (or the deferred quit continuation) and must run in its own process.

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <glibmm/main.h>
#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <giomm/file.h>
#include <sigc++/connection.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <filesystem>
#include <cstring>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
// Wincon macro collides with Gdk::ToplevelState::DOUBLE_CLICK below.
#undef DOUBLE_CLICK
#endif

#include "desktop.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "inkscape-window.h"
#include "ui/dialog/inkscape-preferences.h"
#include "ui/dialog/startup.h"
#include "preferences.h"

namespace {

class TestApplication : public InkscapeApplication
{
public:
    void setActionsForTest(Glib::ustring const &actions)
    {
        _command_line_actions.clear();
        parse_actions(actions, _command_line_actions);
    }
    bool hasActionsForTest() const { return !_command_line_actions.empty(); }
    void setBatchProcessForTest(bool enabled) { _batch_process = enabled; }
};

void ensureIsolatedWelcomeProcess()
{
    if (auto const *root = g_getenv("WELCOME_APP_CHILD_ROOT")) {
        auto *manager = gtk_recent_manager_get_default();
        gchar *filename = nullptr;
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(manager), "filename")) {
            g_object_get(manager, "filename", &filename, nullptr);
        }
        bool const isolated = filename && g_str_has_prefix(filename, root) &&
            (filename[std::strlen(root)] == '/' || filename[std::strlen(root)] == '\\');
        g_free(filename);
        if (!isolated) {
            std::fprintf(stderr, "GTK RecentManager is outside the isolated Welcome profile\n");
            std::exit(1);
        }
        return;
    }
    std::string executable;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    executable.resize(size);
    if (_NSGetExecutablePath(executable.data(), &size) != 0) std::exit(1);
#elif defined(_WIN32)
    char path[MAX_PATH];
    auto const length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!length || length == MAX_PATH) std::exit(1);
    executable.assign(path, length);
#else
    executable = "/proc/self/exe";
#endif
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("vacards-welcome-app-child-XXXXXX", &error);
    if (!tmp) {
        std::fprintf(stderr, "Cannot isolate Welcome profile: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        std::exit(1);
    }
    std::string const root(tmp);
    g_free(tmp);
    for (auto const *subdir : {"data", "config", "profile"}) {
        std::filesystem::create_directories(root + "/" + subdir);
    }
    auto *env = g_get_environ();
    env = g_environ_setenv(env, "WELCOME_APP_CHILD_ROOT", root.c_str(), true);
    env = g_environ_setenv(env, "XDG_DATA_HOME", (root + "/data").c_str(), true);
    env = g_environ_setenv(env, "XDG_CONFIG_HOME", (root + "/config").c_str(), true);
    env = g_environ_setenv(env, "INKSCAPE_PROFILE_DIR", (root + "/profile").c_str(), true);
    std::string filter = std::string("--gtest_filter=") + GTEST_FLAG_GET(filter);
    gchar *args[] = {executable.data(), filter.data(), nullptr};
    gchar *output = nullptr, *errors = nullptr;
    int status = -1;
    bool const spawned = g_spawn_sync(nullptr, args, env, G_SPAWN_DEFAULT, nullptr, nullptr,
                                      &output, &errors, &status, &error);
    bool const passed = spawned && g_spawn_check_wait_status(status, &error);
    if (output) std::fputs(output, stdout);
    if (errors) std::fputs(errors, stderr);
    if (!passed) std::fprintf(stderr, "Isolated Welcome child failed: %s\n", error ? error->message : "unknown");
    g_free(output);
    g_free(errors);
    if (error) g_error_free(error);
    g_strfreev(env);
    std::filesystem::remove_all(root);
    std::exit(passed ? 0 : 1);
}

TestApplication &testApplication()
{
    static auto application = [] {
        ensureIsolatedWelcomeProcess();
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "preferencesappbindingtest", true);
        auto *result = new TestApplication();
        result->gio_app()->register_application(); // Emits startup, so app.preferences exists.
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

bool guiTestsEnabled()
{
    auto const *gui = std::getenv("INKSCAPE_TEST_GUI");
    return gui && std::string(gui) == "1";
}

void drainMainContext()
{
    for (unsigned i = 0; i < 10000 && g_main_context_pending(nullptr); ++i) {
        g_main_context_iteration(nullptr, false);
    }
}

void pumpFrames()
{
    auto loop = g_main_loop_new(nullptr, false);
    g_timeout_add(100, [](gpointer data) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop *>(data));
        return G_SOURCE_REMOVE;
    }, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

GtkWidget *findWidget(GtkWidget *root, bool (*matches)(GtkWidget *))
{
    if (matches(root)) return root;
    for (auto *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        if (auto *found = findWidget(child, matches)) return found;
    }
    return nullptr;
}

GtkWidget *welcomeBrowseButton(Gtk::Window &welcome)
{
    return findWidget(GTK_WIDGET(welcome.gobj()), [](GtkWidget *candidate) {
        return GTK_IS_BUTTON(candidate) &&
            g_strcmp0(gtk_widget_get_tooltip_text(candidate), "Open an existing document") == 0;
    });
}

std::unique_ptr<SPDocument> makeDocument()
{
    return SPDocument::createNewDocFromMem(
        std::string_view{"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"/>"});
}

Inkscape::UI::Dialog::InkscapePreferences *ownedPanel()
{
    auto &app = testApplication();
    auto *host = app.ownedPreferencesWindow();
    if (!host) {
        return nullptr;
    }
    return dynamic_cast<Inkscape::UI::Dialog::InkscapePreferences *>(host->get_child());
}

// Bounded native-dialog responder. This drives the actual GtkMessageDialog
// created by document_check_for_data_loss (title "Save Document"); it is not a
// test switch and does not bypass consent. It polls on a 20 ms timeout rather
// than an always-ready idle, which could spin throughout the nested modal loop,
// and gives up after a fixed budget instead of running forever. The connection
// is scoped to this object, so a fatal ASSERT unwinding out of a test body
// cannot leave a callback holding dangling stack state.
class DataLossCancelResponder
{
public:
    explicit DataLossCancelResponder(bool &cancelled)
        : _cancelled(cancelled)
        , _source(Glib::signal_timeout().connect([this] { return poll(); }, 20))
    {
    }

    bool answered() const { return _answered; }
    bool timedOut() const { return _timed_out; }
    // Stop watching so a later, unrelated prompt cannot be answered by mistake.
    void stop() { _source.disconnect(); }

private:
    bool poll()
    {
        auto list = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(list); ++i) {
            auto *object = g_list_model_get_item(list, i);
            bool const found = GTK_IS_MESSAGE_DIALOG(object) &&
                g_strcmp0(gtk_window_get_title(GTK_WINDOW(object)), "Save Document") == 0;
            if (found) {
                _cancelled = true;
                _answered = true;
                gtk_dialog_response(GTK_DIALOG(object), GTK_RESPONSE_CANCEL);
            }
            g_object_unref(object);
            if (found) {
                return false;
            }
        }
        // 500 ticks x 20 ms is the same 10 s absolute budget as the outer loop.
        if (++_ticks > 500) {
            _timed_out = true;
            return false;
        }
        return true;
    }

    bool &_cancelled;
    bool _answered = false;
    bool _timed_out = false;
    unsigned _ticks = 0;
    // Declared last so it disconnects before the members the callback reads.
    sigc::scoped_connection _source;
};

// Each ordinary case owns the documents/desktops it creates and leaves the
// process-singleton application document-free, so no test depends on execution
// order. The retained owned Preferences host intentionally survives the fixture.
class PreferencesAppBindingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!guiTestsEnabled()) {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        testApplication();
    }

    void TearDown() override
    {
        // GTEST_SKIP() in SetUp still runs TearDown; never construct or drive the
        // GUI application when the suite was skipped.
        if (!guiTestsEnabled()) {
            return;
        }
        auto &app = testApplication();
        for (auto *document : app.get_documents()) {
            document->setModifiedSinceSave(false);
        }
        app.destroy_all(); // No prompts: modified flags are cleared above.
        drainMainContext();
    }
};

// Zero documents (the welcome/start-screen path): app.preferences must already
// exist at GUI startup, activation must yield a visible owned native host, and a
// second activation must reuse the exact same host. No initial-null assumption:
// the retained host may already exist from an earlier case.
TEST_F(PreferencesAppBindingTest, ZeroDocumentActionCreatesAndReusesOwnedHost)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());

    auto action = app.gio_app()->lookup_action("preferences");
    ASSERT_TRUE(action) << "app.preferences must be registered before any window";

    app.gio_app()->activate_action("preferences");
    drainMainContext();
    pumpFrames();

    auto *host = app.ownedPreferencesWindow();
    ASSERT_TRUE(host) << "cold-start Preferences must yield an application-owned host";
    EXPECT_TRUE(host->get_visible());
    auto *panel = ownedPanel();
    ASSERT_TRUE(panel) << "owned host must parent the real native Preferences panel";

    app.gio_app()->activate_action("preferences");
    drainMainContext();
    pumpFrames();
    EXPECT_EQ(app.ownedPreferencesWindow(), host) << "re-present must reuse the owned host";
}

TEST_F(PreferencesAppBindingTest, AppNewClosesWelcomeWithExactlyOneDesktop)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());
    auto *welcome = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    app.gtk_app()->add_window(*welcome);
    welcome->present();
    drainMainContext();

    auto const before = INKSCAPE.get_desktops().size();
    app.on_new();
    drainMainContext();

    EXPECT_EQ(INKSCAPE.get_desktops().size(), before + 1);
    for (auto *window : app.gtk_app()->get_windows()) {
        EXPECT_EQ(dynamic_cast<Inkscape::UI::Dialog::StartScreen *>(window), nullptr);
    }
}

TEST_F(PreferencesAppBindingTest, ExternalOpenWhileWelcomeChooserPending)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());
    auto *welcome = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    app.gtk_app()->add_window(*welcome);
    welcome->present();
    unsigned chooser_completions = 0;
    welcome->set_chooser_completed_for_test([&] { ++chooser_completions; });
    auto *browse = welcomeBrowseButton(*welcome);
    ASSERT_NE(browse, nullptr);
    g_signal_emit_by_name(browse, "clicked"); // Asynchronous; no nested loop.

    gchar *path = nullptr;
    int fd = g_file_open_tmp("welcome-wp2-pending-XXXXXX.svg", &path, nullptr);
    ASSERT_GE(fd, 0);
    g_close(fd, nullptr);
    ASSERT_TRUE(g_file_set_contents(path,
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'/>", -1, nullptr));
    auto const before = INKSCAPE.get_desktops().size();
    app.gio_app()->open({Gio::File::create_for_path(path)}, "");
    drainMainContext();
    for (int i = 0; i < 50 && chooser_completions == 0; ++i) pumpFrames();
    EXPECT_EQ(chooser_completions, 1u);
    EXPECT_EQ(INKSCAPE.get_desktops().size(), before + 1);
    for (auto *window : app.gtk_app()->get_windows()) {
        EXPECT_EQ(dynamic_cast<Inkscape::UI::Dialog::StartScreen *>(window), nullptr);
    }
    g_unlink(path);
    g_free(path);
}

TEST_F(PreferencesAppBindingTest, ChooserCancelKeepsWelcome)
{
    auto &app = testApplication();
    auto *welcome = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    app.gtk_app()->add_window(*welcome);
    welcome->present();
    auto *browse = welcomeBrowseButton(*welcome);
    ASSERT_NE(browse, nullptr);
    unsigned completions = 0;
    welcome->set_chooser_completed_for_test([&] { ++completions; });
    g_signal_emit_by_name(browse, "clicked");
    ASSERT_TRUE(welcome->chooser_pending_for_test());
    welcome->cancel_chooser_for_test();
    for (int i = 0; i < 50 && welcome->chooser_pending_for_test(); ++i) pumpFrames();
    ASSERT_FALSE(welcome->chooser_pending_for_test());
    EXPECT_EQ(completions, 1u);
    EXPECT_TRUE(welcome->get_visible());
    EXPECT_TRUE(app.get_documents().empty());
    g_signal_emit_by_name(browse, "clicked");
    EXPECT_TRUE(welcome->chooser_pending_for_test());
    welcome->close();
    for (int i = 0; i < 50 && completions < 2; ++i) pumpFrames();
    EXPECT_EQ(completions, 2u);
}

TEST_F(PreferencesAppBindingTest, ChooserErrorShowsOneDialog)
{
    auto &app = testApplication();
    auto *welcome = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    app.gtk_app()->add_window(*welcome);
    welcome->present();
    unsigned shown = 0;
    sigc::scoped_connection responder = Glib::signal_timeout().connect([&] {
        auto list = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(list); ++i) {
            auto *object = g_list_model_get_item(list, i);
            if (GTK_IS_MESSAGE_DIALOG(object) && gtk_widget_get_visible(GTK_WIDGET(object))) {
                ++shown;
                gtk_dialog_response(GTK_DIALOG(object), GTK_RESPONSE_CLOSE);
            }
            g_object_unref(object);
        }
        return shown == 0;
    }, 20);
    auto completion = welcome->make_open_completion();
    completion({}, {}, "Welcome chooser injected failure");
    responder.disconnect();
    EXPECT_EQ(shown, 1u);
    EXPECT_TRUE(welcome->get_visible());
    EXPECT_TRUE(app.get_documents().empty());
    welcome->close();
}

TEST_F(PreferencesAppBindingTest, LateChooserCompletionAfterExternalOpen)
{
    auto &app = testApplication();
    auto *welcome = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    app.gtk_app()->add_window(*welcome);
    auto completion = welcome->make_open_completion();

    gchar *path = nullptr;
    int fd = g_file_open_tmp("welcome-wp2-late-XXXXXX.svg", &path, nullptr);
    ASSERT_GE(fd, 0);
    g_close(fd, nullptr);
    ASSERT_TRUE(g_file_set_contents(path,
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'/>", -1, nullptr));
    auto file = Gio::File::create_for_path(path);
    app.gio_app()->open({file}, ""); // Closes and destroys Welcome.
    auto *first = app.get_active_document();
    ASSERT_NE(first, nullptr);
    completion(file, {}, {}); // Inject GTK's delayed selected-file result.
    EXPECT_NE(app.get_active_document(), first);
    for (auto *window : app.gtk_app()->get_windows()) {
        EXPECT_EQ(dynamic_cast<Inkscape::UI::Dialog::StartScreen *>(window), nullptr);
    }
    g_unlink(path);
    g_free(path);
}

TEST_F(PreferencesAppBindingTest, TemplateCompletionAfterWelcomeDestroyed)
{
    auto &app = testApplication();
    auto *welcome = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    app.gtk_app()->add_window(*welcome);
    auto completion = welcome->make_template_completion();
    app.on_new(); // This closes Welcome while template settings could be open.
    auto const before = INKSCAPE.get_desktops().size();
    auto *template_document = app.document_new();
    ASSERT_NE(template_document, nullptr);
    completion(template_document); // Inject the nested settings dialog's return.
    EXPECT_EQ(INKSCAPE.get_desktops().size(), before + 1);
    EXPECT_EQ(app.get_active_document(), template_document);
}

TEST_F(PreferencesAppBindingTest, TemplateCompletionAfterLastWelcomeCloses)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());
    if (auto *host = app.ownedPreferencesWindow()) host->close();
    auto *welcome = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    app.gtk_app()->add_window(*welcome);
    auto completion = welcome->make_template_completion();
    welcome->close();
    drainMainContext();
    ASSERT_TRUE(app.gtk_app()->get_windows().empty());
    auto *document = app.document_new();
    ASSERT_NE(document, nullptr);
    auto const before = INKSCAPE.get_desktops().size();
    completion(document);
    EXPECT_EQ(INKSCAPE.get_desktops().size(), before + 1);
    EXPECT_EQ(app.get_active_document(), document);
}

TEST_F(PreferencesAppBindingTest, WelcomeCheckboxObservesBootMode)
{
    auto &app = testApplication();
    auto *prefs = Inkscape::Preferences::get();
    auto const old_mode = prefs->getInt("/options/boot/mode", 1);
    auto *welcome = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    app.gtk_app()->add_window(*welcome);
    app.presentPreferences(welcome);
    ASSERT_NE(app.ownedPreferencesWindow(), nullptr);
    auto *widget = findWidget(GTK_WIDGET(welcome->gobj()), [](GtkWidget *candidate) {
        return GTK_IS_CHECK_BUTTON(candidate) != 0;
    });
    ASSERT_NE(widget, nullptr);
    unsigned notifications = 0;
    auto observer = prefs->createObserver("/options/boot/mode", [&] { ++notifications; });
    prefs->setInt("/options/boot/mode", 0);
    EXPECT_FALSE(gtk_check_button_get_active(GTK_CHECK_BUTTON(widget)));
    EXPECT_EQ(notifications, 1u);
    prefs->setInt("/options/boot/mode", 1);
    EXPECT_TRUE(gtk_check_button_get_active(GTK_CHECK_BUTTON(widget)));
    EXPECT_EQ(notifications, 2u);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widget), false);
    EXPECT_EQ(notifications, 3u);
    welcome->close();
    app.ownedPreferencesWindow()->close();
    prefs->setInt("/options/boot/mode", old_mode);
}

// The app no longer holds itself alive after Welcome closes (a real Dock reopen
// never reaches activate on GTK/macOS). An explicit activation still shows Welcome.
TEST_F(PreferencesAppBindingTest, ActivationAfterClosingWelcomeShowsWelcomeAgain)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());
    app.gio_app()->activate();
    drainMainContext();
    auto *welcome = dynamic_cast<Inkscape::UI::Dialog::StartScreen *>(
        app.gtk_app()->get_active_window());
    ASSERT_NE(welcome, nullptr);
    welcome->close();
    drainMainContext();
    EXPECT_TRUE(app.gtk_app()->get_windows().empty());
    app.gio_app()->activate();
    drainMainContext();
    EXPECT_NE(dynamic_cast<Inkscape::UI::Dialog::StartScreen *>(
        app.gtk_app()->get_active_window()), nullptr);
    EXPECT_TRUE(app.get_documents().empty());
}

TEST_F(PreferencesAppBindingTest, PreferencesOnlyActivationReopensWelcome)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());
    auto *prefs = Inkscape::Preferences::get();
    auto const old_mode = prefs->getInt("/options/boot/mode", 1);
    prefs->setInt("/options/boot/mode", 1);
    app.gio_app()->activate();
    drainMainContext();
    auto *welcome = dynamic_cast<Inkscape::UI::Dialog::StartScreen *>(
        app.gtk_app()->get_active_window());
    ASSERT_NE(welcome, nullptr);
    app.presentPreferences(welcome);
    ASSERT_NE(app.ownedPreferencesWindow(), nullptr);
    prefs->setInt("/options/boot/mode", 0);
    welcome->close();
    drainMainContext();
    app.gio_app()->activate();
    drainMainContext();
    unsigned welcome_count = 0;
    for (auto *window : app.gtk_app()->get_windows()) {
        if (dynamic_cast<Inkscape::UI::Dialog::StartScreen *>(window)) ++welcome_count;
    }
    EXPECT_EQ(welcome_count, 1u);
    EXPECT_TRUE(app.get_documents().empty());
    prefs->setInt("/options/boot/mode", old_mode);
}

TEST_F(PreferencesAppBindingTest, CommandLineActionsWithoutFileSkipWelcome)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());
    unsigned action_runs = 0;
    app.gio_app()->add_action("welcome-test-action", [&] { ++action_runs; });
    app.setActionsForTest("welcome-test-action");
    ASSERT_TRUE(app.hasActionsForTest());
    app.gio_app()->activate();
    drainMainContext();
    EXPECT_FALSE(app.get_documents().empty());
    EXPECT_EQ(action_runs, 1u);
    for (auto *window : app.gtk_app()->get_windows()) {
        EXPECT_EQ(dynamic_cast<Inkscape::UI::Dialog::StartScreen *>(window), nullptr);
    }
    app.setActionsForTest("");
    app.gio_app()->remove_action("welcome-test-action");
}

// Closing the last ordinary document must not close the retained owned host,
// and the owned panel must be unbound from the destroyed desktop.
TEST_F(PreferencesAppBindingTest, ClosingLastDocumentRetainsOwnedHostAndClearsDesktop)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());
    app.presentPreferences(nullptr);
    drainMainContext();
    pumpFrames();

    auto *host = app.ownedPreferencesWindow();
    ASSERT_TRUE(host);
    auto *panel = ownedPanel();
    ASSERT_TRUE(panel);

    auto document = makeDocument();
    ASSERT_TRUE(document);
    auto *document_ptr = app.document_add(std::move(document));
    ASSERT_TRUE(document_ptr);
    auto *desktop = app.createDesktop(document_ptr, false, true);
    ASSERT_TRUE(desktop);

    app.set_active_desktop(desktop);
    drainMainContext();
    ASSERT_EQ(panel->getDesktop(), desktop);

    document_ptr->setModifiedSinceSave(false);
    ASSERT_TRUE(app.destroyDesktop(desktop));
    drainMainContext();
    pumpFrames();

    EXPECT_TRUE(app.get_documents().empty());
    EXPECT_EQ(app.ownedPreferencesWindow(), host) << "owned host must survive the last document close";
    EXPECT_EQ(panel->getDesktop(), nullptr) << "closing the active desktop must clear the owned binding";
    EXPECT_TRUE(gtk_widget_get_visible(GTK_WIDGET(host->gobj()))) << "retained host must stay visible";
}

// The owned panel follows the application's active desktop across a real A/B
// switch (the current-context update delivered through set_active_desktop).
TEST_F(PreferencesAppBindingTest, ActiveDesktopSwitchRebindsOwnedPanel)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());
    app.presentPreferences(nullptr);
    drainMainContext();
    pumpFrames();

    auto *panel = ownedPanel();
    ASSERT_TRUE(panel);

    auto document = makeDocument();
    ASSERT_TRUE(document);
    auto *document_ptr = app.document_add(std::move(document));
    ASSERT_TRUE(document_ptr);
    auto *desktop_a = app.createDesktop(document_ptr, false, true);
    auto *desktop_b = app.createDesktop(document_ptr, false, true);
    ASSERT_TRUE(desktop_a && desktop_b);
    ASSERT_NE(desktop_a, desktop_b);

    // Read-only diagnostic: capture live binding/window state at each switch
    // step. Query calls only; no present/dispatch/focus/state mutation, no
    // signals. Neutral observations; no assumption that desktop B is the cause.
    auto record = [&](char const *label, char const *file, int line) {
        auto *active = app.get_active_desktop();
        auto *bound = panel->getDesktop();
        auto *win_a = desktop_a->getInkscapeWindow();
        auto *win_b = desktop_b->getInkscapeWindow();
        GtkWindow *gwin_a = win_a ? GTK_WINDOW(win_a->gobj()) : nullptr;
        GtkWindow *gwin_b = win_b ? GTK_WINDOW(win_b->gobj()) : nullptr;
        std::fprintf(stderr,
                     "[prefs-bind] %s %s:%d t=%" G_GINT64_FORMAT
                     " active=%p panel=%p A=%p B=%p winA=%p winB=%p"
                     " winA[active=%d mapped=%d focus=%p]"
                     " winB[active=%d mapped=%d focus=%p]\n",
                     label, file, line, g_get_monotonic_time(),
                     static_cast<void *>(active), static_cast<void *>(bound),
                     static_cast<void *>(desktop_a), static_cast<void *>(desktop_b),
                     static_cast<void *>(gwin_a), static_cast<void *>(gwin_b),
                     gwin_a ? gtk_window_is_active(gwin_a) : 0,
                     gwin_a ? gtk_widget_get_mapped(GTK_WIDGET(gwin_a)) : 0,
                     static_cast<void *>(gwin_a ? gtk_window_get_focus(gwin_a) : nullptr),
                     gwin_b ? gtk_window_is_active(gwin_b) : 0,
                     gwin_b ? gtk_widget_get_mapped(GTK_WIDGET(gwin_b)) : 0,
                     static_cast<void *>(gwin_b ? gtk_window_get_focus(gwin_b) : nullptr));
    };

    record("after-two-windows-created", __FILE__, __LINE__);

    // Logical oracle, moved ahead of the drain: set_active_desktop must bind
    // the application and the owned panel to the exact requested desktop
    // synchronously, before any queued native activation can be delivered.
    app.set_active_desktop(desktop_a);
    record("immediately-after-set-A", __FILE__, __LINE__);
    EXPECT_EQ(app.get_active_desktop(), desktop_a);
    EXPECT_EQ(panel->getDesktop(), desktop_a);

    // The drain may legitimately deliver desktop B's already-queued native
    // activation (observed), so it is diagnostic only here; the native phase
    // below drives the real switch to A.
    drainMainContext();
    record("post-drain-A", __FILE__, __LINE__);

    auto *window_a = desktop_a->getInkscapeWindow();
    ASSERT_TRUE(window_a) << "desktop A must own a native Inkscape window";
    window_a->present();
    record("after-present-A", __FILE__, __LINE__);

    // One shared fixed monotonic deadline bounds both native waits (10 s
    // total). Each frame pumps the real GTK main loop through the existing
    // pumpFrames(); a timeout is a real failure, not a skip.
    auto const native_deadline = g_get_monotonic_time() + 10000000;
    auto const native_bound = [&](SPDesktop *desired, InkscapeWindow *window) {
        return gtk_window_is_active(GTK_WINDOW(window->gobj())) &&
               app.get_active_desktop() == desired && panel->getDesktop() == desired;
    };
    while (g_get_monotonic_time() < native_deadline && !native_bound(desktop_a, window_a)) {
        pumpFrames();
    }
    record("native-wait-A", __FILE__, __LINE__);
    EXPECT_TRUE(gtk_window_is_active(GTK_WINDOW(window_a->gobj())))
        << "desktop A window must be natively active after present";
    EXPECT_EQ(app.get_active_desktop(), desktop_a) << "native A activation must select desktop A";
    EXPECT_EQ(panel->getDesktop(), desktop_a) << "native A activation must rebind the owned panel";

    app.set_active_desktop(desktop_b);
    record("immediately-after-set-B", __FILE__, __LINE__);
    EXPECT_EQ(app.get_active_desktop(), desktop_b);
    EXPECT_EQ(panel->getDesktop(), desktop_b);

    drainMainContext();
    record("post-drain-B", __FILE__, __LINE__);

    auto *window_b = desktop_b->getInkscapeWindow();
    ASSERT_TRUE(window_b) << "desktop B must own a native Inkscape window";
    window_b->present();
    record("after-present-B", __FILE__, __LINE__);
    while (g_get_monotonic_time() < native_deadline && !native_bound(desktop_b, window_b)) {
        pumpFrames();
    }
    record("native-wait-B", __FILE__, __LINE__);
    EXPECT_TRUE(gtk_window_is_active(GTK_WINDOW(window_b->gobj())))
        << "desktop B window must be natively active after present";
    EXPECT_EQ(app.get_active_desktop(), desktop_b) << "native B activation must select desktop B";
    EXPECT_EQ(panel->getDesktop(), desktop_b) << "native B activation must rebind the owned panel";

    document_ptr->setModifiedSinceSave(false);
    app.destroyDesktop(desktop_a);
    app.destroyDesktop(desktop_b);
    drainMainContext();
    pumpFrames();
}

// Terminal-Quit tests run in their own process. They exercise explicit
// destruction (not merely hide) and the deferred consent continuation.
class PreferencesAppBindingQuitTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!guiTestsEnabled()) {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        testApplication();
    }

    void TearDown() override
    {
        // GTEST_SKIP() in SetUp still runs TearDown; never construct the GUI
        // application when the suite was skipped.
        if (!guiTestsEnabled()) {
            return;
        }
        if (_quit_finalized) {
            return; // gio_app()->quit() was reached; do not pump further sources.
        }
        auto &app = testApplication();
        for (auto *document : app.get_documents()) {
            document->setModifiedSinceSave(false);
        }
        app.destroy_all();
        drainMainContext();
    }

    bool _quit_finalized = false;
};

TEST_F(PreferencesAppBindingQuitTest, BatchActionsWithoutFileRunBeforeQuit)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());
    unsigned action_runs = 0;
    app.gio_app()->add_action("welcome-batch-test-action", [&] { ++action_runs; });
    app.setActionsForTest("welcome-batch-test-action");
    ASSERT_TRUE(app.hasActionsForTest());
    app.setBatchProcessForTest(true);
    app.gio_app()->activate();
    _quit_finalized = true;
    EXPECT_EQ(action_runs, 1u);
    EXPECT_FALSE(app.quitPending());
}

// A Preferences host destroyed during Quit finalization may run callbacks that
// create a fresh dirty document. The next consent pass must show the real
// data-loss dialog; Cancel must keep that document and the application alive,
// and a later Preferences request on the cold zero-document path must build a
// fresh owned host. A nested Preferences request from inside shutdown must be
// suppressed.
TEST_F(PreferencesAppBindingQuitTest, ShutdownObserverDocumentCancelKeepsDocumentAndFreshPreferences)
{
    auto &app = testApplication();
    ASSERT_TRUE(app.get_documents().empty());

    app.presentPreferences(nullptr);
    drainMainContext();
    pumpFrames();
    auto *host = app.ownedPreferencesWindow();
    ASSERT_TRUE(host);
    ASSERT_TRUE(ownedPanel());

    SPDocument *late_document = nullptr;
    SPDesktop *late_desktop = nullptr;
    bool late_document_alive = true;
    bool observer_ran = false;
    bool host_query_observed = false;
    Gtk::Window *host_during_shutdown = nullptr;
    sigc::scoped_connection late_document_destroy;
    sigc::scoped_connection host_destroy = host->signal_destroy().connect([&] {
        observer_ran = true;
        // Nested request with zero documents: only the finishing-quit guard
        // stops a fresh owned host from being created mid-teardown.
        app.presentPreferences(nullptr);
        host_during_shutdown = app.ownedPreferencesWindow();
        // Actual observed lookup: the null result below must not be a false pass
        // from the variable's initial value if the observer never ran.
        host_query_observed = true;

        late_document = app.document_add(SPDocument::createNewDocFromMem(
            std::string_view{"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"/>"}));
        if (late_document) {
            late_desktop = app.createDesktop(late_document, false, true);
            late_document->setModifiedSinceSave(true);
            late_document_destroy = late_document->connectDestroy([&] { late_document_alive = false; });
        }
    });

    bool cancelled = false;
    DataLossCancelResponder respond(cancelled);

    app.on_quit(); // Closes ordinary windows, then destroys the owned host.

    ASSERT_TRUE(observer_ran) << "host destruction must run the shutdown observer";
    ASSERT_TRUE(host_query_observed) << "the observer must record the post-request lookup";
    EXPECT_EQ(host_during_shutdown, nullptr) << "nested Preferences during shutdown must be suppressed";
    EXPECT_EQ(app.ownedPreferencesWindow(), nullptr) << "torn-down host must not be recreated by shutdown";
    EXPECT_TRUE(app.quitPending()) << "the deferred consent pass must still be pending";
    ASSERT_TRUE(late_document);
    ASSERT_TRUE(late_desktop);
    ASSERT_TRUE(late_document_alive);
    EXPECT_TRUE(late_document->isModifiedSinceSave());

    // Drive the scheduled idle continuation: the full consent pass runs and the
    // real data-loss dialog must be answered Cancel, keeping the document.
    auto const deadline = g_get_monotonic_time() + 10000000;
    while (!cancelled && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }

    EXPECT_TRUE(respond.answered()) << "the real data-loss dialog must be present and answered";
    EXPECT_FALSE(respond.timedOut()) << "the bounded responder must answer before its deadline";
    respond.stop();
    ASSERT_TRUE(cancelled) << "the late dirty document must reach the real data-loss dialog";
    ASSERT_FALSE(app.quitPending()) << "Cancel must abort the quit";
    ASSERT_TRUE(late_document_alive) << "Cancel must keep the late document alive";
    ASSERT_TRUE(late_document);
    EXPECT_TRUE(late_document->isModifiedSinceSave()) << "Cancel must not discard modifications";

    // Coverage limit: the late document and desktop are removed (modified flag
    // cleared) BEFORE the next present, so this drives the zero-document
    // cold-start path, where an owned host is the designed outcome. It does not
    // prove that a present while a live document/desktop exists creates a new
    // owned host; that case routes to the live docked container by design.
    late_document->setModifiedSinceSave(false);
    app.destroyDesktop(late_desktop);
    drainMainContext();

    app.presentPreferences(nullptr);
    drainMainContext();
    pumpFrames();
    auto *fresh = app.ownedPreferencesWindow();
    ASSERT_TRUE(fresh) << "the cold zero-document path must build a fresh owned host after the aborted quit";
    EXPECT_TRUE(fresh->get_visible());
    EXPECT_TRUE(ownedPanel()) << "the fresh host must parent a real native panel";
}

// An explicit completed Quit must shut the owned host down by destroying the
// native window, not merely hiding it. Root runs each case in its own CTest
// process, so this case does not rely on declaration order: the process-singleton
// presenter is terminated by design and every case starts from its own process.
// The `_library_quit_guard` reservation path is not covered by this fixture (no
// artwork library host is created); see REVIEW.md coverage limits.
TEST_F(PreferencesAppBindingQuitTest, ExplicitQuitDestroysOwnedHost)
{
    auto &app = testApplication();
    app.presentPreferences(nullptr);
    drainMainContext();
    pumpFrames();
    auto *host = app.ownedPreferencesWindow();
    ASSERT_TRUE(host);
    EXPECT_TRUE(host->get_visible());

    bool destroyed = false;
    sigc::scoped_connection destroy_signal = host->signal_destroy().connect([&] { destroyed = true; });

    app.on_quit_immediate();
    _quit_finalized = true;

    // Destruction is synchronous inside _finishQuitWhenReady; do not pump the
    // context after gio_app()->quit(). `host` is dangling from here on and is
    // never dereferenced again.
    EXPECT_TRUE(destroyed) << "completed Quit must destroy the native host, not merely hide it";
    EXPECT_EQ(app.ownedPreferencesWindow(), nullptr) << "completed Quit must release the owned host";
    EXPECT_FALSE(app.quitPending());
}

TEST_F(PreferencesAppBindingQuitTest, LateChooserCompletionAfterQuitDoesNotOpen)
{
    auto &app = testApplication();
    auto *welcome = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    app.gtk_app()->add_window(*welcome);
    auto completion = welcome->make_open_completion();
    auto const before = INKSCAPE.get_desktops().size();

    gchar *path = nullptr;
    int fd = g_file_open_tmp("vacards-welcome-quit-XXXXXX.svg", &path, nullptr);
    ASSERT_GE(fd, 0);
    g_close(fd, nullptr);
    ASSERT_TRUE(g_file_set_contents(path,
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'/>", -1, nullptr));
    auto file = Gio::File::create_for_path(path);

    app.on_quit(); // Closes Welcome before finalizing the application.
    _quit_finalized = true;
    completion(file, {}, {});
    EXPECT_EQ(INKSCAPE.get_desktops().size(), before);
    EXPECT_TRUE(app.get_documents().empty());
    g_unlink(path);
    g_free(path);
}

} // namespace
