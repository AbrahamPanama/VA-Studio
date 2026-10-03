// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Integration regression for BUG-002: a suffixless native-chooser save path
 * must resolve to the native editable Inkscape SVG output module, while
 * explicit known and unknown suffixes keep their existing behaviour, and the
 * real save/overwrite/cancel/reopen outcomes must hold for that path.
 *
 * The module lookup used below is the production dispatch called by
 * sp_file_save_dialog() in src/file.cpp, normalization is the production helper
 * from src/io/save-filename.h, and every write goes through the production
 * Inkscape::Extension::save() in src/extension/system.cpp. None of that logic
 * is mirrored here.
 *
 * The overwrite confirmation is the headless seam added to system.h: the
 * production GUI predicate cannot run without a desktop. That seam proves the
 * exact filename handed to the predicate and the confirm/cancel document
 * outcomes; it does NOT exercise the native chooser panel itself, so the
 * panel/double-prompt interaction still needs the manual GUI battery.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#ifdef __APPLE__
#include <sys/resource.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/xattr.h>
#endif
#include <unistd.h>
#endif

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <giomm/file.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/miscutils.h>
#include <gtkmm.h>
#include <zlib.h>

#include "display/cairo-utils.h"
#include "document-undo.h"
#include "document.h"
#include "event-log.h"
#include "extension/db.h"
#include "extension/extension.h"
#include "extension/implementation/implementation.h"
#include "extension/init.h"
#include "extension/output.h"
#include "extension/internal/svg-publication.h"
#include "extension/internal/svg.h"
#include "extension/system.h"
#include "inkgc/gc-core.h"
#include "inkscape.h"
#include "io/save-filename.h"
#include "io/existing-file-replacement.h"
#include "io/save-path-split.h"
#include "io/sys.h"
#include "io/file-export-cmd.h"
#include "io/stream/bufferstream.h"
#include "object/sp-image.h"
#include "object/sp-object.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "rdf.h"
#include "util/cast.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "xml/repr.h"
#include "xml/repr-save-output-stream.h"

// Production save-target dispatch, defined in src/file.cpp. Declared here so the
// integration test calls the real function rather than a copy of its logic.
Inkscape::Extension::Output *get_output_extension_for_save(std::string filename);
Glib::ustring saved_older_revision_dialog_text(Glib::ustring const &display_name,
                                               std::string const &save_notice);
Glib::ustring saved_stale_document_dialog_text(Glib::ustring const &display_name,
                                               std::string const &reason,
                                               std::string const &save_notice);
std::pair<Glib::ustring, Glib::ustring> classify_file_save_exception_for_testing(
    std::exception_ptr, Glib::ustring const &, std::string &);
unsigned classify_file_save_flags_for_testing(std::exception_ptr);
bool complete_file_save_without_window_for_testing(SPDocument *, Glib::RefPtr<Gio::File> const &, bool);

#ifdef __APPLE__
namespace Inkscape::IO::detail {
void set_recovery_delete_failure_errno_for_testing(int errno_value);
void set_rename_failure_errno_for_testing(int errno_value);
}
#elif defined(_WIN32)
namespace Inkscape::IO::detail {
void set_replace_fault_for_testing(unsigned long code, int kind);
}
#endif

namespace {

using Inkscape::DocumentUndo;
using Inkscape::IO::append_save_extension_if_missing;

#ifdef __APPLE__
struct ScopedRecoveryDeleteFailureInjection {
    explicit ScopedRecoveryDeleteFailureInjection(int error)
    {
        Inkscape::IO::detail::set_recovery_delete_failure_errno_for_testing(error);
    }
    ~ScopedRecoveryDeleteFailureInjection()
    {
        Inkscape::IO::detail::set_recovery_delete_failure_errno_for_testing(0);
    }
};
#endif

constexpr char kSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10'>"
    "<rect id='bug002' width='4' height='5'/></svg>";

std::string read_file(std::string const &path)
{
    // Read through GLib: paths are UTF-8 on every platform, matching production
    // fopen_utf8name(). std::ifstream would interpret a UTF-8 path as the active
    // ANSI code page on Windows and fail to open a non-ASCII destination (for
    // example the Unicode logical name used by the staged-serializer test).
    // Failure still yields an empty string, as before.
    gchar *contents = nullptr;
    gsize length = 0;
    GError *error = nullptr;
    if (!g_file_get_contents(path.c_str(), &contents, &length, &error)) {
        if (error) {
            g_error_free(error);
        }
        return {};
    }
    std::string data(contents, length);
    g_free(contents);
    return data;
}

bool write_file(std::string const &path, std::string const &data)
{
    return g_file_set_contents(path.c_str(), data.data(),
                               static_cast<gssize>(data.size()), nullptr) != FALSE;
}

// Raw, nonmutating XML snapshot: node types, all attributes in their actual
// order, all text/comments and child order. Deliberately does NOT serialize
// (sp_repr_save_buf mutates); used to prove the live document is byte-for-byte
// untouched.
void fingerprint_node(Inkscape::XML::Node const *node, std::string &out)
{
    auto field = [&](char const *text) {
        std::string_view value = text ? text : "";
        out += std::to_string(value.size()) + ":";
        out.append(value);
    };
    out += "[" + std::to_string(static_cast<int>(node->type()));
    field(node->name());
    field(node->content());
    for (auto const &attr : node->attributeList()) {
        field(g_quark_to_string(attr.key));
        field(static_cast<char const *>(attr.value));
    }
    out += ";";
    for (auto child = node->firstChild(); child; child = child->next()) {
        fingerprint_node(child, out);
    }
    out += "]";
}

std::string fingerprint(SPDocument &document)
{
    std::string result;
    fingerprint_node(document.getReprDoc(), result);
    return result;
}

// A valid 1x1 RGBA PNG so the linked image loads without turning into an error.
// Bytes generated and independently validated once with Python stdlib
// struct+zlib: IHDR/IDAT/IEND CRCs all true and the IDAT zlib stream decodes to
// the filter byte 0x00 followed by the expected RGBA pixel 11 22 33 ff.
std::string png_1x1()
{
    static constexpr unsigned char bytes[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x10, 0x54, 0x32, 0xfe,
        0x0f, 0x00, 0x02, 0x14, 0x01, 0x66, 0xdb, 0xbe, 0xcc, 0x6f, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    return std::string(reinterpret_cast<char const *>(bytes), sizeof(bytes));
}

// A document with a linked relative image, a title and an editable rect, used to
// observe href rebasing, title normalization and undo/redo bookkeeping.
std::string linked_svg()
{
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" "
           "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
           "xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" "
           "xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\" "
           "width=\"20\" height=\"10\" version=\"2.0\">"
           "<title>Linked artwork</title>"
           "<image id=\"img\" xlink:href=\"linked.png\" width=\"5\" height=\"5\"/>"
           "<rect id=\"art\" x=\"1\" width=\"4\" height=\"5\"/>"
           "</svg>";
}

// Proves a linked image resolved to a real, decodable 1x1 RGBA resource with
// the exact pixel bytes authored by png_1x1(), not merely that an href string
// was preserved.
void expect_usable_1x1_rgba(SPDocument &doc, char const *id)
{
    auto *image = cast<SPImage>(doc.getObjectById(id));
    ASSERT_NE(image, nullptr);
    EXPECT_FALSE(image->missing);
    ASSERT_TRUE(image->pixbuf);
    EXPECT_EQ(image->pixbuf->width(), 1);
    EXPECT_EQ(image->pixbuf->height(), 1);

    Inkscape::Pixbuf pixels(*image->pixbuf);
    GdkPixbuf *raw = pixels.getPixbufRaw();
    ASSERT_NE(raw, nullptr);
    ASSERT_EQ(gdk_pixbuf_get_n_channels(raw), 4);
    guchar const *px = gdk_pixbuf_get_pixels(raw);
    ASSERT_NE(px, nullptr);
    EXPECT_EQ(px[0], 0x11);
    EXPECT_EQ(px[1], 0x22);
    EXPECT_EQ(px[2], 0x33);
    EXPECT_EQ(px[3], 0xff);
}

// A non-std exception (not derived from std::exception) proves that the
// production catch-all rollback category is untouched by the save transaction.
struct ObserverUnknown {};

// Test-only output implementation built through the existing build_from_mem()
// seam. It observes the original document while Output::save() is running and
// then throws, proving there is no save-owned pre-write live mutation and no
// catch-specific rollback.
class ObservingOutputImplementation final : public Inkscape::Extension::Implementation::Implementation {
public:
    enum class Failure { SaveFailed, SaveCancelled, Unknown, PublishAndEdit,
                         PublishThenUncertain, PublishThenFailed,
                         SaveUnsupported, SaveConflict, SaveUncertain, SaveUncertainPathAvailable };

    SPDocument *original = nullptr;
    Failure failure = Failure::SaveFailed;
    std::function<bool()> original_unchanged;
    std::function<void(gchar const *)> publish_and_edit;
    std::string publication_notice;
    std::string uncertain_recovery_path;
    bool observed_original_unchanged_during_save = false;
    bool saw_distinct_projection = false;

    void save(Inkscape::Extension::Output *module, SPDocument *doc, gchar const *filename) override
    {
        saw_distinct_projection = (doc != original);
        if (original_unchanged) {
            observed_original_unchanged_during_save = original_unchanged();
        }
        switch (failure) {
        case Failure::SaveFailed:
            throw Inkscape::Extension::Output::save_failed();
        case Failure::SaveCancelled:
            throw Inkscape::Extension::Output::save_cancelled();
        case Failure::Unknown:
            throw ObserverUnknown{};
        case Failure::SaveUnsupported:
            throw Inkscape::Extension::Output::save_unsupported("observed unsupported");
        case Failure::SaveConflict:
            throw Inkscape::Extension::Output::save_conflict("observed conflict");
        case Failure::SaveUncertain:
            throw Inkscape::Extension::Output::save_uncertain("observed uncertain",
                                                              uncertain_recovery_path,
                                                              /*recovery_path_available=*/false);
        case Failure::SaveUncertainPathAvailable:
            throw Inkscape::Extension::Output::save_uncertain("observed uncertain path available",
                                                              uncertain_recovery_path,
                                                              /*recovery_path_available=*/true);
        case Failure::PublishAndEdit:
            if (publish_and_edit) publish_and_edit(filename);
            if (!publication_notice.empty()) module->report_save_notice(publication_notice);
            return;
        case Failure::PublishThenUncertain:
            if (publish_and_edit) publish_and_edit(filename);
            throw Inkscape::Extension::Output::save_uncertain("published but uncertain");
        case Failure::PublishThenFailed:
            if (publish_and_edit) publish_and_edit(filename);
            throw Inkscape::Extension::Output::save_failed();
        }
    }
};

// Blocker-1 compatibility contract: the non-GUI export entry points
// (src/ui/dialog/export.cpp, src/io/file-export-cmd.cpp) catch only
// Output::save_failed. The three typed save outcomes must therefore be
// catchable as save_failed. Enforce the derivation at compile time so a future
// change back to unrelated plain classes breaks the build immediately.
static_assert(std::is_base_of<Inkscape::Extension::Output::save_failed,
                              Inkscape::Extension::Output::save_unsupported>::value,
              "save_unsupported must derive from save_failed so non-GUI callers that catch save_failed still catch it");
static_assert(std::is_base_of<Inkscape::Extension::Output::save_failed,
                              Inkscape::Extension::Output::save_conflict>::value,
              "save_conflict must derive from save_failed so non-GUI callers that catch save_failed still catch it");
static_assert(std::is_base_of<Inkscape::Extension::Output::save_failed,
                              Inkscape::Extension::Output::save_uncertain>::value,
              "save_uncertain must derive from save_failed so non-GUI callers that catch save_failed still catch it");

using ContextString = Inkscape::Util::Internal::ContextString;

class SaveTargetTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        Inkscape::Application::create(false);
        Inkscape::Extension::init();
    }

    void SetUp() override
    {
        Inkscape::IO::reset_file_io_test_hooks_for_testing();
        GError *error = nullptr;
        gchar *tmp = g_dir_make_tmp("vacards-save-target-XXXXXX", &error);
        ASSERT_NE(tmp, nullptr) << (error ? error->message : "g_dir_make_tmp failed");
        g_clear_error(&error);
        workdir = tmp;
        g_free(tmp);

        doc = SPDocument::createNewDocFromMem(std::span<char const>(kSvg, std::strlen(kSvg)));
        ASSERT_NE(doc, nullptr);
        doc->ensureUpToDate();
        doc->setModifiedSinceSave(true);

        auto *prefs = Inkscape::Preferences::get();
        saved_save_as_extension = prefs->getString("/dialogs/save_as/default");
        saved_save_copy_extension = prefs->getString("/dialogs/save_copy/default");
        saved_use_absref = prefs->getBool("/options/svgoutput/usesodipodiabsref");
        saved_enable_svgexport = prefs->getBool("/dialogs/save_as/enable_svgexport");
    }

    void TearDown() override
    {
        Inkscape::IO::reset_file_io_test_hooks_for_testing();
        auto *prefs = Inkscape::Preferences::get();
        prefs->setString("/dialogs/save_as/default", saved_save_as_extension);
        prefs->setString("/dialogs/save_copy/default", saved_save_copy_extension);
        prefs->setBool("/options/svgoutput/usesodipodiabsref", saved_use_absref);
        prefs->setBool("/dialogs/save_as/enable_svgexport", saved_enable_svgexport);
    }

    std::string path(std::string const &name) const { return Glib::build_filename(workdir, name); }

    // Normalizes a suffixless native-chooser return the way file.cpp does.
    std::string normalized(std::string const &name)
    {
        std::string p = path(name);
        EXPECT_TRUE(append_save_extension_if_missing(p, ".svg")) << name;
        EXPECT_EQ(p, path(name + ".svg")) << name;
        return p;
    }

    // Replace the fixture document with one based in the temp directory that has
    // a real linked asset and a title.
    void make_linked_document()
    {
        ASSERT_TRUE(write_file(path("linked.png"), png_1x1()));
        auto xml = linked_svg();
        doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()), path("source.svg"));
        ASSERT_NE(doc, nullptr);
        doc->ensureUpToDate();
        doc->setModifiedSinceSave(true);
    }

    // Common contract for a failure after the native writer has been reached: no
    // live state changes and intact undo/redo, with no extra save entry.
    void expect_failed_save_preserves_live(Inkscape::Extension::Output *module,
                                           bool check_overwrite, bool official, bool sync_title)
    {
        doc->ensureUpToDate();
        DocumentUndo::setUndoSensitive(doc.get(), true);

        // Seed explicit nondefault state so a failed save cannot hide a mutation
        // behind the fixture or profile defaults.
        doc->getReprRoot()->setAttribute("inkscape:dataloss", "legacy-marker");
        DocumentUndo::done(doc.get(), ContextString{"failed-save setup history"}, "");
        DocumentUndo::clearUndo(doc.get());
        DocumentUndo::clearRedo(doc.get());

        auto *prefs = Inkscape::Preferences::get();
        prefs->setString("/dialogs/save_as/default", SP_MODULE_KEY_OUTPUT_SVG);
        prefs->setString("/dialogs/save_copy/default", SP_MODULE_KEY_OUTPUT_SVGZ);

        // The deliberate user edit is the first entry in the settled history, so
        // Undo must restore the authored x="1" rather than drop the attribute.
        auto *art = doc->getObjectById("art");
        ASSERT_NE(art, nullptr);
        art->getRepr()->setAttribute("x", "7");
        DocumentUndo::done(doc.get(), ContextString{"failed-save deliberate user edit"}, "");

        if (sync_title) {
            EXPECT_EQ(rdf_get_work_entity(doc.get(), rdf_find_entity("title")), nullptr);
        }

        auto const fingerprint_before = fingerprint(*doc);
        std::string const filename_before = doc->getDocumentFilename() ? doc->getDocumentFilename() : "";
        std::string const base_before = doc->getDocumentBase() ? doc->getDocumentBase() : "";
        std::string const name_before = doc->getDocumentName() ? doc->getDocumentName() : "";
        char const *href = doc->getObjectById("img")->getAttribute("xlink:href");
        std::string const href_before = href ? href : "";
        char const *dataloss = doc->getReprRoot()->attribute("inkscape:dataloss");
        std::string const dataloss_before = dataloss ? dataloss : "";
        bool const modified_before = doc->isModifiedSinceSave();
        bool const autosave_before = doc->isModifiedSinceAutoSave();
        auto const save_as_before = prefs->getString("/dialogs/save_as/default");
        auto const save_copy_before = prefs->getString("/dialogs/save_copy/default");

        auto const blocked_parent = path("not-a-directory");
        ASSERT_TRUE(g_file_set_contents(blocked_parent.c_str(), "sentinel", -1, nullptr));
        std::string const target = Glib::build_filename(blocked_parent, std::string("drawing") + module->get_extension());

        bool predicate_called = false;
        Inkscape::Extension::OverwriteConfirm const confirm =
            [&predicate_called](std::string const &) { predicate_called = true; return true; };

        EXPECT_THROW({
            Inkscape::Extension::save(
                module, doc.get(), target.c_str(), check_overwrite, official,
                official ? Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS
                         : Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY,
                confirm, sync_title);
        }, Inkscape::Extension::Output::save_failed);

        EXPECT_EQ(predicate_called, check_overwrite);
        EXPECT_FALSE(g_file_test(target.c_str(), G_FILE_TEST_EXISTS));
        EXPECT_EQ(fingerprint(*doc), fingerprint_before);
        EXPECT_EQ(filename_before, doc->getDocumentFilename() ? doc->getDocumentFilename() : "");
        EXPECT_EQ(base_before, doc->getDocumentBase() ? doc->getDocumentBase() : "");
        EXPECT_EQ(name_before, doc->getDocumentName() ? doc->getDocumentName() : "");
        char const *href_after = doc->getObjectById("img")->getAttribute("xlink:href");
        EXPECT_EQ(href_before, href_after ? href_after : "");
        char const *dataloss_after = doc->getReprRoot()->attribute("inkscape:dataloss");
        EXPECT_EQ(dataloss_before, dataloss_after ? dataloss_after : "");
        EXPECT_STREQ(dataloss_after, "legacy-marker");
        if (sync_title) {
            EXPECT_EQ(rdf_get_work_entity(doc.get(), rdf_find_entity("title")), nullptr);
        }
        EXPECT_TRUE(modified_before);
        EXPECT_EQ(doc->isModifiedSinceSave(), modified_before);
        EXPECT_EQ(doc->isModifiedSinceAutoSave(), autosave_before);
        EXPECT_EQ(prefs->getString("/dialogs/save_as/default"), save_as_before);
        EXPECT_EQ(prefs->getString("/dialogs/save_copy/default"), save_copy_before);

        // Existing undo/redo entries survive; no save entry was pushed.
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        doc->ensureUpToDate();
        EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "1");
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        doc->ensureUpToDate();
        EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "7");
    }

    std::unique_ptr<SPDocument> doc;
    std::string workdir;
    Glib::ustring saved_save_as_extension;
    Glib::ustring saved_save_copy_extension;
    bool saved_use_absref = false;
    bool saved_enable_svgexport = false;
};

// ---------------------------------------------------------------------------
// Dispatch coverage (production helper + production get_output_extension_for_save)
// ---------------------------------------------------------------------------

// Baseline defect: without normalization a suffixless chooser path matches no
// output module, which is the reported "No ... extension found to save" error.
TEST_F(SaveTargetTest, SuffixlessPathIsUnresolvableBeforeNormalization)
{
    EXPECT_EQ(get_output_extension_for_save("drawing"), nullptr);
}

TEST_F(SaveTargetTest, SuffixlessPathResolvesToEditableInkscapeSvg)
{
    std::string path = "drawing";
    ASSERT_TRUE(append_save_extension_if_missing(path, ".svg"));
    ASSERT_EQ(path, "drawing.svg");

    auto *module = get_output_extension_for_save(path);
    ASSERT_NE(module, nullptr);
    EXPECT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);
    EXPECT_STREQ(module->get_extension(), ".svg");
    EXPECT_FALSE(module->is_raster());
}

// Even when the document was last saved in another format, a suffixless
// Save As / Save Copy resolves to the editable Inkscape SVG module.
TEST_F(SaveTargetTest, SuffixlessPathIgnoresPreviouslySelectedFormat)
{
    std::string path = "previously-saved-as-svgz";
    ASSERT_TRUE(append_save_extension_if_missing(path, ".svg"));
    auto *module = get_output_extension_for_save(path);
    ASSERT_NE(module, nullptr);
    EXPECT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);
    EXPECT_STRNE(module->get_id(), SP_MODULE_KEY_OUTPUT_SVGZ_INKSCAPE);
}

TEST_F(SaveTargetTest, ExplicitSupportedSuffixIsPreserved)
{
    std::string path = "drawing.svgz";
    EXPECT_FALSE(append_save_extension_if_missing(path, ".svg"));
    EXPECT_EQ(path, "drawing.svgz");

    auto *module = get_output_extension_for_save(path);
    ASSERT_NE(module, nullptr);
    EXPECT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVGZ_INKSCAPE);
}

TEST_F(SaveTargetTest, ExplicitUnknownSuffixIsPreservedAndRejected)
{
    std::string path = "drawing.blarg";
    EXPECT_FALSE(append_save_extension_if_missing(path, ".svg"));
    EXPECT_EQ(path, "drawing.blarg");
    EXPECT_EQ(get_output_extension_for_save(path), nullptr);
}

TEST_F(SaveTargetTest, DottedDirectoryUnicodeFinalFilenameResolves)
{
    // Final filename correctness: the normalized path is exactly what the
    // overwrite confirmation and file_save() then receive.
    std::string path = "/tmp/dir.x/caf\xC3\xA9";
    ASSERT_TRUE(append_save_extension_if_missing(path, ".svg"));
    EXPECT_EQ(path, "/tmp/dir.x/caf\xC3\xA9.svg");
    EXPECT_EQ(Glib::path_get_basename(path), "caf\xC3\xA9.svg");

    auto *module = get_output_extension_for_save(path);
    ASSERT_NE(module, nullptr);
    EXPECT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);
}

TEST_F(SaveTargetTest, NoDoubleSuffixThroughDispatch)
{
    std::string path = "drawing.svg";
    EXPECT_FALSE(append_save_extension_if_missing(path, ".svg"));
    EXPECT_EQ(path, "drawing.svg");

    auto *module = get_output_extension_for_save(path);
    ASSERT_NE(module, nullptr);
    EXPECT_STREQ(module->get_extension(), ".svg");
    EXPECT_EQ(path.find(".svg.svg"), std::string::npos);
}

TEST_F(SaveTargetTest, UppercaseSuffixesResolveNativeSvgAndSvgz)
{
    auto *upper = get_output_extension_for_save(path("drawing.SVG"));
    ASSERT_NE(upper, nullptr);
    EXPECT_STREQ(upper->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);

    auto *upperz = get_output_extension_for_save(path("drawing.SVGZ"));
    ASSERT_NE(upperz, nullptr);
    EXPECT_STREQ(upperz->get_id(), SP_MODULE_KEY_OUTPUT_SVGZ_INKSCAPE);
}

TEST_F(SaveTargetTest, TrailingDotNameIsPreservedAndRejected)
{
    // A trailing dot is an explicit (unknown) suffix; the helper must not
    // rewrite it, so dispatch keeps the pre-existing rejection.
    std::string p = path("name.");
    EXPECT_FALSE(append_save_extension_if_missing(p, ".svg"));
    EXPECT_EQ(p, path("name."));
    EXPECT_EQ(get_output_extension_for_save(p), nullptr);
}

TEST_F(SaveTargetTest, SuffixlessHiddenFilenameNormalizesAndResolves)
{
    // A leading dot is a hidden file, not an extension.
    std::string p = path(".hidden");
    ASSERT_TRUE(append_save_extension_if_missing(p, ".svg"));
    EXPECT_EQ(p, path(".hidden.svg"));

    auto *module = get_output_extension_for_save(p);
    ASSERT_NE(module, nullptr);
    EXPECT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);
}

// ---------------------------------------------------------------------------
// Real save outcomes through production Inkscape::Extension::save()
// ---------------------------------------------------------------------------

// A suffixless chooser return is normalized to the editable Inkscape SVG path;
// the raw suffixless path is never written, and the written file carries the
// document and reopens. The overwrite predicate (seam) receives the normalized
// target, not the raw chooser return.
TEST_F(SaveTargetTest, SaveAsWritesNormalizedNativeSvgAndReopens)
{
    std::string const target = normalized("drawing");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);
    EXPECT_FALSE(module->is_raster());

    std::string confirmed;
    Inkscape::Extension::OverwriteConfirm const confirm =
        [&confirmed](std::string const &f) { confirmed = f; return true; };

    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));

    EXPECT_EQ(confirmed, target); // normalized target reached the overwrite check
    EXPECT_TRUE(g_file_test(target.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test(path("drawing").c_str(), G_FILE_TEST_EXISTS)); // raw path untouched
    auto const content = read_file(target);
    EXPECT_NE(content.find("<svg"), std::string::npos);
    EXPECT_NE(content.find("bug002"), std::string::npos);
    EXPECT_STREQ(doc->getDocumentFilename(), target.c_str());
    EXPECT_FALSE(doc->isModifiedSinceSave());

    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    ASSERT_NE(reopened->getReprRoot(), nullptr);
    EXPECT_STREQ(reopened->getReprRoot()->name(), "svg:svg");
    ASSERT_NE(reopened->getDocumentFilename(), nullptr);
    EXPECT_STREQ(reopened->getDocumentFilename(), target.c_str());
    EXPECT_EQ(std::string(Glib::path_get_basename(reopened->getDocumentFilename())), "drawing.svg");
}

// Cancel on an existing normalized target: the predicate sees the normalized
// target and a false reply aborts with no_overwrite before any mutation, so the
// bytes, document URI and modified state are all preserved.
TEST_F(SaveTargetTest, OverwriteCancelPreservesBytesUriAndModified)
{
    std::string const baseline = normalized("baseline");
    auto *module = get_output_extension_for_save(baseline);
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const allow = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), baseline.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, allow));
    std::string const uri_before = doc->getDocumentFilename();
    ASSERT_EQ(uri_before, baseline);
    doc->setModifiedSinceSave(true);

    std::string const target = normalized("existing");
    ASSERT_TRUE(write_file(target, "SENTINEL"));
    std::string seen;
    Inkscape::Extension::OverwriteConfirm const cancel =
        [&seen](std::string const &f) { seen = f; return false; };

    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                           Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, cancel),
                 Inkscape::Extension::Output::no_overwrite);

    EXPECT_EQ(seen, target);
    EXPECT_EQ(read_file(target), "SENTINEL");
    EXPECT_EQ(std::string(doc->getDocumentFilename()), uri_before);
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

// Replace on an existing normalized target: a true reply proceeds and the exact
// normalized file is rewritten, with the document identity updated and dirtiness
// cleared by the official save.
TEST_F(SaveTargetTest, OverwriteConfirmReplacesExpectedFile)
{
    std::string const target = normalized("replace");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(write_file(target, "SENTINEL"));

    std::string seen;
    Inkscape::Extension::OverwriteConfirm const confirm =
        [&seen](std::string const &f) { seen = f; return true; };

    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));

    EXPECT_EQ(seen, target);
    auto const content = read_file(target);
    EXPECT_EQ(content.find("SENTINEL"), std::string::npos);
    EXPECT_NE(content.find("bug002"), std::string::npos);
    EXPECT_STREQ(doc->getDocumentFilename(), target.c_str());
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_FALSE(g_file_test(path("replace.svg.svg").c_str(), G_FILE_TEST_EXISTS));
}

#ifdef _WIN32
TEST_F(SaveTargetTest, WindowsForwardAndMixedSeparatorsSaveAndReopen)
{
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    for (bool mixed : {false, true}) {
        auto target = path(mixed ? "mixed.svg" : "forward.svg");
        if (mixed) {
            std::replace(target.begin(), target.end(), '/', '\\');
            auto const slash = target.find_last_of('\\');
            ASSERT_NE(slash, std::string::npos);
            target[slash] = '/';
        } else {
            std::replace(target.begin(), target.end(), '\\', '/');
        }
        auto *module = get_output_extension_for_save(target);
        ASSERT_NE(module, nullptr);
        ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                                  Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
        auto reopened = SPDocument::createNewDoc(target.c_str(), false);
        ASSERT_NE(reopened, nullptr) << target;
        EXPECT_NE(reopened->getObjectById("bug002"), nullptr);
    }
}
#endif

#ifdef __APPLE__
TEST_F(SaveTargetTest, ExistingSvgSaveRefusesAliasesWithoutDamagingOriginal)
{
    auto const target = path("original.svg");
    auto const alias = path("alias.svg");
    auto const hard = path("hard.svg");
    ASSERT_TRUE(write_file(target, "complete original"));
    ASSERT_EQ(::symlink(target.c_str(), alias.c_str()), 0);
    auto *module = get_output_extension_for_save(alias);
    ASSERT_NE(module, nullptr);
    auto const prior_identity = std::string(doc->getDocumentFilename() ? doc->getDocumentFilename() : "");
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };

    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), alias.c_str(), true, true,
                 Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::Output::save_unsupported);
    ASSERT_EQ(::link(target.c_str(), hard.c_str()), 0);
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                 Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::Output::save_unsupported);
    EXPECT_EQ(read_file(target), "complete original");
    EXPECT_EQ(read_file(alias), "complete original");
    EXPECT_EQ(read_file(hard), "complete original");
    EXPECT_EQ(std::string(doc->getDocumentFilename() ? doc->getDocumentFilename() : ""), prior_identity);
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

// F1: a definite existing-file failure must preserve its diagnostic reason. A
// read-only parent directory makes replace_existing_local_file() fail when it
// creates its sibling stage; that reason must reach the caller as
// save_failed::error instead of the earlier empty save_failed().
TEST_F(SaveTargetTest, ExistingSvgDefiniteFailureKeepsDiagnosticReason)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses directory permissions; the staging failure cannot be forced";
    }
    auto const dir = path("locked-dir");
    ASSERT_EQ(::mkdir(dir.c_str(), 0700), 0);
    auto const target = Glib::build_filename(dir, "existing.svg");
    ASSERT_TRUE(write_file(target, "complete original"));
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_EQ(::chmod(dir.c_str(), 0500), 0);

    bool threw = false;
    std::string reason;
    try {
        Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                  Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm);
    } catch (Inkscape::Extension::Output::save_failed const &e) {
        threw = true;
        reason = e.error;
    } catch (...) {
        // Leave threw == false so the exact-type assertion below reports it.
    }

    ASSERT_EQ(::chmod(dir.c_str(), 0700), 0);
    EXPECT_TRUE(threw) << "existing-file staging failure did not throw save_failed";
    EXPECT_FALSE(reason.empty()) << "save_failed discarded the definite failure reason";
    EXPECT_NE(reason.find("create sibling stage"), std::string::npos) << reason;
    EXPECT_EQ(read_file(target), "complete original");
}
#endif

// Save Copy writes the copy but keeps the original document identity and the
// pre-save modified state, because the save is unofficial.
TEST_F(SaveTargetTest, SaveCopyPreservesIdentityAndDirty)
{
    std::string const baseline = normalized("original");
    auto *module = get_output_extension_for_save(baseline);
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const allow = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), baseline.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, allow));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    doc->setModifiedSinceSave(true);

    std::string const copy = normalized("copy");
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), copy.c_str(), true, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, allow));

    EXPECT_TRUE(g_file_test(copy.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_NE(read_file(copy).find("bug002"), std::string::npos);
    EXPECT_STREQ(doc->getDocumentFilename(), baseline.c_str()); // identity preserved
    EXPECT_TRUE(doc->isModifiedSinceSave());                    // dirty preserved
}

TEST_F(SaveTargetTest, NativeSvgSaveGolden)
{
    auto const target = normalized("golden");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, false,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, {}, false, nullptr, true));
    auto payload = read_file(target);
    ASSERT_FALSE(payload.empty());
    // The version attribute embeds the build revision; compare all other bytes exactly.
    auto const stamp = payload.find("inkscape:version=\"");
    ASSERT_NE(stamp, std::string::npos);
    auto const stamp_end = payload.find('"', stamp + sizeof("inkscape:version=\"") - 1);
    ASSERT_NE(stamp_end, std::string::npos);
    payload.replace(stamp, stamp_end - stamp + 1, "inkscape:version=\"BUILD\"");
    auto *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        reinterpret_cast<guchar const *>(payload.data()), payload.size());
    ASSERT_NE(digest, nullptr);
    // a77b8bbe8 pinned raw SHA-256 2d2f2032d3b3ff4e44ec2c76b64523d2ff947ce9d85ed1b1a4613e11b3420566.
    // Its retained golden.svg (version 59da699ed2) matches that raw digest;
    // replacing only inkscape:version with BUILD yields the digest below,
    // which also matches the current serialized output after normalization.
    EXPECT_STREQ(digest, "108953892336ac8a0798213e0f8dabd69b6c2d183a69635dc07c8aca87c65d90");
    g_free(digest);
}

TEST_F(SaveTargetTest, NativeInteractiveBytesMatchDirectSaveAndSaveCopy)
{
    make_linked_document();
    auto const destination = path("other/drawing.svg");
    ASSERT_EQ(g_mkdir_with_parents(path("other").c_str(), 0700), 0);
    auto *module = get_output_extension_for_save(destination);
    ASSERT_NE(module, nullptr);
    auto const direct_gui = module->get_gui();
    module->set_gui(false); // Baseline direct writer, same logical target.
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), destination.c_str(), false, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY));
    auto const direct = read_file(destination);
    ASSERT_FALSE(direct.empty());
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), destination.c_str(), false, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY,
                                              {}, false, nullptr, true));
    EXPECT_EQ(read_file(destination), direct);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_STREQ(doc->getDocumentFilename(), path("source.svg").c_str());
    module->set_gui(direct_gui);

    auto const compressed = path("other/drawing.svgz");
    auto *svgz = get_output_extension_for_save(compressed);
    ASSERT_NE(svgz, nullptr);
    auto const svgz_gui = svgz->get_gui();
    svgz->set_gui(false);
    ASSERT_NO_THROW(Inkscape::Extension::save(svgz, doc.get(), compressed.c_str(), false, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY));
    auto const direct_z = read_file(compressed);
    ASSERT_FALSE(direct_z.empty());
    ASSERT_NO_THROW(Inkscape::Extension::save(svgz, doc.get(), compressed.c_str(), false, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY,
                                              {}, false, nullptr, true));
    EXPECT_EQ(read_file(compressed), direct_z);
    svgz->set_gui(svgz_gui);

    auto const official = path("other/official.svg");
    module->set_gui(false);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), official.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    auto const direct_official = read_file(official);
    ASSERT_FALSE(direct_official.empty());
    doc->setModifiedSinceSave(true);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), official.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS,
                                              {}, false, nullptr, true));
    EXPECT_EQ(read_file(official), direct_official);
    EXPECT_STREQ(doc->getDocumentFilename(), official.c_str());
    EXPECT_FALSE(doc->isModifiedSinceSave());
    auto reopened = SPDocument::createNewDoc(official.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    ASSERT_NE(reopened->getObjectById("img"), nullptr);
    EXPECT_STREQ(reopened->getObjectById("img")->getAttribute("xlink:href"), "../linked.png");
    module->set_gui(direct_gui);
}

// Characterization pin for the native SVG serializer. For one fixed fixture and
// a matrix of /options/svgoutput preferences it records the exact output bytes
// (SHA-256 of the version-normalized text) for both native formats, and proves
// the direct writer and the buffered interactive route emit identical bytes.
// The digests describe the CURRENT serializer; later serializer packets must
// keep them unchanged or update this table deliberately as a reviewed change.
TEST_F(SaveTargetTest, SerializerParityDigestMatrix)
{
    auto *prefs = Inkscape::Preferences::get();

    // Restores every touched /options/svgoutput preference on every exit path,
    // including an early ASSERT return, so no combo leaks into a later test.
    struct PrefGuard {
        enum { kN = 12 };
        Inkscape::Preferences *prefs = nullptr;
        Glib::ustring paths[kN];
        bool is_int[kN] = {};
        bool saved_bool[kN] = {};
        int saved_int[kN] = {};
        int n = 0;
        void remember_bool(char const *p)
        {
            paths[n] = p;
            saved_bool[n] = prefs->getBool(p);
            is_int[n] = false;
            ++n;
        }
        void remember_int(char const *p, int def)
        {
            paths[n] = p;
            saved_int[n] = prefs->getInt(p, def);
            is_int[n] = true;
            ++n;
        }
        ~PrefGuard()
        {
            for (int k = 0; k < n; ++k) {
                if (is_int[k]) {
                    prefs->setInt(paths[k], saved_int[k]);
                } else {
                    prefs->setBool(paths[k], saved_bool[k]);
                }
            }
        }
    };
    PrefGuard guard;
    guard.prefs = prefs;
    guard.remember_bool("/options/svgoutput/inlineattrs");
    guard.remember_int("/options/svgoutput/indent", 2);
    guard.remember_bool("/options/svgoutput/check_on_writing");
    guard.remember_bool("/options/svgoutput/sort_attributes");
    guard.remember_bool("/options/svgoutput/disable_optimizations");
    guard.remember_bool("/options/svgoutput/incorrect_attributes_warn");
    guard.remember_bool("/options/svgoutput/incorrect_attributes_remove");
    guard.remember_bool("/options/svgoutput/incorrect_style_properties_warn");
    guard.remember_bool("/options/svgoutput/incorrect_style_properties_remove");
    guard.remember_bool("/options/svgoutput/style_defaults_warn");
    guard.remember_bool("/options/svgoutput/style_defaults_remove");
    guard.remember_bool("/options/svgoutput/usesodipodiabsref");

    struct Combo {
        char const *name;
        bool inlineattrs;
        int indent;
        bool check;
        bool sort;
        bool disopt;
        bool warn;   // all three *_warn flags
        bool rm;     // all three *_remove flags
        bool absref;
    };
    // Every combination sets all twelve preferences explicitly, so a profile
    // default can never change a combo's output.
    Combo const combos[] = {
        {"base",         false, 2, false, false, false, false, false, false},
        {"inline",       true,  2, false, false, false, false, false, false},
        {"indent0",      false, 0, false, false, false, false, false, false},
        {"indent4",      false, 4, false, false, false, false, false, false},
        {"clean-rm",     false, 2, true,  false, false, false, true,  false},
        {"clean-rm-off", false, 2, true,  false, true,  false, true,  false},
        {"clean-warn",   false, 2, true,  false, false, true,  false, false},
        {"sort",         false, 2, false, true,  false, false, false, false},
        {"sort-off",     false, 2, false, true,  true,  false, false, false},
        {"absref",       false, 2, false, false, false, false, false, true},
    };

    // Pinned SHA-256 digests of the normalized serializer output on the CURRENT
    // code. They were recorded once with VACARDS_PRINT_PARITY_DIGESTS=1 during
    // pinning; a later mismatch here means the serializer changed.
    static constexpr std::pair<std::string_view, std::string_view> expected[] = {
        {"base.svg", "b5cda24cd7982c6d897484f4fe062431ca0e39ca3df793c85c1ec3cc7d3e40ce"},
        {"base.svgz", "942ae356851ad5460d907abb5860dbb4db6c59f3e95b809b7611c94d24ba7fc3"},
        {"inline.svg", "a7417f610b5d6d9deba41f8143499c569ebd2d881fb9e63b8070e1a4993ab8aa"},
        {"inline.svgz", "6481bd09f52846c1ebdc135a3fb157481e1fe4bf73708ed67ec53f8f23037a10"},
        {"indent0.svg", "bf80d5d4601fb7ff61fbd9bdc493961f5b37fa61c7856d601f809b453d260767"},
        {"indent0.svgz", "25b8e3cb77a3fd2b8f5646cf8f4bf4711cbcf327d466bcce070f48ce8f32f7f6"},
        {"indent4.svg", "5340adc6b4219f2fc28f19cf772b1a978fcae655d66492db3dfc2134c3269e77"},
        {"indent4.svgz", "387cd0895ee4c8382f8c2f184f16d2fb7e1d7229611a805ff855e4b5f415955f"},
        {"clean-rm.svg", "6506f1d84629cb1a3f6629726566041a18bca21621593f3495fe6ba1add64e75"},
        {"clean-rm.svgz", "b3983e134c0177cc21ce39c55312fcadbc19fc1d86f53dbb4e3d21f52a0d7115"},
        {"clean-rm-off.svg", "b5cda24cd7982c6d897484f4fe062431ca0e39ca3df793c85c1ec3cc7d3e40ce"},
        {"clean-rm-off.svgz", "942ae356851ad5460d907abb5860dbb4db6c59f3e95b809b7611c94d24ba7fc3"},
        {"clean-warn.svg", "b5cda24cd7982c6d897484f4fe062431ca0e39ca3df793c85c1ec3cc7d3e40ce"},
        {"clean-warn.svgz", "942ae356851ad5460d907abb5860dbb4db6c59f3e95b809b7611c94d24ba7fc3"},
        {"sort.svg", "9ea6703ba30232da0872ba98615d08f64330abc9b146c80055a3365f84ba9594"},
        {"sort.svgz", "d4a950454a2a73cb6115749b3b2a30bbc680960368047b94cad833e92e90d14c"},
        {"sort-off.svg", "b5cda24cd7982c6d897484f4fe062431ca0e39ca3df793c85c1ec3cc7d3e40ce"},
        {"sort-off.svgz", "942ae356851ad5460d907abb5860dbb4db6c59f3e95b809b7611c94d24ba7fc3"},
        {"absref.svg", "b5cda24cd7982c6d897484f4fe062431ca0e39ca3df793c85c1ec3cc7d3e40ce"},
        {"absref.svgz", "942ae356851ad5460d907abb5860dbb4db6c59f3e95b809b7611c94d24ba7fc3"},
    };

    // Fixture built once: a relative linked image, an embedded data URI, and one
    // instance of each feature the pinned digests are meant to cover.
    std::string const png = png_1x1();
    ASSERT_TRUE(write_file(path("linked.png"), png));
    gchar *b64_raw = g_base64_encode(reinterpret_cast<guchar const *>(png.data()), png.size());
    ASSERT_NE(b64_raw, nullptr);
    std::string const b64 = b64_raw;
    g_free(b64_raw);

    // Only two newlines in the whole document: after the XML declaration and
    // after the leading comment. Every other break in the packet listing is
    // readability only.
    std::string const xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
        "<!-- leading comment -->\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\""
        " xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\""
        " xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\""
        " xmlns:foo=\"http://example.com/foo\" width=\"40\" height=\"20\" viewBox=\"0 0 40 20\" version=\"1.1\">"
        "<title>Parity</title>"
        "<style id=\"st\"><![CDATA[ rect { fill: red; } ]]></style>"
        "<g id=\"g1\" foo:custom=\"kept\" data-x=\"1\">"
        "<rect id=\"r1\" x=\"1\" y=\"2\" width=\"3\" height=\"4\""
        " style=\"fill:#ff0000;stroke:none;bogus-prop:1;opacity:1\" bogus=\"x\" transform=\"translate(1,2)\"/>"
        "</g>"
        "<text id=\"t1\" xml:space=\"preserve\" x=\"5\" y=\"15\">  two  spaces "
        "<tspan id=\"ts1\" sodipodi:role=\"line\">line</tspan></text>"
        "<!-- inner comment -->"
        "<unknown:thing xmlns:unknown=\"http://example.com/unknown\" id=\"u1\" unknown:attr=\"v\"/>"
        "<image id=\"linked\" xlink:href=\"linked.png\" width=\"5\" height=\"5\"/>"
        "<image id=\"embedded\" xlink:href=\"data:image/png;base64," + b64 + "\" width=\"5\" height=\"5\"/>"
        "</svg>";

    doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()), path("source.svg"));
    ASSERT_NE(doc, nullptr);
    doc->ensureUpToDate();
    // Match every other fixture in this file: start dirty so step 5 observes the
    // save copies preserving the live dirty flag rather than the load default.
    doc->setModifiedSinceSave(true);
    ASSERT_EQ(g_mkdir_with_parents(path("other").c_str(), 0700), 0);

    bool const modified_before = doc->isModifiedSinceSave();

    for (auto const &combo : combos) {
        for (char const *suffix : {".svg", ".svgz"}) {
            // Step a: every one of the twelve preferences is set explicitly.
            prefs->setBool("/options/svgoutput/inlineattrs", combo.inlineattrs);
            prefs->setInt("/options/svgoutput/indent", combo.indent);
            prefs->setBool("/options/svgoutput/check_on_writing", combo.check);
            prefs->setBool("/options/svgoutput/sort_attributes", combo.sort);
            prefs->setBool("/options/svgoutput/disable_optimizations", combo.disopt);
            prefs->setBool("/options/svgoutput/incorrect_attributes_warn", combo.warn);
            prefs->setBool("/options/svgoutput/incorrect_attributes_remove", combo.rm);
            prefs->setBool("/options/svgoutput/incorrect_style_properties_warn", combo.warn);
            prefs->setBool("/options/svgoutput/incorrect_style_properties_remove", combo.rm);
            prefs->setBool("/options/svgoutput/style_defaults_warn", combo.warn);
            prefs->setBool("/options/svgoutput/style_defaults_remove", combo.rm);
            prefs->setBool("/options/svgoutput/usesodipodiabsref", combo.absref);

            std::string const destination = path(std::string("other/parity") + suffix);
            auto *module = get_output_extension_for_save(destination);
            ASSERT_NE(module, nullptr);
            std::string const key = std::string(combo.name) + suffix;

            // Step c: baseline direct writer.
            bool const remembered_gui = module->get_gui();
            module->set_gui(false);
            ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), destination.c_str(), false, false,
                                                      Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY));
            std::string const direct = read_file(destination);
            ASSERT_FALSE(direct.empty());

            // Step d: buffered interactive route, then restore the module gui.
            ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), destination.c_str(), false, false,
                                                      Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY,
                                                      {}, false, nullptr, true));
            std::string const buffered = read_file(destination);
            EXPECT_EQ(buffered, direct) << key;
            module->set_gui(remembered_gui);

            // Step e: text is the direct bytes, decompressed for .svgz.
            std::string text;
            if (std::string_view(suffix) == ".svgz") {
                ASSERT_GE(direct.size(), 2u);
                EXPECT_EQ(static_cast<unsigned char>(direct[0]), 0x1f) << key;
                EXPECT_EQ(static_cast<unsigned char>(direct[1]), 0x8b) << key;
                z_stream z{};
                ASSERT_EQ(inflateInit2(&z, 16 + MAX_WBITS), Z_OK) << key;
                z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(direct.data()));
                z.avail_in = static_cast<uInt>(direct.size());
                char chunk[64 * 1024];
                int ret = Z_OK;
                do {
                    z.next_out = reinterpret_cast<Bytef *>(chunk);
                    z.avail_out = sizeof(chunk);
                    ret = inflate(&z, Z_NO_FLUSH);
                    text.append(chunk, sizeof(chunk) - z.avail_out);
                } while (ret == Z_OK);
                int const final_ret = ret;
                inflateEnd(&z);
                ASSERT_EQ(final_ret, Z_STREAM_END) << key;
            } else {
                text = direct;
            }

            // Step f: normalize only the build revision.
            auto const stamp = text.find("inkscape:version=\"");
            if (stamp != std::string::npos) {
                auto const stamp_end = text.find('"', stamp + sizeof("inkscape:version=\"") - 1);
                ASSERT_NE(stamp_end, std::string::npos) << key;
                text.replace(stamp, stamp_end - stamp + 1, "inkscape:version=\"BUILD\"");
            }

            // Step g: digest of the normalized text.
            gchar *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                reinterpret_cast<guchar const *>(text.data()), text.size());
            ASSERT_NE(digest, nullptr) << key;
            std::string const actual_digest = digest;
            g_free(digest);

            // Step h: compare against the pinned table.
            std::string_view expected_digest;
            bool found = false;
            for (auto const &entry : expected) {
                if (entry.first == key) {
                    expected_digest = entry.second;
                    found = true;
                    break;
                }
            }
            ASSERT_TRUE(found) << key;
            EXPECT_EQ(actual_digest, std::string(expected_digest)) << key;
            if (g_getenv("VACARDS_PRINT_PARITY_DIGESTS")) {
                std::cout << "PARITY " << key << " " << actual_digest << "\n";
            }

            // Step i: prove the fixture really exercises the intended features.
            if (std::string_view(combo.name) == "base") {
                EXPECT_NE(text.find("<![CDATA["), std::string::npos) << key;
                EXPECT_NE(text.find("<!-- inner comment -->"), std::string::npos) << key;
                EXPECT_NE(text.find("xml:space=\"preserve\""), std::string::npos) << key;
                EXPECT_NE(text.find("foo:custom=\"kept\""), std::string::npos) << key;
                EXPECT_NE(text.find("unknown:attr=\"v\""), std::string::npos) << key;
                EXPECT_NE(text.find("../linked.png"), std::string::npos) << key;
                EXPECT_NE(text.find("data:image/png;base64,"), std::string::npos) << key;
            }
        }
    }

    // Step 5: save copies never change the persisted dirty flag.
    EXPECT_TRUE(doc->isModifiedSinceSave() == modified_before);
}

// Save slice 2 / P1: the UI-thread prepare step and the GC-free writer must
// produce byte-identical output to the checked save entry point for the same
// document, options and href bases, and the writer must not mutate the tree.
// The linked fixture exercises href rebasing and injected xmlns attributes.
TEST_F(SaveTargetTest, PreparedWriterMatchesSaveOutputStream)
{
    make_linked_document();
    ASSERT_EQ(g_mkdir_with_parents(path("other").c_str(), 0700), 0);

    char const *const old_base = doc->getDocumentBase();
    std::string const new_base = path("other");

    for (bool compress : {false, true}) {
        Inkscape::IO::BufferOutputStream a;
        Inkscape::IO::BufferOutputStream b;

        ASSERT_NO_THROW(sp_repr_save_output_stream(doc->getReprDoc(), a, SP_SVG_NS_URI, compress,
                                                   old_base, new_base.c_str()));
        auto const options = sp_repr_capture_serializer_options();
        auto const plan = sp_repr_prepare_serializer_plan(doc->getReprDoc(), SP_SVG_NS_URI);
        ASSERT_NO_THROW(sp_repr_write_prepared(doc->getReprDoc(), b, compress, options, plan,
                                               old_base, new_base.c_str()));

        EXPECT_FALSE(a.getBuffer().empty());
        EXPECT_EQ(a.getBuffer(), b.getBuffer()) << "compress=" << compress;
    }

    // The writer allocates no GC memory and must not mutate the tree: a second
    // write that reuses an already-computed plan leaves the root attribute
    // count unchanged.
    auto const options = sp_repr_capture_serializer_options();
    auto const plan = sp_repr_prepare_serializer_plan(doc->getReprDoc(), SP_SVG_NS_URI);
    auto const root_attributes_before = doc->getReprRoot()->attributeList().size();
    Inkscape::IO::BufferOutputStream c;
    ASSERT_NO_THROW(sp_repr_write_prepared(doc->getReprDoc(), c, false, options, plan,
                                           old_base, new_base.c_str()));
    EXPECT_EQ(root_attributes_before, doc->getReprRoot()->attributeList().size());
}

TEST_F(SaveTargetTest, NativeTestHooksIgnoredUntilOptIn)
{
    auto const target = normalized("hooks-disabled");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    g_setenv("VACARDS_SAVE_TEST_BUFFER_FAILURE", "1", TRUE);
    g_setenv("VACARDS_SAVE_TEST_BUFFER_CAP", "4", TRUE);
    std::string notice;
    EXPECT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, &notice, true));
    g_unsetenv("VACARDS_SAVE_TEST_BUFFER_FAILURE");
    g_unsetenv("VACARDS_SAVE_TEST_BUFFER_CAP");
    EXPECT_EQ(notice, "");
    EXPECT_NE(read_file(target).find("bug002"), std::string::npos);
}

#ifdef __APPLE__
TEST_F(SaveTargetTest, NativeBufferCapUsesDirectRoute)
{
    Inkscape::IO::enable_file_io_test_hooks();
    for (auto const suffix : {".svg", ".svgz"}) {
        for (bool existing : {false, true}) {
            auto const target = path(std::string(existing ? "cap-existing" : "cap-new") + suffix);
            auto *module = get_output_extension_for_save(target);
            ASSERT_NE(module, nullptr);
            ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, false,
                Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY));
            auto const direct = read_file(target);
            ASSERT_FALSE(direct.empty());
            if (!existing) ASSERT_EQ(g_remove(target.c_str()), 0);
            auto loaded = existing ? SPDocument::createNewDoc(target.c_str(), false) : nullptr;
            auto unknown = existing ? nullptr
                : SPDocument::createNewDocFromMem(std::span<char const>(kSvg, std::strlen(kSvg)));
            SPDocument *saving_doc = existing ? loaded.get() : unknown.get();
            ASSERT_NE(saving_doc, nullptr);
            std::string notice;
            g_setenv("VACARDS_SAVE_TEST_BUFFER_CAP", "4", TRUE);
            auto *svg = dynamic_cast<Inkscape::Extension::Internal::Svg *>(module->get_imp());
            ASSERT_NE(svg, nullptr);
            auto const planned = svg->begin_publication(saving_doc, target.c_str(),
                                                        saving_doc->lastKnownSerializedSize());
            // Save slice 2: an unknown size is serialized on the publication
            // side, which discovers the cap and streams instead (counted below).
            EXPECT_EQ(planned.route, existing ? "direct_size" : "owned_bytes");
            EXPECT_EQ(planned.deferred.has_value(), !existing);
            Inkscape::Extension::Internal::reset_native_serialization_count_for_testing();
            EXPECT_NO_THROW(Inkscape::Extension::save(module, saving_doc, target.c_str(), false, false,
                Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, {}, false, &notice, true));
            g_unsetenv("VACARDS_SAVE_TEST_BUFFER_CAP");
            EXPECT_EQ(read_file(target), direct);
            EXPECT_EQ(notice, "");
            EXPECT_EQ(Inkscape::Extension::Internal::native_serialization_count_for_testing(),
                existing ? 1u : 2u);
            auto reopened = SPDocument::createNewDoc(target.c_str(), false);
            EXPECT_NE(reopened, nullptr);
            if (reopened) EXPECT_NE(reopened->getObjectById("bug002"), nullptr);
        }
    }
}

TEST_F(SaveTargetTest, CapFallbackRecordsHintForNextOfficialSave)
{
    auto const target = path("cap-hint.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    Inkscape::IO::enable_file_io_test_hooks();
    g_setenv("VACARDS_SAVE_TEST_BUFFER_CAP", "4", TRUE);
    struct ResetCap { ~ResetCap() { g_unsetenv("VACARDS_SAVE_TEST_BUFFER_CAP"); } } reset_cap;
    Inkscape::Extension::Internal::reset_native_serialization_count_for_testing();
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    EXPECT_EQ(Inkscape::Extension::Internal::native_serialization_count_for_testing(), 2u);
    EXPECT_EQ(doc->lastKnownSerializedSize(), read_file(target).size());
    EXPECT_GT(doc->lastKnownSerializedSize(), 4u);
    Inkscape::Extension::Internal::reset_native_serialization_count_for_testing();
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    EXPECT_EQ(Inkscape::Extension::Internal::native_serialization_count_for_testing(), 1u);
}
#endif

TEST_F(SaveTargetTest, NativeInteractiveStageFailurePreservesExistingFile)
{
    auto const target = normalized("stage-failure");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(write_file(target, "complete original"));
    Inkscape::IO::enable_file_io_test_hooks();
    g_setenv("VACARDS_SAVE_TEST_STAGE_FAILURE", "1", TRUE);
    std::string notice;
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, false,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, {}, false, &notice, true),
        Inkscape::Extension::Output::save_failed);
    g_unsetenv("VACARDS_SAVE_TEST_STAGE_FAILURE");
    EXPECT_EQ(notice, "");
    EXPECT_EQ(read_file(target), "complete original");
}

TEST_F(SaveTargetTest, LastKnownSizeTracksOnlyOfficialNativePublishedBytes)
{
    auto const copy = path("size-copy.svg");
    auto const official = path("size-official.svg");
    auto *module = get_output_extension_for_save(official);
    ASSERT_NE(module, nullptr);
    doc->setLastKnownSerializedSize(777);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), copy.c_str(), false, false,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, {}, false, nullptr, true));
    ASSERT_FALSE(read_file(copy).empty());
    EXPECT_EQ(doc->lastKnownSerializedSize(), 777u);
#ifdef __APPLE__
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), official.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    ASSERT_FALSE(read_file(official).empty());
    EXPECT_EQ(doc->lastKnownSerializedSize(), read_file(official).size());
#endif
}

#ifdef __APPLE__
TEST_F(SaveTargetTest, TransientReserveFailureReturnsToBufferedRoute)
{
    auto const target = path("reserve-recovery.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    Inkscape::IO::enable_file_io_test_hooks();
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    doc->setLastKnownSerializedSize(777);
    g_setenv("VACARDS_SAVE_TEST_RESERVE_FAILURE", "1", TRUE);
    struct ResetReserveFailure { ~ResetReserveFailure() { g_unsetenv("VACARDS_SAVE_TEST_RESERVE_FAILURE"); } } reset_reserve_failure;
    auto *svg = dynamic_cast<Inkscape::Extension::Internal::Svg *>(module->get_imp());
    ASSERT_NE(svg, nullptr);
    auto planned = svg->begin_publication(doc.get(), target.c_str(), doc->lastKnownSerializedSize());
    // Save slice 2: the reservation happens where the snapshot is serialized;
    // a failed reservation streams through the checked stage writer instead.
    EXPECT_EQ(planned.route, "owned_bytes");
    EXPECT_FALSE(planned.direct_size_hint.has_value());
    ASSERT_TRUE(planned.deferred.has_value());
    EXPECT_TRUE(planned.deferred->inject_reserve_failure);
    auto const reserved = Inkscape::Extension::Internal::publish(std::move(planned));
    EXPECT_EQ(reserved.route, "direct_allocation");
    EXPECT_EQ(reserved.outcome, Inkscape::Extension::Internal::PublicationOutcome::Published);
    EXPECT_EQ(reserved.serialized_size, std::optional<std::size_t>(read_file(target).size()));
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    g_unsetenv("VACARDS_SAVE_TEST_RESERVE_FAILURE");
    ASSERT_EQ(doc->lastKnownSerializedSize(), read_file(target).size());
    auto next = svg->begin_publication(doc.get(), target.c_str(), doc->lastKnownSerializedSize());
    EXPECT_EQ(next.route, "owned_bytes");
    {
        // Planned route alone no longer proves buffering (serialization is
        // deferred): the publication itself must stay on the buffered route.
        auto const next_owner = std::move(next.snapshot_owner);
        auto const published = Inkscape::Extension::Internal::publish(std::move(next));
        EXPECT_EQ(published.route, "owned_bytes");
        EXPECT_EQ(published.outcome, Inkscape::Extension::Internal::PublicationOutcome::Published);
    }
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    EXPECT_EQ(doc->lastKnownSerializedSize(), read_file(target).size());
}

TEST_F(SaveTargetTest, ShrunkDirectSaveReturnsToBufferedRoute)
{
    auto const target = path("shrunk-direct.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    Inkscape::IO::enable_file_io_test_hooks();
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    constexpr std::size_t cap = 100000;
    doc->setLastKnownSerializedSize(cap + 1);
    g_setenv("VACARDS_SAVE_TEST_BUFFER_CAP", "100000", TRUE);
    struct ResetCap { ~ResetCap() { g_unsetenv("VACARDS_SAVE_TEST_BUFFER_CAP"); } } reset_cap;
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    ASSERT_LT(read_file(target).size(), cap);
    ASSERT_EQ(doc->lastKnownSerializedSize(), read_file(target).size());
    auto *svg = dynamic_cast<Inkscape::Extension::Internal::Svg *>(module->get_imp());
    ASSERT_NE(svg, nullptr);
    auto next = svg->begin_publication(doc.get(), target.c_str(), doc->lastKnownSerializedSize());
    EXPECT_EQ(next.route, "owned_bytes");
    {
        // Planned route alone no longer proves buffering (serialization is
        // deferred): the publication itself must stay on the buffered route.
        auto const next_owner = std::move(next.snapshot_owner);
        auto const published = Inkscape::Extension::Internal::publish(std::move(next));
        EXPECT_EQ(published.route, "owned_bytes");
        EXPECT_EQ(published.outcome, Inkscape::Extension::Internal::PublicationOutcome::Published);
    }
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
}
#endif

#ifdef __APPLE__
TEST_F(SaveTargetTest, ReadOnlyNativeSaveDefersAdmissionAndKeepsCleanDocument)
{
    auto const target = path("read-only-native.svg");
    ASSERT_TRUE(write_file(target, "complete original"));
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    doc->setModifiedSinceSave(false);
    auto const attempt = doc->saveAttemptGeneration();
    g_setenv("VACARDS_SAVE_TEST_BUFFER_CAP", "4", TRUE);
    ASSERT_EQ(::chmod(target.c_str(), 0400), 0);
    bool read_only = false;
    try {
        Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
            Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true);
    } catch (Inkscape::Extension::Output::file_read_only const &) {
        read_only = true;
    } catch (...) {}
    ASSERT_EQ(::chmod(target.c_str(), 0600), 0);
    g_unsetenv("VACARDS_SAVE_TEST_BUFFER_CAP");
    EXPECT_TRUE(read_only);
    EXPECT_GT(doc->saveAttemptGeneration(), attempt);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_EQ(read_file(target), "complete original");
}
#endif

#ifdef __APPLE__
TEST_F(SaveTargetTest, NativeDirectFallbackStageFailurePreservesExistingFile)
{
    auto const target = normalized("direct-stage-failure");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(write_file(target, "complete original"));
    Inkscape::IO::enable_file_io_test_hooks();
    Inkscape::Extension::Internal::reset_native_serialization_count_for_testing();
    g_setenv("VACARDS_SAVE_TEST_BUFFER_CAP", "4", TRUE);
    g_setenv("VACARDS_SAVE_TEST_STAGE_FAILURE", "1", TRUE);
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, false,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, {}, false, nullptr, true),
        Inkscape::Extension::Output::save_failed);
    g_unsetenv("VACARDS_SAVE_TEST_STAGE_FAILURE");
    g_unsetenv("VACARDS_SAVE_TEST_BUFFER_CAP");
    EXPECT_EQ(Inkscape::Extension::Internal::native_serialization_count_for_testing(), 1u);
    EXPECT_EQ(read_file(target), "complete original");
}
#endif

#ifndef __APPLE__
TEST_F(SaveTargetTest, NativeInteractiveReadOnlyRefusesBeforeSaveAttempt)
{
    auto const target = path("non-apple-read-only.svg");
    ASSERT_TRUE(write_file(target, "complete original"));
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    doc->setModifiedSinceSave(false);
    auto const attempt = doc->saveAttemptGeneration();
    ASSERT_EQ(g_chmod(target.c_str(), 0400), 0);
    struct RestoreMode {
        std::string path;
        ~RestoreMode() { g_chmod(path.c_str(), 0600); }
    } restore{target};
    ASSERT_FALSE(Inkscape::IO::file_is_writable(target.c_str()));
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true),
        Inkscape::Extension::Output::file_read_only);
    EXPECT_EQ(doc->saveAttemptGeneration(), attempt);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_EQ(read_file(target), "complete original");
}
#endif

TEST_F(SaveTargetTest, SavedOlderRevisionDialogContainsOnlyUserNotice)
{
    EXPECT_EQ(saved_older_revision_dialog_text("drawing.svg", ""),
        "An earlier version was saved to drawing.svg, but newer changes are still unsaved. "
        "Save again before closing this document.");
    EXPECT_EQ(saved_older_revision_dialog_text("drawing.svg", "retained recovery copy"),
        "An earlier version was saved to drawing.svg, but newer changes are still unsaved. "
        "Save again before closing this document.\n\nretained recovery copy");
}

#ifdef __APPLE__
TEST_F(SaveTargetTest, StalePublishedSaveShowsRetainedRecoveryNotice)
{
    make_linked_document();
    auto const target = path("stale-notice.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    Inkscape::IO::enable_file_io_test_hooks();
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    doc->getObjectById("art")->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"stale notice edit"}, "");
    std::string notice;
    g_setenv("VACARDS_SAVE_TEST_STALE_AFTER_PUBLISH", "1", TRUE);
    {
        ScopedRecoveryDeleteFailureInjection const fault(EACCES);
        EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
            Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, &notice, true),
            Inkscape::Extension::PublishedStaleDocument);
    }
    g_unsetenv("VACARDS_SAVE_TEST_STALE_AFTER_PUBLISH");
    ASSERT_NE(notice.find("recovery copy"), std::string::npos);
    auto const dialog = saved_stale_document_dialog_text("stale-notice.svg", "stale snapshot", notice);
    EXPECT_NE(dialog.find("recovery copy"), std::string::npos);
    EXPECT_NE(dialog.find("stale snapshot"), std::string::npos);
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, BufferedReadOnlyAdmissionKeepsCleanStaleDocument)
{
    auto const target = path("read-only-stale.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    Inkscape::IO::enable_file_io_test_hooks();
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    ASSERT_EQ(g_chmod(target.c_str(), 0400), 0);
    struct RestoreMode {
        std::string path;
        ~RestoreMode() { g_chmod(path.c_str(), 0600); }
    } restore{target};
    ASSERT_FALSE(Inkscape::IO::file_is_writable(target.c_str()));
    g_setenv("VACARDS_SAVE_TEST_STALE_AFTER_PUBLISH", "1", TRUE);
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true),
        Inkscape::Extension::Output::file_read_only);
    g_unsetenv("VACARDS_SAVE_TEST_STALE_AFTER_PUBLISH");
    EXPECT_FALSE(doc->isModifiedSinceSave());
}
#endif

#ifdef __APPLE__
TEST_F(SaveTargetTest, NativeSerializedBytesKeepOldFileOnPublicationFailure)
{
    auto const target = normalized("publish-failure");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(write_file(target, "complete original"));
    Inkscape::IO::detail::set_rename_failure_errno_for_testing(EACCES);
    std::string notice;
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, false,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, {}, false, &notice, true),
        Inkscape::Extension::Output::save_failed);
    Inkscape::IO::detail::set_rename_failure_errno_for_testing(0);
    EXPECT_EQ(notice, "");
    EXPECT_EQ(read_file(target), "complete original");
}
#endif

#ifdef __APPLE__
TEST_F(SaveTargetTest, NativeBufferFailureDoesNotPublish)
{
    auto const target = normalized("buffer-failure");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(write_file(target, "complete original"));
    Inkscape::IO::enable_file_io_test_hooks();
    g_setenv("VACARDS_SAVE_TEST_BUFFER_FAILURE", "1", TRUE);
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true),
        Inkscape::Extension::Output::save_failed);
    EXPECT_EQ(read_file(target), "complete original");
    // A direct caller with gui=false, and TEMPORARY even with a requested
    // buffer flag, still use the direct writer.
    InkFileExportCmd exporter;
    exporter.export_filename = path("export-do.svg");
    exporter.export_type = "svg";
    exporter.do_export(doc.get(), path("source.svg"));
    ASSERT_FALSE(exporter.had_export_failure());
    EXPECT_FALSE(module->get_gui());
    EXPECT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, false,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY));
    EXPECT_NE(read_file(target).find("bug002"), std::string::npos);
    auto const temporary = normalized("temporary");
    EXPECT_NO_THROW(Inkscape::Extension::save(module, doc.get(), temporary.c_str(), false, false,
        Inkscape::Extension::FILE_SAVE_METHOD_TEMPORARY, {}, false, nullptr, true));
    EXPECT_NE(read_file(temporary).find("bug002"), std::string::npos);
    // export-do mutates this shared module flag. Explicit Save routing remains
    // independent of its value in the same process.
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, false,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, {}, false, nullptr, true),
        Inkscape::Extension::Output::save_failed);
    g_unsetenv("VACARDS_SAVE_TEST_BUFFER_FAILURE");
}
#endif

#ifdef __APPLE__
TEST_F(SaveTargetTest, OwnedPublicationJobResultsWithoutDocument)
{
    using namespace Inkscape::Extension::Internal;
    Inkscape::IO::enable_file_io_test_hooks();
    auto make_job = [&](std::string const &name) {
        PublicationJob job;
        job.test_hooks_enabled = true;
        job.absolute_path = path(name);
        job.bytes.resize(std::strlen(kSvg));
        std::memcpy(job.bytes.data(), kSvg, job.bytes.size());
        return job;
    };

    auto fresh = make_job("job-new.svg");
    doc.reset(); // Publication has no live SPDocument or XML to consult.
    auto saved = publish(std::move(fresh));
    EXPECT_EQ(saved.outcome, PublicationOutcome::Published);
    EXPECT_EQ(saved.route, "owned_bytes");
    EXPECT_EQ(saved.notice, "");
    EXPECT_EQ(read_file(path("job-new.svg")), kSvg);

    ASSERT_TRUE(write_file(path("job-existing.svg"), "previous"));
    saved = publish(make_job("job-existing.svg"));
    EXPECT_EQ(saved.outcome, PublicationOutcome::Published);
    EXPECT_EQ(saved.route, "owned_bytes");
    EXPECT_EQ(saved.notice, "");
    EXPECT_EQ(read_file(path("job-existing.svg")), kSvg);

    auto write_failure = make_job("job-write-failure.svg");
    write_failure.inject_stage_write_failure = true;
    auto failed = publish(std::move(write_failure));
    EXPECT_EQ(failed.outcome, PublicationOutcome::Failed);
    EXPECT_EQ(failed.route, "owned_bytes");
    EXPECT_EQ(failed.notice, "");
    EXPECT_FALSE(g_file_test(path("job-write-failure.svg").c_str(), G_FILE_TEST_EXISTS));

    auto publication_failure = make_job("job-publish-failure.svg");
    publication_failure.test_hooks_enabled = true;
    publication_failure.stage_hooks[4].failure = PublicationOutcome::Failed;
    failed = publish(std::move(publication_failure));
    EXPECT_EQ(failed.outcome, PublicationOutcome::Failed);
    EXPECT_EQ(failed.error, "injected pre-publication failure");
    EXPECT_EQ(failed.route, "owned_bytes");
    EXPECT_EQ(failed.notice, "");
    EXPECT_FALSE(g_file_test(path("job-publish-failure.svg").c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(SaveTargetTest, OwnedPublicationRunsOnWorkerThread)
{
    using namespace Inkscape::Extension::Internal;
    PublicationJob job;
    job.absolute_path = path("thread-published.svg");
    job.bytes.resize(std::strlen(kSvg));
    std::memcpy(job.bytes.data(), kSvg, job.bytes.size());
    PublicationResult result;
    std::thread worker([&result, job = std::move(job)]() mutable {
        result = publish(std::move(job));
    });
    worker.join();
    EXPECT_EQ(result.outcome, PublicationOutcome::Published);
    EXPECT_EQ(result.route, "owned_bytes");
    EXPECT_EQ(result.notice, "");
    EXPECT_EQ(read_file(path("thread-published.svg")), kSvg);
}

TEST_F(SaveTargetTest, PublicationJobFlagsRequireOptIn)
{
    using namespace Inkscape::Extension::Internal;
    PublicationJob job;
    job.absolute_path = path("flags-disabled.svg");
    job.bytes.resize(std::strlen(kSvg));
    std::memcpy(job.bytes.data(), kSvg, job.bytes.size());
    job.inject_stage_write_failure = true;
    job.stage_hooks[4].failure = PublicationOutcome::Failed;
    auto result = publish(std::move(job));
    EXPECT_EQ(result.outcome, PublicationOutcome::Published);
    EXPECT_EQ(result.route, "owned_bytes");
    EXPECT_EQ(result.notice, "");
    EXPECT_EQ(read_file(path("flags-disabled.svg")), kSvg);
}

TEST_F(SaveTargetTest, OwnedPublicationUsesCapturedHooksAfterGlobalReset)
{
    using namespace Inkscape::Extension::Internal;
    Inkscape::IO::enable_file_io_test_hooks();
    auto const target = path("job-captured-hook.svg");
    ASSERT_TRUE(write_file(target, "previous"));
    PublicationJob job;
    job.absolute_path = target;
    job.bytes.resize(std::strlen(kSvg));
    std::memcpy(job.bytes.data(), kSvg, job.bytes.size());
    Inkscape::IO::detail::set_rename_failure_errno_for_testing(EACCES);
    job.existing_file_options = Inkscape::IO::capture_existing_file_options(false);
    Inkscape::IO::detail::set_rename_failure_errno_for_testing(0);
    doc.reset();
    auto const result = publish(std::move(job));
    EXPECT_EQ(result.outcome, PublicationOutcome::Failed);
    EXPECT_EQ(result.route, "owned_bytes");
    EXPECT_EQ(result.notice, "");
    EXPECT_EQ(read_file(target), "previous");
}

#endif

#ifdef _WIN32
TEST_F(SaveTargetTest, BeginCapturesWindowsReplaceHookForWorkerPublication)
{
    using namespace Inkscape::Extension::Internal;
    Inkscape::IO::enable_file_io_test_hooks();
    auto const target = path("begin-worker-hook.svg");
    ASSERT_TRUE(write_file(target, "previous"));
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    auto *svg = dynamic_cast<Svg *>(module->get_imp());
    ASSERT_NE(svg, nullptr);
    Inkscape::IO::detail::set_replace_fault_for_testing(5, 1);
    auto job = svg->begin_publication(doc.get(), target.c_str());
    Inkscape::IO::detail::set_replace_fault_for_testing(0, 0);
    ASSERT_EQ(job.route, "owned_bytes");
    // The snapshot owner stays on this (initiating) thread, as in production.
    auto const snapshot_owner = std::move(job.snapshot_owner);
    PublicationResult result;
    std::thread worker([&result, job = std::move(job)]() mutable {
        result = publish(std::move(job));
    });
    worker.join();
    EXPECT_EQ(result.outcome, PublicationOutcome::Failed);
    EXPECT_EQ(read_file(target), "previous");
}
#endif

TEST_F(SaveTargetTest, OptInNativeSaveMeasurement)
{
    auto const *size_text = g_getenv("VACARDS_SAVE_MEASURE_MIB");
    auto const *directory = g_getenv("VACARDS_SAVE_MEASURE_DIR");
    if (!size_text || !directory) GTEST_SKIP() << "opt-in measurement";
    auto const *fixture = g_getenv("VACARDS_SAVE_MEASURE_FIXTURE");
    char *end = nullptr;
    auto const mib = std::strtoul(size_text, &end, 10);
    ASSERT_TRUE(end && *end == '\0');
    ASSERT_TRUE(mib == 1 || mib == 50 || mib == 250 || mib == 200);
#ifdef __APPLE__
    rusage before{};
    ASSERT_EQ(getrusage(RUSAGE_SELF, &before), 0);
#endif
    if (fixture) {
        doc = SPDocument::createNewDoc(fixture, false);
    } else {
        std::string xml = "<svg xmlns='http://www.w3.org/2000/svg'><desc>";
        xml.append(mib * 1024 * 1024 - xml.size() - 6, 'x');
        xml += "</desc></svg>";
        doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
    }
    ASSERT_NE(doc, nullptr);
    doc->ensureUpToDate();
    doc->setModifiedSinceSave(true);
    auto const target = Glib::build_filename(directory, std::string("synthetic-") + size_text + ".svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    auto const prior_gui = module->get_gui();
    std::string route_notice;
    auto const begin = std::chrono::steady_clock::now();
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, &route_notice, true));
    auto const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::fprintf(stderr, "VACARDS_SAVE_MEASUREMENT save_ms=%.3f route=%s\n", ms, route_notice.c_str());
    module->set_gui(prior_gui);
    GStatBuf file_stat{};
    ASSERT_EQ(g_stat(target.c_str(), &file_stat), 0);
    std::fprintf(stderr, "VACARDS_SAVE_MEASUREMENT mib=%lu output_bytes=%lld\n",
                 mib, static_cast<long long>(file_stat.st_size));
#ifdef __APPLE__
    rusage after{};
    ASSERT_EQ(getrusage(RUSAGE_SELF, &after), 0);
    std::fprintf(stderr, "VACARDS_SAVE_MEASUREMENT peak_rss_delta_bytes=%lld peak_rss_bytes=%lld\n",
                 static_cast<long long>(after.ru_maxrss - before.ru_maxrss),
                 static_cast<long long>(after.ru_maxrss));
#endif
}

// Ordinary Save (file.cpp passes check_overwrite=false) must not consult the
// confirmation callback at all; Save As (check_overwrite=true) must consult it
// and, on confirm, move the document identity to the new path and clear dirt.
TEST_F(SaveTargetTest, OrdinarySaveAndSaveAsSuccessfulState)
{
    std::string const baseline = normalized("baseline");
    auto *module = get_output_extension_for_save(baseline);
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const allow = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), baseline.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, allow));
    doc->setModifiedSinceSave(true);

    // Ordinary Save: callback must be unreachable and must not be able to abort.
    bool ordinary_called = false;
    Inkscape::Extension::OverwriteConfirm const refuse =
        [&ordinary_called](std::string const &) { ordinary_called = true; return false; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), baseline.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, refuse));
    EXPECT_FALSE(ordinary_called);
    EXPECT_STREQ(doc->getDocumentFilename(), baseline.c_str());
    EXPECT_FALSE(doc->isModifiedSinceSave());

    // Save As to a new path: callback is consulted and the identity moves.
    std::string const renamed = normalized("renamed");
    std::string seen;
    Inkscape::Extension::OverwriteConfirm const confirm =
        [&seen](std::string const &f) { seen = f; return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), renamed.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    EXPECT_EQ(seen, renamed);
    EXPECT_TRUE(g_file_test(renamed.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_STREQ(doc->getDocumentFilename(), renamed.c_str());
    EXPECT_FALSE(doc->isModifiedSinceSave());
}

// Unsupported suffixes resolve no module, and an output failure during an
// official save must roll the document identity and modified state back.
TEST_F(SaveTargetTest, UnsupportedAndFailedSavePreserveDocument)
{
    std::string const baseline = normalized("baseline");
    auto *module = get_output_extension_for_save(baseline);
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const allow = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), baseline.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, allow));
    std::string const uri_before = doc->getDocumentFilename();
    doc->setModifiedSinceSave(true);

    // Unsupported suffix: never reaches save and never mutates the document.
    EXPECT_EQ(get_output_extension_for_save(path("drawing.blarg")), nullptr);
    EXPECT_EQ(std::string(doc->getDocumentFilename()), uri_before);
    EXPECT_TRUE(doc->isModifiedSinceSave());

    // A missing directory is created by the SVG writer. Use a regular file as
    // the parent instead, making the destination invalid without permissions
    // assumptions (also works when tests run as an administrator).
    auto const blocked_parent = path("not-a-directory");
    ASSERT_TRUE(g_file_set_contents(blocked_parent.c_str(), "sentinel", -1, nullptr));
    std::string const failed = Glib::build_filename(blocked_parent, "drawing.svg");
    auto *failed_module = get_output_extension_for_save(failed);
    ASSERT_NE(failed_module, nullptr);
    EXPECT_THROW(Inkscape::Extension::save(failed_module, doc.get(), failed.c_str(), true, true,
                                           Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, allow),
                 Inkscape::Extension::Output::save_failed);
    EXPECT_EQ(std::string(doc->getDocumentFilename()), uri_before);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_FALSE(g_file_test(failed.c_str(), G_FILE_TEST_EXISTS));
}

// ---------------------------------------------------------------------------
// F2 save-state transaction: native failures must not touch the live document
// ---------------------------------------------------------------------------

TEST_F(SaveTargetTest, FailedOfficialSvgSavePreservesLiveState)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);
    ASSERT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);
    expect_failed_save_preserves_live(module, /*check_overwrite=*/true, /*official=*/true, /*sync_title=*/false);
}

TEST_F(SaveTargetTest, FailedOrdinarySvgSavePreservesLiveState)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);
    expect_failed_save_preserves_live(module, /*check_overwrite=*/false, /*official=*/true, /*sync_title=*/false);
}

// Title normalization on an ordinary (check_overwrite=false) official save is
// the remaining matrix cell: the failed write must not commit the RDF title.
TEST_F(SaveTargetTest, FailedOrdinarySvgTitleSyncPreservesLiveState)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);
    ASSERT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);
    expect_failed_save_preserves_live(module, /*check_overwrite=*/false, /*official=*/true, /*sync_title=*/true);
}

TEST_F(SaveTargetTest, FailedOfficialSvgzSavePreservesLiveState)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svgz"));
    ASSERT_NE(module, nullptr);
    ASSERT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVGZ_INKSCAPE);
    expect_failed_save_preserves_live(module, /*check_overwrite=*/true, /*official=*/true, /*sync_title=*/false);
}

TEST_F(SaveTargetTest, FailedOrdinarySvgzSavePreservesLiveState)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svgz"));
    ASSERT_NE(module, nullptr);
    expect_failed_save_preserves_live(module, /*check_overwrite=*/false, /*official=*/true, /*sync_title=*/false);
}

TEST_F(SaveTargetTest, FailedSaveCopyPreservesLiveState)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);
    // Dialog-path Save Copy also requests title normalization, which must not
    // reach the live document when the write fails.
    expect_failed_save_preserves_live(module, /*check_overwrite=*/true, /*official=*/false, /*sync_title=*/true);
}

// ---------------------------------------------------------------------------
// F2 save-state transaction: success commits identity/title only after writing
// ---------------------------------------------------------------------------

TEST_F(SaveTargetTest, OfficialSaveRebasesLinkedImageAndCommitsState)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);
    ASSERT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);
    EXPECT_FALSE(module->causes_dataloss());

    std::string const target = path("out/drawing.svg");
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                              /*sync_title=*/true));

    EXPECT_TRUE(g_file_test(target.c_str(), G_FILE_TEST_EXISTS));
    // Live identity and href were committed only after the successful write.
    EXPECT_STREQ(doc->getDocumentFilename(), target.c_str());
    ASSERT_NE(doc->getDocumentBase(), nullptr);
    EXPECT_EQ(std::string(doc->getDocumentBase()), Glib::path_get_dirname(target));
    EXPECT_FALSE(doc->isModifiedSinceSave());
    ASSERT_NE(doc->getObjectById("img"), nullptr);
    EXPECT_STREQ(doc->getObjectById("img")->getAttribute("xlink:href"), "../linked.png");
    doc->ensureUpToDate();
    expect_usable_1x1_rgba(*doc, "img");
    EXPECT_FALSE(doc->getReprRoot()->attribute("inkscape:dataloss"));
    // Title normalization was committed to the live document and prefs.
    ASSERT_NE(rdf_find_entity("title"), nullptr);
    EXPECT_STREQ(rdf_get_work_entity(doc.get(), rdf_find_entity("title")), "Linked artwork");
    EXPECT_STREQ(Inkscape::Preferences::get()->getString("/dialogs/save_as/default").c_str(), module->get_id());

    // Reopen the written file: the literal relative href resolves back to the
    // same asset from the new directory, and the synced title is in the file.
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    reopened->ensureUpToDate();
    ASSERT_NE(reopened->getObjectById("img"), nullptr);
    EXPECT_STREQ(reopened->getObjectById("img")->getAttribute("xlink:href"), "../linked.png");
    expect_usable_1x1_rgba(*reopened, "img");
    EXPECT_STREQ(rdf_get_work_entity(reopened.get(), rdf_find_entity("title")), "Linked artwork");
    // Artwork structure is unchanged by the output pipeline.
    ASSERT_NE(reopened->getObjectById("art"), nullptr);
}

TEST_F(SaveTargetTest, SaveCopySuccessKeepsLiveStateAndWritesRebasedCopy)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);

    auto const live_before = fingerprint(*doc);
    char const *live_href = doc->getObjectById("img")->getAttribute("xlink:href");
    std::string const live_href_before = live_href ? live_href : "";
    doc->setModifiedSinceSave(true);

    std::string const copy = path("copies/copy.svg");
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), copy.c_str(), true, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, confirm,
                                              /*sync_title=*/true));

    // Live document is byte-for-byte identical: no identity, href, title,
    // dataloss, dirty or save-anchor change.
    EXPECT_EQ(fingerprint(*doc), live_before);
    char const *live_href_after = doc->getObjectById("img")->getAttribute("xlink:href");
    EXPECT_EQ(live_href_before, live_href_after ? live_href_after : "");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(rdf_get_work_entity(doc.get(), rdf_find_entity("title")), nullptr);

    // The written copy exists and carries the rebased reference and the synced
    // title (dialog-path opt-in), with the relative meaning intact.
    EXPECT_TRUE(g_file_test(copy.c_str(), G_FILE_TEST_EXISTS));
    auto reopened = SPDocument::createNewDoc(copy.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    reopened->ensureUpToDate();
    ASSERT_NE(reopened->getObjectById("img"), nullptr);
    EXPECT_STREQ(reopened->getObjectById("img")->getAttribute("xlink:href"), "../linked.png");
    expect_usable_1x1_rgba(*reopened, "img");
    EXPECT_STREQ(rdf_get_work_entity(reopened.get(), rdf_find_entity("title")), "Linked artwork");
}

TEST_F(SaveTargetTest, OfficialSvgzSaveReopensWithHrefAndTitle)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svgz"));
    ASSERT_NE(module, nullptr);
    ASSERT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVGZ_INKSCAPE);

    std::string const target = path("svgz/out.svgz");
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                              /*sync_title=*/true));
    EXPECT_TRUE(g_file_test(target.c_str(), G_FILE_TEST_EXISTS));
    ASSERT_NE(doc->getObjectById("img"), nullptr);
    EXPECT_STREQ(doc->getObjectById("img")->getAttribute("xlink:href"), "../linked.png");
    EXPECT_FALSE(doc->isModifiedSinceSave());

    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    reopened->ensureUpToDate();
    ASSERT_NE(reopened->getObjectById("img"), nullptr);
    EXPECT_STREQ(reopened->getObjectById("img")->getAttribute("xlink:href"), "../linked.png");
    EXPECT_STREQ(rdf_get_work_entity(reopened.get(), rdf_find_entity("title")), "Linked artwork");
}

TEST_F(SaveTargetTest, ExistingSvgzSaveReplacesAndReopensWithLinkedImage)
{
    make_linked_document();
    auto const folder = path("existing-svgz");
    ASSERT_EQ(g_mkdir_with_parents(folder.c_str(), 0700), 0);
    auto const target = Glib::build_filename(folder, "drawing.svgz");
    ASSERT_TRUE(write_file(target, "SENTINEL"));
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_STREQ(module->get_id(), SP_MODULE_KEY_OUTPUT_SVGZ_INKSCAPE);

    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                              /*sync_title=*/true));
    auto const content = read_file(target);
    ASSERT_GE(content.size(), 2u);
    EXPECT_EQ(static_cast<unsigned char>(content[0]), 0x1f);
    EXPECT_EQ(static_cast<unsigned char>(content[1]), 0x8b);
    EXPECT_FALSE(doc->isModifiedSinceSave());

    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    reopened->ensureUpToDate();
    ASSERT_NE(reopened->getObjectById("img"), nullptr);
    EXPECT_STREQ(reopened->getObjectById("img")->getAttribute("xlink:href"), "../linked.png");
    expect_usable_1x1_rgba(*reopened, "img");
    EXPECT_STREQ(rdf_get_work_entity(reopened.get(), rdf_find_entity("title")), "Linked artwork");
}

// Save Copy must not move the EventLog saved anchor; Undo back to the official
// save point must be recognized as clean.
TEST_F(SaveTargetTest, SaveCopyDoesNotMoveSavedUndoAnchor)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"saved-anchor setup history"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());

    std::string const saved = path("saved.svg");
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), saved.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                              /*sync_title=*/false));
    ASSERT_FALSE(doc->isModifiedSinceSave());

    // New undoable user edit after the official save.
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"post-save user edit"}, "");
    EXPECT_TRUE(doc->isModifiedSinceSave());

    // Save Copy must leave the saved anchor where the official save put it.
    std::string const copy = path("copy/saved.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), copy.c_str(), true, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, confirm,
                                              /*sync_title=*/true));
    EXPECT_TRUE(doc->isModifiedSinceSave());

    // Undo returns to the exact saved state -> clean, with the authored value.
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "1");
    EXPECT_FALSE(doc->isModifiedSinceSave());

    // Redo restores the user edit and dirties again.
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "6");
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, CompletedSaveAnchorsUndoPosition)
{
    make_linked_document();
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"anchor baseline"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    auto *log = doc->get_event_log();
    auto const at_begin = log->getCurrEventSerial();
    auto const target = path("captured-undo.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"edit during publication"}, "");
    ASSERT_NE(log->getCurrEventSerial(), at_begin);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(log->getCurrEventSerial(), at_begin);
    EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, SplitSavePathPreservesRootSpellings)
{
    using Inkscape::IO::split_save_path;
    auto check = [](std::string const &path, std::string const &parent, std::string const &name) {
        auto const parts = split_save_path(path);
        ASSERT_TRUE(parts.has_value()) << path;
        EXPECT_EQ(parts->parent, parent);
        EXPECT_EQ(parts->name, name);
    };
    check("/file.svg", "/", "file.svg");
    check("/directory/file.svg", "/directory", "file.svg");
#ifndef _WIN32
    check("/d/a\\b.svg", "/d", "a\\b.svg");
#else
    check("/d/a\\b.svg", "/d/a", "b.svg");
#endif
#ifdef _WIN32
    check("C:\\file.svg", "C:\\", "file.svg");
    check("C:/file.svg", "C:/", "file.svg");
    check("\\\\server\\share\\file.svg", "\\\\server\\share\\", "file.svg");
    EXPECT_FALSE(split_save_path("C:\\").has_value());
#endif
    EXPECT_FALSE(split_save_path("/").has_value());
}

#ifdef __APPLE__
TEST_F(SaveTargetTest, NativeSavePreservesBackslashInFilename)
{
    auto const target = path("a\\b.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    EXPECT_TRUE(g_file_test(target.c_str(), G_FILE_TEST_IS_REGULAR));
    EXPECT_FALSE(g_file_test(path("a").c_str(), G_FILE_TEST_IS_DIR));
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    reopened->ensureUpToDate();
    EXPECT_NE(reopened->getObjectById("bug002"), nullptr);
}
#endif

TEST_F(SaveTargetTest, SaveUndoNewEditDiscardsSavedAnchor)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("saved.svg"));
    ASSERT_NE(module, nullptr);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    auto *art = doc->getObjectById("art");
    art->getRepr()->setAttribute("x", "2");
    DocumentUndo::done(doc.get(), ContextString{"saved edit"}, "");
    auto *log = doc->get_event_log();
    auto const saved_serial = log->getCurrEventSerial();
    auto const target = path("saved.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    art->getRepr()->setAttribute("x", "3");
    DocumentUndo::done(doc.get(), ContextString{"branch edit"}, "");
    EXPECT_FALSE(log->findEventBySerial(saved_serial));
    EXPECT_FALSE(log->hasFileSaveAnchor());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, SavedChildOfPrunedRootNeverMarksAnotherStateClean)
{
    make_linked_document();
    DocumentUndo::setUndoSensitive(doc.get(), true);
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    auto edit = [&](char const *value, char const *label, char const *type) {
        art->getRepr()->setAttribute("x", value);
        DocumentUndo::done(doc.get(), ContextString{label}, type);
    };
    edit("2", "A", "type-x");
    edit("3", "B1", "type-y");
    edit("4", "B2", "type-y");
    auto *log = doc->get_event_log();
    auto const saved_serial = log->getCurrEventSerial();
    auto const target = path("saved-child.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    ASSERT_TRUE(log->hasFileSaveAnchor());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
    edit("5", "C", "type-z");
    EXPECT_FALSE(log->findEventBySerial(saved_serial));
    EXPECT_FALSE(log->hasFileSaveAnchor());
    EXPECT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, ExpiredOldestRootCannotRestoreOldSaveState)
{
    make_linked_document();
    auto *prefs = Inkscape::Preferences::get();
    auto const old_limit = prefs->getBool("/options/undo/limit");
    auto const old_size = prefs->getInt("/options/undo/size", 200);
    struct RestoreUndoLimit {
        Inkscape::Preferences *prefs;
        bool limit;
        int size;
        ~RestoreUndoLimit() {
            prefs->setBool("/options/undo/limit", limit);
            prefs->setInt("/options/undo/size", size);
        }
    } restore{prefs, old_limit, old_size};
    prefs->setBool("/options/undo/limit", true);
    prefs->setInt("/options/undo/size", 1);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    auto *log = doc->get_event_log();
    auto const original_serial = log->getCurrEventSerial();
    auto const target = path("saved-root.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "2");
    DocumentUndo::done(doc.get(), ContextString{"first root"}, "type-x");
    art->getRepr()->setAttribute("x", "3");
    DocumentUndo::done(doc.get(), ContextString{"second root"}, "type-y");
    EXPECT_NE((*log->getEventListStore()->children().begin())[
                  Inkscape::EventLog::getColumns().serial], original_serial);
    EXPECT_FALSE(log->findEventBySerial(original_serial));
    EXPECT_FALSE(log->hasFileSaveAnchor());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

// Crash regression: with undo size 1 a same-type expiry repurposes the only
// root row; that must not crash and must leave undo/redo usable. It asserts no
// saved-state semantics, so it is named for the crash it guards.
TEST_F(SaveTargetTest, UndoSizeOneSameTypeExpiryDoesNotCrash)
{
    make_linked_document();
    auto *prefs = Inkscape::Preferences::get();
    auto const old_limit = prefs->getBool("/options/undo/limit");
    auto const old_size = prefs->getInt("/options/undo/size", 200);
    struct RestoreUndoLimit {
        Inkscape::Preferences *prefs;
        bool limit;
        int size;
        ~RestoreUndoLimit() {
            prefs->setBool("/options/undo/limit", limit);
            prefs->setInt("/options/undo/size", size);
        }
    } restore{prefs, old_limit, old_size};
    prefs->setBool("/options/undo/limit", true);
    prefs->setInt("/options/undo/size", 1);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "2");
    DocumentUndo::done(doc.get(), ContextString{"first same type"}, "type-y");
    art->getRepr()->setAttribute("x", "3");
    DocumentUndo::done(doc.get(), ContextString{"second same type"}, "type-y");
    auto *log = doc->get_event_log();
    EXPECT_TRUE(log->getCurrEvent());
    EXPECT_STREQ(art->getAttribute("x"), "3");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_STREQ(art->getAttribute("x"), "2");
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(art->getAttribute("x"), "3");
}

TEST_F(SaveTargetTest, SaveResetsActionKeySoNextKeyedEditStartsFreshRow)
{
    make_linked_document();
    DocumentUndo::setUndoSensitive(doc.get(), true);
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);

    // First keyed edit: opens a new Undo row and arms the coalescing key.
    art->getRepr()->setAttribute("x", "2");
    DocumentUndo::maybeDone(doc.get(), "save-anchor-nudge", ContextString{"first nudge"}, "");
    auto *log = doc->get_event_log();
    auto const first_row_serial = log->getCurrEventSerial();

    // A real official save anchors that row on disk.
    auto const target = path("keyed-anchor.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    ASSERT_TRUE(log->hasFileSaveAnchor());

    // A second keyed edit with the SAME key inside the expiry window must not
    // merge into the saved row; it must open a fresh Undo row.
    art->getRepr()->setAttribute("x", "3");
    DocumentUndo::maybeDone(doc.get(), "save-anchor-nudge", ContextString{"second nudge"}, "");
    EXPECT_NE(log->getCurrEventSerial(), first_row_serial);

    // Undo returns to the saved row (clean); redo must report dirty because the
    // disk still holds only the first nudge.
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, UndoBackToInitialRowIsClean)
{
    // The constructor anchors the starting row, so a document that is edited
    // and then undone back to the start is not modified since save.
    DocumentUndo::setUndoSensitive(doc.get(), true);
    auto *art = doc->getObjectById("bug002");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "2");
    DocumentUndo::done(doc.get(), ContextString{"constructor-anchor edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, UndoExpiryMergeInvalidatesSavedParent)
{
    make_linked_document();
    auto *prefs = Inkscape::Preferences::get();
    auto const old_limit = prefs->getBool("/options/undo/limit");
    auto const old_size = prefs->getInt("/options/undo/size", 200);
    struct RestoreUndoLimit {
        Inkscape::Preferences *prefs;
        bool limit;
        int size;
        ~RestoreUndoLimit() {
            prefs->setBool("/options/undo/limit", limit);
            prefs->setInt("/options/undo/size", size);
        }
    } restore{prefs, old_limit, old_size};
    prefs->setBool("/options/undo/limit", true);
    prefs->setInt("/options/undo/size", 1);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    auto *art = doc->getObjectById("art");
    art->getRepr()->setAttribute("x", "2");
    DocumentUndo::done(doc.get(), ContextString{"first edit"}, "");
    auto *log = doc->get_event_log();
    auto const saved_serial = log->getCurrEventSerial();
    auto const target = path("expiry.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    art->getRepr()->setAttribute("x", "3");
    DocumentUndo::done(doc.get(), ContextString{"second edit"}, "");
    EXPECT_FALSE(log->findEventBySerial(saved_serial));
    EXPECT_FALSE(log->hasFileSaveAnchor());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, MissingBeginRowInvalidatesAnchorAtCompletion)
{
    make_linked_document();
    DocumentUndo::setUndoSensitive(doc.get(), true);
    auto *art = doc->getObjectById("art");
    art->getRepr()->setAttribute("x", "2");
    DocumentUndo::done(doc.get(), ContextString{"row to expire"}, "");
    auto *log = doc->get_event_log();
    auto const begin_serial = log->getCurrEventSerial();
    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Expiry Output</name><id>org.vacards.test.expiry-output</id>"
        "<output><extension>.expiry</extension><mimetype>application/x-vacards-expiry</mimetype>"
        "<filetypename>Expiry Output</filetypename></output></inkscape-extension>";
    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.expiry-output"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.expiry-output"));
    }
    ASSERT_NE(module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(module->get_imp());
    observer->original = doc.get();
    observer->failure = ObservingOutputImplementation::Failure::PublishAndEdit;
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "published snapshot"));
        auto row = log->getCurrEvent();
        Inkscape::Event *event = (*row)[Inkscape::EventLog::getColumns().event];
        log->notifyUndoExpired(event);
    };
    auto const target = path("expired.expiry");
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    EXPECT_FALSE(log->findEventBySerial(begin_serial));
    EXPECT_FALSE(log->hasFileSaveAnchor());
    doc->setModifiedSinceSave(true);
    log->seekTo(log->getCurrEvent());
    EXPECT_TRUE(doc->isModifiedSinceSave());
    observer->publish_and_edit = {};
    observer->original = nullptr;
}

TEST_F(SaveTargetTest, TemplateSaveBumpsAttemptGeneration)
{
    auto const target = path("template.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    auto const before = doc->saveAttemptGeneration();
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, false,
        Inkscape::Extension::FILE_SAVE_METHOD_INKSCAPE_SVG));
    EXPECT_EQ(doc->saveAttemptGeneration(), before + 1);
}

// ---------------------------------------------------------------------------
// F2 preserves output processing, direct Output::save defaults and title opt-in
// ---------------------------------------------------------------------------

TEST_F(SaveTargetTest, OutputProcessingActionsRunOnlyOnTheCopy)
{
    make_linked_document();
    auto *prefs = Inkscape::Preferences::get();
    // The set-svg-version-1 processing action is gated by
    // '/dialogs/save_as/enable_svgexport' with a leading '!' default of false;
    // enable it explicitly so the version conversion is actually exercised.
    prefs->setBool("/dialogs/save_as/enable_svgexport", true);
    doc->getReprRoot()->setAttribute("version", "2.0");
    doc->ensureUpToDate();

    auto *plain = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get(SP_MODULE_KEY_OUTPUT_SVG));
    ASSERT_NE(plain, nullptr);
    ASSERT_TRUE(plain->causes_dataloss());

    std::string const target = path("plain/out.svg");
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(plain, doc.get(), target.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                              /*sync_title=*/false));

    // The version conversion ran on the output copy only.
    EXPECT_NE(read_file(target).find("version=\"1.1\""), std::string::npos);
    // The live document kept its version and only gained the format dataloss.
    EXPECT_STREQ(doc->getReprRoot()->attribute("version"), "2.0");
    EXPECT_STREQ(doc->getReprRoot()->attribute("inkscape:dataloss"), "true");
    EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, StagedSerializerUsesLogicalSvgFormatAndHrefBase)
{
    auto const destination_dir = path("destination");
    ASSERT_EQ(g_mkdir_with_parents(destination_dir.c_str(), 0700), 0);

    for (bool linked : {false, true}) {
        if (linked) {
            make_linked_document();
        }
        for (bool compressed : {false, true}) {
            std::string const suffix = compressed ? ".svgz" : ".svg";
            std::string const logical = Glib::build_filename(destination_dir,
                                                                linked ? "diseño-linked" + suffix : "new" + suffix);
            // The staging file deliberately has no format suffix and lives in
            // another directory. Neither detail may affect output bytes.
            std::string const stage = path(linked ? "linked-stage" + suffix + ".tmp"
                                                  : "new-stage" + suffix + ".tmp");
            char const *old_base = doc->getDocumentBase();
            ASSERT_TRUE(sp_repr_save_rebased_file(doc->getReprDoc(), logical.c_str(), SP_SVG_NS_URI,
                                                 old_base, logical.c_str()));

            auto const options = sp_repr_rebased_save_options(logical.c_str(), old_base, logical.c_str());
            EXPECT_EQ(options.compress, compressed);
            EXPECT_EQ(options.new_href_abs_base, destination_dir);
            FILE *stream = g_fopen(stage.c_str(), "wb");
            ASSERT_NE(stream, nullptr);
            sp_repr_save_stream(doc->getReprDoc(), stream, SP_SVG_NS_URI, options.compress,
                                options.old_href_abs_base.c_str(), options.new_href_abs_base.c_str());
            ASSERT_EQ(fclose(stream), 0);
            auto const direct_bytes = read_file(logical);
            ASSERT_FALSE(direct_bytes.empty()) << logical;
            EXPECT_EQ(read_file(stage), direct_bytes) << logical;
            if (compressed) {
                ASSERT_GE(direct_bytes.size(), 2u);
                EXPECT_EQ(static_cast<unsigned char>(direct_bytes[0]), 0x1f);
                EXPECT_EQ(static_cast<unsigned char>(direct_bytes[1]), 0x8b);
            }
        }
    }
}

TEST_F(SaveTargetTest, DirectOutputSaveKeepsDefaults)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);

    auto const before = fingerprint(*doc);
    std::string const filename_before = doc->getDocumentFilename() ? doc->getDocumentFilename() : "";

    // Three-argument surface (existing symbol/default) and explicit empty
    // preparation are identical and mutate nothing on the passed document.
    std::string const target_a = path("direct/a.svg");
    ASSERT_NO_THROW(module->save(doc.get(), target_a.c_str()));
    EXPECT_TRUE(g_file_test(target_a.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(fingerprint(*doc), before);
    EXPECT_EQ(filename_before, doc->getDocumentFilename() ? doc->getDocumentFilename() : "");
    EXPECT_FALSE(doc->getReprRoot()->attribute("inkscape:dataloss"));
    EXPECT_EQ(rdf_get_work_entity(doc.get(), rdf_find_entity("title")), nullptr);

    std::string const target_b = path("direct/b.svg");
    ASSERT_NO_THROW(module->save(doc.get(), target_b.c_str(), false, Inkscape::Extension::SavePreparation{}));
    EXPECT_TRUE(g_file_test(target_b.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(fingerprint(*doc), before);
}

TEST_F(SaveTargetTest, OrdinarySaveDoesNotSyncTitleByDefault)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);
    // No RDF title exists, even though svg:title does.
    EXPECT_EQ(rdf_get_work_entity(doc.get(), rdf_find_entity("title")), nullptr);

    std::string const target = path("ordinary/out.svg");
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));

    // The six/seven-argument entry points default sync_title to false.
    EXPECT_EQ(rdf_get_work_entity(doc.get(), rdf_find_entity("title")), nullptr);
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(rdf_get_work_entity(reopened.get(), rdf_find_entity("title")), nullptr);
}

// The production architecture must defer all live mutation until after the
// implementation returns. A test-only implementation observes the original while
// Output::save() is running and throws each failure kind. This needs no extra
// production seam: build_from_mem() is the existing registration API.
TEST_F(SaveTargetTest, OutputFailureNeverMutatesOriginalBeforeWrite)
{
    make_linked_document();

    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
        "  <name>Observing Output</name>\n"
        "  <id>org.vacards.test.observing-output</id>\n"
        "  <output>\n"
        "    <extension>.obs</extension>\n"
        "    <mimetype>image/x-vacards-observing</mimetype>\n"
        "    <filetypename>Observing Output</filetypename>\n"
        "    <filetypetooltip>Test-only output</filetypetooltip>\n"
        "  </output>\n"
        "</inkscape-extension>\n";

    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.observing-output"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.observing-output"));
    }
    ASSERT_NE(module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();

    auto const original_before = fingerprint(*doc);
    std::string const filename_before = doc->getDocumentFilename() ? doc->getDocumentFilename() : "";
    bool const modified_before = doc->isModifiedSinceSave();
    bool const autosave_before = doc->isModifiedSinceAutoSave();
    auto const pref_before = Inkscape::Preferences::get()->getString("/dialogs/save_as/default");

    // Observe XML, filename, dirty and preference state from inside the writer.
    observer->original_unchanged = [&]() {
        return fingerprint(*doc) == original_before
            && std::string(doc->getDocumentFilename() ? doc->getDocumentFilename() : "") == filename_before
            && doc->isModifiedSinceSave() == modified_before
            && doc->isModifiedSinceAutoSave() == autosave_before
            && Inkscape::Preferences::get()->getString("/dialogs/save_as/default") == pref_before;
    };

    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    std::string const target = path("observing/out.obs");

    for (bool check_overwrite : {true, false}) {
        for (auto failure : {ObservingOutputImplementation::Failure::SaveFailed,
                             ObservingOutputImplementation::Failure::SaveCancelled,
                             ObservingOutputImplementation::Failure::Unknown}) {
            observer->failure = failure;
            observer->observed_original_unchanged_during_save = false;
            observer->saw_distinct_projection = false;
            // Each iteration must surface the exact exception kind: a different
            // type escaping EXPECT_THROW is itself a failure.
            switch (failure) {
            case ObservingOutputImplementation::Failure::SaveFailed:
                EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), check_overwrite, true,
                                                       Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                                       /*sync_title=*/true),
                             Inkscape::Extension::Output::save_failed);
                break;
            case ObservingOutputImplementation::Failure::SaveCancelled:
                EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), check_overwrite, true,
                                                       Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                                       /*sync_title=*/true),
                             Inkscape::Extension::Output::save_cancelled);
                break;
            case ObservingOutputImplementation::Failure::Unknown:
                // Non-std exception: proves the catch-all rollback category is
                // exercised without any std::exception requirement.
                EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), check_overwrite, true,
                                                       Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                                       /*sync_title=*/true),
                             ObserverUnknown);
                break;
            case ObservingOutputImplementation::Failure::PublishAndEdit:
                FAIL() << "Publication is tested separately";
                break;
            case ObservingOutputImplementation::Failure::SaveUnsupported:
            case ObservingOutputImplementation::Failure::SaveConflict:
            case ObservingOutputImplementation::Failure::SaveUncertain:
            case ObservingOutputImplementation::Failure::SaveUncertainPathAvailable:
                FAIL() << "Typed save outcomes are tested separately";
                break;
            }
            EXPECT_TRUE(observer->saw_distinct_projection);
            EXPECT_TRUE(observer->observed_original_unchanged_during_save);
            EXPECT_EQ(fingerprint(*doc), original_before);
            EXPECT_EQ(filename_before, doc->getDocumentFilename() ? doc->getDocumentFilename() : "");
            EXPECT_EQ(doc->isModifiedSinceSave(), modified_before);
            EXPECT_EQ(doc->isModifiedSinceAutoSave(), autosave_before);
            EXPECT_EQ(Inkscape::Preferences::get()->getString("/dialogs/save_as/default"), pref_before);
        }
    }

    // The singleton test extension must not retain a callback or a document
    // pointer once this test returns.
    observer->original = nullptr;
    observer->original_unchanged = {};
}

// The three typed save outcomes are small plain classes. They must escape
// Extension::save unchanged with their payloads and must skip every post-save
// live commit (identity, dirty state, Undo anchor). This is the wrapper contract
// the real Svg::save classification relies on.
TEST_F(SaveTargetTest, TypedSaveOutcomesSurviveExtensionSaveAndStayDirty)
{
    make_linked_document();

    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
        "  <name>Observing Output</name>\n"
        "  <id>org.vacards.test.observing-output</id>\n"
        "  <output>\n"
        "    <extension>.obs</extension>\n"
        "    <mimetype>image/x-vacards-observing</mimetype>\n"
        "    <filetypename>Observing Output</filetypename>\n"
        "    <filetypetooltip>Test-only output</filetypetooltip>\n"
        "  </output>\n"
        "</inkscape-extension>\n";

    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.observing-output"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.observing-output"));
    }
    ASSERT_NE(module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();

    auto const before = fingerprint(*doc);
    std::string const filename_before = doc->getDocumentFilename() ? doc->getDocumentFilename() : "";
    bool const modified_before = doc->isModifiedSinceSave();
    auto const pref_before = Inkscape::Preferences::get()->getString("/dialogs/save_as/default");
    observer->original_unchanged = [&]() {
        return fingerprint(*doc) == before
            && std::string(doc->getDocumentFilename() ? doc->getDocumentFilename() : "") == filename_before
            && doc->isModifiedSinceSave() == modified_before
            && Inkscape::Preferences::get()->getString("/dialogs/save_as/default") == pref_before;
    };

    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    std::string const target = path("typed/out.obs");

    auto save_once = [&]() {
        observer->observed_original_unchanged_during_save = false;
        observer->saw_distinct_projection = false;
        Inkscape::Extension::save(module, doc.get(), target.c_str(), true, true,
                                  Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                  /*sync_title=*/true);
    };
    auto expect_live_unchanged = [&]() {
        EXPECT_TRUE(observer->saw_distinct_projection);
        EXPECT_TRUE(observer->observed_original_unchanged_during_save);
        EXPECT_EQ(fingerprint(*doc), before);
        EXPECT_EQ(filename_before, doc->getDocumentFilename() ? doc->getDocumentFilename() : "");
        EXPECT_EQ(doc->isModifiedSinceSave(), modified_before);
        EXPECT_EQ(Inkscape::Preferences::get()->getString("/dialogs/save_as/default"), pref_before);
    };

    observer->failure = ObservingOutputImplementation::Failure::SaveUnsupported;
    try {
        save_once();
        FAIL() << "save_unsupported did not escape Extension::save";
    } catch (Inkscape::Extension::Output::save_unsupported const &e) {
        EXPECT_EQ(e.error, "observed unsupported");
    }
    expect_live_unchanged();

    observer->failure = ObservingOutputImplementation::Failure::SaveConflict;
    try {
        save_once();
        FAIL() << "save_conflict did not escape Extension::save";
    } catch (Inkscape::Extension::Output::save_conflict const &e) {
        EXPECT_EQ(e.error, "observed conflict");
    }
    expect_live_unchanged();

    // Unavailable recovery: a path may be carried but must never be marked
    // available on the producer's say-so alone.
    observer->failure = ObservingOutputImplementation::Failure::SaveUncertain;
    observer->uncertain_recovery_path = path("typed/retained.obs");
    try {
        save_once();
        FAIL() << "save_uncertain did not escape Extension::save";
    } catch (Inkscape::Extension::Output::save_uncertain const &e) {
        EXPECT_EQ(e.error, "observed uncertain");
        EXPECT_EQ(e.recovery_path, path("typed/retained.obs"));
        EXPECT_FALSE(e.recovery_path_available);
    }
    expect_live_unchanged();

    // Empty path: no recovery location is fabricated.
    observer->uncertain_recovery_path.clear();
    try {
        save_once();
        FAIL() << "save_uncertain did not escape Extension::save";
    } catch (Inkscape::Extension::Output::save_uncertain const &e) {
        EXPECT_TRUE(e.recovery_path.empty());
        EXPECT_FALSE(e.recovery_path_available);
    }
    expect_live_unchanged();

    // Path availability is only ever signalled by the producer; it never claims
    // the retained bytes are complete.
    observer->failure = ObservingOutputImplementation::Failure::SaveUncertainPathAvailable;
    observer->uncertain_recovery_path = path("typed/available.obs");
    try {
        save_once();
        FAIL() << "save_uncertain did not escape Extension::save";
    } catch (Inkscape::Extension::Output::save_uncertain const &e) {
        EXPECT_EQ(e.recovery_path, path("typed/available.obs"));
        EXPECT_TRUE(e.recovery_path_available);
    }
    expect_live_unchanged();

    // Blocker-1 compatibility contract exercised through the real wrapper: the
    // non-GUI callers export.cpp and file-export-cmd.cpp catch only
    // Output::save_failed, so each typed outcome must be caught by that exact
    // handler. The catch(...) after it means "not catchable as save_failed".
    for (auto failure : {ObservingOutputImplementation::Failure::SaveUnsupported,
                         ObservingOutputImplementation::Failure::SaveConflict,
                         ObservingOutputImplementation::Failure::SaveUncertain}) {
        observer->failure = failure;
        observer->uncertain_recovery_path.clear();
        try {
            save_once();
            FAIL() << "typed save outcome bypassed the save_failed handler";
        } catch (Inkscape::Extension::Output::save_failed const &) {
            // Expected: this is the handler the non-GUI callers use.
        } catch (...) {
            FAIL() << "typed save outcome was not catchable as save_failed";
        }
        expect_live_unchanged();
    }

    observer->uncertain_recovery_path.clear();
    observer->original = nullptr;
    observer->original_unchanged = {};
}

TEST_F(SaveTargetTest, PublishedOlderSnapshotDoesNotClearLaterEdits)
{
    make_linked_document();
    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Revision Output</name><id>org.vacards.test.revision-output</id>"
        "<output><extension>.rev</extension><mimetype>image/x-vacards-revision</mimetype>"
        "<filetypename>Revision Output</filetypename></output></inkscape-extension>";
    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    }
    ASSERT_NE(module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();
    observer->failure = ObservingOutputImplementation::Failure::PublishAndEdit;

    auto const prior_name = std::string(doc->getDocumentFilename());
    auto const prior_pref = Inkscape::Preferences::get()->getString("/dialogs/save_as/default");
    auto const target = path("earlier.rev");
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete earlier snapshot"));
        doc->getObjectById("art")->getRepr()->setAttribute("x", "9");
    };
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    observer->publication_notice = "retained previous version at test location";
    std::string notice;
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
                                          Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS,
                                          confirm, false, &notice),
                 Inkscape::Extension::PublishedOlderRevision);
    EXPECT_EQ(notice, observer->publication_notice);
    observer->publication_notice.clear();
    EXPECT_EQ(read_file(target), "complete earlier snapshot");
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "9");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(std::string(doc->getDocumentFilename()), prior_name);
    EXPECT_EQ(Inkscape::Preferences::get()->getString("/dialogs/save_as/default"), prior_pref);

    // A Save Copy has no official saved-state anchor and remains a successful
    // copy even if the live document changes during output.
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete earlier snapshot"));
        doc->getObjectById("art")->getRepr()->setAttribute("x", "10");
    };
    auto const copy = path("copy.rev");
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), copy.c_str(), false, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, confirm));
    EXPECT_EQ(read_file(copy), "complete earlier snapshot");
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "10");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(std::string(doc->getDocumentFilename()), prior_name);

    // A non-Undo invalidation can leave XML unchanged. Its own generation
    // still prevents this older snapshot from becoming the saved anchor.
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete earlier snapshot"));
        doc->setModifiedSinceSave(true);
    };
    auto const invalidation_target = path("invalidation.rev");
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), invalidation_target.c_str(), false, true,
                                          Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::PublishedOlderRevision);
    EXPECT_EQ(read_file(invalidation_target), "complete earlier snapshot");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(std::string(doc->getDocumentFilename()), prior_name);

    // A different official path after publication cannot receive this save's
    // live preparation or clean state.
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"stale path baseline"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    auto *native_module = get_output_extension_for_save(prior_name);
    ASSERT_NE(native_module, nullptr);
    ASSERT_NO_THROW(Inkscape::Extension::save(native_module, doc.get(), prior_name.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    doc->getObjectById("art")->getRepr()->setAttribute("x", "11");
    DocumentUndo::done(doc.get(), ContextString{"stale path user edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());
    auto const moved_name = path("moved.rev");
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete earlier snapshot"));
        doc->setDocumentFilename(moved_name.c_str());
    };
    auto const path_changed_target = path("path-changed.rev");
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), path_changed_target.c_str(), false, true,
                                          Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::PublishedStaleDocument);
    EXPECT_EQ(std::string(doc->getDocumentFilename()), moved_name);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());

    // Rebase changes the document instance without changing its address.
    auto const replacement = path("replacement.svg");
    ASSERT_TRUE(write_file(replacement, kSvg));
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete earlier snapshot"));
        ASSERT_TRUE(doc->rebase(replacement.c_str()));
    };
    auto const generation_before = doc->saveInstanceGeneration();
    auto const replaced_target = path("replaced.rev");
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), replaced_target.c_str(), false, true,
                                          Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::PublishedStaleDocument);
    EXPECT_NE(doc->saveInstanceGeneration(), generation_before);

    native_module = get_output_extension_for_save(path("nested.svg"));
    ASSERT_NE(native_module, nullptr);
    DocumentUndo::done(doc.get(), ContextString{"superseded baseline"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    auto const superseded_official = path("superseded-official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(native_module, doc.get(), superseded_official.c_str(),
        false, true, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    doc->getObjectById("bug002")->getRepr()->setAttribute("width", "7");
    DocumentUndo::done(doc.get(), ContextString{"superseded user edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());
    auto const nested_target = path("nested.svg");
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete earlier snapshot"));
        ASSERT_NO_THROW(Inkscape::Extension::save(native_module, doc.get(), nested_target.c_str(), false,
            false, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY));
    };
    auto const superseded_target = superseded_official;
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), superseded_target.c_str(), false, true,
                                          Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::PublishedStaleDocument);
    EXPECT_EQ(read_file(superseded_target), "complete earlier snapshot");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());

    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete earlier snapshot"));
        doc.reset();
    };
    auto const closed_target = path("closed.rev");
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), closed_target.c_str(), false, true,
                                          Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::PublishedStaleDocument);
    EXPECT_EQ(doc, nullptr);

    observer->publish_and_edit = {};
    observer->original = nullptr;
}

// P2 oracle: an official save that publishes an older snapshot (S1) while the
// live document keeps changing throws PublishedOlderRevision. The previous save
// point (S0) is no longer a valid clean anchor, because disk holds S1 while Undo
// can only reach S0. The document must therefore stay dirty after Undo to S0,
// and a later successful official save must re-arm normal clean-on-undo.
// Before the fix, _last_saved still points at S0, so Undo to S0 clears the dirty
// flag even though no clean position exists.
TEST_F(SaveTargetTest, PublishedOlderRevisionLeavesNoCleanUndoAnchor)
{
    make_linked_document();
    auto *svg_module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(svg_module, nullptr);
    ASSERT_STREQ(svg_module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);

    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"published-older setup history"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());

    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };

    // S0: a real ordinary official save arms the saved Undo anchor and clears dirt.
    std::string const saved = path("saved.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                              /*sync_title=*/false));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    std::string const filename_after_s0 = doc->getDocumentFilename() ? doc->getDocumentFilename() : "";
    auto const pref_after_s0 = Inkscape::Preferences::get()->getString("/dialogs/save_as/default");

    // E1: one committed user edit after the anchor.
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"published-older user edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());

    // Stale S1: the custom output publishes S1 bytes to the same path and makes a
    // further live edit before Extension::save re-checks the revision.
    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Revision Output</name><id>org.vacards.test.revision-output</id>"
        "<output><extension>.rev</extension><mimetype>image/x-vacards-revision</mimetype>"
        "<filetypename>Revision Output</filetypename></output></inkscape-extension>";
    auto *rev_module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    if (!rev_module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        rev_module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    }
    ASSERT_NE(rev_module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(rev_module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();
    observer->failure = ObservingOutputImplementation::Failure::PublishAndEdit;
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete older snapshot S1"));
        doc->getObjectById("art")->getRepr()->setAttribute("x", "9");
    };

    EXPECT_THROW(Inkscape::Extension::save(rev_module, doc.get(), saved.c_str(), false, true,
                                           Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::PublishedOlderRevision);

    // The published bytes are on disk, the live edit survived, and the stale
    // official save moved neither identity nor save preferences.
    EXPECT_EQ(read_file(saved), "complete older snapshot S1");
    ASSERT_NE(doc->getObjectById("art"), nullptr);
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "9");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(doc->getDocumentFilename() ? doc->getDocumentFilename() : "", filename_after_s0);
    EXPECT_EQ(Inkscape::Preferences::get()->getString("/dialogs/save_as/default"), pref_after_s0);

    // Undo walks back over the committed user edit to the S0 row. Because disk
    // holds S1 and the live document is S0, S0 is not a clean anchor: the
    // document must remain dirty. THIS is the pre-fix red assertion.
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "1");
    EXPECT_TRUE(doc->isModifiedSinceSave());

    // A subsequent successful official save re-arms the anchor and restores
    // normal clean-on-undo behavior.
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                              /*sync_title=*/false));
    EXPECT_FALSE(doc->isModifiedSinceSave());

    auto *art_after = doc->getObjectById("art");
    ASSERT_NE(art_after, nullptr);
    art_after->getRepr()->setAttribute("x", "7");
    DocumentUndo::done(doc.get(), ContextString{"post-rearm user edit"}, "");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "1");
    EXPECT_FALSE(doc->isModifiedSinceSave());

    observer->publish_and_edit = {};
    observer->original = nullptr;
}

// P2 edge-case oracle: a stale official publish that writes a DIFFERENT path must
// not invalidate the saved Undo anchor of the still-current official file. S0 is
// the official save of `official.svg`; a later stale Save As publishes S1 to
// `different-target.rev` and mutates the live document, so Extension::save
// throws PublishedOlderRevision. Because the live apply_save_preparation never
// runs for the stale save, the official identity is still `official.svg` and its
// bytes are still S0. Undo back to S0 is therefore clean relative to disk. The
// current narrow implementation invalidates the anchor for ANY official stale
// publish, so this test is RED until the invalidation is restricted to a target
// that may alias the official saved file.
TEST_F(SaveTargetTest, StaleSaveAsToDifferentPathKeepsOfficialUndoAnchor)
{
    make_linked_document();
    auto *svg_module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(svg_module, nullptr);
    ASSERT_STREQ(svg_module->get_id(), SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE);

    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"save-as-different setup history"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());

    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };

    // S0: a real official save of the original path arms the saved anchor.
    std::string const official = path("official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), official.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm,
                                              /*sync_title=*/false));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    std::string const official_bytes = read_file(official);
    ASSERT_FALSE(official_bytes.empty());
    std::string const s0_fingerprint = fingerprint(*doc);
    std::string const filename_after_s0 = doc->getDocumentFilename() ? doc->getDocumentFilename() : "";
    auto const pref_after_s0 = Inkscape::Preferences::get()->getString("/dialogs/save_as/default");

    // E1: one committed user edit after the anchor.
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"save-as-different user edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());

    // Stale S1: the custom output publishes S1 to a DIFFERENT path and makes a
    // further live edit before Extension::save re-checks the revision.
    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Revision Output</name><id>org.vacards.test.revision-output</id>"
        "<output><extension>.rev</extension><mimetype>image/x-vacards-revision</mimetype>"
        "<filetypename>Revision Output</filetypename></output></inkscape-extension>";
    auto *rev_module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    if (!rev_module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        rev_module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    }
    ASSERT_NE(rev_module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(rev_module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();
    observer->failure = ObservingOutputImplementation::Failure::PublishAndEdit;
    std::string const new_target = path("different-target.rev");
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete older snapshot S1"));
        doc->getObjectById("art")->getRepr()->setAttribute("x", "9");
    };

    EXPECT_THROW(Inkscape::Extension::save(rev_module, doc.get(), new_target.c_str(), false, true,
                                           Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::PublishedOlderRevision);

    // The new target holds S1, the old official file is untouched, and the live
    // document still identifies as the old official path with its save prefs.
    EXPECT_EQ(read_file(new_target), "complete older snapshot S1");
    EXPECT_EQ(read_file(official), official_bytes);
    ASSERT_NE(doc->getObjectById("art"), nullptr);
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "9");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(doc->getDocumentFilename() ? doc->getDocumentFilename() : "", filename_after_s0);
    EXPECT_EQ(Inkscape::Preferences::get()->getString("/dialogs/save_as/default"), pref_after_s0);

    // Undo walks back over E1 to the S0 row. `official.svg` still holds S0, so S0
    // is a valid clean anchor and the document must return to clean. THIS is the
    // pre-fix red assertion: the current unconditional invalidateFileSave() has
    // already dropped the anchor, so the dirty flag stays set.
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "1");
    EXPECT_EQ(fingerprint(*doc), s0_fingerprint);
    EXPECT_FALSE(doc->isModifiedSinceSave());

    observer->publish_and_edit = {};
    observer->original = nullptr;
}

TEST_F(SaveTargetTest, SupersededDistinctSaveAsKeepsOfficialUndoAnchor)
{
    make_linked_document();
    auto *svg_module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(svg_module, nullptr);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"superseded distinct setup"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    auto const official = path("superseded-distinct-official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), official.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto const official_bytes = read_file(official);
    auto const s0 = fingerprint(*doc);
    doc->getObjectById("art")->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"superseded distinct edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());

    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Revision Output</name><id>org.vacards.test.revision-output</id>"
        "<output><extension>.rev</extension><mimetype>image/x-vacards-revision</mimetype>"
        "<filetypename>Revision Output</filetypename></output></inkscape-extension>";
    auto *rev_module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    if (!rev_module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        rev_module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    }
    ASSERT_NE(rev_module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(rev_module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();
    observer->failure = ObservingOutputImplementation::Failure::PublishAndEdit;
    auto const nested_target = path("superseding-copy.svg");
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete older snapshot S1"));
        ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), nested_target.c_str(), false,
            false, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY));
    };
    auto const distinct_target = path("superseded-distinct.rev");
    EXPECT_THROW(Inkscape::Extension::save(rev_module, doc.get(), distinct_target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS), Inkscape::Extension::PublishedStaleDocument);
    EXPECT_EQ(read_file(distinct_target), "complete older snapshot S1");
    EXPECT_EQ(read_file(official), official_bytes);
    EXPECT_FALSE(read_file(nested_target).empty());
    EXPECT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(fingerprint(*doc), s0);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    observer->publish_and_edit = {};
    observer->original = nullptr;
}

// A save can publish S1 and still report an uncertain outcome. The old S0 Undo
// anchor must not make the document clean when Undo returns to S0: disk may now
// contain S1. This is an observable document-state contract, not an assertion
// about which exception branch the wrapper takes.
TEST_F(SaveTargetTest, UncertainSamePathSaveDoesNotLeaveCleanUndoAnchor)
{
    make_linked_document();
    auto *svg_module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(svg_module, nullptr);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"uncertain setup history"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };

    auto const saved = path("uncertain-official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"uncertain user edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());

    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Revision Output</name><id>org.vacards.test.revision-output</id>"
        "<output><extension>.rev</extension><mimetype>image/x-vacards-revision</mimetype>"
        "<filetypename>Revision Output</filetypename></output></inkscape-extension>";
    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    }
    ASSERT_NE(module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();
    observer->failure = ObservingOutputImplementation::Failure::PublishThenUncertain;
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete S1 despite uncertain outcome"));
    };

    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), saved.c_str(), false, true,
                                           Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::Output::save_uncertain);
    EXPECT_EQ(read_file(saved), "complete S1 despite uncertain outcome");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "1");
    EXPECT_TRUE(doc->isModifiedSinceSave());

    // A subsequent confirmed save re-arms the anchor. A generic save_failed
    // after output may also have replaced the target, so it must not restore
    // the old clean point when the next edit is undone.
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto *art_after = doc->getObjectById("art");
    ASSERT_NE(art_after, nullptr);
    art_after->getRepr()->setAttribute("x", "7");
    DocumentUndo::done(doc.get(), ContextString{"generic-failure user edit"}, "");
    observer->failure = ObservingOutputImplementation::Failure::PublishThenFailed;
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "complete S2 despite generic failure"));
    };
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), saved.c_str(), false, true,
                                           Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm),
                 Inkscape::Extension::Output::save_failed);
    EXPECT_EQ(read_file(saved), "complete S2 despite generic failure");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "1");
    EXPECT_TRUE(doc->isModifiedSinceSave());

    observer->publish_and_edit = {};
    observer->original = nullptr;
}

// Save Copy normally preserves the official saved state, but copying onto that
// very same file publishes different bytes. The old Undo anchor is no longer a
// valid clean point even though Save Copy never adopts a new filename.
TEST_F(SaveTargetTest, SaveCopyOntoOfficialPathDoesNotLeaveCleanUndoAnchor)
{
    make_linked_document();
    auto *svg_module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(svg_module, nullptr);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"copy-over-official setup history"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };

    auto const saved = path("copy-over-official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"copy-over-official user edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());

    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), saved.c_str(), false, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, confirm));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "1");
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

// Extension temp output runs against the live document but has no user save
// semantics. Even when the scratch file already exists, it cannot disturb the
// official file's clean Undo point.
TEST_F(SaveTargetTest, TemporaryExtensionSaveKeepsOfficialUndoAnchor)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"temp setup history"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    auto const saved = path("temp-official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto const scratch = path("extension-existing-temp.svg");
    ASSERT_TRUE(write_file(scratch, "prior scratch bytes"));
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), scratch.c_str(), false, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_TEMPORARY, confirm));
    EXPECT_FALSE(doc->isModifiedSinceSave());
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"edit after temp"}, "");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, TemporaryExtensionSaveDoesNotSupersedeInteractiveAttempt)
{
    auto const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Temp interleave output</name><id>org.vacards.test.temp-interleave</id>"
        "<output><extension>.tmp-interleave</extension><mimetype>application/x-vacards-test</mimetype>"
        "<filetypename>Temp interleave output</filetypename></output></inkscape-extension>";
    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.temp-interleave"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml, std::make_unique<ObservingOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.temp-interleave"));
    }
    ASSERT_NE(module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(module->get_imp());
    observer->original = doc.get();
    observer->failure = ObservingOutputImplementation::Failure::PublishAndEdit;
    auto *svg = get_output_extension_for_save(path("scratch.svg"));
    ASSERT_NE(svg, nullptr);
    auto const scratch = path("extension-scratch.svg");
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "published interactive snapshot"));
        ASSERT_NO_THROW(Inkscape::Extension::save(svg, doc.get(), scratch.c_str(), false, false,
            Inkscape::Extension::FILE_SAVE_METHOD_TEMPORARY));
    };
    auto const generation_before = doc->saveAttemptGeneration();
    auto const target = path("outer.tmp-interleave");
    EXPECT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
        Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true));
    EXPECT_EQ(doc->saveAttemptGeneration(), generation_before + 1);
    EXPECT_EQ(read_file(target), "published interactive snapshot");
    EXPECT_FALSE(doc->isModifiedSinceSave());
    observer->publish_and_edit = {};
    observer->original = nullptr;
}

// An existing but different destination must not be mistaken for the saved
// file. This exercises Windows handle identity, where g_stat inode is unusable.
TEST_F(SaveTargetTest, SaveCopyToExistingDistinctFileKeepsCleanState)
{
    make_linked_document();
    auto *module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    auto const saved = path("distinct-official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto const copy = path("existing-distinct-copy.svg");
    ASSERT_TRUE(write_file(copy, "older unrelated copy"));
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), copy.c_str(), false, false,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, confirm));
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_EQ(doc->getDocumentFilename() ? doc->getDocumentFilename() : "", saved);
    EXPECT_NE(read_file(copy), "older unrelated copy");
}

// A Save Copy that partially overwrites the official file and then fails must
// dirty even an initially clean document, so close still prompts the user.
TEST_F(SaveTargetTest, FailedSaveCopyOverOfficialDirtiesCleanDocument)
{
    make_linked_document();
    auto *svg_module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(svg_module, nullptr);
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    auto const saved = path("failed-copy-official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());

    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Revision Output</name><id>org.vacards.test.revision-output</id>"
        "<output><extension>.rev</extension><mimetype>image/x-vacards-revision</mimetype>"
        "<filetypename>Revision Output</filetypename></output></inkscape-extension>";
    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    }
    ASSERT_NE(module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();
    observer->failure = ObservingOutputImplementation::Failure::PublishThenUncertain;
    observer->publish_and_edit = [&](gchar const *filename) {
        ASSERT_TRUE(write_file(filename, "partial copy overwrite"));
    };
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), saved.c_str(), false, false,
                                           Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, confirm),
                 Inkscape::Extension::Output::save_uncertain);
    EXPECT_EQ(read_file(saved), "partial copy overwrite");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    observer->publish_and_edit = {};
    observer->original = nullptr;
}

#ifdef __APPLE__
// A different pathname can still publish the official file through an inode
// alias. Exercise the identity probe, rather than the identical-path shortcut.
TEST_F(SaveTargetTest, SaveCopyThroughSymlinkToOfficialDirtiesCleanDocument)
{
    make_linked_document();
    auto *svg_module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(svg_module, nullptr);
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    auto const saved = path("aliased-official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto const alias = path("symlink-to-official.svg");
    ASSERT_EQ(::symlink(saved.c_str(), alias.c_str()), 0);

    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Revision Output</name><id>org.vacards.test.revision-output</id>"
        "<output><extension>.rev</extension><mimetype>image/x-vacards-revision</mimetype>"
        "<filetypename>Revision Output</filetypename></output></inkscape-extension>";
    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    }
    ASSERT_NE(module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();
    observer->failure = ObservingOutputImplementation::Failure::PublishThenUncertain;
    observer->publish_and_edit = [&](gchar const *filename) {
        auto *stream = g_fopen(filename, "wb");
        ASSERT_NE(stream, nullptr);
        static constexpr char bytes[] = "alias overwrite";
        EXPECT_EQ(std::fwrite(bytes, 1, sizeof(bytes) - 1, stream), sizeof(bytes) - 1);
        EXPECT_EQ(std::fclose(stream), 0);
    };
    EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), alias.c_str(), false, false,
                                           Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY, confirm),
                 Inkscape::Extension::Output::save_uncertain);
    EXPECT_EQ(read_file(saved), "alias overwrite");
    EXPECT_TRUE(doc->isModifiedSinceSave());
    observer->publish_and_edit = {};
    observer->original = nullptr;
}
#endif

// A definite no-publication error and an uncertain Save As to a proven
// different filename do not invalidate the original file's S0 Undo anchor.
// After each attempt, Undo to S0 must still become clean relative to that file.
TEST_F(SaveTargetTest, NonPublishingOrDistinctSaveFailureKeepsOfficialUndoAnchor)
{
    make_linked_document();
    auto *svg_module = get_output_extension_for_save(path("drawing.svg"));
    ASSERT_NE(svg_module, nullptr);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), ContextString{"failure-boundary setup history"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };

    auto const saved = path("failure-boundary-official.svg");
    ASSERT_NO_THROW(Inkscape::Extension::save(svg_module, doc.get(), saved.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto const original_bytes = read_file(saved);

    std::string const xml =
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">"
        "<name>Revision Output</name><id>org.vacards.test.revision-output</id>"
        "<output><extension>.rev</extension><mimetype>image/x-vacards-revision</mimetype>"
        "<filetypename>Revision Output</filetypename></output></inkscape-extension>";
    auto *module = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    if (!module) {
        Inkscape::Extension::build_from_mem(xml.c_str(), std::make_unique<ObservingOutputImplementation>());
        module = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.vacards.test.revision-output"));
    }
    ASSERT_NE(module, nullptr);
    auto *observer = static_cast<ObservingOutputImplementation *>(module->get_imp());
    ASSERT_NE(observer, nullptr);
    observer->original = doc.get();

    auto attempt_and_undo = [&](ObservingOutputImplementation::Failure failure,
                                std::string const &target) {
        auto *art = doc->getObjectById("art");
        ASSERT_NE(art, nullptr);
        art->getRepr()->setAttribute("x", "6");
        DocumentUndo::done(doc.get(), ContextString{"failure-boundary user edit"}, "");
        ASSERT_TRUE(doc->isModifiedSinceSave());
        observer->failure = failure;
        try {
            Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
                                      Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm);
            FAIL() << "expected output failure";
        } catch (Inkscape::Extension::Output::save_failed const &) {
        }
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        doc->ensureUpToDate();
        EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"), "1");
        EXPECT_FALSE(doc->isModifiedSinceSave());
        EXPECT_EQ(read_file(saved), original_bytes);
    };

    attempt_and_undo(ObservingOutputImplementation::Failure::SaveUnsupported, saved);
    attempt_and_undo(ObservingOutputImplementation::Failure::SaveConflict, saved);
    attempt_and_undo(ObservingOutputImplementation::Failure::SaveUncertain,
                     path("proven-distinct-uncertain.rev"));

    observer->original = nullptr;
}

#ifdef __APPLE__
TEST_F(SaveTargetTest, SuccessfulSaveReportsRecoveryNoticeWithoutDirtyingDocument)
{
    make_linked_document();
    auto const target = path("notice.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto const previous_bytes = read_file(target);
    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "6");
    DocumentUndo::done(doc.get(), ContextString{"notice edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());

    std::string notice;
    {
        ScopedRecoveryDeleteFailureInjection const fault(EACCES);
        Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
                                  Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS,
                                  confirm, false, &notice);
    }
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_NE(notice.find("recovery copy"), std::string::npos);
    GDir *listing = g_dir_open(workdir.c_str(), 0, nullptr);
    ASSERT_NE(listing, nullptr);
    std::string recovery;
    while (char const *name = g_dir_read_name(listing)) {
        if (g_str_has_prefix(name, "vacards-recovery-")) recovery = path(name);
    }
    g_dir_close(listing);
    ASSERT_FALSE(recovery.empty());
    EXPECT_NE(notice.find(recovery), std::string::npos);
    EXPECT_TRUE(g_file_test(recovery.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(recovery), previous_bytes);
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_TRUE(reopened);
    ASSERT_NE(reopened->getObjectById("art"), nullptr);
    EXPECT_STREQ(reopened->getObjectById("art")->getRepr()->attribute("x"), "6");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_FALSE(doc->isModifiedSinceSave());

    std::string next_notice;
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS,
                                              confirm, false, &next_notice));
    EXPECT_TRUE(next_notice.empty());
    EXPECT_EQ(read_file(recovery), previous_bytes);
    listing = g_dir_open(workdir.c_str(), 0, nullptr);
    ASSERT_NE(listing, nullptr);
    unsigned recovery_count = 0;
    while (char const *name = g_dir_read_name(listing)) {
        if (g_str_has_prefix(name, "vacards-recovery-")) ++recovery_count;
    }
    g_dir_close(listing);
    EXPECT_EQ(recovery_count, 1u);
}

TEST_F(SaveTargetTest, SaveCopyRetainedRecoveryNoticePreservesOfficialIdentity)
{
    make_linked_document();
    auto const official = path("official.svg");
    auto const copy = path("copy.svg");
    auto *module = get_output_extension_for_save(official);
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), official.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    ASSERT_FALSE(doc->isModifiedSinceSave());
    auto const official_bytes = read_file(official);
    ASSERT_TRUE(write_file(copy, "prior copy bytes"));

    auto *art = doc->getObjectById("art");
    ASSERT_NE(art, nullptr);
    art->getRepr()->setAttribute("x", "7");
    DocumentUndo::done(doc.get(), ContextString{"copy edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());
    auto const live_before = fingerprint(*doc);
    auto const name_before = std::string(doc->getDocumentFilename());

    std::string notice;
    {
        ScopedRecoveryDeleteFailureInjection const fault(EACCES);
        ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), copy.c_str(), false, false,
                                                  Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY,
                                                  confirm, false, &notice));
    }
    EXPECT_NE(notice.find("recovery copy"), std::string::npos);
    EXPECT_EQ(fingerprint(*doc), live_before);
    EXPECT_EQ(std::string(doc->getDocumentFilename()), name_before);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(read_file(official), official_bytes);
    auto reopened = SPDocument::createNewDoc(copy.c_str(), false);
    ASSERT_TRUE(reopened);
    ASSERT_NE(reopened->getObjectById("art"), nullptr);
    EXPECT_STREQ(reopened->getObjectById("art")->getRepr()->attribute("x"), "7");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_TRUE(doc->isModifiedSinceSave());
}

TEST_F(SaveTargetTest, RetainedRecoveryWithoutNoticeSinkDoesNotFailSave)
{
    make_linked_document();
    auto const target = path("headless.svg");
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    doc->getObjectById("art")->getRepr()->setAttribute("x", "8");
    DocumentUndo::done(doc.get(), ContextString{"headless edit"}, "");
    ASSERT_TRUE(doc->isModifiedSinceSave());
    {
        ScopedRecoveryDeleteFailureInjection const fault(EACCES);
        ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
                                                  Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm));
    }
    EXPECT_FALSE(doc->isModifiedSinceSave());
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_TRUE(reopened);
    ASSERT_NE(reopened->getObjectById("art"), nullptr);
    EXPECT_STREQ(reopened->getObjectById("art")->getRepr()->attribute("x"), "8");

    std::string next_notice;
    ASSERT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
                                              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS,
                                              confirm, false, &next_notice));
    EXPECT_TRUE(next_notice.empty());
}

// Opt-in end-to-end SMB check: the existing-file adapter must publish a real
// SVG, and a fresh document must parse it. Only the unique scratch child is
// removed on success; failures remain available for inspection.
TEST_F(SaveTargetTest, ExistingSvgOnSmbPublishesAndReopens)
{
    char const *parent = g_getenv("VACARDS_NAS_SCRATCH");
    if (!parent || !*parent) GTEST_SKIP() << "set VACARDS_NAS_SCRATCH to a writable scratch parent";
    make_linked_document();
    gchar *uuid = g_uuid_string_random();
    std::string const dir = std::string(parent) + "/vacards-svg-save-test-" + uuid;
    g_free(uuid);
    ASSERT_EQ(g_mkdir(dir.c_str(), 0700), 0);
    std::string const target = dir + "/existing.svg";
    ASSERT_TRUE(write_file(target, "complete previous file"));
    auto *module = get_output_extension_for_save(target);
    ASSERT_NE(module, nullptr);
    Inkscape::Extension::OverwriteConfirm const confirm = [](std::string const &) { return true; };
    bool saved = true;
    try {
        Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
                                  Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, confirm);
    } catch (std::exception const &e) {
        saved = false;
        ADD_FAILURE() << "SMB SVG save failed: " << e.what() << " scratch=" << dir;
    } catch (...) {
        saved = false;
        ADD_FAILURE() << "SMB SVG save failed; scratch=" << dir;
    }
    if (!saved) return;
    auto reopened = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_NE(reopened, nullptr) << "scratch=" << dir;
    reopened->ensureUpToDate();
    EXPECT_NE(reopened->getObjectById("art"), nullptr);
    GDir *listing = g_dir_open(dir.c_str(), 0, nullptr);
    ASSERT_NE(listing, nullptr) << "scratch=" << dir;
    while (char const *name = g_dir_read_name(listing)) {
        EXPECT_STREQ(name, "existing.svg") << "unexpected SMB leftover in " << dir;
    }
    g_dir_close(listing);
    if (!HasFailure()) {
        EXPECT_EQ(g_remove(target.c_str()), 0);
        EXPECT_EQ(g_rmdir(dir.c_str()), 0);
    }
}

TEST_F(SaveTargetTest, ExistingSvgOnSmbStage4EioPreservesOriginal)
{
    char const *parent = g_getenv("VACARDS_NAS_SCRATCH");
    if (!parent || !*parent) GTEST_SKIP() << "set VACARDS_NAS_SCRATCH to a writable scratch parent";
    gchar *uuid = g_uuid_string_random();
    std::string const dir = std::string(parent) + "/vacards-svg-eio-test-" + uuid;
    g_free(uuid);
    ASSERT_EQ(g_mkdir(dir.c_str(), 0700), 0);
    auto const target = dir + "/existing.svg";
    std::string const old_bytes = "complete previous file";
    ASSERT_TRUE(write_file(target, old_bytes));
    std::string const fresh = "complete new bytes";
    bool observed = false;
    Inkscape::IO::ExistingFileOptions options;
    options.stage_observer = [&](unsigned stage) {
        if (stage != 4) return false;
        observed = true;
        EXPECT_EQ(read_file(target), old_bytes);
        return true; // synthetic EIO at PUBLISH, before rename
    };
    auto const bytes = std::span<std::byte const>(
        reinterpret_cast<std::byte const *>(fresh.data()), fresh.size());
    auto const result = Inkscape::IO::replace_existing_local_file(target, bytes, false, options);
    EXPECT_TRUE(observed);
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication);
    EXPECT_NE(result.error.find("replace SMB destination"), std::string::npos);
    EXPECT_EQ(read_file(target), old_bytes);
    EXPECT_TRUE(result.recovery_path.empty());
    GDir *listing = g_dir_open(dir.c_str(), 0, nullptr);
    ASSERT_NE(listing, nullptr) << dir;
    while (char const *name = g_dir_read_name(listing)) {
        EXPECT_STREQ(name, "existing.svg") << dir;
    }
    g_dir_close(listing);
    if (!HasFailure()) {
        EXPECT_EQ(g_remove(target.c_str()), 0);
        EXPECT_EQ(g_rmdir(dir.c_str()), 0);
    }
}
#endif

} // namespace

#ifdef __APPLE__
TEST_F(SaveTargetTest, ExistingStageFailuresObserveRealBoundariesAndCleanUp)
{
    using namespace Inkscape::IO;
    auto const target = path("observed-existing-stage.svg");
    auto const parent = Glib::path_get_dirname(target);
    std::string const fresh = "complete new bytes";
    for (unsigned fail_stage = 1; fail_stage <= 5; ++fail_stage) {
        ASSERT_TRUE(write_file(target, "previous"));
        bool observed = false;
        ExistingFileOptions options;
        options.stage_observer = [&](unsigned stage) {
            if (stage != fail_stage) return false;
            observed = true;
            GDir *listing = g_dir_open(parent.c_str(), 0, nullptr);
            if (!listing) { ADD_FAILURE() << "could not inspect stage directory"; return true; }
            bool stage_seen = false;
            bool recovery_seen = false;
            while (char const *name = g_dir_read_name(listing)) {
                stage_seen |= g_str_has_prefix(name, "vacards-save-");
                recovery_seen |= g_str_has_prefix(name, "vacards-recovery-");
            }
            g_dir_close(listing);
            if (stage <= 4) EXPECT_TRUE(stage_seen);
            if (stage == 4) {
                struct stat metadata{};
                if (::stat(target.c_str(), &metadata) != 0) ADD_FAILURE() << "target missing at rename boundary";
                else EXPECT_EQ(metadata.st_nlink, 2);
                EXPECT_TRUE(recovery_seen);
            }
            if (stage == 5) {
                EXPECT_EQ(read_file(target), fresh);
                EXPECT_TRUE(recovery_seen);
            }
            return true;
        };
        auto const bytes = std::span<std::byte const>(
            reinterpret_cast<std::byte const *>(fresh.data()), fresh.size());
        auto const result = replace_existing_local_file(target, bytes, false, options);
        EXPECT_TRUE(observed) << fail_stage;
        if (fail_stage < 5) {
            EXPECT_EQ(result.outcome, ExistingFileOutcome::FailedBeforePublication) << fail_stage;
            EXPECT_EQ(read_file(target), "previous");
            EXPECT_TRUE(result.recovery_path.empty());
            if (fail_stage == 4) EXPECT_NE(result.error.find("replace destination"), std::string::npos);
            else EXPECT_NE(result.error.find("injected stage-"), std::string::npos);
            GDir *listing = g_dir_open(parent.c_str(), 0, nullptr);
            ASSERT_NE(listing, nullptr);
            while (char const *name = g_dir_read_name(listing)) {
                EXPECT_FALSE(g_str_has_prefix(name, "vacards-save-"));
                EXPECT_FALSE(g_str_has_prefix(name, "vacards-recovery-"));
            }
            g_dir_close(listing);
        } else {
            EXPECT_EQ(result.outcome, ExistingFileOutcome::Published);
            EXPECT_EQ(read_file(target), fresh);
            EXPECT_NE(result.error.find("cleanup"), std::string::npos);
            ASSERT_FALSE(result.recovery_path.empty());
            ASSERT_EQ(g_remove(result.recovery_path.c_str()), 0);
        }
    }
}

TEST_F(SaveTargetTest, NewStageFailuresReportTheirBoundaryAndCleanUp)
{
    using namespace Inkscape::Extension::Internal;
    char const *errors[] = {"", "injected stage-create failure",
                            "injected stage-write failure", "injected stage-seal failure",
                            "injected pre-publication failure"};
    for (unsigned stage = 1; stage <= 5; ++stage) {
        PublicationJob job;
        job.absolute_path = path("new-boundary-" + std::to_string(stage) + ".svg");
        job.bytes.resize(std::strlen(kSvg));
        std::memcpy(job.bytes.data(), kSvg, job.bytes.size());
        job.test_hooks_enabled = true;
        job.stage_hooks[stage].failure = PublicationOutcome::Failed;
        auto const result = publish(std::move(job));
        if (stage < 5) {
            EXPECT_EQ(result.outcome, PublicationOutcome::Failed) << stage;
            EXPECT_NE(result.error.find(errors[stage]), std::string::npos) << stage;
            EXPECT_FALSE(g_file_test(path("new-boundary-" + std::to_string(stage) + ".svg").c_str(), G_FILE_TEST_EXISTS));
        } else {
            EXPECT_EQ(result.outcome, PublicationOutcome::Published);
            EXPECT_FALSE(result.notice.empty());
            EXPECT_NE(read_file(path("new-boundary-5.svg")).find("<svg"), std::string::npos);
            if (!result.recovery_path.empty()) EXPECT_EQ(g_remove(result.recovery_path.c_str()), 0);
        }
        GDir *listing = g_dir_open(workdir.c_str(), 0, nullptr);
        ASSERT_NE(listing, nullptr);
        while (char const *name = g_dir_read_name(listing)) {
            if (stage < 5) {
                EXPECT_FALSE(g_str_has_prefix(name, "vacards-save-"));
                EXPECT_FALSE(g_str_has_prefix(name, "vacards-recovery-"));
            }
        }
        g_dir_close(listing);
    }
}

#define SAVE_STAGE_FAILURE_TEST(Name, Stage, Outcome, ErrorType) \
TEST_F(SaveTargetTest, Name) { \
    Inkscape::IO::enable_file_io_test_hooks(); \
    for (bool existing : {false, true}) { \
        auto const target = path(std::string("stage-") + Stage + (existing ? "-existing.svg" : "-new.svg")); \
        if (existing) ASSERT_TRUE(write_file(target, "previous")); \
        auto *module = get_output_extension_for_save(target); \
        ASSERT_NE(module, nullptr); \
        g_setenv("VACARDS_SAVE_TEST_" Stage "_OUTCOME", Outcome, TRUE); \
        EXPECT_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true, \
            Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, nullptr, true), ErrorType); \
        g_unsetenv("VACARDS_SAVE_TEST_" Stage "_OUTCOME"); \
        if (existing) EXPECT_EQ(read_file(target), "previous"); \
        else EXPECT_FALSE(g_file_test(target.c_str(), G_FILE_TEST_EXISTS)); \
        EXPECT_TRUE(doc->isModifiedSinceSave()); \
    } \
}
SAVE_STAGE_FAILURE_TEST(AdmissionHook, "ADMISSION", "unsupported", Inkscape::Extension::Output::save_unsupported)
SAVE_STAGE_FAILURE_TEST(CreateHook, "CREATE", "failed", Inkscape::Extension::Output::save_failed)
SAVE_STAGE_FAILURE_TEST(StageWriteHook, "STAGE_WRITE", "failed", Inkscape::Extension::Output::save_failed)
SAVE_STAGE_FAILURE_TEST(SealHook, "SEAL", "failed", Inkscape::Extension::Output::save_failed)
SAVE_STAGE_FAILURE_TEST(PublishHook, "PUBLISH", "failed", Inkscape::Extension::Output::save_failed)
#undef SAVE_STAGE_FAILURE_TEST

TEST_F(SaveTargetTest, CleanupHookReportsPublishedNotice)
{
    Inkscape::IO::enable_file_io_test_hooks();
    for (bool existing : {false, true}) {
        auto const target = path(existing ? "cleanup-existing.svg" : "cleanup-new.svg");
        if (existing) ASSERT_TRUE(write_file(target, "previous"));
        auto *module = get_output_extension_for_save(target);
        ASSERT_NE(module, nullptr);
        doc->setModifiedSinceSave(true);
        std::string notice;
        g_setenv("VACARDS_SAVE_TEST_CLEANUP_OUTCOME", "failed", TRUE);
        EXPECT_NO_THROW(Inkscape::Extension::save(module, doc.get(), target.c_str(), false, true,
            Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, {}, false, &notice, true));
        auto *svg = dynamic_cast<Inkscape::Extension::Internal::Svg *>(module->get_imp());
        ASSERT_NE(svg, nullptr);
        auto const mapped = path(existing ? "mapped-existing.svg" : "mapped-new.svg");
        if (existing) ASSERT_TRUE(write_file(mapped, "previous"));
        auto job = svg->begin_publication(doc.get(), mapped.c_str());
        ASSERT_EQ(job.stage_hooks[5].failure, Inkscape::Extension::Internal::PublicationOutcome::Failed);
        auto const result = Inkscape::Extension::Internal::publish(std::move(job));
        EXPECT_EQ(result.outcome, Inkscape::Extension::Internal::PublicationOutcome::Published);
        EXPECT_FALSE(result.notice.empty());
        g_unsetenv("VACARDS_SAVE_TEST_CLEANUP_OUTCOME");
        EXPECT_FALSE(notice.empty());
        EXPECT_NE(read_file(target).find("<svg"), std::string::npos);
        EXPECT_FALSE(doc->isModifiedSinceSave());
    }
}

TEST_F(SaveTargetTest, PublicationStallHookDelaysPublish)
{
    using namespace Inkscape::Extension::Internal;
    Inkscape::IO::enable_file_io_test_hooks();
    for (bool existing : {false, true}) {
        PublicationJob job;
        job.absolute_path = path(existing ? "stall-existing.svg" : "stall-new.svg");
        if (existing) ASSERT_TRUE(write_file(job.absolute_path, "previous"));
        job.bytes.resize(std::strlen(kSvg));
        std::memcpy(job.bytes.data(), kSvg, job.bytes.size());
        job.test_hooks_enabled = true;
        job.stage_hooks[1].stall_ms = 200;
        auto const start = std::chrono::steady_clock::now();
        EXPECT_EQ(publish(std::move(job)).outcome, PublicationOutcome::Published);
        EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
    }
}
#endif

TEST_F(SaveTargetTest, ClassifiesSaveExceptions)
{
    using O = Inkscape::Extension::Output;
    std::string notice;
    auto check = [&](auto error, char const *status, char const *dialog_part) {
        auto const [message, dialog] = classify_file_save_exception_for_testing(
            std::make_exception_ptr(error), "drawing.svg", notice);
        EXPECT_EQ(message, status);
        EXPECT_NE(dialog.find(dialog_part), static_cast<Glib::ustring::size_type>(-1));
    };
    check(O::file_read_only{}, "Document not saved.", "write protected");
    check(O::save_unsupported{"unsupported"}, "Document not saved.", "cannot safely be saved");
    check(O::save_conflict{"conflict"}, "Document not saved.", "not overwritten");
    check(O::save_uncertain{"uncertain"}, "Save outcome uncertain; inspect the destination.", "may or may not have completed");
    check(O::save_failed{"failed"}, "Document not saved.", "could not be saved");
    check(O::export_id_not_found{"missing"}, "Document not saved.", "missing");
    check(Inkscape::Extension::PublishedOlderRevision{}, "Newer changes remain unsaved.", "newer changes");
    check(Inkscape::Extension::PublishedStaleDocument{"stale"}, "Saved snapshot does not match the current document.", "stale");
    EXPECT_EQ(classify_file_save_flags_for_testing(std::make_exception_ptr(O::save_failed{})), 0u);
    EXPECT_EQ(classify_file_save_flags_for_testing(std::make_exception_ptr(O::save_uncertain{"uncertain"})), 9u);
    EXPECT_EQ(classify_file_save_flags_for_testing(std::make_exception_ptr(Inkscape::Extension::PublishedOlderRevision{})), 5u);
    EXPECT_EQ(classify_file_save_flags_for_testing(std::make_exception_ptr(Inkscape::Extension::PublishedStaleDocument{"stale"})), 1u);
    EXPECT_EQ(classify_file_save_flags_for_testing(std::make_exception_ptr(O::no_overwrite{})), 2u);
    auto const cancelled = classify_file_save_exception_for_testing(std::make_exception_ptr(O::save_cancelled{}), "drawing.svg", notice);
    EXPECT_EQ(cancelled.first, "Document not saved.");
    EXPECT_TRUE(cancelled.second.empty());
    auto const declined = classify_file_save_exception_for_testing(std::make_exception_ptr(O::no_overwrite{}), "drawing.svg", notice);
    EXPECT_TRUE(declined.first.empty());
    EXPECT_TRUE(declined.second.empty());
}

TEST_F(SaveTargetTest, CompletionWithoutWindowOrDesktop)
{
    Inkscape::IO::enable_file_io_test_hooks();
    auto const target = path("completion-without-window.svg");
    auto file = Gio::File::create_for_path(target);
    EXPECT_TRUE(complete_file_save_without_window_for_testing(doc.get(), file, false));
}

// THUMB-1: a completed official save of a clean document stores its Welcome
// thumbnails (1x and 2x) from the open document. Kept here because this suite
// runs save completion without a display on every platform.
TEST_F(SaveTargetTest, CompletionStoresTheWelcomeThumbnail)
{
    Inkscape::IO::enable_file_io_test_hooks();
    auto const target = path("welcome-thumbnail.svg");
    ASSERT_TRUE(write_file(target, "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='30'>"
                                   "<rect x='5' y='5' width='20' height='10' fill='#ff0000'/></svg>"));
    auto saved = SPDocument::createNewDoc(target.c_str(), false);
    ASSERT_TRUE(saved);
#ifdef __APPLE__
    struct stat saved_version {};
    ASSERT_EQ(stat(target.c_str(), &saved_version), 0);
#endif
    auto const cache = path("welcome-cache");
    auto count = [&cache] {
        std::size_t pngs = 0;
        if (auto *dir = g_dir_open(cache.c_str(), 0, nullptr)) {
            while (auto const *name = g_dir_read_name(dir)) pngs += g_str_has_suffix(name, ".png") ? 1 : 0;
            g_dir_close(dir);
        }
        return pngs;
    };
    auto const *inherited = g_getenv("VACARDS_WELCOME_THUMBNAIL_TEST_CACHE");
    std::string const previous = inherited ? inherited : "";
    g_setenv("VACARDS_WELCOME_THUMBNAIL_TEST_CACHE", cache.c_str(), true);
    complete_file_save_without_window_for_testing(saved.get(), Gio::File::create_for_path(target), false);
    // The Finder icon is written on a background thread (macOS).
    auto icon_written = [&] {
#ifdef __APPLE__
        struct stat now {};
        return getxattr(target.c_str(), "com.apple.ResourceFork", nullptr, 0, 0, 0) > 0 &&
               stat(target.c_str(), &now) == 0 && now.st_mtimespec.tv_sec == saved_version.st_mtimespec.tv_sec &&
               now.st_mtimespec.tv_nsec == saved_version.st_mtimespec.tv_nsec;
#else
        return true;
#endif
    };
    auto const end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((count() < 2 || !icon_written()) && std::chrono::steady_clock::now() < end) {
        while (g_main_context_iteration(nullptr, false)) {}
        g_usleep(1000);
    }
    if (inherited) g_setenv("VACARDS_WELCOME_THUMBNAIL_TEST_CACHE", previous.c_str(), true);
    else g_unsetenv("VACARDS_WELCOME_THUMBNAIL_TEST_CACHE");
    EXPECT_EQ(count(), 2u);
#ifdef __APPLE__
    // VIEW-1: the saved SVG carries its drawing as Finder custom icon, and the
    // file keeps its modification time (the Welcome cache key) and bytes.
    EXPECT_GT(getxattr(target.c_str(), "com.apple.ResourceFork", nullptr, 0, 0, 0), 0);
    unsigned char finder_info[32] = {};
    ASSERT_EQ(getxattr(target.c_str(), "com.apple.FinderInfo", finder_info, sizeof(finder_info), 0, 0), 32);
    EXPECT_TRUE(finder_info[8] & 0x04) << "kHasCustomIcon";
    struct stat after_icon {};
    ASSERT_EQ(stat(target.c_str(), &after_icon), 0);
    EXPECT_EQ(after_icon.st_mtimespec.tv_sec, saved_version.st_mtimespec.tv_sec);
    EXPECT_EQ(after_icon.st_mtimespec.tv_nsec, saved_version.st_mtimespec.tv_nsec);
    EXPECT_EQ(after_icon.st_size, saved_version.st_size);
#endif
}

// ---------------------------------------------------------------------------
// Save slice 2 / P3: deferred serialization on the publication worker.
//
// begin_publication() prepares everything it needs on the initiating thread and
// duplicates a GC-anchored standalone XML snapshot; publish() serializes that
// snapshot on whatever thread runs it and never touches the live document. The
// tests below prove that a snapshot serialized on a worker while the main
// thread edits the live tree and runs a full collection is byte-identical to an
// all-on-this-thread reference, and that the over-cap streaming route used on
// the worker produces the same bytes as the uncapped buffered route.
// ---------------------------------------------------------------------------

#if defined(__APPLE__) || defined(_WIN32)
TEST_F(SaveTargetTest, DeferredSnapshotSerializesOnWorkerWhileMainThreadMutatesAndCollects)
{
    using namespace Inkscape::Extension::Internal;

    make_linked_document();

    // Give the serializer enough content that it takes measurable time, which
    // widens the window in which the worker reads the snapshot while the main
    // thread mutates the live tree and forces collections.
    for (int i = 0; i < 4000; ++i) {
        auto *r = doc->getReprDoc()->createElement("svg:path");
        r->setAttribute("id", "p" + std::to_string(i));
        std::string d = "M 0,0";
        for (int j = 0; j < 60; ++j) {
            d += " L " + std::to_string(i) + "," + std::to_string(j);
        }
        r->setAttribute("d", d.c_str());
        doc->getReprRoot()->appendChild(r);
        Inkscape::GC::release(r);
    }
    doc->ensureUpToDate();

    ASSERT_EQ(g_mkdir_with_parents(path("other").c_str(), 0700), 0);

    auto *module = get_output_extension_for_save(path("other/a.svg"));
    ASSERT_NE(module, nullptr);
    auto *svg = dynamic_cast<Svg *>(module->get_imp());
    ASSERT_NE(svg, nullptr);

    for (char const *suffix : {".svg", ".svgz"}) {
        // Reference, entirely on this thread: snapshot creation, serialization
        // and publication never cross a thread boundary.
        auto copy_a = doc->copy();
        ASSERT_NE(copy_a, nullptr);
        copy_a->ensureUpToDate();
        auto job_a = svg->begin_publication(
            copy_a.get(), path(std::string("other/ref") + suffix).c_str(), std::nullopt);
        ASSERT_EQ(job_a.route, "owned_bytes");
        ASSERT_TRUE(job_a.deferred.has_value());
        auto owner_a = std::move(job_a.snapshot_owner);
        auto result_a = publish(std::move(job_a));
        ASSERT_EQ(result_a.outcome, PublicationOutcome::Published);
        copy_a.reset();
        owner_a.reset();

        // Concurrent: the snapshot owner stays on this thread, the SPDocument
        // copy is destroyed before the worker starts (the snapshot must not
        // depend on it), and the worker serializes the borrowed snapshot while
        // the main thread edits the live document and runs the collector.
        auto copy_b = doc->copy();
        ASSERT_NE(copy_b, nullptr);
        copy_b->ensureUpToDate();
        auto job_b = svg->begin_publication(
            copy_b.get(), path(std::string("other/worker") + suffix).c_str(), std::nullopt);
        ASSERT_EQ(job_b.route, "owned_bytes");
        ASSERT_TRUE(job_b.deferred.has_value());
        auto owner_b = std::move(job_b.snapshot_owner); // stays on THIS thread
        copy_b.reset();

        std::atomic<bool> done{false};
        PublicationResult result_b;
        std::thread worker([&] {
            result_b = publish(std::move(job_b));
            done = true;
        });
        int iterations = 0;
        while (!done.load()) {
            // Mutate the LIVE document and collect garbage while the worker
            // reads the snapshot.
            auto *extra = doc->getReprDoc()->createElement("svg:rect");
            extra->setAttribute("id", "live" + std::to_string(iterations));
            doc->getReprRoot()->appendChild(extra);
            Inkscape::GC::release(extra);
            doc->getReprRoot()->removeChild(extra);
            Inkscape::GC::Core::gcollect();
            ++iterations;
        }
        worker.join();
        EXPECT_EQ(result_b.outcome, PublicationOutcome::Published);
        EXPECT_EQ(read_file(path(std::string("other/worker") + suffix)),
                  read_file(path(std::string("other/ref") + suffix)));
        // The worker may finish before the main thread gets to overlap, so no
        // lower bound is required; record how much overlap actually happened.
        std::printf("iterations=%d suffix=%s\n", iterations, suffix);
        owner_b.reset(); // released on this thread after the worker finished
    }
}

TEST_F(SaveTargetTest, DeferredSnapshotCapStreamsOnWorkerWithSameBytes)
{
    using namespace Inkscape::Extension::Internal;

    make_linked_document();
    Inkscape::IO::enable_file_io_test_hooks();
    g_setenv("VACARDS_SAVE_TEST_BUFFER_CAP", "64", TRUE);
    struct ResetCap {
        ~ResetCap() { g_unsetenv("VACARDS_SAVE_TEST_BUFFER_CAP"); }
    } reset_cap;

    for (char const *suffix : {".svg", ".svgz"}) {
        for (bool existing : {false, true}) {
            auto const target =
                path(std::string(existing ? "cap-stream-existing" : "cap-stream-new") + suffix);
            if (existing) ASSERT_TRUE(write_file(target, "previous bytes"));

            auto *module = get_output_extension_for_save(target);
            ASSERT_NE(module, nullptr);
            auto *svg = dynamic_cast<Svg *>(module->get_imp());
            ASSERT_NE(svg, nullptr);

            auto copy = doc->copy();
            ASSERT_NE(copy, nullptr);
            copy->ensureUpToDate();
            auto job = svg->begin_publication(copy.get(), target.c_str(), std::nullopt);
            ASSERT_EQ(job.route, "owned_bytes");
            ASSERT_TRUE(job.deferred.has_value());
            EXPECT_EQ(job.deferred->cap, 64u);
            auto owner = std::move(job.snapshot_owner);

            reset_native_serialization_count_for_testing();
            PublicationResult result;
            std::thread worker([&] { result = publish(std::move(job)); });
            worker.join();
            EXPECT_EQ(result.outcome, PublicationOutcome::Published);
            EXPECT_EQ(result.route, "direct_cap_or_allocation");
            EXPECT_EQ(native_serialization_count_for_testing(), 2u); // one buffer attempt + one streamed write
            ASSERT_TRUE(result.serialized_size.has_value());
            EXPECT_EQ(*result.serialized_size, read_file(target).size());

            // Reference without the cap, produced on this thread.
            g_unsetenv("VACARDS_SAVE_TEST_BUFFER_CAP");
            auto const reference_path =
                path(std::string("cap-ref-") + (existing ? "e" : "n") + suffix);
            auto copy2 = doc->copy();
            ASSERT_NE(copy2, nullptr);
            copy2->ensureUpToDate();
            auto job2 = svg->begin_publication(copy2.get(), reference_path.c_str(), std::nullopt);
            auto owner2 = std::move(job2.snapshot_owner);
            auto r2 = publish(std::move(job2));
            EXPECT_EQ(r2.outcome, PublicationOutcome::Published);
            EXPECT_EQ(r2.route, "owned_bytes");
            EXPECT_EQ(read_file(target), read_file(reference_path));
            g_setenv("VACARDS_SAVE_TEST_BUFFER_CAP", "64", TRUE);
            owner.reset();
            owner2.reset();
        }
    }
}
#endif
