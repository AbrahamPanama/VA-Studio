// SPDX-License-Identifier: GPL-2.0-or-later
// Full-library GTK integration, not part of the standalone destination helper tests.
// Main must set INKSCAPE_EXPORT_HINTS_TEST_BINARY to the exact newly built executable
// and run serially with an isolated profile/display and a bounded CTest timeout.
// Temporary output directories are retained as evidence, never recursively deleted.
#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <glibmm/convert.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/entry.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/togglebutton.h>
#include <gtk/gtk.h>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "extension/implementation/implementation.h"
#include "file.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "io/export-destination.h"
#include "object/sp-item.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection.h"
#include "ui/builder-utils.h"
#include "ui/dialog/export-single.h"
#include "ui/dialog/export.h"
#include "ui/widget/export-lists.h"
#include "ui/widget/export-preview.h"
#include "ui/widget/spinbutton.h"
#include "xml/repr.h"
#include "xml/document.h"
#include "xml/node-observer.h"

using namespace Inkscape;
namespace fs = std::filesystem;

namespace {
InkscapeApplication &application()
{
    if (auto app = InkscapeApplication::instance()) return *app;
    Gtk::Application::wrap_in_search_entry2();
    g_setenv("INKSCAPE_APP_ID_TAG", "exporthintstest", true);
    return *new InkscapeApplication(); // Same process-lifetime convention as other GTK fixtures.
}

void drain_bounded()
{
    for (unsigned i = 0; i < 64 && g_main_context_pending(nullptr); ++i) {
        g_main_context_iteration(nullptr, false);
    }
}

std::array<std::string, 3> hints(SPObject *object)
{
    std::array<std::string, 3> result;
    constexpr std::array names{"inkscape:export-filename", "inkscape:export-xdpi", "inkscape:export-ydpi"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto value = object->getRepr()->attribute(names[i]);
        result[i] = value ? value : "";
    }
    return result;
}

// Independent PNG header/chunk inspection: dimensions and physical resolution.
void expect_png(fs::path const &path, unsigned pixels, unsigned pixels_per_metre)
{
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good()) << path;
    std::vector<unsigned char> data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    ASSERT_GE(data.size(), 33u);
    std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    EXPECT_TRUE(std::equal(signature.begin(), signature.end(), data.begin()));
    auto u32 = [&](std::size_t at) {
        return (std::uint32_t(data.at(at)) << 24) | (std::uint32_t(data.at(at + 1)) << 16) |
               (std::uint32_t(data.at(at + 2)) << 8) | data.at(at + 3);
    };
    EXPECT_EQ(u32(16), pixels);
    EXPECT_EQ(u32(20), pixels);
    bool found_phys = false;
    for (std::size_t at = 8; at <= data.size() - 12;) {
        auto length = u32(at);
        ASSERT_LE(length, data.size() - at - 12);
        std::string type(data.begin() + at + 4, data.begin() + at + 8);
        if (type == "pHYs") {
            ASSERT_EQ(length, 9u);
            EXPECT_EQ(u32(at + 8), pixels_per_metre);
            EXPECT_EQ(u32(at + 12), pixels_per_metre);
            EXPECT_EQ(data.at(at + 16), 1u);
            found_phys = true;
        }
        at += length + 12;
    }
    EXPECT_TRUE(found_phys);
}

struct OnceHintObserver final : XML::NodeObserver {
    std::function<void(XML::Node &)> invoke;
    unsigned calls = 0;
    void notifyAttributeChanged(XML::Node &node, GQuark key, Util::ptr_shared, Util::ptr_shared) override {
        if (key != g_quark_from_static_string("inkscape:export-filename")) return;
        node.removeObserver(*this); // Callback can release/rebind this node's SPObject.
        ++calls;
        invoke(node);
    }
};

class ExportHintsGtkTest : public testing::Test {
protected:
    InkscapeApplication *app = nullptr;
    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    Glib::RefPtr<Gtk::Builder> builder;
    UI::Dialog::SingleExport *panel = nullptr;
    fs::path directory, source, output;
    std::string history_key, old_history;
    sigc::scoped_connection destroyed;

    template <typename T> T &widget(char const *id) { return UI::get_widget<T>(builder, id); }
    void click(char const *id) { g_signal_emit_by_name(widget<Gtk::Button>(id).gobj(), "clicked"); }
    SPItem *item() const { return cast<SPItem>(document->getObjectById("target")); }
    void mode(char const *id) { widget<Gtk::ToggleButton>(id).set_active(true); }
    void resolution(double dpi) { widget<UI::Widget::SpinButton>("si_dpi_sb").set_value(dpi); }
    void filename(fs::path const &path) {
        widget<Gtk::Entry>("si_filename").set_text(Glib::filename_to_utf8(path.string()));
    }
    void save() { ASSERT_TRUE(sp_file_save_document(*desktop->getInkscapeWindow(), document)); }
    void forget_undo() {
        DocumentUndo::done(document, Util::Internal::ContextString{"Fixture setup"}, "");
        DocumentUndo::clearUndo(document);
        DocumentUndo::clearRedo(document);
        document->setModifiedSinceSave(false);
    }

    void SetUp() override {
        app = &application();
        ASSERT_TRUE(app->gtk_app()) << "A real GTK display is required; this is not a skipped acceptance case";
        app->gio_app()->register_application();
        if (!Application::exists()) Application::create(false);
        GError *error = nullptr;
        auto temporary = g_dir_make_tmp("vacards-export-hints-XXXXXX", &error);
        ASSERT_NE(temporary, nullptr) << (error ? error->message : "no temporary directory");
        directory = temporary;
        g_free(temporary);
        g_clear_error(&error);
        RecordProperty("evidence_directory", directory.string());
        source = directory / "Source.revision.2.svg";
        output = directory / "GUI output.png";
        auto owned = SPDocument::createNewDocFromMem(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="192" height="192" viewBox="0 0 192 192"><rect id="target" x="0" y="0" width="96" height="96" fill="#2468ac"/></svg>)");
        ASSERT_TRUE(owned);
        document = app->document_add(std::move(owned));
        destroyed = document->connectDestroy([this] { document = nullptr; desktop = nullptr; });
        document->setDocumentFilename(source.string().c_str());
        desktop = app->createDesktop(document, false, true);
        ASSERT_TRUE(desktop);
        app->set_active_desktop(desktop);
        INKSCAPE.activate_desktop(desktop);
        ASSERT_EQ(SP_ACTIVE_DESKTOP, desktop);
        desktop->getSelection()->set(item());
        document->ensureUpToDate();
        forget_undo();
        ASSERT_NO_FATAL_FAILURE(save()); // Normal native save, before any hints exist.
        builder = UI::create_builder("dialog-export.glade");
        panel = &UI::get_derived_widget<UI::Dialog::SingleExport>(builder, "single-image");
        panel->setApp(app);
        panel->setDesktop(desktop);
        panel->setDocument(document);
        panel->selectionChanged(desktop->getSelection());
        widget<UI::Dialog::ExtensionList>("si_extention").setExtensionFromFilename("fixture.png");
        mode("si_s_document");
        history_key = IO::ExportDestination::preference_key(IO::ExportDestination::format_key("image/png", ".png"));
        old_history = Preferences::get()->getString(history_key);
        filename(output); // Public entry signal pins the destination; no asynchronous timing oracle.
        resolution(300);
        ASSERT_EQ(hints(document->getRoot()), (std::array<std::string, 3>{}));
        ASSERT_EQ(hints(item()), (std::array<std::string, 3>{}));
        forget_undo();
    }

    void TearDown() override {
        if (panel) {
            panel->setDocument(nullptr);
            panel->setDesktop(nullptr);
        }
        panel = nullptr;
        builder.reset();
        if (document) {
            document->setModifiedSinceSave(false);
            if (desktop) app->destroyDesktop(desktop);
            else app->document_close(document);
        }
        drain_bounded();
        if (!history_key.empty()) Preferences::get()->setString(history_key, old_history);
        // Outputs, serialized SVG and subprocess stdout/stderr deliberately remain for main.
    }

    void cli(std::vector<std::string> arguments, std::string const &evidence_name) {
        auto executable = g_getenv("INKSCAPE_EXPORT_HINTS_TEST_BINARY");
        ASSERT_TRUE(executable && *executable)
            << "Main must supply the exact new executable; no PATH/installed-app fallback";
        ASSERT_TRUE(fs::is_regular_file(executable));
        auto uuid = g_uuid_string_random();
        arguments.insert(arguments.begin(), std::string("--app-id-tag=exporthintschild") + uuid);
        g_free(uuid); // Never remote-forward to this GTK fixture or an installed app.
        arguments.insert(arguments.begin(), executable);
        arguments.push_back(source.string());
        std::vector<char *> argv;
        for (auto &arg : arguments) argv.push_back(arg.data());
        argv.push_back(nullptr);
        gchar *stdout_text = nullptr, *stderr_text = nullptr;
        gint status = -1;
        GError *error = nullptr;
        auto launched = g_spawn_sync(directory.string().c_str(), argv.data(), nullptr,
            G_SPAWN_DEFAULT, nullptr, nullptr, &stdout_text, &stderr_text, &status, &error);
        std::ofstream(directory / (evidence_name + ".stdout")) << (stdout_text ? stdout_text : "");
        std::ofstream(directory / (evidence_name + ".stderr")) << (stderr_text ? stderr_text : "");
        std::string message = error ? error->message : (stderr_text ? stderr_text : "");
        g_free(stdout_text);
        g_free(stderr_text);
        g_clear_error(&error);
        ASSERT_TRUE(launched) << message;
        EXPECT_TRUE(g_spawn_check_wait_status(status, &error)) << (error ? error->message : message);
        g_clear_error(&error);
    }
};

class ExportHintsRoundTrip : public ExportHintsGtkTest, public testing::WithParamInterface<bool> {};

TEST_P(ExportHintsRoundTrip, GtkWriterNativeSaveReopenThenRealCliHints)
{
    bool const selection = GetParam();
    mode(selection ? "si_s_selection" : "si_s_document");
    resolution(300);
    auto target = selection ? static_cast<SPObject *>(item()) : document->getRoot();
    click("si_export"); // Real production writer and real encoder, never synthesize hint XML.
    ASSERT_NO_FATAL_FAILURE(expect_png(output, 300, 11811));
    auto const written = hints(target);
    ASSERT_FALSE(written[0].empty());
    EXPECT_EQ(written[1], "300");
    EXPECT_EQ(written[2], "300");
    EXPECT_TRUE(document->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(document));
    EXPECT_EQ(hints(target), (std::array<std::string, 3>{}));
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    EXPECT_EQ(hints(target), written);
    EXPECT_FALSE(DocumentUndo::redo(document));
    ASSERT_NO_FATAL_FAILURE(save());
    auto reopened = SPDocument::createNewDoc(source.string().c_str());
    ASSERT_TRUE(reopened);
    auto reopened_target = selection ? reopened->getObjectById("target") : reopened->getRoot();
    EXPECT_EQ(hints(reopened_target), written);
    fs::rename(output, directory / "original-gui.png");
    ASSERT_NO_FATAL_FAILURE(cli({"--export-use-hints", selection ? "--export-id=target" : "--export-area-drawing"}, "cli-hints"));
    ASSERT_NO_FATAL_FAILURE(expect_png(output, 300, 11811));
    fs::rename(output, directory / "cli-hints.png");
    // The public action entrypoint must consume the same GUI-created saved hints.
    ASSERT_NO_FATAL_FAILURE(cli({selection
        ? "--actions=export-id:target;export-use-hints:true;export-do"
        : "--actions=export-area-drawing:true;export-use-hints:true;export-do"}, "action-hints"));
    ASSERT_NO_FATAL_FAILURE(expect_png(output, 300, 11811));
    fs::rename(output, directory / "action-hints.png");
    ASSERT_NO_FATAL_FAILURE(cli({"--export-use-hints", selection ? "--export-id=target" : "--export-area-drawing", "--export-dpi=192"}, "cli-dpi-override"));
    ASSERT_NO_FATAL_FAILURE(expect_png(output, 192, 7559));
}
INSTANTIATE_TEST_SUITE_P(DrawingAndSelection, ExportHintsRoundTrip, testing::Bool());

TEST_F(ExportHintsGtkTest, RepeatedIdenticalSuccessfulExportDoesNotConsumeRedoOrDirtyDocument)
{
    click("si_export");
    ASSERT_TRUE(fs::exists(output));
    auto const written = hints(document->getRoot());
    ASSERT_NO_FATAL_FAILURE(save());
    DocumentUndo::clearUndo(document);
    document->getReprRoot()->setAttribute("data-redo", "retained");
    DocumentUndo::done(document, Util::Internal::ContextString{"Redo sentinel"}, "");
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->setModifiedSinceSave(false);
    fs::rename(output, directory / "first-success.png"); // Avoid any overwrite prompt.
    click("si_export");
    EXPECT_TRUE(fs::exists(output));
    EXPECT_EQ(hints(document->getRoot()), written);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    EXPECT_STREQ(document->getReprRoot()->attribute("data-redo"), "retained");
}

TEST_F(ExportHintsGtkTest, CancellationBeforeEncodingChangesNeitherHintsNorHistory)
{
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto const history = Preferences::get()->getString(history_key);
    bool delivered = false;
    auto cancel = Glib::signal_idle().connect([&] { delivered = true; click("si_cancel"); return false; }, G_PRIORITY_HIGH);
    click("si_export");
    cancel.disconnect();
    EXPECT_TRUE(delivered);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_EQ(Preferences::get()->getString(history_key), history);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(ExportHintsGtkTest, DestinationEditingAndRefreshAreNotDocumentEdits)
{
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto const history = Preferences::get()->getString(history_key);
    filename(directory / "Different basename.svg");
    panel->refresh();
    filename(directory / "Different basename.png");
    panel->refresh();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_EQ(Preferences::get()->getString(history_key), history);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(ExportHintsGtkTest, DestinationRefreshAndFormatChangeKeepCurrentResolution)
{
    click("si_export"); // Establish a genuine old 300 DPI hint.
    ASSERT_TRUE(fs::exists(output));
    auto const written = hints(document->getRoot());
    resolution(450);
    auto &extension = widget<UI::Dialog::ExtensionList>("si_extention");
    extension.setExtensionFromFilename("new.svg");
    EXPECT_DOUBLE_EQ(widget<UI::Widget::SpinButton>("si_dpi_sb").get_value(), 450);
    extension.setExtensionFromFilename("new.png");
    EXPECT_DOUBLE_EQ(widget<UI::Widget::SpinButton>("si_dpi_sb").get_value(), 450);
    document->setDocumentFilename((directory / "Renamed.svg").string().c_str());
    ASSERT_NO_FATAL_FAILURE(save()); // Actual native save with the changed filename.
    panel->refresh();
    EXPECT_DOUBLE_EQ(widget<UI::Widget::SpinButton>("si_dpi_sb").get_value(), 450);
    EXPECT_EQ(hints(document->getRoot()), written);
    EXPECT_EQ(widget<Gtk::Entry>("si_filename").get_text(), Glib::filename_to_utf8(output.string()));
}

TEST_F(ExportHintsGtkTest, MissingParentEncoderFailureChangesNeitherHintsNorHistory)
{
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto const history = Preferences::get()->getString(history_key);
    auto missing_output = directory / "missing-parent" / "failure.png";
    filename(missing_output);
    // Allow the existing encoder's error dialog to finish, without mocking it.
    auto dismiss = Glib::signal_idle().connect([] {
        auto windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
            auto window = g_list_model_get_item(windows, i);
            if (GTK_IS_MESSAGE_DIALOG(window)) gtk_dialog_response(GTK_DIALOG(window), GTK_RESPONSE_CLOSE);
            g_object_unref(window);
        }
        return true;
    });
    click("si_export");
    dismiss.disconnect();
    EXPECT_FALSE(fs::exists(missing_output));
    EXPECT_FALSE(fs::exists(missing_output.parent_path()));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_EQ(Preferences::get()->getString(history_key), history);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(ExportHintsGtkTest, ReleasedSelectionTargetIsNotReboundByIdBeforeEncoding)
{
    mode("si_s_selection");
    auto const history = Preferences::get()->getString(history_key);
    bool delivered = false;
    auto release = Glib::signal_idle().connect([&] {
        delivered = true;
        item()->deleteObject();
        auto repr = document->getReprDoc()->createElement("svg:rect");
        repr->setAttribute("id", "target");
        repr->setAttribute("width", "96");
        repr->setAttribute("height", "96");
        document->getReprRoot()->appendChild(repr);
        GC::release(repr);
        return false;
    }, G_PRIORITY_HIGH);
    click("si_export");
    release.disconnect();
    EXPECT_TRUE(delivered);
    ASSERT_TRUE(item());
    EXPECT_EQ(hints(item()), (std::array<std::string, 3>{}));
    EXPECT_EQ(hints(document->getRoot()), (std::array<std::string, 3>{}));
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(Preferences::get()->getString(history_key), history);
}

TEST_F(ExportHintsGtkTest, CustomAreaWritesRootHintsAndSelectedPageWritesOnlyThatPage)
{
    mode("si_s_custom");
    resolution(240);
    click("si_export");
    ASSERT_TRUE(fs::exists(output));
    EXPECT_EQ(document->getRoot()->getExportDpi(), Geom::Point(240, 240));
    ASSERT_TRUE(DocumentUndo::undo(document));
    EXPECT_EQ(hints(document->getRoot()), (std::array<std::string, 3>{}));
    DocumentUndo::clearRedo(document);
    auto &pages = document->getPageManager();
    pages.newDocumentPage(Geom::Rect(Geom::Point(0, 0), Geom::Point(96, 96)));
    pages.newDocumentPage(Geom::Rect(Geom::Point(192, 0), Geom::Point(288, 96)));
    panel->refresh();
    mode("si_s_page");
    auto &list = widget<Gtk::FlowBox>("si_pages");
    auto child = list.get_child_at_index(0);
    ASSERT_TRUE(child);
    auto batch_item = dynamic_cast<UI::Dialog::BatchItem *>(child);
    ASSERT_TRUE(batch_item);
    auto page = batch_item->getPage();
    ASSERT_TRUE(page);
    list.select_child(*child);
    forget_undo();
    filename(directory / "page.png");
    resolution(192);
    click("si_export");
    ASSERT_TRUE(fs::exists(directory / "page.png"));
    EXPECT_EQ(page->getExportDpi(), Geom::Point(192, 192));
    EXPECT_FALSE(page->getExportFilename().empty());
    EXPECT_EQ(hints(document->getRoot()), (std::array<std::string, 3>{}));
    ASSERT_TRUE(DocumentUndo::undo(document));
    EXPECT_EQ(hints(page), (std::array<std::string, 3>{}));
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(ExportHintsGtkTest, RegisteredWindowCloseBeforeEncoderSkipsHintsAndBalancesLease)
{
    auto const history = Preferences::get()->getString(history_key);
    unsigned destructions = 0;
    sigc::scoped_connection counted = document->connectDestroy([&] { ++destructions; });
    bool delivered = false;
    auto close = Glib::signal_idle().connect([&] {
        delivered = true;
        EXPECT_FALSE(document->isModifiedSinceSave());
        unsigned confirmations = 0;
        auto confirm = Glib::signal_idle().connect([&] {
            auto windows = gtk_window_get_toplevels();
            for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
                auto window = g_list_model_get_item(windows, i);
                if (GTK_IS_MESSAGE_DIALOG(window) &&
                    g_strcmp0(gtk_window_get_title(GTK_WINDOW(window)), g_dgettext("inkscape", "Save Document")) == 0) {
                    ++confirmations;
                    gtk_dialog_response(GTK_DIALOG(window), GTK_RESPONSE_NO);
                }
                g_object_unref(window);
            }
            return true;
        });
        EXPECT_TRUE(app->destroyDesktop(desktop)); // Real owner path, before entering shared encoder.
        confirm.disconnect();
        RecordProperty("close_confirmation_count", confirmations);
        EXPECT_EQ(destructions, 0u);
        EXPECT_TRUE(app->documentClosePending(document));
        panel->setDocument(nullptr);
        panel->setDesktop(nullptr);
        return false;
    }, G_PRIORITY_HIGH);
    click("si_export");
    close.disconnect();
    EXPECT_TRUE(delivered);
    EXPECT_FALSE(fs::exists(output));
    if (document) EXPECT_EQ(hints(document->getRoot()), (std::array<std::string, 3>{}));
    drain_bounded();
    EXPECT_EQ(destructions, 1u);
    EXPECT_EQ(app->documentOperationCount(), 0u);
    EXPECT_FALSE(app->documentHoldActive());
    EXPECT_EQ(Preferences::get()->getString(history_key), history);
}

TEST_F(ExportHintsGtkTest, DocumentReplacementBeforeEncodingCannotReceiveOldExportHints)
{
    auto replacement = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96"><rect width="96" height="96"/></svg>)");
    ASSERT_TRUE(replacement);
    auto const before = sp_repr_save_buf(document->getReprDoc());
    auto const replacement_before = sp_repr_save_buf(replacement->getReprDoc());
    bool delivered = false;
    auto change = Glib::signal_idle().connect([&] {
        delivered = true;
        panel->setDocument(replacement.get());
        return false;
    }, G_PRIORITY_HIGH);
    click("si_export");
    change.disconnect();
    EXPECT_TRUE(delivered);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_EQ(sp_repr_save_buf(replacement->getReprDoc()), replacement_before);
    panel->setDocument(document); // Private replacement owner survives every synchronous call.
}

TEST_F(ExportHintsGtkTest, AdmittedHintsFinishAgainstCapturedSourceAfterViewAndFocusSwitch)
{
    auto other_document = app->document_add(SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96"><rect width="96" height="96"/></svg>)"));
    SPDesktop *other_desktop = nullptr;
    other_document->ensureUpToDate();
    auto other_xml = sp_repr_save_buf(other_document->getReprDoc());
    app->set_active_desktop(desktop);
    INKSCAPE.activate_desktop(desktop);
    OnceHintObserver observer;
    observer.invoke = [&](XML::Node &) {
        // createDesktop presents a native window. Before encoding, its delayed
        // activation can legitimately cancel the export at a progress callback.
        // This test exercises a switch AFTER metadata admission, so create and
        // present the second desktop only at that boundary.
        other_desktop = app->createDesktop(other_document, false, true);
        ASSERT_TRUE(other_desktop);
        other_document->ensureUpToDate();
        // Window initialization may update namedview state; compare export's
        // effects against the initialized second document, as before.
        other_xml = sp_repr_save_buf(other_document->getReprDoc());
        panel->setDesktop(other_desktop);
        panel->setDocument(other_document);
        app->set_active_desktop(other_desktop);
        INKSCAPE.activate_desktop(other_desktop);
    };
    document->getReprRoot()->addObserver(observer);
    click("si_export");
    document->getReprRoot()->removeObserver(observer);
    EXPECT_TRUE(fs::exists(output)); // Distinguish encoder failure from publication failure.
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_EQ(document->getRoot()->getExportDpi(), Geom::Point(300, 300));
    EXPECT_FALSE(document->getRoot()->getExportFilename().empty());
    EXPECT_EQ(sp_repr_save_buf(other_document->getReprDoc()), other_xml);
    EXPECT_EQ(Preferences::get()->getString(history_key), Glib::filename_to_utf8(directory.string()));
    EXPECT_TRUE(DocumentUndo::undo(document));
    EXPECT_EQ(hints(document->getRoot()), (std::array<std::string, 3>{}));
    EXPECT_FALSE(DocumentUndo::undo(document));
    panel->setDesktop(desktop);
    panel->setDocument(document);
    app->set_active_desktop(desktop);
    INKSCAPE.activate_desktop(desktop);
    other_document->setModifiedSinceSave(false);
    if (other_desktop) {
        app->destroyDesktop(other_desktop);
    } else {
        app->document_close(other_document);
    }
}

TEST_F(ExportHintsGtkTest, CancelAfterMetadataAdmissionDoesNotSplitTupleOrSuppressCompletedHistory)
{
    OnceHintObserver observer;
    observer.invoke = [&](XML::Node &) { click("si_cancel"); };
    document->getReprRoot()->addObserver(observer);
    click("si_export");
    document->getReprRoot()->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_TRUE(fs::exists(output));
    EXPECT_EQ(document->getRoot()->getExportDpi(), Geom::Point(300, 300));
    EXPECT_FALSE(document->getRoot()->getExportFilename().empty());
    EXPECT_EQ(Preferences::get()->getString(history_key), Glib::filename_to_utf8(directory.string()));
    ASSERT_TRUE(DocumentUndo::undo(document));
    EXPECT_EQ(hints(document->getRoot()), (std::array<std::string, 3>{}));
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(ExportHintsGtkTest, FinalUiResetDoesNotAdmitNestedExport)
{
    unsigned attempted = 0;
    auto second = directory / "must-not-export.png";
    auto reenter = panel->property_sensitive().signal_changed().connect([&] {
        if (panel->get_sensitive()) {
            ++attempted;
            filename(second);
            click("si_export");
        }
    });
    click("si_export");
    reenter.disconnect();
    EXPECT_EQ(attempted, 1u);
    EXPECT_TRUE(fs::exists(output));
    EXPECT_FALSE(fs::exists(second));
    ASSERT_TRUE(DocumentUndo::undo(document));
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(ExportHintsGtkTest, IdenticalHintTupleDoesNotEnterXmlMutationOrAttributeCallbacks)
{
    click("si_export");
    ASSERT_TRUE(fs::exists(output));
    fs::rename(output, directory / "first-noop-oracle.png");
    ASSERT_NO_FATAL_FAILURE(save());
    document->ensureUpToDate();
    drain_bounded();
    unsigned mutations = 0, attributes = 0;
    struct Observer final : XML::NodeObserver {
        unsigned &calls;
        explicit Observer(unsigned &value) : calls(value) {}
        void notifyAttributeChanged(XML::Node &, GQuark key, Util::ptr_shared, Util::ptr_shared) override {
            if (key == g_quark_from_static_string("inkscape:export-filename") ||
                key == g_quark_from_static_string("inkscape:export-xdpi") ||
                key == g_quark_from_static_string("inkscape:export-ydpi")) ++calls;
        }
    } observer(attributes);
    document->getReprRoot()->addObserver(observer);
    auto mutation = document->getReprDoc()->signalMutationFinished().connect([&] { ++mutations; });
    click("si_export");
    mutation.disconnect();
    document->getReprRoot()->removeObserver(observer);
    EXPECT_EQ(attributes, 0u);
    EXPECT_EQ(mutations, 0u); // Also catches equal-value setters, not just attribute changes.
    EXPECT_FALSE(document->isModifiedSinceSave());
}

// An admitted hint tuple must survive reentrant commits and object rebinding.
TEST_F(ExportHintsGtkTest, ExternalDoneMustNotSplitHintTupleOrConsumeFreshTransaction)
{
    unsigned commits = 0;
    auto committed = document->connectCommit([&] { ++commits; });
    OnceHintObserver observer;
    observer.invoke = [&](XML::Node &node) {
        node.setAttribute("data-external", "committed");
        DocumentUndo::done(document, Util::Internal::ContextString{"External action"}, "");
        node.setAttribute("data-after", "fresh-pending-transaction");
    };
    document->getReprRoot()->addObserver(observer);
    click("si_export");
    document->getReprRoot()->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_EQ(commits, 1u); // Export must not commit the fresh data-after transaction.
    EXPECT_STREQ(document->getReprRoot()->attribute("data-after"), "fresh-pending-transaction");
    EXPECT_EQ(document->getRoot()->getExportDpi(), Geom::Point(300, 300));
    DocumentUndo::done(document, Util::Internal::ContextString{"Later independent action"}, "");
    EXPECT_EQ(commits, 2u);
    committed.disconnect();
    ASSERT_TRUE(DocumentUndo::undo(document));
    EXPECT_EQ(document->getReprRoot()->attribute("data-after"), nullptr);
    EXPECT_STREQ(document->getReprRoot()->attribute("data-external"), "committed");
}

TEST_F(ExportHintsGtkTest, ReleasedAndReboundRepresentationMustNotRetainPartialHints)
{
    mode("si_s_selection");
    OnceHintObserver observer;
    observer.invoke = [&](XML::Node &node) {
        GC::anchor(&node);
        node.parent()->removeChild(&node);
        document->getReprRoot()->appendChild(&node); // Same XML, different SPObject identity.
        GC::release(&node);
    };
    auto repr = item()->getRepr();
    repr->addObserver(observer);
    click("si_export");
    repr->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    ASSERT_TRUE(item());
    auto const actual = hints(item());
    bool const unchanged = actual == std::array<std::string, 3>{};
    bool const complete = !actual[0].empty() && actual[1] == "300" && actual[2] == "300";
    EXPECT_TRUE(unchanged || complete) << "A newly bound item must not inherit only part of an admitted hint tuple";
}

TEST_F(ExportHintsGtkTest, SharedRasterCancellationDuringEncodingDoesNotPublishOutput)
{
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto extension = widget<UI::Dialog::ExtensionList>("si_extention").getExtension();
    unsigned callbacks = 0;
    auto cancel = +[](float, void *data) -> unsigned {
        ++*static_cast<unsigned *>(data);
        return false;
    };
    EXPECT_FALSE(UI::Dialog::Export::exportRaster(Geom::Rect(Geom::Point(0, 0), Geom::Point(96, 96)), 300, 300, 300,
        Colors::Color(0xffffffff), Glib::filename_to_utf8(output.string()), true, cancel, &callbacks, extension, nullptr));
    EXPECT_GT(callbacks, 0u);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(ExportHintsGtkTest, SharedRasterCloseDuringEncodingRetainsDocumentUntilDrawingReleased)
{
    auto extension = widget<UI::Dialog::ExtensionList>("si_extention").getExtension();
    unsigned callbacks = 0, destructions = 0;
    sigc::scoped_connection counted = document->connectDestroy([&] { ++destructions; });
    std::function<void()> close = [&] {
        ++callbacks;
        panel->setDocument(nullptr);
        panel->setDesktop(nullptr);
        document->setModifiedSinceSave(false);
        auto closing = std::exchange(desktop, nullptr);
        EXPECT_TRUE(app->destroyDesktop(closing));
        EXPECT_EQ(destructions, 0u);
        EXPECT_TRUE(app->documentClosePending(document));
    };
    auto callback = +[](float, void *data) -> unsigned {
        (*static_cast<std::function<void()> *>(data))();
        return true; // The shared helper must detect closure independently.
    };
    EXPECT_FALSE(UI::Dialog::Export::exportRaster(Geom::Rect(Geom::Point(0, 0), Geom::Point(96, 96)), 300, 300, 300,
        Colors::Color(0xffffffff), Glib::filename_to_utf8(output.string()), true, callback, &close, extension, nullptr));
    EXPECT_EQ(callbacks, 1u);
    EXPECT_FALSE(fs::exists(output));
    drain_bounded();
    EXPECT_EQ(destructions, 1u);
    EXPECT_EQ(app->documentOperationCount(), 0u);
    EXPECT_FALSE(app->documentHoldActive());
}

TEST_F(ExportHintsGtkTest, InterlacedRasterCancellationInvokesClientOnlyOnce)
{
    auto extension = widget<UI::Dialog::ExtensionList>("si_extention").getExtension();
    struct Restore {
        Extension::Output *extension;
        bool interlacing;
        ~Restore() { extension->set_param_bool("png_interlacing", interlacing); }
    } restore{extension, extension->get_param_bool("png_interlacing", false)};
    extension->set_param_bool("png_interlacing", true);
    unsigned callbacks = 0;
    auto cancel = +[](float, void *data) -> unsigned {
        ++*static_cast<unsigned *>(data);
        return false;
    };
    EXPECT_FALSE(UI::Dialog::Export::exportRaster(Geom::Rect(Geom::Point(0, 0), Geom::Point(96, 96)),
        300, 300, 300, Colors::Color(0xffffffff), Glib::filename_to_utf8(output.string()),
        true, cancel, &callbacks, extension, nullptr));
    EXPECT_EQ(callbacks, 1u);
    EXPECT_FALSE(fs::exists(output));
}

TEST_F(ExportHintsGtkTest, UnloadableOutputCannotReportSuccessfulRasterOrVectorSave)
{
    struct Unloadable : Extension::Implementation::Implementation {
        unsigned loads = 0, saves = 0;
        bool load(Extension::Extension *) override { ++loads; return false; }
        void save(Extension::Output *, SPDocument *, gchar const *) override { ++saves; }
        void export_raster(Extension::Output *, SPDocument const *, std::string const &, gchar const *) override {
            ++saves;
        }
    } implementation;
    auto xml = std::shared_ptr<XML::Document>(sp_repr_read_buf(
        "<inkscape-extension xmlns='http://www.inkscape.org/namespace/inkscape/extension'>"
        "<name>Unloadable test output</name><id>org.vacards.test.unloadable-output</id>"
        "<output><extension>.svg</extension><mimetype>image/svg+xml</mimetype></output>"
        "</inkscape-extension>", INKSCAPE_EXTENSION_URI));
    ASSERT_TRUE(xml);
    Extension::Output module(xml->root(), Extension::Extension::ImplementationHolder::make_nonowning(&implementation), nullptr);
    EXPECT_THROW(module.save(document, output.string().c_str()), Extension::Output::save_failed);
    EXPECT_THROW(module.export_raster(document, "unused-source.png", output.string().c_str(), false),
                 Extension::Output::save_failed);
    EXPECT_EQ(implementation.loads, 2u);
    EXPECT_EQ(implementation.saves, 0u);
    EXPECT_FALSE(fs::exists(output));
}

TEST_F(ExportHintsGtkTest, RelativeDestinationStaysBoundToSubmissionAfterDocumentRename)
{
    auto moved = directory / "renamed-folder";
    ASSERT_TRUE(fs::create_directory(moved));
    for (auto suffix : {"png", "svg"}) {
        SCOPED_TRACE(suffix);
        document->setDocumentFilename(source.string().c_str());
        auto basename = std::string("relative.") + suffix;
        widget<UI::Dialog::ExtensionList>("si_extention").setExtensionFromFilename(basename);
        filename(basename);
        bool delivered = false;
        auto rename = Glib::signal_idle().connect([&] {
            delivered = true;
            document->setDocumentFilename((moved / "renamed.svg").string().c_str());
            return false;
        }, G_PRIORITY_HIGH);
        click("si_export");
        rename.disconnect();
        EXPECT_TRUE(delivered);
        EXPECT_TRUE(fs::exists(directory / basename));
        EXPECT_FALSE(fs::exists(moved / basename));
        auto written = Glib::filename_from_utf8(hints(document->getRoot())[0]);
        EXPECT_EQ(Glib::canonicalize_filename(UI::Dialog::Export::absolutizePath(document, written)),
                  (directory / basename).string());
    }
}

TEST_F(ExportHintsGtkTest, FilenameNotificationCanDestroyPanelWithoutLaterMemberAccess)
{
    auto &button = widget<Gtk::Button>("si_export");
    button.set_sensitive(false);
    auto entry = widget<Gtk::Entry>("si_filename").gobj();
    g_object_ref(entry); // Keep the emitting native widget alive, not the panel.
    auto object = G_OBJECT(panel->gobj());
    bool destroyed = false;
    auto notify = +[](gpointer data, GObject *) { *static_cast<bool *>(data) = true; };
    g_object_weak_ref(object, notify, &destroyed);
    auto change = button.property_sensitive().signal_changed().connect([&] {
        if (!button.get_sensitive()) return;
        panel = nullptr;
        builder.reset(); // Release the actual GtkBuilder-owned panel hierarchy.
    });
    gtk_editable_set_text(GTK_EDITABLE(entry), (directory / "edited.png").string().c_str());
    change.disconnect();
    EXPECT_TRUE(destroyed);
    if (!destroyed) g_object_weak_unref(object, notify, &destroyed);
    g_object_unref(entry);
}
} // namespace


namespace {
// These cases exercise real GLib timeout dispatch and native PreviewDrawing, but
// do not create a desktop/window or change the existing close-during-export test.
class ExportPreviewGtkTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto &app = application();
        ASSERT_TRUE(app.gtk_app()) << "A real GTK display is required";
        app.gio_app()->register_application();
        if (!Application::exists()) Application::create(false);
    }

    static std::unique_ptr<SPDocument> source(char const *fill)
    {
        auto xml = std::string(R"(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 96 96"><rect width="96" height="96" fill=")") +
                   fill + R"("/></svg>)";
        auto document = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
        if (document) document->ensureUpToDate();
        return document;
    }

    // Unlike drain_bounded(), this cannot return simply because no source is
    // ready yet. The witness itself must dispatch after the requested interval.
    static void dispatch_for(unsigned milliseconds)
    {
        auto loop = Glib::MainLoop::create();
        bool witnessed = false;
        auto const start = g_get_monotonic_time();
        sigc::scoped_connection witness = Glib::signal_timeout().connect([&] {
            witnessed = true;
            loop->quit();
            return false;
        }, milliseconds);
        loop->run();
        EXPECT_TRUE(witnessed);
        EXPECT_GE(g_get_monotonic_time() - start, gint64(milliseconds) * 1000);
    }

    // Rendering has two asynchronous stages. Wait for their actual result,
    // bounded by a separate deadline rather than assuming a machine's speed.
    static bool dispatch_until(std::function<bool()> const &ready)
    {
        auto loop = Glib::MainLoop::create();
        bool observed = false;
        sigc::scoped_connection poll = Glib::signal_timeout().connect([&] {
            if (!ready()) return true;
            observed = true;
            loop->quit();
            return false;
        }, 10);
        sigc::scoped_connection deadline = Glib::signal_timeout().connect([&] {
            loop->quit();
            return false;
        }, 2000);
        loop->run();
        return observed;
    }

    static void expect_blue(UI::Dialog::ExportPreview &preview)
    {
        auto paintable = preview.get_paintable();
        ASSERT_TRUE(paintable);
        ASSERT_TRUE(GDK_IS_TEXTURE(paintable->gobj()));
        auto texture = GDK_TEXTURE(paintable->gobj());
        auto width = gdk_texture_get_width(texture);
        auto height = gdk_texture_get_height(texture);
        ASSERT_GT(width, 0);
        ASSERT_GT(height, 0);
        ASSERT_LE(width, 128);
        ASSERT_LE(height, 128);
        // Public GDK download format is native-endian CAIRO_FORMAT_ARGB32.
        std::vector<std::uint32_t> pixels(std::size_t(width) * height);
        gdk_texture_download(texture, reinterpret_cast<guchar *>(pixels.data()),
                             std::size_t(width) * sizeof(std::uint32_t));
        EXPECT_EQ(pixels[std::size_t(height / 2) * width + width / 2], 0xff0000ffu);
    }
};

TEST_F(ExportPreviewGtkTest, DetachBeforeRefreshTimerLeavesSurvivingWidgetEmpty)
{
    auto document = source("#ff0000");
    ASSERT_TRUE(document);
    UI::Dialog::ExportPreview preview;
    auto drawing = std::make_shared<UI::Dialog::PreviewDrawing>(document.get());
    std::weak_ptr<UI::Dialog::PreviewDrawing> weak = drawing;
    preview.setBox(Geom::Rect(Geom::Point(0, 0), Geom::Point(96, 96)));
    preview.setDrawing(drawing);
    preview.queueRefresh();
    preview.setDrawing({});
    drawing.reset();
    ASSERT_TRUE(weak.expired());
    document.reset(); // The detached widget intentionally survives its source.
    preview.queueRefresh(); // Detached refresh is also a no-op.
    dispatch_for(175); // Actually cross the old 100 ms refresh deadline.
    EXPECT_FALSE(preview.get_paintable());
}

TEST_F(ExportPreviewGtkTest, RebindCancelsOldRefreshAndExplicitRequeueRendersNewSource)
{
    auto red = source("#ff0000");
    auto blue = source("#0000ff");
    ASSERT_TRUE(red);
    ASSERT_TRUE(blue);
    UI::Dialog::ExportPreview preview;
    auto previous = std::make_shared<UI::Dialog::PreviewDrawing>(red.get());
    std::weak_ptr<UI::Dialog::PreviewDrawing> weak = previous;
    auto current = std::make_shared<UI::Dialog::PreviewDrawing>(blue.get());
    preview.setBox(Geom::Rect(Geom::Point(0, 0), Geom::Point(96, 96)));
    preview.setDrawing(previous);
    preview.queueRefresh();
    preview.setDrawing(current);
    previous.reset();
    ASSERT_TRUE(weak.expired());
    red.reset();

    // Rebinding must cancel the old request, not silently transfer it. Allow
    // enough dispatches for the old refresh + construction + render pipeline.
    dispatch_for(450);
    EXPECT_FALSE(preview.get_paintable());
    preview.queueRefresh();
    preview.queueRefresh(); // Coalesce while a refresh is queued.
    ASSERT_TRUE(dispatch_until([&] { return bool(preview.get_paintable()); }));
    ASSERT_NO_FATAL_FAILURE(expect_blue(preview));
    auto first = preview.get_paintable();
    dispatch_for(175);
    EXPECT_EQ(preview.get_paintable(), first); // Completed refresh must stop.

    preview.queueRefresh(); // A completed timeout must not suppress a new one.
    ASSERT_TRUE(dispatch_until([&] { return preview.get_paintable() != first; }));
    ASSERT_NO_FATAL_FAILURE(expect_blue(preview));
    preview.setDrawing({});
    current.reset(); // Manager destructs while its source is still alive.
}

TEST_F(ExportPreviewGtkTest, DetachWithQueuedNativeConstructionCancelsBothTimers)
{
    auto document = source("#ff0000");
    ASSERT_TRUE(document);
    UI::Dialog::ExportPreview preview;
    auto drawing = std::make_shared<UI::Dialog::PreviewDrawing>(document.get());
    std::weak_ptr<UI::Dialog::PreviewDrawing> weak = drawing;
    Geom::Rect const box(Geom::Point(0, 0), Geom::Point(96, 96));
    preview.setBox(box);
    preview.setDrawing(drawing);
    // Public render() queues the real manager's 100 ms construction timeout.
    // Set up this intermediate state synchronously, not with a timing race
    // between an initial refresh callback and its construction callback.
    EXPECT_FALSE(drawing->render(&preview, 0, nullptr, 128, Geom::OptRect(box)));
    EXPECT_FALSE(preview.get_paintable());
    preview.queueRefresh();
    preview.setDrawing({});
    drawing.reset();
    ASSERT_TRUE(weak.expired());
    document.reset();
    dispatch_for(175); // Both queued deadlines are now in the past.
    EXPECT_FALSE(preview.get_paintable());
}
} // namespace
