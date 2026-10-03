// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Outcome-based tests for the F5 autosave recovery publication core
 * (src/auto-save.{h,cpp}).
 *
 * Frozen seam: AutoSave::getInstance().init(&real_app) and
 * AutoSave::getInstance().save(IO::DocumentTransaction::SystemCalls&). The bool
 * return is the timer contract (always true), never the outcome; assertions are
 * on filesystem/XML/dirty effects. Fault injection subclasses the existing
 * SystemCalls boundary and delegates production make_platform_system_calls()
 * for real disk. The oracle is independent of the serializer: a raw const XML
 * traversal fingerprint, the live contentRevision(), metadata, and explicit
 * Undo/Redo width checks; recovery SVGs are parsed back natively.
 *
 * The fixture mirrors document-file-operation-test.cpp: a real registered
 * SPDocument, bounded main-context dispatch for close, an owned g_dir_make_tmp
 * directory and exact preference restore (raw value + setness). No window, no
 * installed app, no GTEST_SKIP. Root owns CMake registration; no build/run here.
 */

#include "auto-save.h"

#include "document-undo.h"
#include "document.h"
#include "desktop.h"
#include "extension/init.h"
#include "display/cairo-utils.h"
#include "gc-anchored.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "file.h"
#include "io/document-file-transaction.h"
#include "io/recent-files.h"
#include "io/stream/bufferstream.h"
#include "object/sp-image.h"
#include "object/uri.h"
#include "preferences.h"
#include "ui/widget/gtk-registry.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/repr.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/convert.h>
#include <glibmm/miscutils.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#define F5A2_HAVE_FUNOPEN 1
#endif

using namespace Inkscape;
namespace DT = Inkscape::IO::DocumentTransaction;
bool complete_file_save_without_window_for_testing(SPDocument *, Glib::RefPtr<Gio::File> const &, bool);

namespace {

// Real native SVG: deliberate noncanonical root order, persisted namedview
// viewport (inkscape:zoom/cx/cy), and internal defs/use/clip content.
char const kFixtureSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' "
    "xmlns:xlink='http://www.w3.org/1999/xlink' "
    "xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' "
    "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
    "viewBox='0 0 96 96' height='96' width='96'>"
    "<defs><rect id='clip-src' x='0' y='0' width='48' height='48'/>"
    "<clipPath id='clip'><use xlink:href='#clip-src'/></clipPath></defs>"
    "<sodipodi:namedview id='namedview' inkscape:zoom='1.75' inkscape:cx='12.5' "
    "inkscape:cy='34.25'/>"
    "<g id='layer'><rect id='native' x='2' y='3' width='16' height='12' "
    "style='fill:#ff0000;stroke:none'/>"
    "<use id='reference' xlink:href='#native' clip-path='url(#clip)'/></g></svg>";
char const kPriorForeignSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>"
    "<rect id='foreign' width='4' height='4'/></svg>";
char const kPriorAutosaveSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='5' height='5'>"
    "<circle id='prior' cx='2' cy='2' r='2'/></svg>";
// A second owned document with id/title distinct from kFixtureSvg's `native`.
char const kSecondSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64' viewBox='0 0 64 64'>"
    "<rect id='second-native' x='1' y='1' width='8' height='8'/>"
    "<circle id='second-marker' cx='32' cy='32' r='4'/></svg>";

void dispatch_events()
{
    for (unsigned n = 0; n != 128 && g_main_context_iteration(nullptr, false); ++n) {}
}

InkscapeApplication &test_application()
{
    if (auto app = InkscapeApplication::instance()) return *app;
    Gtk::Application::wrap_in_search_entry2();
    g_setenv("INKSCAPE_APP_ID_TAG", "f5a2autosaverecovery", true);
    return *new InkscapeApplication(); // process-lifetime, as in the export fixture
}
InkscapeApplication &ensure_test_application()
{
    // These cases drive a real GTK application and the native Recent manager.
    // Fail loudly before any app creation unless the root CTest registration
    // opts in and provides an isolated XDG_DATA_HOME/profile. No skip, no
    // pass, no disabling of fatal criticals.
    char const *gui = g_getenv("INKSCAPE_TEST_GUI");
    if (!gui || std::strcmp(gui, "1") != 0) {
        throw std::runtime_error(
            "AutoSaveRecoveryTest requires INKSCAPE_TEST_GUI=1 with an isolated "
            "XDG_DATA_HOME/profile; run the registered CTest target, not the bare "
            "test binary.");
    }
    InkscapeApplication &app = test_application();
    if (!Application::exists()) Application::create(true);
    if (!gtk_is_initialized()) {
        throw std::runtime_error(
            "AutoSaveRecoveryTest requires an initialized GTK (INKSCAPE_TEST_GUI=1 "
            "with a usable display and profile).");
    }
    if (!Gtk::RecentManager::get_default()) {
        throw std::runtime_error(
            "AutoSaveRecoveryTest requires a live Gtk::RecentManager default; "
            "provide an isolated XDG_DATA_HOME/profile.");
    }
    return app;
}

std::unique_ptr<SPDocument> fixture_document(char const *xml, std::string const &filename)
{
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(xml, std::strlen(xml)), filename);
    if (!doc) throw std::runtime_error("native document fixture");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("F5-A2 fixture"), "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    doc->setModifiedSinceSave(false);
    return doc;
}

// Raw const traversal only: never the serializer, never a serialized original.
// Optionally omit one attribute of the node carrying `skip_id` so a live
// fingerprint can be compared across one exact authored change.
void append_node(XML::Node const *node, std::string &out,
                 char const *skip_id = nullptr, char const *skip_attr = nullptr)
{
    auto field = [&](char const *s) {
        if (!s) { out += "-;"; return; }
        out += std::to_string(std::strlen(s)) + ":" + s;
    };
    out += "[" + std::to_string(static_cast<int>(node->type()));
    field(node->name());
    field(node->content());
    char const *id = node->attribute("id");
    bool const skip = skip_id && id && !std::strcmp(id, skip_id);
    for (auto const &a : node->attributeList()) {
        char const *key = g_quark_to_string(a.key);
        if (skip && skip_attr && !std::strcmp(key, skip_attr)) { out += "-skipped;"; continue; }
        field(key); field(a.value);
    }
    out += ";";
    for (auto *c = node->firstChild(); c; c = c->next()) append_node(c, out, skip_id, skip_attr);
    out += "]";
}
std::string xml_fingerprint(SPDocument const &doc)
{
    std::string out;
    append_node(doc.getReprDoc(), out);
    return out;
}
std::string xml_fingerprint_without_native_width(SPDocument const &doc)
{
    std::string out;
    append_node(doc.getReprDoc(), out, "native", "width");
    return out;
}
std::unique_ptr<SPDocument> parse_svg(std::string const &bytes)
{
    return SPDocument::createNewDocFromMem(std::span<char const>(bytes.data(), bytes.size()));
}
XML::Node const *find_node(XML::Node const *root, char const *name)
{
    if (!std::strcmp(root->name(), name)) return root;
    for (auto const *c = root->firstChild(); c; c = c->next())
        if (auto const *n = find_node(c, name)) return n;
    return nullptr;
}
XML::Node *find_node(XML::Node *root, char const *name)
{
    if (!std::strcmp(root->name(), name)) return root;
    for (auto *c = root->firstChild(); c; c = c->next())
        if (auto *n = find_node(c, name)) return n;
    return nullptr;
}
XML::Node const *find_by_id(XML::Node const *root, char const *id)
{
    auto const *value = root->attribute("id");
    if (value && !std::strcmp(value, id)) return root;
    for (auto const *c = root->firstChild(); c; c = c->next())
        if (auto const *n = find_by_id(c, id)) return n;
    return nullptr;
}
XML::Node *find_by_id(XML::Node *root, char const *id)
{
    auto const *value = root->attribute("id");
    if (value && !std::strcmp(value, id)) return root;
    for (auto *c = root->firstChild(); c; c = c->next())
        if (auto *n = find_by_id(c, id)) return n;
    return nullptr;
}
std::string attribute_of(SPDocument const &doc, char const *node_name, char const *attr)
{
    auto const *node = find_node(doc.getReprDoc(), node_name);
    auto const *value = node ? node->attribute(attr) : nullptr;
    return value ? std::string(value) : std::string();
}
std::string attribute_of_id(SPDocument const &doc, char const *id, char const *attr)
{
    auto const *node = find_by_id(doc.getReprDoc(), id);
    auto const *value = node ? node->attribute(attr) : nullptr;
    return value ? std::string(value) : std::string();
}
std::string view_attributes(SPDocument const &doc)
{
    if (!find_node(doc.getReprDoc(), "sodipodi:namedview")) return {};
    return attribute_of(doc, "sodipodi:namedview", "inkscape:zoom") + "|"
        + attribute_of(doc, "sodipodi:namedview", "inkscape:cx") + "|"
        + attribute_of(doc, "sodipodi:namedview", "inkscape:cy");
}
std::string metadata_of(SPDocument const &doc)
{
    auto part = [](char const *s) { return s ? std::string(s) : std::string(); };
    std::string uri;
    if (char const *filename = doc.getDocumentFilename()) {
        try {
            uri = Glib::filename_to_uri(filename);
        } catch (...) {
            uri.clear();
        }
    }
    return uri + "|" + part(doc.getDocumentFilename()) + "|" + part(doc.getDocumentBase()) + "|"
        + part(doc.getDocumentName());
}
std::string read_file(std::string const &path)
{
    gchar *contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr)) return {};
    std::string const result(contents, length);
    g_free(contents);
    return result;
}
void write_file(std::string const &path, std::string const &bytes)
{
    ASSERT_TRUE(g_file_set_contents(path.c_str(), bytes.data(), static_cast<gssize>(bytes.size()), nullptr));
}
void deliberate_width_edit(SPDocument &doc, char const *width)
{
    auto *rect = find_by_id(doc.getReprRoot(), "native");
    if (!rect) throw std::runtime_error("fixture rect");
    ASSERT_EQ(attribute_of_id(doc, "native", "width"), "16");
    rect->setAttribute("width", width);
    doc.ensureUpToDate();
    DocumentUndo::done(&doc, Util::Internal::ContextString("F5-A2 width edit"), "");
}

// Exact restore of raw value and setness for a bounded preference set.
struct PrefGuard {
    struct Saved { std::string path; bool was_set = false; Glib::ustring raw; };
    std::vector<Saved> saved;
    explicit PrefGuard(std::initializer_list<char const *> paths)
    {
        auto *prefs = Preferences::get();
        for (auto const *path : paths) {
            auto entry = prefs->getEntry(path);
            bool const set = entry.isSet();
            saved.push_back({path, set, set ? entry.getString() : Glib::ustring()});
        }
    }
    void restore()
    {
        auto *prefs = Preferences::get();
        for (auto const &s : saved) {
            if (s.was_set) prefs->setString(s.path, s.raw);
            else prefs->remove(s.path);
        }
        saved.clear();
    }
    ~PrefGuard() { restore(); }
};

struct RecoveryHarness {
    InkscapeApplication &app;
    std::filesystem::path dir;
    PrefGuard prefs;
    SPDocument *doc = nullptr;
    sigc::connection destroy;
    std::string serial;
    SPDocument *doc2 = nullptr;
    sigc::connection destroy2;
    bool destroyed = false;
    bool doc2_destroyed = false;

    RecoveryHarness()
        : app(ensure_test_application())
        , prefs({"/options/autosave/enable", "/options/autosave/path", "/options/autosave/max",
                 "/options/svgoutput/check_on_writing", "/options/svgoutput/sort_attributes",
                 "/options/svgoutput/disable_optimizations"})
    {
        GError *error = nullptr;
        gchar *created = g_dir_make_tmp("vacards-f5a2-autosave-XXXXXX", &error);
        if (!created) {
            std::string const message = error ? error->message : "g_dir_make_tmp failed";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
        dir = std::filesystem::u8path(created);
        g_free(created);

        auto *p = Preferences::get();
        p->setBool("/options/autosave/enable", false); // fixture guard: no timer
        p->setString("/options/autosave/path", dir.string());
        p->setBool("/options/svgoutput/check_on_writing", true);
        p->setBool("/options/svgoutput/sort_attributes", true);
        p->setBool("/options/svgoutput/disable_optimizations", false);

        AutoSave::getInstance().init(&app);
        doc = app.document_add(fixture_document(kFixtureSvg, file("live.svg")));
        if (!doc) throw std::runtime_error("registered fixture document");
        destroy = doc->connectDestroy([this] { doc = nullptr; destroyed = true; });
        serial = std::to_string(doc->serial());
    }
    ~RecoveryHarness()
    {
        if (doc2) app.document_close(doc2);
        if (doc) app.document_close(doc);
        dispatch_events();
        destroy2.disconnect();
        destroy.disconnect();
        prefs.restore();
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
    std::string file(std::string const &name) const { return (dir / name).string(); }
    std::vector<std::string> names_with_prefix(std::string const &prefix) const
    {
        std::vector<std::string> names;
        GDir *d = g_dir_open(dir.string().c_str(), 0, nullptr);
        if (!d) return names;
        while (char const *entry = g_dir_read_name(d))
            if (g_str_has_prefix(entry, prefix.c_str())) names.emplace_back(entry);
        g_dir_close(d);
        std::sort(names.begin(), names.end());
        return names;
    }
    std::vector<std::string> recoveries_for(unsigned long number) const
    {
        std::vector<std::string> out;
        std::string const tag = "-" + std::to_string(number) + "-";
        for (auto const &name : names_with_prefix("autosave-"))
            if (name.find(tag) != std::string::npos) out.push_back(name);
        return out;
    }
    SPDocument *add_second(char const *xml = kSecondSvg)
    {
        doc2 = app.document_add(fixture_document(xml, file("second.svg")));
        if (!doc2) throw std::runtime_error("registered second fixture document");
        destroy2 = doc2->connectDestroy([this] { doc2 = nullptr; doc2_destroyed = true; });
        return doc2;
    }
};

void seed_prior_recoveries(RecoveryHarness const &h)
{
    write_file(h.file("prior-foreign.svg"), kPriorForeignSvg);
    write_file(h.file("prior-autosave.svg"), kPriorAutosaveSvg);
}

// Native GdkPixbuf PNG bytes for a solid RGBA image (0xRRGGBBAA). No
// hardcoded base64 and no external file: the encoder is the production one.
std::string solid_png_bytes(int width, int height, guint32 rgba)
{
    GdkPixbuf *raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!raw) throw std::runtime_error("gdk_pixbuf_new");
    gdk_pixbuf_fill(raw, rgba);
    gchar *png = nullptr;
    gsize size = 0;
    gboolean const saved = gdk_pixbuf_save_to_buffer(raw, &png, &size, "png", nullptr, nullptr);
    g_object_unref(raw);
    if (!saved || !png || size == 0) {
        if (png) g_free(png);
        throw std::runtime_error("gdk_pixbuf_save_to_buffer");
    }
    std::string bytes(png, size);
    g_free(png);
    return bytes;
}

// The live shared pixbuf is only deep-copied and converted, so the native
// document pixel store is never mutated by the oracle.
void expect_loaded_rgba_image(SPImage const &image, int width, int height, guint32 rgba)
{
    ASSERT_FALSE(image.missing);
    ASSERT_TRUE(image.pixbuf);
    EXPECT_EQ(image.pixbuf->width(), width);
    EXPECT_EQ(image.pixbuf->height(), height);
    Pixbuf pixels(*image.pixbuf);
    pixels.ensurePixelFormat(Pixbuf::PF_GDK);
    ASSERT_EQ(pixels.pixelFormat(), Pixbuf::PF_GDK);
    ASSERT_EQ(pixels.width(), width);
    ASSERT_EQ(pixels.height(), height);
    guint8 const expected[4] = {static_cast<guint8>(rgba >> 24), static_cast<guint8>(rgba >> 16),
                                static_cast<guint8>(rgba >> 8), static_cast<guint8>(rgba)};
    guchar const *data = pixels.pixels();
    int const stride = pixels.rowstride();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::size_t const offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                EXPECT_EQ(data[offset + channel], expected[channel])
                    << "pixel " << x << "," << y << " channel " << channel;
            }
        }
    }
}

// Injecting adapter over the existing SystemCalls boundary; every non-faulted
// call delegates to the production platform adapter, so the stage is real.
class FaultCalls final : public DT::SystemCalls
{
public:
    enum class Fault {
        None, PreUnsupported, WriteReadOnly, FlushFail, SyncFail, CloseFail,
        PublishFail, PublishUncertain, Conflict, SyncUnsupported, WriteShort,
    };

    char const *const kSentinel = "F5A2-CONFLICT-SENTINEL-BYTES";
    Fault fault = Fault::None;
    std::unique_ptr<DT::SystemCalls> real = DT::make_platform_system_calls();
    int create_calls = 0, flush_calls = 0, sync_calls = 0, close_calls = 0;
    int publish_calls = 0, remove_calls = 0, support_calls = 0, availability_calls = 0;
    bool last_sync_unsupported = false;
    bool fail_parent_sync = false; // report a failed directory sync after an otherwise real publication
    std::string last_staged, last_final;
    DT::PublicationStatus last_status = DT::PublicationStatus::NotAttempted;
    DT::PublicationStatus uncertain_real_status = DT::PublicationStatus::NotAttempted;

    // Narrow per-instance, one-shot callback hooks for the F5-A3 cases. Each is
    // consumed (swapped empty) before it runs, so a reentrant tick cannot fire it
    // twice. No process-global state and no new production seam.
    std::function<void()> pre_create;
    std::function<void()> before_create;
    std::function<void()> before_publish;
    std::vector<std::string> attempted_finals;

    static void consume(std::function<void()> &hook)
    {
        std::function<void()> callable;
        callable.swap(hook);
        if (callable) callable();
    }

#ifdef F5A2_HAVE_FUNOPEN
    struct ShortWriteState {
        FILE *backing = nullptr;
        std::size_t accepted = 0;
        int requested_total = 0, short_returns = 0, write_calls = 0, close_calls = 0;
    };
    ShortWriteState short_state;
    static constexpr std::size_t kBudget = 16;

    static int sw_write(void *cookie, char const *buffer, int length)
    {
        auto *s = static_cast<ShortWriteState *>(cookie);
        ++s->write_calls;
        if (length <= 0) return 0;
        s->requested_total += length;
        std::size_t const remaining = s->accepted < kBudget ? kBudget - s->accepted : 0;
        std::size_t const take = std::min<std::size_t>(remaining, static_cast<std::size_t>(length));
        std::size_t wrote = 0;
        if (take > 0) { wrote = std::fwrite(buffer, 1, take, s->backing); s->accepted += wrote; }
        if (take < static_cast<std::size_t>(length)) ++s->short_returns;
        return static_cast<int>(wrote);
    }
    static int sw_close(void *cookie)
    {
        auto *s = static_cast<ShortWriteState *>(cookie);
        ++s->close_calls;
        if (s->backing) { std::fclose(s->backing); s->backing = nullptr; }
        return 0;
    }
#endif

    DT::FailureKind pre_create_support(std::string const &parent_dir, std::string &error) override
    {
        ++support_calls;
        consume(pre_create);
        if (fault == Fault::PreUnsupported) {
            error = "injected unsupported capability";
            return DT::FailureKind::Unsupported;
        }
        return real->pre_create_support(parent_dir, error);
    }
    DT::StagingAvailability retained_stage_availability(std::string const &staged_path,
                                                        std::string &error) override
    {
        ++availability_calls;
        return real->retained_stage_availability(staged_path, error);
    }
    bool create_exclusive_file(std::string &path_template, FILE *&out,
                               bool &already_exists, std::string &error) override
    {
        ++create_calls;
        consume(before_create);
        if (fault == Fault::WriteReadOnly) {
            if (!real->create_exclusive_file(path_template, out, already_exists, error)) return false;
            std::fclose(out); // real stage exists; reopen it read-only, portably
            out = std::fopen(path_template.c_str(), "rb");
            if (!out) { error = "readonly reopen failed"; real->remove_file(path_template); return false; }
            return true;
        }
#ifdef F5A2_HAVE_FUNOPEN
        if (fault == Fault::WriteShort) {
            if (!real->create_exclusive_file(path_template, short_state.backing, already_exists, error)) return false;
            out = funopen(&short_state, nullptr, &FaultCalls::sw_write, nullptr, &FaultCalls::sw_close);
            if (!out) {
                std::fclose(short_state.backing);
                short_state.backing = nullptr;
                real->remove_file(path_template);
                error = "funopen failed";
                return false;
            }
            return true;
        }
#endif
        return real->create_exclusive_file(path_template, out, already_exists, error);
    }
    bool flush_file(FILE *stream, std::string &error) override
    {
        ++flush_calls;
        if (fault == Fault::FlushFail) { error = "injected flush failure"; return false; }
        return real->flush_file(stream, error);
    }
    bool sync_file(FILE *stream, bool &unsupported, std::string &error) override
    {
        ++sync_calls;
        if (fault == Fault::SyncFail) { error = "injected sync failure"; return false; }
        if (fault == Fault::SyncUnsupported) { unsupported = true; last_sync_unsupported = true; return true; }
        return real->sync_file(stream, unsupported, error);
    }
    bool close_file(FILE *stream, std::string &error) override
    {
        ++close_calls;
        std::string ignored;
        bool const closed = real->close_file(stream, ignored); // exactly once
        if (fault == Fault::CloseFail) { error = "injected close failure"; return false; }
        if (!closed) { error = ignored; return false; }
        return true;
    }
    DT::PublicationStatus publish_new_file(std::string const &staged_path,
                                           std::string const &final_path,
                                           std::string &error) override
    {
        ++publish_calls;
        last_staged = staged_path;
        last_final = final_path;
        consume(before_publish);
        attempted_finals.push_back(final_path);
        if (fault == Fault::PublishFail) {
            last_status = DT::PublicationStatus::Failed;
            error = "injected definite publication failure";
            return last_status;
        }
        if (fault == Fault::PublishUncertain) {
            std::string real_error;
            uncertain_real_status = real->publish_new_file(staged_path, final_path, real_error);
            last_status = DT::PublicationStatus::Uncertain;
            error = "injected ambiguous publication";
            return last_status;
        }
        if (fault == Fault::Conflict) {
            if (FILE *sentinel = std::fopen(final_path.c_str(), "wb")) {
                std::fwrite(kSentinel, 1, std::strlen(kSentinel), sentinel);
                std::fclose(sentinel);
            }
            std::string real_error;
            last_status = real->publish_new_file(staged_path, final_path, real_error);
            if (last_status != DT::PublicationStatus::Conflict) error = real_error;
            return last_status;
        }
        last_status = real->publish_new_file(staged_path, final_path, error);
        return last_status;
    }
    bool remove_file(std::string const &path) noexcept override
    {
        ++remove_calls;
        return real->remove_file(path);
    }
    bool sync_parent_directory(std::string const &parent, bool &unsupported,
                               std::string &error) override
    {
        if (fail_parent_sync) { error = "injected parent sync failure"; return false; }
        return real->sync_parent_directory(parent, unsupported, error);
    }
};

// One injected fault variant. The parameterized suite gives each its own
// RecoveryHarness, so a fatal assertion in one case cannot abort the rest.
struct FaultVariant {
    char const *name;
    FaultCalls::Fault fault;
};

} // namespace

// 1. A published recovery preserves live state and every prior file.
TEST(AutoSaveRecoveryTest, OpeningRecoveredFileKeepsUndoToStartDirty)
{
    auto &app = ensure_test_application();
    Inkscape::Extension::init();
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("vacards-recovered-open-XXXXXX", &error);
    ASSERT_NE(tmp, nullptr) << (error ? error->message : "temporary directory failed");
    g_clear_error(&error);
    std::string const dir = tmp;
    g_free(tmp);
    auto const recovery = Glib::build_filename(dir, "recovered.svg");
    auto const original = Glib::build_filename(dir, "original.svg");
    write_file(recovery, kPriorAutosaveSvg);
    Inkscape::IO::addInkscapeRecentSvg(recovery, "Recovered", {"Auto"}, original);
    dispatch_events();
    auto [opened, cancelled] = app.document_open(Gio::File::create_for_path(recovery));
    ASSERT_FALSE(cancelled);
    ASSERT_NE(opened, nullptr);
    EXPECT_TRUE(opened->isModifiedSinceSave());
    DocumentUndo::setUndoSensitive(opened, true);
    opened->getObjectById("prior")->getRepr()->setAttribute("cx", "3");
    DocumentUndo::done(opened, Util::Internal::ContextString("edit recovered file"), "");
    ASSERT_TRUE(DocumentUndo::undo(opened));
    EXPECT_TRUE(opened->isModifiedSinceSave());
    app.document_close(opened);
    Inkscape::IO::removeInkscapeRecent(recovery);
    g_remove(recovery.c_str());
    g_rmdir(dir.c_str());
}

TEST(AutoSaveRecoveryTest, CompletionWithoutWindowRemembersPathAndReportsNotice)
{
    ensure_test_application();
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("vacards-completion-XXXXXX", &error);
    ASSERT_NE(tmp, nullptr) << (error ? error->message : "temporary directory failed");
    g_clear_error(&error);
    std::string const dir = tmp;
    g_free(tmp);
    auto const target = Glib::build_filename(dir, "saved.svg");
    write_file(target, kPriorAutosaveSvg);
    auto doc = fixture_document(kPriorAutosaveSvg, target);
    doc->setDocumentFilename(target.c_str());
    auto file = Gio::File::create_for_path(target);
    auto *prefs = Inkscape::Preferences::get();
    auto const prior_path = prefs->getString("/dialogs/save_as/path");
    Inkscape::IO::enable_file_io_test_hooks();
    EXPECT_TRUE(complete_file_save_without_window_for_testing(doc.get(), file, true));
    dispatch_events();
    EXPECT_EQ(prefs->getString("/dialogs/save_as/path"), dir);
    EXPECT_NE(g_object_get_data(G_OBJECT(file->gobj()), "vacards-recent-add-called"), nullptr);
    Inkscape::IO::reset_file_io_test_hooks_for_testing();
    prefs->setString("/dialogs/save_as/path", prior_path);
    Inkscape::IO::removeInkscapeRecent(target);
    g_remove(target.c_str());
    g_rmdir(dir.c_str());
}

TEST(AutoSaveRecoveryTest, CompletionWithoutRememberPathLeavesRecentAndPreferenceUntouched)
{
    ensure_test_application();
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("vacards-no-remember-XXXXXX", &error);
    ASSERT_NE(tmp, nullptr) << (error ? error->message : "temporary directory failed");
    g_clear_error(&error);
    std::string const dir = tmp;
    g_free(tmp);
    auto const target = Glib::build_filename(dir, "saved.svg");
    write_file(target, kPriorAutosaveSvg);
    auto doc = fixture_document(kPriorAutosaveSvg, target);
    doc->setDocumentFilename(target.c_str());
    auto file = Gio::File::create_for_path(target);
    auto *prefs = Inkscape::Preferences::get();
    auto const prior_path = prefs->getString("/dialogs/save_as/path");
    Inkscape::IO::enable_file_io_test_hooks();
    EXPECT_TRUE(complete_file_save_without_window_for_testing(doc.get(), file, false));
    EXPECT_EQ(prefs->getString("/dialogs/save_as/path"), prior_path);
    EXPECT_EQ(g_object_get_data(G_OBJECT(file->gobj()), "vacards-recent-add-called"), nullptr);
    Inkscape::IO::reset_file_io_test_hooks_for_testing();
    g_remove(target.c_str());
    g_rmdir(dir.c_str());
}

TEST(AutoSaveRecoveryTest, DeclinedOverwriteLeavesRecentAndPreferenceUntouched)
{
    auto &app = ensure_test_application();
    Inkscape::Extension::init();
    app.gio_app()->register_application();
    Inkscape::UI::Widget::register_all();
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("vacards-declined-save-XXXXXX", &error);
    ASSERT_NE(tmp, nullptr) << (error ? error->message : "temporary directory failed");
    g_clear_error(&error);
    std::string const dir = tmp;
    g_free(tmp);
    auto const target = Glib::build_filename(dir, "existing.svg");
    write_file(target, kPriorAutosaveSvg);
    auto *doc = app.document_add(fixture_document(kFixtureSvg, Glib::build_filename(dir, "live.svg")));
    ASSERT_NE(doc, nullptr);
    auto *desktop = app.createDesktop(doc, false, true);
    ASSERT_NE(desktop, nullptr);
    auto *window = desktop->getInkscapeWindow();
    ASSERT_NE(window, nullptr);
    auto file = Gio::File::create_for_path(target);
    auto *prefs = Inkscape::Preferences::get();
    auto const prior_path = prefs->getString("/dialogs/save_as/path");
    Inkscape::IO::removeInkscapeRecent(target);
    Inkscape::IO::enable_file_io_test_hooks();
    bool saw_dialog = false;
    guint const cancel = g_timeout_add(10, +[](gpointer data) -> gboolean {
        auto *seen = static_cast<bool *>(data);
        auto *model = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
            auto *item = g_list_model_get_item(model, i);
            if (GTK_IS_MESSAGE_DIALOG(item)) {
                *seen = true;
                gtk_dialog_response(GTK_DIALOG(item), GTK_RESPONSE_NO);
                g_object_unref(item);
                return G_SOURCE_REMOVE;
            }
            g_object_unref(item);
        }
        return G_SOURCE_CONTINUE;
    }, &saw_dialog);
    EXPECT_EQ(sp_file_save_bound(*window, doc, desktop, file,
              Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS), FileSaveResult::RetryWithDifferentName);
    if (!saw_dialog) g_source_remove(cancel);
    EXPECT_TRUE(saw_dialog);
    EXPECT_EQ(g_object_get_data(G_OBJECT(file->gobj()), "vacards-recent-add-called"), nullptr);
    EXPECT_FALSE(Inkscape::IO::getInkscapeRecent(target));
    EXPECT_EQ(prefs->getString("/dialogs/save_as/path"), prior_path);
    Inkscape::IO::reset_file_io_test_hooks_for_testing();
    app.desktopClose(desktop);
    app.document_close(doc);
    dispatch_events();
    g_remove(target.c_str());
    g_rmdir(dir.c_str());
}

TEST(AutoSaveRecoveryTest, PublishedRecoveryPreservesLiveStateAndOldFiles)
{
    RecoveryHarness h;
    seed_prior_recoveries(h);
    std::string const foreign_bytes = read_file(h.file("prior-foreign.svg"));
    std::string const prior_bytes = read_file(h.file("prior-autosave.svg"));
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true); // both save and autosave dirty

    std::string const before_fp = xml_fingerprint(*h.doc);
    auto const before_rev = h.doc->getReprDoc()->contentRevision();
    std::string const before_meta = metadata_of(*h.doc);
    std::string const before_view = view_attributes(*h.doc);
    bool const before_virgin = h.doc->getVirgin();
    ASSERT_TRUE(before_rev.has_value());
    // Exact persisted viewport, so the later preservation compare cannot pass
    // on empty/missing attributes.
    ASSERT_EQ(before_view, "1.75|12.5|34.25");

    FaultCalls calls;
    AutoSave::getInstance().save(calls);
    ASSERT_EQ(calls.publish_calls, 1);
    ASSERT_EQ(calls.last_status, DT::PublicationStatus::Published);

    std::vector<std::string> const recoveries = h.names_with_prefix("autosave-");
    ASSERT_EQ(recoveries.size(), 1u);
    std::string const svg = read_file(h.file(recoveries.front()));
    ASSERT_FALSE(svg.empty());
    auto parsed = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(attribute_of_id(*parsed, "native", "width"), "23");
    EXPECT_NE(find_node(parsed->getReprDoc(), "svg:defs"), nullptr);
    EXPECT_NE(find_node(parsed->getReprDoc(), "svg:clipPath"), nullptr);
    EXPECT_NE(find_node(parsed->getReprDoc(), "svg:use"), nullptr);
    EXPECT_EQ(view_attributes(*parsed), before_view);

    EXPECT_EQ(read_file(h.file("prior-foreign.svg")), foreign_bytes);
    EXPECT_EQ(read_file(h.file("prior-autosave.svg")), prior_bytes);
    EXPECT_EQ(xml_fingerprint(*h.doc), before_fp);
    EXPECT_EQ(h.doc->getReprDoc()->contentRevision(), before_rev);
    EXPECT_EQ(metadata_of(*h.doc), before_meta);
    EXPECT_EQ(view_attributes(*h.doc), before_view);
    EXPECT_EQ(h.doc->getVirgin(), before_virgin);
    EXPECT_TRUE(h.doc->isModifiedSinceSave());
    EXPECT_FALSE(h.doc->isModifiedSinceAutoSave());

    // Only a published recovery may enter native Recent. The harness gate
    // guarantees GTK and a non-empty Recent manager default, so this is a
    // mandatory positive check: exact file, "Auto" group, and the captured
    // original document URI as the private-item description.
    std::string const published_path = h.file(recoveries.front());
    auto recent = Inkscape::IO::getInkscapeRecent(published_path);
    ASSERT_TRUE(recent) << published_path;
    EXPECT_TRUE(recent->has_group("Auto"));
    EXPECT_EQ(recent->get_description(), Glib::filename_to_uri(h.doc->getDocumentFilename()));

    // The real width 16->23 Undo step survived autosave and still round-trips.
    ASSERT_TRUE(DocumentUndo::undo(h.doc));
    EXPECT_EQ(attribute_of_id(*h.doc, "native", "width"), "16");
    ASSERT_TRUE(DocumentUndo::redo(h.doc));
    EXPECT_EQ(attribute_of_id(*h.doc, "native", "width"), "23");
}

// 2. Repeated successful ticks keep every recovery while within the max.
TEST(AutoSaveRecoveryTest, RepeatedSuccessfulTicksKeepEveryRecovery)
{
    RecoveryHarness h;
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true);

    FaultCalls first;
    AutoSave::getInstance().save(first);
    ASSERT_EQ(first.publish_calls, 1);
    std::vector<std::string> const after_first = h.names_with_prefix("autosave-");
    ASSERT_EQ(after_first.size(), 1u);
    std::string const first_bytes = read_file(h.file(after_first.front()));
    EXPECT_FALSE(h.doc->isModifiedSinceAutoSave());
    EXPECT_TRUE(h.doc->isModifiedSinceSave());

    Preferences::get()->setInt("/options/autosave/max", 10); // within the limit: nothing is pruned
    h.doc->setModifiedSinceSave(true);
    FaultCalls second;
    AutoSave::getInstance().save(second);
    ASSERT_EQ(second.publish_calls, 1);

    std::vector<std::string> const recoveries = h.names_with_prefix("autosave-");
    ASSERT_EQ(recoveries.size(), 2u);
    EXPECT_NE(recoveries[0], recoveries[1]);
    EXPECT_EQ(read_file(h.file(after_first.front())), first_bytes);
    auto session_of = [&](std::string const &name) {
        auto const pos = name.find("-" + h.serial + "-");
        return pos == std::string::npos ? std::string() : name.substr(0, pos);
    };
    EXPECT_FALSE(session_of(recoveries[0]).empty());
    EXPECT_EQ(session_of(recoveries[0]), session_of(recoveries[1]));
    for (auto const &name : recoveries) {
        EXPECT_NE(name.find("-" + h.serial + "-"), std::string::npos);
        std::string const svg = read_file(h.file(name));
        auto parsed = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
        EXPECT_TRUE(parsed) << name;
        if (parsed) EXPECT_EQ(attribute_of_id(*parsed, "native", "width"), "23");
    }
}

// H1: /options/autosave/max bounds this session's published generations.
TEST(AutoSaveRecoveryTest, RetentionKeepsOnlyNewestMaxGenerationsAndRecentEntries)
{
    RecoveryHarness h;
    seed_prior_recoveries(h); // legacy/foreign files: never pruned
    std::string const foreign_bytes = read_file(h.file("prior-foreign.svg"));
    std::string const prior_bytes = read_file(h.file("prior-autosave.svg"));
    // Another session's recovery for the same serial: never pruned either.
    std::string const other_session = "autosave-00000000-other-session-" + h.serial + "-0.svg";
    write_file(h.file(other_session), kPriorAutosaveSvg);

    Preferences::get()->setInt("/options/autosave/max", 2);
    std::vector<std::string> all_published;
    for (int i = 0; i < 5; ++i) {
        {
            auto *rect = find_by_id(h.doc->getReprRoot(), "native");
            ASSERT_NE(rect, nullptr);
            rect->setAttribute("width", std::to_string(20 + i));
            h.doc->ensureUpToDate();
            DocumentUndo::done(h.doc, Util::Internal::ContextString("H1 retention edit"), "");
        }
        FaultCalls calls;
        AutoSave::getInstance().save(calls);
        ASSERT_EQ(calls.publish_calls, 1) << "tick " << i;
        ASSERT_EQ(calls.last_status, DT::PublicationStatus::Published);
        std::vector<std::string> mine;
        for (auto const &name : h.recoveries_for(std::stoul(h.serial)))
            if (name != other_session) mine.push_back(name);
        for (auto const &name : mine)
            if (std::find(all_published.begin(), all_published.end(), name) == all_published.end())
                all_published.push_back(name);
        EXPECT_LE(mine.size(), 2u) << "tick " << i;
    }
    ASSERT_EQ(all_published.size(), 5u);

    std::vector<std::string> remaining;
    for (auto const &name : h.recoveries_for(std::stoul(h.serial)))
        if (name != other_session) remaining.push_back(name);
    ASSERT_EQ(remaining.size(), 2u);
    // The two newest generations survive, the three oldest are gone.
    std::sort(all_published.begin(), all_published.end(), [](std::string const &a, std::string const &b) {
        auto gen = [](std::string const &n) { return std::stoul(n.substr(n.rfind('-') + 1)); };
        return gen(a) < gen(b);
    });
    EXPECT_EQ(remaining[0] == all_published[3] || remaining[1] == all_published[3], true);
    EXPECT_EQ(remaining[0] == all_published[4] || remaining[1] == all_published[4], true);
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(Inkscape::IO::getInkscapeRecent(h.file(all_published[i]))) << all_published[i];
    }
    for (int i = 3; i < 5; ++i) {
        EXPECT_TRUE(Inkscape::IO::getInkscapeRecent(h.file(all_published[i]))) << all_published[i];
    }

    // Recovery still offers the newest content.
    std::string const newest = read_file(h.file(all_published[4]));
    auto parsed = SPDocument::createNewDocFromMem(std::span<char const>(newest.data(), newest.size()));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(attribute_of_id(*parsed, "native", "width"), "24");

    EXPECT_EQ(read_file(h.file("prior-foreign.svg")), foreign_bytes);
    EXPECT_EQ(read_file(h.file("prior-autosave.svg")), prior_bytes);
    EXPECT_FALSE(read_file(h.file(other_session)).empty());
}

// H1 (finding 6c): a Published generation whose directory sync failed must not
// cost an older recovery; pruning resumes after the next durable generation.
TEST(AutoSaveRecoveryTest, RetentionWaitsForDurablePublication)
{
    RecoveryHarness h;
    Preferences::get()->setInt("/options/autosave/max", 1);
    auto tick = [&](int width, bool fail_sync) {
        auto *rect = find_by_id(h.doc->getReprRoot(), "native");
        ASSERT_NE(rect, nullptr);
        rect->setAttribute("width", std::to_string(width));
        h.doc->ensureUpToDate();
        DocumentUndo::done(h.doc, Util::Internal::ContextString("H1 durability edit"), "");
        FaultCalls calls;
        calls.fail_parent_sync = fail_sync;
        AutoSave::getInstance().save(calls);
        ASSERT_EQ(calls.publish_calls, 1);
    };
    tick(20, false);
    EXPECT_EQ(h.recoveries_for(std::stoul(h.serial)).size(), 1u);
    tick(21, true);
    auto const after_unsynced = h.recoveries_for(std::stoul(h.serial));
    EXPECT_EQ(after_unsynced.size(), 2u) << "an unsynced new generation must not prune the older one";
    tick(22, false);
    EXPECT_EQ(h.recoveries_for(std::stoul(h.serial)).size(), 1u) << "a durable generation prunes the backlog";
}

// 3/4. Injected faults preserve prior recoveries and never falsely publish.
// Each variant is an independent parameterized case with its own fixture, so a
// fatal assertion in one case cannot abort the remaining faults.
class AutoSaveRecoveryFaultTest : public ::testing::TestWithParam<FaultVariant> {};

TEST_P(AutoSaveRecoveryFaultTest, PreservesPriorAndDoesNotFalselyPublish)
{
    FaultVariant const &c = GetParam();
    RecoveryHarness h;
    seed_prior_recoveries(h);
    std::string const foreign_bytes = read_file(h.file("prior-foreign.svg"));
    std::string const prior_bytes = read_file(h.file("prior-autosave.svg"));
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true);
    std::string const before_fp = xml_fingerprint(*h.doc);
    auto const before_rev = h.doc->getReprDoc()->contentRevision();

    FaultCalls calls;
    calls.fault = c.fault;
    AutoSave::getInstance().save(calls);

    ASSERT_TRUE(before_rev.has_value());
    EXPECT_EQ(read_file(h.file("prior-foreign.svg")), foreign_bytes);
    EXPECT_EQ(read_file(h.file("prior-autosave.svg")), prior_bytes);
    EXPECT_EQ(xml_fingerprint(*h.doc), before_fp);
    EXPECT_EQ(h.doc->getReprDoc()->contentRevision(), before_rev);
    EXPECT_TRUE(h.doc->isModifiedSinceSave());
    EXPECT_LE(calls.close_calls, 1);

    if (c.fault == FaultCalls::Fault::SyncUnsupported) {
        // 4. A capability gap is a limited result, not a failed publish.
        EXPECT_EQ(calls.publish_calls, 1);
        EXPECT_EQ(calls.last_status, DT::PublicationStatus::Published);
        EXPECT_TRUE(calls.last_sync_unsupported);
        EXPECT_FALSE(h.doc->isModifiedSinceAutoSave());
        std::vector<std::string> const published = h.names_with_prefix("autosave-");
        ASSERT_EQ(published.size(), 1u);
        std::string const published_bytes = read_file(h.file(published.front()));
        ASSERT_FALSE(published_bytes.empty());
        auto recovered = SPDocument::createNewDocFromMem(
            std::span<char const>(published_bytes.data(), published_bytes.size()));
        ASSERT_TRUE(recovered);
        EXPECT_EQ(attribute_of_id(*recovered, "native", "width"), "23");
    } else if (c.fault == FaultCalls::Fault::PreUnsupported) {
        EXPECT_EQ(calls.support_calls, 1);
        EXPECT_EQ(calls.create_calls, 0);
        EXPECT_EQ(calls.publish_calls, 0);
        EXPECT_TRUE(h.doc->isModifiedSinceAutoSave());
    } else if (c.fault == FaultCalls::Fault::PublishUncertain) {
        EXPECT_EQ(calls.publish_calls, 1);
        EXPECT_EQ(calls.last_status, DT::PublicationStatus::Uncertain);
        EXPECT_EQ(calls.uncertain_real_status, DT::PublicationStatus::Published);
        EXPECT_EQ(calls.availability_calls, 1);
        ASSERT_FALSE(calls.last_staged.empty());
        ASSERT_FALSE(calls.last_final.empty());
        ASSERT_TRUE(std::filesystem::is_regular_file(calls.last_staged));
        ASSERT_TRUE(std::filesystem::is_regular_file(calls.last_final));
        std::string const staged_bytes = read_file(calls.last_staged);
        std::string const final_bytes = read_file(calls.last_final);
        ASSERT_FALSE(staged_bytes.empty());
        ASSERT_FALSE(final_bytes.empty());
        EXPECT_EQ(staged_bytes, final_bytes);
        auto recovered = SPDocument::createNewDocFromMem(
            std::span<char const>(final_bytes.data(), final_bytes.size()));
        ASSERT_TRUE(recovered);
        EXPECT_EQ(attribute_of_id(*recovered, "native", "width"), "23");
        EXPECT_TRUE(h.doc->isModifiedSinceAutoSave());
        EXPECT_EQ(h.names_with_prefix("autosave-").size(), 1u);
        EXPECT_EQ(h.names_with_prefix("vacards-new-").size(), 1u);
    } else if (c.fault == FaultCalls::Fault::Conflict) {
        EXPECT_EQ(calls.publish_calls, 1);
        EXPECT_EQ(calls.last_status, DT::PublicationStatus::Conflict);
        ASSERT_FALSE(calls.last_final.empty());
        EXPECT_EQ(read_file(calls.last_final), std::string(calls.kSentinel));
        EXPECT_TRUE(h.doc->isModifiedSinceAutoSave());
        EXPECT_EQ(h.names_with_prefix("vacards-new-").size(), 0u);
    } else if (c.fault == FaultCalls::Fault::PublishFail) {
        EXPECT_EQ(calls.publish_calls, 1);
        EXPECT_EQ(calls.last_status, DT::PublicationStatus::Failed);
        EXPECT_TRUE(h.doc->isModifiedSinceAutoSave());
        EXPECT_EQ(h.names_with_prefix("autosave-").size(), 0u);
        EXPECT_EQ(h.names_with_prefix("vacards-new-").size(), 0u);
    } else {
        EXPECT_EQ(calls.publish_calls, 0);
        EXPECT_TRUE(h.doc->isModifiedSinceAutoSave());
        EXPECT_EQ(h.names_with_prefix("autosave-").size(), 0u);
        EXPECT_EQ(h.names_with_prefix("vacards-new-").size(), 0u);
    }
    if (c.fault == FaultCalls::Fault::CloseFail) EXPECT_EQ(calls.close_calls, 1);
    // Native Recent must never gain a non-published recovery. The harness gate
    // guarantees GTK and a live Recent manager, so this is mandatory; there is
    // no conditional bypass and no "pending" RecordProperty.
    if (!calls.last_final.empty() && c.fault != FaultCalls::Fault::SyncUnsupported) {
        EXPECT_FALSE(Inkscape::IO::getInkscapeRecent(calls.last_final));
    }
}

INSTANTIATE_TEST_SUITE_P(
    InjectedFaults, AutoSaveRecoveryFaultTest,
    ::testing::Values(
        FaultVariant{"pre_create_unsupported", FaultCalls::Fault::PreUnsupported},
        FaultVariant{"staging_write_readonly", FaultCalls::Fault::WriteReadOnly},
        FaultVariant{"flush_failure", FaultCalls::Fault::FlushFail},
        FaultVariant{"sync_failure", FaultCalls::Fault::SyncFail},
        FaultVariant{"close_failure", FaultCalls::Fault::CloseFail},
        FaultVariant{"definite_publish_failure", FaultCalls::Fault::PublishFail},
        FaultVariant{"uncertain_may_have_applied", FaultCalls::Fault::PublishUncertain},
        FaultVariant{"conflict_sentinel", FaultCalls::Fault::Conflict},
        FaultVariant{"unsupported_sync_publishes", FaultCalls::Fault::SyncUnsupported}),
    [](::testing::TestParamInfo<FaultVariant> const &info) { return std::string(info.param.name); });

#ifdef F5A2_HAVE_FUNOPEN
// SHORTWRITE: native funopen around the real stage; partial prefix, no publish.
TEST(AutoSaveRecoveryTest, ShortWriteMacBsdStopsBeforePublish)
{
    RecoveryHarness h;
    seed_prior_recoveries(h);
    std::string const foreign_bytes = read_file(h.file("prior-foreign.svg"));
    std::string const prior_bytes = read_file(h.file("prior-autosave.svg"));
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true);
    std::string const before_fp = xml_fingerprint(*h.doc);

    FaultCalls calls;
    calls.fault = FaultCalls::Fault::WriteShort;
    AutoSave::getInstance().save(calls);

    EXPECT_GT(calls.short_state.accepted, 0u);
    EXPECT_LT(calls.short_state.accepted, static_cast<std::size_t>(calls.short_state.requested_total));
    EXPECT_GT(calls.short_state.short_returns, 0);
    EXPECT_GT(calls.short_state.write_calls, 0);
    EXPECT_EQ(calls.short_state.close_calls, 1); // backing closed exactly once
    EXPECT_EQ(calls.publish_calls, 0);           // write/seal never reached publish
    EXPECT_TRUE(h.doc->isModifiedSinceAutoSave());
    EXPECT_TRUE(h.doc->isModifiedSinceSave());
    EXPECT_EQ(read_file(h.file("prior-foreign.svg")), foreign_bytes);
    EXPECT_EQ(read_file(h.file("prior-autosave.svg")), prior_bytes);
    EXPECT_EQ(xml_fingerprint(*h.doc), before_fp);
    EXPECT_EQ(h.names_with_prefix("autosave-").size(), 0u);
    EXPECT_EQ(h.names_with_prefix("vacards-new-").size(), 0u);
}
#endif // F5A2_HAVE_FUNOPEN

// F5-A3 callback and failure-isolation outcomes. Each case is independent and
// asserts filesystem/XML/dirty effects, never the timer bool.

// 1. A nested tick inside a create callback is a no-op for the whole tick: the
//    support/create/publish counts are unchanged, so this distinguishes the
//    whole-tick guard from a per-document foreign-lease skip (which would still
//    save the second document).
TEST(AutoSaveRecoveryTest, NestedTickDoesNotStartSecondDocumentEarly)
{
    RecoveryHarness h;
    h.add_second();
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true);
    h.doc2->setModifiedSinceSave(true);
    std::string const first_fp = xml_fingerprint(*h.doc);
    std::string const second_fp = xml_fingerprint(*h.doc2);
    unsigned long const first_serial = h.doc->serial();
    unsigned long const second_serial = h.doc2->serial();
    ASSERT_NE(first_serial, second_serial);

    FaultCalls calls;
    int nested_support = -1, nested_create = -1, nested_publish = -1;
    bool nested_return = false, callback_fired = false;
    calls.before_create = [&] {
        callback_fired = true;
        int const s0 = calls.support_calls, c0 = calls.create_calls, p0 = calls.publish_calls;
        nested_return = AutoSave::getInstance().save(calls);
        nested_support = calls.support_calls;
        nested_create = calls.create_calls;
        nested_publish = calls.publish_calls;
        EXPECT_EQ(nested_support, s0);
        EXPECT_EQ(nested_create, c0);
        EXPECT_EQ(nested_publish, p0);
    };
    AutoSave::getInstance().save(calls);

    EXPECT_TRUE(callback_fired);
    EXPECT_TRUE(nested_return);
    EXPECT_GE(nested_support, 0);
    EXPECT_EQ(calls.publish_calls, 2);
    ASSERT_EQ(calls.attempted_finals.size(), 2u);
    auto const first_recoveries = h.recoveries_for(first_serial);
    auto const second_recoveries = h.recoveries_for(second_serial);
    ASSERT_EQ(first_recoveries.size(), 1u);
    ASSERT_EQ(second_recoveries.size(), 1u);
    for (std::string const *name : {&first_recoveries.front(), &second_recoveries.front()}) {
        auto parsed = parse_svg(read_file(h.file(*name)));
        ASSERT_TRUE(parsed) << *name;
    }
    EXPECT_FALSE(h.doc->isModifiedSinceAutoSave());
    EXPECT_FALSE(h.doc2->isModifiedSinceAutoSave());
    EXPECT_TRUE(h.doc->isModifiedSinceSave());
    EXPECT_TRUE(h.doc2->isModifiedSinceSave());
    EXPECT_EQ(xml_fingerprint(*h.doc), first_fp);
    EXPECT_EQ(xml_fingerprint(*h.doc2), second_fp);
}

// 2. A foreign operation lease and a live rollbackable interaction each skip
//    the whole document (no create/publish/file) while the real readiness probe
//    is false; a later real save publishes, proving the guard resets.
TEST(AutoSaveRecoveryTest, BusyLeaseAndLiveInteractionAreSkipped)
{
    RecoveryHarness h;
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true);
    std::string const before_fp = xml_fingerprint(*h.doc);

    {
        auto foreign = Inkscape::DocumentUndo::holdInteractionOperation(h.doc);
        ASSERT_TRUE(foreign);
        EXPECT_FALSE(Inkscape::DocumentUndo::fileOperationFreshReady(h.doc));
        FaultCalls calls;
        AutoSave::getInstance().save(calls);
        EXPECT_EQ(calls.support_calls, 0);
        EXPECT_EQ(calls.create_calls, 0);
        EXPECT_EQ(calls.publish_calls, 0);
        EXPECT_TRUE(calls.attempted_finals.empty());
        EXPECT_TRUE(h.names_with_prefix("autosave-").empty());
        EXPECT_TRUE(h.doc->isModifiedSinceSave());
        EXPECT_TRUE(h.doc->isModifiedSinceAutoSave());
        EXPECT_EQ(xml_fingerprint(*h.doc), before_fp);
    }

    {
        auto token = Inkscape::DocumentUndo::beginRollbackableInteraction(h.doc);
        ASSERT_TRUE(token.has_value());
        EXPECT_FALSE(Inkscape::DocumentUndo::fileOperationFreshReady(h.doc));
        FaultCalls calls;
        AutoSave::getInstance().save(calls);
        EXPECT_EQ(calls.support_calls, 0);
        EXPECT_EQ(calls.create_calls, 0);
        EXPECT_EQ(calls.publish_calls, 0);
        EXPECT_TRUE(h.doc->isModifiedSinceSave());
        EXPECT_TRUE(h.doc->isModifiedSinceAutoSave());
        token->rollback();
    }

    EXPECT_TRUE(Inkscape::DocumentUndo::fileOperationFreshReady(h.doc));
    FaultCalls after;
    AutoSave::getInstance().save(after);
    EXPECT_EQ(after.publish_calls, 1);
    EXPECT_FALSE(h.doc->isModifiedSinceAutoSave());
    EXPECT_EQ(h.recoveries_for(h.doc->serial()).size(), 1u);
    EXPECT_EQ(xml_fingerprint(*h.doc), before_fp);
}

// 3. A per-document pre-create failure is isolated: the failing document stays
//    dirty with unchanged XML and is never published, while the next document
//    publishes its own actual native content and clears autosave. The injected
//    throw is converted by the F3 NewDocumentFile create pre-create catch to
//    Unsupported, so this proves per-document failure isolation, not a direct
//    outer AutoSave catch.
TEST(AutoSaveRecoveryTest, PreCreateFailureDoesNotStopNextDocument)
{
    RecoveryHarness h;
    h.add_second();
    seed_prior_recoveries(h);
    std::string const foreign_bytes = read_file(h.file("prior-foreign.svg"));
    std::string const prior_bytes = read_file(h.file("prior-autosave.svg"));
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true);
    h.doc2->setModifiedSinceSave(true);

    std::vector<SPDocument *> const order = h.app.get_documents();
    ASSERT_EQ(order.size(), 2u);
    SPDocument *first = order[0];
    SPDocument *second = order[1];
    ASSERT_NE(first, second);
    unsigned long const first_serial = first->serial();
    unsigned long const second_serial = second->serial();
    std::string const first_fp = xml_fingerprint(*first);

    // Registry order is unconstrained: capture the actual second document's
    // native identity, width, marker and document title BEFORE the one-shot
    // callback. Never assume h.doc2 is the second processed document.
    std::string const second_native_id =
        find_by_id(second->getReprDoc(), "native") ? "native" : "second-native";
    ASSERT_FALSE(second_native_id.empty());
    std::string const second_width = attribute_of_id(*second, second_native_id.c_str(), "width");
    std::string const second_marker_id =
        find_by_id(second->getReprDoc(), "second-marker") ? "second-marker" : "reference";
    char const *second_docname = second->getReprDoc()->root()->attribute("sodipodi:docname");
    std::string const second_title = second_docname ? second_docname : std::string();

    FaultCalls calls;
    calls.pre_create = [] { throw std::runtime_error("injected pre-create callback failure"); };
    AutoSave::getInstance().save(calls);

    EXPECT_EQ(calls.publish_calls, 1);
    ASSERT_EQ(calls.attempted_finals.size(), 1u);
    EXPECT_TRUE(h.recoveries_for(first_serial).empty());
    auto const second_recoveries = h.recoveries_for(second_serial);
    ASSERT_EQ(second_recoveries.size(), 1u);
    auto parsed = parse_svg(read_file(h.file(second_recoveries.front())));
    ASSERT_TRUE(parsed);
    // The published file must carry the actual second document's content.
    EXPECT_NE(find_by_id(parsed->getReprDoc(), second_native_id.c_str()), nullptr);
    EXPECT_EQ(attribute_of_id(*parsed, second_native_id.c_str(), "width"), second_width);
    EXPECT_NE(find_by_id(parsed->getReprDoc(), second_marker_id.c_str()), nullptr);
    char const *other_native_id =
        std::strcmp(second_native_id.c_str(), "native") == 0 ? "second-native" : "native";
    EXPECT_EQ(find_by_id(parsed->getReprDoc(), other_native_id), nullptr);
    char const *parsed_docname = parsed->getReprDoc()->root()->attribute("sodipodi:docname");
    EXPECT_EQ(parsed_docname ? parsed_docname : "", second_title);

    EXPECT_TRUE(first->isModifiedSinceSave());
    EXPECT_TRUE(first->isModifiedSinceAutoSave());
    EXPECT_EQ(xml_fingerprint(*first), first_fp);
    EXPECT_TRUE(second->isModifiedSinceSave());
    EXPECT_FALSE(second->isModifiedSinceAutoSave());
    EXPECT_EQ(read_file(h.file("prior-foreign.svg")), foreign_bytes);
    EXPECT_EQ(read_file(h.file("prior-autosave.svg")), prior_bytes);
}

// 4. Closing the actual later document from the first document's publication
//    callback is safe: the deferred serial is re-resolved and skipped, so
//    exactly one recovery is written and no dead pointer is dereferenced.
TEST(AutoSaveRecoveryTest, ClosingLaterDocumentDuringEarlierPublicationIsSafe)
{
    RecoveryHarness h;
    h.add_second();
    seed_prior_recoveries(h);
    std::string const foreign_bytes = read_file(h.file("prior-foreign.svg"));
    std::string const prior_bytes = read_file(h.file("prior-autosave.svg"));
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true);
    h.doc2->setModifiedSinceSave(true);

    std::vector<SPDocument *> const order = h.app.get_documents();
    ASSERT_EQ(order.size(), 2u);
    SPDocument *first = order[0];
    SPDocument *later = order[1];
    ASSERT_NE(first, later);
    unsigned long const first_serial = first->serial();
    unsigned long const later_serial = later->serial();
    std::string const first_fp = xml_fingerprint(*first);
    // Registry order is unconstrained: capture the actual first document's
    // native identity, width and marker before the callback.
    std::string const first_native_id =
        find_by_id(first->getReprDoc(), "native") ? "native" : "second-native";
    ASSERT_FALSE(first_native_id.empty());
    std::string const first_width = attribute_of_id(*first, first_native_id.c_str(), "width");
    std::string const first_marker_id =
        find_by_id(first->getReprDoc(), "second-marker") ? "second-marker" : "reference";

    // Local destroy observers on both documents; the harness's existing
    // callbacks independently null their own slots. Scoped connections
    // disconnect before the observer bools leave scope.
    bool later_destroyed = false;
    bool first_destroyed = false;
    sigc::scoped_connection later_observer =
        later->connectDestroy([&later_destroyed] { later_destroyed = true; });
    sigc::scoped_connection first_observer =
        first->connectDestroy([&first_destroyed] { first_destroyed = true; });

    FaultCalls calls;
    calls.before_publish = [&] { h.app.document_close(later); };
    AutoSave::getInstance().save(calls);
    dispatch_events();

    // The actual later document must be the one destroyed, not merely one slot.
    EXPECT_TRUE(later_destroyed);
    EXPECT_FALSE(first_destroyed);
    for (SPDocument *d : h.app.get_documents()) EXPECT_NE(d, later);

    EXPECT_EQ(calls.publish_calls, 1);
    ASSERT_EQ(calls.attempted_finals.size(), 1u);
    EXPECT_TRUE(h.recoveries_for(later_serial).empty());
    auto const first_recoveries = h.recoveries_for(first_serial);
    ASSERT_EQ(first_recoveries.size(), 1u);
    auto parsed = parse_svg(read_file(h.file(first_recoveries.front())));
    ASSERT_TRUE(parsed);
    EXPECT_NE(find_by_id(parsed->getReprDoc(), first_native_id.c_str()), nullptr);
    EXPECT_EQ(attribute_of_id(*parsed, first_native_id.c_str(), "width"), first_width);
    EXPECT_NE(find_by_id(parsed->getReprDoc(), first_marker_id.c_str()), nullptr);

    // The first raw pointer is dereferenced only after it is proven live and
    // still registered as an app member.
    ASSERT_FALSE(first_destroyed);
    bool first_registered = false;
    for (SPDocument *d : h.app.get_documents()) {
        if (d == first) first_registered = true;
    }
    ASSERT_TRUE(first_registered);
    EXPECT_FALSE(first->isModifiedSinceAutoSave());
    EXPECT_TRUE(first->isModifiedSinceSave());
    EXPECT_EQ(xml_fingerprint(*first), first_fp);
    EXPECT_EQ(read_file(h.file("prior-foreign.svg")), foreign_bytes);
    EXPECT_EQ(read_file(h.file("prior-autosave.svg")), prior_bytes);
}

// 5. A raw live mutation after the snapshot leaves the document autosave-dirty,
//    so the first recovery still carries the snapshot width while the next tick
//    publishes the live width and clears.
TEST(AutoSaveRecoveryTest, MutationAfterSnapshotKeepsDirtyUntilNextRecovery)
{
    RecoveryHarness h;
    seed_prior_recoveries(h);
    std::string const foreign_bytes = read_file(h.file("prior-foreign.svg"));
    std::string const prior_bytes = read_file(h.file("prior-autosave.svg"));
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true);
    std::string const before_no_width = xml_fingerprint_without_native_width(*h.doc);
    std::string const before_meta = metadata_of(*h.doc);
    std::string const before_view = view_attributes(*h.doc);
    bool const before_virgin = h.doc->getVirgin();
    auto const before_rev = h.doc->getReprDoc()->contentRevision();
    ASSERT_TRUE(before_rev.has_value());

    bool fired = false;
    std::optional<std::uint64_t> during_rev;
    FaultCalls first;
    first.before_publish = [&] {
        auto *rect = find_by_id(h.doc->getReprRoot(), "native");
        if (!rect) throw std::runtime_error("fixture rect");
        rect->setAttribute("width", "37");
        fired = true;
        during_rev = h.doc->getReprDoc()->contentRevision();
    };
    AutoSave::getInstance().save(first);

    EXPECT_TRUE(fired);
    ASSERT_TRUE(during_rev.has_value());
    EXPECT_NE(during_rev, before_rev);
    EXPECT_EQ(attribute_of_id(*h.doc, "native", "width"), "37");
    EXPECT_TRUE(h.doc->isModifiedSinceSave());
    EXPECT_TRUE(h.doc->isModifiedSinceAutoSave());
    EXPECT_EQ(first.publish_calls, 1);
    auto const first_recoveries = h.recoveries_for(h.doc->serial());
    ASSERT_EQ(first_recoveries.size(), 1u);
    std::string const first_bytes = read_file(h.file(first_recoveries.front()));
    auto first_parsed = parse_svg(first_bytes);
    ASSERT_TRUE(first_parsed);
    EXPECT_EQ(attribute_of_id(*first_parsed, "native", "width"), "23");

    FaultCalls second;
    AutoSave::getInstance().save(second);
    EXPECT_EQ(second.publish_calls, 1);
    EXPECT_FALSE(h.doc->isModifiedSinceAutoSave());
    EXPECT_EQ(attribute_of_id(*h.doc, "native", "width"), "37");
    ASSERT_EQ(h.recoveries_for(h.doc->serial()).size(), 2u);
    EXPECT_EQ(read_file(h.file(first_recoveries.front())), first_bytes);
    ASSERT_EQ(second.attempted_finals.size(), 1u);
    auto second_parsed = parse_svg(read_file(second.attempted_finals.front()));
    ASSERT_TRUE(second_parsed);
    EXPECT_EQ(attribute_of_id(*second_parsed, "native", "width"), "37");
    EXPECT_EQ(xml_fingerprint_without_native_width(*h.doc), before_no_width);
    EXPECT_EQ(metadata_of(*h.doc), before_meta);
    EXPECT_EQ(view_attributes(*h.doc), before_view);
    EXPECT_EQ(h.doc->getVirgin(), before_virgin);
    EXPECT_EQ(read_file(h.file("prior-foreign.svg")), foreign_bytes);
    EXPECT_EQ(read_file(h.file("prior-autosave.svg")), prior_bytes);
}

// 6. Closing the current document in its own publication callback is deferred
//    while the operation lease is held; after the lease releases and a bounded
//    dispatch the destroy observer fires and the one published file parses.
TEST(AutoSaveRecoveryTest, ClosingCurrentDocumentWaitsForLease)
{
    RecoveryHarness h;
    deliberate_width_edit(*h.doc, "23");
    h.doc->setModifiedSinceSave(true);
    SPDocument *identity = h.doc;
    unsigned long const serial = h.doc->serial();

    bool alive_in_callback = false, pending_in_callback = false;
    FaultCalls calls;
    calls.before_publish = [&] {
        if (!h.doc) return;
        h.app.document_close(h.doc);
        alive_in_callback = (h.doc != nullptr);
        pending_in_callback = Inkscape::DocumentUndo::interactionCloseRequested(h.doc);
    };
    AutoSave::getInstance().save(calls);

    EXPECT_TRUE(alive_in_callback);
    EXPECT_TRUE(pending_in_callback);
    EXPECT_FALSE(h.destroyed);
    EXPECT_NE(h.doc, nullptr);
    dispatch_events();
    EXPECT_TRUE(h.destroyed);
    EXPECT_EQ(h.doc, nullptr);
    for (SPDocument *d : h.app.get_documents()) EXPECT_NE(d, identity);
    EXPECT_EQ(h.recoveries_for(serial).size(), 1u);
    ASSERT_EQ(calls.attempted_finals.size(), 1u);
    auto parsed = parse_svg(read_file(calls.attempted_finals.front()));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(attribute_of_id(*parsed, "native", "width"), "23");
}

// 7. A relative linked raster survives the private XML snapshot even though the
//    recovery directory differs from the live document base: the written
//    xlink:href must still resolve to the same actual PNG. The live authored
//    href, XML and the prior recovery bytes stay untouched.
TEST(AutoSaveRecoveryTest, RelativeImageHrefReopensFromDifferentRecoveryDirectory)
{
    RecoveryHarness h;
    guint32 const known = 0x2a78cfff; // R=0x2a G=0x78 B=0xcf A=0xff
    std::string const original_png = h.file("linked.png");
    write_file(original_png, solid_png_bytes(2, 2, known));
    std::string const png_bytes = read_file(original_png);
    ASSERT_FALSE(png_bytes.empty());

    // Native XML fixture: append svg:image beside the existing defs/use/clip.
    auto *image_repr = h.doc->getReprDoc()->createElement("svg:image");
    image_repr->setAttribute("id", "linked-image");
    image_repr->setAttribute("xlink:href", "linked.png");
    image_repr->setAttribute("x", "32");
    image_repr->setAttribute("y", "32");
    image_repr->setAttribute("width", "8");
    image_repr->setAttribute("height", "8");
    h.doc->getReprRoot()->appendChild(image_repr);
    GC::release(image_repr);
    h.doc->ensureUpToDate();
    DocumentUndo::done(h.doc, Util::Internal::ContextString("F5-A4 href fixture"), "");
    h.doc->setModifiedSinceSave(true);

    auto *live_image = dynamic_cast<SPImage *>(h.doc->getObjectById("linked-image"));
    ASSERT_TRUE(live_image);
    expect_loaded_rgba_image(*live_image, 2, 2, known);
    std::string const live_resolved = live_image->getURI().toNativeFilename();
    std::error_code live_ec;
    ASSERT_TRUE(std::filesystem::equivalent(std::filesystem::path(live_resolved),
                                            std::filesystem::path(original_png), live_ec));
    ASSERT_FALSE(live_ec);
    EXPECT_EQ(attribute_of_id(*h.doc, "linked-image", "xlink:href"), "linked.png");

    // New recovery parent, different from the original base. It exists only to
    // host the seeded prior foreign recovery; production creates the published
    // file inside it.
    std::string const recovery_dir = h.file("recoveries");
    ASSERT_TRUE(std::filesystem::create_directory(recovery_dir));
    std::string const prior_path = (std::filesystem::path(recovery_dir) / "prior-foreign.svg").string();
    write_file(prior_path, kPriorForeignSvg);
    std::string const prior_bytes = read_file(prior_path);
    ASSERT_FALSE(prior_bytes.empty());
    EXPECT_NE(std::filesystem::path(recovery_dir).lexically_normal(), h.dir.lexically_normal());
    Preferences::get()->setString("/options/autosave/path", recovery_dir);

    std::string const before_fp = xml_fingerprint(*h.doc);
    auto const before_rev = h.doc->getReprDoc()->contentRevision();
    std::string const before_meta = metadata_of(*h.doc);
    bool const before_virgin = h.doc->getVirgin();
    ASSERT_TRUE(before_rev.has_value());
    ASSERT_TRUE(h.doc->isModifiedSinceSave());
    ASSERT_TRUE(h.doc->isModifiedSinceAutoSave());

    FaultCalls calls;
    AutoSave::getInstance().save(calls);
    ASSERT_EQ(calls.publish_calls, 1);
    ASSERT_EQ(calls.last_status, DT::PublicationStatus::Published);
    ASSERT_FALSE(calls.last_final.empty());

    std::error_code parent_ec;
    EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(calls.last_final).parent_path(),
                                            std::filesystem::path(recovery_dir), parent_ec));
    EXPECT_FALSE(parent_ec);

    // Reopen the ACTUAL final with its filename so the base is the recovery dir.
    auto reopened = SPDocument::createNewDoc(calls.last_final.c_str());
    ASSERT_TRUE(reopened);
    auto *recovered_image = dynamic_cast<SPImage *>(reopened->getObjectById("linked-image"));
    ASSERT_TRUE(recovered_image);
    reopened->ensureUpToDate();
    expect_loaded_rgba_image(*recovered_image, 2, 2, known);

    // The written href is no longer the live string, yet the native URI loader
    // resolves it to the same real file (no hardcoded separators).
    std::string const recovered_href = attribute_of_id(*reopened, "linked-image", "xlink:href");
    EXPECT_FALSE(recovered_href.empty());
    EXPECT_NE(recovered_href, "linked.png");
    std::string const recovered_resolved = recovered_image->getURI().toNativeFilename();
    std::error_code recovered_ec;
    ASSERT_TRUE(std::filesystem::equivalent(std::filesystem::path(recovered_resolved),
                                            std::filesystem::path(original_png), recovered_ec));
    ASSERT_FALSE(recovered_ec);
    EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(recovered_resolved),
                                            std::filesystem::path(live_resolved), recovered_ec));

    // Internal fragment references and the native objects they name persist.
    EXPECT_EQ(attribute_of_id(*reopened, "reference", "xlink:href"), "#native");
    auto const *clip_path = find_by_id(reopened->getReprDoc(), "clip");
    ASSERT_NE(clip_path, nullptr);
    auto const *clip_use = find_node(clip_path, "svg:use");
    ASSERT_NE(clip_use, nullptr);
    char const *clip_href = clip_use->attribute("xlink:href");
    EXPECT_STREQ(clip_href ? clip_href : "", "#clip-src");
    EXPECT_NE(reopened->getObjectById("native"), nullptr);
    EXPECT_NE(reopened->getObjectById("clip-src"), nullptr);
    EXPECT_NE(reopened->getObjectById("reference"), nullptr);

    // Live state, authored href, prior recovery and PNG bytes are unchanged.
    EXPECT_EQ(attribute_of_id(*h.doc, "linked-image", "xlink:href"), "linked.png");
    EXPECT_EQ(xml_fingerprint(*h.doc), before_fp);
    EXPECT_EQ(h.doc->getReprDoc()->contentRevision(), before_rev);
    EXPECT_EQ(metadata_of(*h.doc), before_meta);
    EXPECT_EQ(h.doc->getVirgin(), before_virgin);
    EXPECT_TRUE(h.doc->isModifiedSinceSave());
    EXPECT_FALSE(h.doc->isModifiedSinceAutoSave());
    EXPECT_EQ(read_file(original_png), png_bytes);
    EXPECT_EQ(read_file(prior_path), prior_bytes);
    EXPECT_EQ(h.names_with_prefix("autosave-").size(), 0u); // nothing in old base
}
