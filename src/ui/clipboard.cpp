// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * System-wide clipboard management - implementation.
 *//*
 * Authors:
 * see git history
 *   Krzysztof Kosiński <tweenk@o2.pl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Incorporates some code from selection-chemistry.cpp, see that file for more credits.
 *   Abhishek Sharma
 *   Tavmjong Bah
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/explode-bitmap-publication.h"
#include "clipboard.h"

#include "clipboard-lease.h"
#include "clipboard-wait.h"

#include <boost/bimap.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>
#include <giomm/application.h>
#include <giomm/cancellable.h>
#include <gdkmm/contentformats.h>
#include <gdkmm/contentprovider.h>
#include <glib/gi18n.h>
#include <glibmm/bytes.h>
#include <glibmm/convert.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <sigc++/scoped_connection.h>
#include <2geom/path-sink.h>

#include "context-fns.h"
#include "desktop.h"
#include "display/cairo-utils.h"
#include "document-undo.h"
#include "document.h"
#include "layer-manager.h"
#include "libnrtype/font-factory.h"
#include "object/sp-image.h"
#include "object/sp-text.h"
#include "preferences.h"
#include "text-editing.h"
#include "ui/icon-names.h"
#include "ui/text-paste.h"
#include "ui/text-paste-external.h"
#include "ui/text-paste-font.h"
#include "ui/dialog/text-paste-dialog.h" // UI-owned prompt helper (required UI feature)
#include "xml/attribute-record.h"
#include "xml/repr.h"
#include "desktop-style.h"
#include "path/path-curve.h"
#include "extension/db.h" // extension database
#include "extension/input.h"
#include "extension/output.h"
#include "file.h" // for file_import, used in _pasteImage
#include "filter-chemistry.h"
#include "gradient-drag.h"
#include "helper/png-write.h"
#include "id-clash.h"
#include "live_effects/lpe-bspline.h"
#include "live_effects/lpe-spiro.h"
#include "live_effects/lpeobject-reference.h"
#include "live_effects/parameter/path.h"
#include "object/box3d.h"
#include "object/persp3d.h"
#include "object/sp-clippath.h"
#include "object/sp-defs.h"
#include "object/sp-gradient-reference.h"
#include "object/sp-hatch.h"
#include "object/sp-linear-gradient.h"
#include "object/sp-marker.h"
#include "object/sp-mask.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-page.h"
#include "object/sp-path.h"
#include "object/sp-pattern.h"
#include "object/sp-radial-gradient.h"
#include "object/sp-root.h"
#include "object/sp-symbol.h"
#include "object/sp-textpath.h"
#include "object/sp-use.h"
#include "selection-chemistry.h"
#include "selection.h"
#include "svg/svg.h" // for sp_svg_transform_write, used in _copySelection
#include "text-chemistry.h"
#include "ui/tool/control-point-selection.h"
#include "ui/tool/multi-path-manipulator.h"
#include "ui/tools/dropper-tool.h" // used in copy()
#include "ui/tools/node-tool.h"
#include "ui/tools/text-tool.h"
#include "util/value-utils.h"
#include "xml/sp-css-attr.h"

#ifdef _WIN32
#undef NOGDI
#include <windows.h>
#endif

using namespace Inkscape::Util;

namespace Inkscape::UI {
namespace {

constexpr bool DEBUG_CLIPBOARD = false;

bool clipboard_unavailable_for_testing = false;

Glib::RefPtr<Gdk::Clipboard> system_clipboard()
{
    if (clipboard_unavailable_for_testing) return {};
    auto display = Gdk::Display::get_default();
    return display ? display->get_clipboard() : Glib::RefPtr<Gdk::Clipboard>{};
}

/// Made up mimetype to represent Gdk::Pixbuf clipboard contents.
constexpr auto CLIPBOARD_GDK_PIXBUF_TARGET = "image/x-gdk-pixbuf";

constexpr auto CLIPBOARD_TEXT_TARGET = "text/plain";

/**
 * Heuristic for the plain-text fallback of an object copy: Inkscape publishes SVG
 * markup as text/plain as well. That markup must keep going to the SVG import
 * path instead of being typed as literal characters, unless the user is actively
 * editing a text object and therefore intentionally pasting characters.
 */
bool looks_like_svg_document(std::string const &text)
{
    std::size_t i = 0;
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) { // UTF-8 BOM
        i = 3;
    }
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
        ++i;
    }
    auto const starts_with = [&] (char const *prefix) {
        return text.compare(i, std::char_traits<char>::length(prefix), prefix) == 0;
    };
    return starts_with("<?xml") || starts_with("<svg") || starts_with("<!DOCTYPE svg");
}

/**
 * The interoperable plain-text MIME spellings, in preference order. Target
 * selection and the read request must use this one list: macOS/GTK serializes
 * native text only as "text/plain;charset=utf-8", while the local string
 * provider advertises both spellings. Two diverging checks here were the
 * confirmed cause of missed external Selector paste.
 */
std::vector<Glib::ustring> const &plain_text_mime_types()
{
    static std::vector<Glib::ustring> const types{"text/plain;charset=utf-8", "text/plain"};
    return types;
}

/** @return true when @a formats advertises any accepted plain-text spelling. */
bool has_plain_text(Glib::RefPtr<Gdk::ContentFormats> const &formats)
{
    if (!formats) {
        return false;
    }
    for (auto const &mime : plain_text_mime_types()) {
        if (formats->contain_mime_type(mime)) {
            return true;
        }
    }
    return false;
}

/** @return true when @a formats advertises an external rich-text representation. */
bool has_rich_text(Glib::RefPtr<Gdk::ContentFormats> const &formats)
{
    if (!formats) {
        return false;
    }
    for (auto const &mime : Inkscape::UI::TextPaste::html_mime_aliases()) {
        if (formats->contain_mime_type(mime)) {
            return true;
        }
    }
    for (auto const &mime : Inkscape::UI::TextPaste::rtf_mime_aliases()) {
        if (formats->contain_mime_type(mime)) {
            return true;
        }
    }
    return false;
}

/**
 * Wire-byte budget for the interoperable plain-text stream: the semantic
 * MAX_CHARS byte budget plus at most one terminal NUL appended by the platform
 * transport. GTK's macOS reader publishes `strlen(text) + 1` bytes for
 * "text/plain;charset=utf-8" (gdkmacospasteboard.c) and GTK's Windows reader
 * reports `u8_len + 1` for the transmuted CF_UNICODETEXT/CF_TEXT
 * (gdkclipdrop-win32.c), so this is platform-neutral, not an Apple special case.
 * from_plain_text() still enforces MAX_CHARS after that single terminator has
 * been removed: an over-limit payload is rejected as a whole, never truncated.
 */
constexpr std::size_t PLAIN_TEXT_TRANSPORT_LIMIT = Inkscape::UI::TextPaste::MAX_CHARS + 1u;
static_assert(PLAIN_TEXT_TRANSPORT_LIMIT > Inkscape::UI::TextPaste::MAX_CHARS,
              "the plain transport budget must admit one optional terminator");

/**
 * Total bytes one paste may retain across every candidate representation:
 * three 256 KiB payload budgets (native, HTML, RTF) plus the plain transport
 * budget. The plain budget is PLAIN_TEXT_TRANSPORT_LIMIT (MAX_CHARS bytes plus
 * the one optional terminal NUL), so the declared ceiling covers what the plain
 * read can actually retain instead of being one byte short. Each representation
 * is read at most once; this bounds the whole read even if a future candidate is
 * added without its own budget.
 */
constexpr std::size_t MAX_TOTAL_CANDIDATE_BYTES =
    3u * Inkscape::UI::TextPaste::MAX_PAYLOAD_BYTES + PLAIN_TEXT_TRANSPORT_LIMIT;


/** List of supported clipboard targets, in order of preference.
 *
 * Clipboard Formats: http://msdn.microsoft.com/en-us/library/ms649013(VS.85).aspx
 * On Windows, bitmap clipboard interoperability uses CF_DIB/CF_BITMAP.
 * GTK automatically presents an "image/bmp" target as CF_DIB/CF_BITMAP
 */
constexpr auto preferred_targets = std::to_array({
    "image/x-inkscape-svg",
    "image/svg+xml",
    "image/svg+xml-compressed",
    "application/pdf",
    "image/x-adobe-illustrator"
});

#ifdef __APPLE__

template <typename L, typename R>
boost::bimap<L, R> make_bimap(std::initializer_list<typename boost::bimap<L, R>::value_type> list)
{
    return boost::bimap<L, R>(list.begin(), list.end());
}

// MIME type to Universal Type Identifiers
auto const mime_uti = make_bimap<std::string, std::string>({
    {"image/x-inkscape-svg", "org.inkscape.svg"},
    {"image/svg+xml",        "public.svg-image"},
    {"image/png",            "public.png"},
    {"image/webp",           "public.webp"},
    {"image/tiff",           "public.tiff"},
    {"image/jpeg",           "public.jpeg"},
    {"image/x-e-postscript", "com.adobe.encapsulated-postscript"},
    {"image/x-postscript",   "com.adobe.postscript"},
 // {"text/plain",           "public.plain-text"}, - GIMP color palette
    {"text/html",            "public.html"},
    {"application/pdf",      "com.adobe.pdf"},
    {"application/tar",      "public.tar-archive"},
    {"application/x-zip",    "public.zip-archive"},
});

#endif

/// Type used to represent the Inkscape clipboard on the GTK clipboard.
struct ClipboardSvg {};

/*
 * Fixme: Get rid of all event pumpers.
 * This will require big changes to the ClipboardManager API and its users.
 *
 * Note that it may be a good idea to wait for coroutine support first,
 * otherwise the code in this file will first be refactored to a mess of callbacks,
 * and then refactored back again when coroutine support finally lands.
 *
 * Every clipboard wait in the paste flow is now bounded by an absolute deadline
 * (see wait_for_request/read_stream_bounded below); the old unbounded
 * iteration(true) pump was the confirmed cause of a hostile owner hanging the
 * GUI thread forever.
 */

/** Outcome of the bounded asynchronous clipboard reads below. */
enum class StreamReadStatus {
    complete,  ///< EOF reached, no more than @a limit bytes were read
    too_large, ///< more than @a limit bytes are available; nothing is returned
    failed,    ///< read error, no stream or unavailable MIME type
    timed_out, ///< producer stalled; the pending request or read was cancelled
};

/**
 * Outcome of one text-paste policy pass. This distinguishes "this pass found no
 * supported text, so the legacy object/SVG targets may still handle the
 * clipboard" from "the paste was aborted" - user cancellation, an invalidated
 * destination, or a limit/timeout rejection. An abort must stop the WHOLE
 * normal paste: no plaintext/object fallback, no second prompt, no second
 * clipboard read, no mutation and no Undo record.
 */
enum class TextPasteOutcome {
    not_handled, ///< no supported text for this pass; other targets may still apply
    pasted,      ///< text was inserted; commit responsibility stays with the caller
    aborted,     ///< cancel/revalidation/limit/timeout: stop, never fall back
};

/** Result of one bounded clipboard text read (see _readClipboardText). */
enum class ClipboardTextRead {
    no_text, ///< nothing usable was found; other paste targets may still apply
    read,    ///< a complete fragment was returned in the accompanying result
    aborted, ///< over-limit or stalled read; the whole paste must stop
};

/**
 * Explicit origin / representation / capability result of one clipboard text
 * read. The legacy `bool native` conflated where the payload came from with
 * whether it can carry rich formatting; every policy branch now uses the field
 * it actually means (object priority and the SVG-markup guard use origin, rich
 * insertion and the formatting question use representation + styles).
 */
struct ClipboardTextReadResult {
    Inkscape::UI::TextPaste::Origin origin = Inkscape::UI::TextPaste::Origin::External;
    Inkscape::UI::TextPaste::Representation representation = Inkscape::UI::TextPaste::Representation::Plain;
    Inkscape::UI::TextPaste::Fragment fragment;
    bool has_meaningful_styles = false;
    std::vector<std::string> losses; ///< bounded diagnostic reason codes (never user-facing)
};

/**
 * One user paste command's clipboard-text snapshot.
 *
 * A normal paste may run the text policy twice (the native shortcut first, then
 * the resolved text target). Both passes must observe the SAME clipboard
 * snapshot, obtained under ONE absolute deadline: a second read would buy a
 * second 5 s window, and a clipboard replaced between the passes would be pasted
 * even though the first pass was already committed to the earlier contents.
 * The attempt records the snapshot, its warning, the read count and the
 * clipboard generation at snapshot time; a generation change invalidates the
 * cache and aborts instead of reusing or re-reading.
 */
struct PasteAttemptText {
    bool active = false;
    gint64 deadline = 0;
    unsigned clipboard_generation = 0; ///< generation observed when the snapshot was taken
    unsigned read_count = 0;           ///< full _readClipboardText invocations for this command
    ClipboardTextRead status = ClipboardTextRead::no_text;
    ClipboardTextReadResult result;
    std::string warning;
    /// A formatting question answered during THIS command. The second policy pass
    /// must reuse the user's decision instead of re-deriving it from preferences
    /// (which would discard an explicit "Use destination" choice).
    bool decision_made = false;
    TextPasteMode decided_mode = TextPasteMode::Automatic;
    bool decided_remember = false;
    bool decided_native = false; ///< true when the decision belongs to the native preference family
};


/// One absolute deadline covers a whole clipboard paste: the initial native or
/// plain request, every stream chunk and the fallback attempt.
constexpr gint64 PASTE_DEADLINE_US = 5 * 1000 * 1000;
/// Grace period after cancelling a stalled clipboard producer before giving up.
constexpr gint64 CANCEL_GRACE_US = 100 * 1000;

/**
 * Advance the main loop by one bounded, non-blocking step.
 *
 * The deadline check and the finite dispatch budget live inside
 * ClipboardWait::pump_step(): a source that re-arms immediately (for example
 * SPDocument::idle_handler while document updates are pending) cannot hold this
 * helper past the absolute deadline, because each call dispatches at most
 * ClipboardWait::DEFAULT_DISPATCH_BUDGET events and re-checks the deadline
 * before every one of them. See src/ui/clipboard-wait.h for the contract and
 * the clipboard-free unit test that pins it.
 *
 * @return true when @a deadline expired.
 */
bool pump_wait_step(gint64 deadline)
{
    return Inkscape::UI::ClipboardWait::pump_step(*Glib::MainContext::get_default(), deadline);
}

/**
 * Read a Gio stream to EOF, or until more than @a limit bytes are available.
 *
 * A single read_bytes() is not a read-to-EOF: a stream may return a short chunk
 * while more data remains. Each chunk is therefore requested asynchronously and
 * the main loop is pumped while it is pending, so in-process and cross-instance
 * Gdk content providers that need the GUI loop to deliver data keep running and
 * the GUI thread never blocks on a producer. All chunks share the absolute
 * @a deadline supplied by the caller together with the initial clipboard
 * request, so the total wait is bounded no matter how many chunks arrive; on
 * expiry the producer is cancelled with an internally owned cancellable. The
 * read state is held by shared_ptr so a late completion after a timeout can
 * never touch a destroyed stack frame. On too_large the caller must reject the
 * payload as a whole: no partial data is returned and no truncation happens
 * here.
 */
StreamReadStatus read_stream_bounded(Glib::RefPtr<Gio::InputStream> const &stream, std::size_t limit,
                                     std::string &out, gint64 deadline)
{
    constexpr std::size_t CHUNK_SIZE = 16u * 1024u;

    out.clear();
    if (!stream) {
        return StreamReadStatus::failed;
    }

    struct ReadState {
        Glib::RefPtr<Gio::AsyncResult> result;
        bool done = false;
    };
    auto state = std::make_shared<ReadState>();
    auto const cancellable = Gio::Cancellable::create();

    for (;;) {
        // A producer can complete every chunk immediately (even synchronously
        // inside read_bytes_async), in which case the inner wait below never
        // runs; checking the absolute deadline here stops an endless series of
        // tiny chunks from evading it. No read is pending at this point, so
        // there is nothing to cancel or grace-wait for.
        if (g_get_monotonic_time() > deadline) {
            return StreamReadStatus::timed_out;
        }
        state->result.reset();
        state->done = false;
        std::size_t const want = std::min(CHUNK_SIZE, limit + 1 - out.size());
        stream->read_bytes_async(want, [state] (auto &res) {
            state->result = res;
            state->done = true;
        }, cancellable);
        while (!state->done) {
            if (pump_wait_step(deadline)) {
                cancellable->cancel();
                gint64 const grace_deadline = g_get_monotonic_time() + CANCEL_GRACE_US;
                while (!state->done && g_get_monotonic_time() < grace_deadline) {
                    pump_wait_step(grace_deadline);
                }
                return StreamReadStatus::timed_out;
            }
        }
        if (!state->result) {
            return StreamReadStatus::failed;
        }
        Glib::RefPtr<Glib::Bytes> bytes;
        try {
            bytes = stream->read_bytes_finish(state->result);
        } catch (Glib::Error const &) {
            return StreamReadStatus::failed;
        }
        if (!bytes || bytes->get_size() == 0) {
            return StreamReadStatus::complete; // EOF
        }
        if (out.size() + bytes->get_size() > limit) {
            return StreamReadStatus::too_large;
        }
        gsize size = 0;
        auto const *data = static_cast<char const *>(bytes->get_data(size));
        out.append(data, size);
    }
}

/** Completion state shared with one GdkClipboard::read_async callback. */
struct ClipboardRequest {
    Glib::RefPtr<Gio::AsyncResult> result;
    bool done = false;
};

/** Completion state of one bounded output-stream splice (temp file, export). */
struct ClipboardSplice {
    Glib::RefPtr<Gio::OutputStream> output;
    bool done = false;
    bool ok = false;
};

/**
 * Wait for one asynchronous clipboard operation to complete, or for @a deadline
 * to expire.
 *
 * The state is shared_ptr-owned and the operation carries a cancellable, so a
 * producer that never answers can never leave a callback referring to the
 * caller's stack frame. On expiry the operation is cancelled; the caller must
 * then abort without mutation (a timed-out representation cannot be retried
 * with a fresh deadline). The waiting loop itself lives in
 * ClipboardWait::wait_for_request_in() so the same deadline/budget contract can
 * be unit-tested on a private main context without a clipboard.
 * @return false when @a deadline expired before the operation completed.
 */
template <typename State>
bool wait_for_request(std::shared_ptr<State> const &state,
                      Glib::RefPtr<Gio::Cancellable> const &cancellable, gint64 deadline)
{
    return Inkscape::UI::ClipboardWait::wait_for_request_in(
        *Glib::MainContext::get_default(), state, cancellable, deadline, CANCEL_GRACE_US);
}

/**
 * Request one of @a mime_types from the clipboard and read its stream to EOF,
 * all under the remaining operation @a deadline.
 *
 * @param expected_mime when non-null, a stream of any other MIME type is
 *        rejected as failed so the caller can try its next representation
 *        (native fragment first, plain fallback) within the same deadline.
 */
StreamReadStatus read_clipboard_bounded(Glib::RefPtr<Gdk::Clipboard> const &clipboard,
                                        std::vector<Glib::ustring> const &mime_types,
                                        std::size_t limit, gint64 deadline, std::string &out,
                                        Glib::ustring const *expected_mime = nullptr)
{
    if (g_get_monotonic_time() > deadline) {
        return StreamReadStatus::timed_out;
    }
    auto request = std::make_shared<ClipboardRequest>();
    auto const cancellable = Gio::Cancellable::create();
    clipboard->read_async(mime_types, 0, [request] (auto &res) {
        request->result = res;
        request->done = true;
    }, cancellable);
    if (!wait_for_request(request, cancellable, deadline)) {
        return StreamReadStatus::timed_out;
    }
    if (!request->result) {
        return StreamReadStatus::failed;
    }
    Glib::ustring mime;
    auto stream = clipboard->read_finish(request->result, mime);
    if (!stream || (expected_mime && mime != *expected_mime)) {
        return StreamReadStatus::failed;
    }
    return read_stream_bounded(stream, limit, out, deadline);
}

// Fixme: Get rid of temporary files hack.
/** Get a temporary file name.
 * 
 * @arg suffix file suffix. May only contain ASCII characters.
 * 
 * @returns Filename with absolute path.
 * Value is in platform-native encoding (see Glib::filename_to_utf8).
 */
std::string get_tmp_filename(char const *suffix)
{
    return Glib::build_filename(Glib::get_user_cache_dir(), suffix);
}

// Explicit-document mechanics extracted from ordinary Copy/Symbols for reuse by
// detached staging. Library policy is kept out of the default Copy path.
// No constructor here acquires a clipboard or registers application callbacks.
class SelectionCopyContext {
public:
    SelectionCopyContext(XML::Document *doc, XML::Node *root, XML::Node *defs,
                         std::set<SPItem *> &clones,
                         std::function<void(XML::Node *)> collect = {},
                         std::function<bool(SPItem *)> enter = {})
        : _doc(doc), _root(root), _defs(defs), cloned_elements(clones),
          _collect(std::move(collect)), _enter(std::move(enter)) {}

    void _copyCompleteStyle(SPItem *, XML::Node *, bool child = false);
    void _copySingleStyle(SPObject *, XML::Node *, bool child);
    void _copyUsedDefs(SPItem *);
    void _copyGradient(SPGradient *);
    void _copyPattern(SPPattern *);
    void _copyHatch(SPHatch *);
    void _copyTextPath(SPTextPath *);
    XML::Node *_copyNode(XML::Node *, XML::Document *, XML::Node *);
    XML::Node *_copyIgnoreDup(XML::Node *, XML::Document *, XML::Node *);

private:
    XML::Document *_doc;
    XML::Node *_root;
    XML::Node *_defs;
    std::set<SPItem *> &cloned_elements;
    std::function<void(XML::Node *)> _collect;
    std::function<bool(SPItem *)> _enter;
};

/** Default implementation of the clipboard manager. */
class ClipboardManagerImpl : public ClipboardManager
{
public:
    bool ensureClipboard(SPDesktop *desktop) override;
    void copy(ObjectSet *set) override;
    void copyPathParameter(Inkscape::LivePathEffect::PathParam *) override;
    bool copyString(Glib::ustring str) override;
    void copySymbol(Inkscape::XML::Node* symbol, gchar const* style, SPDocument *source, const char* symbol_set, Geom::Rect const &bbox, bool set_clipboard) override;
    void insertSymbol(SPDesktop *desktop, Geom::Point const &shift_dt, bool read_clipboard) override;
    bool paste(SPDesktop *desktop, bool in_place, bool on_page) override;
    bool pasteText(SPDesktop *desktop, TextPasteMode mode) override;
    bool pasteStyle(ObjectSet *set) override;
    bool pasteSize(ObjectSet *set, bool separately, bool apply_x, bool apply_y) override;
    bool pastePathEffect(ObjectSet *set) override;
    Glib::ustring getPathParameter(SPDesktop* desktop) override;
    Glib::ustring getShapeOrTextObjectId(SPDesktop *desktop) override;
    std::vector<Glib::ustring> getElementsOfType(SPDesktop *desktop, gchar const* type = "*", gint maxdepth = -1) override;
    Glib::ustring getFirstObjectID() override;

    ClipboardManagerImpl();

private:
    void _cleanStyle(SPCSSAttr *);
    void _copySelection(ObjectSet *);
    void _copyCompleteStyle(SPItem *item, Inkscape::XML::Node *target, bool child = false);
    void _copyUsedDefs(SPItem *);
    bool _copyNodes(SPDesktop *desktop, ObjectSet *set);
    Inkscape::XML::Node *_copyNode(Inkscape::XML::Node *, Inkscape::XML::Document *, Inkscape::XML::Node *);

    bool _pasteImage(SPDesktop *desktop, gint64 deadline = 0);
    TextPasteOutcome _pasteText(SPDesktop *desktop, bool require_native = false);
    TextPasteOutcome _pasteTextPolicy(SPDesktop *desktop, TextPasteMode mode, bool prompt_allowed, bool require_native,
                                      bool *needs_commit);
    ClipboardTextRead _readClipboardText(ClipboardTextReadResult &result, std::string *warning, gint64 deadline);
    ClipboardTextRead _obtainClipboardText(PasteAttemptText &attempt, ClipboardTextReadResult &result,
                                           std::string *warning);
    bool _pasteTextObject(SPDesktop *desktop, Inkscape::UI::TextPaste::Fragment const &fragment, TextPasteMode mode);
    void _publishTextFragment(Inkscape::UI::TextPaste::Fragment const &fragment);
    bool _pasteNodes(SPDesktop *desktop, SPDocument *clipdoc, bool in_place, bool on_page);
    void _applyPathEffect(SPItem *, char const *);
    void _retrieveClipboard(Glib::ustring best_target = "", gint64 deadline = 0);

    // clipboard callbacks
    void _onGet(char const *mime_type, Glib::RefPtr<Gio::OutputStream> const &output);

    // various helpers
    void _createInternalClipboard();
    void _discardInternalClipboard();
    Inkscape::XML::Node *_createClipNode();
    Geom::Scale _getScale(SPDesktop *desktop, Geom::Point const &min, Geom::Point const &max, Geom::Rect const &obj_rect, bool apply_x, bool apply_y);
    Glib::ustring _getBestTarget(SPDesktop *desktop = nullptr);
    void _registerSerializers();
    void _setClipboardTargets();
    void _setClipboardColor(Colors::Color const &color);
    void _userWarn(SPDesktop *, char const *);

    // private properties
    /// True once a formatting question was shown during the current user paste
    /// attempt. One paste command may run the text pass twice (native shortcut,
    /// then the text target); the question must appear at most once, and an
    /// accept-then-failed-insertion must never re-prompt on the second pass.
    bool _prompt_shown_in_attempt = false;
    bool _paste_in_progress = false; ///< paste() is running (K7: not reentrant)

    /// The current user paste command's single clipboard-text snapshot (one
    /// read, one absolute deadline, reusable by the second policy pass). Reset
    /// at the start of every normal paste.
    PasteAttemptText _text_attempt;

    /// Monotonic clipboard generation, bumped by GdkClipboard::signal_changed.
    /// A paste attempt must never reuse or re-read a snapshot taken before a
    /// change: a clipboard replaced during the command is neither pasted nor
    /// re-read.
    unsigned _clipboard_generation = 0;
    sigc::scoped_connection _clipboard_changed_connection;
    std::unique_ptr<SPDocument> _clipboardSPDoc; ///< Document that stores the clipboard until someone requests it
    Inkscape::XML::Node *_defs; ///< Reference to the clipboard document's defs node
    Inkscape::XML::Node *_root; ///< Reference to the clipboard's root node
    Inkscape::XML::Node *_clipnode; ///< The node that holds extra information
    Inkscape::XML::Document *_doc; ///< Reference to the clipboard's Inkscape::XML::Document
    std::set<SPItem*> cloned_elements;
    std::vector<SPCSSAttr*> te_selected_style;
    std::vector<unsigned> te_selected_style_positions;

    // we need a way to copy plain text AND remember its style;
    // the standard _clipnode is only available in an SVG tree, hence this special storage
    SPCSSAttr *_text_style; ///< Style copied along with plain text fragment

    Glib::RefPtr<Gdk::Clipboard> _clipboard; ///< Handle to the system wide clipboard - for convenience

    // For throttling rogue clipboard managers.
    std::optional<std::chrono::steady_clock::time_point> last_req;
};

ClipboardManagerImpl::ClipboardManagerImpl()
    : _defs(nullptr),
      _root(nullptr),
      _clipnode(nullptr),
      _doc(nullptr),
      _text_style(nullptr),
      _clipboard(system_clipboard())
{
    // Clipboard requests on app termination can cause undesired extension
    // popup windows. Clearing the clipboard can prevent this.
    if (auto application = Gio::Application::get_default()) {
        application->signal_shutdown().connect([this] { _discardInternalClipboard(); });
    }

    _registerSerializers();

    // One process-wide clipboard generation counter: a paste attempt uses it to
    // detect that the clipboard changed since its snapshot.
    if (_clipboard) {
        _clipboard_changed_connection = _clipboard->signal_changed().connect([this] { ++_clipboard_generation; });
    }
}

bool ClipboardManagerImpl::ensureClipboard(SPDesktop *desktop)
{
    // Check the current display even if a clipboard was cached earlier. A
    // singleton first used headless may also acquire a display later.
    auto clipboard = system_clipboard();
    if (clipboard) {
        if (_clipboard != clipboard) {
            _clipboard = std::move(clipboard);
            _clipboard_changed_connection = _clipboard->signal_changed().connect([this] { ++_clipboard_generation; });
        }
        return true;
    }

    auto const message = _("Clipboard unavailable (no display)");
    _userWarn(desktop, message);
    g_printerr("[clipboard-unavailable] %s\n", message);
    return false;
}

/**
 * Copy selection contents to the clipboard.
 */
void ClipboardManagerImpl::copy(ObjectSet *set)
{
    if (!ensureClipboard(set ? set->desktop() : nullptr)) return;
    if (set && Bitmap::publicationBoundaryPending(set->document())) {
        if (set->desktop()) Bitmap::deferPublicationBoundary(*set->desktop(),[](SPDesktop &d) { d.getSelection()->copy(); });
        return; // Headless callers have no lifetime-safe desktop command to replay.
    }

    if (auto const desktop = set->desktop()) {
        // Special case for when the gradient dragger is active - copies gradient color
        if (auto const drag = desktop->getTool()->get_drag();
            drag && drag->hasSelection())
        {
            Color col = drag->getColor();

            // set the color as clipboard content (text in RRGGBBAA format)
            _setClipboardColor(col);

            // create a style with this color on fill and opacity in master opacity, so it can be
            // pasted on other stops or objects
            if (_text_style) {
                sp_repr_css_attr_unref(_text_style);
                _text_style = nullptr;
            }
            _text_style = sp_repr_css_attr_new();
            // print and set properties
            sp_repr_css_set_property_string(_text_style, "fill", col.toString(false));
            sp_repr_css_set_property_double(_text_style, "opacity", col.getOpacity());

            _discardInternalClipboard();
            return;
        }

        // Special case for when the color picker ("dropper") is active - copies color under cursor
        if (auto const dt = dynamic_cast<Tools::DropperTool const *>(desktop->getTool())) {
            _setClipboardColor(*dt->get_color(false, true));
            _discardInternalClipboard();
            return;
        }

        // Special case for when the text tool is active - if some text is selected, copy plain text,
        // not the object that holds it; also copy the style at cursor into
        if (auto const text_tool = dynamic_cast<Tools::TextTool*>(desktop->getTool())) {
            // Publish plain + native rich fragment atomically. The rich fragment
            // is derived from the current selection only; nothing is cached for
            // later pastes, so an external clipboard replacement cannot leak
            // stale formatting.
            auto fragment = text_tool->extractSelectionFragment();
            if (fragment && !fragment->empty()) {
                _discardInternalClipboard();
                _publishTextFragment(*fragment);
            } else {
                auto const plain = get_selected_text(*text_tool);
                if (!plain.empty()) {
                    _discardInternalClipboard();
                    _clipboard->set_text(plain);
                } else {
                    _userWarn(desktop, _("Nothing was copied."));
                }
            }
            if (_text_style) {
                sp_repr_css_attr_unref(_text_style);
                _text_style = nullptr;
            }
            _text_style = get_style_at_cursor(*text_tool);
            return;
        }

        // Special case for copying part of a path instead of the whole selected object.
        if (_copyNodes(desktop, set)) {
            return;
        }

        if (set->isEmpty()) {  // check whether something is selected
            _userWarn(desktop, "Nothing was copied.");
            return;
        }
    }

    _createInternalClipboard();   // construct a new clipboard document
    _copySelection(set);   // copy all items in the selection to the internal clipboard

    _setClipboardTargets();
}

/**
 * Copy a Live Path Effect path parameter to the clipboard.
 * @param pp The path parameter to store in the clipboard.
 */
void ClipboardManagerImpl::copyPathParameter(Inkscape::LivePathEffect::PathParam *pp)
{
    if (!ensureClipboard(nullptr)) return;
    if (!pp) {
        return;
    }
    SPItem * item = SP_ACTIVE_DESKTOP->getSelection()->singleItem();
    Geom::PathVector pv = pp->get_pathvector();
    if (item != nullptr) {
        pv *= item->i2doc_affine();
    }
    auto svgd = sp_svg_write_path(pv);

    if (svgd.empty()) {
        return;
    }

    _createInternalClipboard();

    Inkscape::XML::Node *pathnode = _doc->createElement("svg:path");
    pathnode->setAttribute("d", svgd);
    _root->appendChild(pathnode);
    Inkscape::GC::release(pathnode);

    fit_canvas_to_drawing(_clipboardSPDoc.get());
    _setClipboardTargets();
}

/**
 * @brief copies a string to the clipboard
 *
 * @param str string to copy
 */
bool ClipboardManagerImpl::copyString(Glib::ustring str) {
    if (!ensureClipboard(nullptr)) return false;
    if (!str.empty()) {
        _discardInternalClipboard();
        _clipboard->set_text(str);
        return true;
    }
    return false;
}

/**
 * Copy a symbol from the symbol dialog.
 *
 * @param symbol The Inkscape::XML::Node for the symbol.
 * @param style The style to be applied to the symbol.
 * @param source The source document of the symbol.
 * @param bbox The bounding box of the symbol, in desktop coordinates.
 */
void ClipboardManagerImpl::copySymbol(Inkscape::XML::Node* symbol, gchar const* style, SPDocument *source, const char* symbol_set,
                                      Geom::Rect const &bbox, bool set_clipboard)
{
    if (set_clipboard && !ensureClipboard(nullptr)) return;
    if (!symbol)
        return;

    _createInternalClipboard();

    // We add "_duplicate" to have a well defined symbol name that
    // bypasses the "prevent_id_classes" routine. We'll get rid of it
    // when we paste.
    auto original = cast<SPItem>(source->getObjectByRepr(symbol));
    _copyUsedDefs(original);
    Inkscape::XML::Node *repr = symbol->duplicate(_doc);
    Glib::ustring symbol_name;
    // disambiguate symbols from various symbol sets
    if (symbol_set && *symbol_set) {
        symbol_name = symbol_set;
        symbol_name += ":";
        symbol_name = sanitize_id(symbol_name);
    }
    symbol_name += repr->attribute("id");
    symbol_name += "_inkscape_duplicate";
    repr->setAttribute("id", symbol_name);
    _defs->appendChild(repr);
    auto nsymbol = cast<SPSymbol>(_clipboardSPDoc->getObjectById(symbol_name));
    if (nsymbol) {
        _copyCompleteStyle(original, repr, true);
        auto scale = _clipboardSPDoc->getDocumentScale();
        // Convert scale from source to clipboard user units
        nsymbol->scaleChildItemsRec(scale, Geom::Point(0, 0), false);
        if (!nsymbol->title()) {
            nsymbol->setTitle(nsymbol->label() ? nsymbol->label() : nsymbol->getId());
        }
        auto href = Glib::ustring("#") + symbol_name;
        size_t pos = href.find( "_inkscape_duplicate" );
        // while ffix rename id we do this hack
        href.erase( pos );
        Inkscape::XML::Node *use_repr = _doc->createElement("svg:use");
        use_repr->setAttribute("xlink:href", href);
   
        /**
        * If the symbol has a viewBox but no width or height, then take width and
        * height from the viewBox and set them on the use element. Otherwise, the
        * use element will have 100% document width and height!
        */
        {
            auto widthAttr = symbol->attribute("width");
            auto heightAttr = symbol->attribute("height");
            auto viewBoxAttr = symbol->attribute("viewBox");
            if (viewBoxAttr && !(heightAttr || widthAttr)) {
                SPViewBox vb;
                vb.set_viewBox(viewBoxAttr);
                if (vb.viewBox_set) {
                    use_repr->setAttributeSvgDouble("width", vb.viewBox.width());
                    use_repr->setAttributeSvgDouble("height", vb.viewBox.height());
                }
            }
        }
        // Set a default style in <use> rather than <symbol> so it can be changed.
        use_repr->setAttribute("style", style);
        _root->appendChild(use_repr);
        // because a extrange reason on append use getObjectsByElement("symbol") return 2 elements, 
        // it not give errrost by the moment;
        if (auto use = cast<SPUse>(_clipboardSPDoc->getObjectByRepr(use_repr))) {
            Geom::Affine affine = source->getDocumentScale();
            use->doWriteTransform(affine, &affine, false);
        }
        // Set min and max offsets based on the bounding rectangle.
        _clipnode->setAttributePoint("min", bbox.min());
        _clipnode->setAttributePoint("max", bbox.max());
        fit_canvas_to_drawing(_clipboardSPDoc.get());
    }
    if (set_clipboard) {
        _setClipboardTargets();
    }
}

/**
 * Insert a symbol into the document at the prescribed position (at the end of a drag).
 *
 * @param desktop The desktop onto which the symbol has been dropped.
 * @param shift_dt The vector by which the symbol position should be shifted, in desktop coordinates.
 */
void ClipboardManagerImpl::insertSymbol(SPDesktop *desktop, Geom::Point const &shift_dt, bool read_clipboard)
{
    if (read_clipboard && !ensureClipboard(desktop)) return;
    if (!desktop || !Inkscape::have_viable_layer(desktop, desktop->messageStack())) {
        return;
    }
    if (read_clipboard) {
        // The clipboard wait can dispatch window/document destruction.
        ClipboardLease::DestinationLease const lease(desktop, desktop->getDocument());
        _retrieveClipboard("text/plain;charset=utf-8");
        if (!lease.valid()) {
            return;
        }
    }
    auto &symbol = _clipboardSPDoc;
    if (!symbol) {
        return;
    }

    auto *root = symbol->getRoot();

    // Synthesize a clipboard position in order to paste the symbol where it got dropped.
    if (auto *clipnode = sp_repr_lookup_name(root->getRepr(), "inkscape:clipboard", 1)) {
        clipnode->setAttributePoint("min", clipnode->getAttributePoint("min") + shift_dt);
        clipnode->setAttributePoint("max", clipnode->getAttributePoint("max") + shift_dt);
    }

    sp_import_document(desktop, symbol.get(), true);
}

/**
 * Paste from the system clipboard into the active desktop.
 * @param in_place Whether to put the contents where they were when copied.
 */
bool ClipboardManagerImpl::paste(SPDesktop *desktop, bool in_place, bool on_page)
{
    if (!ensureClipboard(desktop)) return false;
    if (desktop && Bitmap::deferPublicationBoundary(*desktop,[in_place,on_page](SPDesktop &d) { sp_selection_paste(&d,in_place,on_page); })) return false;

    // paste() pumps the main context while a slow owner (delayed rendering)
    // answers, so a repeated or held Ctrl+V re-enters it. A nested paste would
    // reset _text_attempt under the outer one: refuse it, with no document change.
    ClipboardLease::ReentryGuard const reentry_guard(_paste_in_progress);
    if (!reentry_guard.entered()) {
        return false;
    }

    // do any checking whether we really are able to paste before requesting the contents
    if (!desktop) {
        return false;
    }
    if (!Inkscape::have_viable_layer(desktop, desktop->messageStack())) {
        return false;
    }

    // One user paste command may run the text pass twice (native shortcut, then
    // the resolved text target). The formatting question must appear at most
    // once per command, so the latch is reset here and set when a dialog opens.
    _prompt_shown_in_attempt = false;

    // One clipboard-text snapshot under one absolute deadline for the WHOLE
    // command. The second policy pass reuses this snapshot instead of reading
    // again (which would buy a second deadline and could paste a clipboard that
    // was replaced between the passes); a clipboard change invalidates it.
    _text_attempt = PasteAttemptText{};
    _text_attempt.active = true;
    _text_attempt.deadline = g_get_monotonic_time() + PASTE_DEADLINE_US;
    // The attempt (snapshot + prompted decision) is scoped to this one command:
    // an explicit paste action later must never reuse it.
    auto end_attempt = scope_exit([this] { _text_attempt.active = false; });

    // A native rich text fragment always wins over object targets, because copy()
    // only publishes that MIME for in-text selections (never for whole objects).
    // Plain-only clipboards keep the existing object/target priority below.
    //
    // Documented assumption (requirements-matrix ORIGIN-01): this shortcut keys
    // on the advertised MIME, not on `is_local()`. The MIME is application-
    // private, and gating it on locality would drop cross-instance native rich
    // paste (copy in one application instance, paste in another), which is an
    // existing feature. A foreign owner that publishes the private MIME still
    // has to pass the strict native parser and the style sanitizer below; if
    // locality gating is ever wanted, it belongs here and needs cross-instance
    // acceptance coverage.
    if (!on_page && _clipboard->get_formats()->contain_mime_type(Inkscape::UI::TextPaste::MIME_TYPE)) {
        switch (_pasteText(desktop, /*require_native=*/true)) {
        case TextPasteOutcome::pasted:
            return true;
        case TextPasteOutcome::aborted:
            // Cancel/revalidation/limit/timeout aborts the whole paste: return
            // without a fallback read, prompt, object/SVG paste or Undo record.
            return false;
        case TextPasteOutcome::not_handled:
            break; // malformed native -> plain conversion stays available below
        }
    }

    Glib::ustring target = _getBestTarget(desktop);
    if constexpr (DEBUG_CLIPBOARD) {
        std::cout << "paste(): Best target: " << target << std::endl;
    }

    // Special cases of clipboard content handling go here
    // Note that target priority is determined in _getBestTarget.
    // TODO: Handle x-special/gnome-copied-files and text/uri-list to support pasting files

    // if there is an image on the clipboard, paste it
    if (!on_page && target == CLIPBOARD_GDK_PIXBUF_TARGET) {
        return _pasteImage(desktop, _text_attempt.deadline);
    }
    if (!on_page && target == CLIPBOARD_TEXT_TARGET) {
        // It was text, and we did paste it. If not, continue on.
        switch (_pasteText(desktop)) {
        case TextPasteOutcome::pasted:
            return true;
        case TextPasteOutcome::aborted:
            // The text paste was aborted (cancel/revalidation/limit/timeout):
            // the whole paste stops here instead of retrying the new clipboard
            // or falling back to the SVG/object import path.
            return false;
        case TextPasteOutcome::not_handled:
            break; // no supported text here; SVG-document detection may still apply
        }
        // If the clipboard contains text/plain, but is an svg document
        // then we'll try and detect it and then paste it if possible.
    }

    // The object/SVG path shares the command's remaining absolute budget: an
    // ordinary paste may not total more than one five-second clipboard wait.
    //
    // Lifetime lease (r3 product review P1-3): _retrieveClipboard() pumps the
    // main context, so a queued window close can destroy the desktop and free
    // the document while we wait, and the destination can switch documents.
    // Both destroy signals are connected as scoped leases (sigc::connection does
    // NOT disconnect on destruction, and a raw pointer would dangle), and every
    // use below is revalidated against the captured pair.
    // A window switch does not redirect this path: it resolves every destination
    // through the captured desktop itself (_pasteNodes, sp_import_document,
    // desktop->getSelection()); the image path is the one that also needs the
    // active-window check (file_import() reads SP_ACTIVE_DESKTOP).
    ClipboardLease::DestinationLease const lease(desktop, desktop->getDocument());

    _retrieveClipboard(target, _text_attempt.deadline);
    auto &tempdoc = _clipboardSPDoc;

    // Revalidate before any message, import, selection change or Undo: the wait
    // above pumped the main context. A dead or switched destination aborts
    // mutation-free (the caller then records no Undo step).
    if (!lease.valid()) {
        return false;
    }

    if (!tempdoc) {
        if (target == CLIPBOARD_TEXT_TARGET) {
            _userWarn(desktop, _("Can't paste text outside of the text tool."));
            return false;
        } else {
            _userWarn(desktop, _("Nothing on the clipboard."));
            return false;
        }
    }

    if (_pasteNodes(desktop, tempdoc.get(), in_place, on_page)) {
        return true;
    }

    // copy definitions
    if (!lease.valid()) {
        return false;
    }
    sp_import_document(desktop, tempdoc.get(), in_place, on_page);

    // _copySelection() has put all items in groups, now ungroup them (preserves transform
    // relationships of clones, text-on-path, etc.)
    if (target == "image/x-inkscape-svg") {
        if (!lease.valid()) {
            return false; // no ungrouping, selection update or LPE refresh on a dead destination
        }
        SPDocument *doc = nullptr;
        desktop->getSelection()->ungroup(true);
        auto vec2 = desktop->getSelection()->items_vector();
        for (auto item : vec2) {
            // just a bit beauty on paste hidden items unselect
            doc = item->document;
            if (vec2.size() > 1 && item->isHidden()) {
                desktop->getSelection()->remove(item);
            }
            if (auto pasted_lpe_item = cast<SPLPEItem>(item)) {
                remove_hidder_filter(pasted_lpe_item);
            }
        }
        if (doc) {
            doc->update_lpobjs();
        }
    }

    return true;
}

/**
 * Copy any selected nodes and return true if there were nodes.
 */
bool ClipboardManagerImpl::_copyNodes(SPDesktop *desktop, ObjectSet *set)
{
    auto const node_tool = dynamic_cast<Inkscape::UI::Tools::NodeTool *>(desktop->getTool());
    if (!node_tool || !node_tool->_selected_nodes)
        return false;

    SPPath *first_path = nullptr;
    for (auto obj : set->items()) {
        if ((first_path = cast<SPPath>(obj))) {
            break;
        }
    }

    auto builder = new Geom::PathBuilder();
    node_tool->_multipath->copySelectedPath(builder);
    Geom::PathVector pathv = builder->peek();

    // _createInternalClipboard done after copy, as deleting clipboard
    // document may trigger tool switch (as in PathParam::~PathParam)
    _createInternalClipboard();

    // Copy document height so that desktopVisualBounds() is equivalent in the
    // source document and the clipboard.
    _clipboardSPDoc->setWidthAndHeight(desktop->doc()->getWidth(), desktop->doc()->getHeight());

    // Were any nodes actually copied?
    if (pathv.empty() || !first_path)
        return false;

    Inkscape::XML::Node *pathRepr = _doc->createElement("svg:path");

    // pathv is in desktop coordinates
    auto source_scale = first_path->i2dt_affine();
    pathRepr->setAttribute("d", sp_svg_write_path(pathv * source_scale.inverse()));
    pathRepr->setAttributeOrRemoveIfEmpty("transform", first_path->getAttribute("transform"));

    // Group the path to make it consistant with other copy processes
    auto group = _doc->createElement("svg:g");
    _root->appendChild(group);
    Inkscape::GC::release(group);

    // Store the style for paste-as-object operations. Ignored if pasting into an other path.
    pathRepr->setAttribute("style", first_path->style->write(SP_STYLE_FLAG_IFSET) );
    group->appendChild(pathRepr);
    Inkscape::GC::release(pathRepr);

    // Store the parent transformation, and scaling factor of the copied object
    if (auto parent = cast<SPItem>(first_path->parent)) {
          auto transform_str = sp_svg_transform_write(parent->i2doc_affine());
          group->setAttributeOrRemoveIfEmpty("transform", transform_str);
    }

    // Set the translation for paste-in-place operation, must be done after repr appends
    if (auto path_obj = cast<SPPath>(_clipboardSPDoc->getObjectByRepr(pathRepr))) {
        // we could use pathv.boundsFast here, but that box doesn't include stroke width
        // so we must take the value from the visualBox of the new shape instead.
        assert(Geom::are_near(path_obj->document->getDimensions(), first_path->document->getDimensions()));
        auto bbox = *(path_obj->desktopVisualBounds());
        _clipnode->setAttributePoint("min", bbox.min());
        _clipnode->setAttributePoint("max", bbox.max());
    }
    _setClipboardTargets();
    return true;
}

/**
 * Paste nodes into a selected path and return true if it's possible.
 *   if the node tool selected
 *   and one path selected in target
 *   and one path in source
 */
bool ClipboardManagerImpl::_pasteNodes(SPDesktop *desktop, SPDocument *clipdoc, bool in_place, bool on_page)
{
    auto const node_tool = dynamic_cast<Inkscape::UI::Tools::NodeTool *>(desktop->getTool());
    if (!node_tool || desktop->getSelection()->objects().size() != 1)
        return false;

    SPObject *obj = desktop->getSelection()->objects().back();
    auto target_path = cast<SPPath>(obj);
    if (!target_path)
        return false;

    auto const dt_to_target = target_path->dt2i_affine();
    // Select all nodes prior to pasting in, for later inversion.
    node_tool->_selected_nodes->selectAll();

    for (auto node = clipdoc->getReprRoot()->firstChild(); node; node = node->next()) {
        auto source_obj = clipdoc->getObjectByRepr(node);

        // Unpack group that may have a transformation inside it.
        if (auto source_group = cast<SPGroup>(source_obj)) {
            if (source_group->children.size() == 1) {
                source_obj = source_group->firstChild();
            }
        }

        if (auto source_path = cast<SPPath>(source_obj)) {
            auto source_to_target = source_path->i2dt_affine();
            auto source_curve = *source_path->curveForEdit();
            auto target_curve = *target_path->curveForEdit();

            auto bbox = *(source_path->desktopVisualBounds());
            if (!in_place) {
                // Move the source curve to the mouse pointer (desktop coordinates)
                source_to_target *= Geom::Translate((desktop->point() - bbox.midpoint()).round());
            } else if (auto clipnode = sp_repr_lookup_name(clipdoc->getReprRoot(), "inkscape:clipboard", 1)) {
                // Force translation so a foreign path will end up in the right place.
                source_to_target *= Geom::Translate(clipnode->getAttributePoint("min") - bbox.min());
            }

            source_to_target *= dt_to_target;

            // Finally convert the curve into path item's coordinate system
            source_curve *= source_to_target;

            // Add the source curve to the target copy
            pathvector_append(target_curve, std::move(source_curve));

            // Set the attribute to keep the document up to date (fixes undo)
            auto str = sp_svg_write_path(target_curve);
            target_path->setAttribute("d", str);

            if (on_page) {
                g_warning("Node paste on page not Implemented");
            }
        }
    }

    // Finally we invert the selection, this selects all newly added nodes.
    node_tool->_selected_nodes->invertSelection();

    return true;
}

/**
 * Returns the id of the first visible copied object.
 */
Glib::ustring ClipboardManagerImpl::getFirstObjectID()
{
    if (!ensureClipboard(nullptr)) return {};
    _retrieveClipboard("image/x-inkscape-svg");
    auto tempdoc = _clipboardSPDoc.get();
    if (!tempdoc) {
        return {};
    }

    Inkscape::XML::Node *root = tempdoc->getReprRoot();

    if (!root) {
        return {};
    }

    Inkscape::XML::Node *ch = root->firstChild();
    Inkscape::XML::Node *child = nullptr;
    // now clipboard is wrapped on copy since 202d57ea fix
    while (ch != nullptr &&
           g_strcmp0(ch->name(), "svg:g") &&
           g_strcmp0(child?child->name():nullptr, "svg:g") &&
           g_strcmp0(child?child->name():nullptr, "svg:path") &&
           g_strcmp0(child?child->name():nullptr, "svg:use") &&
           g_strcmp0(child?child->name():nullptr, "svg:text") &&
           g_strcmp0(child?child->name():nullptr, "svg:image") &&
           g_strcmp0(child?child->name():nullptr, "svg:rect") &&
           g_strcmp0(child?child->name():nullptr, "svg:ellipse") &&
           g_strcmp0(child?child->name():nullptr, "svg:circle")
        ) {
        ch = ch->next();
        child = ch ? ch->firstChild(): nullptr;
    }

    if (child) {
        char const *id = child->attribute("id");
        if (id) {
            return id;
        }
    }

    return {};
}

/**
 * Remove certain css elements which are not useful for pasteStyle
 */
void ClipboardManagerImpl::_cleanStyle(SPCSSAttr *style)
{
    if (style) {
        /* Clean text 'position' properties */
        sp_repr_css_unset_property(style, "text-anchor");
        sp_repr_css_unset_property(style, "shape-inside");
        sp_repr_css_unset_property(style, "shape-subtract");
        sp_repr_css_unset_property(style, "shape-padding");
        sp_repr_css_unset_property(style, "shape-margin");
        sp_repr_css_unset_property(style, "inline-size");
    }
}

/**
 * Implements the Paste Style action.
 */
bool ClipboardManagerImpl::pasteStyle(ObjectSet *set)
{
    if (!ensureClipboard(set ? set->desktop() : nullptr)) return false;
    if (set && set->desktop() && Bitmap::deferPublicationBoundary(*set->desktop(),[](SPDesktop &d) { d.getSelection()->pasteStyle(); })) return false;

    auto dt = set->desktop();
    if (!dt) {
        return false;
    }

    // check whether something is selected
    if (set->isEmpty()) {
        _userWarn(set->desktop(), _("Select <b>object(s)</b> to paste style to."));
        return false;
    }

    // The clipboard wait can dispatch window/document destruction.
    ClipboardLease::DestinationLease const lease(dt, set->document());
    _retrieveClipboard("image/x-inkscape-svg");
    if (!lease.valid()) {
        return false;
    }
    auto &tempdoc = _clipboardSPDoc;
    if (!tempdoc) {
        // no document, but we can try _text_style
        if (_text_style) {
            _cleanStyle(_text_style);
            sp_desktop_set_style(set, set->desktop(), _text_style);
            return true;
        } else {
            _userWarn(set->desktop(), _("No style on the clipboard."));
            return false;
        }
    }

    auto prefs = Inkscape::Preferences::get();
    auto const copy_computed = prefs->getBool("/options/copycomputedstyle/value", true);

    Inkscape::XML::Node *root = tempdoc->getReprRoot();
    Inkscape::XML::Node *clipnode = sp_repr_lookup_name(root, "inkscape:clipboard", 1);

    if (!clipnode) {
        _userWarn(set->desktop(), _("No style on the clipboard."));
        return false;
    }

    bool pasted = false;

    if (copy_computed) {
        SPCSSAttr *style = sp_repr_css_attr(clipnode, "style");
        sp_desktop_set_style(set, set->desktop(), style);
        pasted = true;
    } else {
        for (auto node : set->xmlNodes()) {
            pasted = node->copyAttribute("class", clipnode, true) || pasted;
            pasted = node->copyAttribute("style", clipnode, true) || pasted;
        }
    }

    if (pasted) {
        // pasted style might depend on defs from the source
        set->document()->importDefs(tempdoc.get());
    }

    return pasted;
}

/**
 * Resize the selection or each object in the selection to match the clipboard's size.
 * @param separately Whether to scale each object in the selection separately
 * @param apply_x Whether to scale the width of objects / selection
 * @param apply_y Whether to scale the height of objects / selection
 */
bool ClipboardManagerImpl::pasteSize(ObjectSet *set, bool separately, bool apply_x, bool apply_y)
{
    if (!ensureClipboard(set ? set->desktop() : nullptr)) return false;

    if (set && Bitmap::publicationBoundaryPending(set->document())) {
        if (set->desktop()) Bitmap::deferPublicationBoundary(*set->desktop(),[this,separately,apply_x,apply_y](SPDesktop &d) {
            if (pasteSize(d.getSelection(),separately,apply_x,apply_y)) DocumentUndo::done(d.getDocument(),RC_("Undo","Paste size"),"");
        });
        return false; // A borrowed headless ObjectSet cannot be safely replayed.
    }

    if (!apply_x && !apply_y) {
        return false; // pointless parameters
    }

    if (set->isEmpty()) {
        if(set->desktop())
            _userWarn(set->desktop(), _("Select <b>object(s)</b> to paste size to."));
        return false;
    }

    // FIXME: actually, this should accept arbitrary documents
    // The clipboard wait can dispatch window/document destruction.
    ClipboardLease::DestinationLease const lease(set->desktop(), set->document());
    _retrieveClipboard("image/x-inkscape-svg");
    if (!lease.valid()) {
        return false;
    }
    auto tempdoc = _clipboardSPDoc.get();
    if (!tempdoc) {
        if (set->desktop()) {
            _userWarn(set->desktop(), _("No size on the clipboard."));
        }
        return false;
    }

    // retrieve size information from the clipboard
    Inkscape::XML::Node *root = tempdoc->getReprRoot();
    Inkscape::XML::Node *clipnode = sp_repr_lookup_name(root, "inkscape:clipboard", 1);
    if (!clipnode) return false;

    Geom::Point min, max;
    bool visual_bbox = !Inkscape::Preferences::get()->getInt("/tools/bounding_box");
    min = clipnode->getAttributePoint((visual_bbox ? "min" : "geom-min"), min);
    max = clipnode->getAttributePoint((visual_bbox ? "max" : "geom-max"), max);

    if (separately) {
        // resize each object in the selection
        auto itemlist = set->items();
        for (auto item : itemlist) {
            if (item) {
                Geom::OptRect obj_size = item->desktopPreferredBounds();
                if ( obj_size ) {
                    item->scale_rel(_getScale(set->desktop(), min, max, *obj_size, apply_x, apply_y));
                }
            } else {
                g_assert_not_reached();
            }
        }
    } else {
        // resize the selection as a whole
        Geom::OptRect sel_size = set->preferredBounds();
        if (sel_size) {
            set->scaleRelative(sel_size->midpoint(),
                                         _getScale(set->desktop(), min, max, *sel_size, apply_x, apply_y));
        }
    }

    return true;
}

/**
 * Applies a path effect from the clipboard to the selected path.
 */
bool ClipboardManagerImpl::pastePathEffect(ObjectSet *set)
{
    if (!ensureClipboard(set ? set->desktop() : nullptr)) return false;
    if (set && set->desktop() && Bitmap::deferPublicationBoundary(*set->desktop(),[](SPDesktop &d) { d.getSelection()->pastePathEffect(); })) return false;

    /** @todo FIXME: pastePathEffect crashes when moving the path with the applied effect,
        segfaulting in fork_private_if_necessary(). */

    if (!set->desktop()) {
        return false;
    }

    if (!set || set->isEmpty()) {
        _userWarn(set->desktop(), _("Select <b>object(s)</b> to paste live path effect to."));
        return false;
    }

    // The clipboard wait can dispatch window/document destruction.
    ClipboardLease::DestinationLease const lease(set->desktop(), set->document());
    _retrieveClipboard("image/x-inkscape-svg");
    if (!lease.valid()) {
        return false;
    }
    auto &tempdoc = _clipboardSPDoc;
    if (tempdoc) {
        Inkscape::XML::Node *root = tempdoc->getReprRoot();
        Inkscape::XML::Node *clipnode = sp_repr_lookup_name(root, "inkscape:clipboard", 1);
        if ( clipnode ) {
            char const *effectstack = clipnode->attribute("inkscape:path-effect");
            if ( effectstack ) {
                set->document()->importDefs(tempdoc.get());

                // importing defs can adjust node names, so grab the current names again
                effectstack = clipnode->attribute("inkscape:path-effect");

                // make sure all selected items are converted to paths first (i.e. rectangles)
                set->toLPEItems();
                auto itemlist= set->items();
                for(auto item : itemlist){
                    _applyPathEffect(item, effectstack);
                    item->doWriteTransform(item->transform);
                }

                return true;
            }
        }
    }

    // no_effect:
    _userWarn(set->desktop(), _("No effect on the clipboard."));
    return false;
}

/**
 * Get LPE path data from the clipboard.
 * @return The retrieved path data (contents of the d attribute), or "" if no path was found
 */
Glib::ustring ClipboardManagerImpl::getPathParameter(SPDesktop* desktop)
{
    if (!ensureClipboard(desktop)) return {};
    // The clipboard wait can dispatch window/document destruction.
    ClipboardLease::DestinationLease const lease(desktop, desktop ? desktop->getDocument() : nullptr);
    _retrieveClipboard(); // any target will do here
    if (!lease.valid()) {
        return "";
    }
    auto doc = _clipboardSPDoc.get();
    if (!doc) {
        _userWarn(desktop, _("Nothing on the clipboard."));
        return "";
    }

    // unlimited search depth
    auto repr = sp_repr_lookup_name(doc->getReprRoot(), "svg:path", -1);
    auto item = cast<SPItem>(doc->getObjectByRepr(repr));

    if (!item) {
        _userWarn(desktop, _("Clipboard does not contain a path."));
        return "";
    }

    // Adjust any copied path into the target document transform.
    auto tr_p = item->i2doc_affine();
    auto tr_s = doc->getDocumentScale().inverse();
    auto pathv = sp_svg_read_pathv(repr->attribute("d"));
    return sp_svg_write_path(pathv * tr_s * tr_p);
}

/**
 * Get object id of a shape or text item from the clipboard.
 * @return The retrieved id string (contents of the id attribute), or "" if no shape or text item was found.
 */
Glib::ustring ClipboardManagerImpl::getShapeOrTextObjectId(SPDesktop *desktop)
{
    if (!ensureClipboard(desktop)) return {};
    // https://bugs.launchpad.net/inkscape/+bug/1293979
    // basically, when we do a depth-first search, we're stopping
    // at the first object to be <svg:path> or <svg:text>.
    // but that could then return the id of the object's
    // clip path or mask, not the original path!

    // The clipboard wait can dispatch window/document destruction.
    ClipboardLease::DestinationLease const lease(desktop, desktop ? desktop->getDocument() : nullptr);
    _retrieveClipboard(); // any target will do here
    if (!lease.valid()) {
        return "";
    }
    auto tempdoc = _clipboardSPDoc.get();
    if (!tempdoc) {
        _userWarn(desktop, _("Nothing on the clipboard."));
        return "";
    }
    Inkscape::XML::Node *root = tempdoc->getReprRoot();

    // 1293979: strip out the defs of the document
    root->removeChild(tempdoc->getDefs()->getRepr());

    Inkscape::XML::Node *repr = sp_repr_lookup_name(root, "svg:path", -1); // unlimited search depth
    if (!repr) {
        repr = sp_repr_lookup_name(root, "svg:text", -1);
    }
    if (!repr) {
        repr = sp_repr_lookup_name(root, "svg:ellipse", -1);
    }
    if (!repr) {
        repr = sp_repr_lookup_name(root, "svg:rect", -1);
    }
    if (!repr) {
        repr = sp_repr_lookup_name(root, "svg:circle", -1);
    }

    if (!repr) {
        _userWarn(desktop, _("Clipboard does not contain a path."));
        return "";
    }

    auto svgd = repr->attribute("id");
    return svgd ? svgd : "";
}

/**
 * Get all objects id  from the clipboard.
 * @return A vector containing all IDs or empty if no shape or text item was found.
 * type. Set to "*" to retrieve all elements of the types vector inside, feel free to populate more
 */
std::vector<Glib::ustring> ClipboardManagerImpl::getElementsOfType(SPDesktop *desktop, gchar const* type, gint maxdepth)
{
    if (!ensureClipboard(desktop)) return {};
    // The clipboard wait can dispatch window/document destruction.
    ClipboardLease::DestinationLease const lease(desktop, desktop ? desktop->getDocument() : nullptr);
    _retrieveClipboard(); // any target will do here
    if (!lease.valid()) {
        return {};
    }
    auto tempdoc = _clipboardSPDoc.get();
    if (!tempdoc) {
        _userWarn(desktop, _("Nothing on the clipboard."));
        return {};
    }
    Inkscape::XML::Node *root = tempdoc->getReprRoot();

    // 1293979: strip out the defs of the document
    if (auto repr = tempdoc->getDefs()->getRepr()) {
        root->removeChild(repr);
    }
    std::vector<Inkscape::XML::Node const *> reprs;
    if (strcmp(type, "*") == 0){
        //TODO:Fill vector with all possible elements
        std::vector<Glib::ustring> types;
        types.push_back((Glib::ustring)"svg:path");
        types.push_back((Glib::ustring)"svg:circle");
        types.push_back((Glib::ustring)"svg:rect");
        types.push_back((Glib::ustring)"svg:ellipse");
        types.push_back((Glib::ustring)"svg:text");
        types.push_back((Glib::ustring)"svg:use");
        types.push_back((Glib::ustring)"svg:g");
        types.push_back((Glib::ustring)"svg:image");
        for (auto type_elem : types) {
            std::vector<Inkscape::XML::Node const *> reprs_found = sp_repr_lookup_name_many(root, type_elem.c_str(), maxdepth); // unlimited search depth
            reprs.insert(reprs.end(), reprs_found.begin(), reprs_found.end());
        }
    } else {
        reprs = sp_repr_lookup_name_many(root, type, maxdepth);
    }

    std::vector<Glib::ustring> result;
    for (auto node : reprs) {
        result.emplace_back(node->attribute("id"));
    }

    if (result.empty()) {
        _userWarn(desktop, (Glib::ustring::compose(_("Clipboard does not contain any objects of type \"%1\"."), type)).c_str());
        return {};
    }

    return result;
}

/**
 * Iterate over a list of items and copy them to the clipboard.
 */
void ClipboardManagerImpl::_copySelection(ObjectSet *selection)
{
    auto prefs = Preferences::get();
    auto const copy_computed = prefs->getBool("/options/copycomputedstyle/value", true);
    SPPage *page = nullptr;

    // copy the defs used by all items
    auto itemlist = selection->items();
    cloned_elements.clear();
    std::vector<SPItem *> items(itemlist.begin(), itemlist.end());
    for (auto item : itemlist) {
        if (!page) {
            page = item->document->getPageManager().getPageFor(item, false);
        }
        auto lpeitem = cast<SPLPEItem>(item);
        if (lpeitem) {
            for (auto satellite : lpeitem->get_satellites(false, true)) {
                if (satellite) {
                    auto item2 = cast<SPItem>(satellite);
                    if (item2 && std::find(items.begin(), items.end(), item2) == items.end()) {
                        items.push_back(item2);
                    }
                }
            }
        }
    }
    cloned_elements.clear();
    for (auto item : items) {
        if (item) {
            _copyUsedDefs(item);
        } else {
            g_assert_not_reached();
        }
    }

    // copy the representation of the items
    std::vector<SPObject *> sorted_items(items.begin(), items.end());
    {
        // Get external text references and add them to sorted_items
        auto ext_refs = text_categorize_refs(selection->document(),
                sorted_items.begin(), sorted_items.end(),
                TEXT_REF_EXTERNAL);
        for (auto const &ext_ref : ext_refs) {
            sorted_items.push_back(selection->document()->getObjectById(ext_ref.first));
        }
    }
    sort(sorted_items.begin(), sorted_items.end(), sp_object_compare_position_bool);

    //remove already copied elements from cloned_elements
    std::vector<SPItem*>tr;
    for(auto cloned_element : cloned_elements){
        if(std::find(sorted_items.begin(),sorted_items.end(),cloned_element)!=sorted_items.end())
            tr.push_back(cloned_element);
    }
    for(auto & it : tr){
        cloned_elements.erase(it);
    }

    // One group per shared parent
    std::map<SPObject const *, Inkscape::XML::Node *> groups;

    sorted_items.insert(sorted_items.end(),cloned_elements.begin(),cloned_elements.end());
    for(auto sorted_item : sorted_items){
        auto item = cast<SPItem>(sorted_item);
        if (item) {
            // Create a group with the parent transform. This group will be ungrouped when pasting
            // und takes care of transform relationships of clones, text-on-path, etc.
            auto &group = groups[item->parent];
            if (!group) {
                group = _doc->createElement("svg:g");
                group->setAttribute("id", item->parent->getId()); // avoid getting a clashing id
                _root->appendChild(group);
                Inkscape::GC::release(group);

                if (auto parent = cast<SPItem>(item->parent)) {
                    auto transform_str = sp_svg_transform_write(parent->i2doc_affine());
                    group->setAttributeOrRemoveIfEmpty("transform", transform_str);
                }
            }

            Inkscape::XML::Node *obj = item->getRepr();
            Inkscape::XML::Node *obj_copy;
            if(cloned_elements.find(item)==cloned_elements.end())
                obj_copy = _copyNode(obj, _doc, group);
            else
                obj_copy = _copyNode(obj, _doc, _clipnode);

            if (copy_computed) {
                // copy complete inherited style
                _copyCompleteStyle(item, obj_copy);
            }
        }
    }
    // copy style for Paste Style action
    if (auto item = selection->singleItem()) {
        if (copy_computed) {
            SPCSSAttr *style = take_style_from_item(item);
            _cleanStyle(style);
            sp_repr_css_set(_clipnode, style, "style");
            sp_repr_css_attr_unref(style);
        } else {
            _clipnode->copyAttribute("class", item->getRepr(), true);
            _clipnode->copyAttribute("style", item->getRepr(), true);
        }

        // copy path effect from the first path
        if (gchar const *effect = item->getRepr()->attribute("inkscape:path-effect")) {
            _clipnode->setAttribute("inkscape:path-effect", effect);
        }
    }

    if (Geom::OptRect size = selection->visualBounds()) {
        _clipnode->setAttributePoint("min", size->min());
        _clipnode->setAttributePoint("max", size->max());
    }
    if (Geom::OptRect geom_size = selection->geometricBounds()) {
        _clipnode->setAttributePoint("geom-min", geom_size->min());
        _clipnode->setAttributePoint("geom-max", geom_size->max());
    }
    if (page) {
        auto page_rect = page->getDesktopRect();
        _clipnode->setAttributePoint("page-min", page_rect.min());
        _clipnode->setAttributePoint("page-max", page_rect.max());
    }
    // Preferably set bounds based on original doc.
    // Some of the objects like <use> referring to objects which are not part of selection don't have proper bounds
    // at this stage.
    if (Geom::OptRect bounds = selection->documentBounds(SPItem::VISUAL_BBOX)) {
        _clipboardSPDoc->fitToRect(bounds.value());
    } else {
        fit_canvas_to_drawing(_clipboardSPDoc.get());
    }
}

/**
 * Copies the style from the stylesheet to preserve it.
 *
 * @param item - The source item (connected to it's document)
 * @param target - The target xml node to store the style in.
 * @param child - Flag to indicate a recursive call, do not use.
 */
void ClipboardManagerImpl::_copyCompleteStyle(SPItem *item, XML::Node *target, bool child)
{
    SelectionCopyContext(_doc, _root, _defs, cloned_elements)._copyCompleteStyle(item, target, child);
}

void ClipboardManagerImpl::_copyUsedDefs(SPItem *item)
{
    SelectionCopyContext(_doc, _root, _defs, cloned_elements)._copyUsedDefs(item);
}

XML::Node *ClipboardManagerImpl::_copyNode(XML::Node *node, XML::Document *doc, XML::Node *parent)
{
    return SelectionCopyContext(_doc, _root, _defs, cloned_elements)._copyNode(node, doc, parent);
}

void SelectionCopyContext::_copyCompleteStyle(SPItem *item, Inkscape::XML::Node *target, bool child)
{
    _copySingleStyle(item, target, child);
    if (is<SPGroup>(item)) {
        auto source_child = item->getRepr()->firstChild();
        auto target_child = target->firstChild();
        while (source_child && target_child) {
            if (auto child_item = cast<SPItem>(item->document->getObjectByRepr(source_child))) {
                _copyCompleteStyle(child_item, target_child, true);
            }
            source_child = source_child->next();
            target_child = target_child->next();
        }
    }
}

void SelectionCopyContext::_copySingleStyle(SPObject *item, XML::Node *target, bool child)
{
    auto source = item->getRepr();
    SPCSSAttr *css;
    if (child) {
        // Child styles shouldn't copy their parent's existing cascaded style.
        css = sp_repr_css_attr(source, "style");
    } else {
        css = sp_repr_css_attr_inherited(source, "style");
    }
    for (auto iter : item->style->properties()) {
        if (iter->style_src == SPStyleSrc::STYLE_SHEET) {
            css->setAttributeOrRemoveIfEmpty(iter->name(), iter->get_value());
        }
    }
    sp_repr_css_set(target, css, "style");
    sp_repr_css_attr_unref(css);

}

/**
 * Recursively copy all the definitions used by a given item to the clipboard defs.
 */
void SelectionCopyContext::_copyUsedDefs(SPItem *item)
{
    if (_enter && !_enter(item)) return;
    bool recurse = true;

    if (auto use = cast<SPUse>(item)) {
        if (auto original = use->get_original()) {
            if (original->document != use->document) {
                recurse = false;
            } else {
                cloned_elements.insert(original);
            }
        }
    }

    // copy fill and stroke styles (patterns and gradients)
    SPStyle *style = item->style;

    if (style && (style->fill.isPaintserver())) {
        SPPaintServer *server = item->style->getFillPaintServer();
        if (is<SPLinearGradient>(server) || is<SPRadialGradient>(server) || is<SPMeshGradient>(server) ) {
            _copyGradient(cast<SPGradient>(server));
        }
        auto pattern = cast<SPPattern>(server);
        if (pattern) {
            _copyPattern(pattern);
        }
        auto hatch = cast<SPHatch>(server);
        if (hatch) {
            _copyHatch(hatch);
        }
    }
    if (style && (style->stroke.isPaintserver())) {
        SPPaintServer *server = item->style->getStrokePaintServer();
        if (is<SPLinearGradient>(server) || is<SPRadialGradient>(server) || is<SPMeshGradient>(server) ) {
            _copyGradient(cast<SPGradient>(server));
        }
        auto pattern = cast<SPPattern>(server);
        if (pattern) {
            _copyPattern(pattern);
        }
        auto hatch = cast<SPHatch>(server);
        if (hatch) {
            _copyHatch(hatch);
        }
    }

    // For shapes, copy all of the shape's markers
    auto shape = cast<SPShape>(item);
    if (shape) {
        for (auto & i : shape->_marker) {
            if (i) {
                _copyNode(i->getRepr(), _doc, _defs);
            }
        }
    }

    // For 3D boxes, copy perspectives
    if (auto box = cast<SPBox3D>(item)) {
        if (auto perspective = box->get_perspective()) {
            _copyNode(perspective->getRepr(), _doc, _defs);
        }
    }

    // Copy text paths
    {
        auto text = cast<SPText>(item);
        SPTextPath *textpath = text ? cast<SPTextPath>(text->firstChild()) : nullptr;
        if (textpath) {
            _copyTextPath(textpath);
        }
        if (text) {
            for (auto &&shape_prop_ptr : {
                    reinterpret_cast<SPIShapes SPStyle::*>(&SPStyle::shape_inside),
                    reinterpret_cast<SPIShapes SPStyle::*>(&SPStyle::shape_subtract) }) {
                for (auto *href : (text->style->*shape_prop_ptr).hrefs) {
                    auto shape_obj = href->getObject();
                    if (!shape_obj)
                        continue;
                    auto shape_repr = shape_obj->getRepr();
                    if (sp_repr_is_def(shape_repr)) {
                        _copyIgnoreDup(shape_repr, _doc, _defs);
                    }
                }
            }
        }
    }

    // Copy clipping objects
    if (SPObject *clip = item->getClipObject()) {
        _copyNode(clip->getRepr(), _doc, _defs);
        // recurse
        for (auto &o : clip->children) {
            if (auto childItem = cast<SPItem>(&o)) {
                _copyUsedDefs(childItem);
            }
        }
    }
    // Copy mask objects
    if (SPObject *mask = item->getMaskObject()) {
            _copyNode(mask->getRepr(), _doc, _defs);
            // recurse into the mask for its gradients etc.
            for(auto& o: mask->children) {
                auto childItem = cast<SPItem>(&o);
                if (childItem) {
                    _copyUsedDefs(childItem);
                }
            }
    }

    // Copy filters
    if (style->getFilter()) {
        SPObject *filter = style->getFilter();
        if (is<SPFilter>(filter)) {
            _copyNode(filter->getRepr(), _doc, _defs);
        }
    }

    // For lpe items, copy lpe stack if applicable
    auto lpeitem = cast<SPLPEItem>(item);
    if (lpeitem) {
        if (lpeitem->hasPathEffect()) {
            PathEffectList path_effect_list( *lpeitem->path_effect_list);
            for (auto &lperef : path_effect_list) {
                LivePathEffectObject *lpeobj = lperef->lpeobject;
                if (lpeobj) {
                  _copyNode(lpeobj->getRepr(), _doc, _defs);
                }
            }
        }
    }

    if (!recurse) {
        return;
    }

    // recurse
    for(auto& o: item->children) {
        auto childItem = cast<SPItem>(&o);
        if (childItem) {
            _copyUsedDefs(childItem);
        }
    }
}

/**
 * Copy a single gradient to the clipboard's defs element.
 */
void SelectionCopyContext::_copyGradient(SPGradient *gradient)
{
    while (gradient) {
        // climb up the refs, copying each one in the chain
        _copyNode(gradient->getRepr(), _doc, _defs);
        if (gradient->ref){
            gradient = gradient->ref->getObject();
        }
        else {
            gradient = nullptr;
        }
    }
}

/**
 * Copy a single pattern to the clipboard document's defs element.
 */
void SelectionCopyContext::_copyPattern(SPPattern *pattern)
{
    // climb up the references, copying each one in the chain
    while (pattern) {
        _copyNode(pattern->getRepr(), _doc, _defs);

        // items in the pattern may also use gradients and other patterns, so recurse
        for (auto& child: pattern->children) {
            auto childItem = cast<SPItem>(&child);
            if (childItem) {
                _copyUsedDefs(childItem);
            }
        }
        pattern = pattern->ref.getObject();
    }
}

/**
 * Copy a single hatch to the clipboard document's defs element.
 */
void SelectionCopyContext::_copyHatch(SPHatch *hatch)
{
    // climb up the references, copying each one in the chain
    while (hatch) {
        _copyNode(hatch->getRepr(), _doc, _defs);

        for (auto &child : hatch->children) {
            auto childItem = cast<SPItem>(&child);
            if (childItem) {
                _copyUsedDefs(childItem);
            }
        }
        hatch = hatch->ref.getObject();
    }
}

/**
 * Copy a text path to the clipboard's defs element.
 */
void SelectionCopyContext::_copyTextPath(SPTextPath *tp)
{
    SPItem *path = sp_textpath_get_path_item(tp);
    if (!path) {
        return;
    }
    // textpaths that aren't in defs (on the canvas) shouldn't be copied because if
    // both objects are being copied already, this ends up stealing the refs id.
    if(path->parent && is<SPDefs>(path->parent)) {
        _copyIgnoreDup(path->getRepr(), _doc, _defs);
    }
}

/**
 * Copy a single XML node from one document to another.
 * @param node The node to be copied
 * @param target_doc The document to which the node is to be copied
 * @param parent The node in the target document which will become the parent of the copied node
 * @return Pointer to the copied node
 */
Inkscape::XML::Node *SelectionCopyContext::_copyNode(Inkscape::XML::Node *node, Inkscape::XML::Document *target_doc, Inkscape::XML::Node *parent)
{
    if (_collect) { _collect(node); return node; }
    Inkscape::XML::Node *dup = node->duplicate(target_doc);
    parent->appendChild(dup);
    Inkscape::GC::release(dup);
    return dup;
}

Inkscape::XML::Node *SelectionCopyContext::_copyIgnoreDup(Inkscape::XML::Node *node, Inkscape::XML::Document *target_doc, Inkscape::XML::Node *parent)
{
    if (_collect) { _collect(node); return node; }
    if (sp_repr_lookup_child(_root, "id", node->attribute("id"))) {
        // node already copied
        return nullptr;
    }
    Inkscape::XML::Node *dup = node->duplicate(target_doc);
    parent->appendChild(dup);
    Inkscape::GC::release(dup);
    return dup;
}

/**
 * Retrieve a bitmap image from the clipboard and paste it into the active document.
 *
 * Takes the destination @a desktop (not a bare document) so the document stay
 * tied to the window it belongs to: the texture read pumps the main context, and
 * a queued close can destroy both while the request is outstanding.
 */
bool ClipboardManagerImpl::_pasteImage(SPDesktop *desktop, gint64 deadline)
{
    if (!desktop || !_clipboard) {
        return false;
    }
    SPDocument *const doc = desktop->getDocument();
    if (!doc) {
        return false;
    }
    if (deadline <= 0) {
        deadline = g_get_monotonic_time() + PASTE_DEADLINE_US;
    }
    if (g_get_monotonic_time() > deadline) {
        return false;
    }

    // Lifetime lease (r3 product review P1-2): read_texture_async + the bounded
    // wait below pump the main context, so the desktop and its document can be
    // destroyed before the texture arrives, and the user can switch to another
    // window showing a different document. Nothing may be imported into, or
    // selected in, a destination that died, switched documents or stopped being
    // the active window during the wait (product review r4 P2-1); every use
    // below is revalidated against this captured pair plus the active desktop.
    bool desktop_alive = true;
    bool document_alive = true;
    sigc::scoped_connection const desktop_lease =
        desktop->connectDestroy([&desktop_alive] (SPDesktop *) { desktop_alive = false; });
    sigc::scoped_connection const document_lease =
        doc->connectDestroy([&document_alive] { document_alive = false; });
    auto const destination_valid = [&] {
        ClipboardLease::DestinationFacts facts;
        facts.desktop_alive = desktop_alive;
        facts.document_alive = document_alive;
        // Compared only while both are alive: the short-circuit protects the
        // freed-pointer comparison.
        facts.same_document = desktop_alive && document_alive && desktop->getDocument() == doc;
        // The paste was issued on this window; if the active window changed
        // during the wait, `file_import` would resolve its layer and selection
        // from the other window's document, so this must abort mutation-free.
        facts.active_is_captured = ClipboardLease::is_active_desktop(SP_ACTIVE_DESKTOP, desktop);
        return ClipboardLease::destination_valid(facts);
    };

    // retrieve image data under the same absolute deadline as the rest of the
    // paste: a hostile owner that advertises a texture and never answers must
    // not hang the GUI thread.
    auto request = std::make_shared<ClipboardRequest>();
    auto const cancellable = Gio::Cancellable::create();
    try {
        _clipboard->read_texture_async([request] (auto &res) {
            request->result = res;
            request->done = true;
        }, cancellable);
    } catch (Glib::Error const &err) {
        std::cout << "Pasting image failed: " << err.what() << std::endl;
        return false;
    }
    if (!wait_for_request(request, cancellable, deadline) || !request->result) {
        std::cout << "Pasting image timed out" << std::endl;
        return false;
    }
    if (!destination_valid()) {
        return false;
    }

    Glib::RefPtr<Gdk::Texture> img;
    try {
        img = _clipboard->read_texture_finish(request->result);
    } catch (Glib::Error const &err) {
        std::cout << "Pasting image failed: " << err.what() << std::endl;
        return false;
    }

    if (!img) {
        return false;
    }
    // K8: save_to_png() holds the decoded texture, the PNG stream and the
    // imported copy at once; a huge image would exhaust memory (and a failed
    // g_malloc aborts). Refuse it with the usual paste failure, before any copy.
    if (!ClipboardLease::image_within_paste_limit(img->get_width(), img->get_height())) {
        std::cout << "Pasting image failed: image exceeds the paste size limit" << std::endl;
        _userWarn(desktop, _("The image on the clipboard is too large to paste (over 100 megapixels)."));
        return false;
    }

    auto const filename = get_tmp_filename("inkscape-clipboard-import");
    auto delete_file = scope_exit([&] { unlink(filename.c_str()); });
    try {
        img->save_to_png(filename);
    } catch (Glib::Error const &err) {
        std::cout << "Pasting image failed: " << err.what() << std::endl;
        return false;
    } catch (std::exception const &err) {
        std::cout << "Pasting image failed: " << err.what() << std::endl;
        return false;
    }

    auto prefs = Preferences::get();
    auto attr_saved = prefs->getString("/dialogs/import/link");
    bool ask_saved = prefs->getBool("/dialogs/import/ask");
    auto mode_saved = prefs->getString("/dialogs/import/import_mode_svg");
    // Restore the temporary import preferences on EVERY return path, including a
    // throwing importer: the previous revision left them changed on an exception.
    auto restore_prefs = scope_exit([&] {
        prefs->setString("/dialogs/import/link", attr_saved);
        prefs->setBool("/dialogs/import/ask", ask_saved);
        prefs->setString("/dialogs/import/import_mode_svg", mode_saved);
    });
    prefs->setString("/dialogs/import/link", "embed");
    prefs->setBool("/dialogs/import/ask", false);
    prefs->setString("/dialogs/import/import_mode_svg", "embed");

    auto png = Extension::Input::find_by_mime("image/png");
    if (!png) {
        return false;
    }
    png->set_gui(false);
    auto restore_gui = scope_exit([&] { png->set_gui(true); });

    // Last revalidation immediately before file_import(), which mutates the
    // document, changes the selection and records an Undo step. It repeats the
    // full four-fact decision — captured desktop/document alive and paired, and
    // this window still the active one — because the PNG save and the extension
    // lookup above may run code. A window switch during the wait aborts here,
    // mutation-free: no import, no selection change and no Undo step.
    //
    // The import itself is additionally pinned to this desktop: file_import()
    // receives it explicitly and refuses to take a layer or a selection from any
    // window whose document is not the captured document, so even a switch
    // inside the importer cannot redirect the paste into another document.
    if (!destination_valid()) {
        return false;
    }
    file_import(doc, filename, png, desktop->point(), desktop);

    return true;
}

/**
 * Normal-paste text entry: Automatic mode with the stored preference (and the
 * ask prompt) applied. Commits exactly one Undo record for the commit-free
 * insertion paths; legacy pasteInline keeps its own commit. An aborted pass
 * (cancel/revalidation/limit/timeout) returns @c aborted without committing
 * anything, so the caller can stop the whole paste.
 * @param require_native only consume a validated native rich fragment and leave
 *        plain text to the object/SVG paste paths (whole-object clipboard wins).
 */
TextPasteOutcome ClipboardManagerImpl::_pasteText(SPDesktop *desktop, bool require_native)
{
    bool needs_commit = false;
    TextPasteOutcome const outcome =
        _pasteTextPolicy(desktop, TextPasteMode::Automatic, /*prompt_allowed=*/true, require_native, &needs_commit);
    if (outcome == TextPasteOutcome::pasted && needs_commit) {
        // Legacy normal-paste pattern: commit once here; the paste action's own
        // follow-up done() finds an empty repr log and adds no second entry.
        DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Paste text"), INKSCAPE_ICON("draw-text"));
    }
    return outcome;
}

/**
 * Paste clipboard text with an explicit formatting policy (UI actions).
 * Never prompts (the ask preference applies to normal paste only) and never
 * commits: every caller performs exactly one DocumentUndo::done after a true
 * result. Cancel aborts without touching the document, so this returns false
 * and the caller records no Undo step.
 */
bool ClipboardManagerImpl::pasteText(SPDesktop *desktop, TextPasteMode mode)
{
    if (!ensureClipboard(desktop)) return false;

    if (desktop && Bitmap::deferPublicationBoundary(*desktop,[this,mode](SPDesktop &d) {
        if (pasteText(&d,mode)) DocumentUndo::done(d.getDocument(),RC_("Undo","Paste text"),INKSCAPE_ICON("draw-text"));
    })) return false;

    _prompt_shown_in_attempt = false; // explicit commands never prompt; keep the latch consistent
    _text_attempt.active = false;     // never reuse a previous command's snapshot or decision
    bool needs_commit = false;
    return _pasteTextPolicy(desktop, mode, /*prompt_allowed=*/false, /*require_native=*/false, &needs_commit) ==
           TextPasteOutcome::pasted;
}

/**
 * Shared clipboard text read + mode policy + insertion.
 *
 * Both the clipboard read and the optional mode prompt run nested main loops, so
 * a modal window, another process or a programmatic event can close the desktop
 * or switch document/tool/edited text before we mutate anything. The destination
 * is captured before the nested loops (desktop destruction lease, document, tool,
 * edited text item) and revalidated afterwards; on any change nothing is inserted
 * and no Undo record is produced. Explicit Source/Destination commands bypass the
 * prompt; the prompt is shown for any stored default when ask is enabled.
 *
 * The returned outcome separates "no supported text" (@c not_handled, which lets
 * paste() try the legacy object/SVG targets) from "aborted" (@c aborted: user
 * cancellation, a destination/clipboard change during a nested loop, an
 * over-limit payload or a stalled producer). An abort must never trigger a
 * second read, a plaintext fallback, the object/SVG path or an Undo record.
 */
TextPasteOutcome ClipboardManagerImpl::_pasteTextPolicy(SPDesktop *desktop, TextPasteMode mode, bool prompt_allowed,
                                                        bool require_native, bool *needs_commit)
{
    namespace TP = Inkscape::UI::TextPaste;
    if (needs_commit) {
        *needs_commit = false;
    }
    if (!desktop) {
        return TextPasteOutcome::not_handled;
    }
    if (!Inkscape::have_viable_layer(desktop, desktop->messageStack())) {
        return TextPasteOutcome::not_handled;
    }

    // Lifetime lease: record destruction instead of touching a freed desktop later.
    // sigc::connection does NOT disconnect on destruction, so a scoped_connection
    // is required; it disconnects on every return path below, so the callback can
    // never outlive this stack frame (and never dangles at a later desktop
    // destruction). Same scoped pattern for the document/clipboard observers.
    bool destroyed = false;
    sigc::scoped_connection const destroyed_connection =
        desktop->connectDestroy([&destroyed] (SPDesktop *) { destroyed = true; });

    SPDocument *const document_before = desktop->getDocument();
    auto *const tool_before = desktop->getTool();
    auto *const text_tool_before = dynamic_cast<Tools::TextTool *>(tool_before);
    SPItem *const text_item_before = text_tool_before ? text_tool_before->textItem() : nullptr;

    // The destination can also change without the desktop dying: the document
    // may be edited, the tool or edited text item may switch, the caret/editor
    // selection may move, or the clipboard may be republished while the nested
    // read or the modal prompt is running. Record all of that before the nested
    // loops and re-check it before every mutation.
    bool document_changed = false;
    sigc::scoped_connection document_modified_connection;
    if (document_before) {
        document_modified_connection = document_before->connectModified(
            [&document_changed] (unsigned) { document_changed = true; });
    }

    // The process-wide generation counter is bumped by GdkClipboard's
    // signal_changed; the local snapshot records the value this pass started
    // with. No per-pass connection is needed (and the counter also covers a
    // change that happened before this pass while the command was running).
    unsigned const clipboard_generation_before = _clipboard_generation;

    Inkscape::Text::Layout::iterator sel_start_before;
    Inkscape::Text::Layout::iterator sel_end_before;
    bool const check_selection = text_tool_before && text_item_before;
    if (check_selection) {
        sel_start_before = text_tool_before->text_sel_start;
        sel_end_before = text_tool_before->text_sel_end;
    }

    // Cheap field-wise iterator equality: no layout is dereferenced, so stale
    // iterators from a previous layout generation are safe to compare here.
    auto const destination_still_valid = [&] () -> bool {
        if (destroyed || desktop->getDocument() != document_before || document_changed) {
            return false;
        }
        if (_clipboard_generation != clipboard_generation_before) {
            return false;
        }
        auto *const tool_now = desktop->getTool();
        if (tool_now != tool_before) {
            return false;
        }
        auto *const text_tool_now = dynamic_cast<Tools::TextTool *>(tool_now);
        if (text_item_before) {
            if (!text_tool_now || text_tool_now->textItem() != text_item_before) {
                return false;
            }
            if (check_selection && (text_tool_now->text_sel_start != sel_start_before ||
                                    text_tool_now->text_sel_end != sel_end_before)) {
                return false;
            }
        } else if (text_tool_now && text_tool_now->textItem()) {
            return false;
        }
        return true;
    };

    ClipboardTextReadResult read_result;
    std::string read_warning;
    // One snapshot per command: the first policy pass reads under the command's
    // absolute deadline, the second pass reuses the same snapshot (no second
    // read, no second deadline). A clipboard replaced since the snapshot aborts
    // the whole paste instead of pasting or re-reading the new contents.
    ClipboardTextRead const read = _obtainClipboardText(_text_attempt, read_result, &read_warning);
    // The nested chunked read pumped the main loop: revalidate before touching
    // any destination state or letting the caller try any other target. A
    // destination or clipboard change during that read aborts the whole paste,
    // so the newly published clipboard is never pasted by a later pass.
    bool const destination_valid_after_read = destination_still_valid();
    if (read == ClipboardTextRead::aborted) {
        // Over-limit or stalled clipboard: warn, then stop the whole paste. No
        // second read, no plaintext fallback, no prompt, no mutation.
        if (!read_warning.empty() && !destroyed) {
            _userWarn(desktop, read_warning.c_str());
        }
        return TextPasteOutcome::aborted;
    }
    if (!destination_valid_after_read) {
        return TextPasteOutcome::aborted;
    }
    if (read != ClipboardTextRead::read || read_result.fragment.empty()) {
        // Nothing usable on the clipboard; never touch the desktop for a warning
        // if it died during the nested read.
        if (!read_warning.empty() && !destroyed) {
            _userWarn(desktop, read_warning.c_str());
        }
        return TextPasteOutcome::not_handled;
    }

    // Origin, representation and formatting capability are three different
    // facts. Object priority and the SVG-markup guard key on ORIGIN (only this
    // application's own fragment may claim the native object shortcut); rich
    // insertion and the formatting question key on the validated
    // REPRESENTATION plus its decoded styles.
    bool const native = read_result.origin == TP::Origin::Native;
    bool const rich_capability = read_result.representation != TP::Representation::Plain;
    TP::Fragment const &fragment = read_result.fragment;

    // A strict native-only pass without an accepted native payload is NOT an
    // abort: the malformed native fragment may legitimately convert to the plain
    // text that was read completely in the same pass, which the normal text
    // target handles (whole-object SVG priority is preserved below).
    if (require_native && !native) {
        return TextPasteOutcome::not_handled;
    }

    // An object copy also publishes SVG markup as text/plain, and ONLY as plain
    // text: without an active edited text item that markup must reach the SVG
    // import path, not become literal characters in a new text object. A
    // validated external rich representation (HTML/RTF) is an explicit text
    // decision by the source application; decoded text that merely begins with
    // SVG-looking markup stays text (F13). Genuine whole-object copy keeps
    // object priority because it advertises plain text, never a rich text
    // format, and _getBestTarget still prefers the object targets.
    //
    // Deliberate second read (documented in REPORT.md; not a second deadline):
    // this pass classifies the DECODED fragment, but the object/SVG importer
    // consumes the raw advertised stream through a temp file, so paste() below
    // asks the object target for text/plain again via _retrieveClipboard(). The
    // raw bytes are intentionally not taken from the text snapshot: the snapshot
    // is a normalized UTF-8 view (one terminal NUL removed) and the importer must
    // see the platform's exact bytes. Both requests share this command's single
    // absolute deadline, so the second read cannot buy a fresh 5 s window, and
    // the object path discards any stale internal document before waiting.
    if (!rich_capability && !text_item_before && looks_like_svg_document(fragment.plain)) {
        return TextPasteOutcome::not_handled;
    }

    auto effective = mode;
    bool remember_choice = false;
    bool remember_native = false;
    bool const attempt_scoped = _text_attempt.active;
    if (mode == TextPasteMode::Automatic && attempt_scoped && _text_attempt.decision_made) {
        // The user already answered the formatting question during this command
        // (first policy pass). Reuse the answer: re-deriving it from preferences
        // would discard an explicit "Use destination" choice when the first
        // insertion attempt failed and the text target runs a second pass.
        effective = _text_attempt.decided_mode;
        remember_choice = _text_attempt.decided_remember;
        remember_native = _text_attempt.decided_native;
    } else if (mode == TextPasteMode::Automatic) {
        auto prefs = Preferences::get();
        if (native) {
            int const stored = prefs->getInt("/options/textpaste/mode", 0);
            effective = (stored >= 0 && stored <= 2) ? static_cast<TextPasteMode>(stored) : TextPasteMode::Automatic;
            bool const ask = prefs->getBool("/options/textpaste/ask", false);
            // The prompt applies to any stored default (Automatic/Source/Destination),
            // not only Automatic, and only to normal paste with a rich native payload.
            if (prompt_allowed && ask && !_prompt_shown_in_attempt) {
                _prompt_shown_in_attempt = true;
                auto const chosen = Inkscape::UI::Dialog::choose_text_paste_mode(desktop, effective);
                if (!chosen) {
                    // Cancelled: abort the whole paste, never prompt again and never
                    // fall back to the (possibly replaced) clipboard contents.
                    return TextPasteOutcome::aborted;
                }
                effective = chosen->mode;
                remember_choice = chosen->remember;
                remember_native = true;
                if (attempt_scoped) {
                    _text_attempt.decision_made = true;
                    _text_attempt.decided_mode = effective;
                    _text_attempt.decided_remember = remember_choice;
                    _text_attempt.decided_native = remember_native;
                }
            }
        } else if (rich_capability) {
            // Independent external preferences: a native setting never gates or
            // preselects the external question (and vice versa).
            int const stored = prefs->getInt("/options/textpaste/external-mode", 0);
            auto const stored_mode =
                (stored >= 0 && stored <= 2) ? static_cast<TextPasteMode>(stored) : TextPasteMode::Automatic;
            bool const ask = prefs->getBool("/options/textpaste/external-ask", true);
            effective = stored_mode; // Automatic resolves from the actual destination in plan_insertion()
            // The question appears only for a validated rich import that carries
            // meaningful supported formatting: plain or effectively unstyled
            // content never raises a misleading dialog.
            if (prompt_allowed && ask && read_result.has_meaningful_styles && !_prompt_shown_in_attempt) {
                // Resolve the preselection BEFORE the call: the dialog's own
                // index-0 fallback would silently mean "Keep source" in a
                // two-option list.
                bool const inside_text = text_item_before != nullptr;
                TextPasteMode const initial = stored_mode == TextPasteMode::Automatic
                                                  ? (inside_text ? TextPasteMode::Destination : TextPasteMode::Source)
                                                  : stored_mode;
                _prompt_shown_in_attempt = true;
                auto const chosen = Inkscape::UI::Dialog::choose_external_text_paste_mode(desktop, initial);
                if (!chosen) {
                    return TextPasteOutcome::aborted;
                }
                effective = chosen->mode;
                remember_choice = chosen->remember;
                remember_native = false;
                if (attempt_scoped) {
                    _text_attempt.decision_made = true;
                    _text_attempt.decided_mode = effective;
                    _text_attempt.decided_remember = remember_choice;
                    _text_attempt.decided_native = remember_native;
                }
            }
        }
        // Plain external text keeps the context rule and never prompts.
    }

    // The chosen format must not be applied to a destination that changed while
    // the nested prompt was running (document/tool/item, caret selection,
    // document contents or clipboard generation). Any change aborts with no
    // mutation and no Undo record - and, per the accepted contract, with no
    // preference change: the write below only runs after a successful insertion.
    if (!destination_still_valid()) {
        return TextPasteOutcome::aborted;
    }

    // Persist a remembered choice ONLY here, after the destination and clipboard
    // were revalidated and the insertion below succeeded. The dialog never
    // touches preferences. Explicit Source/Destination commands never reach this
    // code (prompt_allowed is false and remember_choice stays false).
    auto remember_after_success = [&] {
        if (!remember_choice || destroyed) {
            return;
        }
        auto prefs = Preferences::get();
        if (remember_native) {
            prefs->setInt("/options/textpaste/mode", static_cast<int>(effective));
            prefs->setBool("/options/textpaste/ask", false);
        } else {
            prefs->setInt("/options/textpaste/external-mode", static_cast<int>(effective));
            prefs->setBool("/options/textpaste/external-ask", false);
        }
    };

    // Missing-font feedback is paste-scoped, nonblocking and only about source
    // styles that are actually applied: destination mode discards the source
    // fonts, so it produces no warning at all.
    auto report_missing_fonts = [&] {
        if (!rich_capability || native) {
            return;
        }
        auto const plan = TP::plan_insertion(effective, text_item_before != nullptr, fragment.has_styles());
        if (!plan.source_run_styles) {
            return;
        }
        auto const report = TP::FontCheck::inspect(fragment);
        if (report.empty()) {
            return;
        }
        auto const message = TP::FontCheck::summarize(report);
        if (!message.empty()) {
            _userWarn(desktop, message.c_str());
        }
    };

    if (auto *text_tool = dynamic_cast<Tools::TextTool *>(desktop->getTool())) {
        if (rich_capability) {
            // Both native and external rich fragments reach the same insertion
            // and unit-conversion machinery; external styled text must never
            // take the legacy pasteInline(fragment.plain) path and lose all
            // formatting.
            if (text_tool->pasteFragment(fragment, effective)) {
                if (needs_commit) {
                    *needs_commit = true;
                }
                remember_after_success();
                report_missing_fonts();
                return TextPasteOutcome::pasted;
            }
            return TextPasteOutcome::not_handled;
        }
        // Legacy inline path (active text object): it keeps its own commit.
        if (text_tool->pasteInline(Glib::ustring(fragment.plain))) {
            remember_after_success();
            return TextPasteOutcome::pasted;
        }
        // Plain text with the text tool active but no edited text object:
        // create editable text with Text tool defaults.
        if (_pasteTextObject(desktop, fragment, effective)) {
            if (needs_commit) {
                *needs_commit = true;
            }
            remember_after_success();
            return TextPasteOutcome::pasted;
        }
        return TextPasteOutcome::not_handled;
    }

    // Recorded routing decision (requirements-matrix ROUTE-01): the short-colour
    // shortcut is a PLAIN-text convenience (copying "red" from a colour picker).
    // A representation with rich capability - native or external HTML/RTF - stays
    // a text decision even when its decoded text happens to be a short colour
    // string, so an external styled copy can never silently become a fill change.
    // Both directions have oracles in text-paste-test.cpp.
    if (!rich_capability && fragment.plain.size() < 30) {
        // Zero makes it impossible to paste a 100% transparent black, but it's useful.
        if (auto color = Colors::Color::parse(Glib::ustring(fragment.plain))) {
            auto color_css = sp_repr_css_attr_new();
            sp_repr_css_set_property_string(color_css, "fill", color->toString(false));
            sp_repr_css_set_property_double(color_css, "fill-opacity", color->getOpacity());
            sp_desktop_set_style(desktop, color_css);
            sp_repr_css_attr_unref(color_css);
            remember_after_success();
            return TextPasteOutcome::pasted; // non-text style change; the caller records the Undo step
        }
    }

    // Text outside the text tool (e.g. Selector paste) becomes editable text at
    // a useful canvas position; object/SVG clipboard priority is unaffected.
    if (_pasteTextObject(desktop, fragment, effective)) {
        if (needs_commit) {
            *needs_commit = true;
        }
        remember_after_success();
        report_missing_fonts();
        return TextPasteOutcome::pasted;
    }
    return TextPasteOutcome::not_handled;
}

/**
 * Read the clipboard text once, preferring the bounded native rich fragment,
 * then the external rich representations (HTML, RTF) and finally interoperable
 * plain text. Never consults remembered style, so externally replaced clipboard
 * contents cannot produce stale formatting.
 *
 * One absolute 5s deadline covers the initial native request, its chunked stream
 * read, every external rich request and the plain fallback with all their chunk
 * reads: a stalled or hostile clipboard owner that never answers makes this
 * return @c aborted instead of pumping the GUI loop forever. A timeout or a size
 * rejection aborts the whole paste: the fallback is never started after either,
 * no partial data is returned and no second deadline is bought, and each
 * representation is requested at most once. Under-cap malformed rich data falls
 * through to the COMPLETE plain alternative, never to a spliced result. Every
 * read loops over stream chunks until EOF (or the cap plus one byte), so a short
 * chunk can never silently drop the payload tail.
 *
 * The plain stream gets one platform-neutral transport allowance: exactly one
 * optional terminal NUL is removed before the semantic MAX_CHARS cap is applied,
 * so the macOS/Windows `length + 1` serialization is accepted without weakening
 * the strict native, HTML or RTF parsers.
 * @return @c read when a usable payload was read (@a result carries origin,
 *         representation, styles and diagnostics), @c no_text when the clipboard
 *         holds nothing usable, @c aborted on a timeout or limit rejection - the
 *         caller must then stop the whole paste without any fallback.
 * @param deadline absolute monotonic deadline shared by the whole user command;
 *        the caller owns it so no pass can buy a fresh timeout.
 */
ClipboardTextRead ClipboardManagerImpl::_readClipboardText(ClipboardTextReadResult &result, std::string *warning,
                                                           gint64 deadline)
{
    namespace TP = Inkscape::UI::TextPaste;
    result = ClipboardTextReadResult{};
    if (warning) {
        warning->clear();
    }
    if (!_clipboard) {
        return ClipboardTextRead::no_text;
    }

    std::size_t total_candidate_bytes = 0;

    // 1. Native rich fragment. Strict parser unchanged: the plain-text transport
    //    normalization below must never be applied to this record stream, and a
    //    trailing NUL here still rejects the payload.
    {
        Glib::ustring const native_mime = TP::MIME_TYPE;
        std::string payload;
        StreamReadStatus status = StreamReadStatus::failed;
        try {
            status = read_clipboard_bounded(_clipboard, {TP::MIME_TYPE}, TP::MAX_PAYLOAD_BYTES, deadline,
                                            payload, &native_mime);
        } catch (Glib::Error const &) {
            // Not a native payload; fall through to the external representations.
        }
        if (status == StreamReadStatus::timed_out) {
            // Never start the fallback after a timeout: it would silently buy a
            // second deadline and could paste a partial representation later.
            // The whole paste aborts.
            return ClipboardTextRead::aborted;
        }
        if (status == StreamReadStatus::too_large) {
            // Supervisor hard contract (response runtime_contract): any size
            // rejection aborts the WHOLE paste. The over-limit native payload
            // is discarded as a whole (never truncated, never partly applied),
            // no plain/object/SVG fallback is read after it, and the caller
            // records no mutation and no Undo step. The earlier revision-8
            // behavior of continuing with a plain fallback was wrong: the r4
            // OversizeRich oracle expecting a fallback is being corrected by
            // root. A malformed or invalid native payload *under* the cap may
            // still fall through to the complete plain read below.
            if (warning) {
                *warning = _("Clipboard text is too large to paste.");
            }
            return ClipboardTextRead::aborted;
        }
        // Only a fully read, validated native payload is accepted; a short,
        // malformed or invalid native read falls through to the external
        // representations, which must themselves be complete to be used.
        if (status == StreamReadStatus::complete && !payload.empty()) {
            total_candidate_bytes += payload.size();
            if (auto parsed = TP::parse(payload); parsed && !parsed->empty()) {
                result.origin = TP::Origin::Native;
                result.representation = TP::Representation::Native;
                result.fragment = std::move(*parsed);
                result.has_meaningful_styles = TP::has_meaningful_styles(result.fragment);
                return ClipboardTextRead::read;
            }
        }
    }

    // 2/3. External rich representations in the deterministic preference order
    //      native > HTML > RTF > plain. Each representation is requested at most
    //      once, on the same deadline, and the first usable decode owns the whole
    //      paste (no cross-representation splicing). An under-cap malformed
    //      representation falls through to the next one; a limit or timeout
    //      aborts the whole paste.
    struct RichCandidate {
        TP::Representation representation;
        std::vector<std::string> const *aliases;
    };
    RichCandidate const rich_candidates[] = {
        {TP::Representation::Html, &TP::html_mime_aliases()},
        {TP::Representation::Rtf, &TP::rtf_mime_aliases()},
    };
    for (auto const &candidate : rich_candidates) {
        auto const formats = _clipboard->get_formats();
        if (!formats) {
            continue;
        }
        Glib::ustring expected;
        for (auto const &alias : *candidate.aliases) {
            if (formats->contain_mime_type(alias)) {
                expected = alias;
                break;
            }
        }
        if (expected.empty()) {
            continue;
        }
        std::string payload;
        StreamReadStatus status = StreamReadStatus::failed;
        try {
            status = read_clipboard_bounded(_clipboard, {expected}, TP::MAX_PAYLOAD_BYTES, deadline, payload, &expected);
        } catch (Glib::Error const &) {
            status = StreamReadStatus::failed;
        }
        if (status == StreamReadStatus::timed_out) {
            return ClipboardTextRead::aborted;
        }
        if (status == StreamReadStatus::too_large) {
            if (warning) {
                *warning = _("Clipboard text is too large to paste.");
            }
            return ClipboardTextRead::aborted;
        }
        if (status != StreamReadStatus::complete || payload.empty()) {
            continue; // unreadable representation: try the next one, same deadline
        }
        total_candidate_bytes += payload.size();
        if (total_candidate_bytes > MAX_TOTAL_CANDIDATE_BYTES) {
            if (warning) {
                *warning = _("Clipboard text is too large to paste.");
            }
            return ClipboardTextRead::aborted;
        }
        auto decoded = TP::decode_external(payload, candidate.representation);
        if (decoded.limit_exceeded) {
            if (warning) {
                *warning = _("Clipboard text is too large to paste.");
            }
            return ClipboardTextRead::aborted;
        }
        for (auto &loss : decoded.losses) {
            if (result.losses.size() >= 64) {
                break;
            }
            result.losses.push_back(std::move(loss));
        }
        if (decoded.usable) {
            result.origin = TP::Origin::External;
            result.representation = candidate.representation;
            result.fragment = std::move(decoded.fragment);
            result.has_meaningful_styles =
                decoded.has_meaningful_styles || TP::has_meaningful_styles(result.fragment);
            return ClipboardTextRead::read;
        }
        // Under-cap malformed/unusable rich data: use the COMPLETE plain
        // alternative below, never a partial rich insert.
    }

    // 4. Plain fallback is untrusted external data too: bound the request and
    //    the stream read before materializing it, on the remaining part of the
    //    same deadline. The wire budget admits exactly one optional terminal NUL
    //    appended by the platform transport; the semantic MAX_CHARS budget is
    //    enforced after that terminator has been removed.
    std::string text;
    StreamReadStatus status = StreamReadStatus::failed;
    try {
        status = read_clipboard_bounded(_clipboard, plain_text_mime_types(), PLAIN_TEXT_TRANSPORT_LIMIT, deadline, text);
    } catch (Glib::Error const &err) {
        std::cout << "Pasting text failed: " << err.what() << std::endl;
        return ClipboardTextRead::no_text;
    }
    if (status == StreamReadStatus::timed_out) {
        // Same stop rule as the native read: a stalled producer must abort the
        // whole paste, never let the caller retry another target.
        return ClipboardTextRead::aborted;
    }
    if (status == StreamReadStatus::too_large) {
        if (warning) {
            *warning = _("Clipboard text is too large to paste.");
        }
        return ClipboardTextRead::aborted;
    }
    if (status != StreamReadStatus::complete || text.empty()) {
        return ClipboardTextRead::no_text;
    }

    // Transport normalization: remove EXACTLY ONE terminal NUL when the
    // platform transport appended one (macOS and Windows both publish
    // strlen/text+1 bytes). It is a transport artefact, not content: whitespace
    // is never trimmed, the payload is never cut at the first NUL, repeated
    // terminators are never removed and malformed UTF-8 is never repaired. A NUL
    // byte is never part of a multi-byte UTF-8 sequence, so this cannot split
    // one. Empty text or a terminator alone is a no-op.
    if (text.back() == '\0') {
        text.pop_back();
        if (text.empty()) {
            return ClipboardTextRead::no_text;
        }
    }
    if (text.find('\0') != std::string::npos) {
        // Embedded or repeated NUL: unusable external data, classified like
        // malformed UTF-8 so the object/SVG/colour targets keep their priority
        // and nothing mutates. from_plain_text() would also reject it through
        // g_utf8_validate; this explicit guard keeps the rejection independent
        // of GLib's embedded-NUL semantics and stops is_valid_run_text()'s
        // control-character cleaning from silently dropping the byte.
        return ClipboardTextRead::no_text;
    }
    if (text.size() > TP::MAX_CHARS) {
        // The semantic cap is re-applied AFTER normalization; reaching the wire
        // budget is not EOF. Never truncate to fit.
        if (warning) {
            *warning = _("Clipboard text is too large to paste.");
        }
        return ClipboardTextRead::aborted;
    }
    // The conversion itself returns a structured reason. Budget rejections
    // (byte/run/paragraph cap) are limit rejections and stop the whole paste;
    // malformed, non-UTF-8 input is treated as "no usable text here" so the
    // object/SVG targets can still apply. Classifying by the parser's own status
    // instead of re-running g_utf8_validate keeps the abort/fallback decision
    // independent of the order of the checks above.
    auto plain = TP::from_plain_text_ex(text);
    if (plain.status != TP::PlainTextStatus::ok || plain.fragment.empty()) {
        if (plain.status == TP::PlainTextStatus::over_limit) {
            if (warning) {
                *warning = _("Clipboard text is too large to paste.");
            }
            return ClipboardTextRead::aborted;
        }
        return ClipboardTextRead::no_text;
    }
    result.fragment = std::move(plain.fragment);
    result.origin = TP::Origin::External;
    result.representation = TP::Representation::Plain;
    result.has_meaningful_styles = false; // plain text never invents source formatting
    return ClipboardTextRead::read;
}

/**
 * Return this command's single clipboard-text snapshot, reading it at most once.
 *
 * The first caller of a paste command performs the bounded read under the
 * attempt's absolute deadline and caches status/result/warning plus the
 * clipboard generation observed after the read. Every later caller in the same
 * command (the second policy pass of a normal paste) reuses the cache: no second
 * read, no second 5 s deadline, and therefore no way to paste contents that
 * replaced the snapshot. When the clipboard changed since the snapshot the
 * attempt aborts: the new contents are neither pasted nor read, which is the
 * contract for a clipboard replaced during a user command.
 *
 * Callers outside a normal paste command (explicit Source/Destination actions)
 * still get exactly one bounded read: the defensive branch below creates a
 * one-shot attempt with its own deadline.
 */
ClipboardTextRead ClipboardManagerImpl::_obtainClipboardText(PasteAttemptText &attempt,
                                                             ClipboardTextReadResult &result, std::string *warning)
{
    if (!attempt.active) {
        attempt = PasteAttemptText{};
        attempt.active = true;
        attempt.deadline = g_get_monotonic_time() + PASTE_DEADLINE_US;
    }

    if (attempt.read_count > 0) {
        if (_clipboard_generation != attempt.clipboard_generation) {
            // The clipboard changed while this command was running. Never reuse
            // the stale snapshot and never read the new contents under this
            // command's deadline: abort without mutation, prompt or Undo.
            result = ClipboardTextReadResult{};
            if (warning) {
                *warning = _("The clipboard changed while pasting.");
            }
            return ClipboardTextRead::aborted;
        }
        result = attempt.result;
        if (warning) {
            *warning = attempt.warning;
        }
        return attempt.status;
    }

    ++attempt.read_count;
    ClipboardTextRead const status = _readClipboardText(result, warning, attempt.deadline);
    attempt.status = status;
    attempt.result = result;
    attempt.warning = warning ? *warning : std::string{};
    // Snapshot the generation AFTER the read: a change during the read makes the
    // cached snapshot invalid for any later pass of this same command.
    attempt.clipboard_generation = _clipboard_generation;
    return status;
}

/**
 * Create a new editable text object and insert the fragment into it, applying
 * copied paragraph properties to the root and copied character runs per run
 * when the policy asks for source formatting. Insertion only: commit-free, the
 * caller records exactly one Undo step after a true result.
 */
bool ClipboardManagerImpl::_pasteTextObject(SPDesktop *desktop, Inkscape::UI::TextPaste::Fragment const &fragment,
                                            TextPasteMode mode)
{
    namespace TP = Inkscape::UI::TextPaste;
    if (!desktop || fragment.empty()) {
        return false;
    }
    // Incoming version-2 styles are untrusted clipboard data: validate every
    // allow-listed numeric length and the destination layer's coordinate
    // mapping before the new text object is created, so a malformed payload or
    // a degenerate mapping cannot leave a partial object or an Undo record.
    if (!TP::fragment_lengths_are_valid(fragment)) {
        return false;
    }
    auto const layer = desktop->layerManager().currentLayer();
    if (!layer) {
        return false;
    }
    if (!TP::is_usable_scale(layer->i2doc_affine().descrim())) {
        return false;
    }

    auto const plan = TP::plan_insertion(mode, /*inside_existing_text=*/false, fragment.has_styles());

    // getCurrentOrToolStyle() returns nullptr for an empty merged tool style;
    // create_text_at_position() accepts that, but the ownership release below
    // must not call sp_repr_css_attr_unref(nullptr) (it asserts).
    auto css = desktop->getCurrentOrToolStyle("/tools/text", true);
    // desktop->point() is in desktop coordinates, while getViewBox() and
    // create_text_at_position() work in document units; convert before the
    // bounds test and placement, as TextTool does (text-tool.cpp: pdoc =
    // _desktop->dt2doc(p1) before create_text_at_position()).
    Geom::Point where = desktop->dt2doc(desktop->point());
    auto const page = desktop->getDocument()->getViewBox();
    if (!page.contains(where)) {
        where = page.midpoint();
    }
    SPText *item = create_text_at_position(layer, css, where, {});
    if (css) {
        sp_repr_css_attr_unref(css);
    }
    if (!item) {
        return false;
    }
    // Destination coordinate context. Fragment lengths are document-space CSS
    // px. The text object was just created by this function and has no nested
    // transformed spans, so every inserted range's coordinate context is this
    // object's own scalar — measured on the live item, not assumed from the
    // outer layer or the source. A degenerate mapping must not leave a partial
    // object behind.
    double const destination_scale = item->i2doc_affine().descrim();
    if (!TP::is_usable_scale(destination_scale)) {
        item->deleteObject();
        return false;
    }
    desktop->getSelection()->set(item);
    te_update_layout_now_recursive(item);
    auto const *layout = te_get_layout(item);
    if (!layout) {
        item->deleteObject();
        return false;
    }
    auto position = layout->begin();

    // Pre-convert every style the insertion will apply before the first
    // character is inserted: a failed conversion (non-finite, malformed or
    // over-bound) must not leave partial text or an Undo record. Relative
    // values the shared scaler would multiply numerically are compensated here
    // against the measured context; unitless line-height stays a multiplier.
    auto prepare_for_receiving = [destination_scale](std::string_view css) {
        return TP::prepare_for_receiving_api(css, destination_scale, destination_scale);
    };
    if (plan.source_run_styles) {
        for (auto const &paragraph : fragment.paragraphs) {
            for (auto const &run : paragraph.runs) {
                if (!run.style.empty() && !prepare_for_receiving(run.style)) {
                    item->deleteObject();
                    return false;
                }
            }
        }
    }
    std::vector<std::string> later_paragraph_css(fragment.paragraphs.size());
    if (plan.source_paragraph_style) {
        for (std::size_t i = 1; i < fragment.paragraphs.size(); ++i) {
            auto prepared = prepare_for_receiving(fragment.paragraphs[i].style);
            if (!prepared) {
                item->deleteObject();
                return false;
            }
            later_paragraph_css[i] = std::move(*prepared);
        }
    }

    // Full paragraph properties of the first source paragraph are merged into
    // the new text object's root style (tool defaults are preserved). This
    // direct root write bypasses the receiving API, so every normalized
    // absolute paragraph length is converted back to the object's local
    // coordinates first.
    if (plan.source_paragraph_style && !fragment.paragraphs.empty() && !fragment.paragraphs.front().style.empty()) {
        auto root_css = TP::localize_for_root_write(fragment.paragraphs.front().style, destination_scale);
        if (!root_css) {
            item->deleteObject();
            return false;
        }
        SPCSSAttr *para_css = sp_repr_css_attr_new();
        sp_repr_css_attr_add_from_string(para_css, root_css->c_str());
        sp_repr_css_change(item->getRepr(), para_css, "style");
        sp_repr_css_attr_unref(para_css);
        te_update_layout_now_recursive(item);
        if (auto const *updated = te_get_layout(item)) {
            position = updated->begin();
        }
    }

    for (std::size_t i = 0; i < fragment.paragraphs.size(); ++i) {
        auto const &paragraph = fragment.paragraphs[i];
        if (i > 0) {
            position = sp_te_insert_line(item, position);
        }
        unsigned para_start_index = 0;
        if (auto const *updated = te_get_layout(item)) {
            para_start_index = updated->iteratorToCharIndex(position);
        }
        for (auto const &run : paragraph.runs) {
            int char_index = 0;
            if (auto const *updated = te_get_layout(item)) {
                char_index = updated->iteratorToCharIndex(position);
            }
            int const inserted = static_cast<int>(g_utf8_strlen(run.text.c_str(), -1));
            std::optional<std::string> prepared;
            if (plan.source_run_styles && !run.style.empty()) {
                prepared = prepare_for_receiving(run.style);
                if (!prepared) {
                    // Prevalidated above with the same deterministic conversion and
                    // the same captured destination_scale, so this is unreachable
                    // today. Keep the abort contract local anyway: a partially
                    // inserted object must never be left behind without Undo.
                    item->deleteObject();
                    return false;
                }
            }
            position = sp_te_replace_styled(item, position, position, run.text.c_str(), nullptr);
            if (prepared) {
                if (auto const *updated = te_get_layout(item)) {
                    int const length = updated->iteratorToCharIndex(updated->end());
                    auto const first = updated->charIndexToIterator(std::min(char_index, length));
                    auto const last = updated->charIndexToIterator(std::min(char_index + inserted, length));
                    if (first != last) {
                        SPCSSAttr *run_css = sp_repr_css_attr_new();
                        sp_repr_css_attr_add_from_string(run_css, prepared->c_str());
                        sp_te_apply_style(item, first, last, run_css);
                        sp_repr_css_attr_unref(run_css);
                    }
                }
                // The style application rebuilds the layout: re-resolve the
                // insertion point from the stable character index.
                if (auto const *final_layout = te_get_layout(item)) {
                    int const final_length = final_layout->iteratorToCharIndex(final_layout->end());
                    position = final_layout->charIndexToIterator(std::min(char_index + inserted, final_length));
                }
            }
        }
        // Later paragraphs carry their own paragraph properties on their range;
        // the CSS was prepared for this object's measured context above.
        if (plan.source_paragraph_style && i > 0 && !paragraph.style.empty()) {
            if (auto const *updated = te_get_layout(item)) {
                auto const start = updated->charIndexToIterator(para_start_index);
                auto const end = updated->charIndexToIterator(updated->iteratorToCharIndex(position));
                if (start != end) {
                    SPCSSAttr *para_css = sp_repr_css_attr_new();
                    sp_repr_css_attr_add_from_string(para_css, later_paragraph_css[i].c_str());
                    sp_te_apply_style(item, start, end, para_css);
                    sp_repr_css_attr_unref(para_css);
                }
            }
        }
    }

    if (auto sptext = cast<SPText>(item)) {
        sptext->rebuildLayout();
        sptext->updateRepr();
    }
    item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
    // Commit-free: the caller owns the single Undo record for this insertion.
    return true;
}

/**
 * Publish a validated rich fragment together with its authoritative plain text
 * as one atomic Gdk content provider union (rich formats first). Any failure
 * degrades to plain text only; the clipboard is never left with a partial payload.
 */
void ClipboardManagerImpl::_publishTextFragment(Inkscape::UI::TextPaste::Fragment const &fragment)
{
    if (fragment.plain.empty()) {
        return;
    }
    namespace TP = Inkscape::UI::TextPaste;
    auto const payload = TP::serialize(fragment);
    if (payload.empty() || payload.size() > TP::MAX_PAYLOAD_BYTES) {
        _clipboard->set_text(Glib::ustring(fragment.plain));
        return;
    }

    auto const rich = Gdk::ContentProvider::create(TP::MIME_TYPE,
                                                  Glib::Bytes::create(payload.data(), payload.size()));
    auto const plain = Gdk::ContentProvider::create("text/plain;charset=utf-8",
                                                   Glib::Bytes::create(fragment.plain.data(), fragment.plain.size()));
    auto const plain_lax = Gdk::ContentProvider::create("text/plain",
                                                       Glib::Bytes::create(fragment.plain.data(), fragment.plain.size()));
    if (rich && plain && plain_lax) {
        std::vector<Glib::RefPtr<Gdk::ContentProvider>> providers;
        providers.push_back(rich);
        providers.push_back(plain);
        providers.push_back(plain_lax);
        if (!_clipboard->set_content(Gdk::ContentProvider::create(providers))) {
            _clipboard->set_text(Glib::ustring(fragment.plain));
        }
    } else {
        _clipboard->set_text(Glib::ustring(fragment.plain));
    }
}

/**
 * Applies a pasted path effect to a given item.
 */
void ClipboardManagerImpl::_applyPathEffect(SPItem *item, char const *effectstack)
{
    if (!item) {
        return;
    }

    auto lpeitem = cast<SPLPEItem>(item);
    if (lpeitem && effectstack) {
        std::istringstream iss(effectstack);
        std::string href;
        while (std::getline(iss, href, ';'))
        {
            SPObject *obj = sp_uri_reference_resolve(item->document, href.c_str());
            if (!obj) {
                return;
            }
            auto lpeobj = cast<LivePathEffectObject>(obj);
            if (lpeobj) {
                Inkscape::LivePathEffect::LPESpiro *spiroto = dynamic_cast<Inkscape::LivePathEffect::LPESpiro *>(lpeobj->get_lpe());
                bool has_spiro = lpeitem->hasPathEffectOfType(Inkscape::LivePathEffect::SPIRO);
                Inkscape::LivePathEffect::LPEBSpline *bsplineto = dynamic_cast<Inkscape::LivePathEffect::LPEBSpline *>(lpeobj->get_lpe());
                bool has_bspline = lpeitem->hasPathEffectOfType(Inkscape::LivePathEffect::BSPLINE);
                if ((!spiroto || !has_spiro) && (!bsplineto || !has_bspline)) {
                    lpeitem->addPathEffect(lpeobj);
                }
            }
        }
        // for each effect in the stack, check if we need to fork it before adding it to the item
        lpeitem->forkPathEffectsIfNecessary(1);
    }
}

/**
 * Retrieve the clipboard contents as a document.
 */
void ClipboardManagerImpl::_retrieveClipboard(Glib::ustring best_target, gint64 deadline)
{
    if (!_clipboard) {
        return;
    }
    if (deadline <= 0) {
        deadline = g_get_monotonic_time() + PASTE_DEADLINE_US;
    }

    if (_clipboard->is_local()) {
        auto const content = _clipboard->get_content();
        if (!content) {
            // A local clipboard with no provider cannot be dereferenced; discard
            // the stale internal document and report nothing on the clipboard.
            _discardInternalClipboard();
            return;
        }

        if (!GlibValue::from_content_provider<ClipboardSvg>(*content)) {
            _discardInternalClipboard();
        }

        // Nothing needs to be done, just use existing clipboard document.
        return;
    }

    _discardInternalClipboard();

    if (g_get_monotonic_time() > deadline) {
        return;
    }

    // Resolve the request target exactly ONCE, before the request. The same
    // spelling is used to request and to finish the read, so a formats change
    // during the wait cannot make read_finish() name a MIME that was never
    // requested (or a target the producer cannot answer).
    Glib::ustring const target = best_target.empty() ? _getBestTarget() : best_target;
    if (target.empty()) {
        return;
    }

    auto request = std::make_shared<ClipboardRequest>();
    auto const cancellable = Gio::Cancellable::create();
    try {
        _clipboard->read_async({target}, 0, [request] (auto &res) {
            request->result = res;
            request->done = true;
        }, cancellable);
    } catch (Glib::Error const &err) {
        std::cout << "Pasting failed: " << target << ' ' << err.what() << std::endl;
        return;
    }
    if (!wait_for_request(request, cancellable, deadline)) {
        // Bounded abort: no clipboard document is produced, so paste() reports
        // the failure and no mutation/Undo can happen.
        std::cout << "Pasting timed out: " << target << std::endl;
        return;
    }
    if (!request->result) {
        return;
    }

    Glib::RefPtr<Gio::InputStream> data;
    Glib::ustring actual_target;
    try {
        data = _clipboard->read_finish(request->result, actual_target);
    } catch (Glib::Error const &err) {
        std::cout << "Pasting failed: " << target << ' ' << err.what() << std::endl;
        return;
    }
    if (!data) {
        return;
    }
    if (actual_target != target) {
        // The producer answered a MIME that was never requested: treat it as a
        // failed read rather than importing bytes under the wrong extension.
        std::cout << "Pasting failed: " << target << " answered as " << actual_target << std::endl;
        return;
    }

    // FIXME: Temporary hack until we add memory input.
    // Save the clipboard contents to some file, then read it
    auto const filename = get_tmp_filename("inkscape-clipboard-import");
    // The temp file is removed on every path below, including a failed or
    // timed-out splice, so a partial import can never be picked up later.
    auto delete_file = scope_exit([&] { unlink(filename.c_str()); });

    {
        Glib::RefPtr<Gio::File> file;
        Glib::RefPtr<Gio::FileOutputStream> out;
        try {
            file = Gio::File::create_for_path(filename);
            out = file->replace();
        } catch (Glib::Error const &err) {
            std::cout << "Pasting failed: cannot create the import file: " << err.what() << std::endl;
            return;
        }
        if (!out) {
            return;
        }

        // The splice state is shared_ptr-owned: a late callback after a timeout
        // can never touch this stack frame, and the stream stays alive until the
        // callback has run.
        auto splice = std::make_shared<ClipboardSplice>();
        splice->output = out;
        try {
            // The same cancellable the wait cancels on expiry: a timed-out splice
            // must not keep writing into the temp file after this frame returns
            // (a later file->replace() could otherwise truncate a file that is
            // still being written).
            out->splice_async(data, [splice] (auto &result) {
                try {
                    splice->output->splice_finish(result);
                    splice->ok = true;
                } catch (Glib::Error const &) {
                    splice->ok = false;
                } catch (std::exception const &) {
                    splice->ok = false;
                }
                splice->done = true;
            }, cancellable);
        } catch (Glib::Error const &err) {
            std::cout << "Pasting failed: " << target << ' ' << err.what() << std::endl;
            return;
        }
        if (!wait_for_request(splice, cancellable, deadline)) {
            std::cout << "Pasting timed out while saving the clipboard payload" << std::endl;
            return;
        }
        if (!splice->ok) {
            std::cout << "Pasting failed: the clipboard payload could not be saved" << std::endl;
            return;
        }
        // Surface a flush/close error as well: a truncated temp file must never
        // be handed to the input extension.
        try {
            out->close();
        } catch (Glib::Error const &err) {
            std::cout << "Pasting failed: " << target << ' ' << err.what() << std::endl;
            return;
        } catch (std::exception const &err) {
            std::cout << "Pasting failed: " << target << ' ' << err.what() << std::endl;
            return;
        }
    }

    // there is no specific plain SVG input extension, so if we can paste the Inkscape SVG format,
    // we use the image/svg+xml mimetype to look up the input extension
    Glib::ustring extension_target = target;
    if (extension_target == "image/x-inkscape-svg" || extension_target == "text/plain") {
        extension_target = "image/svg+xml";
    }

    Extension::DB::InputList inlist;
    Extension::db.get_input_list(inlist);
    auto in = inlist.begin();
    for (; in != inlist.end() && extension_target != (*in)->get_mimetype(); ++in) {
    };
    if (in == inlist.end()) {
        return;
    }

    try {
        _clipboardSPDoc = (*in)->open(filename.c_str());
    } catch (Glib::Error const &err) {
        std::cout << "Pasting failed: " << target << ' ' << err.what() << std::endl;
    } catch (std::exception const &err) {
        std::cout << "Pasting failed: " << target << ' ' << err.what() << std::endl;
    } catch (...) {
        std::cout << "Pasting failed: " << target << std::endl;
    }
}

/**
 * Callback called when some other application requests data from Inkscape.
 *
 * Finds a suitable output extension to save the internal clipboard document,
 * then saves it to memory and sets the clipboard contents.
 */
void ClipboardManagerImpl::_onGet(char const *mime_type, Glib::RefPtr<Gio::OutputStream> const &output)
{
    if (!_clipboardSPDoc) {
        return;
    }

    Glib::ustring target = mime_type;
    g_info("Clipboard _onGet target: %s", target.c_str());

    if (target == "") {
        return; // this shouldn't happen
    }

    if (target == CLIPBOARD_TEXT_TARGET) {
        target = "image/x-inkscape-svg";
    }

#ifdef __APPLE__
    // translate UTI back to MIME
    if (auto mime = mime_uti.right.find(target); mime != mime_uti.right.end()) {
        target = mime->get_left();
    }
#endif

    // Refuse to return anything other than svg/text/png if being inundated with requests from a rogue clipboard manager.
    if (last_req) {
        constexpr auto magic_timeout = std::chrono::milliseconds{100};
        if (std::chrono::steady_clock::now() - *last_req < magic_timeout) {
            // Unbroken chain of rapid clipboard requests since _setClipboardTargets().
            last_req = std::chrono::steady_clock::now();
            if (target != "image/svg+xml" && target != "image/x-inkscape-svg" && target != "image/png") {
                std::cerr << "Denied clipboard request: " << mime_type << std::endl;
                return;
            }
        } else {
            // Chain has ended.
            last_req.reset();
        }
    }

    // FIXME: Temporary hack until we add support for memory output.
    // Save to a temporary file, read it back and then set the clipboard contents
    auto const filename = get_tmp_filename("inkscape-clipboard-export");

    // XXX This is a crude fix for clipboards accessing extensions
    // Remove when gui is extracted from extension execute and uses exceptions.
    bool previous_gui = INKSCAPE.use_gui();
    INKSCAPE.use_gui(false);
    // One cleanup for every path, including "no output extension for this MIME"
    // and a throwing exporter: restore the GUI mode, delete the temp file and
    // refresh the request throttle.
    auto cleanup = scope_exit([&] {
        INKSCAPE.use_gui(previous_gui);
        unlink(filename.c_str()); // delete the temporary file
        if (last_req) {
            last_req = std::chrono::steady_clock::now();
        }
    });

    try {
        Extension::DB::OutputList outlist;
        Extension::db.get_output_list(outlist);
        auto out = outlist.begin();
        for ( ; out != outlist.end() && target != (*out)->get_mimetype(); ++out) {
        }
        // No output extension for the requested MIME: publish nothing instead of
        // dereferencing outlist.end() (r3 product review P3-11).
        if (out == outlist.end()) {
            return;
        }

        if (!(*out)->loaded()) {
            // Need to load the extension.
            (*out)->set_state(Inkscape::Extension::Extension::STATE_LOADED);
        }

        if ((*out)->is_raster()) {
            double dpi = Inkscape::Util::Quantity::convert(1, "in", "px");
            Inkscape::Colors::Color bgcolor{0x00000000};

            auto origin = Geom::Point(_clipboardSPDoc->getRoot()->x.computed, _clipboardSPDoc->getRoot()->y.computed);
            auto area = Geom::Rect(origin, origin + _clipboardSPDoc->getDimensions());

            auto width  = static_cast<unsigned long>(area.width() + 0.5);
            auto height = static_cast<unsigned long>(area.height() + 0.5);

            // read from namedview
            auto const raster_file = Glib::filename_to_utf8(get_tmp_filename("inkscape-clipboard-export-raster"));
            sp_export_png_file(_clipboardSPDoc.get(), raster_file.c_str(), area, width, height, dpi, dpi, bgcolor, nullptr, nullptr, true, {});
            (*out)->export_raster(_clipboardSPDoc.get(), raster_file.c_str(), filename.c_str(), true);
            unlink(raster_file.c_str());
        } else {
            (*out)->save(_clipboardSPDoc.get(), filename.c_str(), true);
        }

        auto file = Gio::File::create_for_path(filename);
        auto in = file->read();
        if (in) {
            // A request from another application must not hang the GUI thread:
            // the splice is bounded and its finish error is caught INSIDE the
            // callback (an exception escaping a GLib callback would call
            // std::terminate). Failures fall through to the cleanup below so the
            // temporary GUI mode and the temp file are always restored.
            auto splice = std::make_shared<ClipboardSplice>();
            splice->output = output;
            auto const cancellable = Gio::Cancellable::create();
            gint64 const deadline = g_get_monotonic_time() + PASTE_DEADLINE_US;
            bool splice_started = false;
            try {
                // Same cancellable as the wait: on expiry the transfer must stop
                // instead of writing into the temp file after this frame returns
                // (r3 product review P3-6).
                output->splice_async(in, [splice] (auto &result) {
                    try {
                        splice->output->splice_finish(result);
                        splice->ok = true;
                    } catch (Glib::Error const &) {
                        splice->ok = false;
                    } catch (std::exception const &) {
                        splice->ok = false;
                    }
                    splice->done = true;
                }, cancellable);
                splice_started = true;
            } catch (Glib::Error const &) {
                splice_started = false;
            }
            if (splice_started && wait_for_request(splice, cancellable, deadline) && splice->ok) {
                file->remove();
            }
        }
    } catch (...) {
    }
}

/**
 * Creates an internal clipboard document from scratch.
 */
void ClipboardManagerImpl::_createInternalClipboard()
{
    _clipboardSPDoc = SPDocument::createNewDoc(nullptr, true);
    assert(_clipboardSPDoc);
    _defs = _clipboardSPDoc->getDefs()->getRepr();
    _doc = _clipboardSPDoc->getReprDoc();
    _root = _clipboardSPDoc->getReprRoot();

    // Preserve ANY copied text kerning
    _root->setAttribute("xml:space", "preserve");

    if (SP_ACTIVE_DOCUMENT) {
        _clipboardSPDoc->setDocumentBase(SP_ACTIVE_DOCUMENT->getDocumentBase());
    }

    _clipnode = _doc->createElement("inkscape:clipboard");
    _root->appendChild(_clipnode);
    Inkscape::GC::release(_clipnode);

    // once we create a SVG document, style will be stored in it, so flush _text_style
    if (_text_style) {
        sp_repr_css_attr_unref(_text_style);
        _text_style = nullptr;
    }
}

/**
 * Deletes the internal clipboard document.
 */
void ClipboardManagerImpl::_discardInternalClipboard()
{
    if (_clipboardSPDoc) {
        _clipboardSPDoc.reset();
        _defs = nullptr;
        _doc = nullptr;
        _root = nullptr;
        _clipnode = nullptr;
    }
}

/**
 * Get the scale to resize an item, based on the command and desktop state.
 */
Geom::Scale ClipboardManagerImpl::_getScale(SPDesktop *desktop, Geom::Point const &min, Geom::Point const &max, Geom::Rect const &obj_rect, bool apply_x, bool apply_y)
{
    double scale_x = 1.0;
    double scale_y = 1.0;

    if (apply_x) {
        scale_x = (max[Geom::X] - min[Geom::X]) / obj_rect[Geom::X].extent();
    }
    if (apply_y) {
        scale_y = (max[Geom::Y] - min[Geom::Y]) / obj_rect[Geom::Y].extent();
    }
    // If the "lock aspect ratio" button is pressed and we paste only a single coordinate,
    // resize the second one by the same ratio too
    if (desktop && Inkscape::Preferences::get()->getBool("/tools/select/lock_aspect_ratio", false)) {
        if (apply_x && !apply_y) {
            scale_y = scale_x;
        }
        if (apply_y && !apply_x) {
            scale_x = scale_y;
        }
    }

    return Geom::Scale(scale_x, scale_y);
}

/**
 * Find the most suitable clipboard target.
 */
Glib::ustring ClipboardManagerImpl::_getBestTarget(SPDesktop *desktop)
{
    auto formats = _clipboard->get_formats();

    if constexpr (DEBUG_CLIPBOARD) {
        std::cout << "_getBestTarget(): Clipboard formats: " << formats->to_string() << std::endl;
    }

    // Prioritise text when the text tool is active
    if (desktop && dynamic_cast<Inkscape::UI::Tools::TextTool *>(desktop->getTool())) {
        if (has_plain_text(formats) || has_rich_text(formats)) {
            return CLIPBOARD_TEXT_TARGET;
        }
    }

    for (auto tgt : preferred_targets) {
        if (formats->contain_mime_type(tgt)) {
            return tgt;
        }
    }
#ifdef _WIN32
    if (OpenClipboard(NULL))
    {   // Ignore retired metafile formats and keep looking for a bitmap.
        UINT format = EnumClipboardFormats(0);
        while (format) {
            if (format == CF_DIB || format == CF_BITMAP) {
                break;
            }
            format = EnumClipboardFormats(format);
        }
        CloseClipboard();

        if (format == CF_DIB || format == CF_BITMAP) {
            return CLIPBOARD_GDK_PIXBUF_TARGET;
        }
    }

#endif
    if (formats->contain_gtype(GDK_TYPE_TEXTURE)) {
        return CLIPBOARD_GDK_PIXBUF_TARGET;
    }
    // The general fallback must recognise every plain spelling the reader
    // accepts (macOS/GTK publishes only the charset-qualified one) and any
    // external rich representation. Position is unchanged: nothing here moves
    // ahead of the object/PDF/Illustrator/texture targets above.
    if (has_plain_text(formats) || has_rich_text(formats)) {
        return CLIPBOARD_TEXT_TARGET;
    }

    return "";
}

/**
 * Register the serializers for the ClipboardSvg type.
 *
 * Fixme: This only happens once on first use, so doesn't adapt to extensions being loaded/unloaded.
 * GTK4 makes this hard to support, because it is not designed to unregister serialisers.
 */
void ClipboardManagerImpl::_registerSerializers()
{
    Extension::DB::OutputList outlist;
    Extension::db.get_output_list(outlist);
    std::vector<std::string> target_list;

    bool plaintextSet = false;
    for (auto out : outlist) {
        if (!out->deactivated()) {
            Glib::ustring mime = out->get_mimetype();
#ifdef __APPLE__
            auto uti = mime_uti.left.find(mime);
            if (uti != mime_uti.left.end()) {
                target_list.emplace_back(uti->get_right());
            }
#endif
            if (mime != CLIPBOARD_TEXT_TARGET) {
                if (!plaintextSet && mime.find("svg") == Glib::ustring::npos) {
                    target_list.emplace_back(CLIPBOARD_TEXT_TARGET);
                    plaintextSet = true;
                }
                target_list.emplace_back(mime);
            }
        }
    }

    // Add PNG export explicitly since there is no extension for this...
    // On Windows, GTK will also present this as a CF_DIB/CF_BITMAP
    target_list.emplace_back("image/png");

    for (auto const &tgt : target_list) {
        gdk_content_register_serializer(GlibValue::type<ClipboardSvg>(), tgt.c_str(), +[] (GdkContentSerializer *serializer) {
            auto mime = gdk_content_serializer_get_mime_type(serializer);
            auto out = Glib::wrap(gdk_content_serializer_get_output_stream(serializer), true);
            auto self = reinterpret_cast<decltype(this)>(gdk_content_serializer_get_user_data(serializer));
            self->_onGet(mime, out);
            gdk_content_serializer_return_success(serializer);
        }, this, nullptr);
    }
}

/**
 * Set the clipboard targets to reflect the mimetypes Inkscape can output.
 */
void ClipboardManagerImpl::_setClipboardTargets()
{
    _clipboard->set_content(Gdk::ContentProvider::create(GlibValue::create<ClipboardSvg>()));
    last_req = std::chrono::steady_clock::now();
}

/**
 * Set the string representation of a 32-bit RGBA color as the clipboard contents.
 */
void ClipboardManagerImpl::_setClipboardColor(Colors::Color const &color)
{
    auto const text = color.toString();
    if (!text.empty()) {
        _clipboard->set_text(text);
    }
}

/**
 * Put a notification on the message stack.
 */
void ClipboardManagerImpl::_userWarn(SPDesktop *desktop, char const *msg)
{
    if (desktop) {
        desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE, msg);
    }
}

// The stricter native-library policy is intentionally separate from the legacy
// clipboard representation. In particular, inkscape:clipboard is NOT SVG defs.
class DetachedCopyPlan {
public:
    DetachedCopyPlan(ObjectSet &selection, SelectionCopyLimits const &limits,
                     std::function<bool()> const &cancelled)
        : source(selection.document()), limits(limits), cancelled(cancelled),
          mechanics(nullptr, nullptr, nullptr, clones,
                    [this](XML::Node *node) { dependency(node); },
                    [this](SPItem *item) {
                        check();
                        if (++engine_steps > this->limits.nodes &&
                            engine_steps - this->limits.nodes > this->limits.references) {
                            fail("Resource-engine traversal limit exceeded");
                        }
                        // Virtual clone children share source reprs and are not
                        // additional selected XML. Their real targets are in the graph.
                        return !item->cloned && states.count(item->getRepr()) && engine_seen.insert(item).second;
                    })
    {
        check();
        if (!source) fail("Selection has no source document");
        if (limits.depth > 256) fail("Requested staging depth exceeds the safe native recursion ceiling (256)");
        std::vector<SPItem *> items;
        for (auto item : selection.items()) {
            check();
            if (items.size() >= limits.nodes) fail("Selection node limit exceeded");
            if (!item || item->document != source || item->cloned) fail("Invalid selection owner");
            items.push_back(item);
            mark_visible(item->getRepr(), 1);
            add_ancestors(item->getRepr());
        }
        if (items.empty()) fail("Selection is empty");
        visual_branches = ancestors;
        visual_branches.insert(visible.begin(), visible.end());
        for (auto item : items) visit(item->getRepr(), 1);

        // Append-only queues reach a fixed point without scanning all source defs.
        // Native resource discovery supplements explicit href/style references.
        std::size_t ancestor_index = 0, object_index = 0;
        while (ancestor_index < ancestor_order.size() || object_index < scan_order.size()) {
            while (ancestor_index < ancestor_order.size()) {
                scan(ancestor_order[ancestor_index++], 1);
            }
            while (object_index < scan_order.size()) {
                auto node = scan_order[object_index++];
                if (auto item = cast<SPItem>(source->getObjectByRepr(node))) {
                    mechanics._copyUsedDefs(item);
                }
            }
        }

        // A referenced ancestor must remain a complete subtree for its use.
        // Mixing it with a pruned visible subtree needs separate IDs/remapping.
        for (auto node : resources) {
            if (!visible.count(node) && visual_branches.count(node)) {
                fail("Reference overlaps a pruned selected ancestor; private remapping required");
            }
        }
        for (auto node : ancestors) {
            if (visible.count(node) || !visual_branches.count(node)) continue;
            if (is<SPText>(source->getObjectByRepr(node))) {
                fail("Partial text-object selection requires a text-range staging policy");
            }
            if (auto item = cast<SPItem>(source->getObjectByRepr(node))) {
                if (item->getClipObject() || item->getMaskObject() || item->style->getFilter()) {
                    fail("Partial visible ancestor with clip, mask or filter requires resource rebasing");
                }
            }
        }
        // Bound potential native clone expansion before attaching any repr to an
        // SPDocument. A small acyclic DAG can otherwise expand exponentially.
        expanded_cost(source->getReprRoot(), 1);
        check();
    }

    void check() const { if (cancelled && cancelled()) throw SelectionCopyCancelled(); }
    [[noreturn]] static void fail(char const *message) { throw std::runtime_error(message); }

    static std::string number(double value)
    {
        if (!std::isfinite(value)) fail("Nonfinite document geometry");
        char buffer[G_ASCII_DTOSTR_BUF_SIZE];
        return g_ascii_dtostr(buffer, sizeof(buffer), value);
    }

    DetachedSelection build(ObjectSet &selection)
    {
        check();
        auto bounds = selection.documentBounds(SPItem::VISUAL_BBOX);
        if (!bounds || !std::isfinite(bounds->width()) || !std::isfinite(bounds->height()) ||
            bounds->width() <= 0 || bounds->height() <= 0) {
            fail("Selection has no finite positive physical extent");
        }
        auto width = bounds->width() * 25.4 / 96;
        auto height = bounds->height() * 25.4 / 96;
        if (!std::isfinite(width) || !std::isfinite(height) ||
            width <= 0 || height <= 0 || width > 1e9 || height > 1e9) {
            fail("Physical dimensions exceed library limits");
        }
        auto source_width = source->getWidth().value("px");
        auto source_height = source->getHeight().value("px");
        if (!std::isfinite(source_width) || !std::isfinite(source_height) ||
            source_width <= 0 || source_height <= 0) fail("Invalid source viewport");
        auto doc = SPDocument::createNewDocFromMem(std::string_view(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\"/>"));
        if (!doc) fail("Cannot create detached document");
        auto root = doc->getReprRoot();
        // Clear factory-generated IDs before source IDs are attached. Keep the
        // empty native defs/namedview scaffolding used by SPDocument itself.
        for (auto child = root->firstChild(); child; child = child->next()) child->setAttribute("id", nullptr);
        root->setAttribute("id", nullptr);
        // Match the outer CSS-pixel viewport to its viewBox without a px->mm->px
        // round trip. Even a one-ULP scale changes native pattern/filter sampling.
        // CSS px still means 1/96 inch; stage_selection converts these physical
        // dimensions to mm for the catalog, without scaling the artwork itself.
        root->setAttribute("width", (number(bounds->width()) + "px").c_str());
        root->setAttribute("height", (number(bounds->height()) + "px").c_str());
        auto viewbox = number(bounds->left()) + " " + number(bounds->top()) + " " +
                       number(bounds->width()) + " " + number(bounds->height());
        root->setAttribute("viewBox", viewbox.c_str());
        root->setAttribute("xml:space", "preserve");

        // Preserve native viewport and ancestral transforms, fitting only a new
        // outer viewport. No new item-geometry calculations or transform rewrite.
        using NodeOwner = std::unique_ptr<XML::Node, void (*)(XML::Node *)>;
        resource_defs = doc->getReprDoc()->createElement("svg:defs");
        NodeOwner resource_owner(resource_defs, [](XML::Node *n) { GC::release(n); });
        auto inner = copy(source->getReprRoot(), doc->getReprDoc(), false, 1);
        NodeOwner owner(inner, [](XML::Node *n) { GC::release(n); });
        if (resource_defs->firstChild()) inner->appendChild(resource_defs);
        inner->setAttribute("width", number(source_width).c_str());
        inner->setAttribute("height", number(source_height).c_str());
        // These attributes are ignored on the source top-level svg, but would
        // acquire meaning on a nested svg.
        inner->setAttribute("x", "0");
        inner->setAttribute("y", "0");
        check();
        root->appendChild(inner); // All copied images already contain data URIs.
        doc->ensureUpToDate();
        check();
        for (auto const &[original, duplicate] : copies) {
            check();
            if (g_strcmp0(original->attribute("id"), duplicate->attribute("id")) != 0 ||
                g_strcmp0(original->attribute("transform"), duplicate->attribute("transform")) != 0) {
                fail("Native staging changed a source ID/transform; publication refused");
            }
        }
        for (auto const &[original, context] : resource_contexts) {
            check();
            if (g_strcmp0(original->attribute("transform"), context.first->attribute("transform")) != 0) {
                fail("Native staging changed a private resource context; publication refused");
            }
        }
        for (auto const &[node, id] : private_ids) {
            check();
            if (g_strcmp0(node->attribute("id"), id.c_str()) != 0) {
                fail("Native staging changed a reserved private ID; publication refused");
            }
        }
        std::size_t final_nodes = 0, final_bytes = 0;
        audit(root, final_nodes, final_bytes, 1);
        DetachedSelection result;
        result.document = std::move(doc);
        result.warnings.assign(warnings.begin(), warnings.end());
        return result;
    }

private:
    void check_subtree_depth(std::size_t depth, std::size_t height, char const *message) const
    {
        // Height includes this node. Subtraction avoids overflowing depth +
        // height - 1; checking a cached subtree at its first depth is not enough.
        if (depth > limits.depth || height == 0 || height - 1 > limits.depth - depth) fail(message);
    }
    struct Cost { std::size_t nodes = 0, bytes = 0, pixels = 0, height = 1; };
    Cost expanded_cost(XML::Node *node, std::size_t depth)
    {
        check();
        if (depth > limits.depth) fail("Expanded reference depth limit exceeded");
        if (auto it = costs.find(node); it != costs.end()) {
            check_subtree_depth(depth, it->second.height, "Expanded reference depth limit exceeded");
            return it->second;
        }
        if (!expanding.insert(node).second) fail("Cyclic resource expansion");
        Cost cost{1, 32, 0, 1};
        auto add = [](std::size_t &value, std::size_t amount, std::size_t limit) {
            if (amount > limit || value > limit - amount) fail("Expanded resource work limit exceeded");
            value += amount;
        };
        if (node->content()) add(cost.bytes, std::strlen(node->content()), limits.svg_bytes);
        for (auto const &attr : node->attributeList()) add(cost.bytes, std::strlen(static_cast<char const *>(attr.value)), limits.svg_bytes);
        if (auto it = images.find(node); it != images.end()) add(cost.bytes, it->second.size(), limits.svg_bytes);
        if (auto image = cast<SPImage>(source->getObjectByRepr(node))) {
            cost.pixels = static_cast<std::size_t>(image->pixbuf->width()) * image->pixbuf->height();
        }
        auto include = [&](XML::Node *child) {
            auto part = expanded_cost(child, depth + 1);
            add(cost.nodes, part.nodes, limits.nodes);
            add(cost.bytes, part.bytes, limits.svg_bytes);
            add(cost.pixels, part.pixels, limits.total_image_pixels);
            // part.height is already bounded by limits.depth (at most 256).
            cost.height = std::max(cost.height, part.height + 1);
        };
        for (auto child = node->firstChild(); child; child = child->next()) {
            check();
            if (++expansion_walked > limits.nodes) fail("Source expansion traversal limit exceeded");
            if (scanned.count(child) || ancestors.count(child)) include(child);
        }
        if (auto it = edges.find(node); it != edges.end()) for (auto target : it->second) include(target);
        check_subtree_depth(depth, cost.height, "Expanded reference depth limit exceeded");
        expanding.erase(node);
        costs.emplace(node, cost);
        return cost;
    }

    void add_ancestors(XML::Node *node)
    {
        std::size_t depth = 0;
        for (auto parent = node->parent(); parent && parent->type() != XML::NodeType::DOCUMENT_NODE;
             parent = parent->parent()) {
            check();
            if (++depth > limits.depth) fail("Ancestor depth limit exceeded");
            if (ancestors.insert(parent).second) ancestor_order.push_back(parent);
            if (ancestors.size() > limits.nodes) fail("Ancestor node limit exceeded");
        }
    }

    void audit(XML::Node *node, std::size_t &count, std::size_t &size, std::size_t depth) const
    {
        check();
        if (++count > limits.nodes || depth > limits.depth) fail("Final copied tree limit exceeded");
        auto add = [&](std::size_t amount) {
            if (amount > limits.svg_bytes - size) fail("Final copied XML byte limit exceeded");
            size += amount;
        };
        add(32);
        if (node->content()) add(std::strlen(node->content()));
        for (auto const &attr : node->attributeList()) {
            add(std::strlen(g_quark_to_string(attr.key)));
            add(std::strlen(static_cast<char const *>(attr.value)));
        }
        for (auto child = node->firstChild(); child; child = child->next()) audit(child, count, size, depth + 1);
    }

    void charge(std::size_t amount)
    {
        if (amount > limits.svg_bytes - bytes) fail("Selection XML/embedded byte limit exceeded");
        bytes += amount;
    }
    void mark_visible(XML::Node *node, std::size_t depth)
    {
        check();
        if (depth > limits.depth) fail("Selection depth limit exceeded");
        if (!visible.insert(node).second) return;
        if (visible.size() > limits.nodes) fail("Selection node limit exceeded");
        for (auto child = node->firstChild(); child; child = child->next()) mark_visible(child, depth + 1);
    }
    void dependency(XML::Node *node)
    {
        check();
        if (++references > limits.references) fail("Selection reference limit exceeded");
        if (!node) fail("Missing resource");
        if (active_node) edges[active_node].push_back(node);
        resources.insert(node);
        add_ancestors(node);
        visit(node, active_depth + 1);
    }
    void href(std::string_view value)
    {
        if (value.size() < 2 || value.front() != '#') fail("Unsupported external or empty reference");
        auto id = std::string(value.substr(1));
        auto target = source->getObjectById(id.c_str());
        if (!target || target->document != source) fail("Missing local reference target");
        dependency(target->getRepr());
    }
    void urls(std::string_view value)
    {
        // Native style values, not an independent CSS parser. Refuse syntax this
        // conservative scanner cannot certify, rather than miss an escaped URL.
        if (value.find('\\') != value.npos || value.find("/*") != value.npos) {
            fail("Escaped/commented resource syntax is not qualified for staging");
        }
        std::string lower(value);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
        std::size_t pos = 0;
        while ((pos = lower.find("url(", pos)) != lower.npos) {
            auto end = lower.find(')', pos + 4);
            if (end == lower.npos) fail("Malformed resource URL");
            auto token = value.substr(pos + 4, end - pos - 4);
            while (!token.empty() && g_ascii_isspace(token.front())) token.remove_prefix(1);
            while (!token.empty() && g_ascii_isspace(token.back())) token.remove_suffix(1);
            if (token.size() >= 2 && (token.front() == '\'' || token.front() == '"') && token.back() == token.front()) {
                token.remove_prefix(1); token.remove_suffix(1);
            }
            href(token);
            pos = end + 1;
        }
    }
    void scan(XML::Node *node, std::size_t depth)
    {
        check();
        if (depth > limits.depth) fail("Resource depth limit exceeded");
        if (!scanned.insert(node).second) return;
        if (scanned.size() > limits.nodes) fail("Selection/resource node limit exceeded");
        scan_order.push_back(node);
        auto previous_node = active_node;
        active_node = node;
        charge(32);
        if (node->content()) charge(std::strlen(node->content()));
        if (node->type() == XML::NodeType::PI_NODE) fail("Processing instructions are not portable artwork");
        if (node->type() != XML::NodeType::ELEMENT_NODE) { active_node = previous_node; return; }
        auto name = std::string_view(node->name());
        if (name == "svg:script" || name == "svg:foreignObject" || name == "svg:style" ||
            name == "svg:animate" || name == "svg:animateTransform" || name == "svg:set") {
            fail("Active, embedded-document or selected stylesheet content is unsupported");
        }
        auto object = source->getObjectByRepr(node);
        auto image = cast<SPImage>(object);
        if (auto item = cast<SPItem>(object)) {
            for (unsigned i = 0; i < 6; ++i) {
                if (!std::isfinite(item->transform[i])) fail("Nonfinite item transform");
            }
        }
        for (auto const &attr : node->attributeList()) {
            std::string_view key(g_quark_to_string(attr.key));
            auto value = std::string_view(static_cast<char const *>(attr.value));
            charge(key.size()); charge(value.size());
            if (key == "xml:base" || key.starts_with("on")) fail("External base/event handlers are unsupported");
            if (key == "inkscape:path-effect") fail("Live path-effect closure is not yet qualified");
            if (key == "inkscape:connection-start" || key == "inkscape:connection-end") {
                fail("Connector endpoint/autorouting closure is not yet qualified");
            }
            if (key == "inkscape:perspectiveID") { href(value); continue; }
            if (key == "href" || key.ends_with(":href")) {
                if (!image) href(value);
            } else if (key == "style" || key == "fill" || key == "stroke" || key == "filter" ||
                       key == "clip-path" || key == "mask" || key.starts_with("marker-") ||
                       key == "shape-inside" || key == "shape-subtract" || key == "cursor") {
                urls(value);
            } else {
                // Include mixed-case URL functions; do not treat free-form labels
                // or arbitrary text as CSS merely because they contain a backslash.
                std::string lower(value);
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
                if (lower.find("url(") != lower.npos) urls(value);
            }
        }
        if (auto id = node->attribute("id")) {
            auto [it, inserted] = ids.emplace(id, node);
            if (!inserted && it->second != node) fail("Duplicate IDs in selected resource closure");
        }
        if (is<SPItem>(object) && object->style) {
            for (auto prop : object->style->properties()) {
                auto value = prop->get_value();
                if (prop->style_src == SPStyleSrc::STYLE_SHEET || prop->inherits) urls(value.raw());
            }
        }
        if (image) prepare_image(image);
        if (name == "svg:text" && object && object->style) {
            auto family = object->style->font_family.get_value().raw();
            if (!font_names) {
                check();
                font_names = FontFactory::get().GetAllFontNames();
                check();
            }
            auto unquoted = family;
            if (unquoted.size() >= 2 && (unquoted.front() == '\'' || unquoted.front() == '"') &&
                unquoted.back() == unquoted.front()) unquoted = unquoted.substr(1, unquoted.size() - 2);
            bool available = unquoted == "sans-serif" || unquoted == "serif" || unquoted == "monospace";
            for (auto const &installed : *font_names) {
                check();
                if (installed == unquoted) { available = true; break; }
            }
            if (!family.empty() && !available) {
                warnings.insert("Font family unavailable or unresolved: " + family + "; fonts are not embedded");
            } else {
                warnings.insert("Editable text retains font descriptions; fonts are not embedded");
            }
        }
        active_node = previous_node;
    }
    std::size_t visit(XML::Node *node, std::size_t depth)
    {
        check();
        if (depth > limits.depth) fail("Resource depth limit exceeded");
        auto &state = states[node];
        if (state == 1) fail("Cyclic selected/resource reference");
        if (state == 2) {
            auto height = visited_heights.at(node);
            check_subtree_depth(depth, height, "Resource depth limit exceeded");
            return height;
        }
        state = 1;
        auto previous_depth = active_depth;
        active_depth = depth;
        scan(node, depth);
        std::size_t height = 1;
        auto include = [&](XML::Node *child) {
            height = std::max(height, visit(child, depth + 1) + 1);
        };
        for (auto child = node->firstChild(); child; child = child->next()) include(child);
        // scan() visits reference targets while discovering them. Reusing their
        // completed heights here accounts for the ENTIRE expanded subtree, not
        // merely the shallow stack used to reach an already visited target.
        if (auto it = edges.find(node); it != edges.end()) for (auto target : it->second) include(target);
        check_subtree_depth(depth, height, "Resource depth limit exceeded");
        visited_heights.emplace(node, height);
        state = 2;
        active_depth = previous_depth;
        return height;
    }
    void prepare_image(SPImage *image)
    {
        if (image->missing || !image->pixbuf) fail("Missing bitmap; refusing placeholder artwork");
        auto href2 = image->getRepr()->attribute("href");
        auto href1 = image->getRepr()->attribute("xlink:href");
        if (href1 && href2 && std::strcmp(href1, href2)) fail("Conflicting image href attributes");
        auto const &pixels = *image->pixbuf;
        if (pixels.width() <= 0 || pixels.height() <= 0) fail("Invalid bitmap dimensions");
        auto width = static_cast<std::size_t>(pixels.width());
        auto height = static_cast<std::size_t>(pixels.height());
        if (width > limits.image_pixels / height) fail("Per-image pixel limit exceeded");
        auto count = width * height;
        if (count > limits.total_image_pixels - image_pixels) fail("Aggregate image pixel limit exceeded");
        image_pixels += count;
        gsize length = 0;
        std::string mime;
        auto original_bytes = pixels.getMimeData(length, mime);
        bool raster = original_bytes && length &&
                      (mime == "image/png" || mime == "image/jpeg" || mime == "image/jp2");
        if (!raster && image->href) {
            // Edited/native pixbufs may no longer carry compressed MIME data.
            // A PNG signature in the existing embedded payload is affirmative
            // raster provenance; a filename or MIME declaration alone is not.
            std::string_view href(image->href);
            constexpr std::string_view prefix = "data:image/png;base64,";
            if (href.starts_with(prefix)) {
                auto first = std::string(href.substr(prefix.size(), 24));
                gsize size = 0;
                auto decoded = g_base64_decode(first.c_str(), &size);
                static constexpr unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
                raster = size >= sizeof(signature) && std::memcmp(decoded, signature, sizeof(signature)) == 0;
                g_free(decoded);
            }
        }
        if (!raster) {
            fail("Image lacks affirmative raster provenance; SVG/unknown image closure is unsupported");
        }
        if (image->color_profile && *image->color_profile &&
            std::strcmp(image->color_profile, "auto") && std::strcmp(image->color_profile, "sRGB")) {
            fail("Named external image color-profile closure is not yet qualified");
        }
        for (auto key : {"icc-profile", "x-dpi", "y-dpi"}) {
            // The const getter asserts PF_GDK. The explicit false overload is a
            // read-only raw-handle lookup: it does NOT convert source pixels.
            auto raw = const_cast<Pixbuf &>(pixels).getPixbufRaw(false);
            if (auto value = gdk_pixbuf_get_option(raw, key)) {
                std::size_t size = 0;
                while (size < limits.svg_bytes && value[size]) ++size;
                if (size == limits.svg_bytes) fail("Bitmap metadata byte limit exceeded");
                charge(size);
            }
        }
        // Existing metadata-aware PNG path preserves alpha, ICC and density. The
        // encoder is synchronous; pixel budgets bound its uninterruptible unit.
        check();
        auto encoded = sp_image_encode_png_data_uri(pixels);
        check();
        if (!encoded) fail("Bitmap embedding failed");
        charge(encoded->size());
        images.emplace(image->getRepr(), std::move(*encoded));
    }
    XML::Node *copy_shallow(XML::Node *node, XML::Document *doc, std::size_t depth, bool context = false)
    {
        check();
        if (depth > limits.depth || ++copied > limits.nodes) fail("Copied tree node/depth limit exceeded");
        if (node->type() != XML::NodeType::ELEMENT_NODE) return node->duplicate(doc);
        auto result = doc->createElement(node->name());
        using NodeOwner = std::unique_ptr<XML::Node, void (*)(XML::Node *)>;
        NodeOwner owner(result, [](XML::Node *n) { GC::release(n); });
        if (!context) copies.emplace(node, result);
        if (context) charge(32);
        for (auto const &attr : node->attributeList()) {
            // `standalone` is XML-declaration metadata. Some native documents
            // expose it on the document/root representation, but it is not an
            // SVG presentation attribute and the artwork-library preflight
            // correctly rejects it as an unsupported CSS property. Do not
            // leak that serialization detail into a staged artwork asset.
            if (!context && node == source->getReprRoot() &&
                std::string_view(g_quark_to_string(attr.key)) == "standalone") {
                continue;
            }
            if (context) {
                charge(std::strlen(g_quark_to_string(attr.key)));
                charge(std::strlen(static_cast<char const *>(attr.value)));
                if (std::string_view(g_quark_to_string(attr.key)) == "id") continue;
            }
            result->setAttribute(g_quark_to_string(attr.key), static_cast<char const *>(attr.value));
        }
        auto object = source->getObjectByRepr(node);
        if (object && object->style) {
            // One source node maps to one copied node. Never zip source children
            // against a pruned destination list. Ordinary Copy still recurses.
            auto old_style = result->attribute("style");
            auto old_size = old_style ? std::strlen(old_style) : 0;
            mechanics._copySingleStyle(object, result, true);
            if (node == source->getReprRoot()) {
                auto css = sp_repr_css_attr(result, "style");
                css->setAttributeOrRemoveIfEmpty("overflow", object->style->overflow.get_value());
                sp_repr_css_set(result, css, "style");
                sp_repr_css_attr_unref(css);
            }
            auto new_style = result->attribute("style");
            auto new_size = new_style ? std::strlen(new_style) : 0;
            if (new_size > old_size) charge(new_size - old_size);
        }
        if (auto it = images.find(node); it != images.end()) {
            result->setAttribute("href", nullptr);
            result->setAttribute("xlink:href", it->second.c_str());
            result->setAttribute("sodipodi:absref", nullptr);
        }
        if (context) assign_private_id(result);
        owner.release();
        return result;
    }

    void assign_private_id(XML::Node *node)
    {
        // Reserve before native attachment: otherwise an autogenerated ID on a
        // context can steal a later source object's ID during recursive build.
        std::string id;
        do {
            check();
            id = "vacards-library-context-" + std::to_string(++private_id_serial);
        } while (ids.count(id));
        charge(id.size() + 2);
        node->setAttribute("id", id.c_str());
        private_ids.emplace(node, std::move(id));
    }

    // A generated defs BELOW a retained SPItem interrupts i2anc_affine before
    // that item's transform. Keep generated defs at the source-root boundary,
    // and reproduce the full item/viewport ancestor chain BELOW it instead.
    // These shallow contexts are not reference targets (the pruned-ancestor
    // overlap check above excludes that case), so replace duplicate IDs with
    // reserved private IDs, leaving references to retained originals unchanged.
    std::pair<XML::Node *, std::size_t> resource_parent(XML::Node *node, XML::Document *doc)
    {
        check();
        if (node == source->getReprRoot()) {
            if (!resource_defs_initialized) {
                if (++copied > limits.nodes || limits.depth < 2) fail("Copied tree node/depth limit exceeded");
                charge(32);
                assign_private_id(resource_defs);
                // Bridge non-inheriting properties for contexts that explicitly
                // inherit them from the root. Use native value serialization:
                // inventing "inherit" for every property emits values that the
                // shape/filter parsers do not support. Inheriting properties
                // already cascade through defs; rewriting e.g. a root percentage
                // font-size here would apply that percentage a second time.
                auto object = source->getRoot();
                if (object->style) {
                    std::string context_style;
                    for (auto prop : object->style->properties()) {
                        if (prop->inherits) continue;
                        auto value = prop->write(SP_STYLE_FLAG_ALWAYS).raw();
                        charge(value.size());
                        context_style += value;
                    }
                    if (!context_style.empty()) resource_defs->setAttribute("style", context_style.c_str());
                }
                resource_defs_initialized = true;
            }
            return {resource_defs, 2};
        }
        if (auto it = resource_contexts.find(node); it != resource_contexts.end()) return it->second;
        auto [parent, depth] = resource_parent(node->parent(), doc);
        auto context = copy_shallow(node, doc, depth + 1, true);
        using NodeOwner = std::unique_ptr<XML::Node, void (*)(XML::Node *)>;
        NodeOwner owner(context, [](XML::Node *n) { GC::release(n); });
        parent->appendChild(context);
        auto result = std::pair{context, depth + 1};
        resource_contexts.emplace(node, result);
        return result;
    }

    XML::Node *copy(XML::Node *node, XML::Document *doc, bool in_defs, std::size_t depth)
    {
        auto result = copy_shallow(node, doc, depth);
        if (node->type() != XML::NodeType::ELEMENT_NODE) return result;
        using NodeOwner = std::unique_ptr<XML::Node, void (*)(XML::Node *)>;
        NodeOwner owner(result, [](XML::Node *n) { GC::release(n); });
        bool const children_in_defs = in_defs || std::string_view(node->name()) == "svg:defs";
        for (auto child = node->firstChild(); child; child = child->next()) {
            check();
            if (++walked > limits.nodes) fail("Source traversal limit exceeded");
            if (!scanned.count(child) && !ancestors.count(child)) continue;
            bool const hidden_branch = !visual_branches.count(child);
            auto parent = result;
            auto child_depth = depth + 1;
            if (hidden_branch && !children_in_defs && std::string_view(child->name()) != "svg:defs") {
                auto context = resource_parent(node, doc);
                parent = context.first;
                child_depth = context.second + 1;
            }
            auto child_copy = copy(child, doc, children_in_defs || hidden_branch, child_depth);
            parent->appendChild(child_copy);
            GC::release(child_copy);
        }
        owner.release();
        return result;
    }

    SPDocument *source;
    SelectionCopyLimits limits;
    std::function<bool()> cancelled;
    std::size_t bytes = 0, references = 0, active_depth = 0, image_pixels = 0, copied = 0, walked = 0, engine_steps = 0;
    std::set<XML::Node *> visible, ancestors, resources, scanned, visual_branches;
    std::vector<XML::Node *> ancestor_order, scan_order;
    std::set<SPItem *> engine_seen;
    XML::Node *active_node = nullptr;
    std::map<XML::Node *, std::vector<XML::Node *>> edges;
    std::set<XML::Node *> expanding;
    std::map<XML::Node *, Cost> costs;
    std::size_t expansion_walked = 0;
    std::map<XML::Node *, int> states;
    std::map<XML::Node *, std::size_t> visited_heights;
    std::map<std::string, XML::Node *> ids;
    std::map<XML::Node *, std::string> images;
    std::map<XML::Node *, XML::Node *> copies;
    XML::Node *resource_defs = nullptr; // Borrowed from build()'s local owner.
    bool resource_defs_initialized = false;
    std::map<XML::Node *, std::pair<XML::Node *, std::size_t>> resource_contexts;
    std::size_t private_id_serial = 0;
    std::map<XML::Node *, std::string> private_ids;
    std::set<std::string> warnings;
    std::optional<std::vector<std::string>> font_names;
    std::set<SPItem *> clones;
    SelectionCopyContext mechanics;
};

} // namespace

DetachedSelection::DetachedSelection() = default;
DetachedSelection::~DetachedSelection() = default;
DetachedSelection::DetachedSelection(DetachedSelection &&) noexcept = default;
DetachedSelection &DetachedSelection::operator=(DetachedSelection &&) noexcept = default;
SelectionCopyCancelled::SelectionCopyCancelled() : std::runtime_error("Selection staging cancelled") {}

DetachedSelection copy_selection_detached(ObjectSet &source, SelectionCopyLimits const &limits,
                                           std::function<bool()> cancelled)
{
    if (Bitmap::publicationBoundaryPending(source.document()))
        throw std::runtime_error("Selection publication is in progress");
    return DetachedCopyPlan(source, limits, cancelled).build(source);
}

void ClipboardManager::setClipboardUnavailableForTesting(bool unavailable)
{
    clipboard_unavailable_for_testing = unavailable;
}

ClipboardManager *ClipboardManager::get()
{
    static ClipboardManagerImpl instance;
    return &instance;
}

} // namespace Inkscape::UI

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
