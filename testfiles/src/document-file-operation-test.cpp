// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Outcome-based tests for the F6a Packet A file-operation admission guard,
 * weak context, owned output gate and terminal lifetime
 * (src/io/document-file-operation.{h,cpp}).
 *
 * The oracle is independent of the component: a real registered SPDocument,
 * raw const XML traversal (never the serializer), a fingerprint of the whole
 * representative node tree, and explicit Undo/Redo width checks. The fixture
 * follows artwork-library-insert-test.cpp: `document()` settles native state,
 * `RegisteredDocument` mirrors `AtomicRegisteredDocument` with real app
 * registration, and close is dispatched through a bounded main-context loop.
 * Nothing here opens a window. Root owns CMake registration; no build/run here.
 */

#include "io/document-file-operation.h"
#include "io/document-file-transaction.h"
#define VACARDS_FILE_IO_TEST_HOOKS
#include "io/publication-worker.h"
#undef VACARDS_FILE_IO_TEST_HOOKS

#include "config.h" // WITH_VACARDS_NESTING
#include "document-undo.h"
#include "selection.h"
#include "auto-save.h"
#include "actions/actions-file-window.h"
#include "document.h"
#include "desktop.h"
#include "event-log.h"
#include "extension/db.h"
#include "extension/extension.h"
#include "extension/implementation/implementation.h"
#include "extension/init.h"
#include "extension/output.h"
#include "extension/system.h"
#include "extension/internal/svg-publication.h"
#include "file.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "message-stack.h"
#include "io/stream/bufferstream.h"
#include "ui/desktop/document-check.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/node-observer.h"
#include "xml/repr.h"

#include <glib.h>
#include <glibmm/ustring.h>
#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <gtk/gtk.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Inkscape;
namespace Fop = Inkscape::IO;
namespace Pub = Inkscape::Extension::Internal;
Glib::ustring save_admission_refusal_text(Fop::FileOperationResult const &refusal);
void document_save_as(InkscapeWindow *window);
namespace Inkscape::IO::detail {
void set_rename_failure_errno_for_testing(int errno_value);
void set_recovery_delete_failure_errno_for_testing(int errno_value);
}
namespace {

char const kFixtureSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
    "width='96' height='96' viewBox='0 0 96 96'><title>F6a fixture</title><g id='layer'>"
    "<rect id='native' x='2' y='3' width='16' height='12' style='fill:#ff0000;stroke:none'/>"
    "<use id='reference' xlink:href='#native'/></g></svg>";

// Bounded, nonblocking dispatch: assert only our own deferred close.
void dispatch_file_op_test_events()
{
    for (unsigned n = 0; n != 128 && g_main_context_iteration(nullptr, false); ++n) {}
}

// One genuine owner-thread GLib main-context turn: queue a single idle source and
// pump the default context until it has run. Unlike the bounded drain above this
// proves a held resource survives an actual dispatched turn with no worker.
unsigned g_file_op_test_idle_turns = 0;

gboolean file_op_test_idle_turn(gpointer)
{
    ++g_file_op_test_idle_turns;
    return G_SOURCE_REMOVE;
}

bool pump_owner_main_context_turn()
{
    unsigned const before = g_file_op_test_idle_turns;
    g_idle_add(file_op_test_idle_turn, nullptr);
    for (unsigned n = 0; n != 128 && g_file_op_test_idle_turns == before; ++n) {
        g_main_context_iteration(nullptr, false);
    }
    return g_file_op_test_idle_turns == before + 1;
}

InkscapeApplication &file_op_test_application()
{
    if (auto app = InkscapeApplication::instance()) return *app;
    Gtk::Application::wrap_in_search_entry2();
    g_setenv("INKSCAPE_APP_ID_TAG", "fileoperationpacketatest", true);
    return *new InkscapeApplication(); // process-lifetime, as in the export fixture
}

// Native settled fixture: commits one setup entry and then clears the setup
// Undo/Redo so the document starts from a clean baseline (no entry retained).
std::unique_ptr<SPDocument> document(std::string const &xml)
{
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
    if (!doc) throw std::runtime_error("native document fixture");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("F6a fixture"), "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    doc->setModifiedSinceSave(false);
    return doc;
}

void snapshot(XML::Node const *node, std::string &out)
{
    auto field = [&](char const *s) {
        if (!s) { out += "-;"; return; }
        out += std::to_string(std::strlen(s)) + ":" + s;
    };
    out += "[" + std::to_string(static_cast<int>(node->type()));
    field(node->name());
    field(node->content());
    for (auto const &a : node->attributeList()) {
        field(g_quark_to_string(a.key));
        field(a.value);
    }
    out += ";";
    for (auto *c = node->firstChild(); c; c = c->next()) snapshot(c, out);
    out += "]";
}
std::string snapshot(SPDocument const &doc)
{
    std::string out;
    snapshot(doc.getReprDoc(), out);
    return out;
}

XML::Node *find_node(XML::Node *root, char const *name)
{
    if (!std::strcmp(root->name(), name)) return root;
    for (auto *c = root->firstChild(); c; c = c->next())
        if (auto *n = find_node(c, name)) return n;
    return nullptr;
}
XML::Node const *find_node(XML::Node const *root, char const *name)
{
    if (!std::strcmp(root->name(), name)) return root;
    for (auto const *c = root->firstChild(); c; c = c->next())
        if (auto const *n = find_node(c, name)) return n;
    return nullptr;
}
std::string attribute_of(SPDocument const &doc, char const *node_name, char const *attr)
{
    auto const *node = find_node(doc.getReprDoc(), node_name);
    auto const *value = node ? node->attribute(attr) : nullptr;
    return value ? std::string(value) : std::string();
}
Fop::FileOperationRequestInfo method_info(Fop::FileOperationMethod method, char const *target = "target",
                                          char const *context = "context")
{
    Fop::FileOperationRequestInfo info;
    info.method = method;
    info.target = target;
    info.context = context;
    return info;
}
// Deliberate width edit; the undo/redo oracle is checked after Busy refusals.
void deliberate_width_edit(SPDocument &doc, char const *width)
{
    auto *rect = find_node(doc.getReprRoot(), "svg:rect");
    if (!rect) throw std::runtime_error("fixture rect");
    rect->setAttribute("width", width);
    doc.ensureUpToDate();
    DocumentUndo::done(&doc, Util::Internal::ContextString("F6a deliberate width"), "");
}

// The native save path needs the real module database, initialized once after
// the fixture has created its application (mirrors save-target-test.cpp).
void ensure_extension_initialized()
{
    static bool const initialized = [] {
        Inkscape::Extension::init();
        return true;
    }();
    (void)initialized;
}

std::string root_title(SPDocument const &document)
{
    auto *root = document.getRoot();
    if (!root) return {};
    std::unique_ptr<char, decltype(&g_free)> title(root->title(), &g_free);
    return title ? std::string(title.get()) : std::string();
}

// Fresh g_dir_make_tmp plus the save preferences the native path may consult;
// restores every preference and removes only the owned temp directory.
struct TempDirAndPreferences {
    std::filesystem::path dir;
    Glib::ustring save_as;
    Glib::ustring save_copy;
    bool enable_svgexport = false;

    TempDirAndPreferences()
    {
        GError *error = nullptr;
        gchar *created = g_dir_make_tmp("vacards-f6a-native-save-XXXXXX", &error);
        if (!created) {
            std::string const message = error ? error->message : "g_dir_make_tmp failed";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
        dir = std::filesystem::u8path(created);
        g_free(created);

        auto *prefs = Preferences::get();
        save_as = prefs->getString("/dialogs/save_as/default");
        save_copy = prefs->getString("/dialogs/save_copy/default");
        enable_svgexport = prefs->getBool("/dialogs/save_as/enable_svgexport");
        // Optional SVG export processing must not rewrite the projection.
        prefs->setBool("/dialogs/save_as/enable_svgexport", false);
    }

    ~TempDirAndPreferences()
    {
        auto *prefs = Preferences::get();
        prefs->setString("/dialogs/save_as/default", save_as);
        prefs->setString("/dialogs/save_copy/default", save_copy);
        prefs->setBool("/dialogs/save_as/enable_svgexport", enable_svgexport);
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    }

    std::string file(std::string const &name) const { return (dir / name).string(); }
};

// Test-only Output built through the existing build_from_mem() seam. It observes
// the projection while the real Extension::save() stack is active, runs the
// deterministic nested callback, then writes the same provided final filename
// with the real native writer.
class NestedSaveOutputImplementation final : public Inkscape::Extension::Implementation::Implementation {
public:
    SPDocument *original = nullptr;
    std::function<void()> nested;
    int output_calls = 0;
    bool saw_distinct_projection = false;

    void save(Inkscape::Extension::Output *, SPDocument *projection, gchar const *filename) override
    {
        ++output_calls;
        saw_distinct_projection = projection != original;
        if (!saw_distinct_projection) throw Inkscape::Extension::Output::save_failed();
        if (nested) nested();
        if (!sp_repr_save_file(projection->getReprDoc(), filename, SP_SVG_NS_URI)) {
            throw Inkscape::Extension::Output::save_failed();
        }
    }
};

struct RegisteredDocument {
    InkscapeApplication &app = file_op_test_application();
    SPDocument *doc = nullptr;
    sigc::connection destroy_connection;
    RegisteredDocument()
    {
        doc = app.document_add(document(kFixtureSvg));
        if (!doc) throw std::runtime_error("registered fixture document");
        destroy_connection = doc->connectDestroy([this] { doc = nullptr; });
    }
    ~RegisteredDocument()
    {
        if (doc) app.document_close(doc);
        dispatch_file_op_test_events();
        destroy_connection.disconnect();
    }
};

struct AttributeObserver final : XML::NodeObserver {
    XML::Node &node;
    std::function<void()> action;
    bool fired = false;
    explicit AttributeObserver(XML::Node &n) : node(n) { node.addObserver(*this); }
    ~AttributeObserver() override { node.removeObserver(*this); }
    void notifyAttributeChanged(XML::Node &, GQuark, Util::ptr_shared, Util::ptr_shared) override
    {
        if (fired) return;
        fired = true;
        if (action) action();
    }
};

class DocumentFileOperationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        app = &file_op_test_application();
        app->gio_app()->register_application();
        doc = app->document_add(document(kFixtureSvg));
        if (!doc) throw std::runtime_error("registered fixture document");
        destroy_connection = doc->connectDestroy([this] { doc = nullptr; });
    }
    void TearDown() override
    {
        if (doc) app->document_close(doc);
        dispatch_file_op_test_events();
        destroy_connection.disconnect();
    }
    InkscapeApplication *app = nullptr;
    SPDocument *doc = nullptr;
    sigc::connection destroy_connection;
};

template <typename Predicate> bool pump_save_wait(Predicate done, unsigned milliseconds = 3000)
{
    auto const end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!done() && std::chrono::steady_clock::now() < end) {
        g_main_context_iteration(nullptr, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

#ifdef __APPLE__
struct AsyncSaveTestScope {
    std::string previous_switch = g_getenv("VACARDS_ASYNC_SAVE") ? g_getenv("VACARDS_ASYNC_SAVE") : "";
    std::string previous_stall = g_getenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS") ? g_getenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS") : "";
    std::string previous_dialog = g_getenv("VACARDS_SAVE_TEST_CAPTURE_ASYNC_DIALOG") ? g_getenv("VACARDS_SAVE_TEST_CAPTURE_ASYNC_DIALOG") : "";
    std::string previous_gui = g_getenv("INKSCAPE_TEST_GUI") ? g_getenv("INKSCAPE_TEST_GUI") : "";
    AsyncSaveTestScope()
    {
        Fop::enable_file_io_test_hooks();
        g_setenv("VACARDS_ASYNC_SAVE", "1", TRUE);
        g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "300", TRUE);
        g_setenv("VACARDS_SAVE_TEST_CAPTURE_ASYNC_DIALOG", "1", TRUE);
        g_setenv("INKSCAPE_TEST_GUI", "1", TRUE);
    }
    ~AsyncSaveTestScope()
    {
        if (previous_switch.empty()) g_unsetenv("VACARDS_ASYNC_SAVE");
        else g_setenv("VACARDS_ASYNC_SAVE", previous_switch.c_str(), TRUE);
        if (previous_stall.empty()) g_unsetenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS"); else g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", previous_stall.c_str(), TRUE);
        if (previous_dialog.empty()) g_unsetenv("VACARDS_SAVE_TEST_CAPTURE_ASYNC_DIALOG"); else g_setenv("VACARDS_SAVE_TEST_CAPTURE_ASYNC_DIALOG", previous_dialog.c_str(), TRUE);
        if (previous_gui.empty()) g_unsetenv("INKSCAPE_TEST_GUI"); else g_setenv("INKSCAPE_TEST_GUI", previous_gui.c_str(), TRUE);
        set_document_check_context_for_testing(nullptr, nullptr);
        set_file_save_chooser_path_for_testing({});
        Extension::set_save_completion_interleaving_for_testing({});
        Fop::reset_file_io_test_hooks_for_testing();
    }
};

GtkWidget *save_wait_dialog()
{
    auto *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
        auto *item = g_list_model_get_item(windows, i);
        auto *widget = GTK_WIDGET(item);
        if (GTK_IS_DIALOG(widget) && gtk_widget_get_visible(widget) &&
            g_strcmp0(gtk_window_get_title(GTK_WINDOW(widget)),
                      "Saving… waiting for the file to finish") == 0) return widget;
        g_object_unref(item);
    }
    return nullptr;
}

void wait_for_async_save(InkscapeApplication &app, SPDocument *doc)
{
    app.waitForPublication(doc);
    dispatch_file_op_test_events();
}
gboolean async_tick(gpointer data) { ++*static_cast<unsigned *>(data); return G_SOURCE_CONTINUE; }
TEST_F(DocumentFileOperationTest, P4bSwitchOffIsSynchronous)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    g_unsetenv("VACARDS_ASYNC_SAVE");
    auto path = temp.file("p4b-off.svg");
    doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "36");
    EXPECT_FALSE(app->hasPublicationRegistry());
    unsigned ticks = 0; auto timer = g_timeout_add(10, async_tick, &ticks);
    ASSERT_TRUE(sp_file_save_document(window, doc, true));
    g_source_remove(timer); EXPECT_EQ(ticks, 0u);
    EXPECT_FALSE(app->hasPublicationRegistry());
    EXPECT_FALSE(app->publicationInFlight(doc)); EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(DocumentFileOperationTest, P4bEndToEndAndHeartbeat)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    auto remove_desktop = [](SPDesktop *d) { INKSCAPE.remove_desktop(d); delete d; };
    std::unique_ptr<SPDesktop, decltype(remove_desktop)> desktop(new SPDesktop(doc->getNamedView()), remove_desktop);
    INKSCAPE.add_desktop(desktop.get());
    auto path = temp.file("p4b-end.svg"); doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "31"); auto const begin_serial = doc->get_event_log()->getCurrEventSerial();
    Pub::reset_native_serialization_count_for_testing(); ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc)); ASSERT_NE(desktop->messageStack()->currentMessage(), nullptr);
    EXPECT_NE(std::string(desktop->messageStack()->currentMessage()).find("Saving"), std::string::npos);
    unsigned ticks = 0; auto timer = g_timeout_add(10, async_tick, &ticks);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (app->publicationInFlight(doc) && std::chrono::steady_clock::now() < deadline)
        { g_main_context_iteration(nullptr, false); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    g_source_remove(timer); wait_for_async_save(*app, doc);
    EXPECT_GE(ticks, 5u);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_EQ(doc->get_event_log()->getCurrEventSerial(), begin_serial);
    EXPECT_TRUE(doc->get_event_log()->hasFileSaveAnchor());
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    EXPECT_EQ(Pub::native_serialization_count_for_testing(), 1u);
    auto const message_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (desktop->messageStack()->currentMessage() && std::chrono::steady_clock::now() < message_deadline)
        g_main_context_iteration(nullptr, false);
    auto const *remaining = desktop->messageStack()->currentMessage(); EXPECT_TRUE(!remaining || std::string(remaining).find("Saving document") == std::string::npos) << (remaining ? remaining : "");
    auto reopened = SPDocument::createNewDoc(path.c_str(), false); ASSERT_TRUE(reopened);
    EXPECT_STREQ(reopened->getObjectById("native")->getAttribute("width"), "31");
}
TEST_F(DocumentFileOperationTest, P4bEditDuringSavePublishesOlder)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    auto path = temp.file("p4b-older.svg");
    doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "32");
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc));
    deliberate_width_edit(*doc, "33");
    wait_for_async_save(*app, doc);
    EXPECT_TRUE(doc->isModifiedSinceSave()); EXPECT_FALSE(doc->get_event_log()->hasFileSaveAnchor());
    auto reopened = SPDocument::createNewDoc(path.c_str(), false);
    ASSERT_TRUE(reopened);
    EXPECT_STREQ(reopened->getObjectById("native")->getAttribute("width"), "32");
}
TEST_F(DocumentFileOperationTest, P4bLiveInteractionDefersCompletion)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    auto path = temp.file("p4b-interaction.svg");
    doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "38");
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc));
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (app->publicationInFlight(doc) && std::chrono::steady_clock::now() < deadline)
        g_main_context_iteration(nullptr, false);
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    EXPECT_TRUE(doc->isModifiedSinceSave());
    interaction->rollback();
    dispatch_file_op_test_events();
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(DocumentFileOperationTest, P4bBusyAndRevert)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    auto path = temp.file("p4b-busy.svg");
    doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "34");
    Pub::reset_native_serialization_count_for_testing();
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc));
    EXPECT_FALSE(sp_file_save_document(window, doc, true));
    Fop::FileOperationResult refusal;
    auto second = Fop::DocumentFileOperation::admit(*doc,
        method_info(Fop::FileOperationMethod::SaveAs), [] { return true; }, &refusal);
    EXPECT_FALSE(second);
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy);
    EXPECT_NE(save_admission_refusal_text(refusal).raw().find("open save operation"), std::string::npos);
    EXPECT_FALSE(app->document_revert(doc));
    EXPECT_EQ(Pub::native_serialization_count_for_testing(), 1u);
    wait_for_async_save(*app, doc);
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
}
TEST_F(DocumentFileOperationTest, P4bClosePromptSaveWaitsWithoutSecondSerialization)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    auto path = temp.file("p4b-prompt.svg");
    doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "35");
    Pub::reset_native_serialization_count_for_testing();
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc));
    set_document_check_context_for_testing(doc, &window);
    set_document_check_response_for_testing(GTK_RESPONSE_YES);
    EXPECT_TRUE(document_check_for_data_loss(nullptr));
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    EXPECT_EQ(Pub::native_serialization_count_for_testing(), 1u);
}
TEST_F(DocumentFileOperationTest, P4bForcedOlderPreviewRollbackStaysDirty)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    auto remove_desktop = [](SPDesktop *d) { INKSCAPE.remove_desktop(d); delete d; };
    std::unique_ptr<SPDesktop, decltype(remove_desktop)> desktop(new SPDesktop(doc->getNamedView()), remove_desktop);
    INKSCAPE.add_desktop(desktop.get());
    app->gio_app()->register_application();
    InkscapeWindow window(desktop.get());
    auto path = temp.file("p4b-forced-older.svg"); doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "51");
    Pub::reset_native_serialization_count_for_testing();
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc));
    DocumentUndo::undo(doc); // Return to the last saved state while X1 is in flight.
    EXPECT_FALSE(doc->isModifiedSinceSave());
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    find_node(doc->getReprRoot(), "svg:rect")->setAttribute("width", "52");
    set_document_check_context_for_testing(doc, &window);
    set_document_check_response_for_testing(GTK_RESPONSE_YES);
    EXPECT_TRUE(document_check_for_data_loss(desktop.get()));
    EXPECT_EQ(Pub::native_serialization_count_for_testing(), 1u);
    interaction->rollback();
    wait_for_async_save(*app, doc);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    auto reopened = SPDocument::createNewDoc(path.c_str(), false);
    ASSERT_TRUE(reopened);
    EXPECT_STREQ(reopened->getObjectById("native")->getAttribute("width"), "51");
    EXPECT_STREQ(doc->getObjectById("native")->getAttribute("width"), "16");
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    EXPECT_TRUE(document_check_for_data_loss(desktop.get()));
}
TEST_F(DocumentFileOperationTest, P4bBusyWorkerStartDoesNotChangeSaveState)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    auto remove_desktop = [](SPDesktop *d) { INKSCAPE.remove_desktop(d); delete d; };
    std::unique_ptr<SPDesktop, decltype(remove_desktop)> desktop(new SPDesktop(doc->getNamedView()), remove_desktop);
    INKSCAPE.add_desktop(desktop.get());
    InkscapeWindow window(desktop.get());
    auto path = temp.file("p4b-start-fail.svg"); doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "53");
    ASSERT_TRUE(sp_file_save_document(window, doc, false));
    ASSERT_TRUE(doc->get_event_log()->hasFileSaveAnchor());
    deliberate_width_edit(*doc, "54");
    auto const anchor = doc->get_event_log()->hasFileSaveAnchor();
    Pub::PublicationJob occupied;
    occupied.absolute_path = temp.file("occupied.svg");
    occupied.bytes = {std::byte{'x'}};
    occupied.test_hooks_enabled = true;
    occupied.stage_hooks[0].stall_ms = 2000;
    ASSERT_EQ(app->publications().start(doc, std::move(occupied), [](auto) {}),
              Fop::PublicationWorkerRegistry::StartResult::Started);
    EXPECT_FALSE(sp_file_save_document(window, doc, true));
    app->waitForPublication(doc);
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(doc->get_event_log()->hasFileSaveAnchor(), anchor);
    ASSERT_NE(desktop->messageStack()->currentMessage(), nullptr);
    EXPECT_NE(std::string(desktop->messageStack()->currentMessage()).find("Document not saved"),
              std::string::npos);
    auto reopened = SPDocument::createNewDoc(path.c_str(), false);
    ASSERT_TRUE(reopened);
    EXPECT_STREQ(reopened->getObjectById("native")->getAttribute("width"), "53");
}
TEST_F(DocumentFileOperationTest, P4bUntitledChooserDefersFilename)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    doc->setDocumentFilename(nullptr);
    deliberate_width_edit(*doc, "37");
    auto path = temp.file("p4b-first.svg");
    set_file_save_chooser_path_for_testing(path);
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    EXPECT_EQ(doc->getDocumentFilename(), nullptr);
    wait_for_async_save(*app, doc);
    ASSERT_NE(doc->getDocumentFilename(), nullptr);
    EXPECT_EQ(std::string(doc->getDocumentFilename()), path);
    EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(DocumentFileOperationTest, P4bSaveCopyPreservesDocumentState)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    auto original = temp.file("p4b-original.svg");
    auto copy = temp.file("p4b-copy.svg");
    doc->setDocumentFilename(original.c_str());
    deliberate_width_edit(*doc, "39");
    ASSERT_TRUE(sp_file_save_document(window, doc));
    deliberate_width_edit(*doc, "40");
    bool const anchor = doc->get_event_log()->hasFileSaveAnchor();
    auto lease = Fop::DocumentFileOperation::admit(*doc,
        method_info(Fop::FileOperationMethod::SaveCopy, copy.c_str()), [] { return true; });
    ASSERT_TRUE(lease);
    ASSERT_EQ(lease->authorizeOutput(lease->context()).outcome, Fop::FileOperationOutcome::Success);
    auto file = Gio::File::create_for_path(copy);
    EXPECT_EQ(sp_file_save_bound(window, doc, nullptr, file,
        Extension::FILE_SAVE_METHOD_SAVE_COPY, &*lease), FileSaveResult::InProgress);
    ASSERT_TRUE(app->publicationInFlight(doc));
    wait_for_async_save(*app, doc);
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    EXPECT_EQ(std::string(doc->getDocumentFilename()), original);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(doc->get_event_log()->hasFileSaveAnchor(), anchor);
    auto reopened = SPDocument::createNewDoc(copy.c_str(), false);
    ASSERT_TRUE(reopened);
    EXPECT_STREQ(reopened->getObjectById("native")->getAttribute("width"), "40");
}

TEST_F(DocumentFileOperationTest, P4dAliasingSaveCopyWaitsForInteraction)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    auto *desktop = app->createDesktop(doc, false, true);
    ASSERT_NE(desktop, nullptr);
    auto *window = desktop->getInkscapeWindow();
    ASSERT_NE(window, nullptr);
    auto path = temp.file("alias.svg");
    doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "41");
    ASSERT_TRUE(sp_file_save_document(*window, doc, false));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto lease = Fop::DocumentFileOperation::admit(*doc,
        method_info(Fop::FileOperationMethod::SaveCopy, path.c_str()), [] { return true; });
    ASSERT_TRUE(lease);
    auto file = Gio::File::create_for_path(path);
    auto accept_replace = +[](gpointer) -> gboolean {
        auto *windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
            auto *object = g_list_model_get_item(windows, i);
            if (GTK_IS_DIALOG(object) && gtk_widget_get_visible(GTK_WIDGET(object)))
                gtk_dialog_response(GTK_DIALOG(object), GTK_RESPONSE_YES);
            g_object_unref(object);
        }
        return G_SOURCE_CONTINUE;
    };
    auto timer = g_timeout_add(5, accept_replace, nullptr);
    auto result = sp_file_save_bound(*window, doc, desktop, file,
        Extension::FILE_SAVE_METHOD_SAVE_COPY, &*lease);
    g_source_remove(timer);
    EXPECT_EQ(result, FileSaveResult::InProgress);
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (app->publicationInFlight(doc) && std::chrono::steady_clock::now() < deadline)
        { g_main_context_iteration(nullptr, false); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    EXPECT_FALSE(app->publicationInFlight(doc));
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    interaction->rollback();
    dispatch_file_op_test_events();
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_FALSE(doc->get_event_log()->hasFileSaveAnchor());
    app->desktopClose(desktop);
}

TEST_F(DocumentFileOperationTest, P4dDesktopSaveAsBindsOnlyOnCompletion)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    auto *desktop = app->createDesktop(doc, false, true);
    ASSERT_NE(desktop, nullptr);
    auto *window = desktop->getInkscapeWindow();
    ASSERT_NE(window, nullptr);
    auto original = temp.file("original.svg");
    auto target = temp.file("explicit.svg");
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1000", TRUE);
    doc->setDocumentFilename(original.c_str());
    deliberate_width_edit(*doc, "42");
    set_file_save_chooser_path_for_testing(target);
    document_save_as(window);
    EXPECT_EQ(std::string(doc->getDocumentFilename()), original);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!app->publicationInFlight(doc) && std::chrono::steady_clock::now() < deadline)
        g_main_context_iteration(nullptr, false);
    ASSERT_TRUE(app->publicationInFlight(doc));
    deliberate_width_edit(*doc, "43");
    EXPECT_EQ(std::string(doc->getDocumentFilename()), original);
    wait_for_async_save(*app, doc);
    EXPECT_EQ(std::string(doc->getDocumentFilename()), original);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_TRUE(reopened);
    EXPECT_STREQ(reopened->getObjectById("native")->getAttribute("width"), "42");
    app->desktopClose(desktop);
}

TEST_F(DocumentFileOperationTest, P4dDesktopSaveAsSuccessBindsAtCompletion)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    auto *desktop = app->createDesktop(doc, false, true);
    ASSERT_NE(desktop, nullptr);
    auto *window = desktop->getInkscapeWindow();
    ASSERT_NE(window, nullptr);
    auto original = temp.file("before.svg");
    auto target = temp.file("after.svg");
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1000", TRUE);
    doc->setDocumentFilename(original.c_str());
    deliberate_width_edit(*doc, "45");
    set_file_save_chooser_path_for_testing(target);
    document_save_as(window);
    EXPECT_EQ(std::string(doc->getDocumentFilename()), original);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!app->publicationInFlight(doc) && std::chrono::steady_clock::now() < deadline)
        g_main_context_iteration(nullptr, false);
    ASSERT_TRUE(app->publicationInFlight(doc));
    EXPECT_EQ(std::string(doc->getDocumentFilename()), original);
    wait_for_async_save(*app, doc);
    EXPECT_EQ(std::string(doc->getDocumentFilename()), target);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    app->desktopClose(desktop);
}

TEST_F(DocumentFileOperationTest, P4dSaveUsesOriginWindowDesktop)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_unsetenv("VACARDS_ASYNC_SAVE");
    auto *origin = app->createDesktop(doc, false, true);
    ASSERT_NE(origin, nullptr);
    auto *window = origin->getInkscapeWindow();
    ASSERT_NE(window, nullptr);
    auto *other = app->document_add(document(kFixtureSvg));
    ASSERT_NE(other, nullptr);
    auto *other_desktop = app->createDesktop(other, false, true);
    ASSERT_NE(other_desktop, nullptr);
    INKSCAPE.activate_desktop(other_desktop);
    auto origin_path = temp.file("origin.svg");
    auto other_path = temp.file("other.svg");
    ASSERT_TRUE(g_file_set_contents(origin_path.c_str(), "origin old", -1, nullptr));
    ASSERT_TRUE(g_file_set_contents(other_path.c_str(), "other old", -1, nullptr));
    doc->setDocumentFilename(origin_path.c_str());
    other->setDocumentFilename(other_path.c_str());
    deliberate_width_edit(*doc, "46");
    deliberate_width_edit(*other, "47");
    EXPECT_TRUE(sp_file_save(*window, nullptr, nullptr));
    auto reopened = SPDocument::createNewDoc(origin_path.c_str(), false);
    ASSERT_TRUE(reopened);
    EXPECT_STREQ(reopened->getObjectById("native")->getAttribute("width"), "46");
    gchar *contents = nullptr; gsize length = 0;
    ASSERT_TRUE(g_file_get_contents(other_path.c_str(), &contents, &length, nullptr));
    EXPECT_EQ(std::string(contents, length), "other old");
    g_free(contents);
    app->desktopClose(other_desktop);
    app->desktopClose(origin);
}

TEST_F(DocumentFileOperationTest, P4dDesktopSaveAsWindowClosesMidSave)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    auto *desktop = app->createDesktop(doc, false, true);
    ASSERT_NE(desktop, nullptr);
    auto *survivor = app->createDesktop(doc, false, true);
    ASSERT_NE(survivor, nullptr);
    auto *window = desktop->getInkscapeWindow();
    ASSERT_NE(window, nullptr);
    auto original = temp.file("original-close.svg");
    doc->setDocumentFilename(original.c_str());
    deliberate_width_edit(*doc, "44");
    set_file_save_chooser_path_for_testing(temp.file("explicit-close.svg"));
    document_save_as(window);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!app->publicationInFlight(doc) && std::chrono::steady_clock::now() < deadline)
        g_main_context_iteration(nullptr, false);
    ASSERT_TRUE(app->publicationInFlight(doc));
    app->desktopClose(desktop);
    wait_for_async_save(*app, doc);
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    app->desktopClose(survivor);
}

TEST_F(DocumentFileOperationTest, P4dExistingDestinationOffOnOutcomes)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    unsigned dialogs_seen = 0;
    auto dismiss_error = +[](gpointer data) -> gboolean {
        auto *windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
            auto *object = g_list_model_get_item(windows, i);
            if (GTK_IS_DIALOG(object) && gtk_widget_get_visible(GTK_WIDGET(object))) {
                ++*static_cast<unsigned *>(data);
                gtk_dialog_response(GTK_DIALOG(object), GTK_RESPONSE_CLOSE);
            }
            g_object_unref(object);
        }
        return G_SOURCE_CONTINUE;
    };
    auto const dialog_timer = g_timeout_add(5, dismiss_error, &dialogs_seen);
    struct TimerGuard { guint id; ~TimerGuard() { g_source_remove(id); } } timer_guard{dialog_timer};
    auto remove_desktop = [](SPDesktop *d) { INKSCAPE.remove_desktop(d); delete d; };
    for (auto scenario : {"success", "older", "failed", "uncertain", "cleanup"}) {
        for (bool async : {false, true}) {
            auto *saved = app->document_add(document(kFixtureSvg));
            ASSERT_NE(saved, nullptr);
            std::unique_ptr<SPDesktop, decltype(remove_desktop)> desktop(new SPDesktop(saved->getNamedView()), remove_desktop);
            INKSCAPE.add_desktop(desktop.get());
            auto window = std::make_unique<InkscapeWindow>(desktop.get());
            auto path = temp.file(std::string(scenario) + (async ? "-on.svg" : "-off.svg"));
            ASSERT_TRUE(g_file_set_contents(path.c_str(), "known old bytes", -1, nullptr));
            saved->setDocumentFilename(path.c_str());
            deliberate_width_edit(*saved, "73");
            g_setenv("VACARDS_ASYNC_SAVE", async ? "1" : "0", TRUE);
            auto const hook = !std::strcmp(scenario, "cleanup")
                ? "VACARDS_SAVE_TEST_CLEANUP_OUTCOME" : "VACARDS_SAVE_TEST_PUBLISH_OUTCOME";
            if (!std::strcmp(scenario, "failed") || !std::strcmp(scenario, "cleanup"))
                g_setenv(hook, "failed", TRUE);
            if (!std::strcmp(scenario, "uncertain")) {
                Fop::detail::set_rename_failure_errno_for_testing(EIO);
                Fop::detail::set_recovery_delete_failure_errno_for_testing(EACCES);
            }
            if (!async && !std::strcmp(scenario, "older"))
                Extension::set_save_completion_interleaving_for_testing(
                    [](SPDocument *during_save) { deliberate_width_edit(*during_save, "74"); });
            auto const dialogs_before = dialogs_seen;
            auto const started = sp_file_save_document(*window, saved, true, desktop.get());
            if (async) EXPECT_FALSE(started);
            if (async && !std::strcmp(scenario, "older")) deliberate_width_edit(*saved, "74");
            if (async && !std::strcmp(scenario, "cleanup")) {
                auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (app->publicationInFlight(saved) && std::chrono::steady_clock::now() < deadline)
                    { g_main_context_iteration(nullptr, false); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
                EXPECT_FALSE(app->publicationInFlight(saved));
                dispatch_file_op_test_events();
            } else if (async) wait_for_async_save(*app, saved);
            g_unsetenv(hook);
            gchar *contents = nullptr; gsize length = 0;
            ASSERT_TRUE(g_file_get_contents(path.c_str(), &contents, &length, nullptr));
            std::string bytes(contents, length); g_free(contents);
            bool const rejected = !std::strcmp(scenario, "failed") || !std::strcmp(scenario, "uncertain");
            EXPECT_EQ(bytes == "known old bytes", rejected) << scenario << async;
            if (!rejected) {
                auto reopened = SPDocument::createNewDoc(path.c_str(), false);
                ASSERT_TRUE(reopened);
                EXPECT_STREQ(reopened->getObjectById("native")->getAttribute("width"), "73");
            }
            EXPECT_EQ(saved->isModifiedSinceSave(), rejected || !std::strcmp(scenario, "older"));
            if (!rejected && std::strcmp(scenario, "older")) {
                EXPECT_TRUE(saved->get_event_log()->hasFileSaveAnchor());
                EXPECT_EQ(saved->get_event_log()->getLastSavedSerial(), saved->get_event_log()->getCurrEventSerial());
            }
            if (!std::strcmp(scenario, "uncertain")) {
                EXPECT_FALSE(saved->get_event_log()->hasFileSaveAnchor());
                ASSERT_NE(desktop->messageStack()->currentMessage(), nullptr);
                EXPECT_NE(std::string(desktop->messageStack()->currentMessage()).find("uncertain"), std::string::npos)
                    << desktop->messageStack()->currentMessage();
                if (async)
                    EXPECT_NE(g_object_get_data(G_OBJECT(window->gobj()), "vacards-async-dialog-seen"), nullptr);
                else EXPECT_GT(dialogs_seen, dialogs_before);
            }
            if (!std::strcmp(scenario, "cleanup")) {
                ASSERT_NE(desktop->messageStack()->currentMessage(), nullptr);
                EXPECT_NE(std::string(desktop->messageStack()->currentMessage()).find("cleanup warning"), std::string::npos);
            } else if (std::strcmp(scenario, "uncertain")) {
                for (auto const &entry : std::filesystem::directory_iterator(temp.dir))
                    EXPECT_EQ(entry.path().filename().string().find("vacards-"), std::string::npos);
            }
            window.reset();
            desktop.reset();
            app->document_close(saved);
        }
    }
}

TEST_F(DocumentFileOperationTest, P4dUncertainDialogUsesCaptureHook)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    auto original = temp.file("capture-original.svg");
    auto target = temp.file("capture-target.svg");
    doc->setDocumentFilename(original.c_str());
    deliberate_width_edit(*doc, "75");
    auto lease = Fop::DocumentFileOperation::admit(*doc,
        method_info(Fop::FileOperationMethod::SaveAs, target.c_str()), [] { return true; });
    ASSERT_TRUE(lease);
    auto file = Gio::File::create_for_path(target);
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_OUTCOME", "uncertain", TRUE);
    auto result = sp_file_save_bound(window, doc, nullptr, file,
        Extension::FILE_SAVE_METHOD_SAVE_AS, &*lease);
    g_unsetenv("VACARDS_SAVE_TEST_PUBLISH_OUTCOME");
    EXPECT_EQ(result, FileSaveResult::InProgress);
    wait_for_async_save(*app, doc);
    EXPECT_NE(g_object_get_data(G_OBJECT(file->gobj()), "vacards-async-dialog-seen"), nullptr);
    EXPECT_EQ(Fop::DocumentFileOperation::lastTerminalOutcomeForTesting(*doc),
              Fop::FileOperationOutcome::Uncertain);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_FALSE(g_file_test(target.c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(DocumentFileOperationTest, Save1cStalledCloseKeepsHeartbeatAndCompletesOnce)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1200", TRUE);
    auto *desktop = app->createDesktop(doc, false, true);
    ASSERT_TRUE(desktop);
    auto path = temp.file("close.svg"); doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "81");
    ASSERT_FALSE(sp_file_save_document(*desktop->getInkscapeWindow(), doc, true, desktop));
    int destroyed = 0; auto connection = doc->connectDestroy([&] { ++destroyed; });
    unsigned ticks = 0; auto timer = g_timeout_add(10, async_tick, &ticks);
    EXPECT_FALSE(app->destroyDesktop(desktop));
    EXPECT_TRUE(app->closePublicationWaitPending(doc));
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    g_source_remove(timer);
    EXPECT_GE(ticks, 5u);
    EXPECT_EQ(destroyed, 1);
    connection.disconnect();
}

TEST_F(DocumentFileOperationTest, Save1cSaveOnClosePendingAndFailedReturnsToEditing)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    auto path = temp.file("failed.svg"); doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "82");
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_OUTCOME", "failed", TRUE);
    set_document_check_context_for_testing(doc, &window);
    set_document_check_response_for_testing(GTK_RESPONSE_YES);
    EXPECT_TRUE(document_check_for_data_loss(nullptr));
    EXPECT_TRUE(app->publicationInFlight(doc));
    EXPECT_TRUE(app->closePublicationWaitPending(doc));
    EXPECT_TRUE(pump_save_wait([&] { return !app->closePublicationWaitPending(doc); }));
    EXPECT_NE(doc, nullptr);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    g_unsetenv("VACARDS_SAVE_TEST_PUBLISH_OUTCOME");
    set_document_check_response_for_testing(GTK_RESPONSE_YES);
    EXPECT_TRUE(document_check_for_data_loss(nullptr));
    deliberate_width_edit(*doc, "83"); // The published snapshot is now older.
    EXPECT_TRUE(pump_save_wait([&] { return !app->closePublicationWaitPending(doc); }));
    EXPECT_NE(doc, nullptr);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    set_document_check_response_for_testing(GTK_RESPONSE_YES);
    EXPECT_TRUE(document_check_for_data_loss(nullptr));
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
}

TEST_F(DocumentFileOperationTest, Save1cCancelReturnsToEditingWhileSaveSettles)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "600", TRUE);
    auto path = temp.file("cancel.svg"); doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "83");
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    set_document_check_context_for_testing(doc, &window);
    EXPECT_TRUE(document_check_for_data_loss(nullptr));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); g_object_unref(dialog);
    EXPECT_TRUE(pump_save_wait([&] { return !app->closePublicationWaitPending(doc); }));
    EXPECT_NE(doc, nullptr);
    EXPECT_TRUE(pump_save_wait([&] { return !app->publicationInFlight(doc); }));
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(DocumentFileOperationTest, Save1cCloseAnywayRetainsDocumentUntilCompletion)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "2000", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    auto *desktop = app->createDesktop(doc, false, true); ASSERT_TRUE(desktop);
    auto path = temp.file("close-anyway.svg"); doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "84");
    ASSERT_FALSE(sp_file_save_document(*desktop->getInkscapeWindow(), doc, true, desktop));
    EXPECT_FALSE(app->destroyDesktop(desktop));
    EXPECT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog();
        if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001) != nullptr;
        g_object_unref(dialog); return ready;
    }, 400));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    auto *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
    ASSERT_TRUE(button);
    EXPECT_STREQ(gtk_button_get_label(GTK_BUTTON(button)), "Close anyway");
    gchar *warning = nullptr; g_object_get(dialog, "secondary-text", &warning, nullptr);
    ASSERT_TRUE(warning);
    EXPECT_NE(std::string(warning).find(path), std::string::npos);
    EXPECT_NE(std::string(warning).find("previous file remains intact"), std::string::npos);
    g_free(warning);
    gtk_dialog_response(GTK_DIALOG(dialog), 1001); g_object_unref(dialog);
    EXPECT_TRUE(pump_save_wait([&] {
        auto const &views = INKSCAPE.get_desktops();
        return std::find(views.begin(), views.end(), desktop) == views.end();
    }, 400));
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(app->publicationInFlight(doc));
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cFix8QuitAnywayPersistsAndExitsNonzeroInChild)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "5000", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    auto path = temp.file("quit-anyway.svg"); doc->setDocumentFilename(path.c_str());
    auto second_path = temp.file("quit-anyway-second.svg");
    deliberate_width_edit(*doc, "85");
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    auto saved_style = ::testing::FLAGS_gtest_death_test_style;
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT({
        Preferences::get()->setString("/options/save1cchild/value", "persisted");
        app->artworkLibraries(true);
        auto *other = app->document_add(document(kFixtureSvg));
        app->publicationStarted(other, second_path);
        app->on_quit_immediate();
        bool ready = pump_save_wait([&] {
            auto *dialog = save_wait_dialog();
            if (!dialog) return false;
            auto *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
            bool found = button && g_strcmp0(gtk_button_get_label(GTK_BUTTON(button)), "Quit anyway") == 0;
            g_object_unref(dialog); return found;
        }, 400);
        if (!ready) _exit(11);
        auto *dialog = save_wait_dialog();
        gchar *warning = nullptr; g_object_get(dialog, "secondary-text", &warning, nullptr);
        bool const named = warning && std::string(warning).find(path) != std::string::npos &&
                           std::string(warning).find(second_path) != std::string::npos;
        g_free(warning);
        if (!named) _exit(12);
        gtk_dialog_response(GTK_DIALOG(dialog), 1001); g_object_unref(dialog);
        _exit(13);
    }, ::testing::ExitedWithCode(1), "VACARDS_LIBRARY_QUIT_GUARD_COMMITTED");
    ::testing::FLAGS_gtest_death_test_style = saved_style;
    auto const prefs_path = std::string(g_getenv("INKSCAPE_PROFILE_DIR")) + "/preferences.xml";
    gchar *contents = nullptr; gsize length = 0;
    ASSERT_TRUE(g_file_get_contents(prefs_path.c_str(), &contents, &length, nullptr));
    EXPECT_NE(std::string(contents, length).find("persisted"), std::string::npos);
    g_free(contents);
    wait_for_async_save(*app, doc);
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cStalledQuitResumesOnIdle)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "650", TRUE);
    auto path = temp.file("quit-wait.svg"); doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "86");
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    auto saved_style = ::testing::FLAGS_gtest_death_test_style;
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT({
        app->on_quit_immediate();
        if (!app->closePublicationWaitPending(doc)) _exit(11);
        unsigned ticks = 0; auto timer = g_timeout_add(10, async_tick, &ticks);
        bool const closed = pump_save_wait([&] { return doc == nullptr; });
        g_source_remove(timer);
        _exit(closed && ticks >= 5 ? 0 : 12);
    }, ::testing::ExitedWithCode(0), "");
    ::testing::FLAGS_gtest_death_test_style = saved_style;
    wait_for_async_save(*app, doc);
}

TEST_F(DocumentFileOperationTest, Save1cFix1QuitChecksOtherDirtyDocumentBeforeAnyway)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1500", TRUE);
    auto *other = app->document_add(document(kFixtureSvg)); ASSERT_TRUE(other);
    auto docs = app->get_documents();
    auto *waiting = std::find(docs.begin(), docs.end(), other) <
                    std::find(docs.begin(), docs.end(), doc) ? other : doc;
    auto *dirty = waiting == other ? doc : other;
    auto *waiting_view = app->createDesktop(waiting, false, true); ASSERT_TRUE(waiting_view);
    auto *dirty_view = app->createDesktop(dirty, false, true); ASSERT_TRUE(dirty_view);
    waiting->setDocumentFilename(temp.file("waiting.svg").c_str());
    deliberate_width_edit(*waiting, "91");
    ASSERT_FALSE(sp_file_save_document(*waiting_view->getInkscapeWindow(), waiting, true, waiting_view));
    deliberate_width_edit(*dirty, "92");
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    app->on_quit();
    EXPECT_FALSE(app->quitPending());
    EXPECT_TRUE(dirty->isModifiedSinceSave());
    EXPECT_TRUE(app->closePublicationWaitPending(waiting));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    EXPECT_FALSE(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); g_object_unref(dialog);
    dispatch_file_op_test_events();
    wait_for_async_save(*app, waiting);
    for (auto *view : {waiting_view, dirty_view}) {
        auto const &views = INKSCAPE.get_desktops();
        if (std::find(views.begin(), views.end(), view) != views.end()) app->desktopClose(view);
    }
    auto remaining = app->get_documents();
    if (std::find(remaining.begin(), remaining.end(), other) != remaining.end())
        app->document_close(other);
}

TEST_F(DocumentFileOperationTest, Save1cFix2FailedCloseAnywayReopensAndPrompts)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "900", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_OUTCOME", "failed", TRUE);
    auto *desktop = app->createDesktop(doc, false, true); ASSERT_TRUE(desktop);
    doc->setDocumentFilename(temp.file("failed-close-anyway.svg").c_str());
    deliberate_width_edit(*doc, "93");
    ASSERT_FALSE(sp_file_save_document(*desktop->getInkscapeWindow(), doc, true, desktop));
    ASSERT_FALSE(app->destroyDesktop(desktop));
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), 1001); g_object_unref(dialog);
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    EXPECT_TRUE(pump_save_wait([&] { return !app->closePublicationWaitPending(doc) &&
        !app->publicationInFlight(doc); }));
    dispatch_file_op_test_events();
    ASSERT_TRUE(doc);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_FALSE(INKSCAPE.get_desktops().empty());
    g_unsetenv("VACARDS_SAVE_TEST_PUBLISH_OUTCOME");
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cFix2UnknownViewlessResultRequiresPrompt)
{
    AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "900", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    auto *desktop = app->createDesktop(doc, false, true); ASSERT_TRUE(desktop);
    auto job = Pub::PublicationJob{};
    job.absolute_path = temp.file("unknown-viewless.svg");
    job.test_hooks_enabled = true;
    job.bytes = {std::byte{'x'}};
    ASSERT_EQ(app->publications().start(doc, std::move(job), [](auto) {}),
              Fop::PublicationWorkerRegistry::StartResult::Started);
    app->publicationStarted(doc, temp.file("unknown-viewless.svg"));
    ASSERT_TRUE(app->deferCloseForPublication(doc, desktop));
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), 1001); g_object_unref(dialog);
    dispatch_file_op_test_events();
    app->waitForPublication(doc);
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    app->publicationSettled(doc, false);
    EXPECT_TRUE(pump_save_wait([&] { return !app->publicationInFlight(doc) &&
        !app->closePublicationWaitPending(doc); }));
    dispatch_file_op_test_events();
    ASSERT_TRUE(doc);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_FALSE(INKSCAPE.get_desktops().empty());
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cFix4RollbackPrecedesPublicationWait)
{
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc); ASSERT_TRUE(interaction);
    app->publicationStarted(doc, "/pending/fixture.svg");
    app->document_close(doc);
    EXPECT_TRUE(app->documentClosePending(doc));
    EXPECT_FALSE(app->closePublicationWaitPending(doc));
    interaction->rollback();
    app->publicationSettled(doc);
    dispatch_file_op_test_events();
}

TEST_F(DocumentFileOperationTest, Save1cFix5FailedSaveReentersNormalClosePrompt)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_OUTCOME", "failed", TRUE);
    auto *desktop = app->createDesktop(doc, false, true); ASSERT_TRUE(desktop);
    doc->setDocumentFilename(temp.file("failed-reprompt.svg").c_str());
    deliberate_width_edit(*doc, "94");
    ASSERT_FALSE(sp_file_save_document(*desktop->getInkscapeWindow(), doc, true, desktop));
    ASSERT_FALSE(app->destroyDesktop(desktop));
    set_document_check_response_for_testing(GTK_RESPONSE_NO);
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    g_unsetenv("VACARDS_SAVE_TEST_PUBLISH_OUTCOME");
}

TEST_F(DocumentFileOperationTest, Save1cFix7WaitButtonTracksQuitAction)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1400", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    doc->setDocumentFilename(temp.file("label.svg").c_str());
    deliberate_width_edit(*doc, "95");
    Gtk::Window window;
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->deferCloseForPublication(doc));
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    app->on_quit_immediate();
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    auto *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
    ASSERT_TRUE(button);
    EXPECT_STREQ(gtk_button_get_label(GTK_BUTTON(button)), "Quit anyway");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); g_object_unref(dialog);
    wait_for_async_save(*app, doc);
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cF1LaterEditsAreNamedBeforeQuitAnyway)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1600", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    doc->setDocumentFilename(temp.file("later-edits.svg").c_str());
    deliberate_width_edit(*doc, "111");
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    deliberate_width_edit(*doc, "112");
    app->on_quit_immediate();
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    auto *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
    ASSERT_TRUE(button);
    EXPECT_STREQ(gtk_button_get_label(GTK_BUTTON(button)), "Discard edits and quit");
    gchar *warning = nullptr; g_object_get(dialog, "secondary-text", &warning, nullptr);
    ASSERT_TRUE(warning);
    EXPECT_NE(std::string(warning).find("Edits to later-edits.svg made after saving started will be lost."), std::string::npos);
    g_free(warning);
    auto saved_style = ::testing::FLAGS_gtest_death_test_style;
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT({
        deliberate_width_edit(*doc, "116");
        gtk_dialog_response(GTK_DIALOG(dialog), 1001);
        dispatch_file_op_test_events();
        auto *refreshed = save_wait_dialog();
        if (!refreshed) _exit(12);
        auto *choice = gtk_dialog_get_widget_for_response(GTK_DIALOG(refreshed), 1001);
        bool correct = choice && g_strcmp0(gtk_button_get_label(GTK_BUTTON(choice)), "Discard edits and quit") == 0;
        g_object_unref(refreshed);
        _exit(correct ? 0 : 13);
    }, ::testing::ExitedWithCode(0), "");
    ::testing::FLAGS_gtest_death_test_style = saved_style;
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); g_object_unref(dialog);
    dispatch_file_op_test_events();
    wait_for_async_save(*app, doc);
    app->document_close(doc);
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cF2CancelClearsEveryQuitWait)
{
    AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1600", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    auto *other = app->document_add(document(kFixtureSvg)); ASSERT_TRUE(other);
    for (auto *item : {doc, other}) {
        auto job = Pub::PublicationJob{};
        job.absolute_path = temp.file(item == doc ? "a.svg" : "b.svg");
        job.test_hooks_enabled = true; job.bytes = {std::byte{'x'}};
        ASSERT_EQ(app->publications().start(item, std::move(job), [](auto) {}),
                  Fop::PublicationWorkerRegistry::StartResult::Started);
        app->publicationStarted(item, temp.file(item == doc ? "a.svg" : "b.svg"));
    }
    app->on_quit_immediate();
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); g_object_unref(dialog);
    dispatch_file_op_test_events();
    EXPECT_FALSE(app->quitPending());
    auto *remaining = save_wait_dialog(); ASSERT_TRUE(remaining);
    auto *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(remaining), 1001);
    ASSERT_TRUE(button);
    EXPECT_STREQ(gtk_button_get_label(GTK_BUTTON(button)), "Close anyway");
    gtk_dialog_response(GTK_DIALOG(remaining), GTK_RESPONSE_CANCEL); g_object_unref(remaining);
    dispatch_file_op_test_events();
    app->waitForPublication(doc); app->waitForPublication(other);
    app->publicationSettled(doc); app->publicationSettled(other);
    app->document_close(other);
    EXPECT_TRUE(pump_save_wait([&] {
        auto documents = app->get_documents();
        return std::find(documents.begin(), documents.end(), other) == documents.end();
    }));
    app->document_close(doc);
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cEscapeOnOneQuitWaitCancelsTheOthers)
{
    AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1600", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    auto *other = app->document_add(document(kFixtureSvg)); ASSERT_TRUE(other);
    for (auto *item : {doc, other}) {
        auto job = Pub::PublicationJob{};
        job.absolute_path = temp.file(item == doc ? "a.svg" : "b.svg");
        job.test_hooks_enabled = true; job.bytes = {std::byte{'x'}};
        ASSERT_EQ(app->publications().start(item, std::move(job), [](auto) {}),
                  Fop::PublicationWorkerRegistry::StartResult::Started);
        app->publicationStarted(item, temp.file(item == doc ? "a.svg" : "b.svg"));
    }
    app->on_quit_immediate();
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    // Escape / window close is a Cancel, not a silent dismissal of one wait.
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_DELETE_EVENT); g_object_unref(dialog);
    dispatch_file_op_test_events();
    EXPECT_FALSE(app->quitPending());
    auto *remaining = save_wait_dialog(); ASSERT_TRUE(remaining);
    auto *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(remaining), 1001);
    ASSERT_TRUE(button);
    EXPECT_STREQ(gtk_button_get_label(GTK_BUTTON(button)), "Close anyway");
    gtk_dialog_response(GTK_DIALOG(remaining), GTK_RESPONSE_CANCEL); g_object_unref(remaining);
    dispatch_file_op_test_events();
    app->waitForPublication(doc); app->waitForPublication(other);
    app->publicationSettled(doc); app->publicationSettled(other);
    app->document_close(other);
    EXPECT_TRUE(pump_save_wait([&] {
        auto documents = app->get_documents();
        return std::find(documents.begin(), documents.end(), other) == documents.end();
    }));
    app->document_close(doc);
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cQuitAnywayRefusedWhileAnotherDocumentHasChanges)
{
    AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1600", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    auto *other = app->document_add(document(kFixtureSvg)); ASSERT_TRUE(other);
    for (auto *item : {doc, other}) {
        auto job = Pub::PublicationJob{};
        job.absolute_path = temp.file(item == doc ? "a.svg" : "b.svg");
        job.test_hooks_enabled = true; job.bytes = {std::byte{'x'}};
        ASSERT_EQ(app->publications().start(item, std::move(job), [](auto) {}),
                  Fop::PublicationWorkerRegistry::StartResult::Started);
        app->publicationStarted(item, temp.file(item == doc ? "a.svg" : "b.svg"));
    }
    app->on_quit_immediate();
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    // B's save settles as a failure while A still waits: B is no longer
    // waiting but keeps unsaved changes that were never prompted.
    app->waitForPublication(other);
    other->setModifiedSinceSave(true);
    app->publicationSettled(other, false);
    dispatch_file_op_test_events();
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    auto *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
    ASSERT_TRUE(button);
    EXPECT_FALSE(gtk_widget_get_sensitive(button));
    // Even a forced activation must not exit (a broken build would _exit here).
    gtk_dialog_response(GTK_DIALOG(dialog), 1001); g_object_unref(dialog);
    dispatch_file_op_test_events();
    auto *still = save_wait_dialog(); EXPECT_TRUE(still);
    if (still) {
        gtk_dialog_response(GTK_DIALOG(still), GTK_RESPONSE_CANCEL); g_object_unref(still);
    }
    dispatch_file_op_test_events();
    other->setModifiedSinceSave(false);
    app->waitForPublication(doc); app->publicationSettled(doc);
    app->document_close(other);
    EXPECT_TRUE(pump_save_wait([&] {
        auto documents = app->get_documents();
        return std::find(documents.begin(), documents.end(), other) == documents.end();
    }));
    app->document_close(doc);
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cCopyInFlightDoesNotExemptUnsavedChanges)
{
    AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1600", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    // A publication that is not a save of the document itself (e.g. Save a
    // Copy) must not make the document's own unsaved changes look saved.
    auto job = Pub::PublicationJob{};
    job.absolute_path = temp.file("copy.svg");
    job.test_hooks_enabled = true; job.bytes = {std::byte{'x'}};
    ASSERT_EQ(app->publications().start(doc, std::move(job), [](auto) {}),
              Fop::PublicationWorkerRegistry::StartResult::Started);
    app->publicationStarted(doc, temp.file("copy.svg"));
    doc->setModifiedSinceSave(true);
    app->on_quit_immediate();
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    auto *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
    ASSERT_TRUE(button);
    EXPECT_FALSE(gtk_widget_get_sensitive(button));
    gtk_dialog_response(GTK_DIALOG(dialog), 1001); g_object_unref(dialog);
    dispatch_file_op_test_events();
    auto *still = save_wait_dialog(); EXPECT_TRUE(still);
    if (still) { gtk_dialog_response(GTK_DIALOG(still), GTK_RESPONSE_CANCEL); g_object_unref(still); }
    dispatch_file_op_test_events();
    doc->setModifiedSinceSave(false);
    app->waitForPublication(doc); app->publicationSettled(doc);
    app->document_close(doc);
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cF3QuitReopensViewlessWaitDialog)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    for (auto *other : app->get_documents())
        if (other != doc) app->document_close(other);
    ASSERT_TRUE(pump_save_wait([&] { return app->get_documents().size() == 1; }));
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "5000", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    auto *desktop = app->createDesktop(doc, false, true); ASSERT_TRUE(desktop);
    doc->setDocumentFilename(temp.file("viewless-quit.svg").c_str());
    deliberate_width_edit(*doc, "113");
    ASSERT_FALSE(sp_file_save_document(*desktop->getInkscapeWindow(), doc, true, desktop));
    ASSERT_FALSE(app->destroyDesktop(desktop));
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), 1001); g_object_unref(dialog);
    ASSERT_TRUE(pump_save_wait([&] {
        auto const &views = INKSCAPE.get_desktops();
        return std::find(views.begin(), views.end(), desktop) == views.end();
    }, 400));
    ASSERT_TRUE(app->closePublicationWaitPending(doc));
    ASSERT_TRUE(app->publicationInFlight(doc));
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    app->on_quit();
    auto *quit_dialog = save_wait_dialog(); ASSERT_TRUE(quit_dialog);
    EXPECT_TRUE(gtk_dialog_get_widget_for_response(GTK_DIALOG(quit_dialog), GTK_RESPONSE_CANCEL));
    gtk_dialog_response(GTK_DIALOG(quit_dialog), GTK_RESPONSE_CANCEL); g_object_unref(quit_dialog);
    dispatch_file_op_test_events();
    // Cancel withdraws the quit but the windowless document keeps its wait,
    // so a failed outcome can still reopen it.
    EXPECT_TRUE(app->closePublicationWaitPending(doc));
    EXPECT_FALSE(app->quitPending());
    wait_for_async_save(*app, doc);
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cF4DeferredSuccessfulCloseAnywayDoesNotReopen)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "2500", TRUE);
    g_setenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS", "20", TRUE);
    Gtk::Window window;
    doc->setDocumentFilename(temp.file("deferred-clean.svg").c_str());
    deliberate_width_edit(*doc, "114");
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->deferCloseForPublication(doc));
    ASSERT_TRUE(pump_save_wait([&] {
        auto *dialog = save_wait_dialog(); if (!dialog) return false;
        bool ready = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), 1001);
        g_object_unref(dialog); return ready;
    }, 400));
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), 1001); g_object_unref(dialog);
    dispatch_file_op_test_events();
    auto hold = DocumentUndo::holdInteractionOperation(doc); ASSERT_TRUE(hold);
    ASSERT_TRUE(pump_save_wait([&] { return !app->publicationInFlight(doc); }));
    EXPECT_TRUE(app->closePublicationWaitPending(doc));
    hold.reset();
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    EXPECT_TRUE(INKSCAPE.get_desktops().empty());
    g_unsetenv("VACARDS_SAVE_TEST_CLOSE_WAIT_MS");
}

TEST_F(DocumentFileOperationTest, Save1cF5WindowlessModifiedQuitPromptsOnceWithSwitchOff)
{
    AsyncSaveTestScope env;
    g_unsetenv("VACARDS_ASYNC_SAVE");
    deliberate_width_edit(*doc, "115");
    reset_document_check_prompt_count_for_testing();
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    app->on_quit();
    EXPECT_EQ(document_check_prompt_count_for_testing(), 1u);
    EXPECT_FALSE(app->quitPending());
    EXPECT_NE(doc, nullptr);
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

TEST_F(DocumentFileOperationTest, Save1cFix3StoredResultEndsDestinationWait)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc); ASSERT_TRUE(interaction);
    doc->setDocumentFilename(temp.file("deferred-result.svg").c_str());
    deliberate_width_edit(*doc, "96");
    Gtk::Window window;
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(pump_save_wait([&] { return !app->publicationInFlight(doc); }));
    EXPECT_FALSE(app->deferCloseForPublication(doc));
    EXPECT_FALSE(app->closePublicationWaitPending(doc));
    interaction->rollback();
    dispatch_file_op_test_events();
}

TEST_F(DocumentFileOperationTest, Save1cFix3RejectedCompletionReleasesLeaseAndWait)
{
    auto saved_style = ::testing::FLAGS_gtest_death_test_style;
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT({
        ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
        doc->setDocumentFilename(temp.file("rejected-completion.svg").c_str());
        deliberate_width_edit(*doc, "99");
        Gtk::Window window;
        if (sp_file_save_document(window, doc, true)) _exit(11);
        DocumentUndo::documentClosing(doc); // The early-return branch in the Undo continuation gate.
        bool const completed = pump_save_wait([&] {
            return !app->publicationInFlight(doc) && !Fop::DocumentFileOperation::hasActiveRequest(*doc);
        });
        if (!completed || app->deferCloseForPublication(doc) || app->closePublicationWaitPending(doc))
            _exit(12);
        _exit(0);
    }, ::testing::ExitedWithCode(0), "");
    ::testing::FLAGS_gtest_death_test_style = saved_style;
}

TEST_F(DocumentFileOperationTest, Save1cFix6CancelledPromptCannotInheritAnotherWait)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    g_setenv("VACARDS_SAVE_TEST_PUBLISH_STALL_MS", "1400", TRUE);
    auto *other = app->document_add(document(kFixtureSvg)); ASSERT_TRUE(other);
    auto docs = app->get_documents();
    auto *dirty = std::find(docs.begin(), docs.end(), other) <
                  std::find(docs.begin(), docs.end(), doc) ? other : doc;
    auto *waiting = dirty == other ? doc : other;
    auto *dirty_view = app->createDesktop(dirty, false, true); ASSERT_TRUE(dirty_view);
    auto *waiting_view = app->createDesktop(waiting, false, true); ASSERT_TRUE(waiting_view);
    waiting->setDocumentFilename(temp.file("other-wait.svg").c_str());
    deliberate_width_edit(*waiting, "97");
    ASSERT_FALSE(sp_file_save_document(*waiting_view->getInkscapeWindow(), waiting, true, waiting_view));
    ASSERT_TRUE(app->deferCloseForPublication(waiting, waiting_view));
    deliberate_width_edit(*dirty, "98");
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    app->on_quit();
    EXPECT_FALSE(app->quitPending());
    EXPECT_TRUE(dirty->isModifiedSinceSave());
    auto *dialog = save_wait_dialog(); ASSERT_TRUE(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); g_object_unref(dialog);
    dispatch_file_op_test_events();
    wait_for_async_save(*app, waiting);
    for (auto *view : {dirty_view, waiting_view}) {
        auto const &views = INKSCAPE.get_desktops();
        if (std::find(views.begin(), views.end(), view) != views.end()) app->desktopClose(view);
    }
    auto remaining = app->get_documents();
    if (std::find(remaining.begin(), remaining.end(), other) != remaining.end())
        app->document_close(other);
}

TEST_F(DocumentFileOperationTest, P4cCloseWaits)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    doc->setDocumentFilename(temp.file("close-clean.svg").c_str());
    deliberate_width_edit(*doc, "61");
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc));
    set_document_check_context_for_testing(doc, &window);
    set_document_check_response_for_testing(GTK_RESPONSE_YES);
    auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(document_check_for_data_loss(nullptr));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
}

TEST_F(DocumentFileOperationTest, P4cQuitDuring)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    doc->setDocumentFilename(temp.file("quit.svg").c_str());
    deliberate_width_edit(*doc, "64");
    bool completed_before_destroy = false;
    auto connection = doc->connectDestroy([&] {
        completed_before_destroy = !app->publicationInFlight(doc) &&
                                   !Fop::DocumentFileOperation::hasActiveRequest(*doc);
    });
    auto *saved_doc = doc;
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc));
    app->on_quit_immediate();
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    EXPECT_TRUE(completed_before_destroy);
    EXPECT_EQ(doc, nullptr);
    EXPECT_FALSE(app->publicationInFlight(saved_doc));
    EXPECT_FALSE(app->publications().pending(saved_doc));
    connection.disconnect();
}

TEST_F(DocumentFileOperationTest, P4cWindowDestroyed)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp;
    auto *origin = app->createDesktop(doc, false, true);
    ASSERT_NE(origin, nullptr);
    auto *survivor = app->createDesktop(doc, false, true);
    ASSERT_NE(survivor, nullptr);
    auto *window = origin->getInkscapeWindow();
    ASSERT_NE(window, nullptr);
    doc->setDocumentFilename(temp.file("closed-window.svg").c_str());
    deliberate_width_edit(*doc, "65");
    ASSERT_FALSE(sp_file_save_document(*window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc));
    app->desktopClose(origin);
    EXPECT_TRUE(app->publicationInFlight(doc));
    wait_for_async_save(*app, doc);
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
    EXPECT_FALSE(doc->isModifiedSinceSave());
    app->desktopClose(survivor);
}

TEST_F(DocumentFileOperationTest, P4cAutosaveSkip)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    auto *prefs = Preferences::get();
    auto old_path = prefs->getString("/options/autosave/path");
    prefs->setString("/options/autosave/path", temp.dir.string());
    struct RestorePath { Glib::ustring old; ~RestorePath() { Preferences::get()->setString("/options/autosave/path", old); } } restore{old_path};
    auto &autosave = AutoSave::getInstance();
    autosave.init(app);
    auto calls = Fop::DocumentTransaction::make_platform_system_calls();
    ASSERT_TRUE(calls);
    auto const target = temp.file("autosave.svg");
    doc->setDocumentFilename(target.c_str());
    deliberate_width_edit(*doc, "66");
    ASSERT_TRUE(doc->isModifiedSinceAutoSave());
    ASSERT_FALSE(sp_file_save_document(window, doc, true));
    ASSERT_TRUE(app->publicationInFlight(doc));
    auto count_files = [&] {
        unsigned count = 0;
        for (auto const &entry : std::filesystem::recursive_directory_iterator(temp.dir))
            if (entry.is_regular_file() && entry.path().filename().string().starts_with("autosave-")) ++count;
        return count;
    };
    auto const before = count_files();
    EXPECT_TRUE(autosave.save(*calls));
    EXPECT_EQ(count_files(), before);
    EXPECT_TRUE(doc->isModifiedSinceAutoSave());
    deliberate_width_edit(*doc, "68"); // Keep a newer unsaved revision when publication finishes.
    wait_for_async_save(*app, doc);
    EXPECT_TRUE(doc->isModifiedSinceAutoSave());
    EXPECT_TRUE(autosave.save(*calls));
    EXPECT_FALSE(doc->isModifiedSinceAutoSave());
    EXPECT_GT(count_files(), before);
}

struct P4cStageCase {
    unsigned stage;
    char const *outcome;
    bool async_first;
};

class P4cStageFailures : public DocumentFileOperationTest,
                         public ::testing::WithParamInterface<P4cStageCase> {};

TEST_P(P4cStageFailures, OffAndOnHaveSameCompletedState)
{
    ensure_extension_initialized(); AsyncSaveTestScope env; TempDirAndPreferences temp; Gtk::Window window;
    static char const *stages[] = {"ADMISSION", "CREATE", "STAGE_WRITE", "SEAL", "PUBLISH", "CLEANUP"};
    auto const param = GetParam();
    auto const hook = std::string("VACARDS_SAVE_TEST_") + stages[param.stage] + "_OUTCOME";
    struct HookGuard { std::string name; ~HookGuard() { g_unsetenv(name.c_str()); } } guard{hook};
    g_setenv(hook.c_str(), param.outcome, TRUE);
    auto dismiss_error = +[](gpointer) -> gboolean {
        auto *windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
            auto *object = g_list_model_get_item(windows, i);
            if (GTK_IS_DIALOG(object) && gtk_widget_get_visible(GTK_WIDGET(object)))
                gtk_dialog_response(GTK_DIALOG(object), GTK_RESPONSE_CLOSE);
            g_object_unref(object);
        }
        return G_SOURCE_CONTINUE;
    };
    auto const dialog_timer = g_timeout_add(5, dismiss_error, nullptr);
    struct TimerGuard { guint id; ~TimerGuard() { g_source_remove(id); } } timer_guard{dialog_timer};
    struct State {
        bool modified = false;
        bool anchor = false;
        std::uint64_t anchor_serial = 0;
        std::uint64_t current_serial = 0;
        std::optional<Fop::FileOperationOutcome> lease_outcome;
        std::string status;
        bool slot_free = false;
        bool file_exists = false;
    } observed[2];
    for (unsigned turn = 0; turn < 2; ++turn) {
        bool const async = turn == 0 ? param.async_first : !param.async_first;
        g_setenv("VACARDS_ASYNC_SAVE", async ? "1" : "0", TRUE);
        auto *test_doc = app->document_add(document(kFixtureSvg));
        ASSERT_NE(test_doc, nullptr);
        auto remove_desktop = [](SPDesktop *d) { INKSCAPE.remove_desktop(d); delete d; };
        std::unique_ptr<SPDesktop, decltype(remove_desktop)> desktop(new SPDesktop(test_doc->getNamedView()), remove_desktop);
        INKSCAPE.add_desktop(desktop.get());
        auto const path = temp.file("stage.svg");
        test_doc->setDocumentFilename(path.c_str());
        deliberate_width_edit(*test_doc, "67");
        auto const result = sp_file_save_document(window, test_doc, true);
        if (async && app->publicationInFlight(test_doc)) {
            EXPECT_FALSE(result);
            if (param.stage == 4 && !param.async_first) {
                auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (app->publicationInFlight(test_doc) && std::chrono::steady_clock::now() < deadline) {
                    g_main_context_iteration(nullptr, false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                EXPECT_FALSE(app->publicationInFlight(test_doc));
                dispatch_file_op_test_events();
            } else wait_for_async_save(*app, test_doc);
        }
        auto &state = observed[async ? 1 : 0];
        state.modified = test_doc->isModifiedSinceSave();
        state.anchor = test_doc->get_event_log()->hasFileSaveAnchor();
        state.anchor_serial = test_doc->get_event_log()->getLastSavedSerial();
        state.current_serial = test_doc->get_event_log()->getCurrEventSerial();
        state.lease_outcome = Fop::DocumentFileOperation::lastTerminalOutcomeForTesting(*test_doc);
        if (auto const *message = desktop->messageStack()->currentMessage()) state.status = message;
        state.slot_free = !Fop::DocumentFileOperation::hasActiveRequest(*test_doc);
        state.file_exists = g_file_test(path.c_str(), G_FILE_TEST_EXISTS);
        desktop.reset();
        app->document_close(test_doc);
        if (turn == 0 && state.file_exists) ASSERT_EQ(std::remove(path.c_str()), 0);
    }
    EXPECT_TRUE(observed[0].slot_free);
    EXPECT_TRUE(observed[1].slot_free);
    EXPECT_EQ(observed[0].modified, observed[1].modified);
    EXPECT_EQ(observed[0].anchor, observed[1].anchor);
    ASSERT_TRUE(observed[0].lease_outcome.has_value());
    ASSERT_TRUE(observed[1].lease_outcome.has_value());
    EXPECT_EQ(observed[0].lease_outcome, observed[1].lease_outcome);
    if (param.stage == 4) {
        auto const expected = std::strcmp(param.outcome, "uncertain") == 0
            ? Fop::FileOperationOutcome::Uncertain : Fop::FileOperationOutcome::Failed;
        EXPECT_EQ(observed[0].lease_outcome, expected);
        EXPECT_EQ(observed[1].lease_outcome, expected);
    }
    EXPECT_EQ(observed[0].anchor_serial, observed[1].anchor_serial);
    EXPECT_EQ(observed[0].current_serial, observed[1].current_serial);
    EXPECT_EQ(observed[0].status, observed[1].status);
    EXPECT_EQ(observed[0].file_exists, observed[1].file_exists);
    bool const rejects_publication = param.stage == 0 || param.stage == 4 ||
        (param.stage < 5 && std::strcmp(param.outcome, "failed") == 0);
    EXPECT_EQ(observed[0].modified, rejects_publication);
    // Definite admission refusals preserve the pre-existing Undo anchor; failed
    // and uncertain publication invalidate it. Both switches must agree.
    bool const preserves_anchor = (param.stage == 0 || param.stage == 4) &&
        (std::strcmp(param.outcome, "conflict") == 0 ||
         std::strcmp(param.outcome, "unsupported") == 0 ||
         std::strcmp(param.outcome, "read_only") == 0);
    EXPECT_EQ(observed[0].anchor, !rejects_publication || preserves_anchor);
    EXPECT_EQ(observed[0].file_exists, !rejects_publication);
}

INSTANTIATE_TEST_SUITE_P(P4c, P4cStageFailures, ::testing::ValuesIn([] {
    std::vector<P4cStageCase> cases;
    for (unsigned stage = 0; stage < 6; ++stage)
        for (auto *outcome : {"failed", "conflict", "unsupported", "uncertain", "read_only"})
            if ((stage < 1 || stage > 3 || std::strcmp(outcome, "failed") == 0) &&
                (stage != 5 || std::strcmp(outcome, "failed") == 0))
            for (bool async_first : {false, true}) cases.push_back({stage, outcome, async_first});
    return cases;
}()));
#endif

TEST_F(DocumentFileOperationTest, RealSaveEntryPointsPublishAndSetDirtyState)
{
    ensure_extension_initialized();
    TempDirAndPreferences temp;
    Gtk::Window window; // The entry points only need a parent; no window realization.
    struct ResetHooks {
        ~ResetHooks() {
            Fop::reset_file_io_test_hooks_for_testing();
            set_file_save_chooser_path_for_testing({});
        }
    } reset_hooks;
    Fop::enable_file_io_test_hooks();
    auto const first = temp.file("first-save.svg");
    auto const save_as = temp.file("save-as.svg");
    auto const copy = temp.file("save-copy.svg");
    auto const close = temp.file("close-prompt.svg");
    auto expect_written = [](std::string const &path, char const *width) {
        auto reopened = SPDocument::createNewDoc(path.c_str(), false);
        ASSERT_NE(reopened, nullptr) << path;
        auto *rect = reopened->getObjectById("native");
        ASSERT_NE(rect, nullptr);
        EXPECT_STREQ(rect->getAttribute("width"), width);
    };

    doc->setDocumentFilename(nullptr);
    ASSERT_EQ(doc->getDocumentFilename(), nullptr);
    deliberate_width_edit(*doc, "21");
    set_file_save_chooser_path_for_testing(first);
    ASSERT_TRUE(sp_file_save_document(window, doc));
    expect_written(first, "21");
    EXPECT_FALSE(doc->isModifiedSinceSave());

    deliberate_width_edit(*doc, "22");
    ASSERT_TRUE(sp_file_save_document(window, doc));
    expect_written(first, "22");
    EXPECT_FALSE(doc->isModifiedSinceSave());

    deliberate_width_edit(*doc, "23");
    set_file_save_chooser_path_for_testing(save_as);
    ASSERT_TRUE(sp_file_save_dialog(window, doc, Extension::FILE_SAVE_METHOD_SAVE_AS));
    expect_written(save_as, "23");
    EXPECT_FALSE(doc->isModifiedSinceSave());

    deliberate_width_edit(*doc, "24");
    set_file_save_chooser_path_for_testing(copy);
    ASSERT_TRUE(sp_file_save_dialog(window, doc, Extension::FILE_SAVE_METHOD_SAVE_COPY));
    expect_written(copy, "24");
    EXPECT_TRUE(doc->isModifiedSinceSave());

    doc->setDocumentFilename(close.c_str());
    deliberate_width_edit(*doc, "25");
    ASSERT_TRUE(document_check_save_for_close(window, doc));
    expect_written(close, "25");
    EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(DocumentFileOperationTest, UntitledClosePromptSavesThroughRealEntryPoint)
{
    ensure_extension_initialized();
    TempDirAndPreferences temp;
    Fop::enable_file_io_test_hooks();
    struct ResetHooks {
        ~ResetHooks() {
            set_document_check_context_for_testing(nullptr, nullptr);
            set_file_save_chooser_path_for_testing({});
            Fop::reset_file_io_test_hooks_for_testing();
        }
    } reset_hooks;
    Gtk::Window window;
    set_document_check_context_for_testing(doc, &window);
    doc->setDocumentFilename(nullptr);
    deliberate_width_edit(*doc, "27");
    auto const target = temp.file("untitled-close.svg");
    set_file_save_chooser_path_for_testing(target);
    set_document_check_response_for_testing(GTK_RESPONSE_YES);
    EXPECT_FALSE(document_check_for_data_loss(nullptr));
    EXPECT_FALSE(doc->isModifiedSinceSave());
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    auto *rect = reopened->getObjectById("native");
    ASSERT_NE(rect, nullptr);
    EXPECT_STREQ(rect->getAttribute("width"), "27");
}

TEST_F(DocumentFileOperationTest, RevertRefusesBeforeReplacingDocumentDuringExplicitSave)
{
    TempDirAndPreferences temp;
    auto const path = temp.file("revert.svg");
    ASSERT_TRUE(g_file_set_contents(path.c_str(), kFixtureSvg, -1, nullptr));
    doc->setDocumentFilename(path.c_str());
    deliberate_width_edit(*doc, "31.25");
    auto const before = snapshot(*doc);
    auto *const original = doc;

    auto lease = Fop::DocumentFileOperation::admit(
        *doc, method_info(Fop::FileOperationMethod::SaveAs), [] { return true; });
    ASSERT_TRUE(lease);

    EXPECT_FALSE(app->document_revert(doc));
    EXPECT_EQ(doc, original);
    EXPECT_EQ(snapshot(*doc), before);
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(*doc));
}

TEST_F(DocumentFileOperationTest, AdmitOwnsRequestInfoReusesGateAndPreservesOracle)
{
    auto &d = *doc;
    deliberate_width_edit(d, "31.25");
    auto const before = snapshot(d);
    auto const before_dirty = d.isModifiedSinceSave();

    auto info = method_info(Fop::FileOperationMethod::SaveCopy, "target-original", "context-original");
    auto guard = Fop::DocumentFileOperation::admit(d, info, [] { return true; });
    ASSERT_TRUE(guard);
    EXPECT_FALSE(guard->terminal());
    EXPECT_FALSE(guard->terminalResult());

    auto ctx = guard->context();
    ASSERT_TRUE(ctx.valid());
    ASSERT_TRUE(ctx.info());
    EXPECT_EQ(ctx.info()->method, Fop::FileOperationMethod::SaveCopy);
    EXPECT_EQ(ctx.info()->target, "target-original");
    EXPECT_EQ(ctx.info()->context, "context-original");
    info.target = "target-mutated";
    info.context = "context-mutated";
    ASSERT_TRUE(ctx.info());
    EXPECT_EQ(ctx.info()->target, "target-original");
    EXPECT_EQ(ctx.info()->context, "context-original");

    // The save -> save-as/overwrite-retry caller may re-gate the same request.
    for (int i = 0; i < 3; ++i) {
        auto const gate = guard->authorizeOutput(ctx);
        EXPECT_EQ(gate.outcome, Fop::FileOperationOutcome::Success) << gate.detail;
    }

    // Every fresh method is Busy while the slot/lease is held.
    for (auto method : {Fop::FileOperationMethod::Save, Fop::FileOperationMethod::SaveAs,
                        Fop::FileOperationMethod::SaveCopy}) {
        Fop::FileOperationResult refusal;
        auto denied = Fop::DocumentFileOperation::admit(d, method_info(method), [] { return true; }, &refusal);
        EXPECT_FALSE(denied);
        EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy);
    }

    // A second registered document admits independently (no cross-document slot).
    RegisteredDocument other;
    ASSERT_NE(other.doc, nullptr);
    auto other_guard = Fop::DocumentFileOperation::admit(
        *other.doc, method_info(Fop::FileOperationMethod::Save), [] { return true; });
    ASSERT_TRUE(other_guard);

    Fop::FileOperationContext empty;
    EXPECT_EQ(guard->authorizeOutput(empty).outcome, Fop::FileOperationOutcome::StaleTarget);
    EXPECT_EQ(guard->authorizeOutput(other_guard->context()).outcome, Fop::FileOperationOutcome::StaleTarget);

    // Busy refusals above changed no fingerprint, dirty bit, sensitivity or history.
    EXPECT_EQ(snapshot(d), before);
    EXPECT_EQ(d.isModifiedSinceSave(), before_dirty);
    EXPECT_TRUE(DocumentUndo::getUndoSensitive(&d));
    EXPECT_TRUE(d.getReprDoc()->inTransaction());

    guard.reset();
    other_guard.reset();
    dispatch_file_op_test_events();

    EXPECT_EQ(attribute_of(d, "svg:rect", "width"), "31.25");
    EXPECT_TRUE(DocumentUndo::undo(&d));
    EXPECT_EQ(attribute_of(d, "svg:rect", "width"), "16");
    EXPECT_TRUE(DocumentUndo::redo(&d));
    EXPECT_EQ(attribute_of(d, "svg:rect", "width"), "31.25");
    EXPECT_EQ(snapshot(d), before);
}

TEST_F(DocumentFileOperationTest, GuardMoveCompletionEscapeAndAbandon)
{
    auto &d = *doc;
    auto const baseline = app->documentOperationCount();
    auto guard = Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::SaveCopy), [] { return true; });
    ASSERT_TRUE(guard);
    auto ctx = guard->context();

    auto moved = std::move(*guard);
    EXPECT_TRUE(ctx.valid());
    EXPECT_FALSE(static_cast<bool>(*guard));
    EXPECT_FALSE(guard->terminal());
    EXPECT_FALSE(guard->terminalResult());
    EXPECT_EQ(guard->authorizeOutput(ctx).outcome, Fop::FileOperationOutcome::StaleTarget);

    unsigned notifications = 0;
    Fop::FileOperationResult observed;
    moved.complete({Fop::FileOperationOutcome::Uncertain, "publication ambiguous"},
                   [&](Fop::FileOperationResult const &r) { ++notifications; observed = r; });
    EXPECT_EQ(notifications, 1u);
    EXPECT_EQ(observed.outcome, Fop::FileOperationOutcome::Uncertain);
    EXPECT_EQ(observed.detail, "publication ambiguous");
    ASSERT_TRUE(moved.terminalResult());
    EXPECT_EQ(moved.terminalResult()->outcome, Fop::FileOperationOutcome::Uncertain);
    EXPECT_EQ(moved.terminalResult()->detail, "publication ambiguous");

    moved.complete({Fop::FileOperationOutcome::Success, "must not replace"},
                   [&](Fop::FileOperationResult const &) { ++notifications; });
    EXPECT_EQ(notifications, 1u);
    ASSERT_TRUE(moved.terminalResult());
    EXPECT_EQ(moved.terminalResult()->outcome, Fop::FileOperationOutcome::Uncertain);
    EXPECT_EQ(moved.terminalResult()->detail, "publication ambiguous");
    EXPECT_TRUE(moved.notificationError().empty());

    // Terminal does not free the slot; it stays Busy until the guard is reset.
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
    Fop::FileOperationResult refusal;
    auto denied = Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::Save), [] { return true; }, &refusal);
    EXPECT_FALSE(denied);
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy);

    auto escaped = moved.context();
    EXPECT_TRUE(escaped.valid());
    moved = Fop::FileOperationLease{};
    EXPECT_FALSE(escaped.valid());
    EXPECT_FALSE(escaped.info());
    dispatch_file_op_test_events();

    // A new guard owns the document; the escaped old token is still denied.
    auto next = Fop::DocumentFileOperation::admit(d, method_info(Fop::FileOperationMethod::Save), [] { return true; });
    ASSERT_TRUE(next);
    EXPECT_EQ(next->authorizeOutput(escaped).outcome, Fop::FileOperationOutcome::StaleTarget);
    next.reset();
    dispatch_file_op_test_events();

    // An abandoned guard completes nothing and releases the slot/lease.
    {
        auto abandoned = Fop::DocumentFileOperation::admit(
            d, method_info(Fop::FileOperationMethod::SaveAs), [] { return true; });
        ASSERT_TRUE(abandoned);
        EXPECT_FALSE(abandoned->terminal());
        EXPECT_FALSE(abandoned->terminalResult());
        EXPECT_TRUE(abandoned->notificationError().empty());
    }
    dispatch_file_op_test_events();
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline);
}

TEST_F(DocumentFileOperationTest, AdmissionAndGateRefusalsLeaveNoSlotOrLease)
{
    auto &d = *doc;
    auto const before = snapshot(d);
    bool const before_dirty = d.isModifiedSinceSave();
    auto const baseline = app->documentOperationCount();
    auto info = method_info(Fop::FileOperationMethod::Save);
    Fop::FileOperationResult refusal;

    EXPECT_FALSE(Fop::DocumentFileOperation::admit(d, info, {}, &refusal));
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Failed);

    auto private_doc = document(kFixtureSvg);
    ASSERT_TRUE(private_doc);
    EXPECT_FALSE(Fop::DocumentFileOperation::admit(*private_doc, info, [] { return true; }, &refusal));
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Failed);

    EXPECT_FALSE(Fop::DocumentFileOperation::admit(d, info, [] { return false; }, &refusal));
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::StaleTarget);

    EXPECT_FALSE(Fop::DocumentFileOperation::admit(
        d, info, []() -> bool { throw std::runtime_error("std binding failure"); }, &refusal));
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Failed);
    EXPECT_NE(refusal.detail.find("std binding failure"), std::string::npos);

    struct UnknownBinding {};
    EXPECT_FALSE(Fop::DocumentFileOperation::admit(d, info, []() -> bool { throw UnknownBinding{}; }, &refusal));
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Failed);
    EXPECT_FALSE(refusal.detail.empty());

    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline);

    // The predicate runs again at output time; a late false/throw denies output.
    int false_calls = 0;
    auto false_guard = Fop::DocumentFileOperation::admit(d, info, [&]() -> bool { return ++false_calls == 1; });
    ASSERT_TRUE(false_guard);
    EXPECT_EQ(false_guard->authorizeOutput(false_guard->context()).outcome, Fop::FileOperationOutcome::StaleTarget);
    false_guard.reset();
    dispatch_file_op_test_events();

    int throw_calls = 0;
    auto throw_guard = Fop::DocumentFileOperation::admit(d, info, [&]() -> bool {
        if (++throw_calls == 2) throw std::runtime_error("late binding failure");
        return true;
    });
    ASSERT_TRUE(throw_guard);
    auto const late = throw_guard->authorizeOutput(throw_guard->context());
    EXPECT_EQ(late.outcome, Fop::FileOperationOutcome::Failed);
    EXPECT_NE(late.detail.find("late binding failure"), std::string::npos);
    throw_guard.reset();
    dispatch_file_op_test_events();

    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline);

    // Refusals and late-binding gates changed no fingerprint or dirty bit.
    EXPECT_EQ(snapshot(d), before);
    EXPECT_EQ(d.isModifiedSinceSave(), before_dirty);
}

TEST_F(DocumentFileOperationTest, FreshReadinessAndOwnedOutputGate)
{
    auto &d = *doc;
    auto const before = snapshot(d);
    auto info = method_info(Fop::FileOperationMethod::Save);

    // A refused fresh admission must leave no slot and no *additional* lease;
    // the foreign-lease case below already holds one real lease of its own.
    auto refused_busy = [&](char const *label) {
        SCOPED_TRACE(label);
        Fop::FileOperationResult refusal;
        EXPECT_FALSE(Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &refusal));
        EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy) << refusal.detail;
        EXPECT_EQ(refusal.refusal, Fop::FileOperationRefusal::NotReady) << refusal.detail;
        EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
        EXPECT_EQ(snapshot(d), before);
    };

    {
        auto live = DocumentUndo::beginRollbackableInteraction(&d);
        ASSERT_TRUE(live);
        // The broad quiescence query is intentionally NOT a fresh-ready gate: it
        // is still true for an ordinary idle rollback token, yet a fresh GUI
        // admit must be Busy (the stricter fresh-readiness query rejects it).
        EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(&d));
        refused_busy("live rollbackable preview");
        live->rollback();
    }
    EXPECT_EQ(snapshot(d), before);

    {
        auto foreign = DocumentUndo::holdInteractionOperation(&d);
        ASSERT_TRUE(foreign);
        refused_busy("foreign operation lease");
    }
    EXPECT_EQ(snapshot(d), before);

    DocumentUndo::setUndoSensitive(&d, false);
    refused_busy("undo-insensitive");
    DocumentUndo::setUndoSensitive(&d, true);
    EXPECT_TRUE(DocumentUndo::getUndoSensitive(&d));
    EXPECT_EQ(snapshot(d), before);

    auto *rdoc = d.getReprDoc();
    ASSERT_TRUE(rdoc->inTransaction());
    rdoc->commit();
    EXPECT_FALSE(rdoc->inTransaction());
    refused_busy("inactive transaction");
    rdoc->beginTransaction();
    EXPECT_TRUE(rdoc->inTransaction());
    EXPECT_EQ(snapshot(d), before);

    // With every condition restored, the owned gate accepts exactly its request.
    auto guard = Fop::DocumentFileOperation::admit(d, info, [] { return true; });
    ASSERT_TRUE(guard);
    auto ctx = guard->context();
    auto gate = [&] { return guard->authorizeOutput(ctx).outcome; };
    EXPECT_EQ(gate(), Fop::FileOperationOutcome::Success);

    {
        auto extra = DocumentUndo::holdInteractionOperation(&d);
        ASSERT_TRUE(extra);
        EXPECT_EQ(gate(), Fop::FileOperationOutcome::Busy);
    }
    EXPECT_EQ(gate(), Fop::FileOperationOutcome::Success);

    DocumentUndo::setUndoSensitive(&d, false);
    EXPECT_EQ(gate(), Fop::FileOperationOutcome::Busy);
    DocumentUndo::setUndoSensitive(&d, true);
    EXPECT_EQ(gate(), Fop::FileOperationOutcome::Success);

    rdoc->commit();
    EXPECT_EQ(gate(), Fop::FileOperationOutcome::Busy);
    rdoc->beginTransaction();
    EXPECT_EQ(gate(), Fop::FileOperationOutcome::Success);

    guard.reset();
    dispatch_file_op_test_events();
    EXPECT_FALSE(DocumentUndo::undo(&d)); // no history was created by any probe
}

TEST_F(DocumentFileOperationTest, BindingTimeNotReadyRefusesAndReleasesSlot)
{
    auto &d = *doc;
    auto const before = snapshot(d);
    auto const baseline = app->documentOperationCount();
    Fop::FileOperationResult refusal;
    auto denied = Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::Save), [&] {
            DocumentUndo::setUndoSensitive(&d, false);
            return true;
        }, &refusal);
    EXPECT_FALSE(denied);
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy);
    EXPECT_EQ(refusal.refusal, Fop::FileOperationRefusal::NotReady);
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline);
    EXPECT_EQ(snapshot(d), before);
    DocumentUndo::setUndoSensitive(&d, true);
    auto retry = Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::Save), [] { return true; });
    EXPECT_TRUE(retry);
}

TEST_F(DocumentFileOperationTest, SaveAdmissionMessageUsesTypedRefusal)
{
    Fop::FileOperationResult refusal;
    refusal.outcome = Fop::FileOperationOutcome::Busy;
    refusal.detail = "diagnostic text can change";
    refusal.refusal = Fop::FileOperationRefusal::NotReady;
    EXPECT_EQ(save_admission_refusal_text(refusal), "Finish the current edit before saving.");
    refusal.refusal = Fop::FileOperationRefusal::None;
    EXPECT_EQ(save_admission_refusal_text(refusal), "Finish the open save operation before saving again.");
}

TEST_F(DocumentFileOperationTest, NativeCallbacksRejectAdmissionAndOutput)
{
    auto &d = *doc;
    auto *rect = find_node(d.getReprRoot(), "svg:rect");
    ASSERT_NE(rect, nullptr);
    auto info = method_info(Fop::FileOperationMethod::Save);

    // XML attribute observer: a fresh admit during a live mutation is Busy.
    bool admit_busy_in_attribute = false;
    {
        AttributeObserver observer(*rect);
        observer.action = [&] {
            Fop::FileOperationResult refusal;
            auto denied = Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &refusal);
            admit_busy_in_attribute = !denied && refusal.outcome == Fop::FileOperationOutcome::Busy;
        };
        rect->setAttribute("x", "9");
        EXPECT_TRUE(observer.fired);
    }
    EXPECT_TRUE(admit_busy_in_attribute);
    dispatch_file_op_test_events();
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));

    // connectBeforeCommit: a fresh admit while committing is Busy.
    bool admit_busy_in_commit = false;
    {
        auto before = d.connectBeforeCommit([&] {
            Fop::FileOperationResult refusal;
            auto denied = Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &refusal);
            admit_busy_in_commit = !denied && refusal.outcome == Fop::FileOperationOutcome::Busy;
        });
        rect->setAttribute("y", "13");
        DocumentUndo::done(&d, Util::Internal::ContextString("F6a native commit"), "");
        before.disconnect();
    }
    EXPECT_TRUE(admit_busy_in_commit);
    dispatch_file_op_test_events();

    // The owned request's gate is also refused inside both native callbacks.
    auto guard = Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::SaveCopy), [] { return true; });
    ASSERT_TRUE(guard);
    auto ctx = guard->context();

    bool gate_busy_in_attribute = false;
    {
        AttributeObserver observer(*rect);
        observer.action = [&] {
            gate_busy_in_attribute = guard->authorizeOutput(ctx).outcome == Fop::FileOperationOutcome::Busy;
        };
        rect->setAttribute("width", "21");
        EXPECT_TRUE(observer.fired);
    }
    EXPECT_TRUE(gate_busy_in_attribute);
    EXPECT_EQ(guard->authorizeOutput(ctx).outcome, Fop::FileOperationOutcome::Success);

    bool gate_busy_in_commit = false;
    {
        auto before = d.connectBeforeCommit([&] {
            gate_busy_in_commit = guard->authorizeOutput(ctx).outcome == Fop::FileOperationOutcome::Busy;
        });
        rect->setAttribute("height", "7");
        DocumentUndo::done(&d, Util::Internal::ContextString("F6a native commit"), "");
        before.disconnect();
    }
    EXPECT_TRUE(gate_busy_in_commit);
    EXPECT_EQ(guard->authorizeOutput(ctx).outcome, Fop::FileOperationOutcome::Success);
}

// Group B1: a close intent raised from the initial binding predicate (after the
// slot/lease reservation) is a new pending close. Admission must accept it,
// the owned gate must still authorize, a fresh admission must be Busy, and the
// document must survive bounded pumping until the guard/lease is released.
TEST_F(DocumentFileOperationTest, BindingPredicateCloseDefersUntilGuardRelease)
{
    auto &d = *doc;
    auto const baseline = app->documentOperationCount();
    auto const info = method_info(Fop::FileOperationMethod::Save);
    Fop::FileOperationResult refusal;

    int binding_calls = 0;
    auto guard = Fop::DocumentFileOperation::admit(d, info, [&]() -> bool {
        ++binding_calls;         // reservation already owns the slot/lease here
        app->document_close(&d); // real close intent; must wait on our lease
        return true;
    });
    ASSERT_TRUE(guard);
    EXPECT_EQ(binding_calls, 1);
    EXPECT_TRUE(app->documentClosePending(&d));
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline + 1);

    auto ctx = guard->context();
    ASSERT_TRUE(ctx.valid());
    auto const gate = guard->authorizeOutput(ctx);
    EXPECT_EQ(gate.outcome, Fop::FileOperationOutcome::Success) << gate.detail;
    EXPECT_EQ(binding_calls, 2); // gate re-ran the predicate; close already pending

    // Slot presence and the held operation lease, not just Busy, prove the
    // refusal is ours rather than the already-pending close.
    auto denied = Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &refusal);
    EXPECT_FALSE(denied);
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy);
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline + 1);

    // Bounded pumping while the guard owns the lease cannot finish the close.
    dispatch_file_op_test_events();
    EXPECT_NE(doc, nullptr);
    EXPECT_TRUE(app->documentClosePending(&d));
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline + 1);

    int destroys = 0;
    auto destroy_connection = d.connectDestroy([&] { ++destroys; });

    guard.reset();
    // No inline owner dispatch: closing is queued, not run from the teardown
    // path. Closing continuations are prioritized, but this test does not
    // depend on any other continuation running before the close.
    EXPECT_NE(doc, nullptr);
    EXPECT_EQ(destroys, 0);
    EXPECT_TRUE(app->documentClosePending(&d));
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline);

    dispatch_file_op_test_events();
    EXPECT_EQ(destroys, 1);
    EXPECT_EQ(doc, nullptr);
    destroy_connection.disconnect();

    // A close already pending before any reservation refuses fresh admission.
    // The unrelated operation lease is released first (without pumping), so the
    // Busy refusal is caused by the pending close alone, not a foreign lease.
    RegisteredDocument other;
    ASSERT_NE(other.doc, nullptr);
    auto hold = DocumentUndo::holdInteractionOperation(other.doc);
    ASSERT_TRUE(hold);
    app->document_close(other.doc);
    ASSERT_TRUE(app->documentClosePending(other.doc));
    hold.reset();
    ASSERT_NE(other.doc, nullptr);
    EXPECT_TRUE(app->documentClosePending(other.doc));
    EXPECT_EQ(app->documentOperationCount(), baseline);
    denied = Fop::DocumentFileOperation::admit(*other.doc, info, [] { return true; }, &refusal);
    EXPECT_FALSE(denied);
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy);
    dispatch_file_op_test_events();
    EXPECT_EQ(other.doc, nullptr);
}

// Group B2: a throwing terminal listener (std and non-std) for both latched
// outcomes. The slot and operation lease stay held for the callback, the
// latched result is never rewritten, the diagnostic is separate, duplicate
// completion is inert, and abandoning the guard invokes nothing.
TEST_F(DocumentFileOperationTest, ThrowingTerminalListenerPreservesResultAndLease)
{
    auto &d = *doc;
    auto const baseline = app->documentOperationCount();
    auto const info = method_info(Fop::FileOperationMethod::Save);

    enum class ThrowKind { Std, NonStd };
    struct NonStdTerminalError {};

    auto run_case = [&](Fop::FileOperationOutcome outcome, ThrowKind kind, char const *detail) {
        SCOPED_TRACE(std::string("outcome=") + Fop::to_string(outcome) +
                     (kind == ThrowKind::Std ? " std" : " nonstd"));
        auto guard = Fop::DocumentFileOperation::admit(d, info, [] { return true; });
        ASSERT_TRUE(guard);
        int notifications = 0;
        bool saw_slot = false;
        bool saw_lease = false;
        bool saw_terminal = false;
        bool saw_busy = false;
        Fop::FileOperationOutcome saw_outcome = Fop::FileOperationOutcome::Failed;
        std::string saw_detail;
        guard->complete({outcome, detail}, [&](Fop::FileOperationResult const &r) {
            ++notifications;
            saw_slot = Fop::DocumentFileOperation::hasActiveRequest(d);
            saw_lease = app->documentOperationCount() == baseline + 1;
            saw_terminal = guard->terminal();
            saw_outcome = r.outcome;
            saw_detail = r.detail;
            Fop::FileOperationResult nested_refusal;
            auto nested = Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &nested_refusal);
            saw_busy = !nested && nested_refusal.outcome == Fop::FileOperationOutcome::Busy;
            if (kind == ThrowKind::Std) throw std::runtime_error("terminal listener failed");
            throw NonStdTerminalError{};
        });

        EXPECT_EQ(notifications, 1);
        EXPECT_TRUE(saw_slot);
        EXPECT_TRUE(saw_lease);
        EXPECT_TRUE(saw_terminal);
        EXPECT_TRUE(saw_busy);
        EXPECT_EQ(saw_outcome, outcome);
        EXPECT_EQ(saw_detail, detail);
        ASSERT_TRUE(guard->terminalResult());
        EXPECT_EQ(guard->terminalResult()->outcome, outcome);
        EXPECT_EQ(guard->terminalResult()->detail, detail);
        EXPECT_FALSE(guard->notificationError().empty());

        // Notification failure is not a publication failure: a duplicate
        // completion neither replaces the result nor invokes the listener.
        guard->complete({Fop::FileOperationOutcome::Failed, "duplicate must not replace"},
                        [&](Fop::FileOperationResult const &) { ++notifications; });
        EXPECT_EQ(notifications, 1);
        ASSERT_TRUE(guard->terminalResult());
        EXPECT_EQ(guard->terminalResult()->outcome, outcome);
        EXPECT_EQ(guard->terminalResult()->detail, detail);

        guard.reset();
        dispatch_file_op_test_events();
        EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
        EXPECT_EQ(app->documentOperationCount(), baseline);
    };

    run_case(Fop::FileOperationOutcome::Success, ThrowKind::Std, "success std listener");
    run_case(Fop::FileOperationOutcome::Success, ThrowKind::NonStd, "success nonstd listener");
    run_case(Fop::FileOperationOutcome::Uncertain, ThrowKind::Std, "uncertain std listener");
    run_case(Fop::FileOperationOutcome::Uncertain, ThrowKind::NonStd, "uncertain nonstd listener");

    // Abandoning a guard runs no callback and releases the slot and lease.
    {
        auto abandoned = Fop::DocumentFileOperation::admit(d, info, [] { return true; });
        ASSERT_TRUE(abandoned);
        EXPECT_FALSE(abandoned->terminal());
        EXPECT_TRUE(abandoned->notificationError().empty());
        EXPECT_EQ(app->documentOperationCount(), baseline + 1);
    }
    dispatch_file_op_test_events();
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline);
}

// Group B3: the outer guard is destroyed from inside authorizeOutput's binding
// predicate. The dispatch pin must keep the slot and lease until every
// predicate capture (including a shared_ptr custom deleter) is gone.
TEST_F(DocumentFileOperationTest, GuardDestroyedInBindingPredicatePinsSlotUntilCaptureGone)
{
    auto &d = *doc;
    auto const baseline = app->documentOperationCount();
    auto const info = method_info(Fop::FileOperationMethod::Save);

    int binding_calls = 0;
    bool deleter_ran = false;
    Fop::FileOperationContext ctx;
    std::optional<Fop::FileOperationLease> guard;

    struct Probe {};
    auto probe = std::shared_ptr<Probe>(new Probe, [&, baseline](Probe *p) {
        // Runs from teardownRequest while the registry slot and the operation
        // lease are still held (before eraseSlot / lease.reset()).
        EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
        EXPECT_EQ(app->documentOperationCount(), baseline + 1);
        Fop::FileOperationResult late_refusal;
        auto late = Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &late_refusal);
        EXPECT_FALSE(late);
        EXPECT_EQ(late_refusal.outcome, Fop::FileOperationOutcome::Busy);
        deleter_ran = true;
        delete p;
    });

    std::function<bool()> binding = [&, probe, outer = &guard]() -> bool {
        ++binding_calls;
        if (binding_calls == 1) {
            return true; // initial admission: reservation already active
        }
        // Later invocation: destroy the outer guard inside its own predicate.
        EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
        EXPECT_EQ(app->documentOperationCount(), baseline + 1);
        outer->reset();
        EXPECT_FALSE(ctx.valid());
        // Immediately after reset the slot and lease are still pinned.
        EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
        EXPECT_EQ(app->documentOperationCount(), baseline + 1);
        Fop::FileOperationResult nested_refusal;
        auto nested = Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &nested_refusal);
        EXPECT_FALSE(nested);
        EXPECT_EQ(nested_refusal.outcome, Fop::FileOperationOutcome::Busy);
        // A real pending close plus nested pumping must not finish under the pin.
        app->document_close(&d);
        EXPECT_TRUE(app->documentClosePending(&d));
        dispatch_file_op_test_events();
        EXPECT_NE(doc, nullptr);
        EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
        return true;
    };

    guard = Fop::DocumentFileOperation::admit(d, info, std::move(binding));
    ASSERT_TRUE(guard);
    EXPECT_EQ(binding_calls, 1);
    EXPECT_FALSE(deleter_ran); // an external capture copy is still alive
    ctx = guard->context();
    binding = nullptr; // dispose any move-retained source before the sole-owner probe
    probe.reset(); // the stored predicate capture is now the sole owner

    auto const result = guard->authorizeOutput(ctx);
    EXPECT_EQ(result.outcome, Fop::FileOperationOutcome::StaleTarget) << result.detail;
    EXPECT_FALSE(ctx.valid());
    EXPECT_EQ(binding_calls, 2);
    EXPECT_FALSE(guard);
    EXPECT_TRUE(deleter_ran);
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline);
    EXPECT_TRUE(app->documentClosePending(&d));

    int destroys = 0;
    auto destroy_connection = d.connectDestroy([&] { ++destroys; });
    EXPECT_NE(doc, nullptr);
    dispatch_file_op_test_events();
    EXPECT_EQ(destroys, 1);
    EXPECT_EQ(doc, nullptr);
    destroy_connection.disconnect();
}

// Group B4: the outer guard is destroyed from inside the terminal listener.
// The listener observes the latched result before destruction; the pending
// close waits through the listener and the listener's final capture
// destructor, then the bounded drain completes it.
TEST_F(DocumentFileOperationTest, GuardDestroyedInTerminalListenerPinsSlotUntilCaptureGone)
{
    auto &d = *doc;
    auto const baseline = app->documentOperationCount();
    auto const info = method_info(Fop::FileOperationMethod::Save);

    int notifications = 0;
    bool deleter_ran = false;
    Fop::FileOperationContext ctx;
    std::optional<Fop::FileOperationLease> guard;
    Fop::FileOperationResult observed;

    struct Probe {};
    auto probe = std::shared_ptr<Probe>(new Probe, [&, baseline](Probe *p) {
        // The listener's capture is destroyed while complete() still holds the
        // dispatch pin: fresh admission must be Busy and the slot/lease alive.
        EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
        EXPECT_EQ(app->documentOperationCount(), baseline + 1);
        Fop::FileOperationResult late_refusal;
        auto late = Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &late_refusal);
        EXPECT_FALSE(late);
        EXPECT_EQ(late_refusal.outcome, Fop::FileOperationOutcome::Busy);
        deleter_ran = true;
        delete p;
    });

    guard = Fop::DocumentFileOperation::admit(d, info, [] { return true; });
    ASSERT_TRUE(guard);
    ctx = guard->context();
    ASSERT_TRUE(ctx.valid());

    // One direct temporary listener with the sole moved shared_ptr capture: no
    // named std::function or external probe.reset() retains a copy, so the
    // capture is destroyed under complete()'s dispatch pin.
    guard->complete({Fop::FileOperationOutcome::Success, "terminal detail"},
                    [&, probe = std::move(probe)](Fop::FileOperationResult const &r) {
                        ++notifications;
                        observed = r; // exact latched result observed before destruction
                        EXPECT_EQ(r.outcome, Fop::FileOperationOutcome::Success);
                        EXPECT_EQ(r.detail, "terminal detail");
                        EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
                        EXPECT_EQ(app->documentOperationCount(), baseline + 1);

                        app->document_close(&d);
                        EXPECT_TRUE(app->documentClosePending(&d));
                        dispatch_file_op_test_events();
                        EXPECT_NE(doc, nullptr); // pending close waits through the listener
                        EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));

                        Fop::FileOperationResult nested_refusal;
                        auto nested = Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &nested_refusal);
                        EXPECT_FALSE(nested);
                        EXPECT_EQ(nested_refusal.outcome, Fop::FileOperationOutcome::Busy);

                        guard.reset();
                        EXPECT_FALSE(ctx.valid());
                        EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
                        EXPECT_EQ(app->documentOperationCount(), baseline + 1);
                    });

    EXPECT_EQ(notifications, 1);
    EXPECT_EQ(observed.outcome, Fop::FileOperationOutcome::Success);
    EXPECT_EQ(observed.detail, "terminal detail");
    EXPECT_FALSE(guard);
    EXPECT_FALSE(ctx.valid());
    EXPECT_TRUE(deleter_ran);
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline);
    EXPECT_TRUE(app->documentClosePending(&d));

    int destroys = 0;
    auto destroy_connection = d.connectDestroy([&] { ++destroys; });
    EXPECT_NE(doc, nullptr);
    dispatch_file_op_test_events();
    EXPECT_EQ(destroys, 1);
    EXPECT_EQ(doc, nullptr);
    destroy_connection.disconnect();
}

// F6a capture-disposal lifetime (root-approved ownership regression): a small
// sole-shared_ptr listener capture must be disposed under the dispatch pin on
// BOTH the first completion and a duplicate completion. The custom deleter
// observes the exact latched result/detail, the occupied slot and the held
// lease before teardown; the duplicate listener itself must never run.
TEST_F(DocumentFileOperationTest, TerminalCaptureDisposalPinsSlot)
{
    auto &d = *doc;
    auto const info = method_info(Fop::FileOperationMethod::Save);

    // Exactly four scenarios: first/duplicate disposal x Success/Uncertain.
    for (bool duplicate_case : {false, true}) {
        for (auto const outcome :
             {Fop::FileOperationOutcome::Success, Fop::FileOperationOutcome::Uncertain}) {
            SCOPED_TRACE(std::string("duplicate=") + (duplicate_case ? "true" : "false") +
                         " outcome=" + Fop::to_string(outcome));

            // Baseline is taken BEFORE admission; +1 while held, baseline after.
            auto const baseline = app->documentOperationCount();
            auto const first_detail = std::string("first terminal ") + Fop::to_string(outcome);

            auto guard = Fop::DocumentFileOperation::admit(d, info, [] { return true; });
            ASSERT_TRUE(guard);
            EXPECT_EQ(app->documentOperationCount(), baseline + 1);
            auto ctx = guard->context();

            if (duplicate_case) {
                // Initial, first-wins completion with owned detail; no listener.
                guard->complete({outcome, first_detail});
                ASSERT_TRUE(guard->terminalResult());
                EXPECT_EQ(guard->terminalResult()->outcome, outcome);
                EXPECT_EQ(guard->terminalResult()->detail, first_detail);
                EXPECT_TRUE(guard->notificationError().empty());
            } else {
                // First mode has no terminal result before the listener-bearing call.
                EXPECT_FALSE(guard->terminalResult());
            }

            int deletions = 0;
            int notifications = 0;
            Fop::FileOperationOutcome observed_first_outcome = Fop::FileOperationOutcome::Failed;
            std::string observed_first_detail;

            struct Probe {
                int *notifications;
            };
            auto probe = std::shared_ptr<Probe>(new Probe{&notifications}, [&](Probe *p) {
                // Sole-owner capture destruction: the slot and lease must still be
                // held and the latched terminal result/detail must be intact.
                EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
                EXPECT_EQ(app->documentOperationCount(), baseline + 1);
                auto const pinned = guard->terminalResult();
                EXPECT_TRUE(pinned.has_value());
                if (pinned) {
                    EXPECT_EQ(pinned->outcome, outcome);
                    EXPECT_EQ(pinned->detail, first_detail);
                    observed_first_outcome = pinned->outcome;
                    observed_first_detail = pinned->detail;
                }
                guard.reset();
                EXPECT_FALSE(guard);
                EXPECT_FALSE(ctx.valid());
                // The dispatch pin still holds the slot and lease.
                EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
                EXPECT_EQ(app->documentOperationCount(), baseline + 1);
                Fop::FileOperationResult refusal;
                auto denied = Fop::DocumentFileOperation::admit(d, info, [] { return true; }, &refusal);
                EXPECT_FALSE(denied);
                EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy);
                ++deletions;
                delete p;
            });

            // Small closure holding only the probe. First mode latches and runs
            // the listener; duplicate mode must dispose it without invoking it.
            if (duplicate_case) {
                guard->complete({Fop::FileOperationOutcome::Failed, "duplicate must not replace"},
                                [probe = std::move(probe)](Fop::FileOperationResult const &) {
                                    ++(*probe->notifications);
                                });
            } else {
                guard->complete({outcome, first_detail},
                                [probe = std::move(probe)](Fop::FileOperationResult const &) {
                                    ++(*probe->notifications);
                                });
            }

            EXPECT_EQ(deletions, 1);
            EXPECT_EQ(notifications, duplicate_case ? 0 : 1);
            EXPECT_EQ(observed_first_outcome, outcome);
            EXPECT_EQ(observed_first_detail, first_detail);
            EXPECT_FALSE(guard);
            EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
            EXPECT_EQ(app->documentOperationCount(), baseline);
        }
    }

    EXPECT_NE(doc, nullptr);
}

// F6a-NATIVE-SAVE: the real Extension::save() writer stack holds the admitted
// SaveCopy guard while the registered test Output runs a deterministic nested
// callback. Every fresh Save/SaveAs/SaveCopy during that callback must be Busy,
// the live document must stay untouched through the write and the Undo anchor,
// and the published file must reopen with the authored width, title and href.
TEST_F(DocumentFileOperationTest, RealSaveNestedFreshRequestsBusy)
{
    ensure_extension_initialized();
    auto &d = *doc;
    deliberate_width_edit(d, "31.25"); // establishes the Undo anchor
    auto const before = snapshot(d);
    bool const dirty_before = d.isModifiedSinceSave();
    std::string const filename_before = d.getDocumentFilename() ? d.getDocumentFilename() : "";
    auto const save_as_before = Preferences::get()->getString("/dialogs/save_as/default");
    auto const save_copy_before = Preferences::get()->getString("/dialogs/save_copy/default");

    TempDirAndPreferences sandbox;
    std::string const final_path = sandbox.file("final.svg");

    auto guard = Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::SaveCopy, final_path.c_str(), "F6a nested save-copy"),
        [] { return true; });
    ASSERT_TRUE(guard);
    auto ctx = guard->context();
    ASSERT_TRUE(ctx.valid());
    auto const initial_gate = guard->authorizeOutput(ctx);
    EXPECT_EQ(initial_gate.outcome, Fop::FileOperationOutcome::Success) << initial_gate.detail;

    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
        "  <name>Nested file-operation output</name>\n"
        "  <id>org.vacards.test.file-operation-output</id>\n"
        "  <output>\n"
        "    <extension>.svg</extension>\n"
        "    <mimetype>image/svg+xml</mimetype>\n"
        "    <filetypename>Nested output</filetypename>\n"
        "    <filetypetooltip>Test-only native output</filetypetooltip>\n"
        "  </output>\n"
        "</inkscape-extension>\n";
    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.file-operation-output"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<NestedSaveOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.file-operation-output"));
    }
    ASSERT_NE(module, nullptr);
    auto *implementation = static_cast<NestedSaveOutputImplementation *>(module->get_imp());
    ASSERT_NE(implementation, nullptr);

    // The singleton implementation must not retain a document pointer or a
    // callback capture once this test returns, including on an ASSERT return.
    struct ResetImplementation {
        NestedSaveOutputImplementation *impl;
        ~ResetImplementation()
        {
            if (impl) {
                impl->original = nullptr;
                impl->nested = {};
            }
        }
    } reset_implementation{implementation};

    implementation->original = &d;
    implementation->output_calls = 0;
    implementation->saw_distinct_projection = false;

    int refusals = 0;
    int forbidden_output_attempts = 0;
    bool nested_ran = false;
    implementation->nested = [&] {
        nested_ran = true;
        for (auto method : {Fop::FileOperationMethod::Save, Fop::FileOperationMethod::SaveAs,
                            Fop::FileOperationMethod::SaveCopy}) {
            Fop::FileOperationResult refusal;
            auto denied = Fop::DocumentFileOperation::admit(d, method_info(method), [] { return true; }, &refusal);
            if (denied) {
                ++forbidden_output_attempts; // never write from the nested callback
            } else {
                EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy) << refusal.detail;
                ++refusals;
            }
        }
        // No live mutation is permitted inside the real Extension::save() stack.
        EXPECT_EQ(root_title(d), "F6a fixture");
        EXPECT_EQ(attribute_of(d, "svg:use", "xlink:href"), "#native");
        EXPECT_EQ(snapshot(d), before);
        EXPECT_EQ(d.isModifiedSinceSave(), dirty_before);
        EXPECT_EQ(d.getDocumentFilename() ? d.getDocumentFilename() : "", filename_before);
        EXPECT_EQ(Preferences::get()->getString("/dialogs/save_as/default"), save_as_before);
        EXPECT_EQ(Preferences::get()->getString("/dialogs/save_copy/default"), save_copy_before);
    };

    EXPECT_NO_THROW(Inkscape::Extension::save(module, &d, final_path.c_str(), false, false,
                                             Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY));

    EXPECT_TRUE(nested_ran);
    EXPECT_EQ(implementation->output_calls, 1);
    EXPECT_TRUE(implementation->saw_distinct_projection);
    EXPECT_EQ(refusals, 3);
    EXPECT_EQ(forbidden_output_attempts, 0);
    auto const still_authorized = guard->authorizeOutput(ctx);
    EXPECT_EQ(still_authorized.outcome, Fop::FileOperationOutcome::Success) << still_authorized.detail;

    EXPECT_TRUE(g_file_test(final_path.c_str(), G_FILE_TEST_EXISTS));
    auto reopened = SPDocument::createNewDoc(final_path.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(attribute_of(*reopened, "svg:rect", "width"), "31.25");
    EXPECT_EQ(root_title(*reopened), "F6a fixture");
    EXPECT_EQ(attribute_of(*reopened, "svg:use", "xlink:href"), "#native");

    EXPECT_EQ(snapshot(d), before);
    EXPECT_EQ(d.isModifiedSinceSave(), dirty_before);
    EXPECT_EQ(d.getDocumentFilename() ? d.getDocumentFilename() : "", filename_before);
    EXPECT_EQ(Preferences::get()->getString("/dialogs/save_as/default"), save_as_before);
    EXPECT_EQ(Preferences::get()->getString("/dialogs/save_copy/default"), save_copy_before);

    int notifications = 0;
    guard->complete({Fop::FileOperationOutcome::Success, "nested save copy complete"},
                    [&](Fop::FileOperationResult const &) { ++notifications; });
    EXPECT_EQ(notifications, 1);
    guard->complete({Fop::FileOperationOutcome::Failed, "duplicate must not notify"},
                    [&](Fop::FileOperationResult const &) { ++notifications; });
    EXPECT_EQ(notifications, 1);
    guard.reset();
    dispatch_file_op_test_events();

    EXPECT_TRUE(DocumentUndo::undo(&d));
    EXPECT_EQ(attribute_of(d, "svg:rect", "width"), "16");
    EXPECT_TRUE(DocumentUndo::redo(&d));
    EXPECT_EQ(attribute_of(d, "svg:rect", "width"), "31.25");
    EXPECT_EQ(snapshot(d), before);
}

// F6a-CLI-TEST: the core native save path is not gated by the GUI-only
// FileOperationLease admission. With a real DocumentUndo operation lease held
// on a registered document, a fresh GUI admit is Busy, yet the real
// Extension::save() of the native SVG output still publishes the file and
// reopens with the authored content while the live document stays untouched.
// This proves core compatibility under a CLI-style outer lease only; it is NOT
// command-line binary or GUI qualification.
TEST_F(DocumentFileOperationTest, CoreSaveUnderExistingLeaseBypassesGuiAdmission)
{
    ensure_extension_initialized();
    auto &d = *doc;
    deliberate_width_edit(d, "31.25");
    auto const before = snapshot(d);
    bool const dirty_before = d.isModifiedSinceSave();

    // CLI-style outer lease: the same real operation lease admission would hold.
    auto held = DocumentUndo::holdInteractionOperation(&d);
    ASSERT_TRUE(held);

    // The held foreign lease makes a fresh GUI admit Busy on the registered doc.
    Fop::FileOperationResult refusal;
    auto denied = Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::SaveCopy), [] { return true; }, &refusal);
    EXPECT_FALSE(denied);
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy) << refusal.detail;

    // Existing native SVG output module; no test-only module or seam.
    auto *native = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get(SP_MODULE_KEY_OUTPUT_SVG));
    ASSERT_NE(native, nullptr);

    TempDirAndPreferences sandbox;
    std::string const target = sandbox.file("core-save.svg");

    EXPECT_NO_THROW(Inkscape::Extension::save(native, &d, target.c_str(), false, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY));

    EXPECT_TRUE(g_file_test(target.c_str(), G_FILE_TEST_EXISTS));
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(attribute_of(*reopened, "svg:rect", "width"), "31.25");
    EXPECT_EQ(root_title(*reopened), "F6a fixture");
    EXPECT_EQ(attribute_of(*reopened, "svg:use", "xlink:href"), "#native");

    // The core writer ran on its working copy; the live document is untouched.
    EXPECT_EQ(snapshot(d), before);
    EXPECT_EQ(d.isModifiedSinceSave(), dirty_before);

    held.reset();
    dispatch_file_op_test_events();
}

// F6a-OWNER-ASYNC: the move-only FileOperationLease survives real owner-thread
// GLib main-context turns (the Save As chooser gap) without any worker. The
// retained lease still authorizes output; a read-only binding predicate that
// flips after admission rejects the later gate as StaleTarget; and releasing the
// lease after a close deferred across the gap frees the registry slot and only
// then lets the deferred close settle.
TEST_F(DocumentFileOperationTest, LeaseRetainedAcrossOwnerMainContextTurns)
{
    auto &d = *doc;
    auto const baseline = app->documentOperationCount();
    auto const before = snapshot(d);
    bool const dirty_before = d.isModifiedSinceSave();

    bool target_current = true;
    int binding_calls = 0;
    auto guard = Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::SaveAs), [&]() -> bool {
            ++binding_calls;
            return target_current; // read-only observation of the bound target
        });
    ASSERT_TRUE(guard);
    auto ctx = guard->context();
    ASSERT_TRUE(ctx.valid());
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline + 1);
    EXPECT_EQ(binding_calls, 1);

    // Two genuine owner-thread turns: the async chooser callback would run here.
    EXPECT_TRUE(pump_owner_main_context_turn());
    EXPECT_TRUE(pump_owner_main_context_turn());
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline + 1);
    EXPECT_TRUE(ctx.valid());

    auto const authorized = guard->authorizeOutput(ctx);
    EXPECT_EQ(authorized.outcome, Fop::FileOperationOutcome::Success) << authorized.detail;
    EXPECT_EQ(binding_calls, 2); // the gate re-read the predicate after the gap
    EXPECT_EQ(snapshot(d), before);
    EXPECT_EQ(d.isModifiedSinceSave(), dirty_before);

    // Flip the read-only predicate (a window/tab retarget) after admission and
    // across further owner-thread turns: the exact request must not retarget.
    target_current = false;
    EXPECT_TRUE(pump_owner_main_context_turn());
    EXPECT_TRUE(pump_owner_main_context_turn());
    auto const stale = guard->authorizeOutput(ctx);
    EXPECT_EQ(stale.outcome, Fop::FileOperationOutcome::StaleTarget) << stale.detail;
    EXPECT_EQ(binding_calls, 3);
    EXPECT_EQ(snapshot(d), before);
    EXPECT_EQ(d.isModifiedSinceSave(), dirty_before);
    EXPECT_FALSE(guard->terminal());

    // A close raised after the async gap is deferred by the retained lease, not
    // aborted, and cannot settle while the lease is held.
    int destroys = 0;
    auto destroy_connection = d.connectDestroy([&] { ++destroys; });
    app->document_close(&d);
    ASSERT_TRUE(app->documentClosePending(&d));
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline + 1);
    EXPECT_NE(doc, nullptr);
    dispatch_file_op_test_events();
    EXPECT_EQ(destroys, 0);
    EXPECT_NE(doc, nullptr);
    EXPECT_TRUE(app->documentClosePending(&d));
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline + 1);

    // Release: the slot is erased immediately, the lease is dropped, and only
    // then can the deferred close run.
    guard.reset();
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_EQ(app->documentOperationCount(), baseline);
    EXPECT_TRUE(app->documentClosePending(&d));
    EXPECT_NE(doc, nullptr);
    EXPECT_EQ(destroys, 0);

    dispatch_file_op_test_events();
    EXPECT_EQ(destroys, 1);
    EXPECT_EQ(doc, nullptr);
    destroy_connection.disconnect();
}

TEST_F(DocumentFileOperationTest, PublicationLeasePermitsEditingButBlocksFreshAdmission)
{
    auto &d = *doc;
    auto guard = Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::SaveAs), [] { return true; });
    ASSERT_TRUE(guard);
    EXPECT_FALSE(DocumentUndo::fileOperationFreshReady(&d));
    guard->enterPublication();
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_FALSE(DocumentUndo::fileOperationFreshReady(&d)); // autosave gate too
    Fop::FileOperationResult refusal;
    EXPECT_FALSE(Fop::DocumentFileOperation::admit(
        d, method_info(Fop::FileOperationMethod::Save), [] { return true; }, &refusal));
    EXPECT_EQ(refusal.outcome, Fop::FileOperationOutcome::Busy);
    EXPECT_TRUE(DocumentUndo::publicationCompletable(&d));

    auto interaction = DocumentUndo::beginRollbackableInteraction(&d);
    ASSERT_TRUE(interaction);
    find_node(d.getReprRoot(), "svg:rect")->setAttribute("width", "41");
    interaction->commit(Util::Internal::ContextString("P4a edit"), "");
    EXPECT_EQ(attribute_of(d, "svg:rect", "width"), "41");
    EXPECT_FALSE(DocumentUndo::fileOperationFreshReady(&d));
    guard->complete({Fop::FileOperationOutcome::Success});
    guard.reset();
    EXPECT_FALSE(Fop::DocumentFileOperation::hasActiveRequest(d));
    EXPECT_TRUE(DocumentUndo::fileOperationFreshReady(&d));
}

TEST_F(DocumentFileOperationTest, PublicationCompletionWaitsForInteractionAndClearsOnClose)
{
    auto &d = *doc;
    int calls = 0;
    bool released_before_destroy = false;
    bool destroyed = false;
    auto destroyed_connection = d.connectDestroy([&] { destroyed = true; });
    DocumentUndo::whenPublicationCompletable(&d, [&](SPDocument &) { ++calls; }, [&](SPDocument &) { ++calls; });
    EXPECT_EQ(calls, 1);

    auto interaction = DocumentUndo::beginRollbackableInteraction(&d);
    ASSERT_TRUE(interaction);
    DocumentUndo::whenPublicationCompletable(&d, [&](SPDocument &) { ++calls; }, [&](SPDocument &) { ++calls; });
    dispatch_file_op_test_events();
    EXPECT_EQ(calls, 1);
    interaction->rollback();
    dispatch_file_op_test_events();
    EXPECT_EQ(calls, 2);

    interaction = DocumentUndo::beginRollbackableInteraction(&d);
    ASSERT_TRUE(interaction);
    DocumentUndo::whenPublicationCompletable(&d, [&](SPDocument &) { ++calls; },
        [&](SPDocument &) { ++calls; });
    find_node(d.getReprRoot(), "svg:rect")->setAttribute("width", "42");
    interaction->commit(Util::Internal::ContextString("P4a edit"), "");
    dispatch_file_op_test_events();
    EXPECT_EQ(calls, 3);

    interaction = DocumentUndo::beginRollbackableInteraction(&d);
    ASSERT_TRUE(interaction);
    auto capture = std::shared_ptr<int>(new int, [&](int *value) {
        released_before_destroy = !destroyed;
        delete value;
    });
    DocumentUndo::whenPublicationCompletable(&d, [&, capture = std::move(capture)](SPDocument &) { ++calls; },
        [&](SPDocument &) { ++calls; });
    app->document_close(&d);
    interaction->rollback();
    dispatch_file_op_test_events();
    EXPECT_EQ(calls, 4); // Close forces delivery before document destruction.
    EXPECT_TRUE(released_before_destroy);
    EXPECT_EQ(doc, nullptr);
    destroyed_connection.disconnect();
}

TEST_F(DocumentFileOperationTest, CancelCleanupDrainsQueuedPublicationWithoutIdleSpin)
{
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    auto token = std::make_shared<DocumentUndo::RollbackableInteraction>(std::move(*interaction));
    int completed = 0;
    int cleaned = 0;
    DocumentUndo::whenPublicationCompletable(doc, [&](SPDocument &) { ++completed; },
        [&](SPDocument &) { ++completed; });
    DocumentUndo::deferInteractionCleanup(doc, [token, &cleaned](SPDocument &) {
        token->rollback();
        ++cleaned;
    });
    for (int i = 0; i < 128 && (!completed || !cleaned); ++i)
        g_main_context_iteration(nullptr, false);
    EXPECT_EQ(cleaned, 1);
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(pump_owner_main_context_turn()); // A HIGH_IDLE re-arm must not starve this idle.
}

TEST_F(DocumentFileOperationTest, CloseDrainsFailedAndUncertainBeforeDestroy)
{
    ensure_extension_initialized();
    TempDirAndPreferences sandbox;
    auto *native = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get(SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE));
    ASSERT_NE(native, nullptr);
    for (auto outcome : {Pub::PublicationOutcome::Failed, Pub::PublicationOutcome::Uncertain}) {
        auto *current = doc;
        auto const target = sandbox.file(outcome == Pub::PublicationOutcome::Failed
            ? "close-failed.svg" : "close-uncertain.svg");
        deliberate_width_edit(*current, "19");
        Inkscape::Extension::save(native, current, target.c_str(), false, true,
            Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false);
        ASSERT_TRUE(current->get_event_log()->hasFileSaveAnchor());
        auto pending = Inkscape::Extension::begin_save_async(native, current, target.c_str(),
            false, true, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true);
        ASSERT_TRUE(pending);
        auto shared_pending = std::shared_ptr<Inkscape::Extension::PendingSave>(std::move(pending));
        auto interaction = DocumentUndo::beginRollbackableInteraction(current);
        ASSERT_TRUE(interaction);
        find_node(current->getReprRoot(), "svg:rect")->setAttribute("width", "41");
        bool delivered = false, dirty_at_destroy = false, anchor_gone_at_destroy = false;
        std::string width_at_destroy;
        auto connection = current->connectDestroy([&] {
            dirty_at_destroy = current->isModifiedSinceSave();
            anchor_gone_at_destroy = !current->get_event_log()->hasFileSaveAnchor();
            width_at_destroy = attribute_of(*current, "svg:rect", "width");
        });
        Pub::PublicationResult result;
        result.outcome = outcome;
        DocumentUndo::whenPublicationCompletable(current, [&, shared_pending](SPDocument &) {
            try { Inkscape::Extension::finish_save_async(*shared_pending, result, nullptr, false); }
            catch (Inkscape::Extension::Output::save_failed const &) { delivered = true; }
        }, [&](SPDocument &) { delivered = true; });
        app->document_close(current);
        dispatch_file_op_test_events();
        EXPECT_TRUE(delivered);
        EXPECT_TRUE(dirty_at_destroy);
        EXPECT_TRUE(anchor_gone_at_destroy);
        EXPECT_EQ(width_at_destroy, "19"); // Preview must have rolled back before completion.
        EXPECT_EQ(doc, nullptr);
        connection.disconnect();
        if (outcome == Pub::PublicationOutcome::Failed) {
            doc = app->document_add(document(kFixtureSvg));
            ASSERT_NE(doc, nullptr);
            destroy_connection = doc->connectDestroy([this] { doc = nullptr; });
        }
    }
}

TEST_F(DocumentFileOperationTest, DeferredCloseReportsOutcomesQueuedAfterCloseRequest)
{
    auto operation = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(operation);
    std::vector<Pub::PublicationOutcome> reported;
    bool reported_before_destroy = false;
    auto connection = doc->connectDestroy([&] { reported_before_destroy = reported.size() == 2; });
    app->document_close(doc);
    ASSERT_TRUE(app->documentClosePending(doc));
    for (auto outcome : {Pub::PublicationOutcome::Failed, Pub::PublicationOutcome::Uncertain}) {
        DocumentUndo::whenPublicationCompletable(doc, [&, outcome](SPDocument &) {
            reported.push_back(outcome);
        }, [&, outcome](SPDocument &) { reported.push_back(outcome); });
    }
    operation.reset();
    dispatch_file_op_test_events();
    EXPECT_EQ(reported, (std::vector<Pub::PublicationOutcome>{
        Pub::PublicationOutcome::Failed, Pub::PublicationOutcome::Uncertain}));
    EXPECT_TRUE(reported_before_destroy);
    EXPECT_EQ(doc, nullptr);
    connection.disconnect();
}

TEST(P4aDocumentClosing, ReportsOnlyAndReleasesCaptureBeforeDestroySignal)
{
    if (!Application::exists()) Application::create(false);
    auto local = document(kFixtureSvg);
    bool destroyed = false;
    bool released_before_destroy = false;
    bool reported_clean = false, full_called = false;
    auto connection = local->connectDestroy([&] { destroyed = true; });
    auto interaction = DocumentUndo::beginRollbackableInteraction(local.get());
    ASSERT_TRUE(interaction);
    auto capture = std::shared_ptr<int>(new int, [&](int *value) {
        released_before_destroy = !destroyed;
        delete value;
    });
    DocumentUndo::whenPublicationCompletable(local.get(),
        [capture = std::move(capture), &full_called](SPDocument &) { full_called = true; },
        [&](SPDocument &d) { reported_clean = !d.isModifiedSinceSave(); });
    local.reset();
    EXPECT_TRUE(reported_clean);
    EXPECT_FALSE(full_called);
    EXPECT_TRUE(released_before_destroy);
    EXPECT_TRUE(destroyed);
    connection.disconnect();
}

TEST(P4aDocumentClosing, MissingReporterWarnsBeforeDestroy)
{
    if (!Application::exists()) Application::create(false);
    auto local = document(kFixtureSvg);
    auto interaction = DocumentUndo::beginRollbackableInteraction(local.get());
    ASSERT_TRUE(interaction);
    DocumentUndo::whenPublicationCompletable(local.get(), [](SPDocument &) {}, {});
    testing::internal::CaptureStderr();
    local.reset();
    auto const warning = testing::internal::GetCapturedStderr();
    EXPECT_NE(warning.find("Publication outcome could not be applied safely before close"),
              std::string::npos);
}

TEST_F(DocumentFileOperationTest, StaleInteractionCloseTerminates)
{
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    int full = 0, report_only = 0;
    DocumentUndo::whenPublicationCompletable(doc,
        [&](SPDocument &) { ++full; }, [&](SPDocument &) { ++report_only; });
    app->document_close(doc);
    ASSERT_TRUE(app->documentClosePending(doc));
    interaction.reset(); // Close-request rollback returns early and destroys the state.
    for (int turn = 0; turn < 50 && doc; ++turn)
        g_main_context_iteration(nullptr, false);
    EXPECT_EQ(doc, nullptr);
    EXPECT_EQ(full, 0);
    EXPECT_EQ(report_only, 1);
}

// U1: DocumentUndo::clearUndo (clipped-bitmap conversion while opening a CorelDRAW file) must clear the Undo
// History rows too, and seeking must never mark a document that holds changes as clean.
namespace {
std::size_t history_row_count(Inkscape::EventLog &log)
{
    std::size_t rows = 0;
    for (auto const &parent : log.getEventListStore()->children()) {
        ++rows;
        rows += parent.children().size();
    }
    return rows;
}
char const *fixture_width(SPDocument &doc) { return doc.getObjectById("native")->getAttribute("width"); }
} // namespace

TEST_F(DocumentFileOperationTest, U1ClearUndoClearsHistoryRowsAndKeepsDocumentModified)
{
    auto *log = doc->get_event_log();
    ASSERT_FALSE(doc->isModifiedSinceSave());
    ASSERT_TRUE(log->hasFileSaveAnchor()); // the pristine start row is the saved state
    deliberate_width_edit(*doc, "41");
    ASSERT_TRUE(doc->isModifiedSinceSave());
    ASSERT_EQ(history_row_count(*log), 2u);

    DocumentUndo::clearUndo(doc);

    EXPECT_EQ(history_row_count(*log), 1u) << "Undo History still lists steps the document no longer has";
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(doc));
    // Seeking to the start row (a stale dialog selection) must not report the changed document as clean.
    log->seekTo(log->getEventListStore()->children().begin());
    EXPECT_STREQ(fixture_width(*doc), "41");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_FALSE(log->hasFileSaveAnchor());
}

TEST_F(DocumentFileOperationTest, U1ClearUndoKeepsRedoRowsAndSeekStaysConsistent)
{
    auto *log = doc->get_event_log();
    deliberate_width_edit(*doc, "41");
    deliberate_width_edit(*doc, "42");
    deliberate_width_edit(*doc, "43");
    ASSERT_TRUE(DocumentUndo::undo(doc)); // width 42, one redo step left
    ASSERT_EQ(history_row_count(*log), 4u);

    DocumentUndo::clearUndo(doc);

    ASSERT_EQ(history_row_count(*log), 2u); // start row + the one redo step
    EXPECT_EQ(log->getCurrEvent(), log->getEventListStore()->children().begin());
    EXPECT_TRUE(doc->isModifiedSinceSave());
    auto redo_row = log->getEventListStore()->children().begin();
    ++redo_row;
    log->seekTo(redo_row);
    EXPECT_STREQ(fixture_width(*doc), "43");
    EXPECT_EQ(log->getCurrEvent(), redo_row);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    log->seekTo(log->getEventListStore()->children().begin());
    EXPECT_STREQ(fixture_width(*doc), "42"); // the state at the moment of the clear
    EXPECT_EQ(log->getCurrEvent(), log->getEventListStore()->children().begin());
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(doc));
}

TEST_F(DocumentFileOperationTest, U1ClearUndoOnCleanDocumentStaysCleanAtStart)
{
    auto *log = doc->get_event_log();
    deliberate_width_edit(*doc, "41");
    log->rememberFileSave(log->getCurrEventSerial());
    doc->setModifiedSinceSave(false);

    DocumentUndo::clearUndo(doc);

    EXPECT_EQ(history_row_count(*log), 1u);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_TRUE(log->hasFileSaveAnchor());
    deliberate_width_edit(*doc, "44");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    log->seekTo(log->getEventListStore()->children().begin());
    EXPECT_STREQ(fixture_width(*doc), "41");
    EXPECT_FALSE(doc->isModifiedSinceSave());
}

// Template::new_from_template marks the document clean and then drops the history (same order as the fix in
// extension/template.cpp): edit -> undo back to the start must be clean again.
TEST_F(DocumentFileOperationTest, U1TemplateOrderKeepsStartRowClean)
{
    auto *log = doc->get_event_log();
    deliberate_width_edit(*doc, "41"); // the document is dirty with history, as a freshly built template can be
    ASSERT_TRUE(doc->isModifiedSinceSave());
    doc->setModifiedSinceSave(false);
    DocumentUndo::clearUndo(doc);
    EXPECT_EQ(history_row_count(*log), 1u);
    EXPECT_TRUE(log->hasFileSaveAnchor());
    deliberate_width_edit(*doc, "42");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(doc));
    EXPECT_STREQ(fixture_width(*doc), "41");
    EXPECT_FALSE(doc->isModifiedSinceSave()) << "undoing back to the new-from-template state must be clean";
}

TEST_F(DocumentFileOperationTest, U1SeekStopsWhenDocumentCannotUndo)
{
    auto *log = doc->get_event_log();
    deliberate_width_edit(*doc, "41");
    deliberate_width_edit(*doc, "42");
    // A live interaction makes Undo refuse; the rows must not run ahead of the document.
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    auto const before = log->getCurrEvent();
    log->seekTo(log->getEventListStore()->children().begin());
    EXPECT_EQ(log->getCurrEvent(), before);
    EXPECT_STREQ(fixture_width(*doc), "42");
}

// M1: the Undo history must be bounded on a default install (limit preference never written) and by memory, not
// only by step count: one bitmap edit keeps a whole base64 image in the log.
namespace {
struct UndoPrefsScope {
    struct Saved { std::string path; bool had; int value; };
    std::vector<Saved> saved;
    Preferences *prefs = Preferences::get();
    bool limit_had = false; bool limit_value = false;
    UndoPrefsScope()
    {
        auto const limit = prefs->getEntry("/options/undo/limit");
        limit_had = limit.isSet(); limit_value = limit_had && limit.getBool();
        for (char const *path : {"/options/undo/size", "/options/undo/max-mb"}) {
            auto const entry = prefs->getEntry(path);
            saved.push_back({path, entry.isSet(), entry.isSet() ? entry.getInt() : 0});
        }
        prefs->remove("/options/undo/limit"); // default-install state
        prefs->remove("/options/undo/size");
        prefs->remove("/options/undo/max-mb");
    }
    ~UndoPrefsScope()
    {
        if (limit_had) prefs->setBool("/options/undo/limit", limit_value); else prefs->remove("/options/undo/limit");
        for (auto const &s : saved) { if (s.had) prefs->setInt(s.path, s.value); else prefs->remove(s.path); }
    }
};
void commit_attribute(SPDocument &doc, std::string const &name, std::string const &value)
{
    auto *rect = find_node(doc.getReprRoot(), "svg:rect");
    if (!rect) throw std::runtime_error("fixture rect");
    rect->setAttribute(name.c_str(), value.c_str());
    doc.ensureUpToDate();
    DocumentUndo::done(&doc, Util::Internal::ContextString("M1 step"), "");
}
std::size_t undo_all(SPDocument &doc)
{
    std::size_t n = 0;
    while (DocumentUndo::undo(&doc)) ++n;
    return n;
}
} // namespace

TEST_F(DocumentFileOperationTest, M1DefaultInstallTrimsHistoryByCount)
{
    UndoPrefsScope scope; // "/options/undo/limit" is unset, as on a fresh install
    Preferences::get()->setInt("/options/undo/size", 3);
    for (int i = 1; i <= 6; ++i) commit_attribute(*doc, "data-m1-" + std::to_string(i), std::to_string(i));
    auto *log = doc->get_event_log();
    EXPECT_EQ(history_row_count(*log), 1u + 3u);
    EXPECT_EQ(undo_all(*doc), 3u) << "an unset limit preference must still cap the history";
    auto *rect = find_node(doc->getReprRoot(), "svg:rect");
    EXPECT_STREQ(rect->attribute("data-m1-3"), "3"); // steps older than the cap were forgotten, not undone
    EXPECT_EQ(rect->attribute("data-m1-4"), nullptr);
}

TEST_F(DocumentFileOperationTest, M1ExplicitlyDisabledLimitKeepsEverything)
{
    UndoPrefsScope scope;
    Preferences::get()->setBool("/options/undo/limit", false);
    Preferences::get()->setInt("/options/undo/size", 3);
    for (int i = 1; i <= 6; ++i) commit_attribute(*doc, "data-m1-" + std::to_string(i), std::to_string(i));
    EXPECT_EQ(undo_all(*doc), 6u);
}

TEST_F(DocumentFileOperationTest, M1ByteBudgetDropsOldestStepsAndKeepsNewest)
{
    UndoPrefsScope scope;
    Preferences::get()->setInt("/options/undo/max-mb", 25);
    std::string const big(10 * 1024 * 1024, 'x');
    for (int i = 1; i <= 5; ++i) commit_attribute(*doc, "data-m1-" + std::to_string(i), big + std::to_string(i));
    auto *log = doc->get_event_log();
    auto *rect = find_node(doc->getReprRoot(), "svg:rect");

    // 10 MB per step, 25 MB budget: the two newest steps survive and the newest is never dropped.
    EXPECT_EQ(history_row_count(*log), 1u + 2u);
    ASSERT_NE(rect->attribute("data-m1-5"), nullptr);
    EXPECT_EQ(std::string(rect->attribute("data-m1-5")).back(), '5');

    EXPECT_EQ(undo_all(*doc), 2u);
    EXPECT_EQ(rect->attribute("data-m1-5"), nullptr);
    EXPECT_EQ(rect->attribute("data-m1-4"), nullptr);
    ASSERT_NE(rect->attribute("data-m1-3"), nullptr); // forgotten steps keep their effect
    EXPECT_EQ(std::string(rect->attribute("data-m1-3")).back(), '3');
    EXPECT_EQ(log->getCurrEvent(), log->getEventListStore()->children().begin());

    // Redo restores exactly the surviving steps, with the right values.
    ASSERT_TRUE(DocumentUndo::redo(doc));
    ASSERT_TRUE(DocumentUndo::redo(doc));
    EXPECT_FALSE(DocumentUndo::redo(doc));
    ASSERT_NE(rect->attribute("data-m1-4"), nullptr);
    ASSERT_NE(rect->attribute("data-m1-5"), nullptr);
    EXPECT_EQ(std::string(rect->attribute("data-m1-4")).back(), '4');
    EXPECT_EQ(std::string(rect->attribute("data-m1-5")).back(), '5');
    EXPECT_EQ(history_row_count(*log), 1u + 2u);
}

TEST_F(DocumentFileOperationTest, M1OversizedSingleStepIsKept)
{
    UndoPrefsScope scope;
    Preferences::get()->setInt("/options/undo/max-mb", 1);
    commit_attribute(*doc, "data-m1-big", std::string(3 * 1024 * 1024, 'y'));
    EXPECT_EQ(undo_all(*doc), 1u); // the step just committed is never dropped, even over budget
}

TEST_F(DocumentFileOperationTest, StaleFailedCloseReportsQueuedResultAndResets)
{
    deliberate_width_edit(*doc, "57");
    doc->get_event_log()->rememberFileSave(doc->get_event_log()->getCurrEventSerial());
    ASSERT_TRUE(doc->get_event_log()->hasFileSaveAnchor());
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    int full = 0, reported = 0;
    DocumentUndo::whenPublicationCompletable(doc,
        [&](SPDocument &) { ++full; }, [&](SPDocument &) { ++reported; });
    ASSERT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(doc,
        [&](SPDocument &d) {
            DocumentUndo::clearStaleInteractionForClose(&d);
            d.setModifiedSinceSave(false); // Undo returned to the old saved row while X1 published.
            throw std::runtime_error("injected stale close failure");
        }, true));
    interaction.reset();
    dispatch_file_op_test_events();
    EXPECT_EQ(full, 0);
    EXPECT_EQ(reported, 1);
    EXPECT_FALSE(DocumentUndo::publicationPending(doc));
    EXPECT_FALSE(doc->get_event_log()->hasFileSaveAnchor());
    EXPECT_TRUE(doc->isModifiedSinceSave());
    bool next_applied = false;
    DocumentUndo::whenPublicationCompletable(doc,
        [&](SPDocument &) { next_applied = true; }, [&](SPDocument &) {});
    dispatch_file_op_test_events();
    EXPECT_TRUE(next_applied);
}

TEST_F(DocumentFileOperationTest, P4dFinishedQueuedPublicationDoesNotDirtyOnStaleClose)
{
    deliberate_width_edit(*doc, "59");
    doc->get_event_log()->rememberFileSave(doc->get_event_log()->getCurrEventSerial());
    doc->setModifiedSinceSave(false);
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    int full = 0, reported = 0;
    bool finished = false;
    DocumentUndo::whenPublicationCompletable(doc,
        [&](SPDocument &) { ++full; }, [&](SPDocument &) { ++reported; },
        [&] { return finished; });
    ASSERT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(doc,
        [&](SPDocument &d) {
            DocumentUndo::clearStaleInteractionForClose(&d);
            finished = true;
            d.setModifiedSinceSave(false);
            throw std::runtime_error("injected stale close failure");
        }, true));
    interaction.reset();
    dispatch_file_op_test_events();
    EXPECT_EQ(full, 0);
    EXPECT_EQ(reported, 1);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_TRUE(doc->get_event_log()->hasFileSaveAnchor());
}

TEST_F(DocumentFileOperationTest, StaleCloseAlreadyClosingKeepsQueuedPublication)
{
    deliberate_width_edit(*doc, "58");
    doc->get_event_log()->rememberFileSave(doc->get_event_log()->getCurrEventSerial());
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    std::shared_ptr<void> blocking_close;
    int full = 0, reported = 0;
    DocumentUndo::whenPublicationCompletable(doc,
        [&](SPDocument &) { ++full; }, [&](SPDocument &) { ++reported; });
    ASSERT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(doc,
        [&](SPDocument &d) {
            DocumentUndo::clearStaleInteractionForClose(&d);
            blocking_close = DocumentUndo::holdInteractionOperation(&d);
            app->document_close(&d); // A second close is pending when this continuation returns.
        }, true));
    interaction.reset();
    dispatch_file_op_test_events();
    EXPECT_EQ(full, 0);
    EXPECT_EQ(reported, 0);
    ASSERT_TRUE(app->documentClosePending(doc));
    EXPECT_TRUE(doc->get_event_log()->hasFileSaveAnchor());
    blocking_close.reset();
    dispatch_file_op_test_events();
    EXPECT_EQ(doc, nullptr);
}

TEST_F(DocumentFileOperationTest, ThrowingRealCloseContinuationRefreshesHold)
{
    Fop::enable_file_io_test_hooks();
    auto operation = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(operation);
    app->failNextDeferredDocumentCloseForTesting();
    app->document_close(doc);
    ASSERT_TRUE(app->documentClosePending(doc));
    auto const refreshes = app->documentHoldRefreshesForTesting();
    operation.reset();
    dispatch_file_op_test_events();
    EXPECT_NE(doc, nullptr);
    EXPECT_FALSE(app->documentClosePending(doc));
    EXPECT_GT(app->documentHoldRefreshesForTesting(), refreshes);
    Fop::reset_file_io_test_hooks_for_testing();
}

TEST_F(DocumentFileOperationTest, ThrowingCloseContinuationReleasesAdmission)
{
    auto operation = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(operation);
    ASSERT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(doc,
        [](SPDocument &) { throw std::runtime_error("injected close failure"); }, true));
    operation.reset();
    dispatch_file_op_test_events();
    EXPECT_NE(doc, nullptr);
    EXPECT_FALSE(DocumentUndo::interactionCloseRequested(doc));
    auto save = Fop::DocumentFileOperation::admit(
        *doc, method_info(Fop::FileOperationMethod::Save), [] { return true; });
    EXPECT_TRUE(save);
}

TEST_F(DocumentFileOperationTest, NoDeferStillWaitsForOwner)
{
    auto operation = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(operation);
    int calls = 0;
    EXPECT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(doc,
        [&](SPDocument &) { ++calls; }, true, true));
    EXPECT_EQ(calls, 0);
    operation.reset();
    dispatch_file_op_test_events();
    EXPECT_EQ(calls, 1);
}

TEST_F(DocumentFileOperationTest, QueuedPublishedDrainsClean)
{
    ensure_extension_initialized();
    TempDirAndPreferences sandbox;
    auto const target = sandbox.file("queued-published.svg");
    auto *native = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get(SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE));
    ASSERT_NE(native, nullptr);
    auto pending = Inkscape::Extension::begin_save_async(native, doc, target.c_str(),
        false, true, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true);
    ASSERT_TRUE(pending);
    Pub::PublicationResult result;
    auto job = Inkscape::Extension::take_publication_job(*pending, &result);
    ASSERT_TRUE(job);
    result = Pub::publish(std::move(*job));
    ASSERT_EQ(result.outcome, Pub::PublicationOutcome::Published);
    auto operation = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(operation);
    bool delivered = false, reported = false;
    DocumentUndo::whenPublicationCompletable(doc, [&](SPDocument &) {
        Inkscape::Extension::finish_save_async(*pending, result, nullptr, false);
        delivered = true;
    }, [&](SPDocument &) { reported = true; });
    EXPECT_TRUE(DocumentUndo::publicationPending(doc));
    operation.reset();
    dispatch_file_op_test_events();
    EXPECT_TRUE(delivered);
    EXPECT_FALSE(reported);
    EXPECT_FALSE(DocumentUndo::publicationPending(doc));
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_TRUE(doc->get_event_log()->hasFileSaveAnchor());
}

TEST_F(DocumentFileOperationTest, PendingResultKeepsPrompt)
{
    ensure_extension_initialized();
    Fop::enable_file_io_test_hooks();
    struct ResetHooks {
        ~ResetHooks() {
            set_document_check_context_for_testing(nullptr, nullptr);
            Fop::reset_file_io_test_hooks_for_testing();
        }
    } reset_hooks;
    Gtk::Window window;
    set_document_check_context_for_testing(doc, &window);
    TempDirAndPreferences sandbox;
    auto const target = sandbox.file("queued-failed.svg");
    auto *native = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get(SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE));
    ASSERT_NE(native, nullptr);
    auto pending = Inkscape::Extension::begin_save_async(native, doc, target.c_str(),
        false, true, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true);
    ASSERT_TRUE(pending);
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    Pub::PublicationResult result;
    result.outcome = Pub::PublicationOutcome::Failed;
    find_node(doc->getReprRoot(), "svg:rect")->setAttribute("width", "41");
    bool reported = false;
    DocumentUndo::whenPublicationCompletable(doc, [&](SPDocument &) {
        try { Inkscape::Extension::finish_save_async(*pending, result, nullptr, false); }
        catch (Inkscape::Extension::Output::save_failed const &) { reported = true; }
    }, [&](SPDocument &) { reported = true; });
    interaction->rollback();
    auto operation = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(operation);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_TRUE(DocumentUndo::publicationPending(doc));
    EXPECT_FALSE(DocumentUndo::publicationCompletable(doc));
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    EXPECT_TRUE(document_check_for_data_loss(nullptr));
    operation.reset();
    dispatch_file_op_test_events();
    EXPECT_TRUE(reported);
}

TEST_F(DocumentFileOperationTest, ThrowingContinuationDoesNotStall)
{
    auto operation = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(operation);
    bool second_ran = false;
    DocumentUndo::whenPublicationCompletable(doc,
        [](SPDocument &) { throw std::runtime_error("injected continuation failure"); },
        [](SPDocument &) {});
    DocumentUndo::whenPublicationCompletable(doc,
        [&](SPDocument &) { second_ran = true; }, [](SPDocument &) {});
    operation.reset();
    dispatch_file_op_test_events();
    EXPECT_TRUE(second_ran);
}

TEST_F(DocumentFileOperationTest, ThrowingOwnerAndCleanupDoNotStall)
{
    auto operation = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(operation);
    int owner_calls = 0, cleanup_calls = 0;
    DocumentUndo::deferUntilInteractionQuiescent(doc,
        [](SPDocument &) { throw std::runtime_error("owner failure"); });
    DocumentUndo::deferUntilInteractionQuiescent(doc,
        [&](SPDocument &) { ++owner_calls; });
    DocumentUndo::deferInteractionCleanup(doc,
        [](SPDocument &) { throw std::runtime_error("cleanup failure"); });
    DocumentUndo::deferInteractionCleanup(doc,
        [&](SPDocument &) { ++cleanup_calls; });
    operation.reset();
    dispatch_file_op_test_events();
    EXPECT_EQ(owner_calls, 1);
    EXPECT_EQ(cleanup_calls, 1);
}

TEST(P4aQuit, ReentrantDocumentCloseCompletesQuit)
{
    if (!Application::exists()) Application::create(false);
    auto *app = &file_op_test_application();
    TempDirAndPreferences sandbox;
    auto *one = app->document_add(document(kFixtureSvg));
    auto *two = app->document_add(document(kFixtureSvg));
    ASSERT_NE(one, nullptr);
    ASSERT_NE(two, nullptr);
    auto documents = app->get_documents();
    ASSERT_EQ(documents.size(), 2);
    int completed = 0;
    bool destroyed_after_completion = false;
    auto connection = documents[0]->connectDestroy([&] {
        destroyed_after_completion = completed == 1;
    });
    auto job = Pub::PublicationJob{};
    job.absolute_path = sandbox.file("quit-wait.svg");
    job.bytes = {std::byte{'x'}};
    ASSERT_EQ(app->publications().start(documents[0], std::move(job), [&](auto) {
        ++completed;
        app->document_close(documents[1]);
    }), Fop::PublicationWorkerRegistry::StartResult::Started);
    app->on_quit_immediate();
    EXPECT_TRUE(pump_save_wait([&] { return app->get_documents().empty(); }));
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(destroyed_after_completion);
    EXPECT_TRUE(app->get_documents().empty());
    connection.disconnect();
    delete app;
}

TEST_F(DocumentFileOperationTest, PublicationWaitIsLazyAndDeliversOnce)
{
    Fop::enable_file_io_test_hooks();
    struct ResetHooks {
        ~ResetHooks() {
            set_document_check_context_for_testing(nullptr, nullptr);
            Fop::reset_file_io_test_hooks_for_testing();
        }
    } reset_hooks;
    Gtk::Window window;
    set_document_check_context_for_testing(doc, &window);
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    bool const had_registry = app->hasPublicationRegistry();
    EXPECT_FALSE(app->publicationWaitActive(doc));
    EXPECT_FALSE(app->waitForPublication(doc));
    EXPECT_EQ(app->hasPublicationRegistry(), had_registry);
    int calls = 0;
    TempDirAndPreferences sandbox;
    auto &registry = app->publications();
    auto job = Inkscape::Extension::Internal::PublicationJob{};
    job.absolute_path = sandbox.file("stub-publication.svg");
    job.test_hooks_enabled = true;
    job.bytes = {std::byte{'x'}};
    EXPECT_EQ(registry.start(doc, std::move(job), [&](auto) {
        EXPECT_TRUE(app->publicationWaitActive(doc));
        ++calls;
    }), Fop::PublicationWorkerRegistry::StartResult::Started);
    EXPECT_TRUE(registry.pending(doc));
    EXPECT_TRUE(app->publicationInFlight(doc));
    EXPECT_TRUE(document_check_for_data_loss(nullptr));
    EXPECT_TRUE(app->waitForPublication(doc));
    EXPECT_FALSE(registry.pending(doc));
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(app->waitForPublication(doc));
    EXPECT_EQ(calls, 1);
}

TEST_F(DocumentFileOperationTest, ClosePromptWaitsForDeferredCleanOutcome)
{
    ensure_extension_initialized();
    Fop::enable_file_io_test_hooks();
    struct ResetHooks {
        ~ResetHooks() {
            set_document_check_context_for_testing(nullptr, nullptr);
            Fop::reset_file_io_test_hooks_for_testing();
        }
    } reset_hooks;
    Gtk::Window window;
    set_document_check_context_for_testing(doc, &window);
    set_document_check_response_for_testing(GTK_RESPONSE_CANCEL);
    doc->setModifiedSinceSave(true);
    TempDirAndPreferences sandbox;
    auto *native = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get(SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE));
    ASSERT_NE(native, nullptr);
    auto const target = sandbox.file("prompt-wait.svg");
    auto pending = Inkscape::Extension::begin_save_async(native, doc, target.c_str(),
        false, true, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true);
    ASSERT_TRUE(pending);
    auto shared_pending = std::shared_ptr<Inkscape::Extension::PendingSave>(std::move(pending));
    Pub::PublicationResult result;
    auto job = Inkscape::Extension::take_publication_job(*shared_pending, &result);
    ASSERT_TRUE(job);
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    int delivered = 0;
    ASSERT_EQ(app->publications().start(doc, std::move(*job), [&, shared_pending](auto published) {
        DocumentUndo::whenPublicationCompletable(doc, [&, shared_pending, published](SPDocument &) {
            Inkscape::Extension::finish_save_async(*shared_pending, published, nullptr, false);
            ++delivered;
        }, [&](SPDocument &) { ++delivered; });
    }), Fop::PublicationWorkerRegistry::StartResult::Started);
    EXPECT_TRUE(document_check_for_data_loss(nullptr)); // Pending publication defers close.
    EXPECT_EQ(delivered, 0);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    interaction->rollback();
    app->waitForPublication(doc);
    dispatch_file_op_test_events();
    EXPECT_EQ(delivered, 1);
    EXPECT_EQ(doc, nullptr); // The pending clean result resumed the confirmed close.
}

TEST_F(DocumentFileOperationTest, DocumentCloseWaitsForRegistryCompletion)
{
    TempDirAndPreferences sandbox;
    int completions = 0;
    bool destroyed_after_completion = false;
    auto connection = doc->connectDestroy([&] {
        destroyed_after_completion = completions == 1;
    });
    auto job = Inkscape::Extension::Internal::PublicationJob{};
    job.absolute_path = sandbox.file("close-wait.svg");
    job.bytes = {std::byte{'x'}};
    ASSERT_EQ(app->publications().start(doc, std::move(job),
        [&](auto) { ++completions; }),
        Fop::PublicationWorkerRegistry::StartResult::Started);
    app->document_close(doc);
    EXPECT_EQ(completions, 0);
    EXPECT_NE(doc, nullptr);
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    EXPECT_EQ(completions, 1);
    EXPECT_TRUE(destroyed_after_completion);
    EXPECT_EQ(doc, nullptr);
    connection.disconnect();
}

TEST_F(DocumentFileOperationTest, ReentrantCloseDuringWaitDoesNotReportUnregisteredDocument)
{
    TempDirAndPreferences sandbox;
    auto job = Pub::PublicationJob{};
    job.absolute_path = sandbox.file("reentrant-close.svg");
    job.bytes = {std::byte{'x'}};
    ASSERT_EQ(app->publications().start(doc, std::move(job), [&](auto) {
        app->document_close(doc);
    }), Fop::PublicationWorkerRegistry::StartResult::Started);
    testing::internal::CaptureStderr();
    app->document_close(doc);
    EXPECT_TRUE(pump_save_wait([&] { return doc == nullptr; }));
    auto const errors = testing::internal::GetCapturedStderr();
    EXPECT_EQ(doc, nullptr);
    EXPECT_EQ(errors.find("Document not registered"), std::string::npos) << errors;
}

TEST_F(DocumentFileOperationTest, PendingSavePublishesOlderSnapshotWithoutCleaningLiveEdits)
{
    ensure_extension_initialized();
    TempDirAndPreferences sandbox;
    auto const target = sandbox.file("p4a-older.svg");
    auto *native = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get(
#ifdef __APPLE__
            SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE
#else
            SP_MODULE_KEY_OUTPUT_SVG
#endif
        ));
    ASSERT_NE(native, nullptr);
    auto pending = Inkscape::Extension::begin_save_async(
        native, doc, target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true);
    ASSERT_TRUE(pending);
    Inkscape::Extension::Internal::PublicationResult result;
    auto job = Inkscape::Extension::take_publication_job(*pending, &result);
#if defined(__APPLE__) || defined(_WIN32)
#ifdef __APPLE__
    ASSERT_TRUE(job); // Inkscape SVG must use buffered publication on Apple.
#endif
    if (job) result = Inkscape::Extension::Internal::publish(std::move(*job));
#else
    EXPECT_FALSE(job);
#endif
    find_node(doc->getReprRoot(), "svg:rect")->setAttribute("width", "41");
    EXPECT_THROW(Inkscape::Extension::finish_save_async(*pending, result, nullptr, false),
                 Inkscape::Extension::PublishedOlderRevision);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(attribute_of(*reopened, "svg:rect", "width"), "16");
    EXPECT_EQ(attribute_of(*doc, "svg:rect", "width"), "41");
}

TEST(P4aShutdown, RegistryReleasesCapturesBeforeOwnedDocuments)
{
    if (!Application::exists()) Application::create(false);
    auto *app = &file_op_test_application();
    ASSERT_NE(app, nullptr);
    TempDirAndPreferences sandbox;
    auto *doc = app->document_add(document(kFixtureSvg));
    ASSERT_NE(doc, nullptr);
    bool destroyed = false;
    bool capture_released_before_document = false;
    auto connection = doc->connectDestroy([&] { destroyed = true; });
    auto capture = std::shared_ptr<int>(new int, [&](int *value) {
        capture_released_before_document = !destroyed;
        delete value;
    });
    auto job = Inkscape::Extension::Internal::PublicationJob{};
    job.absolute_path = sandbox.file("shutdown.svg");
    job.bytes = {std::byte{'x'}};
    ASSERT_EQ(app->publications().start(doc, std::move(job),
        [capture = std::move(capture)](auto) {}),
        Fop::PublicationWorkerRegistry::StartResult::Started);
    delete app;
    EXPECT_TRUE(capture_released_before_document);
    EXPECT_TRUE(destroyed);
    connection.disconnect();
}



using Worker = Inkscape::IO::PublicationWorkerRegistry;

Pub::PublicationJob worker_job(std::string path = "/unused-publication-worker-target")
{
    Pub::PublicationJob job;
    job.absolute_path = std::move(path);
    job.test_hooks_enabled = true;
    job.bytes = {std::byte{'x'}};
    return job;
}

bool pump_until(std::function<bool()> done, GMainContext *context = nullptr,
                std::chrono::seconds limit = std::chrono::seconds(10))
{
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        g_main_context_iteration(context, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

TEST(PublicationWorker, CompletionOnDefaultContextDespitePrivateThreadDefault)
{
    auto owner = std::this_thread::get_id();
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    std::thread::id observed;
    std::atomic<int> calls = 0;
    std::atomic<bool> published = false;
    {
        Worker worker([&](Pub::PublicationJob) {
            published = true;
            return Pub::PublicationResult{Pub::PublicationOutcome::Failed};
        });
        int key = 0;
        EXPECT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) {
            observed = std::this_thread::get_id(); ++calls;
        }), Worker::StartResult::Started);
        while (!published.load()) std::this_thread::yield();
        // Wait until the completion has actually been posted before checking
        // that the private context cannot dispatch it.
        while (!g_main_context_pending(nullptr)) std::this_thread::yield();
        g_main_context_iteration(context, false);
        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(pump_until([&] { return calls == 1; }));
        EXPECT_EQ(observed, owner);
        EXPECT_TRUE(worker.wait(&key));
        EXPECT_EQ(calls, 1);
    }
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
}

TEST(PublicationWorker, CompletionOnRunningMainLoop)
{
    auto *loop = g_main_loop_new(nullptr, false);
    Worker worker([](Pub::PublicationJob) {
        return Worker::Result{Pub::PublicationOutcome::Failed};
    });
    auto const owner = std::this_thread::get_id();
    int calls = 0;
    int key = 0;
    ASSERT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        ++calls;
        g_main_loop_quit(loop);
    }), Worker::StartResult::Started);
    auto *timeout = g_timeout_source_new_seconds(5);
    g_source_set_callback(timeout, [](gpointer data) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop *>(data));
        return G_SOURCE_REMOVE;
    }, loop, nullptr);
    g_source_attach(timeout, nullptr);
    g_main_loop_run(loop);
    g_source_destroy(timeout);
    g_source_unref(timeout);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(worker.wait(&key));
    g_main_loop_unref(loop);
}

TEST(PublicationWorker, BusyDoesNotQueueAndWaitCompletesOnce)
{
    std::atomic<bool> release = false;
    std::atomic<int> runs = 0;
    int calls = 0;
    auto owner = std::this_thread::get_id();
    Worker worker([&](Pub::PublicationJob) {
        ++runs;
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return Pub::PublicationResult{Pub::PublicationOutcome::Failed};
    });
    int key = 0;
    ASSERT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) {
        EXPECT_EQ(std::this_thread::get_id(), owner); ++calls;
    }), Worker::StartResult::Started);
    EXPECT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) { ++calls; }),
              Worker::StartResult::Busy);
    release = true;
    EXPECT_TRUE(worker.wait(&key));
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(calls, 1);
    dispatch_file_op_test_events(); // posted idle must do nothing
    EXPECT_EQ(calls, 1);
}

TEST(PublicationWorker, FailedThreadStartReleasesDocumentKey)
{
    int key = 0;
    int calls = 0;
    Worker worker([](Pub::PublicationJob) { return Worker::Result{}; });
    Worker::fail_next_thread_start_for_testing();
    EXPECT_THROW(worker.start(&key, worker_job(), [&](Worker::Result) { ++calls; }),
                 std::runtime_error);
    EXPECT_FALSE(worker.wait(&key));
    EXPECT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) { ++calls; }),
              Worker::StartResult::Started);
    EXPECT_TRUE(worker.wait(&key));
    EXPECT_EQ(calls, 1);
}

TEST(PublicationWorker, HolderAllocationFailsBeforeRegisteringDocument)
{
    int key = 0;
    Worker worker([](Pub::PublicationJob) { return Worker::Result{}; });
    Worker::fail_next_holder_allocation_for_testing();
    EXPECT_THROW(worker.start(&key, worker_job(), [](Worker::Result) {}), std::bad_alloc);
    EXPECT_FALSE(worker.wait(&key));
    EXPECT_EQ(worker.start(&key, worker_job(), [](Worker::Result) {}), Worker::StartResult::Started);
    EXPECT_TRUE(worker.wait(&key));
}

TEST(PublicationWorker, ResultStoreFailureStillDeliversUncertain)
{
    int key = 0;
    std::optional<Worker::Result> observed;
    Worker worker([](Pub::PublicationJob) { return Worker::Result{Pub::PublicationOutcome::Published}; });
    Worker::fail_next_result_store_for_testing();
    ASSERT_EQ(worker.start(&key, worker_job(), [&](Worker::Result result) {
        observed = std::move(result);
    }), Worker::StartResult::Started);
    EXPECT_TRUE(worker.wait(&key));
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->outcome, Pub::PublicationOutcome::Uncertain);
}

TEST(PublicationWorker, DestructorDetachesKeysBeforeReleasingCompletions)
{
    int key = 0;
    bool checked = false;
    auto worker = std::make_unique<Worker>([](Pub::PublicationJob) { return Worker::Result{}; });
    auto *registry = worker.get();
    auto token = std::shared_ptr<int>(new int(1), [&](int *value) {
        delete value;
        checked = true;
        EXPECT_FALSE(registry->wait(&key));
    });
    ASSERT_EQ(worker->start(&key, worker_job(), [token = std::move(token)](Worker::Result) {}),
              Worker::StartResult::Started);
    worker.reset();
    EXPECT_TRUE(checked);
}

TEST(PublicationWorker, ExceptionIsUncertainAndDestructionJoinsWithoutCompletion)
{
    int key = 0;
    int calls = 0;
    Worker throwing([](Pub::PublicationJob) -> Worker::Result { throw std::runtime_error("injected"); });
    ASSERT_EQ(throwing.start(&key, worker_job(), [&](Worker::Result result) {
        ++calls;
        EXPECT_EQ(result.outcome, Pub::PublicationOutcome::Uncertain);
    }), Worker::StartResult::Started);
    EXPECT_TRUE(throwing.wait(&key));
    EXPECT_EQ(calls, 1);
    std::atomic<bool> running = false;
    std::atomic<bool> finished = false;
    {
        Worker joined([&](Pub::PublicationJob) {
            running = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            finished = true;
            return Worker::Result{};
        });
        ASSERT_EQ(joined.start(&key, worker_job(), [&](Worker::Result) { ++calls; }),
                  Worker::StartResult::Started);
    }
    EXPECT_TRUE(running);
    EXPECT_TRUE(finished);
    dispatch_file_op_test_events();
    EXPECT_EQ(calls, 1);
}

TEST(PublicationWorker, CompletionMayStartAndWaitForSameDocumentInsideWait)
{
    int key = 0;
    int calls = 0;
    Worker worker([](Pub::PublicationJob) {
        return Worker::Result{Pub::PublicationOutcome::Failed};
    });
    ASSERT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) {
        ++calls;
        EXPECT_FALSE(worker.wait(&key));
        EXPECT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) { ++calls; }),
                  Worker::StartResult::Started);
        EXPECT_TRUE(worker.wait(&key));
    }), Worker::StartResult::Started);
    EXPECT_TRUE(worker.wait(&key));
    EXPECT_EQ(calls, 2);
    EXPECT_FALSE(worker.wait(&key));
}

TEST(PublicationWorker, ThrowingIdleCompletionIsCaughtAndRunsOnce)
{
    int key = 0;
    int calls = 0;
    Worker worker([](Pub::PublicationJob) {
        return Worker::Result{Pub::PublicationOutcome::Failed};
    });
    ASSERT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) {
        ++calls;
        throw std::runtime_error("injected completion failure");
    }), Worker::StartResult::Started);
    EXPECT_TRUE(pump_until([&] { return calls == 1; }));
    EXPECT_TRUE(worker.wait(&key));
    EXPECT_EQ(calls, 1);
}

TEST(PublicationWorker, ThrowingWaitCompletionIsCaughtAndRunsOnce)
{
    int key = 0;
    int calls = 0;
    Worker worker([](Pub::PublicationJob) { return Worker::Result{}; });
    ASSERT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) {
        ++calls;
        throw std::runtime_error("injected wait completion failure");
    }), Worker::StartResult::Started);
    g_test_expect_message(nullptr, G_LOG_LEVEL_WARNING, "*injected wait completion failure*");
    EXPECT_NO_THROW(EXPECT_TRUE(worker.wait(&key)));
    g_test_assert_expected_messages();
    dispatch_file_op_test_events();
    EXPECT_EQ(calls, 1);
}

TEST(PublicationWorker, IdleCompletionCanDestroyRegistry)
{
    int key = 0;
    int calls = 0;
    auto worker = std::make_unique<Worker>([](Pub::PublicationJob) { return Worker::Result{}; });
    ASSERT_EQ(worker->start(&key, worker_job(), [&](Worker::Result) {
        ++calls;
        worker.reset();
    }), Worker::StartResult::Started);
    EXPECT_TRUE(pump_until([&] { return calls == 1; }));
    EXPECT_EQ(worker, nullptr);
    dispatch_file_op_test_events();
    EXPECT_EQ(calls, 1);
}

TEST(PublicationWorker, IdleCompletionMayStartAndWaitForSameDocument)
{
    int key = 0;
    int calls = 0;
    Worker worker([](Pub::PublicationJob) { return Worker::Result{}; });
    ASSERT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) {
        ++calls;
        EXPECT_EQ(worker.start(&key, worker_job(), [&](Worker::Result) { ++calls; }),
                  Worker::StartResult::Started);
        EXPECT_TRUE(worker.wait(&key));
    }), Worker::StartResult::Started);
    EXPECT_TRUE(pump_until([&] { return calls == 2; }));
    EXPECT_FALSE(worker.wait(&key));
    EXPECT_EQ(calls, 2);
}

TEST(PublicationWorker, DeliveredSlotReleasesCompletionAndPublisherCaptures)
{
    int key = 0;
    auto publisher_capture = std::make_shared<int>(1);
    auto completion_capture = std::make_shared<int>(2);
    Worker worker([publisher_capture](Pub::PublicationJob) {
        return Worker::Result{Pub::PublicationOutcome::Failed};
    });
    ASSERT_EQ(worker.start(&key, worker_job(), [completion_capture](Worker::Result) {}),
              Worker::StartResult::Started);
    EXPECT_TRUE(pump_until([&] { return completion_capture.use_count() == 1; }));
    EXPECT_EQ(publisher_capture.use_count(), 2); // caller and registry publisher
    EXPECT_TRUE(worker.wait(&key));
}

TEST(PublicationWorker, StageFailureIsTyped)
{
    Worker worker;
    int key = 0;
    auto job = worker_job();
    job.stage_hooks[0].failure = Pub::PublicationOutcome::Conflict;
    std::optional<Worker::Result> observed;
    ASSERT_EQ(worker.start(&key, std::move(job), [&](Worker::Result result) {
        observed = std::move(result);
    }), Worker::StartResult::Started);
    EXPECT_TRUE(pump_until([&] { return observed.has_value(); }));
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->outcome, Pub::PublicationOutcome::Conflict);
    EXPECT_EQ(observed->error, "injected publication stage failure");
    EXPECT_TRUE(worker.wait(&key));
}

TEST(PublicationWorker, StorageStallKeepsMainLoopResponsive)
{
    char const *env = g_getenv("VACARDS_P2_STALL_MS");
    unsigned stall_ms = env ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 3000;
    ASSERT_GE(stall_ms, 3000u);
    ASSERT_LE(stall_ms, 30000u);
    Worker worker;
    int key = 0;
    auto job = worker_job();
    job.stage_hooks[0].stall_ms = stall_ms;
    job.stage_hooks[0].failure = Pub::PublicationOutcome::Failed;
    bool done = false;
    auto const owner = std::this_thread::get_id();
    struct Heartbeat {
        std::chrono::steady_clock::time_point next;
        std::vector<double> lateness;
    } heartbeat{std::chrono::steady_clock::now() + std::chrono::milliseconds(50), {}};
    auto *source = g_timeout_source_new(50);
    g_source_set_callback(source, [](gpointer data) -> gboolean {
        auto &h = *static_cast<Heartbeat *>(data);
        auto const now = std::chrono::steady_clock::now();
        h.lateness.push_back(std::max(0.0, std::chrono::duration<double, std::milli>(now - h.next).count()));
        h.next = now + std::chrono::milliseconds(50);
        return G_SOURCE_CONTINUE;
    }, &heartbeat, nullptr);
    g_source_attach(source, nullptr);
    ASSERT_EQ(worker.start(&key, std::move(job), [&](Worker::Result result) {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        EXPECT_EQ(result.outcome, Pub::PublicationOutcome::Failed);
        done = true;
    }), Worker::StartResult::Started);
    EXPECT_TRUE(pump_until([&] { return done; }, nullptr,
                           std::chrono::seconds(stall_ms / 1000 + 10)));
    g_source_destroy(source);
    g_source_unref(source);
    EXPECT_TRUE(worker.wait(&key));
    ASSERT_GT(heartbeat.lateness.size(), stall_ms / 70);
    std::sort(heartbeat.lateness.begin(), heartbeat.lateness.end());
    auto const p95 = heartbeat.lateness[static_cast<std::size_t>(
        std::ceil(heartbeat.lateness.size() * .95)) - 1];
    auto const max = heartbeat.lateness.back();
    std::printf("P2 heartbeat stall_ms=%u samples=%zu p95_late_ms=%.3f max_late_ms=%.3f\n",
                stall_ms, heartbeat.lateness.size(), p95, max);
    EXPECT_LT(p95, 100.0);
    EXPECT_LT(max, 250.0);
}
#ifdef WITH_VACARDS_NESTING
// Regression: these app actions were compiled out (no config.h), so the
// context-menu entries and --actions calls silently did nothing.
TEST_F(DocumentFileOperationTest, NestingContourActionsAreRegisteredAndWork)
{
    auto *nest = app->document_add(document(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)"
        R"(<rect id="art" x="0" y="0" width="40" height="40"/>)"
        R"(<rect id="cut" x="5" y="5" width="30" height="30"/></svg>)"));
    ASSERT_TRUE(nest);
    ASSERT_TRUE(app->gio_app()->has_action("selection-set-nesting-contour"));
    ASSERT_TRUE(app->gio_app()->has_action("selection-release-nesting-contour"));
    auto *selection = nest->getSelection();
    app->set_active_selection(selection);
    // Selection order is reversed on purpose: "topmost" means stacking order.
    selection->set(nest->getObjectById("cut"));
    selection->add(nest->getObjectById("art"));
    app->gio_app()->activate_action("selection-set-nesting-contour");
    auto *cut = nest->getObjectById("cut");
    auto *art = nest->getObjectById("art");
    ASSERT_TRUE(cut && art);
    EXPECT_STREQ(cut->getAttribute("inkscape:nesting-contour"), "true");
    EXPECT_EQ(art->getAttribute("inkscape:nesting-contour"), nullptr);
    ASSERT_TRUE(cut->parent && cut->parent == art->parent);
    EXPECT_STREQ(cut->parent->getAttribute("inkscape:nesting-contour-version"), "1");
    // One Undo step removes the group and the markers.
    Inkscape::DocumentUndo::undo(nest);
    cut = nest->getObjectById("cut");
    ASSERT_TRUE(cut);
    EXPECT_EQ(cut->getAttribute("inkscape:nesting-contour"), nullptr);
    EXPECT_EQ(cut->parent, nest->getRoot());
    // Release clears both markers.
    Inkscape::DocumentUndo::redo(nest);
    cut = nest->getObjectById("cut");
    selection->set(cut->parent);
    app->gio_app()->activate_action("selection-release-nesting-contour");
    cut = nest->getObjectById("cut");
    EXPECT_EQ(cut->getAttribute("inkscape:nesting-contour"), nullptr);
    EXPECT_EQ(cut->parent->getAttribute("inkscape:nesting-contour-version"), nullptr);
    app->set_active_selection(nullptr);
    app->document_close(nest);
}
#endif

} // namespace
