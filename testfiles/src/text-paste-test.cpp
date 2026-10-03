// SPDX-License-Identifier: GPL-2.0-or-later
//
// Independent, outcome-based tests for context-aware rich text copy/paste.
// These tests drive the real system clipboard (Gdk), the real ClipboardManager
// entry points and the native text DOM. They intentionally assert document
// outcomes (computed character/paragraph styles, XML, Undo/Redo) instead of
// implementation strings.

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/bytes.h>
#include <glibmm/value.h>
#include <gdkmm/display.h>
#include <gdkmm/clipboard.h>
#include <gdkmm/contentformats.h>
#include <gdkmm/contentprovider.h>
#include <giomm/inputstream.h>
#include <sigc++/scoped_connection.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "layer-manager.h"
#include "message-stack.h"
#include "object/sp-item.h"
#include "object/sp-namedview.h"
#include "object/sp-root.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "preferences.h"
#include "selection.h"
#include "style.h"
#include "text-editing.h"
#include "ui/clipboard.h"
#include "ui/text-paste.h"
#include "text-paste-clipboard-ownership.h"
#include "ui/tools/tool-base.h"
#include "ui/tools/text-tool.h"
#include "ui/widget/canvas.h"
#include "ui/widget/events/canvas-event.h"
#include "inkscape-window.h"
#include "util/cast.h"
#include "xml/repr.h"

using namespace Inkscape;
namespace {

// Locked native clipboard format (core/checkpoint.json, interface lock).
constexpr char const *kNativeMime = Inkscape::UI::TextPaste::MIME_TYPE;
namespace TP = Inkscape::UI::TextPaste;

InkscapeApplication &testApplication()
{
    static auto application = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "textpastetest", true);
        auto result = new InkscapeApplication();
        result->gio_app()->register_application();
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

// Pump the main context for a bounded time so asynchronous Gdk clipboard
// operations (read/write requests) complete.
void pumpFor(int milliseconds)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(nullptr)) {
            g_main_context_iteration(nullptr, false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool pumpUntil(std::function<bool()> const &predicate, int timeout_ms = 4000)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!predicate()) {
        drainMainContext();
        if (std::chrono::steady_clock::now() >= deadline) {
            return predicate();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

Glib::RefPtr<Gdk::Clipboard> defaultClipboard()
{
    auto display = Gdk::Display::get_default();
    if (!display) return {};
    return display->get_clipboard();
}

// ---------------------------------------------------------------------------
// Clipboard ownership token (test fixture safety)
// ---------------------------------------------------------------------------
// GTK 4 has no pasteboard change count, so the fixture counts
// GdkClipboard::signal_changed() emissions. The shared publication ledger
// refreshes its ownership token exactly once per completed publication the
// fixture itself makes; if the live counter differs at cleanup, a third party
// (the user) wrote a newer clipboard and the stale snapshot must not overwrite
// it. See text-paste-clipboard-ownership.h.
unsigned g_clipboard_change_events = 0;
TextPasteTestSupport::FixturePublicationLedger g_publication_ledger;
sigc::scoped_connection g_clipboard_changed_connection;

void ensureClipboardChangeTracking()
{
    if (g_publication_ledger.tracking_available()) {
        return;
    }
    auto clip = defaultClipboard();
    if (!clip) {
        return;
    }
    g_clipboard_changed_connection = clip->signal_changed().connect([] { ++g_clipboard_change_events; });
    g_publication_ledger.begin_tracking(g_clipboard_change_events);
}

/// Call immediately AFTER a fixture publication has been pumped, so the token
/// reflects every change event that publication produced. The exactly-once
/// accounting lives in the shared ledger (driven by the clipboard-free unit
/// regression) so a helper can neither forget nor double-count the refresh.
void noteFixtureClipboardPublish()
{
    if (!g_publication_ledger.tracking_available()) {
        return;
    }
    drainMainContext();
    g_publication_ledger.publication(
        [] { return true; },                       // the pasteboard write already happened
        [] { return g_clipboard_change_events; }); // one counter read after the pump
}

/// One fixture publication: write the pasteboard through @a publish, pump it,
/// then refresh the ownership token exactly once. Every publication helper
/// below goes through this single path.
template <typename PublishFn>
void publishClipboardAndNote(PublishFn &&publish, int pump_ms = 120)
{
    publish();
    pumpFor(pump_ms);
    noteFixtureClipboardPublish();
}

std::vector<std::string> clipboardMimeTypes()
{
    std::vector<std::string> out;
    auto clip = defaultClipboard();
    if (!clip) return out;
    auto formats = clip->get_formats();
    if (!formats) return out;
    for (auto const &mime : formats->get_mime_types()) {
        out.push_back(mime);
    }
    return out;
}

bool clipboardHasMime(std::string const &mime)
{
    auto mimes = clipboardMimeTypes();
    return std::find(mimes.begin(), mimes.end(), mime) != mimes.end();
}

struct ClipboardReadState {
    bool done = false;
    bool ok = false;
    std::string bytes;
    GInputStream *stream = nullptr;
    GCancellable *cancellable = nullptr;
};

void clipboardReadChunk(ClipboardReadState &state);

// Asynchronous chunked read: a blocking read on the main thread deadlocks the
// GTK clipboard pipe because the producer needs the same main loop.
void clipboardReadChunkCallback(GObject *source, GAsyncResult *result, gpointer user_data)
{
    auto *state = static_cast<ClipboardReadState *>(user_data);
    GError *error = nullptr;
    GBytes *chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
    if (!chunk) {
        state->done = true;
        state->ok = false;
        if (error) g_error_free(error);
        return;
    }
    gsize size = 0;
    auto const *data = static_cast<char const *>(g_bytes_get_data(chunk, &size));
    if (size == 0) {
        state->done = true;
        state->ok = true;
        g_bytes_unref(chunk);
        return;
    }
    state->bytes.append(data, size);
    g_bytes_unref(chunk);
    clipboardReadChunk(*state);
}

void clipboardReadChunk(ClipboardReadState &state)
{
    g_input_stream_read_bytes_async(state.stream, 8192, G_PRIORITY_DEFAULT, state.cancellable,
                                    clipboardReadChunkCallback, &state);
}

void clipboardReadCallback(GObject *source, GAsyncResult *result, gpointer user_data)
{
    auto *state = static_cast<ClipboardReadState *>(user_data);
    GError *error = nullptr;
    char const *out_mime = nullptr;
    state->stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), result, &out_mime, &error);
    if (!state->stream) {
        state->done = true;
        if (error) g_error_free(error);
        return;
    }
    clipboardReadChunk(*state);
}

std::optional<std::string> readClipboardBytes(std::string const &mime)
{
    auto clip = defaultClipboard();
    if (!clip) return std::nullopt;
    auto *state = new ClipboardReadState();
    state->cancellable = g_cancellable_new();
    char const *mimes[2] = {mime.c_str(), nullptr};
    gdk_clipboard_read_async(clip->gobj(), mimes, G_PRIORITY_DEFAULT, state->cancellable, clipboardReadCallback, state);
    bool const finished = pumpUntil([&] { return state->done; });
    if (!finished) {
        g_cancellable_cancel(state->cancellable);
        pumpFor(300);
    }
    std::optional<std::string> result;
    if (state->done && state->ok) result = state->bytes;
    if (state->stream) {
        g_object_unref(state->stream);
        state->stream = nullptr;
    }
    g_object_unref(state->cancellable);
    if (state->done) {
        delete state;
    } else {
        // A pending async callback may still reference the state; keep it alive.
        static std::vector<ClipboardReadState *> abandoned;
        abandoned.push_back(state);
    }
    return result;
}

// Simulate a native Inkscape instance publishing a rich fragment (optionally
// together with its plain-text interoperable fallback) atomically.
void setRichClipboard(std::string const &payload, std::optional<std::string> const &plain)
{
    auto clip = defaultClipboard();
    ASSERT_TRUE(clip);
    std::vector<Glib::RefPtr<Gdk::ContentProvider>> providers;
    auto bytes = Glib::Bytes::create(payload.data(), payload.size());
    providers.push_back(Gdk::ContentProvider::create(Glib::ustring(kNativeMime), bytes));
    if (plain) {
        Glib::Value<Glib::ustring> value;
        value.init(Glib::Value<Glib::ustring>::value_type());
        value.set(*plain);
        providers.push_back(Gdk::ContentProvider::create(value));
    }
    publishClipboardAndNote([&] { clip->set_content(Gdk::ContentProvider::create(providers)); });
}

void setPlainClipboard(std::string const &text)
{
    auto clip = defaultClipboard();
    ASSERT_TRUE(clip);
    publishClipboardAndNote([&] { clip->set_text(text); });
}

// Publish exact bytes under exactly the given MIME spellings. `set_text` cannot
// be used for the external transport fixtures: it advertises BOTH plain
// spellings and never appends a terminator, so it can reproduce neither the
// platform `strlen(text)+1` stream nor a charset-only clipboard. This is the
// faithful in-process boundary fixture for the byte-level contract; the separate
// publisher process covers real OS ownership.
void setRawClipboard(std::vector<std::string> const &mimes, std::string const &bytes)
{
    auto clip = defaultClipboard();
    ASSERT_TRUE(clip);
    if (mimes.empty()) {
        return;
    }
    auto data = Glib::Bytes::create(bytes.data(), bytes.size());
    publishClipboardAndNote([&] {
        if (mimes.size() == 1) {
            clip->set_content(Gdk::ContentProvider::create(Glib::ustring(mimes.front()), data));
        } else {
            std::vector<Glib::RefPtr<Gdk::ContentProvider>> providers;
            providers.reserve(mimes.size());
            for (auto const &mime : mimes) {
                providers.push_back(Gdk::ContentProvider::create(Glib::ustring(mime), data));
            }
            clip->set_content(Gdk::ContentProvider::create(providers));
        }
    });
}

// Each representation carries its OWN bytes (a hostile owner may answer every
// MIME differently), which is what the "no cross-representation splicing" and
// "deterministic representation choice" oracles need.
//
// The multi-part publication must refresh the ownership token too: it is the
// LAST publication of 16 of the live tests, so a missing refresh made the
// fixture's own write look foreign at cleanup (r3 tests review P2-1). The
// shared helper below notes exactly once; the single-part case delegates to
// setRawClipboard(), which notes once as well and then returns.
void setRawClipboardMulti(std::vector<std::pair<std::string, std::string>> const &parts)
{
    auto clip = defaultClipboard();
    ASSERT_TRUE(clip);
    ASSERT_FALSE(parts.empty());
    if (parts.size() == 1) {
        setRawClipboard({parts.front().first}, parts.front().second);
        return;
    }
    std::vector<Glib::RefPtr<Gdk::ContentProvider>> providers;
    providers.reserve(parts.size());
    for (auto const &part : parts) {
        auto data = Glib::Bytes::create(part.second.data(), part.second.size());
        providers.push_back(Gdk::ContentProvider::create(Glib::ustring(part.first), data));
    }
    publishClipboardAndNote([&] { clip->set_content(Gdk::ContentProvider::create(providers)); });
}

Text::Layout::iterator iteratorAt(SPItem *text, unsigned index)
{
    auto const *layout = te_get_layout(text);
    auto it = layout->begin();
    for (unsigned i = 0; i < index; ++i) {
        if (!it.nextCharacter()) break;
    }
    return it;
}

std::string characterSignature(SPItem *text, Text::Layout::iterator const &it)
{
    auto const *style = sp_te_style_at_position(text, it);
    if (!style) return "<no-style>";
    char buffer[512];
    unsigned fill = 0xdeadbeefu;
    if (style->fill.isColor()) {
        fill = style->fill.getColor().toRGBA();
    }
    std::snprintf(buffer, sizeof buffer, "fam=%s;size=%.4f;weight=%d;style=%d;caps=%d;ls=%.4f;fill=%08x",
                  style->font_family.value() ? style->font_family.value() : "",
                  style->font_size.computed,
                  static_cast<int>(style->font_weight.computed),
                  static_cast<int>(style->font_style.computed),
                  static_cast<int>(style->font_variant_caps.computed),
                  style->letter_spacing.computed,
                  fill);
    return buffer;
}

std::string paragraphSignature(SPItem *text, Text::Layout::iterator const &it)
{
    auto const *style = sp_te_style_at_position(text, it);
    if (!style) return "<no-style>";
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, "anchor=%d;line-height=%.4f",
                  static_cast<int>(style->text_anchor.computed),
                  style->line_height.computed);
    return buffer;
}

// Character signatures including virtual control codes. A layout contains the
// paragraph break as a control code whose source object is the preceding
// tspan/paragraph wrapper rather than an SPString; it has no stored run style.
// Such a position is normalized to an explicit "<paragraph-break>" marker so the
// vector keeps its layout positions (index-based assertions elsewhere stay
// valid) while no non-existent style is compared (ui/separator-review.json,
// response.json separator_oracle_detail). The marker is positively guarded:
// there must be exactly one per '\n' in the layout's own multiline string and
// each skipped layout position must map to that newline, so a stored character
// can never be dropped. SPString-backed whitespace, tabs and Unicode stay
// compared as-is.
// The guard uses the range overload sp_te_get_string_multiline(text,
// layout->begin(), layout->end()): it emits '\n' at every non-SPString layout
// position, so its entries line up one-to-one with layout character positions
// and its breaks are exactly the virtual breaks the layout exposes. The
// whole-text overload instead defers a trailing sodipodi:role="line" line-break
// tspan (pending_line_break is never flushed for the fixture), so it can omit a
// break that the layout still exposes (r6-findings.json F-guard).
// The guard indexes the result as Glib::ustring: layout iterators count Unicode
// code points (response.json r6_review_fix), so both its size() and operator[]
// are code-point based; indexing a std::string here would compare a byte offset
// for fixtures whose text contains multi-byte characters (Hebrew, combining
// accents, emoji) before the paragraph break.
std::vector<std::string> characterSignatures(SPItem *text)
{
    auto const *layout = te_get_layout(text);
    Glib::ustring const multiline = sp_te_get_string_multiline(text, layout->begin(), layout->end());
    unsigned newlines = 0;
    for (auto const c : multiline) {
        if (c == '\n') ++newlines;
    }
    unsigned control_codes = 0;
    std::vector<std::string> out;
    for (auto it = layout->begin(); it != layout->end(); it.nextCharacter()) {
        SPObject *source = nullptr;
        layout->getSourceOfCharacter(it, &source, nullptr);
        if (!is<SPString>(source)) {
            ++control_codes;
            int const char_index = layout->iteratorToCharIndex(it);
            if (char_index >= 0 && static_cast<std::size_t>(char_index) < multiline.size()) {
                EXPECT_EQ(multiline[static_cast<std::size_t>(char_index)], '\n')
                    << "only a paragraph break may be normalized, never a stored character";
            } else {
                ADD_FAILURE() << "control-code layout position has no stored newline";
            }
            out.push_back("<paragraph-break>");
            continue;
        }
        out.push_back(characterSignature(text, it));
    }
    EXPECT_EQ(control_codes, newlines) << "every normalized control code must be a paragraph break";
    return out;
}

std::vector<std::string> paragraphSignatures(SPItem *text)
{
    std::vector<std::string> out;
    auto const *layout = te_get_layout(text);
    for (auto it = layout->begin(); it != layout->end(); it.nextCharacter()) {
        out.push_back(paragraphSignature(text, it));
    }
    return out;
}

// Paragraph properties of pre-existing characters must survive an insertion or a
// replacement. The pasted fragment may add (or remove) character positions, so
// only the surviving characters are compared: the unchanged prefix keeps its
// indices and the surviving suffix is addressed by a layout index
// (@a suffix_at, measured after the edit). A paragraph break is not necessarily
// one indexed layout position, so the multiline string length must never be
// used to shift layout indices.
void expectPreservedParagraphSignatures(SPItem *text, std::vector<std::string> const &before,
                                        unsigned changed_from, unsigned removed, unsigned suffix_at)
{
    auto const after_count = paragraphSignatures(text).size();
    for (unsigned i = 0; i < before.size(); ++i) {
        if (i >= changed_from && i < changed_from + removed) continue;
        unsigned const index = i < changed_from ? i : suffix_at + (i - (changed_from + removed));
        ASSERT_LT(index, after_count)
            << "layout oracle index out of range for pre-existing character " << i;
        EXPECT_EQ(paragraphSignature(text, iteratorAt(text, index)), before[i])
            << "pre-existing paragraph properties must not change at character " << i;
    }
}

std::string multilineText(SPItem *text)
{
    return sp_te_get_string_multiline(text);
}

// Logical character index (Unicode code points) of the first occurrence of
// @a needle, or max() when absent. Layout iterators count code points, so byte
// offsets must never be used directly.
unsigned logicalIndexOf(SPItem *text, std::string const &needle)
{
    std::string const haystack = multilineText(text);
    auto const pos = haystack.find(needle);
    if (pos == std::string::npos) return std::numeric_limits<unsigned>::max();
    return static_cast<unsigned>(g_utf8_strlen(haystack.c_str(), static_cast<gssize>(pos)));
}

// Convert a byte offset from std::string::find() into a logical character index.
unsigned charIndexOfByte(std::string const &haystack, std::size_t byte_index)
{
    return static_cast<unsigned>(g_utf8_strlen(haystack.c_str(), static_cast<gssize>(byte_index)));
}

std::string subtreeXml(SPItem *item)
{
    return sp_repr_write_buf(item->getRepr(), 0, false, GQuark(0), 0, 0).raw();
}

void diagnosticDump(std::string const &case_name, std::string const &suffix, std::string const &text);

// Test-local isolation for the canvas pointer controllers: the GUI test window
// shares the user's display, so a real pointer enter/motion can arrive mid-case
// and replace the documented no-motion fallback. Event delivery is disabled and
// the motion/enter handlers are blocked on every observed motion controller for
// the critical section; the destructor restores phase and handlers.
struct PropagationPhaseGuard {
    struct Entry {
        GtkEventController *controller;
        GtkPropagationPhase phase;
    };
    std::vector<Entry> entries;

    PropagationPhaseGuard() = default;
    PropagationPhaseGuard(PropagationPhaseGuard const &) = delete;
    PropagationPhaseGuard &operator=(PropagationPhaseGuard const &) = delete;
    ~PropagationPhaseGuard()
    {
        guint const motion_id = g_signal_lookup("motion", GTK_TYPE_EVENT_CONTROLLER_MOTION);
        guint const enter_id = g_signal_lookup("enter", GTK_TYPE_EVENT_CONTROLLER_MOTION);
        for (auto &entry : entries) {
            gtk_event_controller_set_propagation_phase(entry.controller, entry.phase);
            g_signal_handlers_unblock_matched(entry.controller, G_SIGNAL_MATCH_ID, motion_id, 0, nullptr, nullptr,
                                              nullptr);
            g_signal_handlers_unblock_matched(entry.controller, G_SIGNAL_MATCH_ID, enter_id, 0, nullptr, nullptr,
                                              nullptr);
        }
    }
};

// Installs the isolation on every motion controller of the canvas into an
// existing guard; the controllers stay owned by the widget.
void isolateCanvasPointer(UI::Widget::Canvas *canvas, PropagationPhaseGuard &guard)
{
    guint const motion_id = g_signal_lookup("motion", GTK_TYPE_EVENT_CONTROLLER_MOTION);
    guint const enter_id = g_signal_lookup("enter", GTK_TYPE_EVENT_CONTROLLER_MOTION);
    auto controllers = gtk_widget_observe_controllers(GTK_WIDGET(canvas->gobj()));
    for (unsigned i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto controller = G_OBJECT(g_list_model_get_item(controllers, i));
        if (GTK_IS_EVENT_CONTROLLER_MOTION(controller)) {
            auto *motion = GTK_EVENT_CONTROLLER(controller);
            guard.entries.push_back({motion, gtk_event_controller_get_propagation_phase(motion)});
            gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_NONE);
            g_signal_handlers_block_matched(motion, G_SIGNAL_MATCH_ID, motion_id, 0, nullptr, nullptr, nullptr);
            g_signal_handlers_block_matched(motion, G_SIGNAL_MATCH_ID, enter_id, 0, nullptr, nullptr, nullptr);
        }
        g_object_unref(controller);
    }
    g_object_unref(controllers);
}

bool containsSubstring(std::string const &haystack, std::string const &needle)
{
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Numeric length audit. "nan"/"inf" are legal substrings of unrelated
// serialized names (dominant-baseline, -inkscape-font-specification, ids), so a
// whole-document substring search is not a valid nonfinite-length oracle: it
// fails on the legitimate name instead of the invalid value. These helpers
// parse the *values* of the length properties the repair touches and reject
// non-finite numbers; a separate check rejects a non-positive font-size that a
// sign-flipped reflection would produce.
// ---------------------------------------------------------------------------
std::set<std::string> const &auditedLengthProperties()
{
    static std::set<std::string> const properties = {
        "font-size",      "line-height",       "letter-spacing", "word-spacing",
        "text-indent",    "baseline-shift",    "stroke-width",   "stroke-dasharray",
        "stroke-dashoffset", "kerning",        "font-size-adjust",
    };
    return properties;
}

// True when any comma-separated item of @a value starts with a non-finite
// number (an explicit nan/inf literal, with optional sign, or an overflowing
// numeric literal). Keywords without a leading number are ignored.
bool valueHasNonFiniteNumber(std::string const &value)
{
    std::size_t pos = 0;
    while (pos <= value.size()) {
        auto const comma = value.find(',', pos);
        std::string const item =
            value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        char const *token = item.c_str();
        while (g_ascii_isspace(static_cast<guchar>(*token))) ++token;
        char const *literal = (*token == '+' || *token == '-') ? token + 1 : token;
        if (g_ascii_strncasecmp(literal, "nan", 3) == 0 || g_ascii_strncasecmp(literal, "inf", 3) == 0) {
            return true;
        }
        char *end = nullptr;
        double const number = g_ascii_strtod(token, &end);
        if (end != token && !std::isfinite(number)) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

// Visit the style declarations and the presentation-attribute form of every
// audited length property in the serialized document tree.
void forEachAuditedLengthDeclaration(XML::Node *node,
                                     std::function<void(std::string const &, std::string const &)> const &fn)
{
    if (auto const *style = node->attribute("style")) {
        std::string const css(style);
        std::size_t pos = 0;
        while (pos < css.size()) {
            auto const semi = css.find(';', pos);
            std::string const decl = css.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
            pos = semi == std::string::npos ? css.size() : semi + 1;
            auto const colon = decl.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = decl.substr(0, colon);
            while (!name.empty() && g_ascii_isspace(static_cast<guchar>(name.front()))) name.erase(name.begin());
            while (!name.empty() && g_ascii_isspace(static_cast<guchar>(name.back()))) name.pop_back();
            if (auditedLengthProperties().count(name)) {
                fn(name, decl.substr(colon + 1));
            }
        }
    }
    for (auto const &name : auditedLengthProperties()) {
        if (auto const *value = node->attribute(name.c_str())) {
            fn(name, value);
        }
    }
    for (auto child = node->firstChild(); child; child = child->next()) {
        forEachAuditedLengthDeclaration(child, fn);
    }
}

bool documentHasNonFiniteLength(SPDocument *doc)
{
    bool bad = false;
    forEachAuditedLengthDeclaration(doc->getReprDoc()->root(),
                                    [&](std::string const &, std::string const &value) {
                                        if (valueHasNonFiniteNumber(value)) bad = true;
                                    });
    return bad;
}

// A reflection must never persist a negative font size. Parsing the declaration
// catches a sign-flipped value without matching unrelated "-24" substrings
// (coordinates, ids, dash arrays).
bool documentHasNonPositiveFontSize(SPDocument *doc)
{
    bool bad = false;
    forEachAuditedLengthDeclaration(doc->getReprDoc()->root(),
                                    [&](std::string const &name, std::string const &value) {
                                        if (name != "font-size") return;
                                        char const *token = value.c_str();
                                        while (g_ascii_isspace(static_cast<guchar>(*token))) ++token;
                                        char *end = nullptr;
                                        double const number = g_ascii_strtod(token, &end);
                                        if (end != token && (!std::isfinite(number) || number <= 0.0)) bad = true;
                                    });
    return bad;
}

// Preferences::remove() walks the key path without a null check on intermediate
// nodes (preferences.cpp:510-547), so removing a key whose group was never
// created dereferences null. Only remove keys that are actually present.
void removePrefIfSet(char const *key)
{
    if (Preferences::get()->getEntry(key).isSet()) {
        Preferences::get()->remove(key);
    }
}

struct PrefRestore {
    Preferences *prefs = Preferences::get();
    std::string key;
    Preferences::Entry old;
    explicit PrefRestore(char const *k) : key(k), old(prefs->getEntry(k)) {}
    ~PrefRestore()
    {
        if (old.isSet()) {
            prefs->setString(key.c_str(), old.getString());
        } else {
            removePrefIfSet(key.c_str());
        }
    }
};

// ---------------------------------------------------------------------------
// Prompt automation: the UI publishes stable GTK widget names for exactly this
// dialog (ui/interface.json). Automation is scoped to the named dialog and only
// touches named widgets; it never guesses by type/label or closes other windows.
// ---------------------------------------------------------------------------
struct PromptWatch {
    bool seen = false;
    bool acted = false;
    bool timed_out = false;
    bool clicked_confirm = false;
    bool clicked_source = false;
    bool remember = false;              ///< check "Remember this choice"
    std::string remember_name = "text-paste-remember"; ///< the question's own remember widget
    std::string select_name;            ///< radio widget name to select
    std::string dialog_name = "text-paste-dialog"; ///< toplevel to watch (external question has its own)
    std::vector<std::string> preselection_names;   ///< radio names inspected once when the dialog opens
    std::string preselected_name;                  ///< which of them was active
    bool confirm = false;               ///< click text-paste-accept instead of cancel
    std::function<void()> side_effect;  ///< runs once while the dialog is open
    int dialogs = 0;
    int ticks = 0;
    guint source_id = 0;  ///< owned by PromptTimerGuard
    GtkWidget *last_dialog = nullptr;
};

// Exact-name widget lookup (ui/interface.json: match_rule is gtk_widget_get_name
// string equality; no label/type guessing).
GtkWidget *findNamedWidget(GtkWidget *root, std::string const &name)
{
    if (name == gtk_widget_get_name(root)) return root;
    for (auto child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        if (auto found = findNamedWidget(child, name)) return found;
    }
    return nullptr;
}

gboolean promptTick(gpointer data)
{
    auto *watch = static_cast<PromptWatch *>(data);
    ++watch->ticks;
    GListModel *windows = gtk_window_get_toplevels();
    guint const count = g_list_model_get_n_items(windows);
    GtkWidget *dialog = nullptr;
    for (guint i = 0; i < count && !dialog; ++i) {
        auto *window = GTK_WINDOW(g_list_model_get_item(windows, i));
        if (gtk_widget_get_visible(GTK_WIDGET(window)) &&
            std::string(gtk_widget_get_name(GTK_WIDGET(window))) == watch->dialog_name) {
            dialog = GTK_WIDGET(window);
        } else {
            g_object_unref(window);
        }
    }
    if (dialog) {
        if (dialog != watch->last_dialog) {
            watch->last_dialog = dialog;
            auto *child = gtk_window_get_child(GTK_WINDOW(dialog));
            auto *cancel = child ? findNamedWidget(child, "text-paste-cancel") : nullptr;
            auto *accept = child ? findNamedWidget(child, "text-paste-accept") : nullptr;
            if (cancel && accept) {
                watch->seen = true;
                ++watch->dialogs;
                if (!watch->acted) {
                    watch->acted = true;
                    if (watch->side_effect) watch->side_effect();
                }
                if (watch->remember) {
                    if (auto *remember = findNamedWidget(child, watch->remember_name)) {
                        gtk_widget_activate(remember);
                    }
                }
                if (!watch->select_name.empty()) {
                    if (auto *radio = findNamedWidget(child, watch->select_name)) {
                        if (watch->select_name == "text-paste-mode-source") watch->clicked_source = true;
                        gtk_widget_activate(radio);
                    }
                }
                for (auto const &name : watch->preselection_names) {
                    if (auto *radio = findNamedWidget(child, name)) {
                        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(radio))) {
                            watch->preselected_name = name;
                        }
                    }
                }
                if (watch->confirm) {
                    watch->clicked_confirm = true;
                    gtk_widget_activate(accept);
                } else {
                    gtk_widget_activate(cancel);
                }
            }
        }
        g_object_unref(dialog);
        return G_SOURCE_CONTINUE; // keep watching: a second dialog is a failure signal
    }
    watch->last_dialog = nullptr;
    if (watch->ticks > 300) {
        // Only close the positively identified named dialog; never other windows.
        for (guint i = 0; i < count; ++i) {
            auto *window = GTK_WINDOW(g_list_model_get_item(windows, i));
            if (std::string(gtk_widget_get_name(GTK_WIDGET(window))) == watch->dialog_name) {
                gtk_window_close(window);
            }
            g_object_unref(window);
        }
        watch->timed_out = true;
        watch->source_id = 0; // the source is removed by returning G_SOURCE_REMOVE
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// The registered timeout must never outlive its stack-local PromptWatch, even
// when an ASSERT_* early-returns out of a test body before the manual cleanup.
struct PromptTimerGuard {
    explicit PromptTimerGuard(PromptWatch &w) : watch(w) { watch.source_id = g_timeout_add(50, promptTick, &watch); }
    ~PromptTimerGuard() { stop(); }
    PromptTimerGuard(PromptTimerGuard const &) = delete;
    PromptTimerGuard &operator=(PromptTimerGuard const &) = delete;
    void stop()
    {
        if (watch.source_id) {
            g_source_remove(watch.source_id);
            watch.source_id = 0;
        }
    }
    PromptWatch &watch;
};

// ---------------------------------------------------------------------------
// Test-side native clipboard provider. GDK hands the provider's output stream to
// the reader, so a custom GdkContentProvider exercises the product's real
// bounded, chunked clipboard read end-to-end through public GDK APIs: every
// chunk is written asynchronously and the next one is scheduled only from the
// completed write's callback; alternatively a producer delivers one chunk and
// then stalls
// forever. No product test hook is involved.
// ---------------------------------------------------------------------------
struct ChunkedProvider {
    GdkContentProvider parent_instance;
    GBytes *payload;       ///< owned
    char *mime;            ///< owned; advertised transfer format (native by default)
    GBytes *payload2;      ///< owned; optional second advertised representation
    char *mime2;           ///< owned; its MIME spelling, nullptr when unused
    gsize chunk_size;
    guint delay_ms;
    gboolean stall_after_first;
    unsigned requests = 0;  ///< write requests for mime
    unsigned requests2 = 0; ///< write requests for mime2
};

struct ChunkedProviderClass {
    GdkContentProviderClass parent_class;
};

G_DEFINE_TYPE(ChunkedProvider, chunked_provider, GDK_TYPE_CONTENT_PROVIDER)

void chunked_provider_init(ChunkedProvider *) {}

void chunked_provider_finalize(GObject *object)
{
    auto *self = reinterpret_cast<ChunkedProvider *>(object);
    if (self->payload) {
        g_bytes_unref(self->payload);
        self->payload = nullptr;
    }
    if (self->payload2) {
        g_bytes_unref(self->payload2);
        self->payload2 = nullptr;
    }
    g_free(self->mime);
    self->mime = nullptr;
    g_free(self->mime2);
    self->mime2 = nullptr;
    G_OBJECT_CLASS(chunked_provider_parent_class)->finalize(object);
}

GdkContentFormats *chunked_provider_ref_formats(GdkContentProvider *provider)
{
    auto *self = reinterpret_cast<ChunkedProvider *>(provider);
    char const *mimes[] = {self->mime ? self->mime : kNativeMime, self->mime2, nullptr};
    return gdk_content_formats_new(mimes, self->mime2 ? 2 : 1);
}

struct ChunkedWriteState {
    GTask *task = nullptr;
    GOutputStream *stream = nullptr;
    std::string payload;
    gsize chunk_size = 16;
    gsize offset = 0;
    guint delay_ms = 1;
    bool stall_after_first = false;
};

// A stalled producer is intentionally never completed; the product must time
// out. Keep the pending state reachable (its GTask and stream stay referenced
// and the async result is never freed here).
std::vector<ChunkedWriteState *> g_stalled_writes;

void chunked_provider_chunk_written(GObject *source, GAsyncResult *result, gpointer data);

// Start one asynchronous chunk write. The next chunk is scheduled only from the
// completion callback below, so the provider never performs a synchronous
// g_output_stream_write_all() on the main thread. A blocking write there
// deadlocks the local-clipboard read: GDK calls write_mime_type_async on the
// main thread, and the product's bounded reader (which drains the pipe) cannot
// run while that thread is captured (tests/r6-diag/chunked-case.sample.txt).
gboolean chunked_provider_write_next(gpointer data)
{
    auto *state = static_cast<ChunkedWriteState *>(data);
    gsize const remaining = state->payload.size() - state->offset;
    gsize const n = std::min(state->chunk_size, remaining);
    g_output_stream_write_all_async(state->stream, state->payload.data() + state->offset, n,
                                    G_PRIORITY_DEFAULT, g_task_get_cancellable(state->task),
                                    chunked_provider_chunk_written, state);
    return G_SOURCE_REMOVE;
}

void chunked_provider_chunk_written(GObject *source, GAsyncResult *result, gpointer data)
{
    auto *state = static_cast<ChunkedWriteState *>(data);
    gsize written = 0;
    GError *error = nullptr;
    bool const ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, &written, &error);
    if (!ok) {
        g_task_return_error(state->task, error);
        g_object_unref(state->stream);
        g_object_unref(state->task); // frees state through the task data
        return;
    }
    state->offset += written;
    if (state->offset >= state->payload.size()) {
        g_task_return_boolean(state->task, TRUE);
        g_object_unref(state->stream);
        g_object_unref(state->task); // frees state through the task data
        return;
    }
    if (state->stall_after_first) {
        g_stalled_writes.push_back(state); // task stays pending by design
        return;
    }
    // Next chunk only after this write completed, through a timer, so the state,
    // stream and task stay alive across the asynchronous write.
    g_timeout_add_full(G_PRIORITY_DEFAULT, state->delay_ms, chunked_provider_write_next, state, nullptr);
}

gboolean chunked_provider_write_finish(GdkContentProvider *, GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

void chunked_provider_write_async(GdkContentProvider *provider, char const *mime_type, GOutputStream *stream, int,
                                  GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    auto *self = reinterpret_cast<ChunkedProvider *>(provider);
    // Count every representation request so a test can prove one read per user
    // command (the two-pass paste must reuse one snapshot).
    GBytes *bytes = self->payload;
    if (self->mime2 && mime_type && g_strcmp0(mime_type, self->mime2) == 0) {
        ++self->requests2;
        bytes = self->payload2;
    } else {
        ++self->requests;
    }
    auto *state = new ChunkedWriteState();
    gsize size = 0;
    auto const *data = static_cast<char const *>(g_bytes_get_data(bytes, &size));
    state->payload.assign(data, size);
    state->chunk_size = self->chunk_size ? self->chunk_size : 16;
    state->delay_ms = self->delay_ms;
    state->stall_after_first = self->stall_after_first != FALSE;
    state->stream = G_OUTPUT_STREAM(g_object_ref(stream));
    state->task = g_task_new(provider, cancellable, callback, user_data);
    g_task_set_task_data(state->task, state, [](gpointer p) { delete static_cast<ChunkedWriteState *>(p); });
    chunked_provider_write_next(state);
}

void chunked_provider_class_init(ChunkedProviderClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = chunked_provider_finalize;
    auto *provider_class = GDK_CONTENT_PROVIDER_CLASS(klass);
    provider_class->ref_formats = chunked_provider_ref_formats;
    provider_class->write_mime_type_async = chunked_provider_write_async;
    provider_class->write_mime_type_finish = chunked_provider_write_finish;
}

GdkContentProvider *makeChunkedProvider(std::string const &payload, gsize chunk_size, guint delay_ms, bool stall,
                                        char const *mime = kNativeMime)
{
    auto *provider = static_cast<ChunkedProvider *>(g_object_new(chunked_provider_get_type(), nullptr));
    provider->payload = g_bytes_new(payload.data(), payload.size());
    provider->mime = g_strdup(mime ? mime : kNativeMime);
    provider->chunk_size = chunk_size;
    provider->delay_ms = delay_ms;
    provider->stall_after_first = stall ? TRUE : FALSE;
    return GDK_CONTENT_PROVIDER(provider);
}

// Two representations in one provider, with request counters: the fixture for
// "one snapshot per user command" (a malformed native payload plus a plain
// alternative makes the normal paste run the text policy twice).
GdkContentProvider *makeChunkedProviderMulti(std::string const &payload, char const *mime,
                                             std::string const &payload2, char const *mime2,
                                             gsize chunk_size, guint delay_ms, bool stall)
{
    auto *provider = static_cast<ChunkedProvider *>(g_object_new(chunked_provider_get_type(), nullptr));
    provider->payload = g_bytes_new(payload.data(), payload.size());
    provider->mime = g_strdup(mime ? mime : kNativeMime);
    provider->payload2 = g_bytes_new(payload2.data(), payload2.size());
    provider->mime2 = g_strdup(mime2);
    provider->chunk_size = chunk_size;
    provider->delay_ms = delay_ms;
    provider->stall_after_first = stall ? TRUE : FALSE;
    return GDK_CONTENT_PROVIDER(provider);
}

bool installClipboardProvider(GdkContentProvider *provider)
{
    auto clip = defaultClipboard();
    if (!clip) return false;
    publishClipboardAndNote([&] {
        gdk_clipboard_set_content(clip->gobj(), provider);
        g_object_unref(provider); // the clipboard holds its own reference
    }, 60);
    return true;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class TextPasteTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "Skipping GUI integration test: GUI testing not enabled";
        }
        // Live pasteboard use is OPT-IN. On macOS GdkClipboard IS NSPasteboard,
        // and this fixture cannot rebuild a non-text clipboard (image, files,
        // rich foreign formats), so an ordinary `ctest` run must not take the
        // user's clipboard at all. VACARDS_CLIP_ALLOW_LIVE=1 is the explicit
        // disposable-clipboard consent; without it every fixture test skips
        // BEFORE the first clipboard read. Ordinary CTest registration does not
        // set it (testfiles/CMakeLists.txt).
        auto const allow_live = std::getenv("VACARDS_CLIP_ALLOW_LIVE");
        if (!allow_live || std::string(allow_live) != "1") {
            GTEST_SKIP() << "Skipping live-clipboard test: set VACARDS_CLIP_ALLOW_LIVE=1 to let this "
                            "suite replace and restore the pasteboard (non-text content is not restorable)";
        }
        // Ownership token: counts GdkClipboard changed-signal emissions so
        // cleanup can prove the fixture still owns the clipboard. See
        // text-paste-clipboard-ownership.h. Started after the application (and
        // therefore the Gdk display) exists, before the first clipboard read.
        auto &application = testApplication();
        ASSERT_TRUE(application.gtk_app());
        ASSERT_TRUE(Application::exists());
        ensureClipboardChangeTracking();

        // Clipboard preservation (best effort). The pre-test clipboard is
        // snapshotted here and restored in TearDown ONLY when the ownership token
        // still matches the last publication the fixture made: a user copy made
        // during the run is newer and is never overwritten. Only text or this
        // application's native fragment can be reconstructed exactly; a non-text
        // clipboard (image, files, rich foreign formats) cannot be rebuilt from
        // here, which is why the takeover requires the explicit disposable-
        // clipboard opt-in above and is covered faithfully by the separate
        // publisher harness snapshot/restore.
        snapshotClipboardForPreservation();

        constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg"
              xmlns:xlink="http://www.w3.org/1999/xlink"
              xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
              width="480" height="360">
  <sodipodi:namedview id="namedview"/>
  <text id="src" x="10" y="40" transform="rotate(15)" style="font-family:serif;font-size:12px;fill:#112233;letter-spacing:1px;line-height:20px">Alpha e&#x301; &#x5e9;&#x5dc;&#x5d5;&#x5dd;<tspan id="src-bold" style="font-family:monospace;font-size:24px;font-weight:bold;font-style:italic;fill:#cc0000;letter-spacing:5px;font-variant-caps:small-caps">Beta</tspan><tspan id="src-line2" sodipodi:role="line" x="10" y="70" style="font-family:cursive;font-size:18px;text-anchor:middle">Gamma &#x1f600;</tspan></text>
  <text id="dst" x="120" y="220" style="font-family:sans-serif;font-size:16px;fill:#00aa00;letter-spacing:3px;line-height:22px;text-anchor:end">Hello<tspan id="dst-hot" style="font-size:30px;fill:#0000ff">World</tspan> tail</text>
  <text id="dst2" x="140" y="300" style="font-family:sans-serif;font-size:14px">Insert <tspan id="dst2-hot" style="font-weight:bold">here</tspan></text>
  <g id="scaled-src-group" transform="scale(2)"><text id="src-scaled" x="5" y="10" style="font-family:serif;font-size:12px;fill:#112233">HHHHHHHH</text></g>
  <g id="scaled-dst-group" transform="scale(0.5)"><text id="dst-scaled" x="80" y="100" style="font-family:serif;font-size:16px;fill:#224466">HHHHHHHH</text></g>
  <text id="uni-dst" x="140" y="180" style="font-family:sans-serif;font-size:15px;fill:#202020">&#x3a9;m&#xe9;g&#x3b1; <tspan id="uni-hot" style="font-weight:bold;fill:#800000">T&#x3b1;il</tspan> done</text>
  <g id="locked-group" sodipodi:insensitive="1"><text id="locked-text" style="font-family:serif;font-size:12px">Locked</text></g>
  <g id="hidden-group" style="display:none"><text id="hidden-text" style="font-family:serif;font-size:12px">Hidden</text></g>
  <text id="missing-font" x="10" y="330" style="font-family:VacNoSuchFont;font-size:13px">Missing</text>
  <g id="flow-group"><flowRoot id="flow" style="font-family:serif;font-size:12px"><flowRegion><rect id="flow-region" width="200" height="100"/></flowRegion><flowPara id="flow-para">Flow text</flowPara></flowRoot></g>
  <g id="text-group"><text id="group-text" style="font-family:serif;font-size:12px">Grouped</text></g>
  <rect id="rect" x="280" y="40" width="60" height="40" style="fill:#123456"/>
  <rect id="rect2" x="280" y="120" width="60" height="40" style="fill:#654321"/>
</svg>)SVG";

        auto owned = SPDocument::createNewDocFromMem(std::string(svg));
        ASSERT_TRUE(owned);
        document = application.document_add(std::move(owned));
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = application.createDesktop(document, false, true);
        ASSERT_TRUE(desktop);
        checkpoint();
    }

    void TearDown() override
    {
        restoreClipboardAfterPreservation();
        if (document) document->setModifiedSinceSave(false);
        if (desktop) testApplication().destroyDesktop(desktop);
        desktop = nullptr;
        document = nullptr;
    }

    // Snapshot the pre-test clipboard (only the forms this process can restore).
    void snapshotClipboardForPreservation()
    {
        auto clip = defaultClipboard();
        if (!clip) {
            return;
        }
        auto formats = clip->get_formats();
        if (!formats) {
            return;
        }
        if (formats->contain_mime_type(kNativeMime)) {
            clipboard_native_snapshot_ = readClipboardBytes(kNativeMime);
        }
        if (!clipboard_native_snapshot_) {
            for (char const *mime : {"text/plain;charset=utf-8", "text/plain"}) {
                if (!formats->contain_mime_type(mime)) {
                    continue;
                }
                if (auto bytes = readClipboardBytes(mime)) {
                    clipboard_text_snapshot_ = bytes;
                    break;
                }
            }
        }
    }

    // Restore the pre-test clipboard only while the fixture still owns it. A
    // foreign write during the run (the user copying something) moves the change
    // token, and the newer copy is left untouched. The native snapshot is
    // republished atomically with its plain alternative; a text-only snapshot is
    // restored as text. A clipboard this process cannot reconstruct was never
    // allowed to be taken without the disposable-clipboard opt-in.
    void restoreClipboardAfterPreservation()
    {
        bool const has_snapshot = clipboard_native_snapshot_.has_value() || clipboard_text_snapshot_.has_value();
        if (!has_snapshot) {
            return;
        }
        // Deliver pending change notifications before the ownership decision, so
        // a user copy made at the very end of the test is observed. Two probes:
        // GdkClipboard::is_local() (a foreign write re-claims the pasteboard and
        // clears it) and the changed-signal token.
        pumpFor(30);
        auto clip = defaultClipboard();
        bool const clipboard_is_local = clip && clip->is_local();
        auto const decision = TextPasteTestSupport::decide_clipboard_restore(
            has_snapshot, g_publication_ledger.tracking_available(), clipboard_is_local,
            g_clipboard_change_events, g_publication_ledger.own_publish_token());
        if (decision != TextPasteTestSupport::RestoreDecision::restore) {
            clipboard_native_snapshot_.reset();
            clipboard_text_snapshot_.reset();
            return;
        }
        if (!clip) {
            return;
        }
        if (clipboard_native_snapshot_) {
            setRichClipboard(*clipboard_native_snapshot_, clipboard_text_snapshot_);
        } else {
            // The restore is itself a fixture publication: route it through the
            // same helper so the ownership token keeps describing the fixture's
            // newest write for the next test's cleanup decision.
            setPlainClipboard(*clipboard_text_snapshot_);
        }
        clipboard_native_snapshot_.reset();
        clipboard_text_snapshot_.reset();
    }

    SPText *text(char const *id) const
    {
        auto *result = cast<SPText>(document->getObjectById(id));
        EXPECT_TRUE(result) << id;
        return result;
    }

    SPItem *item(char const *id) const { return cast<SPItem>(document->getObjectById(id)); }

    void checkpoint()
    {
        document->ensureUpToDate();
        DocumentUndo::done(document, Util::Internal::ContextString("Prepare paste fixture"), "draw-text");
        DocumentUndo::clearUndo(document);
        DocumentUndo::clearRedo(document);
        document->setModifiedSinceSave(false);
    }

    std::string xml() const { return sp_repr_save_buf(document->getReprDoc()).raw(); }

    std::set<std::string> objectIds() const
    {
        std::set<std::string> out;
        std::function<void(XML::Node *)> walk = [&](XML::Node *node) {
            if (auto id = node->attribute("id")) out.insert(id);
            for (auto child = node->firstChild(); child; child = child->next()) walk(child);
        };
        walk(document->getReprDoc()->root());
        return out;
    }

    std::vector<SPText *> textsNotIn(std::set<std::string> const &before) const
    {
        std::vector<SPText *> out;
        for (auto const &id : objectIds()) {
            if (before.count(id)) continue;
            if (auto *text_object = cast<SPText>(document->getObjectById(id))) out.push_back(text_object);
        }
        return out;
    }

    // The GUI test window can deliver an incidental enter/motion during SetUp
    // (headless pointer), leaving a last_mouse that the documented no-motion
    // pointer fallback must not see. Emit the real motion controller's "leave"
    // signal - the same path taken when the pointer leaves the canvas - and
    // wait until the canvas reports no last mouse, so SPDesktop::point() uses
    // its documented canvas-center fallback deterministically.
    void clearCanvasPointer(UI::Widget::Canvas *canvas)
    {
        auto controllers = gtk_widget_observe_controllers(GTK_WIDGET(canvas->gobj()));
        unsigned emitted = 0;
        for (unsigned i = 0; i < g_list_model_get_n_items(controllers); ++i) {
            auto controller = G_OBJECT(g_list_model_get_item(controllers, i));
            if (GTK_IS_EVENT_CONTROLLER_MOTION(controller)) {
                g_signal_emit_by_name(controller, "leave");
                ++emitted;
            }
            g_object_unref(controller);
        }
        g_object_unref(controllers);
        ASSERT_GT(emitted, 0u) << "the canvas must own a motion controller for the leave fixture";
        ASSERT_TRUE(pumpUntil([&] { return !canvas->get_last_mouse().has_value(); }))
            << "the pointer-leave fixture must clear the canvas last mouse";
    }

    UI::Tools::TextTool *textTool()
    {
        auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
        EXPECT_TRUE(tool);
        return tool;
    }

    void copyRange(SPItem *source, unsigned from, unsigned to)
    {
        desktop->getSelection()->set(source);
        desktop->setTool("/tools/text");
        auto *tool = textTool();
        ASSERT_TRUE(tool);
        tool->text_sel_start = iteratorAt(source, from);
        tool->text_sel_end = iteratorAt(source, to);
        UI::ClipboardManager::get()->copy(desktop->getSelection());
        pumpFor(150);
    }

    // Copy the mixed-style binary run "Beta" from the fixture source text.
    void copyBeta()
    {
        auto *source = text("src");
        unsigned const beta_start = logicalIndexOf(source, "Beta");
        ASSERT_NE(beta_start, std::numeric_limits<unsigned>::max());
        copyRange(source, beta_start, beta_start + 4);
    }

    bool pasteInside(SPItem *destination, unsigned cursor, UI::TextPasteMode mode)
    {
        if (!placeTextCursor(destination, cursor)) return false;
        bool const changed = UI::ClipboardManager::get()->pasteText(desktop, mode);
        document->ensureUpToDate();
        return changed;
    }

    bool placeTextCursor(SPItem *destination, unsigned cursor)
    {
        desktop->getSelection()->set(destination);
        desktop->setTool("/tools/text");
        auto *tool = textTool();
        if (!tool) return false;
        tool->text_sel_start = iteratorAt(destination, cursor);
        tool->text_sel_end = tool->text_sel_start;
        return true;
    }

    InkscapeWindow *inkscapeWindow()
    {
        auto *window = desktop ? desktop->getInkscapeWindow() : nullptr;
        EXPECT_TRUE(window);
        return window;
    }

    // Activate the real window paste action (the action itself commits the
    // single Undo step; pasteText is documented commit-free).
    bool activatePasteAction(char const *action)
    {
        auto *window = inkscapeWindow();
        if (!window) return false;
        // Gtk::Widget::activate_action() takes a *detailed* name prefixed with the
        // action-group prefix ("win.paste"); gtk_widget_activate_action_variant
        // rejects unprefixed names. Inkscape registers these actions on the window
        // under the "win" group (actions-edit-window.cpp, shortcuts.cpp:312).
        bool const activated = window->activate_action(std::string("win.") + action);
        pumpFor(80);
        if (document) document->ensureUpToDate();
        return activated;
    }

    bool pasteReplacing(SPItem *destination, unsigned from, unsigned to, UI::TextPasteMode mode)
    {
        desktop->getSelection()->set(destination);
        desktop->setTool("/tools/text");
        auto *tool = textTool();
        if (!tool) return false;
        tool->text_sel_start = iteratorAt(destination, from);
        tool->text_sel_end = iteratorAt(destination, to);
        bool const changed = UI::ClipboardManager::get()->pasteText(desktop, mode);
        document->ensureUpToDate();
        return changed;
    }

    // Backward (right-to-left) selection: start iterator is logically after end.
    bool pasteReplacingBackward(SPItem *destination, unsigned from, unsigned to, UI::TextPasteMode mode)
    {
        desktop->getSelection()->set(destination);
        desktop->setTool("/tools/text");
        auto *tool = textTool();
        if (!tool) return false;
        tool->text_sel_start = iteratorAt(destination, to);
        tool->text_sel_end = iteratorAt(destination, from);
        bool const changed = UI::ClipboardManager::get()->pasteText(desktop, mode);
        document->ensureUpToDate();
        return changed;
    }

    bool pasteOutside(UI::TextPasteMode mode, char const *selected = "rect")
    {
        desktop->getSelection()->set(item(selected));
        desktop->setTool("/tools/select");
        bool const changed = UI::ClipboardManager::get()->pasteText(desktop, mode);
        document->ensureUpToDate();
        return changed;
    }

    // Plain fallback insertion inside an existing text is exercised through
    // pasteInside(destination, cursor, UI::TextPasteMode::Automatic).

    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    /// Pre-test clipboard forms restored in TearDown (best effort; see SetUp).
    std::optional<std::string> clipboard_native_snapshot_;
    std::optional<std::string> clipboard_text_snapshot_;
};

// ---------------------------------------------------------------------------
// Clipboard-free native copy/paste helpers and rendered-size oracles
// ---------------------------------------------------------------------------

std::set<std::string> objectIdsInDoc(SPDocument *doc)
{
    std::set<std::string> out;
    std::function<void(XML::Node *)> walk = [&](XML::Node *node) {
        if (auto id = node->attribute("id")) out.insert(id);
        for (auto child = node->firstChild(); child; child = child->next()) walk(child);
    };
    walk(doc->getReprDoc()->root());
    return out;
}

std::vector<SPText *> textsInDocNot(SPDocument *doc, std::set<std::string> const &before)
{
    std::vector<SPText *> out;
    for (auto const &id : objectIdsInDoc(doc)) {
        if (before.count(id)) continue;
        if (auto *text_object = cast<SPText>(doc->getObjectById(id))) out.push_back(text_object);
    }
    return out;
}

// The size the user actually sees: the glyph ink bounding box (same repeated
// glyph, so height is proportional to the font size) expressed in document
// coordinates by applying the item's own item-to-document affine. This is the
// real rendered size and it is independent of whether the size number lives on
// the root, on a tspan or on an ancestor group's transform.
double renderedHeightInDoc(SPItem *item)
{
    item->document->ensureUpToDate();
    auto const bounds = item->geometricBounds(item->i2doc_affine());
    EXPECT_TRUE(bounds) << "fixture text must have a computable glyph bounding box";
    return bounds ? bounds->height() : -1.0;
}

// Copy side without the system clipboard: the Text tool's own selection
// extraction, exactly the fragment a native copy publishes.
std::optional<TP::Fragment> extractFragment(SPDesktop *desk, SPText *source, unsigned from, unsigned to)
{
    desk->getSelection()->set(source);
    desk->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desk->getTool());
    if (!tool) return std::nullopt;
    tool->text_sel_start = iteratorAt(source, from);
    tool->text_sel_end = iteratorAt(source, to);
    return tool->extractSelectionFragment();
}

// Paste side without the system clipboard. With the Text tool active and no
// edited text item this is the real outside path for a native payload
// (clipboard.cpp:2086-2094 routes native payloads to TextTool::pasteFragment,
// which creates the object itself when text==nullptr).
bool pasteOutsideFragment(SPDesktop *desk, SPDocument *doc, TP::Fragment const &fragment)
{
    desk->getSelection()->clear();
    desk->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desk->getTool());
    if (!tool || tool->textItem() != nullptr) return false;
    bool const changed = tool->pasteFragment(fragment, UI::TextPasteMode::Automatic);
    doc->ensureUpToDate();
    return changed;
}

bool pasteInsideFragment(SPDesktop *desk, SPDocument *doc, SPItem *destination, unsigned cursor,
                         TP::Fragment const &fragment)
{
    desk->getSelection()->set(destination);
    desk->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desk->getTool());
    if (!tool || tool->textItem() != destination) return false;
    tool->text_sel_start = iteratorAt(destination, cursor);
    tool->text_sel_end = tool->text_sel_start;
    bool const changed = tool->pasteFragment(fragment, UI::TextPasteMode::Automatic);
    doc->ensureUpToDate();
    return changed;
}

// ---------------------------------------------------------------------------
// Copy side
// ---------------------------------------------------------------------------
TEST_F(TextPasteTest, CopyPublishesRichAndPlainAtomicallyWithoutMutatingDocument)
{
    auto *source = text("src");
    unsigned const length = sp_text_get_length(source);
    auto const xml_before = xml();
    ASSERT_FALSE(DocumentUndo::undo(document)) << "fixture checkpoint must clear history";

    copyRange(source, 0, length);

    EXPECT_EQ(xml(), xml_before) << "copy must not mutate the XML tree";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "copy must not create an undo entry";

    auto mimes = clipboardMimeTypes();
    EXPECT_NE(std::find(mimes.begin(), mimes.end(), kNativeMime), mimes.end())
        << "rich native format must be published; offered: " << [&] {
               std::string joined;
               for (auto const &m : mimes) joined += m + " ";
               return joined;
           }();
    EXPECT_NE(std::find(mimes.begin(), mimes.end(), "text/plain"), mimes.end());

    auto payload = readClipboardBytes(kNativeMime);
    ASSERT_TRUE(payload.has_value());
    EXPECT_FALSE(payload->empty());

    auto plain = readClipboardBytes("text/plain");
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(*plain, sp_te_get_string_multiline(source, iteratorAt(source, 0), iteratorAt(source, length)));
}

// ---------------------------------------------------------------------------
// Automatic paste outside an existing text object
// ---------------------------------------------------------------------------
TEST_F(TextPasteTest, AutomaticOutsideCreatesNewTextWithSourceRunsUnicodeAndNoGeometry)
{
    auto *source = text("src");
    unsigned const length = sp_text_get_length(source);
    auto const expected_text = sp_te_get_string_multiline(source, iteratorAt(source, 0), iteratorAt(source, length));
    auto const expected_chars = characterSignatures(source);
    auto const expected_paras = paragraphSignatures(source);

    copyRange(source, 0, length);

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    document->ensureUpToDate();

    EXPECT_EQ(multilineText(pasted), expected_text) << "logical characters/newlines must be preserved";
    EXPECT_EQ(characterSignatures(pasted), expected_chars)
        << "mixed source character runs must be preserved on newly created text";
    EXPECT_EQ(paragraphSignatures(pasted), expected_paras)
        << "source paragraph format must be preserved on newly created text";

    auto repr = pasted->getRepr();
    EXPECT_FALSE(repr->attribute("transform")) << "no copied rotation/transform";
    EXPECT_FALSE(repr->attribute("inline-size"));
    EXPECT_FALSE(repr->attribute("shape-inside"));
    EXPECT_FALSE(containsSubstring(subtreeXml(pasted), "textPath"));
    double const x = repr->getAttributeDouble("x", -9999);
    double const y = repr->getAttributeDouble("y", -9999);
    EXPECT_FALSE(std::abs(x - 10) < 0.001 && std::abs(y - 40) < 0.001)
        << "copied source box position must not be reused";
    if (x > -9999 && y > -9999) {
        EXPECT_GE(x, 0.0);
        EXPECT_GE(y, 0.0);
        EXPECT_LE(x, 480.0) << "placement should be useful/on canvas";
        EXPECT_LE(y, 360.0) << "placement should be useful/on canvas";
    }
}

TEST_F(TextPasteTest, AutomaticOutsidePreservesRequestedMissingFontFamily)
{
    document->ensureUpToDate();
    auto *source = text("missing-font");
    ASSERT_EQ(multilineText(source), "Missing");
    copyRange(source, 0, sp_text_get_length(source));

    auto const expected = characterSignatures(source);
    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(characterSignatures(created.front()), expected)
        << "the requested font family must survive when it is not installed";
    EXPECT_TRUE(containsSubstring(characterSignature(created.front(), iteratorAt(created.front(), 0)),
                                 "fam=VacNoSuchFont"));
}

// ---------------------------------------------------------------------------
// Automatic paste inside an existing text object
// ---------------------------------------------------------------------------
TEST_F(TextPasteTest, AutomaticWithTextToolNascentBlankCanvasClickKeepsSourceFormat)
{
    auto *source = text("src");
    copyRange(source, 0, sp_text_get_length(source));
    auto const expected_chars = characterSignatures(source);
    auto const expected_paras = paragraphSignatures(source);
    auto const src_before = subtreeXml(source);
    auto const dst_before = subtreeXml(text("dst"));

    // Real blank-canvas click with the Text tool: no textItem yet (nascent).
    desktop->getSelection()->clear();
    desktop->setTool("/tools/text");
    auto *tool = textTool();
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool->textItem(), nullptr);

    auto *base = static_cast<UI::Tools::ToolBase *>(tool);
    Geom::Point const click(400, 250); // canvas-widget coordinates
    ButtonPressEvent press;
    press.pos = press.orig_pos = click;
    press.button = 1;
    press.num_press = 1;
    base->root_handler(press);
    ButtonReleaseEvent release;
    release.pos = release.orig_pos = click;
    release.button = 1;
    base->root_handler(release);
    EXPECT_EQ(tool->textItem(), nullptr) << "a blank-canvas click alone must not create a text object";

    auto const before = objectIds();
    ASSERT_TRUE(UI::ClipboardManager::get()->pasteText(desktop, UI::TextPasteMode::Automatic))
        << "a nascent canvas text context must still accept a text paste";
    document->ensureUpToDate();
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u) << "nascent blank-canvas context is outside existing text";
    auto *pasted = created.front();
    EXPECT_EQ(characterSignatures(pasted), expected_chars)
        << "Automatic outside must keep source run format for a nascent canvas text";
    EXPECT_EQ(paragraphSignatures(pasted), expected_paras)
        << "Automatic outside must keep source paragraph format";
    EXPECT_EQ(subtreeXml(source), src_before) << "source text must not be edited";
    EXPECT_EQ(subtreeXml(text("dst")), dst_before) << "unrelated destination text must not be edited";
    // Canvas-coordinate oracle (source evidence): the Text tool records the click
    // as pdoc = desktop->dt2doc(desktop->w2d(event.pos)) (text-tool.cpp:655-657),
    // and create_text_at_position() writes x/y in the parent layer's user space:
    // attr = pdoc * parent->i2doc_affine().inverse() (sp-text.cpp:1372-1374).
    // Both sides are therefore compared in *document* coordinates; raw widget
    // pixels must never be compared with the stored x/y attributes.
    Geom::Point const click_doc = desktop->dt2doc(desktop->w2d(click));
    double const x = pasted->getRepr()->getAttributeDouble("x", -9999);
    double const y = pasted->getRepr()->getAttributeDouble("y", -9999);
    ASSERT_GT(x, -9999) << "new text must have an explicit x position";
    ASSERT_GT(y, -9999) << "new text must have an explicit y position";
    auto const *layer = cast<SPItem>(pasted->parent);
    ASSERT_TRUE(layer) << "new text must have a parent layer";
    Geom::Point const placed_doc = Geom::Point(x, y) * layer->i2doc_affine();
    EXPECT_LT(Geom::L2(placed_doc - click_doc), 60.0)
        << "new text should use the clicked canvas position in document coordinates";
}

TEST_F(TextPasteTest, AutomaticInsideInsertAdoptsDestinationTypingStyle)
{
    auto *source = text("src");
    unsigned const source_len = sp_text_get_length(source);
    copyRange(source, 0, source_len);

    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const destination_chars_before = characterSignatures(destination);
    auto const destination_paras_before = paragraphSignatures(destination);
    std::string const destination_before = multilineText(destination);

    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();

    std::string const after = multilineText(destination);
    auto const inserted_at = after.find("Alpha");
    ASSERT_NE(inserted_at, std::string::npos);
    // Locate the surviving destination suffix in the layout by content. The
    // inserted fragment contains a paragraph break, which is not necessarily one
    // indexed layout position, so string lengths must not shift layout indices.
    unsigned const world_at = logicalIndexOf(destination, "World");
    ASSERT_NE(world_at, std::numeric_limits<unsigned>::max());
    unsigned const suffix_at = world_at - 2; // the surviving "lo" before "World"
    unsigned const inserted_layout = suffix_at - static_cast<unsigned>(charIndexOfByte(after, inserted_at));
    ASSERT_GT(inserted_layout, 0u);
    for (unsigned i = 0; i < inserted_layout; ++i) {
        unsigned const index = static_cast<unsigned>(charIndexOfByte(after, inserted_at)) + i;
        EXPECT_EQ(characterSignature(destination, iteratorAt(destination, index)),
                  destination_chars_before[3])
            << "inserted character " << i << " must adopt the destination typing style";
    }
    // Paragraph properties of pre-existing characters must not change; the
    // inserted multi-paragraph fragment may add its own character positions.
    expectPreservedParagraphSignatures(destination, destination_paras_before, 3, 0, suffix_at);

    std::string const expected = destination_before.substr(0, 3) +
                                 sp_te_get_string_multiline(source, iteratorAt(source, 0), iteratorAt(source, source_len)) +
                                 destination_before.substr(3);
    EXPECT_EQ(after, expected) << "surrounding destination content must be preserved";

    // Adjacent (unrelated) destination characters keep their exact styles.
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_EQ(characterSignature(destination, iteratorAt(destination, i)), destination_chars_before[i])
            << "character " << i << " before the insertion must be unchanged";
    }
    auto const chars_after = characterSignatures(destination);
    for (unsigned i = 0; suffix_at + i < chars_after.size() && 3 + i < destination_chars_before.size(); ++i) {
        EXPECT_EQ(characterSignature(destination, iteratorAt(destination, suffix_at + i)),
                  destination_chars_before[3 + i])
            << "character after the insertion must be unchanged";
    }
}

TEST_F(TextPasteTest, AutomaticInsideReplacementUsesStyleAtFirstLogicalPosition)
{
    auto *source = text("src");
    copyRange(source, 0, sp_text_get_length(source));
    auto *destination = text("dst");
    document->ensureUpToDate();

    // "HelloWorld tail": replace [5,14) ("World tail") whose first logical
    // position carries the blue 30px run style.
    auto const hot_style = characterSignature(destination, iteratorAt(destination, 5));
    auto const base_style = characterSignature(destination, iteratorAt(destination, 0));
    EXPECT_NE(hot_style, base_style) << "fixture must have distinct run styles";
    auto const paragraphs_before = paragraphSignatures(destination);

    ASSERT_TRUE(pasteReplacing(destination, 5, 14, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();

    std::string const after = multilineText(destination);
    ASSERT_EQ(after.substr(0, 5), "Hello");
    auto const inserted_at = 5u;
    EXPECT_GT(after.size(), inserted_at);
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_EQ(characterSignature(destination, iteratorAt(destination, inserted_at + i)), hot_style)
            << "replacement paste must use the style at the start of the replaced selection";
    }
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, 0)), base_style)
        << "unrelated preceding content must keep its style";
    // Pre-existing characters keep their paragraph properties (the replaced
    // range is gone; surviving prefix indices are stable).
    for (unsigned i = 0; i < 5; ++i) {
        EXPECT_EQ(paragraphSignature(destination, iteratorAt(destination, i)), paragraphs_before[i])
            << "destination paragraph properties must not change on replacement";
    }
}

// Multi-byte destination: replacement/insertion offsets must be logical
// Unicode code points, not byte offsets (R4).
TEST_F(TextPasteTest, UnicodeDestinationOffsetsKeepSurroundingCharacters)
{
    copyBeta();
    auto *destination = text("uni-dst");
    document->ensureUpToDate();
    std::string const before = multilineText(destination);
    ASSERT_EQ(before, "\u03a9m\u00e9g\u03b1 T\u03b1il done");
    auto const hot_style = characterSignature(destination, iteratorAt(destination, 6));
    auto const base_style = characterSignature(destination, iteratorAt(destination, 0));
    EXPECT_NE(hot_style, base_style);

    // "Tαil" occupies logical characters [6,10).
    ASSERT_TRUE(pasteReplacing(destination, 6, 10, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();
    std::string const after = multilineText(destination);
    auto const inserted_byte = after.find("Beta");
    ASSERT_NE(inserted_byte, std::string::npos);
    unsigned const inserted_at = charIndexOfByte(after, inserted_byte);
    EXPECT_EQ(inserted_at, 6u) << "insertion index must be counted in code points";
    EXPECT_EQ(after.substr(0, std::string("\u03a9m\u00e9g\u03b1 ").size()), "\u03a9m\u00e9g\u03b1 ")
        << "multi-byte prefix must be preserved";
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, inserted_at)), hot_style)
        << "style at the first logical position of the replaced range must be used";
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, 0)), base_style);
    EXPECT_TRUE(containsSubstring(after, " done")) << "multi-byte suffix must be preserved";
}

TEST_F(TextPasteTest, AutomaticBackwardSelectionUsesStyleAtFirstLogicalPosition)
{
    copyBeta();
    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const hot_style = characterSignature(destination, iteratorAt(destination, 5));
    auto const base_style = characterSignature(destination, iteratorAt(destination, 0));
    auto const paragraphs_before = paragraphSignatures(destination);

    // Right-to-left selection over [5,14): the style source is logical position 5.
    ASSERT_TRUE(pasteReplacingBackward(destination, 5, 14, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();

    std::string const after = multilineText(destination);
    ASSERT_EQ(after.substr(0, 5), "Hello") << "backward selection must still be replaced in logical order";
    ASSERT_TRUE(containsSubstring(after, "Beta"));
    auto const inserted_at = after.find("Beta");
    ASSERT_EQ(inserted_at, 5u);
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, 5)), hot_style)
        << "backward selection must use the style at the first logical position";
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, 0)), base_style);
    // [5,14) was replaced by 4 characters: only surviving characters are compared,
    // and the surviving suffix is located by the inserted content.
    unsigned const beta_at = logicalIndexOf(destination, "Beta");
    ASSERT_NE(beta_at, std::numeric_limits<unsigned>::max());
    expectPreservedParagraphSignatures(destination, paragraphs_before, 5, 9, beta_at + 4);
}

TEST_F(TextPasteTest, ExplicitSourceInsidePreservesCopiedMixedRuns)
{
    auto *source = text("src");
    // Copy only the mixed-style binary run "Beta".
    unsigned const beta_start = logicalIndexOf(source, "Beta");
    ASSERT_NE(beta_start, std::numeric_limits<unsigned>::max());
    copyRange(source, beta_start, beta_start + 4);
    auto const source_chars = characterSignatures(source);

    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const paragraphs_before = paragraphSignatures(destination);
    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Source));
    document->ensureUpToDate();

    std::string const after = multilineText(destination);
    auto const inserted_at = after.find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_EQ(characterSignature(destination, iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(after, inserted_at)) + i)),
                  source_chars[beta_start + i])
            << "explicit Source must keep copied mixed character format inside existing text";
    }
    // Four characters were inserted at logical position 3; locate the surviving
    // suffix in the layout by content before comparing paragraph properties.
    unsigned const world_at = logicalIndexOf(destination, "World");
    ASSERT_NE(world_at, std::numeric_limits<unsigned>::max());
    expectPreservedParagraphSignatures(destination, paragraphs_before, 3, 0, world_at - 2);
}

TEST_F(TextPasteTest, ExplicitDestinationStripsSourceFormatting)
{
    auto *source = text("src");
    unsigned const beta_start = logicalIndexOf(source, "Beta");
    ASSERT_NE(beta_start, std::numeric_limits<unsigned>::max());
    copyRange(source, beta_start, beta_start + 4); // monospace 24px bold italic small-caps red
    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const destination_style = characterSignature(destination, iteratorAt(destination, 3));
    EXPECT_TRUE(containsSubstring(destination_style, "fam=sans-serif"));

    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Destination));
    document->ensureUpToDate();

    std::string const after = multilineText(destination);
    auto const inserted_at = after.find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    for (unsigned i = 0; i < 4; ++i) {
        auto const signature = characterSignature(destination, iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(after, inserted_at)) + i));
        EXPECT_EQ(signature, destination_style)
            << "explicit Destination must use the destination/default text tool style";
        EXPECT_TRUE(containsSubstring(signature, "fam=sans-serif"));
    }
}

TEST_F(TextPasteTest, NoSourceParagraphFormatLeaksOnInlinePaste)
{
    auto *source = text("src");
    // Second source paragraph carries text-anchor:middle and a cursive face.
    unsigned const gamma_start = logicalIndexOf(source, "Gamma");
    ASSERT_NE(gamma_start, std::numeric_limits<unsigned>::max());
    copyRange(source, gamma_start, sp_text_get_length(source));
    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const destination_paras_before = paragraphSignatures(destination);
    auto const destination_chars_before = characterSignatures(destination);

    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Source));
    document->ensureUpToDate();

    // The copied second paragraph was inserted at logical position 3; locate the
    // surviving destination suffix by content in the layout (a paragraph break is
    // not necessarily one indexed layout position).
    unsigned const world_at = logicalIndexOf(destination, "World");
    ASSERT_NE(world_at, std::numeric_limits<unsigned>::max());
    expectPreservedParagraphSignatures(destination, destination_paras_before, 3, 0, world_at - 2);
    auto const chars_after = characterSignatures(destination);
    ASSERT_GT(chars_after.size(), destination_chars_before.size());
    unsigned const gamma_at = logicalIndexOf(destination, "Gamma");
    ASSERT_NE(gamma_at, std::numeric_limits<unsigned>::max()) << "copied text must be present";
    EXPECT_TRUE(containsSubstring(characterSignature(destination, iteratorAt(destination, gamma_at)), "fam=cursive"))
        << "ExplicitSource still keeps copied character format";
}

// ---------------------------------------------------------------------------
// Plain text interoperability / fallbacks
// ---------------------------------------------------------------------------
TEST_F(TextPasteTest, PlainClipboardOutsideUsesTextToolDefaultsAndUsefulPlacement)
{
    setPlainClipboard("plain external");
    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u) << "plain paste outside must create editable text";
    auto *pasted = created.front();
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(pasted), "plain external");

    auto repr = pasted->getRepr();
    auto const *style = sp_te_style_at_position(pasted, iteratorAt(pasted, 0));
    ASSERT_TRUE(style);
    ASSERT_TRUE(style->font_family.value());
    EXPECT_GT(std::string(style->font_family.value()).size(), 0u);
    EXPECT_GT(style->font_size.computed, 0.0);
    EXPECT_FALSE(repr->attribute("transform"));
    double const x = repr->getAttributeDouble("x", -9999);
    double const y = repr->getAttributeDouble("y", -9999);
    if (x > -9999 && y > -9999) {
        EXPECT_GE(x, 0.0);
        EXPECT_GE(y, 0.0);
        EXPECT_LE(x, 480.0);
        EXPECT_LE(y, 360.0);
    }
}

// Focused placement case for core/acceptance-review.json F-A /
// core/placement-ready.json: outside an active Text tool a plain paste must
// place the new text at dt2doc(desktop pointer), not at the desktop pointer
// itself. No motion event is synthesised: the canvas keeps no last mouse, and
// SPDesktop::point() (desktop.cpp) then uses its documented fallback, the center
// of the canvas viewpoint. A y-axis-up document makes dt2doc() a non-identity
// mirror (doc_y = document_height - desktop_y), so storing the raw desktop
// point lands at the mirrored position.
//
// The viewport center is sampled from the actual fallback (desktop->point()),
// never assumed from a requested pan. The pan is applied through the public
// SPDesktop::scroll_absolute() API, which keeps the canvas scroll and the
// desktop affine offset consistent so the paste pump cannot discard it; the
// pan-to-pointer scale is measured with one probe pan and the pointer is
// sampled after the final pan. The expected document point is derived from the
// sampled desktop point with the documented mirror
// doc_y = document_height - desktop_y.
TEST_F(TextPasteTest, PlainClipboardOutsideYAxisUpPlacesAtPointerFallbackViewportCenter)
{
    auto *namedview = document->getNamedView();
    ASSERT_TRUE(namedview);
    namedview->set_y_axis_down(false);
    for (unsigned pass = 0; pass < 4; ++pass) {
        document->ensureUpToDate();
        if (!document->yaxisdown()) break;
    }
    ASSERT_FALSE(document->yaxisdown()) << "the public NamedView toggle must reach dt2doc()";

    auto *canvas = desktop->getCanvas();
    ASSERT_TRUE(canvas);
    document->ensureUpToDate();
    checkpoint();

    setPlainClipboard("yaxis-up plain paste placement point");
    pumpFor(60);
    // The test window shares the user's display: isolate the canvas motion
    // controller for the whole critical section so a real pointer enter/motion
    // cannot replace the documented fallback, then clear any event that was
    // delivered before the isolation. The guard restores the phase on exit.
    PropagationPhaseGuard pointer_isolation;
    isolateCanvasPointer(canvas, pointer_isolation);
    ASSERT_FALSE(pointer_isolation.entries.empty()) << "the canvas must own a motion controller";
    clearCanvasPointer(canvas);

    ASSERT_FALSE(canvas->get_last_mouse().has_value())
        << "the case must exercise the documented pointer fallback, not a motion event";

    // Perform the paste flow's own view changes first, then pan as the LAST view
    // change before the paste and sample immediately: the headless GUI resets
    // bare canvas scrolling during GUI pumping, while the public desktop scroll
    // API keeps the canvas scroll and the desktop affine offset consistent, so
    // the pan survives to the product's pointer read.
    desktop->getSelection()->set(item("rect"));
    desktop->setTool("/tools/select");

    // The requested pan is not an oracle: its pan-to-pointer scale is measured
    // with one probe pan, then the pointer is sampled after the final pan.
    auto const page = document->getViewBox();
    Geom::IntPoint const scroll0 = canvas->get_pos();
    Geom::Point const pointer0 = desktop->point();
    double const probe_scroll_y = scroll0.y() - 10.0;
    desktop->scroll_absolute(Geom::Point(scroll0.x(), probe_scroll_y));
    Geom::Point const pointer_probe = desktop->point();
    double const pointer_per_scroll = (pointer_probe.y() - pointer0.y()) / (probe_scroll_y - scroll0.y());
    ASSERT_GT(std::abs(pointer_per_scroll), 0.1)
        << "the public desktop scroll API must move the fallback pointer";
    double const target_y = page.height() / 4.0; // off the height/2 mirror axis
    double const scroll_y = scroll0.y() + (target_y - pointer0.y()) / pointer_per_scroll;
    desktop->scroll_absolute(Geom::Point(scroll0.x(), scroll_y));
    Geom::Point const pointer_desktop = desktop->point();

    // Independent document-space oracle for the actual sampled desktop point:
    // for the y-axis-up document the vertical desktop axis is mirrored, so the
    // expected document y is document_height - desktop_y. This is not computed
    // through desktop->dt2doc().
    Geom::Point const expected_doc(pointer_desktop.x(), page.height() - pointer_desktop.y());
    EXPECT_TRUE(page.contains(expected_doc))
        << "pointer_desktop=" << pointer_desktop
        << ": the placement point must stay on the page so no midpoint fallback is used";
    EXPECT_GT(std::abs(expected_doc.y() - pointer_desktop.y()), 1.0)
        << "pointer_desktop=" << pointer_desktop
        << ": the case must distinguish document from desktop coordinates";
    EXPECT_GT(std::abs(pointer_desktop.y() - page.height() / 2.0), 1.0)
        << "pointer_desktop=" << pointer_desktop
        << ": the sampled fallback must be away from the identity midline or the "
           "raw-vs-converted discrimination is vacuous";
    // The product's dt2doc() must agree with the documented y-axis-up mirror.
    Geom::Point const dt2doc_pointer = desktop->dt2doc(pointer_desktop);
    EXPECT_NEAR(dt2doc_pointer.x(), expected_doc.x(), 0.01);
    EXPECT_NEAR(dt2doc_pointer.y(), expected_doc.y(), 0.01)
        << "dt2doc() must apply the y-axis-up document mirror";

    ASSERT_FALSE(canvas->get_last_mouse().has_value())
        << "no motion event may appear between the fixture clear and the paste, "
           "or the sampled placement would not be the documented fallback";
    auto const before = objectIds();
    ASSERT_TRUE(UI::ClipboardManager::get()->pasteText(desktop, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u) << "plain paste outside the text tool must create editable text";
    auto *pasted = created.front();
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(pasted), "yaxis-up plain paste placement point");

    auto repr = pasted->getRepr();
    EXPECT_FALSE(repr->attribute("transform")) << "copied source geometry must not be reused";
    double const x = repr->getAttributeDouble("x", -9999);
    double const y = repr->getAttributeDouble("y", -9999);
    ASSERT_GT(x, -9999) << "new text must have an explicit x position";
    ASSERT_GT(y, -9999) << "new text must have an explicit y position";
    auto const *pasted_layer = cast<SPItem>(pasted->parent);
    ASSERT_TRUE(pasted_layer);
    Geom::Point const placed_doc = Geom::Point(x, y) * pasted_layer->i2doc_affine();
    EXPECT_NEAR(placed_doc.x(), expected_doc.x(), 0.01) << "expected document x under the pointer";
    EXPECT_NEAR(placed_doc.y(), expected_doc.y(), 0.01)
        << "plain paste must convert the desktop pointer to document units (dt2doc)";
    EXPECT_GT(std::abs(placed_doc.y() - pointer_desktop.y()), 1.0)
        << "a raw desktop point must not be stored as the document position";
    if (std::getenv("TP_DIAG_DIR")) {
        auto const last_mouse_after = canvas->get_last_mouse();
        Geom::Point const point_after = desktop->point();
        char buffer[384];
        g_snprintf(buffer, sizeof buffer,
                   "sampled_pointer=(%.6f,%.6f)\nlast_mouse_after=%s\npoint_after=(%.6f,%.6f)\n"
                   "expected_doc=(%.6f,%.6f)\nplaced_doc=(%.6f,%.6f)\n",
                   pointer_desktop.x(), pointer_desktop.y(), last_mouse_after ? "present" : "none",
                   point_after.x(), point_after.y(), expected_doc.x(), expected_doc.y(),
                   placed_doc.x(), placed_doc.y());
        diagnosticDump("pointer-fallback", "txt", buffer);
    }
}

TEST_F(TextPasteTest, OversizePlainClipboardIsRejectedWithoutPartialInsert)
{
    setPlainClipboard(std::string(TP::MAX_CHARS + 10, 'P'));
    auto *destination = text("dst");
    auto const before = xml();
    bool const changed = pasteInside(destination, 3, UI::TextPasteMode::Automatic);
    document->ensureUpToDate();
    EXPECT_FALSE(changed) << "over-limit plain text must be rejected, not silently truncated";
    EXPECT_EQ(xml(), before) << "rejected plain text must not partially mutate the document";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "rejected plain text must not create an Undo record";
}

TEST_F(TextPasteTest, PlainClipboardAtCharacterLimitStillPastes)
{
    std::string const at_limit(TP::MAX_CHARS, 'P');
    setPlainClipboard(at_limit);
    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(created.front()).size(), at_limit.size())
        << "plain text exactly at the character limit must paste completely";
}

TEST_F(TextPasteTest, MalformedRichPayloadWithPlainFallsBackToPlainWithoutPartialStyle)
{
    setRichClipboard("this is not a valid rich fragment", std::string("ZZ"));
    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const style_before = characterSignature(destination, iteratorAt(destination, 3));
    auto const paragraphs_before = paragraphSignatures(destination);
    std::string const before = multilineText(destination);

    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();

    std::string const after = multilineText(destination);
    EXPECT_EQ(after, before.substr(0, 3) + "ZZ" + before.substr(3))
        << "malformed rich payload must fall back to the valid plain text";
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, 3)), style_before)
        << "no partial source style may be applied";
    // "ZZ" was inserted at logical position 3; compare only pre-existing characters.
    unsigned const world_at = logicalIndexOf(destination, "World");
    ASSERT_NE(world_at, std::numeric_limits<unsigned>::max());
    expectPreservedParagraphSignatures(destination, paragraphs_before, 3, 0, world_at - 2);
}

// Unsafe style values must be stripped during native payload validation, with
// case/URL/expression variants, before anything reaches the document (R5).
TEST_F(TextPasteTest, UnsafeStyleValuesInNativePayloadAreStrippedEndToEnd)
{
    std::string const payload =
        std::string("vac-text-fragment\t") + std::to_string(TP::FORMAT_VERSION) + "\n"
        "P\t\n"
        "R\tfill:URL(http://evil.example/x);font-family:serif\tx\n"
        "R\tfill:JaVaScRiPt:alert(1);font-size:20px\ty\n"
        "R\tfont-family:expression(alert(1));stroke:url(#grad);color:red\tz\n";
    setRichClipboard(payload, std::string("xyz"));

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    document->ensureUpToDate();
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    EXPECT_EQ(multilineText(pasted), "xyz") << "safe character content must survive validation";
    std::string const document_xml = xml();
    EXPECT_FALSE(containsSubstring(document_xml, "evil")) << "external URL must never reach the document";
    EXPECT_FALSE(containsSubstring(document_xml, "URL("));
    EXPECT_FALSE(containsSubstring(document_xml, "javascript"));
    EXPECT_FALSE(containsSubstring(document_xml, "expression("));
    EXPECT_TRUE(containsSubstring(subtreeXml(pasted), "font-family:serif"))
        << "safe declarations in the same record must survive";
    EXPECT_TRUE(containsSubstring(subtreeXml(pasted), "color:red"));
}

TEST_F(TextPasteTest, MalformedRichPayloadWithoutPlainIsRejectedWithoutMutation)
{
    setRichClipboard("garbage payload only", std::nullopt);
    auto const xml_before = xml();
    auto *destination = text("dst");
    EXPECT_FALSE(pasteInside(destination, 3, UI::TextPasteMode::Automatic))
        << "invalid rich payload with no plain fallback must not report success";
    document->ensureUpToDate();
    EXPECT_EQ(xml(), xml_before) << "rejected payload must not partially mutate the document";
}

// Contract correction (tests/response.json r5_contract_correction): an over-limit
// native payload aborts the WHOLE paste. It must not fall back to the valid
// text/plain part, must not partially insert and must not create an Undo record.
// The under-limit malformed-payload fallback stays covered by
// MalformedRichPayloadWithPlainFallsBackToPlainWithoutPartialStyle.
TEST_F(TextPasteTest, OversizeRichPayloadAbortsWholePasteWithoutPartialInsertOrUndo)
{
    std::string oversize(300 * 1024, 'A');
    setRichClipboard(oversize, std::string("OK"));
    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const before_text = multilineText(destination);
    std::string const before_xml = xml();

    EXPECT_FALSE(pasteInside(destination, 3, UI::TextPasteMode::Automatic))
        << "an over-limit native payload must abort the whole paste, not paste plain";
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before_xml) << "aborted paste must not partially mutate the document";
    EXPECT_EQ(multilineText(destination), before_text);
    EXPECT_FALSE(containsSubstring(multilineText(destination), "OK"))
        << "the over-limit native payload must not fall back to its plain part";
    EXPECT_FALSE(containsSubstring(multilineText(destination), "AAAA"));
    EXPECT_FALSE(DocumentUndo::undo(document)) << "aborted paste must not create an Undo record";
}

TEST_F(TextPasteTest, ExternalPlainReplacementInvalidatesRememberedRichPayload)
{
    copyBeta();
    ASSERT_TRUE(clipboardHasMime(kNativeMime));

    setPlainClipboard("external replacement");
    EXPECT_FALSE(clipboardHasMime(kNativeMime)) << "fixture must have replaced the clipboard externally";

    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const destination_style = characterSignature(destination, iteratorAt(destination, 3));
    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();

    std::string const after = multilineText(destination);
    EXPECT_TRUE(containsSubstring(after, "external replacement"));
    auto const inserted_at = after.find("external replacement");
    ASSERT_NE(inserted_at, std::string::npos);
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(after, inserted_at)))),
              destination_style)
        << "stale remembered rich format must not be applied after external replacement";
}

// ---------------------------------------------------------------------------
// Cross-document / serialized cross-instance payload
// ---------------------------------------------------------------------------
TEST_F(TextPasteTest, SerializedPayloadRoundTripsThroughClipboardBytes)
{
    copyBeta();
    auto payload = readClipboardBytes(kNativeMime);
    ASSERT_TRUE(payload.has_value());
    ASSERT_FALSE(payload->empty());
    auto plain = readClipboardBytes("text/plain");
    ASSERT_TRUE(plain.has_value());

    // Wipe the clipboard so no same-process state can survive, then republish
    // exactly the serialized bytes (simulating a second instance).
    setPlainClipboard("sentinel");
    setRichClipboard(*payload, *plain);

    auto *destination = text("dst");
    document->ensureUpToDate();
    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Source));
    document->ensureUpToDate();
    std::string const after = multilineText(destination);
    EXPECT_TRUE(containsSubstring(after, "Beta"));
    auto const inserted_at = after.find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    EXPECT_TRUE(containsSubstring(
        characterSignature(destination, iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(after, inserted_at)))), "fam=monospace"))
        << "serialized payload must carry the source run format across instances";
}

TEST_F(TextPasteTest, CrossDocumentPastePreservesSourceRuns)
{
    auto *source = text("src");
    unsigned const beta_start = logicalIndexOf(source, "Beta");
    ASSERT_NE(beta_start, std::numeric_limits<unsigned>::max());
    copyRange(source, beta_start, beta_start + 4);
    auto const source_chars = characterSignatures(source);

    constexpr char const *doc2 = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <text id="other" x="10" y="50" style="font-family:sans-serif;font-size:14px;fill:#101010">Target </text>
</svg>)SVG";
    auto owned = SPDocument::createNewDocFromMem(std::string(doc2));
    ASSERT_TRUE(owned);
    auto *document2 = testApplication().document_add(std::move(owned));
    ASSERT_TRUE(document2);
    document2->ensureUpToDate();
    auto *desktop2 = testApplication().createDesktop(document2, false, true);
    ASSERT_TRUE(desktop2);

    auto *other = cast<SPText>(document2->getObjectById("other"));
    ASSERT_TRUE(other);
    desktop2->getSelection()->set(other);
    desktop2->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop2->getTool());
    ASSERT_TRUE(tool);
    tool->text_sel_start = iteratorAt(other, 7);
    tool->text_sel_end = tool->text_sel_start;
    ASSERT_TRUE(UI::ClipboardManager::get()->pasteText(desktop2, UI::TextPasteMode::Source));
    document2->ensureUpToDate();

    std::string const after = sp_te_get_string_multiline(other);
    auto const inserted_at = after.find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_EQ(characterSignature(other, iteratorAt(other, static_cast<unsigned>(charIndexOfByte(after, inserted_at)) + i)),
                  source_chars[beta_start + i])
            << "cross-document paste must preserve copied mixed runs";
    }

    desktop2->getSelection()->clear();
    testApplication().destroyDesktop(desktop2);
}

// ---------------------------------------------------------------------------
// Undo/Redo, save/reopen
// ---------------------------------------------------------------------------
TEST_F(TextPasteTest, PasteIsSingleUndoRedoTransaction)
{
    auto *source = text("src");
    copyRange(source, 0, sp_text_get_length(source));
    desktop->setTool("/tools/select");
    desktop->getSelection()->set(item("rect"));
    std::string const before = xml();
    ASSERT_TRUE(activatePasteAction("paste")) << "the real Paste action must handle a rich text clipboard";
    std::string const after = xml();
    ASSERT_NE(before, after) << "the real Paste action must create the pasted text";

    desktop->getSelection()->clear();
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before) << "one Undo must remove the whole paste";
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), after) << "one Redo must restore the whole paste";

    DocumentUndo::undo(document);
    document->ensureUpToDate();
    EXPECT_FALSE(DocumentUndo::undo(document)) << "paste must not add extra undo entries";
    DocumentUndo::redo(document);
}

TEST_F(TextPasteTest, UiActionsExplicitModesApplyExactlyOneUndoStep)
{
    copyBeta();
    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const source_chars = characterSignatures(text("src"));
    unsigned const beta_start = logicalIndexOf(text("src"), "Beta");
    ASSERT_NE(beta_start, std::numeric_limits<unsigned>::max());
    auto const destination_style = characterSignature(destination, iteratorAt(destination, 3));
    std::string const before = xml();

    // Explicit "Paste with Source Formatting" (Source) inside existing text.
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste-keep-source-formatting"));
    std::string after_source = xml();
    ASSERT_NE(after_source, before);
    auto inserted_at = multilineText(destination).find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(multilineText(destination), inserted_at)))),
              source_chars[beta_start])
        << "explicit Source action must keep the copied run format";
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before) << "explicit Source action must be exactly one Undo step";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "no hidden second undo entry";
    document->ensureUpToDate();

    // Explicit "Paste Without Formatting" (Destination) inside existing text.
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste-without-formatting"));
    ASSERT_NE(xml(), before);
    inserted_at = multilineText(destination).find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(multilineText(destination), inserted_at)))),
              destination_style)
        << "explicit Destination action must strip source format";
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before) << "explicit Destination action must be exactly one Undo step";
    EXPECT_FALSE(DocumentUndo::undo(document));
    document->ensureUpToDate();

    // Normal Paste action with default Automatic preference.
    PrefRestore mode("/options/textpaste/mode");
    PrefRestore ask("/options/textpaste/ask");
    removePrefIfSet("/options/textpaste/mode");
    removePrefIfSet("/options/textpaste/ask");
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste"));
    ASSERT_NE(xml(), before);
    inserted_at = multilineText(destination).find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(multilineText(destination), inserted_at)))),
              destination_style)
        << "normal Paste action must default to Automatic/destination style";
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before) << "normal Paste action must be exactly one Undo step";
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(TextPasteTest, SaveReopenKeepsPastedRunFormat)
{
    auto *source = text("src");
    copyRange(source, 0, sp_text_get_length(source));
    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    document->ensureUpToDate();
    auto const chars = characterSignatures(pasted);
    auto const paras = paragraphSignatures(pasted);
    auto const *id = pasted->getRepr()->attribute("id");
    ASSERT_TRUE(id);

    auto reopened = SPDocument::createNewDocFromMem(xml());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto *reloaded = cast<SPText>(reopened->getObjectById(id));
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(characterSignatures(reloaded), chars) << "saved/reopened paste must keep run format";
    EXPECT_EQ(paragraphSignatures(reloaded), paras) << "saved/reopened paste must keep paragraph format";
}

// ---------------------------------------------------------------------------
// Regressions: whole-object SVG copy/paste and PasteStyle stay unchanged
// ---------------------------------------------------------------------------
// Whole text OBJECT copy/paste (Selector) must keep using the SVG object path:
// the native text fragment MIME must not be published, and the pasted object
// must keep its copied geometry (R1).
TEST_F(TextPasteTest, WholeTextObjectCopyPasteStaysOnSvgObjectPath)
{
    auto *source = text("src");
    desktop->getSelection()->set(source);
    desktop->setTool("/tools/select");
    UI::ClipboardManager::get()->copy(desktop->getSelection());
    pumpFor(200);

    EXPECT_FALSE(clipboardHasMime(kNativeMime))
        << "whole text object copy must not publish the in-text fragment MIME";

    auto const before = objectIds();
    ASSERT_TRUE(UI::ClipboardManager::get()->paste(desktop, false, false));
    document->ensureUpToDate();
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u) << "whole text object paste must clone the SPText";
    auto *clone = created.front();
    EXPECT_EQ(multilineText(clone), multilineText(source)) << "whole text object content must survive";
    EXPECT_TRUE(clone->getRepr()->attribute("transform"))
        << "whole-object paste must keep the copied rotation/transform (fragment paste never does)";
}

TEST_F(TextPasteTest, ObjectSvgCopyPasteAndPasteStyleRemainUnchanged)
{
    auto *rectangle = item("rect");
    auto *target = item("rect2");
    ASSERT_TRUE(rectangle && target);
    desktop->getSelection()->set(rectangle);
    desktop->setTool("/tools/select");
    UI::ClipboardManager::get()->copy(desktop->getSelection());
    pumpFor(150);

    auto const before = objectIds();
    ASSERT_TRUE(UI::ClipboardManager::get()->paste(desktop, false, false));
    document->ensureUpToDate();
    std::vector<SPItem *> created;
    for (auto const &id : objectIds()) {
        if (before.count(id)) continue;
        if (auto *object = cast<SPItem>(document->getObjectById(id))) created.push_back(object);
    }
    ASSERT_FALSE(created.empty()) << "whole object copy/paste must still paste SVG objects";
    for (auto *object : created) {
        EXPECT_FALSE(cast<SPText>(object)) << "object paste must not become a text fragment";
    }

    desktop->getSelection()->set(target);
    ASSERT_TRUE(UI::ClipboardManager::get()->pasteStyle(desktop->getSelection()));
    document->ensureUpToDate();
    ASSERT_TRUE(target->style);
    ASSERT_TRUE(rectangle->style);
    EXPECT_EQ(target->style->fill.getColor().toRGBA(), rectangle->style->fill.getColor().toRGBA());
}

// ---------------------------------------------------------------------------
// Locked / hidden / unsupported targets
// ---------------------------------------------------------------------------
TEST_F(TextPasteTest, LockedAndHiddenTextAreNotEditedByPaste)
{
    copyBeta();
    auto const locked_before = subtreeXml(text("locked-text"));
    auto const hidden_before = subtreeXml(text("hidden-text"));

    // Selecting a locked group with the text tool must not enter editing.
    desktop->getSelection()->set(item("locked-group"));
    desktop->setTool("/tools/text");
    auto *tool = textTool();
    ASSERT_TRUE(tool);
    EXPECT_EQ(tool->textItem(), nullptr) << "locked text must not become editable";

    auto const before = objectIds();
    ASSERT_TRUE(UI::ClipboardManager::get()->pasteText(desktop, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();
    EXPECT_EQ(subtreeXml(text("locked-text")), locked_before);
    EXPECT_EQ(subtreeXml(text("hidden-text")), hidden_before);
    EXPECT_FALSE(textsNotIn(before).empty()) << "paste must create a new object, not edit locked content";

    desktop->getSelection()->set(item("hidden-group"));
    desktop->setTool("/tools/text");
    tool = textTool();
    ASSERT_TRUE(tool);
    EXPECT_EQ(tool->textItem(), nullptr) << "hidden text must not become editable";
    UI::ClipboardManager::get()->pasteText(desktop, UI::TextPasteMode::Automatic);
    document->ensureUpToDate();
    EXPECT_EQ(subtreeXml(text("hidden-text")), hidden_before);

    // Unsupported container: flowed text must not be silently edited.
    auto *flow = cast<SPItem>(document->getObjectById("flow"));
    ASSERT_TRUE(flow);
    auto const flow_before = subtreeXml(flow);
    desktop->getSelection()->set(item("flow-group"));
    desktop->setTool("/tools/text");
    tool = textTool();
    ASSERT_TRUE(tool);
    EXPECT_EQ(tool->textItem(), nullptr) << "flowRoot must not become editable by a text paste";
    UI::ClipboardManager::get()->pasteText(desktop, UI::TextPasteMode::Automatic);
    document->ensureUpToDate();
    EXPECT_EQ(subtreeXml(flow), flow_before);

    // Grouped text is not a text-tool edit target: no accidental member edit.
    auto const group_before = subtreeXml(text("group-text"));
    desktop->getSelection()->set(item("text-group"));
    desktop->setTool("/tools/text");
    tool = textTool();
    ASSERT_TRUE(tool);
    EXPECT_EQ(tool->textItem(), nullptr) << "grouped text must not be edited as a side effect";
    UI::ClipboardManager::get()->pasteText(desktop, UI::TextPasteMode::Automatic);
    document->ensureUpToDate();
    EXPECT_EQ(subtreeXml(text("group-text")), group_before);
}

// ---------------------------------------------------------------------------
// Pure fragment representation/validation (no GUI; real clipboard paths above)
// ---------------------------------------------------------------------------
TEST(TextPasteFragmentTest, SerializationRoundTripsMixedRunsParagraphsAndUnicode)
{
    TP::Fragment fragment;
    TP::Paragraph first;
    first.style = "text-align:center;line-height:20px;";
    TP::Run run_a;
    run_a.text = "Alpha e\u0301 \u05e9\u05dc\u05d5\u05dd";
    run_a.style = "font-family:serif;font-size:12px;fill:#112233;";
    TP::Run run_b;
    run_b.text = "Beta";
    run_b.style = "font-family:monospace;font-size:24px;font-weight:bold;font-style:italic;"
                  "fill:#cc0000;letter-spacing:5px;font-variant-caps:small-caps;";
    first.runs = {run_a, run_b};
    TP::Paragraph second;
    second.style = "text-anchor:middle;";
    TP::Run run_c;
    run_c.text = "Gamma \U0001F600\tCJK \u4f60\u597d";
    run_c.style = "font-family:cursive;font-size:18px;";
    second.runs = {run_c};
    fragment.paragraphs = {first, second};
    fragment.plain = "Alpha e\u0301 \u05e9\u05dc\u05d5\u05ddBeta\nGamma \U0001F600\tCJK \u4f60\u597d";

    auto const payload = TP::serialize(fragment);
    ASSERT_LE(payload.size(), TP::MAX_PAYLOAD_BYTES);
    auto const parsed = TP::parse(payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->plain, fragment.plain) << "logical characters and paragraph newlines must round-trip";
    ASSERT_EQ(parsed->paragraphs.size(), 2u);
    EXPECT_EQ(parsed->paragraphs[0].style, first.style);
    EXPECT_EQ(parsed->paragraphs[1].style, second.style);
    ASSERT_EQ(parsed->paragraphs[0].runs.size(), 2u);
    EXPECT_EQ(parsed->paragraphs[0].runs[0].text, run_a.text);
    EXPECT_TRUE(containsSubstring(parsed->paragraphs[0].runs[1].style, "font-weight:bold"));
    EXPECT_TRUE(containsSubstring(parsed->paragraphs[0].runs[1].style, "font-variant-caps:small-caps"));
    ASSERT_EQ(parsed->paragraphs[1].runs.size(), 1u);
    EXPECT_EQ(parsed->paragraphs[1].runs[0].text, run_c.text);
    EXPECT_EQ(parsed->paragraphs[1].runs[0].style, run_c.style);
}

TEST(TextPasteFragmentTest, ParseRejectsMalformedAndOversizePayloads)
{
    // Current-format structural fixtures: the header always names the format
    // this build publishes (version 2, root-approved). They must keep testing
    // record structure, not accidentally become version-rejection cases.
    std::string const header = std::string("vac-text-fragment\t") + std::to_string(TP::FORMAT_VERSION) + "\n";
    std::string const unsupported_version =
        std::string("vac-text-fragment\t") + std::to_string(TP::FORMAT_VERSION + 99) + "\n";

    EXPECT_FALSE(TP::parse("").has_value());
    EXPECT_FALSE(TP::parse("garbage").has_value());
    EXPECT_FALSE(TP::parse(unsupported_version + "P\t\n").has_value()) << "unsupported version";
    EXPECT_FALSE(TP::parse("vac-text-fragment\t1\nP\t\nR\t\tBeta\n").has_value())
        << "legacy version 1 carries no source-coordinate metadata and must stay unsupported";
    EXPECT_FALSE(TP::parse(header + "R\t\tBeta\n").has_value()) << "run before paragraph";
    EXPECT_FALSE(TP::parse(header + "X\twhatever\n").has_value()) << "unknown record";
    EXPECT_FALSE(TP::parse(header + "P\n").has_value()) << "record without a tab";
    EXPECT_FALSE(TP::parse(header + "P\t\nR\t\tbad\\qescape\n").has_value()) << "bad escape";
    EXPECT_FALSE(TP::parse(header + "P\t\nR\t\t" + '\x01' + "\n").has_value())
        << "control character in run text";
    EXPECT_FALSE(TP::parse(header + "P\t\nR\t\t" + '\xC3' + "\n").has_value())
        << "invalid UTF-8";
    EXPECT_FALSE(TP::parse(std::string(TP::MAX_PAYLOAD_BYTES + 1, 'x')).has_value()) << "oversize payload";

    std::string long_run = header + "P\t\nR\t\t";
    long_run.append(TP::MAX_RUN_LENGTH + 1, 'a');
    long_run += '\n';
    EXPECT_FALSE(TP::parse(long_run).has_value()) << "oversize run";

    std::string many_runs = header + "P\t\n";
    for (std::size_t i = 0; i < TP::MAX_RUNS + 1; ++i) many_runs += "R\t\ta\n";
    EXPECT_FALSE(TP::parse(many_runs).has_value()) << "too many runs";
}

TEST(TextPasteFragmentTest, StyleSanitizerDropsUnsafeUnknownAndDuplicateDeclarations)
{
    auto const sanitized = TP::sanitize_style(
        "font-family:serif;url(http://evil.example);color:red;background:url(x);behavior:evil;"
        "font-size:12px;font-size:99px;",
        false);
    EXPECT_TRUE(containsSubstring(sanitized, "font-family:serif;"));
    EXPECT_TRUE(containsSubstring(sanitized, "color:red;"));
    EXPECT_TRUE(containsSubstring(sanitized, "font-size:12px;")) << "first declaration wins";
    EXPECT_FALSE(containsSubstring(sanitized, "url("));
    EXPECT_FALSE(containsSubstring(sanitized, "background"));
    EXPECT_FALSE(containsSubstring(sanitized, "behavior"));
    EXPECT_FALSE(containsSubstring(sanitized, "font-size:99px"));

    EXPECT_FALSE(containsSubstring(TP::sanitize_style("fill:url(#grad);", false), "url("));
    EXPECT_FALSE(containsSubstring(TP::sanitize_style("fill:javascript:alert(1);", false), "javascript"));
    EXPECT_FALSE(containsSubstring(TP::sanitize_style("fill:expression(alert(1));", false), "expression("));
    EXPECT_FALSE(containsSubstring(TP::sanitize_style("fill:<script>;", false), "<"));
    EXPECT_TRUE(TP::sanitize_style("font-family:serif;", true).empty())
        << "character-only property must not be accepted in paragraph scope";
    EXPECT_TRUE(containsSubstring(TP::sanitize_style("font-family:serif;", false), "font-family:serif;"));
}

TEST(TextPasteFragmentTest, InsertionPlanMatrixMatchesProductContract)
{
    using Mode = Inkscape::UI::TextPasteMode;

    auto const auto_outside_rich = TP::plan_insertion(Mode::Automatic, false, true);
    EXPECT_TRUE(auto_outside_rich.create_object);
    EXPECT_TRUE(auto_outside_rich.source_run_styles);
    EXPECT_TRUE(auto_outside_rich.source_paragraph_style);
    EXPECT_FALSE(auto_outside_rich.destination_typing_style);

    auto const auto_inside_rich = TP::plan_insertion(Mode::Automatic, true, true);
    EXPECT_FALSE(auto_inside_rich.create_object);
    EXPECT_FALSE(auto_inside_rich.source_run_styles);
    EXPECT_FALSE(auto_inside_rich.source_paragraph_style);
    EXPECT_TRUE(auto_inside_rich.destination_typing_style);

    auto const source_inside_rich = TP::plan_insertion(Mode::Source, true, true);
    EXPECT_TRUE(source_inside_rich.source_run_styles);
    EXPECT_FALSE(source_inside_rich.source_paragraph_style) << "no paragraph leak on inline paste";
    EXPECT_FALSE(source_inside_rich.destination_typing_style);

    auto const source_inside_plain = TP::plan_insertion(Mode::Source, true, false);
    EXPECT_FALSE(source_inside_plain.source_run_styles);
    EXPECT_TRUE(source_inside_plain.destination_typing_style) << "missing rich format falls back to destination";

    auto const destination_inside = TP::plan_insertion(Mode::Destination, true, true);
    EXPECT_FALSE(destination_inside.source_run_styles);
    EXPECT_FALSE(destination_inside.source_paragraph_style);
    EXPECT_TRUE(destination_inside.destination_typing_style);

    auto const destination_outside = TP::plan_insertion(Mode::Destination, false, true);
    EXPECT_TRUE(destination_outside.create_object);
    EXPECT_TRUE(destination_outside.destination_typing_style);
}

TEST(TextPasteFragmentTest, PlainTextFragmentKeepsLogicalCharactersAndParagraphs)
{
    auto const fragment = TP::from_plain_text("one\ntwo\tthree");
    ASSERT_EQ(fragment.paragraphs.size(), 2u);
    EXPECT_EQ(fragment.plain, "one\ntwo\tthree");
    EXPECT_EQ(fragment.paragraphs[0].runs.at(0).text, "one");
    EXPECT_EQ(fragment.paragraphs[1].runs.at(0).text, "two\tthree");
}

// ---------------------------------------------------------------------------
// Preferences and prompts
// ---------------------------------------------------------------------------
// A native provider that writes its payload in short chunks with delays must be
// read to EOF: a short read may never be mistaken for the end of the fragment.
TEST_F(TextPasteTest, NativeClipboardShortChunksAreReadToCompletePayload)
{
    copyBeta();
    auto const payload = readClipboardBytes(kNativeMime);
    ASSERT_TRUE(payload.has_value());
    ASSERT_FALSE(payload->empty());

    auto *source = text("src");
    unsigned const beta_start = logicalIndexOf(source, "Beta");
    ASSERT_NE(beta_start, std::numeric_limits<unsigned>::max());
    auto const source_style = characterSignature(source, iteratorAt(source, beta_start));

    ASSERT_TRUE(installClipboardProvider(makeChunkedProvider(*payload, 8, 1, false)));
    ASSERT_TRUE(clipboardHasMime(kNativeMime));

    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const before = xml();
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste-keep-source-formatting"));
    document->ensureUpToDate();

    auto const text_after = multilineText(destination);
    auto const inserted_at = text_after.find("Beta");
    ASSERT_NE(inserted_at, std::string::npos)
        << "the tail of a short-chunked native payload must not be dropped";
    EXPECT_EQ(characterSignature(destination,
                                 iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(text_after, inserted_at)))),
              source_style)
        << "chunk boundaries must not corrupt the copied run styles";
    EXPECT_NE(xml(), before) << "the complete chunked payload must be applied";
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before) << "the chunked paste must still be exactly one Undo step";
}

// A native provider that delivers one chunk and then stalls forever must not
// hang the GUI thread: the product's absolute 5s deadline aborts the whole paste
// with no partial fragment and no Undo record.
TEST_F(TextPasteTest, StalledNativeClipboardAbortsWholePasteWithoutMutationOrUndo)
{
    PrefRestore ask("/options/textpaste/ask");
    PrefRestore mode("/options/textpaste/mode");
    Preferences::get()->setBool("/options/textpaste/ask", false);
    Preferences::get()->setInt("/options/textpaste/mode", 0);

    copyBeta();
    auto const payload = readClipboardBytes(kNativeMime);
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(installClipboardProvider(makeChunkedProvider(*payload, 16, 1, true)));
    ASSERT_TRUE(clipboardHasMime(kNativeMime));

    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const before = xml();
    ASSERT_TRUE(placeTextCursor(destination, 3));

    auto const start = std::chrono::steady_clock::now();
    bool const changed = UI::ClipboardManager::get()->pasteText(desktop, UI::TextPasteMode::Automatic);
    auto const elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    document->ensureUpToDate();

    EXPECT_FALSE(changed) << "a stalled native read must abort the whole paste";
    EXPECT_GE(elapsed_ms, 4000) << "the bounded read must wait for the stalled producer's 5s deadline";
    EXPECT_LT(elapsed_ms, 15000) << "a stalled producer must not hang the GUI thread";
    EXPECT_EQ(xml(), before) << "a timed-out fragment must never be applied, not even partially";
    EXPECT_FALSE(containsSubstring(multilineText(destination), "Beta"))
        << "the partial fragment must not reach the destination";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "an aborted paste must not create an Undo record";
}

TEST_F(TextPasteTest, PreferencesDefaultsMatchTheSkeletonForNativeAndExternalFamilies)
{
    PrefRestore mode("/options/textpaste/mode");
    PrefRestore ask("/options/textpaste/ask");
    PrefRestore external_mode("/options/textpaste/external-mode");
    PrefRestore external_ask("/options/textpaste/external-ask");
    removePrefIfSet("/options/textpaste/mode");
    removePrefIfSet("/options/textpaste/ask");
    removePrefIfSet("/options/textpaste/external-mode");
    removePrefIfSet("/options/textpaste/external-ask");
    // A sentinel fallback makes this discriminating: passing the expected value
    // as the fallback would assert nothing. The skeleton must define the keys
    // (src/preferences-skeleton.h:273), so the sentinel must never come back.
    EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/mode", -12345), 0);
    EXPECT_FALSE(Preferences::get()->getBool("/options/textpaste/ask", true));
    // External defaults are independent: Automatic (0) and asking enabled.
    EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/external-mode", -12345), 0);
    EXPECT_TRUE(Preferences::get()->getBool("/options/textpaste/external-ask", false));
}

TEST_F(TextPasteTest, NormalPasteHonoursModePreferenceAndInvalidPrefFallsBackToAutomatic)
{
    PrefRestore mode("/options/textpaste/mode");
    PrefRestore ask("/options/textpaste/ask");
    auto *source = text("src");
    unsigned const beta_start = logicalIndexOf(source, "Beta");
    ASSERT_NE(beta_start, std::numeric_limits<unsigned>::max());
    copyRange(source, beta_start, beta_start + 4);
    auto const source_chars = characterSignatures(source);

    // mode=1 (Source) with ask=false: the normal existing Paste action must
    // publish copied run format inside an existing text.
    Preferences::get()->setInt("/options/textpaste/mode", 1);
    Preferences::get()->setBool("/options/textpaste/ask", false);
    auto *destination = text("dst");
    desktop->getSelection()->set(destination);
    desktop->setTool("/tools/text");
    auto *tool = textTool();
    ASSERT_TRUE(tool);
    tool->text_sel_start = iteratorAt(destination, 3);
    tool->text_sel_end = tool->text_sel_start;
    ASSERT_TRUE(UI::ClipboardManager::get()->paste(desktop, false, false));
    document->ensureUpToDate();
    std::string after = multilineText(destination);
    auto inserted_at = after.find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, charIndexOfByte(after, inserted_at))),
              source_chars[beta_start]);

    // Reset destination and paste with an invalid preference value: the
    // documented fallback is Automatic (destination typing style).
    desktop->getSelection()->clear();
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    auto const destination_style = characterSignature(destination, iteratorAt(destination, 3));
    Preferences::get()->setString("/options/textpaste/mode", "banana");
    desktop->getSelection()->set(destination);
    desktop->setTool("/tools/text");
    tool = textTool();
    ASSERT_TRUE(tool);
    tool->text_sel_start = iteratorAt(destination, 3);
    tool->text_sel_end = tool->text_sel_start;
    ASSERT_TRUE(UI::ClipboardManager::get()->paste(desktop, false, false));
    document->ensureUpToDate();
    after = multilineText(destination);
    inserted_at = after.find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, charIndexOfByte(after, inserted_at))),
              destination_style)
        << "invalid mode preference must behave as Automatic";
}

TEST_F(TextPasteTest, PlainClipboardNeverPromptsEvenWhenAskIsEnabled)
{
    PrefRestore ask("/options/textpaste/ask");
    PrefRestore mode("/options/textpaste/mode");
    Preferences::get()->setBool("/options/textpaste/ask", true);
    Preferences::get()->setInt("/options/textpaste/mode", 1);
    setPlainClipboard("no prompt plain");

    PromptWatch watch;
    PromptTimerGuard prompt_timer(watch);
    auto *destination = text("dst");
    std::string const before = xml();
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste")) << "normal Paste must handle plain text";
    document->ensureUpToDate();
    EXPECT_EQ(watch.dialogs, 0) << "pure plain-text paste must never show the formatted-paste prompt";
    EXPECT_NE(xml(), before) << "plain text must still paste";
    EXPECT_TRUE(containsSubstring(multilineText(destination), "no prompt plain"));
}

// Cancel while a non-Automatic stored default is active: the normal Paste action
// must prompt for ANY stored default and cancel must leave document and prefs
// untouched, with exactly one dialog.
TEST_F(TextPasteTest, PromptCancelWithStoredModeLeavesDocumentAndPrefsUnchanged)
{
    PrefRestore ask("/options/textpaste/ask");
    PrefRestore mode("/options/textpaste/mode");
    Preferences::get()->setBool("/options/textpaste/ask", true);
    Preferences::get()->setInt("/options/textpaste/mode", 1);

    copyBeta();
    ASSERT_TRUE(clipboardHasMime(kNativeMime));

    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const before = xml();

    PromptWatch watch;
    PromptTimerGuard prompt_timer(watch);
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste")) << "normal Paste must prompt for a stored Source default";
    document->ensureUpToDate();

    ASSERT_FALSE(watch.timed_out) << "paste dialog was not found/automated";
    EXPECT_EQ(watch.dialogs, 1) << "one normal Paste must show exactly one prompt";
    EXPECT_FALSE(watch.clicked_confirm);
    EXPECT_EQ(xml(), before) << "cancel must leave the document unchanged";
    EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/mode", 0), 1)
        << "cancel must not change the stored mode";
    EXPECT_TRUE(Preferences::get()->getBool("/options/textpaste/ask", false))
        << "cancel must not change the ask preference";
}

// Selecting a mode in the prompt (without Remember) must apply exactly that
// mode for this paste and must persist nothing.
TEST_F(TextPasteTest, PromptSelectedModeIsUsedWithoutRemember)
{
    PrefRestore ask("/options/textpaste/ask");
    PrefRestore mode("/options/textpaste/mode");
    Preferences::get()->setBool("/options/textpaste/ask", true);
    Preferences::get()->setInt("/options/textpaste/mode", 1);

    copyBeta();
    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const destination_style = characterSignature(destination, iteratorAt(destination, 3));

    PromptWatch watch;
    watch.select_name = "text-paste-mode-destination";
    watch.confirm = true;
    PromptTimerGuard prompt_timer(watch);
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    ASSERT_FALSE(watch.timed_out) << "paste dialog was not found/automated";
    ASSERT_TRUE(watch.clicked_confirm);
    EXPECT_EQ(watch.dialogs, 1);
    auto const inserted_at = multilineText(destination).find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(multilineText(destination), inserted_at)))),
              destination_style)
        << "the mode selected in the prompt must be applied";
    EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/mode", 0), 1)
        << "without Remember the stored mode must not change";
    EXPECT_TRUE(Preferences::get()->getBool("/options/textpaste/ask", false))
        << "without Remember the ask preference must not change";
}

TEST_F(TextPasteTest, PromptRememberPersistsSelectedModeAndDisablesAsk)
{
    PrefRestore ask("/options/textpaste/ask");
    PrefRestore mode("/options/textpaste/mode");
    Preferences::get()->setBool("/options/textpaste/ask", true);
    Preferences::get()->setInt("/options/textpaste/mode", 2);

    copyBeta();
    auto *source = text("src");
    unsigned const beta_start = logicalIndexOf(source, "Beta");
    ASSERT_NE(beta_start, std::numeric_limits<unsigned>::max());
    auto const source_style = characterSignature(source, iteratorAt(source, beta_start));

    auto *destination = text("dst");
    PromptWatch watch;
    watch.remember = true;
    watch.select_name = "text-paste-mode-source";
    watch.confirm = true;
    PromptTimerGuard prompt_timer(watch);
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    ASSERT_FALSE(watch.timed_out) << "paste dialog was not found/automated";
    ASSERT_TRUE(watch.clicked_confirm);
    ASSERT_TRUE(watch.clicked_source) << "Keep source formatting radio was not found";
    EXPECT_EQ(watch.dialogs, 1);
    EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/mode", 0), 1)
        << "remembered choice must store the selected mode exactly";
    EXPECT_FALSE(Preferences::get()->getBool("/options/textpaste/ask", false))
        << "remember must disable the ask preference";
    auto const inserted_at = multilineText(destination).find("Beta");
    ASSERT_NE(inserted_at, std::string::npos);
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, static_cast<unsigned>(charIndexOfByte(multilineText(destination), inserted_at)))),
              source_style)
        << "the remembered Source mode must be applied to this paste";
}

// Lifetime/revalidation: the clipboard is replaced while the modal dialog is
// open. Confirming must abort the normal paste without applying stale content.
TEST_F(TextPasteTest, PromptRevalidatesClipboardChangedWhileDialogOpen)
{
    PrefRestore ask("/options/textpaste/ask");
    PrefRestore mode("/options/textpaste/mode");
    Preferences::get()->setBool("/options/textpaste/ask", true);
    Preferences::get()->setInt("/options/textpaste/mode", 1);

    copyBeta();
    ASSERT_TRUE(clipboardHasMime(kNativeMime));
    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const before = xml();

    PromptWatch watch;
    watch.confirm = true;
    watch.side_effect = [] { setPlainClipboard("clipboard changed during prompt"); };
    PromptTimerGuard prompt_timer(watch);
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    ASSERT_FALSE(watch.timed_out) << "paste dialog was not found/automated";
    ASSERT_TRUE(watch.clicked_confirm);
    EXPECT_EQ(watch.dialogs, 1);
    EXPECT_EQ(xml(), before) << "the stale pre-prompt fragment must not be applied";
    // The fixture's own source object permanently contains <tspan id="src-bold">Beta</tspan>,
    // so the mutation oracle must be scoped to the destination subtree; the whole-document
    // XML equality above already proves the document was not mutated at all.
    EXPECT_FALSE(containsSubstring(subtreeXml(destination), "Beta"))
        << "the stale pre-prompt fragment must not reach the destination";
    EXPECT_FALSE(containsSubstring(multilineText(destination), "Beta"))
        << "the destination text must not gain the stale fragment's characters";
    EXPECT_FALSE(DocumentUndo::undo(document))
        << "an aborted paste must not create an Undo record";
}

// The caret/selection inside the edited text object is part of the destination
// identity: moving it while the modal prompt is open must abort the whole paste
// (no mutation, no Undo, no second dialog).
TEST_F(TextPasteTest, PromptRevalidatesDestinationSelectionChangedWhileDialogOpen)
{
    PrefRestore ask("/options/textpaste/ask");
    PrefRestore mode("/options/textpaste/mode");
    Preferences::get()->setBool("/options/textpaste/ask", true);
    Preferences::get()->setInt("/options/textpaste/mode", 1);

    copyBeta();
    ASSERT_TRUE(clipboardHasMime(kNativeMime));
    auto *destination = text("dst");
    document->ensureUpToDate();
    // placeTextCursor() installs the text tool through SPDesktop::setTool(),
    // which deletes and recreates the active tool even for the same tool name
    // (desktop.cpp setTool). The live tool pointer must therefore be taken
    // AFTER the cursor is placed: a pointer captured earlier addresses the
    // deleted tool and the callback would write into freed memory (a test
    // use-after-free) instead of moving the destination caret that the product
    // revalidates while the prompt is open.
    ASSERT_TRUE(placeTextCursor(destination, 3));
    auto *tool = textTool();
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool, desktop->getTool());
    ASSERT_EQ(tool->textItem(), destination);
    auto const caret_before_prompt = iteratorAt(destination, 3);
    std::string const before = xml();

    PromptWatch watch;
    watch.confirm = true;
    watch.side_effect = [&] {
        // The callback must hold the live tool that edits the destination, not
        // a pointer to the tool instance deleted by placeTextCursor().
        ASSERT_EQ(desktop->getTool(), tool) << "the callback captured a stale text tool";
        ASSERT_EQ(tool->textItem(), destination) << "the callback must edit the destination text";
        // Move the caret away from the position captured before the prompt.
        tool->text_sel_start = iteratorAt(destination, 0);
        tool->text_sel_end = iteratorAt(destination, 0);
        EXPECT_TRUE(tool->text_sel_start != caret_before_prompt)
            << "the side effect must actually move the live caret";
    };
    PromptTimerGuard prompt_timer(watch);
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    ASSERT_FALSE(watch.timed_out) << "paste dialog was not found/automated";
    ASSERT_TRUE(watch.clicked_confirm);
    EXPECT_EQ(watch.dialogs, 1) << "an aborted paste must not re-prompt";
    EXPECT_EQ(xml(), before) << "moving the destination caret during the prompt must leave the document unchanged";
    EXPECT_FALSE(containsSubstring(subtreeXml(destination), "Beta"))
        << "the fragment must not be applied at the stale caret position";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "an aborted paste must not create an Undo record";
}

// Any document modification while the modal prompt is open invalidates the
// captured destination: the paste must abort without adding its own mutation or
// Undo record. The concurrent edit itself stays as exactly one Undo step.
TEST_F(TextPasteTest, PromptRevalidatesDocumentModifiedWhileDialogOpen)
{
    PrefRestore ask("/options/textpaste/ask");
    PrefRestore mode("/options/textpaste/mode");
    Preferences::get()->setBool("/options/textpaste/ask", true);
    Preferences::get()->setInt("/options/textpaste/mode", 1);

    copyBeta();
    ASSERT_TRUE(clipboardHasMime(kNativeMime));
    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const before = xml();

    std::string after_side_edit;
    PromptWatch watch;
    watch.confirm = true;
    watch.side_effect = [&] {
        // A genuine, undoable document edit while the dialog is open.
        auto *rect = item("rect");
        ASSERT_TRUE(rect);
        rect->getRepr()->setAttribute("fill", "#abcdef");
        document->ensureUpToDate();
        DocumentUndo::done(document, Util::Internal::ContextString("Concurrent edit"), "draw-rect");
        document->ensureUpToDate();
        after_side_edit = xml();
    };
    PromptTimerGuard prompt_timer(watch);
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    ASSERT_FALSE(watch.timed_out) << "paste dialog was not found/automated";
    ASSERT_TRUE(watch.clicked_confirm);
    ASSERT_FALSE(after_side_edit.empty()) << "the concurrent edit did not run";
    EXPECT_NE(after_side_edit, before) << "the concurrent edit must be a real modification";
    EXPECT_EQ(watch.dialogs, 1) << "an aborted paste must not re-prompt";
    EXPECT_EQ(xml(), after_side_edit)
        << "a document modified during the prompt must not receive the stale paste fragment";
    EXPECT_FALSE(containsSubstring(subtreeXml(destination), "Beta"))
        << "the stale fragment must not be applied to the modified document";
    // Exactly one Undo entry exists: the concurrent edit. The aborted paste added none.
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before) << "only the concurrent edit may be undoable";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "the aborted paste must not add an Undo record";
}

// ---------------------------------------------------------------------------
// Rendered (effective) size of native source-format paste. User-confirmed
// report: pasting a copied selection visibly changes the glyph size (7.937mm
// displayed) both on an empty canvas and inside existing text. Ancestor
// transforms are the actual rendered size; the stored local computed style is
// not. These cases are outcome-based on the rendered glyph box, and they assert
// the behavior the user requires (source visual size preserved / destination
// visual size adopted), never a weaker value.
// ---------------------------------------------------------------------------

TEST_F(TextPasteTest, ScaledSourceCopiedOutsidePreservesEffectiveRenderedSize)
{
    auto *source = text("src-scaled");
    ASSERT_EQ(multilineText(source), std::string("HHHHHHHH"));

    auto const *source_style = sp_te_style_at_position(source, iteratorAt(source, 0));
    ASSERT_TRUE(source_style);
    double const source_scale = source->i2doc_affine().descrim();
    ASSERT_NEAR(source_scale, 2.0, 0.001) << "fixture: the source text must be uniformly scaled 2x";
    double const source_effective_size = source_style->font_size.computed * source_scale;
    ASSERT_NEAR(source_effective_size, 24.0, 0.01)
        << "fixture: 12px inside a 2x group renders as 24 document units";
    double const source_rendered_h = renderedHeightInDoc(source);
    ASSERT_GT(source_rendered_h, 0.0);

    auto fragment = extractFragment(desktop, source, 0, sp_text_get_length(source));
    ASSERT_TRUE(fragment);
    ASSERT_FALSE(fragment->empty());

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutsideFragment(desktop, document, *fragment))
        << "native outside paste must create editable text";
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u) << "outside paste must create exactly one text object";
    auto *pasted = created.front();

    EXPECT_NEAR(renderedHeightInDoc(pasted), source_rendered_h,
                std::max(0.05, source_rendered_h * 0.03))
        << "pasted glyphs must keep the source's rendered size, not its pre-transform local size";

    auto const *pasted_style = sp_te_style_at_position(pasted, iteratorAt(pasted, 0));
    ASSERT_TRUE(pasted_style);
    EXPECT_NEAR(pasted_style->font_size.computed * pasted->i2doc_affine().descrim(), source_effective_size, 0.05)
        << "effective font size in document coordinates must be preserved";
    EXPECT_NEAR(pasted_style->font_size.computed, source_effective_size, 0.05)
        << "outside the source transform the stored run size must be the effective size";
}

TEST_F(TextPasteTest, ScaledDestinationAutoInsidePreservesEffectiveRenderedSize)
{
    auto *destination = text("dst-scaled");
    ASSERT_EQ(multilineText(destination), std::string("HHHHHHHH"));

    auto const *dest_style = sp_te_style_at_position(destination, iteratorAt(destination, 0));
    ASSERT_TRUE(dest_style);
    double const dest_scale = destination->i2doc_affine().descrim();
    ASSERT_NEAR(dest_scale, 0.5, 0.001) << "fixture: the destination must be uniformly scaled 0.5x";
    double const dest_local_size = dest_style->font_size.computed;
    double const dest_effective_size = dest_local_size * dest_scale;
    double const rendered_before = renderedHeightInDoc(destination);
    ASSERT_GT(rendered_before, 0.0);

    // Same repeated glyph as the destination, so a correct Automatic-inside
    // insertion cannot change the rendered height of the one-line text.
    auto *source = text("src-scaled");
    ASSERT_EQ(multilineText(source), std::string("HHHHHHHH"));
    auto fragment = extractFragment(desktop, source, 0, 4);
    ASSERT_TRUE(fragment);

    ASSERT_TRUE(pasteInsideFragment(desktop, document, destination, 8, *fragment))
        << "Automatic inside an existing text must insert";
    EXPECT_EQ(multilineText(destination), std::string("HHHHHHHHHHHH"));

    EXPECT_NEAR(renderedHeightInDoc(destination), rendered_before,
                std::max(0.05, rendered_before * 0.03))
        << "inserted characters must render at the destination's effective visual size";

    auto const *inserted = sp_te_style_at_position(destination, iteratorAt(destination, 9));
    ASSERT_TRUE(inserted);
    EXPECT_NEAR(inserted->font_size.computed, dest_local_size, 0.05)
        << "inserted characters must adopt the destination's local typing size";
    EXPECT_NEAR(inserted->font_size.computed * destination->i2doc_affine().descrim(), dest_effective_size, 0.05)
        << "inserted characters must render at the destination's effective size";
}

// A px viewBox document and an mm viewBox document with the same native source
// must both paste outside at the source's own rendered size: document units
// (getDocumentScale) must not be applied a second time to the computed style.
// The rendered glyph box is measured in document coordinates, so the same
// source style has to produce the same local size and the same source-relative
// rendered size in both documents.
namespace {
constexpr char const *kPxDocSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <text id="m-src" x="10" y="40" style="font-family:serif;font-size:12px;fill:#112233">HHHHHHHH</text>
</svg>)SVG";
constexpr char const *kMmDocSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100">
  <text id="m-src" x="10" y="40" style="font-family:serif;font-size:12px;fill:#112233">HHHHHHHH</text>
</svg>)SVG";

struct FreshDocPasteResult {
    SPDocument *doc = nullptr;
    SPDesktop *desktop = nullptr;
    double source_local_size = -1.0;
    double source_rendered_h = -1.0;
    double pasted_local_size = -1.0;
    double pasted_rendered_h = -1.0;
    unsigned created = 0;
};

FreshDocPasteResult pasteNativeOutsideInFreshDoc(char const *svg)
{
    FreshDocPasteResult out;
    auto owned = SPDocument::createNewDocFromMem(std::string(svg));
    if (!owned) return out;
    out.doc = testApplication().document_add(std::move(owned));
    if (!out.doc) return out;
    out.doc->ensureUpToDate();
    out.desktop = testApplication().createDesktop(out.doc, false, true);
    if (!out.desktop) return out;

    auto *src = cast<SPText>(out.doc->getObjectById("m-src"));
    if (!src) return out;
    if (auto const *style = sp_te_style_at_position(src, iteratorAt(src, 0))) {
        out.source_local_size = style->font_size.computed;
    }
    out.source_rendered_h = renderedHeightInDoc(src);

    auto const ids_before = objectIdsInDoc(out.doc);
    auto fragment = extractFragment(out.desktop, src, 0, sp_text_get_length(src));
    if (!fragment || !pasteOutsideFragment(out.desktop, out.doc, *fragment)) return out;
    auto created = textsInDocNot(out.doc, ids_before);
    out.created = static_cast<unsigned>(created.size());
    if (created.size() == 1) {
        auto *pasted = created.front();
        if (auto const *style = sp_te_style_at_position(pasted, iteratorAt(pasted, 0))) {
            out.pasted_local_size = style->font_size.computed;
        }
        out.pasted_rendered_h = renderedHeightInDoc(pasted);
    }
    return out;
}
} // namespace

TEST_F(TextPasteTest, MmViewBoxNativeSourcePasteOutsideMatchesPxDocument)
{
    auto px = pasteNativeOutsideInFreshDoc(kPxDocSvg);
    ASSERT_TRUE(px.doc) << "px control document";
    ASSERT_TRUE(px.desktop) << "px control desktop";
    ASSERT_EQ(px.created, 1u) << "px control: outside paste must create exactly one text";
    EXPECT_NEAR(px.source_local_size, 12.0, 0.05);
    EXPECT_NEAR(px.pasted_local_size, px.source_local_size, 0.05)
        << "px document: the pasted run must keep the source's local size";
    EXPECT_NEAR(px.pasted_rendered_h, px.source_rendered_h,
                std::max(0.05, px.source_rendered_h * 0.03))
        << "px document: the paste must keep the source's rendered size";

    auto mm = pasteNativeOutsideInFreshDoc(kMmDocSvg);
    ASSERT_TRUE(mm.doc) << "mm viewBox document";
    ASSERT_TRUE(mm.desktop) << "mm viewBox desktop";
    ASSERT_EQ(mm.created, 1u) << "mm document: outside paste must create exactly one text";
    EXPECT_NEAR(mm.source_local_size, 12.0, 0.05);
    EXPECT_NEAR(mm.pasted_local_size, mm.source_local_size, 0.05)
        << "mm viewBox document: the pasted run must keep the source's local size";
    EXPECT_NEAR(mm.pasted_rendered_h, mm.source_rendered_h,
                std::max(0.05, mm.source_rendered_h * 0.03))
        << "mm viewBox document: the paste must keep the source's rendered size";
    EXPECT_NEAR(mm.pasted_local_size, px.pasted_local_size, 0.05)
        << "the same native source must produce the same run size in a px and an mm viewBox document";

    for (auto *result : {&px, &mm}) {
        if (result->desktop) {
            result->desktop->getSelection()->clear();
            testApplication().destroyDesktop(result->desktop);
            result->desktop = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// R04-R20 acceptance matrix: physical-size contract.
//
// The unit contract under test: the native fragment carries lengths normalized
// to document-space CSS pixels, D = L * s, where L is the computed local length
// and s is the effective item-to-document scalar
// (SPItem::i2doc_affine().descrim(), already including ancestor and
// document/viewBox mapping). The existing destination application divides by
// the destination scale exactly once. The observable invariant is therefore
// *physical* size, never a raw local number:
//   renderedFontSize(item, i) = computed local size * i2doc scalar
//   renderedHeightInDoc(item)  = glyph ink box already in document/px space
// Cross-document oracles must never expect equal local numbers when the two
// documents have different scales. No test may substitute a constant size or a
// hardcoded 96/25.4 for the measured source size.
// ---------------------------------------------------------------------------

double renderedFontSize(SPItem *item, unsigned index)
{
    auto const *style = sp_te_style_at_position(item, iteratorAt(item, index));
    if (!style) return -1.0;
    return style->font_size.computed * item->i2doc_affine().descrim();
}

double computedLetterSpacing(SPItem *item, unsigned index)
{
    auto const *style = sp_te_style_at_position(item, iteratorAt(item, index));
    return style ? style->letter_spacing.computed : -1.0;
}

double computedWordSpacing(SPItem *item, unsigned index)
{
    auto const *style = sp_te_style_at_position(item, iteratorAt(item, index));
    return style ? style->word_spacing.computed : -1.0;
}

double computedLineHeight(SPItem *item, unsigned index)
{
    auto const *style = sp_te_style_at_position(item, iteratorAt(item, index));
    return style ? style->line_height.computed : -1.0;
}

// Paragraph indentation is the property the receiving run-style API does NOT
// compensate (handoff section 6.4); it is the one that must be pre-divided at
// every boundary that bypasses that API.
double computedTextIndent(SPItem *item, unsigned index)
{
    auto const *style = sp_te_style_at_position(item, iteratorAt(item, index));
    return style ? style->text_indent.computed : -1.0;
}

// Rotation of the item-to-document mapping: proves that a copied rotation is
// not carried with selected text while the scalar size still is. Only the
// linear part is a direction: 2geom's Point * Affine also applies the
// translation (affine.h), so measuring with the full affine reports the
// direction of the translation offset (the fixture's translate(40,40) turned a
// true 25 degrees into 43.11) instead of the item's own rotation.
double i2docAngleDegrees(SPItem *item)
{
    constexpr double kPi = 3.14159265358979323846;
    Geom::Point const axis = Geom::Point(1, 0) * item->i2doc_affine().withoutTranslation();
    return std::atan2(axis.y(), axis.x()) * 180.0 / kPi;
}

// The Text tool's own default typing style: the exact same public call
// TextTool::_setupText() makes, so Destination/default expectations come from
// the tool-style source, never from another paste. A tool style without a
// font-size leaves the created text at the library default
// (SPIFontSize::font_size_default, style-internal.cpp). NF-1: the fallback
// reads that library constant instead of a copied literal, so it cannot go
// stale if the default ever moves.
double textToolDefaultFontSize(SPDesktop *desk)
{
    if (SPCSSAttr *css = desk->getCurrentOrToolStyle("/tools/text", true)) {
        char const *value = sp_repr_css_property(css, "font-size", nullptr);
        double parsed = -1.0;
        if (value) {
            char *end = nullptr;
            double const number = std::strtod(value, &end);
            if (end && end != value) parsed = number; // the tool style is stored in px units
        }
        sp_repr_css_attr_unref(css);
        if (parsed > 0.0) return parsed;
    }
    return SPIFontSize::font_size_default;
}

// ---------------------------------------------------------------------------
// Relative-value oracles. These follow the library's own CSS semantics
// (style-internal.cpp): a unitless line-height is a multiplier, `em`/`ex`
// lengths resolve against the computed font size, and a percentage
// baseline-shift resolves against the computed line height. Comparing the
// ratios to the font size is the only oracle that catches a repair which
// multiplies a relative value by the source scale or drops its unit.
// ---------------------------------------------------------------------------
double effectiveLetterSpacing(SPItem *item, unsigned index)
{
    auto const *style = sp_te_style_at_position(item, iteratorAt(item, index));
    if (!style) return -1.0;
    if (style->letter_spacing.unit == SP_CSS_UNIT_EM) {
        return style->letter_spacing.value * style->font_size.computed;
    }
    if (style->letter_spacing.unit == SP_CSS_UNIT_EX) {
        return style->letter_spacing.value * style->font_size.computed * 0.5;
    }
    return style->letter_spacing.computed;
}

// Unitless line-height keeps its multiplier in `value`; an absolute or
// percentage line-height exposes the resolved px value in `computed`.
double lineHeightMultiplier(SPItem *item, unsigned index)
{
    auto const *style = sp_te_style_at_position(item, iteratorAt(item, index));
    if (!style || style->line_height.normal) return -1.0;
    if (style->line_height.unit == SP_CSS_UNIT_NONE) return style->line_height.value;
    if (style->font_size.computed > 0.0) return style->line_height.computed / style->font_size.computed;
    return -1.0;
}

// Style of the authored, style-bearing item that owns a character position.
// The character source handed out by the layout is an SPString whose own style
// is a cascade copy: it keeps inherited/computed values (font-size, line-height,
// inherited letter-spacing) but loses the authored type/value pair of a
// non-inherited property such as a percentage baseline-shift
// (SPIBaselineShift::cascade copies only `computed` when the property is unset).
// The declared representation therefore lives on the nearest non-SPString item,
// which is exactly the walk the product's source_style() makes.
SPStyle const *authoredStyleAt(SPItem *text, unsigned index)
{
    auto const *layout = te_get_layout(text);
    auto it = iteratorAt(text, index);
    SPObject *source = nullptr;
    layout->getSourceOfCharacter(it, &source, nullptr);
    SPObject const *object = source ? source : text;
    while (object && (is<SPString>(object) || object->style == nullptr)) {
        object = object->parent;
    }
    return object ? object->style : nullptr;
}

// Baseline shift as a fraction of the computed font size. Inkscape resolves a
// percentage against the computed font size (SPIBaselineShift::cascade:
// "Percentage for baseline shift is relative to computed line-height which is
// just font-size"; style-internal.cpp:2870-2873), so `computed` is the
// render-facing shift and `computed / font_size` is the independent ratio
// oracle. Reading the SPString cascade copy instead returned -1 for a valid
// `baseline-shift:50%` because the authored PERCENTAGE type is not inherited.
double baselineShiftRatio(SPItem *item, unsigned index)
{
    SPStyle const *style = authoredStyleAt(item, index);
    if (!style || style->font_size.computed <= 0.0) return -1.0;
    return style->baseline_shift.computed / style->font_size.computed;
}

// ---------------------------------------------------------------------------
// TEMPORARY runtime diagnostics (TEST-ONLY, env-gated by TP_DIAG_DIR).
// Dumps the captured fragment and the pasted DOM for the two open product
// symptoms (em letter-spacing, percentage line-height). With TP_DIAG_DIR unset
// there is no file I/O and no behavior change. Removed before the freeze build.
// ---------------------------------------------------------------------------
void diagnosticDump(std::string const &case_name, std::string const &suffix, std::string const &text)
{
    char const *dir = std::getenv("TP_DIAG_DIR");
    if (!dir || !*dir) {
        return;
    }
    std::string const path = std::string(dir) + "/" + case_name + "." + suffix;
    if (FILE *file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
}

std::string styleFieldDump(SPStyle const *style)
{
    if (!style) {
        return "<null style>\n";
    }
    char buffer[640];
    g_snprintf(buffer, sizeof buffer,
               "font-size   : unit=%d value=%.10g computed=%.10g\n"
               "letter-space: unit=%d value=%.10g computed=%.10g\n"
               "word-space  : unit=%d value=%.10g computed=%.10g\n"
               "line-height : unit=%d value=%.10g computed=%.10g normal=%d\n"
               "baseline    : unit=%d value=%.10g computed=%.10g\n",
               static_cast<int>(style->font_size.unit), style->font_size.value, style->font_size.computed,
               static_cast<int>(style->letter_spacing.unit), style->letter_spacing.value, style->letter_spacing.computed,
               static_cast<int>(style->word_spacing.unit), style->word_spacing.value, style->word_spacing.computed,
               static_cast<int>(style->line_height.unit), style->line_height.value, style->line_height.computed,
               style->line_height.normal ? 1 : 0,
               static_cast<int>(style->baseline_shift.unit), style->baseline_shift.value, style->baseline_shift.computed);
    return std::string(buffer);
}

std::string diagnosticStyleChain(SPItem *text, unsigned index)
{
    auto const *layout = te_get_layout(text);
    if (!layout) {
        return "<no layout>\n";
    }
    SPObject *source = nullptr;
    layout->getSourceOfCharacter(iteratorAt(text, index), &source, nullptr);
    std::string out;
    for (SPObject *object = source ? source : text; object; object = object->parent) {
        Inkscape::XML::Node *repr = object->getRepr();
        if (!repr) {
            continue;
        }
        char const *id = repr->attribute("id");
        char const *style = repr->attribute("style");
        out += std::string(repr->name()) + "#" + (id ? id : "-") + " style=" + (style ? style : "<none>") + "\n";
    }
    return out;
}

// Restore the font display-unit preference (int) even when an ASSERT returns
// early from a test body; an unset key is removed, not left behind.
struct IntPrefGuard {
    Preferences *prefs = Preferences::get();
    char const *key;
    int old;
    IntPrefGuard(char const *k, int fallback) : key(k), old(prefs->getInt(k, fallback)) {}
    ~IntPrefGuard()
    {
        if (old < 0) {
            removePrefIfSet(key);
        } else {
            prefs->setInt(key, old);
        }
    }
};

// One self-contained extra document with its own desktop, destroyed on scope
// exit (same lifetime handling as the mm-viewBox reproduction helper).
struct FreshDoc {
    SPDocument *doc = nullptr;
    SPDesktop *desktop = nullptr;

    explicit FreshDoc(char const *svg)
    {
        auto owned = SPDocument::createNewDocFromMem(std::string(svg));
        if (!owned) return;
        doc = testApplication().document_add(std::move(owned));
        if (!doc) return;
        doc->ensureUpToDate();
        desktop = testApplication().createDesktop(doc, false, true);
    }

    ~FreshDoc()
    {
        if (desktop) {
            desktop->getSelection()->clear();
            testApplication().destroyDesktop(desktop);
        }
    }

    FreshDoc(FreshDoc const &) = delete;
    FreshDoc &operator=(FreshDoc const &) = delete;

    bool ok() const { return doc && desktop; }
    SPText *text(char const *id) const { return doc ? cast<SPText>(doc->getObjectById(id)) : nullptr; }
};

// Clipboard-free fragment insertion (caret) with an explicit paste mode.
bool pasteFragmentAt(SPDesktop *desk, SPDocument *doc, SPItem *destination, unsigned cursor,
                     TP::Fragment const &fragment, UI::TextPasteMode mode)
{
    desk->getSelection()->set(destination);
    desk->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desk->getTool());
    if (!tool || tool->textItem() != destination) return false;
    tool->text_sel_start = iteratorAt(destination, cursor);
    tool->text_sel_end = tool->text_sel_start;
    bool const changed = tool->pasteFragment(fragment, mode);
    doc->ensureUpToDate();
    return changed;
}

// Clipboard-free replacement paste with an explicit mode.
bool pasteFragmentReplacing(SPDesktop *desk, SPDocument *doc, SPItem *destination, unsigned from, unsigned to,
                            TP::Fragment const &fragment, UI::TextPasteMode mode)
{
    desk->getSelection()->set(destination);
    desk->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desk->getTool());
    if (!tool || tool->textItem() != destination) return false;
    tool->text_sel_start = iteratorAt(destination, from);
    tool->text_sel_end = iteratorAt(destination, to);
    bool const changed = tool->pasteFragment(fragment, mode);
    doc->ensureUpToDate();
    return changed;
}

// Clipboard-free outside paste with an explicit mode (Text tool active, no
// edited text object: the real native outside route).
bool pasteFragmentOutside(SPDesktop *desk, SPDocument *doc, TP::Fragment const &fragment, UI::TextPasteMode mode)
{
    desk->getSelection()->clear();
    desk->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desk->getTool());
    if (!tool || tool->textItem() != nullptr) return false;
    bool const changed = tool->pasteFragment(fragment, mode);
    doc->ensureUpToDate();
    return changed;
}

// R04/R11: unscaled px document with three run sizes. Nothing is transformed,
// so local == physical and each copied run must keep its own size; a constant
// substitution or a collapse to the root size must fail.
TEST_F(TextPasteTest, UnscaledPxMultipleSizesPasteOutsidePreserveEachRunSize)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <text id="s" x="10" y="40" style="font-family:serif;font-size:12px">HHHH<tspan id="s24" style="font-size:24px">HHHH</tspan><tspan id="s30" style="font-size:30px">HHHH</tspan></text>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok()) << "fixture document";
    auto *source = fx.text("s");
    ASSERT_TRUE(source);
    ASSERT_EQ(multilineText(source), std::string("HHHHHHHHHHHH"));
    std::vector<double> const source_sizes = {renderedFontSize(source, 0), renderedFontSize(source, 4),
                                               renderedFontSize(source, 8)};
    EXPECT_NEAR(source_sizes[0], 12.0, 0.05);
    EXPECT_NEAR(source_sizes[1], 24.0, 0.05);
    EXPECT_NEAR(source_sizes[2], 30.0, 0.05);

    auto fragment = extractFragment(fx.desktop, source, 0, sp_text_get_length(source));
    ASSERT_TRUE(fragment);
    auto const before = objectIdsInDoc(fx.doc);
    ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
    auto created = textsInDocNot(fx.doc, before);
    ASSERT_EQ(created.size(), 1u) << "outside paste must create exactly one text object";
    auto *pasted = created.front();
    ASSERT_EQ(multilineText(pasted), std::string("HHHHHHHHHHHH"));

    EXPECT_NEAR(renderedFontSize(pasted, 0), source_sizes[0], 0.05) << "run 0 must keep its size";
    EXPECT_NEAR(renderedFontSize(pasted, 4), source_sizes[1], 0.05) << "run 1 must keep its size";
    EXPECT_NEAR(renderedFontSize(pasted, 8), source_sizes[2], 0.05) << "run 2 must keep its size";
    EXPECT_NE(renderedFontSize(pasted, 4), renderedFontSize(pasted, 0))
        << "mixed source sizes must stay mixed, never collapse to the root size";
    EXPECT_TRUE(containsSubstring(characterSignature(pasted, iteratorAt(pasted, 0)), "fam=serif"));
}

// R11: a font size inherited from a group must be captured as the computed
// effective value, while a nested tspan override keeps its own size.
TEST_F(TextPasteTest, InheritedGroupFontSizePasteOutsideUsesComputedValue)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <g id="g" style="font-family:serif;font-size:20px"><text id="s" x="10" y="40" style="fill:#102030">HHHH<tspan id="t" style="font-size:32px">HHHH</tspan></text></g>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok()) << "fixture document";
    auto *source = fx.text("s");
    ASSERT_TRUE(source);
    ASSERT_EQ(multilineText(source), std::string("HHHHHHHH"));
    double const source_inherited = renderedFontSize(source, 0);
    double const source_override = renderedFontSize(source, 4);
    EXPECT_NEAR(source_inherited, 20.0, 0.05) << "inherited group size must be computed";
    EXPECT_NEAR(source_override, 32.0, 0.05);
    EXPECT_TRUE(containsSubstring(characterSignature(source, iteratorAt(source, 0)), "fam=serif"));

    auto fragment = extractFragment(fx.desktop, source, 0, sp_text_get_length(source));
    ASSERT_TRUE(fragment);
    auto const before = objectIdsInDoc(fx.doc);
    ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
    auto created = textsInDocNot(fx.doc, before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    ASSERT_EQ(multilineText(pasted), std::string("HHHHHHHH"));
    EXPECT_NEAR(renderedFontSize(pasted, 0), source_inherited, 0.05)
        << "the group-inherited size must survive as computed, not as the root/default size";
    EXPECT_NEAR(renderedFontSize(pasted, 4), source_override, 0.05);
    EXPECT_TRUE(containsSubstring(characterSignature(pasted, iteratorAt(pasted, 0)), "fam=serif"));
}

// R05: an untransformed mm-viewBox document (document scalar ~3.7795) must keep
// the destination's size for Automatic insertion at the caret and for the first
// logical replacement position. The glyph height is the visible oracle: the
// inserted characters use the same repeated glyph as their neighbours.
TEST_F(TextPasteTest, MmViewBoxAutomaticInsideInsertionAndReplacementKeepDestinationSize)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100">
  <text id="d" x="5" y="20" style="font-family:serif;font-size:16px">HHHHHHHH</text>
  <text id="s" x="5" y="60" style="font-family:serif;font-size:12px">HHHH</text>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok()) << "mm fixture document";
    auto *source = fx.text("s");
    auto *destination = fx.text("d");
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);

    double const doc_scale = source->i2doc_affine().descrim();
    ASSERT_NEAR(doc_scale, 96.0 / 25.4, 0.01) << "fixture: mm viewBox document scalar";
    double const destination_local = renderedFontSize(destination, 0) / doc_scale;
    ASSERT_NEAR(destination_local, 16.0, 0.05);
    double const destination_physical = destination_local * doc_scale;
    double const rendered_before = renderedHeightInDoc(destination);
    ASSERT_GT(rendered_before, 0.0);

    auto fragment = extractFragment(fx.desktop, source, 0, 4);
    ASSERT_TRUE(fragment);

    // Insertion at the caret (end of the destination).
    ASSERT_TRUE(pasteFragmentAt(fx.desktop, fx.doc, destination, 8, *fragment, UI::TextPasteMode::Automatic))
        << "Automatic insertion inside mm text must succeed";
    EXPECT_EQ(multilineText(destination), std::string("HHHHHHHHHHHH"));
    EXPECT_NEAR(renderedHeightInDoc(destination), rendered_before, std::max(0.05, rendered_before * 0.03))
        << "inserted glyphs must render at the destination's physical size";
    EXPECT_NEAR(renderedFontSize(destination, 9), destination_physical, 0.1);
    auto const *inserted = sp_te_style_at_position(destination, iteratorAt(destination, 9));
    ASSERT_TRUE(inserted);
    EXPECT_NEAR(inserted->font_size.computed, destination_local, 0.05)
        << "inserted run must keep the destination's local size, not the source's";

    // Replacement at the first logical position of a selected range.
    ASSERT_TRUE(pasteFragmentReplacing(fx.desktop, fx.doc, destination, 0, 4, *fragment, UI::TextPasteMode::Automatic))
        << "Automatic replacement inside mm text must succeed";
    EXPECT_EQ(multilineText(destination), std::string("HHHHHHHHHHHH"));
    EXPECT_NEAR(renderedHeightInDoc(destination), rendered_before, std::max(0.05, rendered_before * 0.03))
        << "replacement glyphs must render at the destination's physical size";
    EXPECT_NEAR(renderedFontSize(destination, 0), destination_physical, 0.1);
}

// R07: explicit Source inside a differently scaled destination must apply the
// copied *physical* size once, keeping the destination's other characters and
// paragraph formatting untouched.
TEST_F(TextPasteTest, ExplicitSourceInsideScaledDestinationKeepsSourcePhysicalSize)
{
    auto *source = text("src-scaled");        // 12 local inside scale(2) -> 24 physical
    auto *destination = text("dst-scaled");   // 16 local inside scale(0.5) -> 8 physical
    ASSERT_EQ(multilineText(source), std::string("HHHHHHHH"));
    ASSERT_EQ(multilineText(destination), std::string("HHHHHHHH"));
    double const source_scale = source->i2doc_affine().descrim();
    double const destination_scale = destination->i2doc_affine().descrim();
    ASSERT_NEAR(source_scale, 2.0, 0.001);
    ASSERT_NEAR(destination_scale, 0.5, 0.001);
    double const source_physical = renderedFontSize(source, 0);
    double const destination_physical = renderedFontSize(destination, 0);
    ASSERT_NEAR(source_physical, 24.0, 0.05);
    ASSERT_NEAR(destination_physical, 8.0, 0.05);
    auto const destination_chars_before = characterSignatures(destination);

    auto fragment = extractFragment(desktop, source, 0, 4);
    ASSERT_TRUE(fragment);
    ASSERT_TRUE(pasteFragmentAt(desktop, document, destination, 8, *fragment, UI::TextPasteMode::Source))
        << "explicit Source insertion must succeed";
    EXPECT_EQ(multilineText(destination), std::string("HHHHHHHHHHHH"));

    EXPECT_NEAR(renderedFontSize(destination, 9), source_physical, 0.2)
        << "inserted characters must render at the copied source's physical size";
    auto const *inserted = sp_te_style_at_position(destination, iteratorAt(destination, 9));
    ASSERT_TRUE(inserted);
    EXPECT_NEAR(inserted->font_size.computed, source_physical / destination_scale, 0.2)
        << "the normalized source size must cross the destination scale exactly once";
    for (unsigned i = 0; i < 8; ++i) {
        EXPECT_EQ(characterSignature(destination, iteratorAt(destination, i)), destination_chars_before[i])
            << "pre-existing destination character " << i << " must be unchanged";
    }
}

// R08: Without Formatting (Destination) outside must use the current Text tool
// defaults, never the source size, and must be independent of any rich source
// styling that happened to be on the clipboard.
TEST_F(TextPasteTest, DestinationOutsideUsesToolDefaultsNotSourceSize)
{
    auto *source = text("src-scaled"); // physical 24
    auto fragment = extractFragment(desktop, source, 0, 4);
    ASSERT_TRUE(fragment);
    double const source_physical = renderedFontSize(source, 0);
    ASSERT_NEAR(source_physical, 24.0, 0.05);

    auto const before = objectIds();
    ASSERT_TRUE(pasteFragmentOutside(desktop, document, *fragment, UI::TextPasteMode::Destination));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u) << "Destination outside paste must create editable text";
    auto *pasted = created.front();
    double const pasted_physical = renderedFontSize(pasted, 0);
    EXPECT_GT(pasted_physical, 0.0);
    EXPECT_GT(std::abs(pasted_physical - source_physical), 1.0)
        << "Without Formatting must not carry the copied source size";

    // Exact expected default: the Text tool's own style (same source
    // _setupText uses), not the result of another paste that could itself be
    // broken. The main fixture document is unscaled (i2doc scalar 1).
    double const tool_default = textToolDefaultFontSize(desktop);
    ASSERT_GT(tool_default, 0.0) << "the Text tool style must expose a default font size";
    EXPECT_NEAR(pasted_physical, tool_default, 0.05)
        << "Destination outside must produce exactly the tool's default size";

    // The same route with a style-less plain fragment must produce that same
    // default: the default may not depend on the rich source payload.
    auto const plain = TP::from_plain_text("HHHH");
    auto const before_plain = objectIds();
    ASSERT_TRUE(pasteFragmentOutside(desktop, document, plain, UI::TextPasteMode::Destination));
    auto created_plain = textsNotIn(before_plain);
    ASSERT_EQ(created_plain.size(), 1u);
    EXPECT_NEAR(renderedFontSize(created_plain.front(), 0), tool_default, 0.05)
        << "Text tool default size must not depend on source rich data";
}

// R09: px -> mm and mm -> px cross-document paste under Source. The physical
// size is preserved; the local number must be converted by the document scalar
// and must never be copied verbatim between unlike documents.
TEST_F(TextPasteTest, CrossDocumentPxToMmAndMmToPxPreservePhysicalSize)
{
    constexpr char const *kPx = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <text id="s" x="10" y="40" style="font-family:serif;font-size:12px">HHHHHHHH</text>
</svg>)SVG";
    constexpr char const *kMm = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100">
  <text id="s" x="10" y="40" style="font-family:serif;font-size:12px">HHHHHHHH</text>
</svg>)SVG";
    double const mm_scale = 96.0 / 25.4;

    TP::Fragment px_fragment;
    TP::Fragment mm_fragment;
    double px_physical = -1.0;
    double mm_physical = -1.0;
    double px_height = -1.0;
    double mm_height = -1.0;
    {
        FreshDoc px(kPx);
        ASSERT_TRUE(px.ok());
        auto *source = px.text("s");
        ASSERT_TRUE(source);
        px_physical = renderedFontSize(source, 0);
        px_height = renderedHeightInDoc(source);
        EXPECT_NEAR(px_physical, 12.0, 0.05) << "px document: untransformed local size is physical";
        auto fragment = extractFragment(px.desktop, source, 0, 8);
        ASSERT_TRUE(fragment);
        px_fragment = *fragment;
    }
    {
        FreshDoc mm(kMm);
        ASSERT_TRUE(mm.ok());
        auto *source = mm.text("s");
        ASSERT_TRUE(source);
        mm_physical = renderedFontSize(source, 0);
        mm_height = renderedHeightInDoc(source);
        EXPECT_NEAR(mm_physical, 12.0 * mm_scale, 0.1)
            << "mm document: the same local number is physically larger";
        auto fragment = extractFragment(mm.desktop, source, 0, 8);
        ASSERT_TRUE(fragment);
        mm_fragment = *fragment;
    }
    ASSERT_FALSE(px_fragment.empty());
    ASSERT_FALSE(mm_fragment.empty());

    // px -> mm: physical 12 px must become ~3.175 local mm-document units.
    {
        FreshDoc mm(kMm);
        ASSERT_TRUE(mm.ok());
        auto const before = objectIdsInDoc(mm.doc);
        ASSERT_TRUE(pasteOutsideFragment(mm.desktop, mm.doc, px_fragment));
        auto created = textsInDocNot(mm.doc, before);
        ASSERT_EQ(created.size(), 1u);
        auto *pasted = created.front();
        EXPECT_NEAR(renderedHeightInDoc(pasted), px_height, std::max(0.05, px_height * 0.03))
            << "px -> mm paste must preserve the physical glyph height";
        EXPECT_NEAR(renderedFontSize(pasted, 0), px_physical, 0.05);
        auto const *style = sp_te_style_at_position(pasted, iteratorAt(pasted, 0));
        ASSERT_TRUE(style);
        EXPECT_NEAR(style->font_size.computed, 12.0 / mm_scale, 0.05)
            << "the local number must be converted by the mm document scalar";
        EXPECT_GT(std::abs(style->font_size.computed - 12.0), 1.0)
            << "cross-document local numbers must not be copied verbatim";
    }

    // mm -> px: physical 12 mm (45.35 px) must become ~45.35 local px units.
    {
        FreshDoc px(kPx);
        ASSERT_TRUE(px.ok());
        auto const before = objectIdsInDoc(px.doc);
        ASSERT_TRUE(pasteOutsideFragment(px.desktop, px.doc, mm_fragment));
        auto created = textsInDocNot(px.doc, before);
        ASSERT_EQ(created.size(), 1u);
        auto *pasted = created.front();
        EXPECT_NEAR(renderedHeightInDoc(pasted), mm_height, std::max(0.05, mm_height * 0.03))
            << "mm -> px paste must preserve the physical glyph height";
        EXPECT_NEAR(renderedFontSize(pasted, 0), mm_physical, 0.1);
        auto const *style = sp_te_style_at_position(pasted, iteratorAt(pasted, 0));
        ASSERT_TRUE(style);
        EXPECT_NEAR(style->font_size.computed, 12.0 * mm_scale, 0.1);
        EXPECT_GT(std::abs(style->font_size.computed - 12.0), 1.0)
            << "cross-document local numbers must not be copied verbatim";
    }
}

// R10: a serialized fragment is self-contained. The source document and desktop
// are destroyed before the payload is pasted into a fresh document, so no
// pointer to the original object, transform or document may be needed.
TEST_F(TextPasteTest, SerializedFragmentPastesIntoFreshDocumentWithoutSourceContext)
{
    constexpr char const *kSrc = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <text id="s" x="10" y="40" style="font-family:serif;font-size:30px;fill:#123456">HHHHHHHH</text>
</svg>)SVG";
    constexpr char const *kDst = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <text id="d" x="10" y="50" style="font-family:serif;font-size:14px">ab</text>
</svg>)SVG";

    std::string payload;
    double source_physical = -1.0;
    {
        FreshDoc src(kSrc);
        ASSERT_TRUE(src.ok());
        auto *source = src.text("s");
        ASSERT_TRUE(source);
        source_physical = renderedFontSize(source, 0);
        auto fragment = extractFragment(src.desktop, source, 0, 8);
        ASSERT_TRUE(fragment);
        payload = TP::serialize(*fragment);
    }
    auto const parsed = TP::parse(payload);
    ASSERT_TRUE(parsed.has_value()) << "serialized current-format payload must parse";
    EXPECT_EQ(parsed->plain, std::string("HHHHHHHH"));

    FreshDoc dst(kDst);
    ASSERT_TRUE(dst.ok());
    auto const before = objectIdsInDoc(dst.doc);
    ASSERT_TRUE(pasteOutsideFragment(dst.desktop, dst.doc, *parsed));
    auto created = textsInDocNot(dst.doc, before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    EXPECT_EQ(multilineText(pasted), std::string("HHHHHHHH"));
    EXPECT_NEAR(renderedFontSize(pasted, 0), source_physical, 0.1)
        << "the transported payload must carry the normalized physical size";
    EXPECT_NEAR(renderedFontSize(pasted, 0), 30.0, 0.1);
}

// R12: pt, mm, percent and inherited-relative font sizes. Values are compared
// against the source's own computed value with a tight tolerance so premature
// rounding cannot pass.
TEST_F(TextPasteTest, PtMmAndPercentFontSizesNormalizeWithoutPrematureRounding)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <text id="s" x="10" y="40" style="font-family:serif;font-size:12pt">HHHH<tspan id="mm" style="font-size:3.5mm">HHHH</tspan><tspan id="pct" style="font-size:150%">HHHH</tspan></text>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok());
    auto *source = fx.text("s");
    ASSERT_TRUE(source);
    ASSERT_EQ(multilineText(source), std::string("HHHHHHHHHHHH"));
    double const source_pt = renderedFontSize(source, 0);
    double const source_mm = renderedFontSize(source, 4);
    double const source_pct = renderedFontSize(source, 8);
    EXPECT_NEAR(source_pt, 16.0, 0.05) << "12pt is 16px";
    EXPECT_NEAR(source_mm, 3.5 * 96.0 / 25.4, 0.05) << "3.5mm in px units";
    EXPECT_NEAR(source_pct, 24.0, 0.05) << "150% of the inherited 16px parent";

    auto fragment = extractFragment(fx.desktop, source, 0, sp_text_get_length(source));
    ASSERT_TRUE(fragment);
    auto const before = objectIdsInDoc(fx.doc);
    ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
    auto created = textsInDocNot(fx.doc, before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    EXPECT_NEAR(renderedFontSize(pasted, 0), source_pt, 0.01) << "12pt run must not be rounded";
    EXPECT_NEAR(renderedFontSize(pasted, 4), source_mm, 0.01) << "mm run must not be rounded";
    EXPECT_NEAR(renderedFontSize(pasted, 8), source_pct, 0.01) << "percent run must stay resolved";
}

// R13: paragraph semantics. Relative line-height stays relative, absolute line
// height and letter/word spacing are converted once, and an absolute length
// under a 2x source transform reaches the destination at its physical value.
//
// Fixture shape: each paragraph is its own sodipodi:role="line" tspan, which is
// what makes the layout emit a PARAGRAPH_BREAK control code (sp-text.cpp:730-741
// only inserts it before a line tspan that has a tspan predecessor). The former
// direct-text + single trailing line tspan shape produced NO break: the fragment
// was a single paragraph, so the second line's absolute 25px was never a
// paragraph property after paste and the stated two-paragraph oracle was not
// achievable.
TEST_F(TextPasteTest, LineHeightSpacingAndScaledAbsoluteLengthsSurvivePaste)
{
    constexpr char const *kUnscaled = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="480" height="360">
  <text id="s" x="10" y="40" style="font-family:serif;font-size:20px;letter-spacing:2px;word-spacing:4px"><tspan id="l1" sodipodi:role="line" x="10" y="40" style="line-height:150%">HHHH</tspan><tspan id="l2" sodipodi:role="line" x="10" y="80" style="line-height:25px">HHHH</tspan></text>
</svg>)SVG";
    {
        FreshDoc fx(kUnscaled);
        ASSERT_TRUE(fx.ok());
        auto *source = fx.text("s");
        ASSERT_TRUE(source);
        auto const source_paragraphs = paragraphSignatures(source);
        ASSERT_GE(source_paragraphs.size(), 2u) << "two lines must expose at least two layout positions";
        EXPECT_NEAR(computedLineHeight(source, 0), 30.0, 0.05)
            << "line-height:150% of 20px must stay a resolved 30px";
        // The second line's absolute line-height is located by value, not by an
        // assumed index: a paragraph break is its own layout position.
        unsigned second_line = 0;
        for (std::size_t i = 1; i < source_paragraphs.size(); ++i) {
            if (std::abs(computedLineHeight(source, static_cast<unsigned>(i)) - computedLineHeight(source, 0)) > 0.01) {
                second_line = static_cast<unsigned>(i);
                break;
            }
        }
        ASSERT_GT(second_line, 0u) << "fixture must expose the second line's absolute 25px line-height";
        EXPECT_NEAR(computedLineHeight(source, second_line), 25.0, 0.05);
        EXPECT_NEAR(computedLetterSpacing(source, 0), 2.0, 0.01);
        EXPECT_NEAR(computedWordSpacing(source, 0), 4.0, 0.01);

        auto fragment = extractFragment(fx.desktop, source, 0, sp_text_get_length(source));
        ASSERT_TRUE(fragment);
        diagnosticDump("lineheight-unscaled", "fragment.txt", TP::serialize(*fragment));
        diagnosticDump("lineheight-unscaled", "source.svg", sp_repr_save_buf(fx.doc->getReprDoc()).raw());
        diagnosticDump("lineheight-unscaled", "source-style.txt",
                       styleFieldDump(sp_te_style_at_position(source, iteratorAt(source, 0)))
                           + diagnosticStyleChain(source, 0));
        auto const before = objectIdsInDoc(fx.doc);
        ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
        auto created = textsInDocNot(fx.doc, before);
        ASSERT_EQ(created.size(), 1u);
        auto *pasted = created.front();
        diagnosticDump("lineheight-unscaled", "pasted.svg", sp_repr_save_buf(fx.doc->getReprDoc()).raw());
        diagnosticDump("lineheight-unscaled", "pasted-style.txt",
                       styleFieldDump(sp_te_style_at_position(pasted, iteratorAt(pasted, 0)))
                           + diagnosticStyleChain(pasted, 0));
        auto const *pasted_style = sp_te_style_at_position(pasted, iteratorAt(pasted, 0));
        ASSERT_TRUE(pasted_style);

        // The paragraph oracle is physical and relative, never a bare local
        // number: line-height only means something together with the font size
        // it resolves against. In this unscaled document source and destination
        // share scalar 1, so the pasted run must keep the source's 20px size,
        // the 150% paragraph must keep a 1.5x multiplier and physical 30px, and
        // the absolute second paragraph must keep physical 25px. (In the matrix
        // run, against the previous single-paragraph fixture shape, the pasted
        // text reported 60px at every position.)
        double const source_scale = source->i2doc_affine().descrim();
        double const pasted_scale = pasted->i2doc_affine().descrim();
        EXPECT_NEAR(pasted_style->font_size.computed, 20.0, 0.05)
            << "pasted run must keep the source's 20px local size";
        EXPECT_NEAR(computedLineHeight(pasted, 0) * pasted_scale,
                    computedLineHeight(source, 0) * source_scale, 0.05)
            << "relative paragraph must keep its physical line-height";
        EXPECT_NEAR(computedLineHeight(pasted, 0) / pasted_style->font_size.computed, 1.5, 0.01)
            << "150% line-height must stay a 1.5x multiplier, not be re-interpreted as a length";
        unsigned pasted_second = 0;
        auto const pasted_paragraphs = paragraphSignatures(pasted);
        ASSERT_GE(pasted_paragraphs.size(), 2u) << "pasted text must keep both layout lines";
        for (std::size_t i = 1; i < pasted_paragraphs.size(); ++i) {
            if (std::abs(computedLineHeight(pasted, static_cast<unsigned>(i)) - computedLineHeight(pasted, 0)) > 0.01) {
                pasted_second = static_cast<unsigned>(i);
                break;
            }
        }
        if (pasted_second == 0) {
            ADD_FAILURE() << "the pasted text must expose the second line's distinct absolute line-height";
        } else {
            EXPECT_NEAR(computedLineHeight(pasted, pasted_second) * pasted_scale,
                        computedLineHeight(source, second_line) * source_scale, 0.05)
                << "absolute 25px line-height must keep its physical value";
        }
        EXPECT_EQ(paragraphSignatures(pasted), source_paragraphs)
            << "paragraph properties must be preserved exactly";
        EXPECT_NEAR(computedLetterSpacing(pasted, 0), 2.0, 0.01);
        EXPECT_NEAR(computedWordSpacing(pasted, 0), 4.0, 0.01);
    }

    constexpr char const *kScaled = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <g id="g" transform="scale(2)"><text id="s" x="5" y="20" style="font-family:serif;font-size:12px;line-height:25px;letter-spacing:3px">HHHH</text></g>
</svg>)SVG";
    {
        FreshDoc fx(kScaled);
        ASSERT_TRUE(fx.ok());
        auto *source = fx.text("s");
        ASSERT_TRUE(source);
        ASSERT_NEAR(source->i2doc_affine().descrim(), 2.0, 0.001);
        EXPECT_NEAR(renderedFontSize(source, 0), 24.0, 0.05);
        EXPECT_NEAR(computedLetterSpacing(source, 0), 3.0, 0.01);
        EXPECT_NEAR(computedLineHeight(source, 0), 25.0, 0.01);

        auto fragment = extractFragment(fx.desktop, source, 0, 4);
        ASSERT_TRUE(fragment);
        auto const before = objectIdsInDoc(fx.doc);
        ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
        auto created = textsInDocNot(fx.doc, before);
        ASSERT_EQ(created.size(), 1u);
        auto *pasted = created.front();
        EXPECT_NEAR(renderedFontSize(pasted, 0), 24.0, 0.1);
        EXPECT_NEAR(computedLetterSpacing(pasted, 0), 6.0, 0.1)
            << "absolute letter-spacing under a 2x source must be converted once";
        EXPECT_NEAR(computedLineHeight(pasted, 0), 50.0, 0.1)
            << "absolute line-height under a 2x source must be converted once";
    }
}

// R13/RD-07 (BF-2): paragraph CSS written directly to a newly created root and
// paragraph CSS applied to later paragraphs must both cross the
// document-space -> local boundary exactly once. The source is a px document
// (local == physical: paragraph 1 line-height 25px / text-indent 12px,
// paragraph 2 line-height 40px / text-indent 18px); the destination is an
// untransformed 100mm viewBox document with scalar 96/25.4. A raw root write of
// the normalized length inflates by x3.7795; a later-paragraph range
// application that divides the compensated line-height twice shrinks it by the
// same factor. Expected destination locals are derived independently from the
// declared source physical values (physical / scalar), and the rendered
// paragraph spacing is checked against the source document's own rendered
// extent in document-space pixels, never against a raw local number copied
// across documents.
//
// Fixture shape: every paragraph is its own sodipodi:role="line" tspan (the
// canonical Inkscape multi-line text form). A direct text node followed by a
// single trailing line tspan does NOT round-trip through
// sp_te_get_ustring_multiline: pending_line_break is only flushed when a later
// child is visited (text-editing.cpp:1180-1195), so that shape reported the two
// paragraphs as HHHHHHHH in the fixture itself.
TEST_F(TextPasteTest, MmViewBoxMultiParagraphRootAndLaterParagraphLengthsConvertOnce)
{
    constexpr char const *kPxSource = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="480" height="360">
  <text id="s" x="10" y="40" style="font-family:serif;font-size:12px"><tspan id="p1" sodipodi:role="line" x="10" y="40" style="line-height:25px;text-indent:12px">HHHH</tspan><tspan id="p2" sodipodi:role="line" x="10" y="80" style="line-height:40px;text-indent:18px">HHHH</tspan></text>
</svg>)SVG";
    constexpr char const *kMmDestination = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100">
  <rect id="anchor" x="0" y="0" width="10" height="10"/>
</svg>)SVG";
    double const mm_scale = 96.0 / 25.4;

    double source_height = -1.0;
    TP::Fragment fragment;
    {
        FreshDoc px(kPxSource);
        ASSERT_TRUE(px.ok()) << "px source fixture";
        auto *source = px.text("s");
        ASSERT_TRUE(source);
        ASSERT_EQ(multilineText(source), std::string("HHHH\nHHHH")) << "fixture: two paragraphs";
        ASSERT_GE(paragraphSignatures(source).size(), 2u)
            << "fixture: two paragraphs must expose at least two layout positions";
        EXPECT_NEAR(computedLineHeight(source, 0), 25.0, 0.05) << "fixture: physical line-height of paragraph 1";
        EXPECT_NEAR(computedTextIndent(source, 0), 12.0, 0.05) << "fixture: physical text-indent of paragraph 1";
        source_height = renderedHeightInDoc(source);
        ASSERT_GT(source_height, 0.0);

        auto extracted = extractFragment(px.desktop, source, 0, sp_text_get_length(source));
        ASSERT_TRUE(extracted) << "the px source must produce a native fragment";
        fragment = *extracted;
        ASSERT_EQ(fragment.paragraphs.size(), 2u) << "the native fragment must carry both paragraphs";
    }

    FreshDoc mm(kMmDestination);
    ASSERT_TRUE(mm.ok()) << "mm destination fixture";
    auto *anchor = cast<SPItem>(mm.doc->getObjectById("anchor"));
    ASSERT_TRUE(anchor) << "mm destination anchor";
    double const destination_scale = anchor->i2doc_affine().descrim();
    ASSERT_NEAR(destination_scale, mm_scale, 0.01) << "fixture: mm viewBox document scalar";

    auto const before = objectIdsInDoc(mm.doc);
    ASSERT_TRUE(pasteOutsideFragment(mm.desktop, mm.doc, fragment))
        << "multi-paragraph outside paste into the mm document must succeed";
    auto created = textsInDocNot(mm.doc, before);
    ASSERT_EQ(created.size(), 1u) << "outside paste must create exactly one text object";
    auto *pasted = created.front();
    ASSERT_EQ(multilineText(pasted), std::string("HHHH\nHHHH"));
    auto const pasted_paragraphs = paragraphSignatures(pasted);
    ASSERT_GE(pasted_paragraphs.size(), 2u) << "the pasted text must keep both paragraphs";

    // Locate the second paragraph by its distinct line-height instead of an
    // assumed layout index (a paragraph break is its own layout position).
    unsigned second_paragraph = 0;
    for (std::size_t i = 1; i < pasted_paragraphs.size(); ++i) {
        if (std::abs(computedLineHeight(pasted, static_cast<unsigned>(i)) - computedLineHeight(pasted, 0)) > 0.01) {
            second_paragraph = static_cast<unsigned>(i);
            break;
        }
    }
    ASSERT_GT(second_paragraph, 0u) << "fixture: the second paragraph's line-height must differ from the first";

    // Direct root write: physical 25px / 12px must become local 25/s and 12/s.
    EXPECT_NEAR(computedLineHeight(pasted, 0), 25.0 / destination_scale, 0.05)
        << "root paragraph line-height must be de-normalized once (physical 25px -> local ~6.61)";
    EXPECT_NEAR(computedTextIndent(pasted, 0), 12.0 / destination_scale, 0.05)
        << "root paragraph text-indent must be de-normalized once (physical 12px -> local ~3.175)";
    // Later paragraph: range application must convert once as well, never twice.
    EXPECT_NEAR(computedLineHeight(pasted, second_paragraph), 40.0 / destination_scale, 0.05)
        << "later-paragraph line-height must be converted once (physical 40px -> local ~10.58)";
    EXPECT_NEAR(computedTextIndent(pasted, second_paragraph), 18.0 / destination_scale, 0.05)
        << "later-paragraph text-indent must be converted once (physical 18px -> local ~4.76)";

    // Independent physical-unit oracle: the local values must render back to
    // the source's physical values, and the whole two-paragraph extent must
    // match the source document's rendered document-space height.
    EXPECT_NEAR(computedLineHeight(pasted, 0) * destination_scale, 25.0, 0.1)
        << "root line-height must render at the declared physical 25px";
    EXPECT_NEAR(computedTextIndent(pasted, 0) * destination_scale, 12.0, 0.1)
        << "root text-indent must render at the declared physical 12px";
    EXPECT_NEAR(computedLineHeight(pasted, second_paragraph) * destination_scale, 40.0, 0.1)
        << "later-paragraph line-height must render at the declared physical 40px";
    EXPECT_NEAR(computedTextIndent(pasted, second_paragraph) * destination_scale, 18.0, 0.1)
        << "later-paragraph text-indent must render at the declared physical 18px";
    EXPECT_NEAR(renderedHeightInDoc(pasted), source_height, std::max(0.05, source_height * 0.03))
        << "rendered paragraph spacing in the mm document must match the source's physical extent";
}

// R14: nested uniform scales are applied exactly once; pure rotation must not
// change the scalar size and must not be carried with the selected text; a
// transformed destination layer is converted once as well.
TEST_F(TextPasteTest, NestedScaleRotationAndTransformedDestinationConvertOnce)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <g id="rot" transform="translate(40,40) rotate(25)"><g id="n1" transform="scale(2)"><g id="n2" transform="scale(3)"><text id="s" x="1" y="5" style="font-family:serif;font-size:10px">HHHH</text></g></g></g>
  <g id="dg" transform="scale(2)"><text id="d" x="20" y="100" style="font-family:serif;font-size:16px">HHHHHHHH</text></g>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok());
    auto *source = fx.text("s");
    auto *destination = fx.text("d");
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);

    ASSERT_NEAR(source->i2doc_affine().descrim(), 6.0, 0.001) << "fixture: nested 2x * 3x scale";
    EXPECT_NEAR(renderedFontSize(source, 0), 60.0, 0.1);
    EXPECT_NEAR(std::abs(i2docAngleDegrees(source)), 25.0, 1.0) << "fixture: rotated 25 degrees";

    auto fragment = extractFragment(fx.desktop, source, 0, 4);
    ASSERT_TRUE(fragment);
    auto const before = objectIdsInDoc(fx.doc);
    ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
    auto created = textsInDocNot(fx.doc, before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    EXPECT_NEAR(renderedFontSize(pasted, 0), 60.0, 0.1)
        << "nested source scales must be applied exactly once";
    EXPECT_NEAR(pasted->i2doc_affine().descrim(), 1.0, 0.001)
        << "the new text is unscaled in a px document";
    EXPECT_LT(std::abs(i2docAngleDegrees(pasted)), 1.0)
        << "pure source rotation must not be copied with the selected text";

    // Transformed destination layer: Automatic inside adopts the destination's
    // local typing size (16), not the copied source size (60 physical / 6 scale).
    double const destination_physical = renderedFontSize(destination, 0);
    ASSERT_NEAR(destination_physical, 32.0, 0.05);
    ASSERT_TRUE(pasteFragmentAt(fx.desktop, fx.doc, destination, 8, *fragment, UI::TextPasteMode::Automatic));
    EXPECT_EQ(multilineText(destination), std::string("HHHHHHHHHHHH"));
    EXPECT_NEAR(renderedFontSize(destination, 9), destination_physical, 0.1)
        << "inserted characters must adopt the transformed destination's physical typing size";
    auto const *inserted = sp_te_style_at_position(destination, iteratorAt(destination, 9));
    ASSERT_TRUE(inserted);
    EXPECT_NEAR(inserted->font_size.computed, 16.0, 0.05);
}

// R15: an empty destination text and a caret at the end. Both must use the
// existing typing-style policy (the style the layout attributes at the caret),
// never an unrelated root/default size.
TEST_F(TextPasteTest, EmptyDestinationAndCaretAtEndAdoptTypingStyle)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <text id="empty" x="10" y="40" style="font-family:serif;font-size:18px;fill:#334455"/>
  <text id="d" x="10" y="80" style="font-family:serif;font-size:14px">ab<tspan id="hot" style="font-size:22px">cd</tspan></text>
  <text id="s" x="10" y="120" style="font-family:serif;font-size:12px">HHHH</text>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok());
    auto *empty = fx.text("empty");
    auto *destination = fx.text("d");
    auto *source = fx.text("s");
    ASSERT_TRUE(empty);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(source);
    ASSERT_EQ(multilineText(empty), std::string(""));
    auto const destination_chars_before = characterSignatures(destination);
    ASSERT_EQ(destination_chars_before.size(), 4u);

    auto fragment = extractFragment(fx.desktop, source, 0, 4);
    ASSERT_TRUE(fragment);

    ASSERT_TRUE(pasteFragmentAt(fx.desktop, fx.doc, empty, 0, *fragment, UI::TextPasteMode::Automatic))
        << "pasting into an empty destination text must succeed";
    EXPECT_EQ(multilineText(empty), std::string("HHHH"));
    EXPECT_NEAR(renderedFontSize(empty, 0), 18.0, 0.05)
        << "empty destination text must keep its own typing style, not the source size";

    ASSERT_TRUE(pasteFragmentAt(fx.desktop, fx.doc, destination, 4, *fragment, UI::TextPasteMode::Automatic))
        << "caret-at-end insertion must succeed";
    EXPECT_EQ(multilineText(destination), std::string("abcdHHHH"));
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_EQ(characterSignature(destination, iteratorAt(destination, i)), destination_chars_before[i])
            << "pre-existing destination character " << i << " must be unchanged";
    }
    EXPECT_NEAR(renderedFontSize(destination, 4), 22.0, 0.05)
        << "caret at end must adopt the style at the end (22px), not the root 14px";
    EXPECT_NEAR(renderedFontSize(destination, 0), 14.0, 0.05);
}

// R16: Unicode, RTL and combining marks keep their characters, run boundaries
// and sizes through the normalized fragment.
TEST_F(TextPasteTest, UnicodeRtlAndCombiningRunsKeepCharactersAndSize)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <text id="s" x="10" y="40" style="font-family:serif;font-size:15px">&#x5e9;&#x5dc;&#x5d5;&#x5dd; e&#x301;<tspan id="big" style="font-size:21px">&#x3a9;&#x1f600;</tspan></text>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok());
    auto *source = fx.text("s");
    ASSERT_TRUE(source);
    std::string const source_text = multilineText(source);
    ASSERT_TRUE(containsSubstring(source_text, "\u05e9\u05dc\u05d5\u05dd"));
    ASSERT_TRUE(containsSubstring(source_text, "e\u0301"));
    unsigned const omega = logicalIndexOf(source, "\u03a9");
    ASSERT_NE(omega, std::numeric_limits<unsigned>::max());
    double const source_small = renderedFontSize(source, 0);
    double const source_big = renderedFontSize(source, omega);
    EXPECT_NEAR(source_small, 15.0, 0.05);
    EXPECT_NEAR(source_big, 21.0, 0.05);

    auto fragment = extractFragment(fx.desktop, source, 0, sp_text_get_length(source));
    ASSERT_TRUE(fragment);
    auto const before = objectIdsInDoc(fx.doc);
    ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
    auto created = textsInDocNot(fx.doc, before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    EXPECT_EQ(multilineText(pasted), source_text) << "logical characters and combining marks must survive";
    unsigned const pasted_omega = logicalIndexOf(pasted, "\u03a9");
    ASSERT_NE(pasted_omega, std::numeric_limits<unsigned>::max());
    EXPECT_NEAR(renderedFontSize(pasted, 0), source_small, 0.05);
    EXPECT_NEAR(renderedFontSize(pasted, pasted_omega), source_big, 0.05);
    EXPECT_TRUE(containsSubstring(characterSignature(pasted, iteratorAt(pasted, pasted_omega)), "fam=serif"));
}

// R19 (representation level): the root-approved native format is version 2 with
// the same MIME; version 1 has no source-coordinate metadata and must never be
// reinterpreted as normalized data; unknown versions are rejected.
TEST(TextPasteFragmentTest, CurrentFormatIsVersionTwoAndLegacyOneIsUnsupported)
{
    EXPECT_EQ(TP::FORMAT_VERSION, 2u) << "root-approved native fragment format is version 2";
    EXPECT_EQ(std::string(TP::MIME_TYPE), std::string("application/x-vac-studio-text-fragment"))
        << "the version bump must keep the existing MIME";

    TP::Fragment fragment;
    TP::Paragraph paragraph;
    TP::Run run;
    run.text = "Beta";
    run.style = "font-family:monospace;font-size:24px;";
    paragraph.runs = {run};
    fragment.paragraphs = {paragraph};
    fragment.plain = "Beta";

    auto const payload = TP::serialize(fragment);
    auto const nl = payload.find('\n');
    ASSERT_NE(nl, std::string::npos) << "payload must start with a header line";
    EXPECT_EQ(payload.substr(0, nl), std::string("vac-text-fragment\t") + std::to_string(TP::FORMAT_VERSION))
        << "serialized header must name the current version";
    auto const parsed = TP::parse(payload);
    ASSERT_TRUE(parsed.has_value()) << "the current version must round-trip";
    ASSERT_EQ(parsed->paragraphs.size(), 1u);
    ASSERT_EQ(parsed->paragraphs[0].runs.size(), 1u);
    EXPECT_EQ(parsed->plain, std::string("Beta"));
    EXPECT_TRUE(containsSubstring(parsed->paragraphs[0].runs[0].style, "font-size:24px;"));

    EXPECT_FALSE(TP::parse("vac-text-fragment\t1\nP\t\nR\tfont-size:12px\tBeta\n").has_value())
        << "version 1 must be unsupported rich data, never silently reinterpreted as normalized";
    EXPECT_FALSE(TP::parse("vac-text-fragment\t99\nP\t\nR\t\tBeta\n").has_value())
        << "unknown future versions must be rejected";
}

// R19/R05: invalid or nonfinite normalized lengths must be prevalidated before
// any insertion: no partial text, no document mutation, no Undo record. The
// payload is built through the real serializer/parser, so this also pins the
// bounded-representation contract.
TEST_F(TextPasteTest, InvalidNormalizedLengthsAbortPasteWithoutMutationOrUndo)
{
    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const xml_before = xml();
    std::string const text_before = multilineText(destination);
    char const *const bad_styles[] = {"font-size:nan;", "font-size:inf;", "font-size:-24px;"};

    for (char const *style : bad_styles) {
        TP::Fragment fragment;
        TP::Paragraph paragraph;
        TP::Run run;
        run.text = "HHHH";
        run.style = style;
        paragraph.runs = {run};
        fragment.paragraphs = {paragraph};
        fragment.plain = "HHHH";
        auto const parsed = TP::parse(TP::serialize(fragment));
        // RD-09: an invalid normalized length must be refused explicitly at one
        // layer. Either the parser rejects the payload, or it hands the
        // declaration through and the paste layer refuses it. A declaration
        // silently dropped by the sanitizer that then lets paste report success
        // is not an accepted contract, so this test never passes merely because
        // no fragment reached the paste call.
        std::string refused_by;
        if (!parsed.has_value()) {
            refused_by = "parser";
        } else if (!pasteFragmentAt(desktop, document, destination, 3, *parsed, UI::TextPasteMode::Source)) {
            refused_by = "paste";
        }
        EXPECT_FALSE(refused_by.empty())
            << style << " must be refused by the parser or by the paste layer";
        document->ensureUpToDate();
        EXPECT_EQ(xml(), xml_before) << style << " must not partially mutate the document";
        EXPECT_EQ(multilineText(destination), text_before) << style << " must not insert text";
    }
    EXPECT_FALSE(DocumentUndo::undo(document)) << "invalid pastes must not leave an Undo record";
}

// R14/R19: a reflection has a usable scalar magnitude and must not yield a
// negative size; a degenerate (zero) scale must abort the conversion rather
// than being clamped to 1 or producing nonfinite geometry.
TEST_F(TextPasteTest, ReflectionAndDegenerateSourceScaleAreNotSilentlyClamped)
{
    constexpr char const *kReflect = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <g id="g" transform="scale(-2)"><text id="s" x="-60" y="40" style="font-family:serif;font-size:12px">HHHH</text></g>
</svg>)SVG";
    {
        FreshDoc fx(kReflect);
        ASSERT_TRUE(fx.ok());
        auto *source = fx.text("s");
        ASSERT_TRUE(source);
        ASSERT_NEAR(source->i2doc_affine().descrim(), 2.0, 0.001) << "fixture: reflected uniform scale";
        EXPECT_NEAR(renderedFontSize(source, 0), 24.0, 0.05);

        auto fragment = extractFragment(fx.desktop, source, 0, 4);
        ASSERT_TRUE(fragment) << "a reflected source still has a usable scalar magnitude";
        auto const before = objectIdsInDoc(fx.doc);
        ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
        auto created = textsInDocNot(fx.doc, before);
        ASSERT_EQ(created.size(), 1u);
        auto *pasted = created.front();
        double const physical = renderedFontSize(pasted, 0);
        EXPECT_GT(physical, 0.0) << "a reflection must never produce a negative font size";
        EXPECT_NEAR(physical, 24.0, 0.1) << "the scalar magnitude must be preserved";
        EXPECT_FALSE(documentHasNonFiniteLength(fx.doc)) << "no nonfinite length may reach the document";
        EXPECT_FALSE(documentHasNonPositiveFontSize(fx.doc))
            << "no sign-flipped (negative) font size may reach the document";
    }

    constexpr char const *kZero = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <g id="g" transform="scale(0)"><text id="s" x="0" y="40" style="font-family:serif;font-size:12px">HHHH</text></g>
</svg>)SVG";
    {
        FreshDoc fx(kZero);
        ASSERT_TRUE(fx.ok());
        auto *source = fx.text("s");
        ASSERT_TRUE(source);
        ASSERT_NEAR(source->i2doc_affine().descrim(), 0.0, 0.0001) << "fixture: degenerate scale";
        auto const before = objectIdsInDoc(fx.doc);
        auto fragment = extractFragment(fx.desktop, source, 0, 4);
        // RD-09: extraction may legitimately refuse a degenerate source, or it
        // may hand back a fragment that the paste layer must refuse. Either way
        // the refusal is explicit: one of the two layers must reject the
        // degenerate scale, never clamp it to 1 and never require an impossible
        // fragment to prove the contract.
        std::string const xml_before = sp_repr_save_buf(fx.doc->getReprDoc()).raw();
        std::string refused_by;
        if (!fragment.has_value()) {
            refused_by = "extraction";
        } else if (!pasteOutsideFragment(fx.desktop, fx.doc, *fragment)) {
            refused_by = "paste";
        }
        EXPECT_FALSE(refused_by.empty())
            << "a degenerate source scale must be refused by extraction or by the paste layer, not clamped to 1";
        EXPECT_TRUE(textsInDocNot(fx.doc, before).empty()) << "no partial text may be created";
        EXPECT_FALSE(documentHasNonFiniteLength(fx.doc)) << "no nonfinite length may reach the document";
        EXPECT_EQ(sp_repr_save_buf(fx.doc->getReprDoc()).raw(), xml_before)
            << "a refused degenerate paste must not change the document";
    }
}

// R12 (display-unit leg): the UI font display unit is a presentation setting;
// it must not change the normalized fragment or the pasted physical size.
TEST_F(TextPasteTest, DisplayUnitPreferenceDoesNotChangeNormalizedPhysicalSize)
{
    IntPrefGuard unit_guard("/options/font/unitType", -1);
    auto *source = text("src-scaled");
    std::vector<std::string> payloads;
    std::vector<double> physicals;
    // Verified enum values from style-internal.h: PX=1, PT=2, MM=4. Each is a
    // pure presentation setting; none may change the normalized fragment or the
    // pasted physical size.
    for (int const unit : {static_cast<int>(SP_CSS_UNIT_PX), static_cast<int>(SP_CSS_UNIT_PT),
                           static_cast<int>(SP_CSS_UNIT_MM)}) {
        Preferences::get()->setInt("/options/font/unitType", unit);
        auto fragment = extractFragment(desktop, source, 0, 4);
        ASSERT_TRUE(fragment) << "display unit " << unit;
        auto const payload = TP::serialize(*fragment);
        auto const before = objectIds();
        ASSERT_TRUE(pasteFragmentOutside(desktop, document, *fragment, UI::TextPasteMode::Automatic))
            << "display unit " << unit;
        auto created = textsNotIn(before);
        ASSERT_EQ(created.size(), 1u) << "display unit " << unit;
        payloads.push_back(payload);
        physicals.push_back(renderedFontSize(created.front(), 0));
    }
    ASSERT_EQ(payloads.size(), 3u);

    EXPECT_EQ(payloads[1], payloads[0]) << "px and pt display units must emit the same normalized fragment";
    EXPECT_EQ(payloads[2], payloads[0]) << "px and mm display units must emit the same normalized fragment";
    EXPECT_NEAR(physicals[1], physicals[0], 0.01) << "pt must not change the pasted physical size";
    EXPECT_NEAR(physicals[2], physicals[0], 0.01) << "mm must not change the pasted physical size";
    EXPECT_NEAR(physicals[0], 24.0, 0.1);
}

// R19/R05 (destination side): a degenerate destination scale must abort before
// any character is inserted: no partial text, no XML change, no Undo record.
// The source is a normal 12px text; it is never rendered through the singular
// mapping, only pasted into the scale(0) destination.
TEST_F(TextPasteTest, DegenerateDestinationScaleAbortsPasteWithoutMutationOrUndo)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <text id="s" x="10" y="40" style="font-family:serif;font-size:12px">HHHH</text>
  <g id="dz" transform="scale(0)"><text id="d" x="0" y="60" style="font-family:serif;font-size:16px">abcd</text></g>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok()) << "degenerate destination fixture";
    auto *source = fx.text("s");
    auto *destination = fx.text("d");
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_EQ(multilineText(destination), std::string("abcd"));
    ASSERT_NEAR(destination->i2doc_affine().descrim(), 0.0, 1e-9) << "fixture: degenerate destination scale";

    auto fragment = extractFragment(fx.desktop, source, 0, 4);
    ASSERT_TRUE(fragment) << "the healthy source must still yield a fragment";
    auto const ids_before = objectIdsInDoc(fx.doc);
    std::string const xml_before = sp_repr_save_buf(fx.doc->getReprDoc()).raw();

    EXPECT_FALSE(pasteFragmentAt(fx.desktop, fx.doc, destination, 2, *fragment, UI::TextPasteMode::Automatic))
        << "Automatic-inside must refuse a degenerate destination, not clamp the scale to 1";
    EXPECT_FALSE(pasteFragmentAt(fx.desktop, fx.doc, destination, 2, *fragment, UI::TextPasteMode::Source))
        << "explicit Source must also refuse a degenerate destination";

    fx.doc->ensureUpToDate();
    EXPECT_EQ(multilineText(destination), std::string("abcd")) << "no partial character may be inserted";
    EXPECT_EQ(sp_repr_save_buf(fx.doc->getReprDoc()).raw(), xml_before)
        << "a refused paste must not mutate the document";
    EXPECT_EQ(objectIdsInDoc(fx.doc), ids_before) << "no object may be created or removed";
    EXPECT_FALSE(DocumentUndo::undo(fx.doc)) << "a refused paste must not leave an Undo record";
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(fx.desktop->getTool());
    ASSERT_TRUE(tool);
    EXPECT_EQ(tool->textItem(), destination) << "a refused inside paste must keep editing the destination";
    EXPECT_TRUE(fx.desktop->getSelection()->includes(destination))
        << "a refused inside paste must keep the destination selected";
}

// BLOCKER-4 (creation-failure contract): an outside paste whose prospective
// creation context is unusable must refuse without leaving any object behind,
// without an Undo record and without disturbing the user's existing selection
// or the Text tool's editing state. The plain outside route creates on the
// current layer, which is pre-validated before _setupText() (text-tool.cpp
// prospective_scale). The post-_setupText destination-scale return is only
// reachable through the shape-group / text-on-path creation contexts, whose
// private canvas-event state cannot be driven from this suite; the fail_paste()
// cleanup that now precedes that return (delete created object, drop the tool
// pointer, restore the pre-creation selection) is covered by the source review
// in review-closure/REPORT.md.
TEST_F(TextPasteTest, DegenerateCreationLayerRefusesOutsidePasteAndKeepsSelection)
{
    // A healthy, self-contained fragment from a normal document: the paste must
    // fail because of the destination, never because of the payload.
    auto *healthy = text("src");
    ASSERT_TRUE(healthy);
    auto fragment = extractFragment(desktop, healthy, 0, 4);
    ASSERT_TRUE(fragment);

    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <g id="layer1" inkscape:groupmode="layer" inkscape:label="Layer 1" transform="scale(0)">
    <rect id="keep" x="5" y="5" width="40" height="20" style="fill:#336699"/>
  </g>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok()) << "degenerate-layer fixture";
    auto const *layer = fx.desktop->layerManager().currentLayer();
    ASSERT_TRUE(layer) << "fixture: the degenerate layer must be the current layer";
    EXPECT_NEAR(layer->i2doc_affine().descrim(), 0.0, 1e-9) << "fixture: degenerate creation layer";
    auto *keep = fx.doc->getObjectById("keep");
    ASSERT_TRUE(keep);
    fx.desktop->getSelection()->set(keep);
    fx.desktop->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(fx.desktop->getTool());
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool->textItem(), nullptr) << "a selected non-text object must not start text editing";

    auto const ids_before = objectIdsInDoc(fx.doc);
    std::string const xml_before = sp_repr_save_buf(fx.doc->getReprDoc()).raw();

    EXPECT_FALSE(tool->pasteFragment(*fragment, UI::TextPasteMode::Automatic))
        << "the degenerate creation layer must refuse the paste, not clamp the scale to 1";
    fx.doc->ensureUpToDate();

    EXPECT_TRUE(textsInDocNot(fx.doc, ids_before).empty()) << "no partial text object may be created";
    EXPECT_EQ(objectIdsInDoc(fx.doc), ids_before) << "no object may be created or removed";
    EXPECT_EQ(sp_repr_save_buf(fx.doc->getReprDoc()).raw(), xml_before)
        << "a refused creation must not change the document";
    EXPECT_FALSE(DocumentUndo::undo(fx.doc)) << "a refused creation must not leave an Undo record";
    EXPECT_TRUE(fx.desktop->getSelection()->includes(keep))
        << "the pre-existing selection must be preserved when creation is refused";
    EXPECT_EQ(tool->textItem(), nullptr) << "no tool pointer may remain after a refused creation";
}

// R12/R13 (relative leg): on a 2x-scaled source, `letter-spacing:0.5em`, a
// unitless line-height multiplier and a percentage baseline shift must keep
// their ratio to the font size. The bounded core approach still has to carry
// relative values through the receiving scaler; this test exposes a repair that
// multiplies a relative value by the source scale or drops its unit.
// Inkscape resolves a percentage baseline shift against the computed font size
// (SPIBaselineShift::cascade, style-internal.cpp:2870-2873), so the 50% source
// resolves to 6px local / 12px physical and the ratio oracle is 0.5, not the
// line-height-multiplied 0.75.
TEST_F(TextPasteTest, RelativeSpacingAndLineHeightKeepRatioAcrossScaledPaste)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <g id="g" transform="scale(2)"><text id="s" x="5" y="20" style="font-family:serif;font-size:12px;letter-spacing:0.5em;line-height:1.5;baseline-shift:50%">HHHH</text></g>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok()) << "scaled relative-value fixture";
    auto *source = fx.text("s");
    ASSERT_TRUE(source);
    ASSERT_NEAR(source->i2doc_affine().descrim(), 2.0, 0.001);

    auto const *source_style = authoredStyleAt(source, 0);
    ASSERT_TRUE(source_style);
    double const source_local_font = source_style->font_size.computed;
    EXPECT_NEAR(source_local_font, 12.0, 0.05);
    EXPECT_NEAR(renderedFontSize(source, 0), 24.0, 0.05);

    double const source_spacing_ratio = effectiveLetterSpacing(source, 0) / source_local_font;
    double const source_line_height = lineHeightMultiplier(source, 0);
    double const source_baseline_ratio = baselineShiftRatio(source, 0);
    EXPECT_NEAR(source_spacing_ratio, 0.5, 0.01) << "fixture: 0.5em is half the font size";
    EXPECT_NEAR(source_line_height, 1.5, 0.01) << "fixture: unitless line-height is 1.5x";
    EXPECT_NEAR(source_baseline_ratio, 0.5, 0.01) << "fixture: 50% of the 12px font size";
    EXPECT_NEAR(source_style->baseline_shift.computed, 6.0, 0.05)
        << "fixture: the render-facing 50% shift is 6px local";

    auto fragment = extractFragment(fx.desktop, source, 0, 4);
    ASSERT_TRUE(fragment);
    diagnosticDump("relspacing", "fragment.txt", TP::serialize(*fragment));
    diagnosticDump("relspacing", "source.svg", sp_repr_save_buf(fx.doc->getReprDoc()).raw());
    diagnosticDump("relspacing", "source-style.txt",
                   styleFieldDump(authoredStyleAt(source, 0)) + diagnosticStyleChain(source, 0));
    auto const before = objectIdsInDoc(fx.doc);
    ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
    auto created = textsInDocNot(fx.doc, before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    diagnosticDump("relspacing", "pasted.svg", sp_repr_save_buf(fx.doc->getReprDoc()).raw());
    diagnosticDump("relspacing", "pasted-style.txt",
                   styleFieldDump(sp_te_style_at_position(pasted, iteratorAt(pasted, 0)))
                       + diagnosticStyleChain(pasted, 0));
    auto const *pasted_style = authoredStyleAt(pasted, 0);
    ASSERT_TRUE(pasted_style);

    EXPECT_NEAR(pasted_style->font_size.computed, 24.0, 0.1) << "physical font size must be preserved";
    EXPECT_NEAR(effectiveLetterSpacing(pasted, 0) / pasted_style->font_size.computed, source_spacing_ratio, 0.01)
        << "em letter-spacing must stay the same fraction of the font size";
    EXPECT_NEAR(effectiveLetterSpacing(pasted, 0), 12.0, 0.1)
        << "0.5em of the 24px physical font is 12px, not 0.5 * source scale";
    EXPECT_NEAR(lineHeightMultiplier(pasted, 0), source_line_height, 0.01)
        << "a unitless line-height must stay a multiplier, never be scaled as a length";
    EXPECT_NEAR(baselineShiftRatio(pasted, 0), source_baseline_ratio, 0.01)
        << "a percentage baseline shift must keep its font-size ratio";
    EXPECT_NEAR(baselineShiftRatio(pasted, 0), 0.5, 0.01);
    // Independent visual oracle: the rendered shift in document-space px, not
    // just the ratio, must survive the 2x source scale.
    EXPECT_NEAR(pasted_style->baseline_shift.computed * pasted->i2doc_affine().descrim(),
                source_style->baseline_shift.computed * source->i2doc_affine().descrim(), 0.1)
        << "the rendered baseline shift must keep its physical value";
    EXPECT_FALSE(documentHasNonFiniteLength(fx.doc));
}

// R11/R14/R15: a nested tspan under a uniformly scaled parent keeps its own
// effective glyph size, and a collapsed caret exactly at a tspan/root boundary
// must adopt the character at the caret's own position, not a neighbouring
// run's size (the get_common_ancestor trap in REVIEW_DESIGN RD-02/RD-03).
//
// Native caret policy asserted here (SUPERVISOR_CHECKPOINT_B boundary
// contract): a collapsed caret uses the native sp_te_style_at_position() at
// the caret position itself, paired with that position's own item context. The
// preceding-character fallback exists only at the true layout end, where no
// character occupies the position (covered by
// EmptyDestinationAndCaretAtEndAdoptTypingStyle, which keeps its 22px end
// expectation). The fixture preconditions below assert both raw sides of the
// boundary (index 1 = 14px, index 2 = 22px), so each inserted run's expectation
// is the native value at the caret, not an assumption.
TEST_F(TextPasteTest, NestedTspanAndCaretBoundaryKeepEffectiveGlyphSize)
{
    constexpr char const *kScaled = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <g id="g" transform="scale(2)"><text id="s" x="5" y="20" style="font-family:serif;font-size:12px">HHHH<tspan id="t" style="font-size:18px">HHHH</tspan></text></g>
</svg>)SVG";
    {
        FreshDoc fx(kScaled);
        ASSERT_TRUE(fx.ok()) << "scaled nested-tspan fixture";
        auto *source = fx.text("s");
        ASSERT_TRUE(source);
        ASSERT_EQ(multilineText(source), std::string("HHHHHHHH"));
        double const root_size = renderedFontSize(source, 0);
        double const nested_size = renderedFontSize(source, 4);
        EXPECT_NEAR(root_size, 24.0, 0.05);
        EXPECT_NEAR(nested_size, 36.0, 0.05);
        EXPECT_GT(std::abs(nested_size - root_size), 1.0) << "fixture: the tspan override must differ";

        auto fragment = extractFragment(fx.desktop, source, 0, 8);
        ASSERT_TRUE(fragment);
        auto const before = objectIdsInDoc(fx.doc);
        ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
        auto created = textsInDocNot(fx.doc, before);
        ASSERT_EQ(created.size(), 1u);
        auto *pasted = created.front();
        ASSERT_EQ(multilineText(pasted), std::string("HHHHHHHH"));
        EXPECT_NEAR(renderedFontSize(pasted, 0), root_size, 0.1) << "root run effective glyph size";
        EXPECT_NEAR(renderedFontSize(pasted, 4), nested_size, 0.1) << "nested tspan effective glyph size";
        EXPECT_NEAR(renderedFontSize(pasted, 4), 36.0, 0.1);
        EXPECT_NE(renderedFontSize(pasted, 4), renderedFontSize(pasted, 0))
            << "the nested override must not collapse to the root size";
    }

    constexpr char const *kBoundary = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <text id="d" x="10" y="40" style="font-family:serif;font-size:14px">ab<tspan id="hot" style="font-size:22px">cd</tspan>ef</text>
  <text id="s" x="10" y="80" style="font-family:serif;font-size:12px">HHHH</text>
</svg>)SVG";
    // Collapsed caret at the tspan start (index 2): the character at the caret
    // is the 22px tspan character, so the inserted run takes the 22px style.
    {
        FreshDoc fx(kBoundary);
        ASSERT_TRUE(fx.ok()) << "caret-boundary fixture";
        auto *destination = fx.text("d");
        auto *source = fx.text("s");
        ASSERT_TRUE(destination);
        ASSERT_TRUE(source);
        ASSERT_EQ(multilineText(destination), std::string("abcdef"));
        auto const before_chars = characterSignatures(destination);
        ASSERT_EQ(before_chars.size(), 6u);
        EXPECT_NE(before_chars[1], before_chars[2]) << "fixture: the two sides of the boundary differ";
        EXPECT_NEAR(renderedFontSize(destination, 1), 14.0, 0.05);
        EXPECT_NEAR(renderedFontSize(destination, 2), 22.0, 0.05);

        auto fragment = extractFragment(fx.desktop, source, 0, 4);
        ASSERT_TRUE(fragment);
        ASSERT_TRUE(pasteFragmentAt(fx.desktop, fx.doc, destination, 2, *fragment, UI::TextPasteMode::Automatic))
            << "Automatic insertion at a tspan start boundary must succeed";
        EXPECT_EQ(multilineText(destination), std::string("abHHHHcdef"));
        auto const after_chars = characterSignatures(destination);
        ASSERT_EQ(after_chars.size(), 10u);
        EXPECT_EQ(after_chars[0], before_chars[0]);
        EXPECT_EQ(after_chars[1], before_chars[1]);
        for (unsigned const i : {2u, 3u, 4u, 5u}) {
            EXPECT_EQ(after_chars[i], before_chars[2])
                << "inserted character " << i << " must adopt the character at the caret";
            EXPECT_NEAR(renderedFontSize(destination, i), 22.0, 0.05)
                << "inserted character " << i << " must keep the tspan size at the caret";
        }
        EXPECT_EQ(after_chars[6], before_chars[2]) << "the nested tspan run must be unchanged";
        EXPECT_EQ(after_chars[7], before_chars[3]);
        EXPECT_EQ(after_chars[8], before_chars[4]);
        EXPECT_EQ(after_chars[9], before_chars[5]);
        EXPECT_NEAR(renderedFontSize(destination, 6), 22.0, 0.05);
    }
    // Collapsed caret at the tspan end (index 4): the character at the caret is
    // the 14px root character 'e', so the inserted run takes the 14px root
    // style (the boundary is internal; the true-end fallback does not apply).
    {
        FreshDoc fx(kBoundary);
        ASSERT_TRUE(fx.ok()) << "caret-boundary fixture";
        auto *destination = fx.text("d");
        auto *source = fx.text("s");
        ASSERT_TRUE(destination);
        ASSERT_TRUE(source);
        auto const before_chars = characterSignatures(destination);
        ASSERT_EQ(before_chars.size(), 6u);

        auto fragment = extractFragment(fx.desktop, source, 0, 4);
        ASSERT_TRUE(fragment);
        ASSERT_TRUE(pasteFragmentAt(fx.desktop, fx.doc, destination, 4, *fragment, UI::TextPasteMode::Automatic))
            << "Automatic insertion at a tspan end boundary must succeed";
        EXPECT_EQ(multilineText(destination), std::string("abcdHHHHef"));
        auto const after_chars = characterSignatures(destination);
        ASSERT_EQ(after_chars.size(), 10u);
        for (unsigned i = 0; i < 4; ++i) {
            EXPECT_EQ(after_chars[i], before_chars[i]) << "pre-existing character " << i << " must be unchanged";
        }
        for (unsigned const i : {4u, 5u, 6u, 7u}) {
            EXPECT_EQ(after_chars[i], before_chars[4])
                << "inserted character " << i << " must adopt the character at the caret";
            EXPECT_NEAR(renderedFontSize(destination, i), 14.0, 0.05)
                << "inserted character " << i << " must not adopt the preceding tspan size";
        }
        EXPECT_EQ(after_chars[8], before_chars[4]);
        EXPECT_EQ(after_chars[9], before_chars[5]);
    }
}

// R14 (bounded nonuniform case): for a nonuniform parent the documented scalar
// convention is descrim() = sqrt(|det|) (2geom affine); the copied fragment
// cannot carry the warp, so the test requires exactly that scalar to cross the
// boundary once and does not use axis-aligned glyph boxes as an oracle
// (handoff section 9).
TEST_F(TextPasteTest, NonuniformParentUsesDocumentedScalarConvention)
{
    constexpr char const *svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <g id="g" transform="matrix(2 0 0 4 0 0)"><text id="s" x="5" y="20" style="font-family:serif;font-size:12px">HHHH</text></g>
</svg>)SVG";
    FreshDoc fx(svg);
    ASSERT_TRUE(fx.ok()) << "nonuniform fixture";
    auto *source = fx.text("s");
    ASSERT_TRUE(source);
    double const documented_scalar = std::sqrt(8.0);
    ASSERT_NEAR(source->i2doc_affine().descrim(), documented_scalar, 1e-6) << "fixture: 2x by 4x nonuniform parent";
    double const expected_physical = 12.0 * documented_scalar;
    EXPECT_NEAR(renderedFontSize(source, 0), expected_physical, 0.1);

    auto fragment = extractFragment(fx.desktop, source, 0, 4);
    ASSERT_TRUE(fragment);
    auto const before = objectIdsInDoc(fx.doc);
    ASSERT_TRUE(pasteOutsideFragment(fx.desktop, fx.doc, *fragment));
    auto created = textsInDocNot(fx.doc, before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    double const pasted_physical = renderedFontSize(pasted, 0);
    EXPECT_TRUE(std::isfinite(pasted_physical));
    EXPECT_GT(pasted_physical, 0.0);
    EXPECT_NEAR(pasted_physical, expected_physical, 0.1)
        << "the documented descrim scalar must be applied exactly once for a nonuniform parent";
    EXPECT_GT(renderedHeightInDoc(pasted), 0.0) << "the pasted text must be renderable";
    EXPECT_FALSE(documentHasNonFiniteLength(fx.doc));
}

// ---------------------------------------------------------------------------
// R06/R10/R17/R19: real clipboard and real action dispatch. These cases drive
// the system clipboard and must only be executed during a coordinated idle
// window; they are written now and reported as planned, not executed in this
// phase.
// ---------------------------------------------------------------------------

// R06: the Selector-owned native paste route (clipboard.cpp _pasteTextObject)
// must consume the same normalized fragment and keep the physical size.
TEST_F(TextPasteTest, SelectorOutsidePastePreservesScaledSourcePhysicalSize)
{
    auto *source = text("src-scaled");
    copyRange(source, 0, sp_text_get_length(source));
    double const source_physical = renderedFontSize(source, 0);
    double const source_height = renderedHeightInDoc(source);
    ASSERT_NEAR(source_physical, 24.0, 0.05);

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Source)) << "Selector-owned rich paste must create text";
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u) << "Selector outside paste must create exactly one text object";
    auto *pasted = created.front();
    EXPECT_NEAR(renderedHeightInDoc(pasted), source_height, std::max(0.05, source_height * 0.03))
        << "Selector route must preserve the copied physical size";
    EXPECT_NEAR(renderedFontSize(pasted, 0), source_physical, 0.1);
}

// R17: the real Paste action keeps the physical size through one Undo and one
// Redo.
TEST_F(TextPasteTest, ActionPasteUndoRedoKeepsRenderedSize)
{
    auto *source = text("src-scaled");
    copyRange(source, 0, sp_text_get_length(source));
    double const source_physical = renderedFontSize(source, 0);
    double const source_height = renderedHeightInDoc(source);
    ASSERT_NEAR(source_physical, 24.0, 0.05);

    desktop->setTool("/tools/select");
    desktop->getSelection()->set(item("rect"));
    auto const before_ids = objectIds();
    std::string const before_xml = xml();
    // Snapshot the pre-existing item subtrees so the paste is proven additive.
    std::vector<std::pair<std::string, std::string>> before_subtrees;
    for (auto const &id : before_ids) {
        if (auto *before_item = item(id.c_str())) {
            before_subtrees.emplace_back(id, subtreeXml(before_item));
        }
    }
    ASSERT_TRUE(activatePasteAction("paste")) << "the real Paste action must handle a rich text clipboard";
    auto const after_ids = objectIds();
    std::vector<std::string> created_ids;
    for (auto const &id : after_ids) {
        if (!before_ids.count(id)) created_ids.push_back(id);
    }
    if (std::getenv("TP_DIAG_DIR")) {
        std::string dump;
        for (auto const &id : created_ids) {
            auto *object = document->getObjectById(id);
            auto *repr = object ? object->getRepr() : nullptr;
            std::string chain;
            for (auto *node = repr; node; node = node->parent()) {
                chain += "/";
                chain += node->name();
                if (auto *node_id = node->attribute("id")) {
                    chain += "#";
                    chain += node_id;
                }
            }
            dump += id + " object=" + (cast<SPText>(object) ? "SPText" :
                                       (cast<SPItem>(object) ? "SPItem" : "other")) +
                    " node=" + (repr ? repr->name() : "<null>") + " chain=" + chain + "\n";
        }
        diagnosticDump("action-created-objects", "txt", dump);
    }
    // The Paste action must add exactly one editable text root ...
    auto created_roots = textsNotIn(before_ids);
    ASSERT_EQ(created_roots.size(), 1u) << "the Paste action must create exactly one text root";
    auto *pasted = created_roots.front();
    auto *pasted_repr = pasted->getRepr();
    ASSERT_TRUE(pasted_repr);
    std::string const pasted_id = pasted_repr->attribute("id");
    // ... and every other new id must be a span inside that root, so no second
    // top-level object (and no unrelated object) was added.
    for (auto const &id : created_ids) {
        if (id == pasted_id) continue;
        auto *object = document->getObjectById(id);
        ASSERT_TRUE(object) << id;
        bool inside = false;
        for (auto *node = object->getRepr(); node; node = node->parent()) {
            if (node == pasted_repr) {
                inside = true;
                break;
            }
        }
        EXPECT_TRUE(inside) << "new object " << id << " must be a span inside the new text root";
    }
    // Pre-existing objects must be byte-identical after the additive paste. The
    // document root contains the new text root, so every pre-existing item is
    // compared except the ancestors of the paste itself.
    std::set<Inkscape::XML::Node *> paste_ancestors;
    for (auto *node = pasted_repr; node; node = node->parent()) {
        paste_ancestors.insert(node);
    }
    unsigned compared = 0;
    for (auto const &entry : before_subtrees) {
        auto *after_item = item(entry.first.c_str());
        ASSERT_TRUE(after_item) << entry.first << " must survive the paste";
        if (paste_ancestors.count(after_item->getRepr())) continue;
        EXPECT_EQ(subtreeXml(after_item), entry.second) << entry.first << " must be unchanged by the paste";
        ++compared;
    }
    EXPECT_GT(compared, 0u) << "the fixture must compare at least one pre-existing object";
    document->ensureUpToDate();
    EXPECT_NEAR(renderedHeightInDoc(pasted), source_height, std::max(0.05, source_height * 0.03))
        << "action paste must preserve the physical size";
    EXPECT_NEAR(renderedFontSize(pasted, 0), source_physical, 0.1);

    desktop->getSelection()->clear();
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before_xml) << "one Undo must remove the whole paste";
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    auto *redone = item(pasted_id.c_str());
    ASSERT_TRUE(redone) << "Redo must restore the pasted text";
    EXPECT_NEAR(renderedHeightInDoc(redone), source_height, std::max(0.05, source_height * 0.03))
        << "the redone paste must keep the same physical size";
    EXPECT_NEAR(renderedFontSize(redone, 0), source_physical, 0.1);
}

// R19: a version-1 rich payload is unsupported; with a complete plain fallback
// the paste must insert that plain text under the destination typing style and
// must not leak any version-1 rich value.
TEST_F(TextPasteTest, Version1RichPayloadFallsBackToPlainTextWithoutStyleLeak)
{
    setRichClipboard("vac-text-fragment\t1\nP\t\nR\tfont-family:monospace;font-size:24px\tBeta\n",
                     std::string("Beta"));
    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const typing_style = characterSignature(destination, iteratorAt(destination, 3));
    std::string const before = multilineText(destination);

    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();
    std::string const after = multilineText(destination);
    EXPECT_EQ(after, before.substr(0, 3) + "Beta" + before.substr(3))
        << "version 1 must fall back to the complete plain text";
    auto const inserted = characterSignature(destination, iteratorAt(destination, 3));
    EXPECT_EQ(inserted, typing_style) << "the plain fallback must adopt the destination typing style";
    EXPECT_FALSE(containsSubstring(inserted, "fam=monospace")) << "version-1 rich data must never leak";
    EXPECT_FALSE(containsSubstring(inserted, "size=24.0000")) << "version-1 rich size must never leak";
}

// R19: a version-1 rich payload without a valid plain fallback must be rejected
// as a whole: no insertion, no mutation, no Undo record.
TEST_F(TextPasteTest, Version1RichPayloadWithoutPlainIsRejectedWithoutMutation)
{
    setRichClipboard("vac-text-fragment\t1\nP\t\nR\tfont-size:24px\tBeta\n", std::nullopt);
    auto const xml_before = xml();
    auto *destination = text("dst");
    std::string const text_before = multilineText(destination);

    EXPECT_FALSE(pasteInside(destination, 3, UI::TextPasteMode::Automatic))
        << "an unsupported version without a plain fallback must not report success";
    document->ensureUpToDate();
    EXPECT_EQ(xml(), xml_before) << "rejected legacy payload must not mutate the document";
    EXPECT_EQ(multilineText(destination), text_before);
    EXPECT_FALSE(DocumentUndo::undo(document)) << "rejected legacy payload must not create an Undo record";
}

// ---------------------------------------------------------------------------
// Milestone A: external plain-text transport, target routing and Undo
// ---------------------------------------------------------------------------
// The fixtures below publish exact wire bytes under exact MIME spellings. They
// are the controlled raw-stream seam for the byte-level contract (one optional
// terminal NUL, embedded/repeated NUL rejection, cap arithmetic, malformed
// UTF-8); the separate publisher process covers real OS clipboard ownership.

// E01: a clipboard that advertises ONLY the charset-qualified UTF-8 spelling
// (what macOS/GTK publishes for native text) must be pasted by ordinary
// Selector paste. Before the fix the general fallback checked only "text/plain"
// and reported "Nothing on the clipboard.".
TEST_F(TextPasteTest, E01SelectorNormalPasteReadsUtf8OnlyPlainMime)
{
    setRawClipboard({"text/plain;charset=utf-8"}, "external selector text");
    ASSERT_TRUE(clipboardHasMime("text/plain;charset=utf-8"));
    ASSERT_FALSE(clipboardHasMime("text/plain")) << "fixture must not advertise the plain alias";

    auto const before = objectIds();
    desktop->getSelection()->set(item("rect"));
    desktop->setTool("/tools/select");
    std::string const xml_before = xml();
    ASSERT_TRUE(activatePasteAction("paste")) << "normal Paste must reach the UTF-8 plain reader";
    document->ensureUpToDate();

    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u) << "Selector paste must create one editable text object";
    EXPECT_EQ(multilineText(created.front()), "external selector text");
    EXPECT_NE(xml(), xml_before);

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), xml_before) << "one paste must be exactly one Undo step";
}

// E02: the Text tool with no edited object (a real blank-canvas click) must
// paste the same fixture and still produce exactly ONE Undo step. The legacy
// inline path used to commit "Create text" and then "Paste text".
TEST_F(TextPasteTest, E02TextToolWithoutEditedObjectPastesUtf8OnlyPlainMime)
{
    PrefRestore external_ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);

    setRawClipboard({"text/plain;charset=utf-8"}, "nascent plain text");
    desktop->getSelection()->clear();
    desktop->setTool("/tools/text");
    auto *tool = textTool();
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool->textItem(), nullptr);

    auto *base = static_cast<UI::Tools::ToolBase *>(tool);
    Geom::Point const click(400, 250);
    ButtonPressEvent press;
    press.pos = press.orig_pos = click;
    press.button = 1;
    press.num_press = 1;
    base->root_handler(press);
    ButtonReleaseEvent release;
    release.pos = release.orig_pos = click;
    release.button = 1;
    base->root_handler(release);
    ASSERT_EQ(tool->textItem(), nullptr) << "a blank-canvas click alone must not create a text object";

    auto const before = objectIds();
    std::string const xml_before = xml();
    ASSERT_TRUE(activatePasteAction("paste")) << "the real Paste action must not silently no-op";
    document->ensureUpToDate();

    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(multilineText(created.front()), "nascent plain text");

    ASSERT_TRUE(DocumentUndo::undo(document)) << "one paste must create exactly one Undo step";
    document->ensureUpToDate();
    EXPECT_EQ(xml(), xml_before) << "the single Undo step must restore the pre-paste document";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "the paste must not add a hidden second Undo entry";
}

// E04/E05: exactly one terminal NUL is a transport artefact (macOS/GTK and
// GTK/win32 publish strlen/text + 1 bytes for UTF-8 plain text) and must be
// removed once; the same text without it is semantically identical.
TEST_F(TextPasteTest, E04TerminalNulPlainStreamIsAcceptedWithoutNulInDocument)
{
    std::string const payload = "terminated external text";
    setRawClipboard({"text/plain;charset=utf-8"}, payload + std::string(1, '\0'));

    auto *destination = text("dst");
    std::string const before = xml();
    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Automatic))
        << "the platform transport terminator must not reject the payload";
    document->ensureUpToDate();

    std::string const after = multilineText(destination);
    EXPECT_TRUE(containsSubstring(after, payload));
    EXPECT_EQ(after.find('\0'), std::string::npos) << "no NUL may reach the document";
    EXPECT_NE(xml(), before);

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before) << "the terminated paste must be exactly one Undo step";
}

TEST_F(TextPasteTest, E05PlainStreamWithoutTerminalNulMatchesTerminatedResult)
{
    std::string const payload = "terminated external text";

    setRawClipboard({"text/plain;charset=utf-8"}, payload + std::string(1, '\0'));
    auto *destination = text("dst");
    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();
    std::string const with_terminator = multilineText(destination);
    std::string const style_with_terminator = characterSignature(destination, iteratorAt(destination, 3));
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();

    setRawClipboard({"text/plain;charset=utf-8"}, payload);
    ASSERT_TRUE(pasteInside(destination, 3, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(destination), with_terminator)
        << "one optional transport terminator must not change the semantic result";
    EXPECT_EQ(characterSignature(destination, iteratorAt(destination, 3)), style_with_terminator);
    DocumentUndo::undo(document);
}

// E06: embedded NUL, repeated terminal NUL and malformed UTF-8 are unusable
// external data: no insertion, no document mutation, no preference change and
// no Undo record.
TEST_F(TextPasteTest, E06EmbeddedNulIsRejectedWithoutMutation)
{
    setRawClipboard({"text/plain;charset=utf-8"}, std::string("AB\0CD", 5));
    auto *destination = text("dst");
    std::string const before_xml = xml();
    std::string const before_text = multilineText(destination);
    std::string const before_ask = Preferences::get()->getString("/options/textpaste/external-ask");

    EXPECT_FALSE(pasteInside(destination, 3, UI::TextPasteMode::Automatic))
        << "an embedded NUL is invalid, not content to cut at";
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before_xml);
    EXPECT_EQ(multilineText(destination), before_text);
    EXPECT_EQ(Preferences::get()->getString("/options/textpaste/external-ask"), before_ask);
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(TextPasteTest, E06RepeatedTerminalNulIsRejectedWithoutMutation)
{
    setRawClipboard({"text/plain;charset=utf-8"}, std::string("text\0\0", 6));
    auto *destination = text("dst");
    std::string const before_xml = xml();
    EXPECT_FALSE(pasteInside(destination, 3, UI::TextPasteMode::Automatic))
        << "exactly one terminator is transport; a second one is invalid data";
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before_xml);
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(TextPasteTest, E06TruncatedUtf8IsRejectedWithoutMutation)
{
    setRawClipboard({"text/plain;charset=utf-8"}, std::string("\xC3\x28", 2));
    auto *destination = text("dst");
    std::string const before_xml = xml();
    EXPECT_FALSE(pasteInside(destination, 3, UI::TextPasteMode::Automatic))
        << "malformed UTF-8 must never be repaired or partially inserted";
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before_xml);
    EXPECT_FALSE(DocumentUndo::undo(document));
}

// E07: exact boundary contract. The semantic cap is MAX_CHARS UTF-8 bytes and is
// re-applied AFTER the single optional terminator is removed; reaching the wire
// budget is not EOF and nothing is ever truncated.
TEST_F(TextPasteTest, E07PlainStreamAtCapWithTerminatorPastes)
{
    std::string const at_cap(TP::MAX_CHARS, 'P');
    setRawClipboard({"text/plain;charset=utf-8"}, at_cap + std::string(1, '\0'));
    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic))
        << "MAX_CHARS text plus the transport terminator is valid";
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(created.front()).size(), at_cap.size())
        << "the terminator must not count against the semantic budget";
}

TEST_F(TextPasteTest, E07PlainStreamOverCapWithAndWithoutTerminatorIsRejected)
{
    for (bool terminator : {false, true}) {
        std::string payload(TP::MAX_CHARS + 1, 'Q');
        if (terminator) {
            payload += '\0';
        }
        setRawClipboard({"text/plain;charset=utf-8"}, payload);
        auto *destination = text("dst");
        std::string const before_xml = xml();
        std::string const before_text = multilineText(destination);
        EXPECT_FALSE(pasteInside(destination, 3, UI::TextPasteMode::Automatic))
            << "over-cap payload (terminator=" << terminator << ") must be rejected as a whole";
        document->ensureUpToDate();
        EXPECT_EQ(xml(), before_xml) << "over-cap payload must not partially mutate the document";
        EXPECT_EQ(multilineText(destination), before_text);
        EXPECT_FALSE(DocumentUndo::undo(document)) << "over-cap payload must not create an Undo record";
    }
}

TEST_F(TextPasteTest, E07TerminatorOnlyIsNoOp)
{
    setRawClipboard({"text/plain;charset=utf-8"}, std::string(1, '\0'));
    auto *destination = text("dst");
    std::string const before_xml = xml();
    EXPECT_FALSE(pasteInside(destination, 3, UI::TextPasteMode::Automatic));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), before_xml);
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(TextPasteTest, E07MultibyteEndingAtCapWithTerminatorPastes)
{
    std::string at_cap(TP::MAX_CHARS - 4, 'P');
    at_cap += "\xf0\x9f\x98\x80"; // U+1F600, four UTF-8 bytes exactly at the cap
    ASSERT_EQ(at_cap.size(), TP::MAX_CHARS);
    setRawClipboard({"text/plain;charset=utf-8"}, at_cap + std::string(1, '\0'));

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    std::string const pasted = multilineText(created.front());
    ASSERT_EQ(pasted.size(), TP::MAX_CHARS) << "the terminator strip must not cut a UTF-8 sequence";
    EXPECT_EQ(pasted.substr(pasted.size() - 4), "\xf0\x9f\x98\x80")
        << "a multi-byte character ending exactly at the cap must survive intact";
}

// E13 (object/SVG representation outranks an advertised plain-text alternative)
// is proved by the separate-process publisher harness case
// `E13_object_priority_svg_plain`, not here. This process's GdkClipboard marks
// every fixture it publishes as LOCAL, and ClipboardManagerImpl deliberately
// skips the object/SVG import path for a local clipboard (see
// `_retrieveClipboard`: `is_local()` discards the internal document). No
// in-process fixture can therefore observe the object branch: the previous
// oracle could not pass, and a weaker "no new text" assertion would also pass on
// a total no-op. The harness publishes from a genuinely separate NSPasteboard
// owner and asserts both the imported element id and the absence of the plain
// alternative as document text.

// E14 (reader equivalence half): every paste trigger reaches the same reader.
// The keyboard/main-menu/right-click routes all dispatch these same actions.
TEST_F(TextPasteTest, E14AllPasteTriggersReadTerminatedUtf8OnlyStream)
{
    for (char const *action : {"paste", "paste-keep-source-formatting", "paste-without-formatting"}) {
        setRawClipboard({"text/plain;charset=utf-8"}, std::string("trigger text") + std::string(1, '\0'));
        auto *destination = text("dst");
        document->ensureUpToDate();
        std::string const before_text = multilineText(destination);
        ASSERT_TRUE(placeTextCursor(destination, 3));
        ASSERT_TRUE(activatePasteAction(action)) << "action " << action << " must be available";
        document->ensureUpToDate();
        EXPECT_NE(multilineText(destination), before_text)
            << "action " << action << " must paste through the same reader";
        EXPECT_TRUE(containsSubstring(multilineText(destination), "trigger text"));
        DocumentUndo::undo(document);
        document->ensureUpToDate();
    }
}

// E08 (controlled stream): a plain producer that writes short chunks must be
// read to EOF, never mistaken for a short payload.
TEST_F(TextPasteTest, E08ShortChunkedPlainStreamIsReadToCompletePayload)
{
    std::string const payload = "chunked plain payload that must arrive complete";
    ASSERT_TRUE(installClipboardProvider(
        makeChunkedProvider(payload, 5, 1, false, "text/plain;charset=utf-8")));
    ASSERT_TRUE(clipboardHasMime("text/plain;charset=utf-8"));

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(created.front()), payload)
        << "the tail of a short-chunked plain payload must not be dropped";
}

// E08 (bounded cancellation): a plain producer that delivers one chunk and then
// stalls must abort the whole paste on the single absolute deadline with no
// partial insert and no Undo record.
TEST_F(TextPasteTest, E08StalledPlainProducerAbortsWithinOneDeadline)
{
    ASSERT_TRUE(installClipboardProvider(makeChunkedProvider(
        std::string(4096, 'S'), 16, 1, true, "text/plain;charset=utf-8")));
    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const before_xml = xml();

    auto const start = std::chrono::steady_clock::now();
    bool const changed = pasteInside(destination, 3, UI::TextPasteMode::Automatic);
    auto const elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    document->ensureUpToDate();

    EXPECT_FALSE(changed) << "a stalled plain read must abort the whole paste";
    EXPECT_GE(elapsed_ms, 4000) << "the bounded read must wait for the single 5s deadline";
    EXPECT_LT(elapsed_ms, 15000) << "a stalled producer must not hang the GUI thread";
    EXPECT_EQ(xml(), before_xml) << "a timed-out plain payload must never be applied, not even partially";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "an aborted paste must not create an Undo record";
}

// N16 (native strictness preserved): the plain-only transport normalization must
// never leak into the native record stream. A native payload with a trailing NUL
// is rejected as a whole and the complete plain alternative is used instead.
TEST_F(TextPasteTest, NativePayloadWithTrailingNulIsStillRejected)
{
    std::string const native = "vac-text-fragment\t2\nP\t\nR\tfont-size:24px\tBeta\n";
    setRichClipboard(native + std::string(1, '\0'), std::string("plain alternative"));
    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(created.front()), "plain alternative")
        << "the malformed native payload must not be accepted and the plain fallback must be complete";
}

// ---------------------------------------------------------------------------
// Milestone B: external rich import, representation choice and the question
// ---------------------------------------------------------------------------
// Simple deterministic fixtures whose expected text is literal. The decoders
// themselves are covered by test_text-paste-html / test_text-paste-rtf; these
// cases cover routing, capability separation, prompt gating and preferences.

constexpr char const *kHtmlStyled = "<p style=\"font-weight:bold\">HTMLONE</p>";
constexpr char const *kHtmlPlainPackaged = "<p>plain packaged</p>";
constexpr char const *kRtfSimple =
    "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fnil Arial;}}\\f0\\fs24 RTFTWO\\par}";

TEST_F(TextPasteTest, B01HtmlIsPreferredOverRtfAndPlainWithoutSplicing)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false); // routing only; the question has its own test
    setRawClipboardMulti({{"text/html", kHtmlStyled},
                          {"text/rtf", kRtfSimple},
                          {"text/plain;charset=utf-8", "PLAINTHREE"}});

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    std::string const pasted = multilineText(created.front());
    EXPECT_EQ(pasted, "HTMLONE") << "native > HTML > RTF > plain must be deterministic and never spliced";
    EXPECT_FALSE(containsSubstring(pasted, "RTFTWO"));
    EXPECT_FALSE(containsSubstring(pasted, "PLAINTHREE"));
    // The HTML <p style="font-weight:bold"> must reach the run style, not just the text.
    EXPECT_TRUE(containsSubstring(characterSignature(created.front(), iteratorAt(created.front(), 0)), "weight=700") ||
                containsSubstring(subtreeXml(created.front()), "font-weight:bold"))
        << "supported external character formatting must be applied";
}

TEST_F(TextPasteTest, B04MalformedHtmlUsesTheCompletePlainAlternative)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    // Invalid UTF-8 makes the rich representation unusable under the cap.
    setRawClipboardMulti({{"text/html", std::string("<p>\xff\xfe</p>", 9)},
                          {"text/plain;charset=utf-8", "COMPLETE PLAIN"}});

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(created.front()), "COMPLETE PLAIN")
        << "a rejected rich representation must fall back to the COMPLETE plain alternative, never a mix";
}

TEST_F(TextPasteTest, B10PlainContentPackagedAsHtmlDoesNotAsk)
{
    PrefRestore ask("/options/textpaste/external-ask");
    PrefRestore mode("/options/textpaste/external-mode");
    Preferences::get()->setBool("/options/textpaste/external-ask", true);
    Preferences::get()->setInt("/options/textpaste/external-mode", 0);
    setRawClipboardMulti({{"text/html", kHtmlPlainPackaged},
                          {"text/plain;charset=utf-8", "plain packaged"}});

    PromptWatch watch;
    watch.dialog_name = "text-paste-external-dialog";
    PromptTimerGuard prompt_timer(watch);
    auto const before = objectIds();
    desktop->getSelection()->set(item("rect"));
    desktop->setTool("/tools/select");
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();
    EXPECT_EQ(watch.dialogs, 0) << "unstyled content must never raise a formatting question";
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(multilineText(created.front()), "plain packaged");
}

TEST_F(TextPasteTest, B12ExternalQuestionAsksOnceAndRemembersOnlyAfterSuccess)
{
    PrefRestore ask("/options/textpaste/external-ask");
    PrefRestore mode("/options/textpaste/external-mode");
    Preferences::get()->setBool("/options/textpaste/external-ask", true);
    Preferences::get()->setInt("/options/textpaste/external-mode", 0);
    setRawClipboardMulti({{"text/html", kHtmlStyled}, {"text/plain;charset=utf-8", "HTMLONE"}});

    PromptWatch watch;
    watch.dialog_name = "text-paste-external-dialog";
    watch.remember_name = "text-paste-external-remember";
    watch.preselection_names = {"text-paste-external-mode-source", "text-paste-external-mode-destination"};
    watch.select_name = "text-paste-external-mode-source";
    watch.remember = true;
    watch.confirm = true;
    PromptTimerGuard prompt_timer(watch);

    auto const before = objectIds();
    desktop->getSelection()->set(item("rect"));
    desktop->setTool("/tools/select");
    std::string const xml_before = xml();
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    EXPECT_EQ(watch.dialogs, 1) << "exactly one external question per paste";
    EXPECT_EQ(watch.preselected_name, "text-paste-external-mode-source")
        << "outside existing text, Automatic must preselect Keep source formatting";
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(multilineText(created.front()), "HTMLONE");
    EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/external-mode", -1), 1)
        << "Remember must persist the chosen external mode after a successful paste";
    EXPECT_FALSE(Preferences::get()->getBool("/options/textpaste/external-ask", true))
        << "Remember must disable the external question";
    // Native preferences stay independent.
    EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/mode", 0), 0);
    EXPECT_FALSE(Preferences::get()->getBool("/options/textpaste/ask", false));
    (void)xml_before;
}

TEST_F(TextPasteTest, B13ExternalQuestionCancelLeavesDocumentAndPreferencesUnchanged)
{
    PrefRestore ask("/options/textpaste/external-ask");
    PrefRestore mode("/options/textpaste/external-mode");
    Preferences::get()->setBool("/options/textpaste/external-ask", true);
    Preferences::get()->setInt("/options/textpaste/external-mode", 0);
    setRawClipboardMulti({{"text/html", kHtmlStyled}, {"text/plain;charset=utf-8", "HTMLONE"}});

    PromptWatch watch;
    watch.dialog_name = "text-paste-external-dialog";
    watch.remember_name = "text-paste-external-remember";
    watch.remember = true; // tick it even though the user cancels: nothing may persist
    PromptTimerGuard prompt_timer(watch);

    auto *destination = text("dst");
    std::string const before_xml = xml();
    std::string const before_text = multilineText(destination);
    desktop->getSelection()->set(item("rect"));
    desktop->setTool("/tools/select");
    activatePasteAction("paste");
    document->ensureUpToDate();

    EXPECT_EQ(watch.dialogs, 1);
    EXPECT_EQ(xml(), before_xml) << "cancel must not mutate the document";
    EXPECT_EQ(multilineText(destination), before_text);
    EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/external-mode", -1), 0)
        << "cancel must not persist a remembered external mode";
    EXPECT_TRUE(Preferences::get()->getBool("/options/textpaste/external-ask", true))
        << "cancel must leave the asking preference enabled";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "cancel must not create an Undo record";
}

TEST_F(TextPasteTest, B14ExplicitDestinationCommandBypassesTheExternalQuestion)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", true); // asking ON: explicit commands still never ask
    setRawClipboardMulti({{"text/html", kHtmlStyled}, {"text/plain;charset=utf-8", "HTMLONE"}});

    PromptWatch watch;
    watch.dialog_name = "text-paste-external-dialog";
    PromptTimerGuard prompt_timer(watch);

    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const before_text = multilineText(destination);
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste-without-formatting"));
    document->ensureUpToDate();

    EXPECT_EQ(watch.dialogs, 0) << "an explicit command must bypass the question entirely";
    std::string const after = multilineText(destination);
    EXPECT_NE(after, before_text);
    EXPECT_TRUE(containsSubstring(after, "HTMLONE")) << "the text must still be inserted without formatting";
    DocumentUndo::undo(document);
}

// B15 (object target outranks external HTML) is proved by the separate-process
// publisher harness case `B15_object_priority_svg_html`. In this process the
// fixture clipboard is local and the object branch is short-circuited, so an
// in-process oracle cannot pass and a no-op oracle would be vacuous; the harness
// publishes SVG and HTML from one foreign owner with distinct bytes and asserts
// the imported object id plus the absence of the HTML text.

// ===========================================================================
// Repair r1: rich text that looks like SVG is text; object priority is
// preserved for genuine whole-object copies (repair 5)
// ===========================================================================

// The same markup must take two different routes: a validated external rich
// representation is a text decision (SVG-looking decoded text is pasted as
// text), while plain-only SVG markup keeps the object import path.
TEST_F(TextPasteTest, F13ExternalHtmlSvgLookingTextIsPastedAsText)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    std::string const markup = "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>";
    std::string const html = "<p>&lt;svg xmlns=\"http://www.w3.org/2000/svg\"&gt;&lt;/svg&gt;</p>";
    setRawClipboardMulti({{"text/html", html}, {"text/plain;charset=utf-8", markup}});

    auto const before = objectIds();
    desktop->getSelection()->set(item("rect"));
    desktop->setTool("/tools/select");
    std::string const xml_before = xml();
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u)
        << "decoded rich text that begins with SVG markup must be inserted as text, not rerouted to the importer";
    EXPECT_EQ(multilineText(created.front()), markup);
    EXPECT_NE(xml(), xml_before);

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), xml_before);
}

// F13 (plain-only SVG-looking markup takes the object import path) is proved by
// the separate-process publisher harness case `F13_plain_svg_object_path`: the
// plain-only MIME cannot be separated from plain-text-to-importer rerouting in
// process, and a local clipboard never reaches the importer. The valid other
// half of F13 - a validated external rich representation whose decoded text
// begins with SVG markup stays text - is the in-process oracle above.


// ===========================================================================
// Repair r1: literal caps, empty/cap-1 payloads (repair 7)
// ===========================================================================

// The documented limits are pinned by literal values, so a wrong constant can
// never silently follow the tests.
TEST_F(TextPasteTest, E07LiteralCapValuesAndBoundaryPayloads)
{
    EXPECT_EQ(TP::MAX_CHARS, 65536u);
    EXPECT_EQ(TP::MAX_PARAGRAPHS, 1024u);
    EXPECT_EQ(TP::MAX_RUNS, 4096u);
    EXPECT_EQ(TP::MAX_RUN_LENGTH, 8192u);
    EXPECT_EQ(TP::MAX_PAYLOAD_BYTES, 262144u);
    EXPECT_EQ(TP::MAX_STYLE_LENGTH, 2048u);
    EXPECT_EQ(TP::MAX_STYLE_VALUE_LENGTH, 256u);

    // cap-1 and exactly-at-cap paste the complete payload.
    for (std::size_t size : {65535u, 65536u}) {
        SCOPED_TRACE(size);
        std::string const payload(size, 'L');
        setRawClipboard({"text/plain;charset=utf-8"}, payload);
        auto const before = objectIds();
        ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
        auto created = textsNotIn(before);
        ASSERT_EQ(created.size(), 1u);
        document->ensureUpToDate();
        EXPECT_EQ(multilineText(created.front()).size(), size) << "the complete payload must arrive, never truncated";
        EXPECT_EQ(multilineText(created.front()), payload);
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
    }

    // One byte over the cap aborts as a whole.
    std::string const over(65537u, 'L');
    setRawClipboard({"text/plain;charset=utf-8"}, over);
    auto const before = objectIds();
    std::string const xml_before = xml();
    EXPECT_FALSE(pasteOutside(UI::TextPasteMode::Automatic));
    document->ensureUpToDate();
    EXPECT_TRUE(textsNotIn(before).empty());
    EXPECT_EQ(xml(), xml_before);
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(TextPasteTest, E07EmptyPlainPayloadIsANoOp)
{
    setRawClipboard({"text/plain;charset=utf-8"}, std::string());
    auto const before = objectIds();
    std::string const xml_before = xml();
    EXPECT_FALSE(pasteOutside(UI::TextPasteMode::Automatic)) << "an empty payload is not pasted";
    document->ensureUpToDate();
    EXPECT_TRUE(textsNotIn(before).empty());
    EXPECT_EQ(xml(), xml_before);
    EXPECT_FALSE(DocumentUndo::undo(document));
}

// ===========================================================================
// Repair r1: MIME spellings, Unicode/RTL/CRLF (repair 7)
// ===========================================================================

TEST_F(TextPasteTest, E01EveryPlainMimeSpellingIsReadAndAllTogetherAreNotSpliced)
{
    struct PlainCase {
        std::vector<std::string> mimes;
        char const *expected;
    };
    PlainCase const cases[] = {
        {{"text/plain;charset=utf-8"}, "utf8 only"},
        {{"text/plain"}, "lax only"},
        {{"text/plain;charset=utf-8", "text/plain"}, "both spellings"},
    };
    for (auto const &plain_case : cases) {
        SCOPED_TRACE(plain_case.expected);
        setRawClipboard(plain_case.mimes, plain_case.expected);
        for (auto const &mime : plain_case.mimes) {
            ASSERT_TRUE(clipboardHasMime(mime)) << mime;
        }
        auto const before = objectIds();
        desktop->getSelection()->set(item("rect"));
        desktop->setTool("/tools/select");
        ASSERT_TRUE(activatePasteAction("paste"));
        document->ensureUpToDate();
        auto created = textsNotIn(before);
        ASSERT_EQ(created.size(), 1u);
        EXPECT_EQ(multilineText(created.front()), plain_case.expected)
            << "the payload must be pasted once, whole and unspliced";
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
    }
}

TEST_F(TextPasteTest, B16ExternalRichMimeAliasesAreRecognised)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    struct RichCase {
        char const *mime;
        char const *payload;
        char const *expected;
    };
    RichCase const cases[] = {
        {"text/html", kHtmlStyled, "HTMLONE"},
        {"application/x.windows.HTML Format", kHtmlStyled, "HTMLONE"},
        {"text/rtf", kRtfSimple, "RTFTWO"},
        {"application/rtf", kRtfSimple, "RTFTWO"},
        {"application/x.windows.Rich Text Format", kRtfSimple, "RTFTWO"},
    };
    for (auto const &rich_case : cases) {
        SCOPED_TRACE(rich_case.mime);
        setRawClipboard({rich_case.mime}, rich_case.payload);
        auto const before = objectIds();
        ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic)) << rich_case.mime;
        auto created = textsNotIn(before);
        ASSERT_EQ(created.size(), 1u) << rich_case.mime;
        document->ensureUpToDate();
        EXPECT_EQ(multilineText(created.front()), rich_case.expected);
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
    }
}

TEST_F(TextPasteTest, E09UnicodeRtlEmojiAndCrlfPlainPayloadIsPreserved)
{
    // CRLF is normalized to the paragraph break (the run text may not contain a
    // control character); Hebrew, Greek and the emoji must survive byte-exact.
    std::string const payload = "line1\r\n\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xf0\x9f\x98\x80\r\nline3";
    setRawClipboard({"text/plain;charset=utf-8"}, payload);
    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(created.front()), "line1\n\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xf0\x9f\x98\x80\nline3");
    EXPECT_TRUE(containsSubstring(multilineText(created.front()), "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"));
    EXPECT_TRUE(containsSubstring(multilineText(created.front()), "\xf0\x9f\x98\x80"));
    EXPECT_EQ(multilineText(created.front()).find('\r'), std::string::npos);

    // The external rich path preserves the same content.
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    setRawClipboard({"text/html"}, "<p>\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xf0\x9f\x98\x80</p>");
    auto const before_rich = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto rich = textsNotIn(before_rich);
    ASSERT_EQ(rich.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(rich.front()), "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xf0\x9f\x98\x80");
}

// ===========================================================================
// Repair r1: fallback and abort classes (repair 7)
// ===========================================================================

TEST_F(TextPasteTest, B04MalformedRtfFallsBackToTheCompletePlainAlternative)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    setRawClipboardMulti({{"text/rtf", "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fnil Arial;}}\\f0\\fs24 unterminated"},
                          {"text/plain;charset=utf-8", "COMPLETE PLAIN"}});

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(created.front()), "COMPLETE PLAIN")
        << "a malformed RTF representation must use the COMPLETE plain alternative, never a mix or a no-op";
}

TEST_F(TextPasteTest, B17OverLimitRichPayloadWithValidPlainAbortsTheWholePaste)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    std::string const over_limit(TP::MAX_PAYLOAD_BYTES + 1, 'H');
    setRawClipboardMulti({{"text/html", over_limit}, {"text/plain;charset=utf-8", "MUST NOT APPEAR"}});

    auto *destination = text("dst");
    document->ensureUpToDate();
    auto const before = objectIds();
    std::string const xml_before = xml();
    std::string const text_before = multilineText(destination);
    EXPECT_FALSE(pasteOutside(UI::TextPasteMode::Automatic))
        << "an over-limit rich representation aborts the whole paste";
    document->ensureUpToDate();
    EXPECT_TRUE(textsNotIn(before).empty()) << "the valid plain alternative must not be used after a limit abort";
    EXPECT_EQ(multilineText(destination), text_before);
    EXPECT_EQ(xml(), xml_before);
    EXPECT_FALSE(DocumentUndo::undo(document));
}

// ===========================================================================
// Repair r1: explicit Destination removal, Undo/Redo and caret invariants,
// preselection, stale Remember (repair 7)
// ===========================================================================

TEST_F(TextPasteTest, B14ExplicitDestinationCommandDropsExternalSourceStyle)
{
    PrefRestore ask("/options/textpaste/external-ask");
    PrefRestore mode("/options/textpaste/external-mode");
    Preferences::get()->setBool("/options/textpaste/external-ask", true);
    Preferences::get()->setInt("/options/textpaste/external-mode", 0);
    std::string const styled = "<p style=\"font-weight:bold;font-size:24px;font-family:monospace\">HTMLONE</p>";
    setRawClipboardMulti({{"text/html", styled}, {"text/plain;charset=utf-8", "HTMLONE"}});

    auto *destination = text("dst");
    document->ensureUpToDate();
    unsigned const at = 3;
    auto const typing_style = characterSignature(destination, iteratorAt(destination, at));
    std::string const before_text = multilineText(destination);
    std::string const xml_before = xml();

    ASSERT_TRUE(placeTextCursor(destination, at));
    ASSERT_TRUE(activatePasteAction("paste-without-formatting"));
    document->ensureUpToDate();
    std::string const after = multilineText(destination);
    ASSERT_EQ(after, before_text.substr(0, at) + "HTMLONE" + before_text.substr(at));
    auto const inserted_style = characterSignature(destination, iteratorAt(destination, at));
    EXPECT_EQ(inserted_style, typing_style)
        << "the explicit Destination command must remove every source character style";
    EXPECT_FALSE(containsSubstring(inserted_style, "fam=monospace"));
    EXPECT_FALSE(containsSubstring(inserted_style, "size=24.0000"));
    EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/external-mode", -1), 0)
        << "an explicit command must not persist a mode";
    EXPECT_TRUE(Preferences::get()->getBool("/options/textpaste/external-ask", true))
        << "an explicit command must not change the asking preference";

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), xml_before);
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(TextPasteTest, B19ExternalPasteUndoRedoAndCaretInvariants)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    setRawClipboardMulti({{"text/html", kHtmlStyled}, {"text/plain;charset=utf-8", "HTMLONE"}});

    auto *destination = text("dst");
    document->ensureUpToDate();
    unsigned const at = 3;
    std::string const before_text = multilineText(destination);
    std::string const after_text = before_text.substr(0, at) + "HTMLONE" + before_text.substr(at);
    std::string const xml_before = xml();

    ASSERT_TRUE(placeTextCursor(destination, at));
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(destination), after_text);
    auto *tool = textTool();
    ASSERT_TRUE(tool);
    EXPECT_TRUE(tool->text_sel_start == tool->text_sel_end) << "a successful paste must leave a caret, not a selection";

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), xml_before) << "one Undo must remove the whole external paste";
    EXPECT_EQ(multilineText(destination), before_text);
    EXPECT_TRUE(tool->text_sel_start == tool->text_sel_end) << "Undo must keep a caret, not a selection";

    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(destination), after_text) << "one Redo must restore the whole external paste";
    EXPECT_TRUE(tool->text_sel_start == tool->text_sel_end) << "Redo must keep a caret, not a selection";

    // Exactly one undo entry for the paste, no hidden second one.
    ASSERT_TRUE(DocumentUndo::undo(document));
    EXPECT_FALSE(DocumentUndo::undo(document)) << "a normal external paste must not add extra Undo entries";
    DocumentUndo::redo(document);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(destination), after_text);
}

TEST_F(TextPasteTest, B20ExternalQuestionPreselectsStoredModeAndInsideDestination)
{
    struct PreselectCase {
        int stored;
        bool inside_text;
        char const *expected;
    };
    PreselectCase const cases[] = {
        {0, false, "text-paste-external-mode-source"},
        {0, true, "text-paste-external-mode-destination"},
        {1, false, "text-paste-external-mode-source"},
        {2, false, "text-paste-external-mode-destination"},
        {7, false, "text-paste-external-mode-source"}, // invalid stored value -> Automatic
    };
    for (auto const &preselect : cases) {
        SCOPED_TRACE(std::string("stored=") + std::to_string(preselect.stored) +
                     (preselect.inside_text ? " inside" : " outside"));
        PrefRestore ask("/options/textpaste/external-ask");
        PrefRestore mode("/options/textpaste/external-mode");
        Preferences::get()->setBool("/options/textpaste/external-ask", true);
        Preferences::get()->setInt("/options/textpaste/external-mode", preselect.stored);
        setRawClipboardMulti({{"text/html", kHtmlStyled}, {"text/plain;charset=utf-8", "HTMLONE"}});

        PromptWatch watch;
        watch.dialog_name = "text-paste-external-dialog";
        watch.preselection_names = {"text-paste-external-mode-source", "text-paste-external-mode-destination"};
        watch.confirm = true;
        PromptTimerGuard prompt_timer(watch);

        desktop->getSelection()->set(item("rect"));
        desktop->setTool("/tools/select");
        if (preselect.inside_text) {
            ASSERT_TRUE(placeTextCursor(text("dst"), 3));
        }
        ASSERT_TRUE(activatePasteAction("paste"));
        document->ensureUpToDate();

        EXPECT_EQ(watch.dialogs, 1) << "exactly one external question per paste";
        EXPECT_EQ(watch.preselected_name, preselect.expected);
        while (DocumentUndo::undo(document)) {
            document->ensureUpToDate();
        }
    }
}

// An accepted "Remember" choice must never persist when the destination,
// document or clipboard changed while the question was open. The same oracle is
// applied to the native and the external preference families.
TEST_F(TextPasteTest, B21RememberThenStaleStateAbortsWithoutPersistingForNativeAndExternal)
{
    enum class Stale { Clipboard, Document, Caret };
    for (bool const native_payload : {true, false}) {
        for (auto stale : {Stale::Clipboard, Stale::Document, Stale::Caret}) {
            SCOPED_TRACE(std::string(native_payload ? "native " : "external ") + std::to_string(static_cast<int>(stale)));
            PrefRestore native_mode("/options/textpaste/mode");
            PrefRestore native_ask("/options/textpaste/ask");
            PrefRestore external_mode("/options/textpaste/external-mode");
            PrefRestore external_ask("/options/textpaste/external-ask");
            Preferences::get()->setInt("/options/textpaste/mode", 0);
            Preferences::get()->setBool("/options/textpaste/ask", true);
            Preferences::get()->setInt("/options/textpaste/external-mode", 0);
            Preferences::get()->setBool("/options/textpaste/external-ask", true);

            if (native_payload) {
                std::string const native = "vac-text-fragment\t2\nP\t\nR\tfont-size:24px\tBeta\n";
                setRichClipboard(native, std::string("Beta"));
            } else {
                setRawClipboardMulti({{"text/html", kHtmlStyled}, {"text/plain;charset=utf-8", "HTMLONE"}});
            }

            auto *destination = text("dst");
            ASSERT_TRUE(placeTextCursor(destination, 3));
            std::string const before_text = multilineText(destination);
            std::string xml_after_stale;

            PromptWatch watch;
            watch.dialog_name = native_payload ? "text-paste-dialog" : "text-paste-external-dialog";
            watch.remember_name = native_payload ? "text-paste-remember" : "text-paste-external-remember";
            watch.remember = true;
            watch.confirm = true;
            watch.side_effect = [&] {
                switch (stale) {
                    case Stale::Clipboard:
                        setPlainClipboard("replacement text");
                        break;
                    case Stale::Document:
                        item("rect")->getRepr()->setAttribute("x", "44");
                        document->ensureUpToDate();
                        break;
                    case Stale::Caret: {
                        auto *tool = textTool();
                        if (tool) {
                            tool->text_sel_start = iteratorAt(destination, 5);
                            tool->text_sel_end = tool->text_sel_start;
                        }
                        break;
                    }
                }
                xml_after_stale = xml();
            };
            PromptTimerGuard prompt_timer(watch);

            (void)activatePasteAction("paste"); // aborted: the action must not report a mutation
            document->ensureUpToDate();

            EXPECT_EQ(watch.dialogs, 1) << "accept-with-Remember must not open a second question";
            EXPECT_EQ(multilineText(destination), before_text) << "an aborted accept must not insert anything";
            EXPECT_EQ(xml(), xml_after_stale) << "an aborted accept must not change the document";
            EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/mode", -1), 0)
                << "the native mode must not be persisted by an aborted accept";
            EXPECT_TRUE(Preferences::get()->getBool("/options/textpaste/ask", true));
            EXPECT_EQ(Preferences::get()->getInt("/options/textpaste/external-mode", -1), 0)
                << "the external mode must not be persisted by an aborted accept";
            EXPECT_TRUE(Preferences::get()->getBool("/options/textpaste/external-ask", true));
            EXPECT_FALSE(DocumentUndo::undo(document)) << "an aborted accept must not create an Undo record";
            EXPECT_FALSE(DocumentUndo::redo(document)) << "an aborted accept must not change Redo state";
            if (stale == Stale::Caret) {
                auto *tool = textTool();
                ASSERT_TRUE(tool);
                EXPECT_TRUE(tool->text_sel_start == iteratorAt(destination, 5))
                    << "the caret must keep the value it had when the destination was invalidated";
            }
        }
    }
}

// ===========================================================================
// Repair r1: in-app missing-font notice presence/absence (repair 7)
// ===========================================================================

TEST_F(TextPasteTest, F02MissingFontNoticeAppearsForAnAppliedSourceStyle)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    std::string const html = "<p style=\"font-family:VacNoSuchFont;font-size:12px\">MissingFont</p>";
    setRawClipboardMulti({{"text/html", html}, {"text/plain;charset=utf-8", "MissingFont"}});

    auto const before = objectIds();
    desktop->getSelection()->set(item("rect"));
    desktop->setTool("/tools/select");
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();
    ASSERT_EQ(textsNotIn(before).size(), 1u);

    char const *message = desktop->messageStack()->currentMessage();
    ASSERT_TRUE(message) << "a paste applying an unavailable family must leave a notice";
    EXPECT_TRUE(containsSubstring(message, "VacNoSuchFont"))
        << "the notice must name the unavailable family; got: " << message;
}

TEST_F(TextPasteTest, F02MissingFontNoticeAbsentForDestinationModeAndAvailableFamily)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);

    // Destination mode discards the source fonts: no warning may be produced.
    std::string const missing = "<p style=\"font-family:VacNoSuchFont;font-size:12px\">MissingFont</p>";
    setRawClipboardMulti({{"text/html", missing}, {"text/plain;charset=utf-8", "MissingFont"}});
    auto *destination = text("dst");
    ASSERT_TRUE(placeTextCursor(destination, 3));
    ASSERT_TRUE(activatePasteAction("paste-without-formatting"));
    document->ensureUpToDate();
    char const *after_destination = desktop->messageStack()->currentMessage();
    EXPECT_FALSE(after_destination && containsSubstring(after_destination, "VacNoSuchFont"))
        << "Use-destination must not warn about discarded source fonts; got: "
        << (after_destination ? after_destination : "<none>");
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();

    // An available generic family produces no warning either.
    std::string const available = "<p style=\"font-family:serif;font-size:12px\">AvailableFont</p>";
    setRawClipboardMulti({{"text/html", available}, {"text/plain;charset=utf-8", "AvailableFont"}});
    auto const before = objectIds();
    desktop->getSelection()->set(item("rect"));
    desktop->setTool("/tools/select");
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();
    ASSERT_EQ(textsNotIn(before).size(), 1u);
    char const *after_available = desktop->messageStack()->currentMessage();
    EXPECT_FALSE(after_available && containsSubstring(after_available, "unavailable"))
        << "an available family must not produce a missing-font notice; got: "
        << (after_available ? after_available : "<none>");
}

// ===========================================================================
// Repair r1: external rich save/reopen (repair 7)
// ===========================================================================

TEST_F(TextPasteTest, B22ExternalRichSaveReopenKeepsRunAndParagraphFormat)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    std::string const html =
        "<p style=\"text-align:center\"><span style=\"font-weight:bold;font-size:32px;font-family:monospace\">BOLD</span></p>";
    setRawClipboardMulti({{"text/html", html}, {"text/plain;charset=utf-8", "BOLD"}});

    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    document->ensureUpToDate();
    auto const chars = characterSignatures(pasted);
    auto const paras = paragraphSignatures(pasted);
    auto const *id = pasted->getRepr()->attribute("id");
    ASSERT_TRUE(id);

    auto reopened = SPDocument::createNewDocFromMem(xml());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto *reloaded = cast<SPText>(reopened->getObjectById(id));
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(characterSignatures(reloaded), chars) << "a saved/reopened external rich paste must keep run format";
    EXPECT_EQ(paragraphSignatures(reloaded), paras)
        << "a saved/reopened external rich paste must keep paragraph format";
}

TEST_F(TextPasteTest, ExternalRichParagraphBreakDoesNotCarryPreviousRunStyle)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    std::string const html =
        "<p>Title <span style=\"font-size:28px;color:red\">HOT</span></p><p>body</p>";
    setRawClipboardMulti({{"text/html", html}, {"text/plain;charset=utf-8", "Title HOT\nbody"}});
    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Source));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    auto *pasted = created.front();
    document->ensureUpToDate();
    auto const content = multilineText(pasted);
    EXPECT_NE(content.find("body"), std::string::npos);
    auto const body_index = logicalIndexOf(pasted, "body");
    ASSERT_NE(body_index, std::numeric_limits<unsigned>::max());
    auto const hot_index = logicalIndexOf(pasted, "HOT");
    ASSERT_NE(hot_index, std::numeric_limits<unsigned>::max());
    auto const hot = characterSignature(pasted, iteratorAt(pasted, hot_index));
    auto const body = characterSignature(pasted, iteratorAt(pasted, body_index));
    EXPECT_NE(body, hot);
    EXPECT_FALSE(containsSubstring(body, "size=28.0000"));
    EXPECT_FALSE(containsSubstring(body, "fill=ff0000ff"));
}



// ===========================================================================
// Repair r1: external rich numeric sizes, scaled and unscaled (repair 7)
// ===========================================================================

namespace {
std::string rtfWithHalfPoints(int half_points, char const *text)
{
    return "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fnil Arial;}}\\f0\\fs" + std::to_string(half_points) + " " + text +
           "\\par}";
}
} // namespace

TEST_F(TextPasteTest, B18ExternalRtfNumericSizesAreDocumentSpace)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    struct SizeCase {
        int half_points; // \fsN is half-points: 24 => 12pt => 16px
        double expected_px;
    };
    SizeCase const cases[] = {{24, 16.0}, {48, 32.0}, {96, 64.0}};
    for (auto const &size_case : cases) {
        SCOPED_TRACE(size_case.half_points);
        setRawClipboard({"text/rtf"}, rtfWithHalfPoints(size_case.half_points, "SIZE"));
        auto const before = objectIds();
        ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
        auto created = textsNotIn(before);
        ASSERT_EQ(created.size(), 1u);
        document->ensureUpToDate();
        EXPECT_EQ(multilineText(created.front()), "SIZE");
        EXPECT_NEAR(renderedFontSize(created.front(), 0), size_case.expected_px, 0.1)
            << "the RTF half-point size must arrive as the documented document-space px size";
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
    }
}

TEST_F(TextPasteTest, B18ExternalRtfNumericSizeIsScaledWithTheDestinationContext)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    setRawClipboard({"text/rtf"}, rtfWithHalfPoints(24, "SIZE"));
    auto *scaled = text("dst-scaled");
    document->ensureUpToDate();
    std::string const xml_before = xml();
    double const destination_px_before = renderedFontSize(scaled, 0);
    unsigned const length_before = static_cast<unsigned>(multilineText(scaled).size());

    ASSERT_TRUE(pasteInside(scaled, 0, UI::TextPasteMode::Source));
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(scaled).substr(0, 4), "SIZE");
    EXPECT_NEAR(renderedFontSize(scaled, 0), 16.0, 0.2)
        << "12pt must stay 16 document px inside a 0.5-scaled destination group";
    EXPECT_NEAR(renderedFontSize(scaled, 4 + length_before - length_before), destination_px_before, 0.2)
        << "pre-existing destination characters must keep their physical size";
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), xml_before);
}



// A normal paste runs the text policy twice when the native representation is
// advertised but unusable (native shortcut, then the resolved text target). Both
// passes must observe ONE snapshot: the native and the plain representation are
// each requested exactly once, and the plain text is still pasted completely.
// The old two-read revision requested both representations twice and bought a
// second five-second deadline on the second pass.
TEST_F(TextPasteTest, PasteAttemptReusesOneSnapshotAcrossBothPolicyPasses)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);

    std::string const malformed_native = "vac-text-fragment\t9\nP\t\nR\tfont-size:24px\tX\n";
    auto *provider = reinterpret_cast<ChunkedProvider *>(makeChunkedProviderMulti(
        malformed_native, kNativeMime, "one snapshot plain text", "text/plain;charset=utf-8", 7, 1, false));
    ASSERT_TRUE(installClipboardProvider(GDK_CONTENT_PROVIDER(provider)));

    auto const before = objectIds();
    desktop->getSelection()->set(item("rect"));
    desktop->setTool("/tools/select");
    std::string const xml_before = xml();
    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    EXPECT_EQ(provider->requests, 1u)
        << "the native representation must be requested exactly once per user command";
    EXPECT_EQ(provider->requests2, 1u)
        << "the plain representation must be requested exactly once per user command";
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u) << "the malformed native payload must fall back to the complete plain text";
    EXPECT_EQ(multilineText(created.front()), "one snapshot plain text");

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), xml_before) << "one paste must be exactly one Undo step";
    EXPECT_FALSE(DocumentUndo::undo(document));
}

// A stalled plain producer reached through the two-pass path must abort the WHOLE
// command inside the command's single absolute deadline: no second request, no
// partial insert, no object/SVG fallback and no Undo record.
TEST_F(TextPasteTest, PasteAttemptStalledPlainAfterMalformedNativeAbortsWithinOneDeadline)
{
    std::string const malformed_native = "vac-text-fragment\t9\nP\t\nR\tfont-size:24px\tX\n";
    auto *provider = reinterpret_cast<ChunkedProvider *>(makeChunkedProviderMulti(
        malformed_native, kNativeMime, std::string(4096, 'S'), "text/plain;charset=utf-8", 16, 1, true));
    ASSERT_TRUE(installClipboardProvider(GDK_CONTENT_PROVIDER(provider)));

    auto *destination = text("dst");
    document->ensureUpToDate();
    std::string const before_xml = xml();
    std::string const before_text = multilineText(destination);
    auto const start = std::chrono::steady_clock::now();
    (void)activatePasteAction("paste"); // the action returns false on abort; boundedness is the oracle
    auto const elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    document->ensureUpToDate();

    EXPECT_EQ(provider->requests, 1u) << "an aborted command must not re-request the native representation";
    EXPECT_EQ(provider->requests2, 1u) << "an aborted command must not re-request the stalled plain representation";
    EXPECT_GE(elapsed_ms, 4000) << "the bounded read must wait for the command's single five-second deadline";
    EXPECT_LT(elapsed_ms, 15000) << "a stalled producer must never hang the GUI thread";
    EXPECT_EQ(xml(), before_xml) << "a timed-out payload must never be applied, not even partially";
    EXPECT_EQ(multilineText(destination), before_text);
    EXPECT_FALSE(DocumentUndo::undo(document)) << "an aborted paste must not create an Undo record";
}

// Repair 2 (object/SVG retrieval path is bounded): proved by the
// separate-process publisher harness case `E08_object_target_stall`. In this
// process `_retrieveClipboard` returns immediately for a LOCAL clipboard, so an
// in-process stall fixture is never read: the previous oracle's wall-clock
// lower bound measured nothing and its "no mutation" assertions also hold for a
// total no-op. The harness publisher advertises an object representation and
// never delivers its data, and the receiver asserts the bounded abort, the
// unchanged document and the measured elapsed time.


// ===========================================================================
// Repair r1: structured plain failure classification (repair 4)
// ===========================================================================
// E16 is proved on the Text-tool route, the only route where an in-process
// fixture can reach the plain read before any object import. With the caret
// inside an existing text object the reader classifies the over-cap stream as a
// structured abort; the paste stops with the over-limit warning and never falls
// through to the retrieval path.
//
// Discriminator: if the structured reason were replaced by "no usable text",
// paste() would continue to `_retrieveClipboard(CLIPBOARD_TEXT_TARGET)`, and
// that path flashes "Can't paste text outside of the text tool." for a local
// clipboard instead of "Clipboard text is too large to paste." The message
// assertion below therefore fails on the regression even though "document
// unchanged" would not.
TEST_F(TextPasteTest, E16OverCapInvalidUtf8SvgLookingPlainAbortsWithoutObjectFallback)
{
    std::string plain = "<?xml version=\"1.0\"?>";
    plain.resize(TP::MAX_CHARS + 1, '\xff'); // 65537 bytes: over the semantic cap and invalid UTF-8
    ASSERT_GT(plain.size(), TP::MAX_CHARS);
    std::string const svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect id=\"must-not-be-pasted\" width=\"10\" height=\"10\"/></svg>";
    auto *provider = reinterpret_cast<ChunkedProvider *>(makeChunkedProviderMulti(
        plain, "text/plain;charset=utf-8", svg, "image/svg+xml", 4096, 0, false));
    ASSERT_TRUE(installClipboardProvider(GDK_CONTENT_PROVIDER(provider)));
    ASSERT_TRUE(clipboardHasMime("image/svg+xml"));

    auto *destination = text("dst");
    ASSERT_TRUE(placeTextCursor(destination, 3));
    document->ensureUpToDate();
    auto const before = objectIds();
    std::string const xml_before = xml();
    std::string const text_before = multilineText(destination);

    (void)activatePasteAction("paste");
    document->ensureUpToDate();

    EXPECT_EQ(provider->requests, 1u) << "the plain representation is read exactly once";
    EXPECT_EQ(provider->requests2, 0u) << "the advertised SVG object must never be requested after the abort";
    EXPECT_EQ(objectIds(), before) << "the SVG object target must not run after the over-cap abort";
    EXPECT_TRUE(textsNotIn(before).empty());
    EXPECT_EQ(xml(), xml_before);
    EXPECT_EQ(multilineText(destination), text_before);
    EXPECT_FALSE(DocumentUndo::undo(document));
    EXPECT_FALSE(DocumentUndo::redo(document));
    char const *message = desktop->messageStack()->currentMessage();
    EXPECT_TRUE(message && containsSubstring(message, "too large"))
        << "the structured over-limit abort must be the reason the paste stopped";
}

// The structured reason also covers budget rejections that are NOT a wire-size
// violation: an SVG-looking plain payload over the paragraph budget must abort
// the whole paste instead of falling through to the retrieval/object path. The
// plain parser builds at most one run per paragraph, so the 4,096-run cap is
// unreachable from plain text and the paragraph cap is the reachable budget
// rejection (documented in the unit suite as well).
TEST_F(TextPasteTest, E16OverParagraphBudgetSvgLookingPlainAbortsWithoutObjectFallback)
{
    std::string plain = "<?xml";
    for (std::size_t i = 0; i < TP::MAX_PARAGRAPHS + 2; ++i) {
        plain += "\nx";
    }
    ASSERT_LT(plain.size(), TP::MAX_CHARS);
    std::string const svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect id=\"must-not-be-pasted\" width=\"10\" height=\"10\"/></svg>";
    auto *provider = reinterpret_cast<ChunkedProvider *>(makeChunkedProviderMulti(
        plain, "text/plain;charset=utf-8", svg, "image/svg+xml", 4096, 0, false));
    ASSERT_TRUE(installClipboardProvider(GDK_CONTENT_PROVIDER(provider)));
    ASSERT_TRUE(clipboardHasMime("image/svg+xml"));

    auto *destination = text("dst");
    ASSERT_TRUE(placeTextCursor(destination, 3));
    document->ensureUpToDate();
    auto const before = objectIds();
    std::string const xml_before = xml();
    std::string const text_before = multilineText(destination);

    (void)activatePasteAction("paste");
    document->ensureUpToDate();

    EXPECT_EQ(provider->requests, 1u) << "the plain representation is read exactly once";
    EXPECT_EQ(provider->requests2, 0u) << "the advertised SVG object must never be requested after the abort";
    EXPECT_EQ(objectIds(), before) << "a budget rejection must abort, never fall back to the object target";
    EXPECT_TRUE(textsNotIn(before).empty());
    EXPECT_EQ(xml(), xml_before);
    EXPECT_EQ(multilineText(destination), text_before);
    EXPECT_FALSE(DocumentUndo::undo(document));
    EXPECT_FALSE(DocumentUndo::redo(document));
    char const *message = desktop->messageStack()->currentMessage();
    EXPECT_TRUE(message && containsSubstring(message, "too large"))
        << "the structured run-budget abort must be the reason the paste stopped";
}

// Positive control for the two E16 oracles above: the same Text-tool route with
// the same fixture and the same SVG-looking plain payload DOES insert when the
// payload is under every cap. Without this control, "nothing changed" could hide
// a route that never read the clipboard at all (which is exactly how the local
// clipboard short-circuit made the old oracles vacuous).
TEST_F(TextPasteTest, E16UnderCapSvgLookingPlainIsReadOnceAndInsertedOnTheTextRoute)
{
    std::string const plain = "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    std::string const svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect id=\"control-object\" width=\"10\" height=\"10\"/></svg>";
    auto *provider = reinterpret_cast<ChunkedProvider *>(makeChunkedProviderMulti(
        plain, "text/plain;charset=utf-8", svg, "image/svg+xml", 4096, 0, false));
    ASSERT_TRUE(installClipboardProvider(GDK_CONTENT_PROVIDER(provider)));

    auto *destination = text("dst");
    ASSERT_TRUE(placeTextCursor(destination, 3));
    document->ensureUpToDate();
    auto const before = objectIds();
    std::string const text_before = multilineText(destination);

    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    EXPECT_EQ(provider->requests, 1u) << "the plain representation is read exactly once";
    EXPECT_EQ(provider->requests2, 0u) << "the text route must not request the SVG object";
    EXPECT_EQ(multilineText(destination), std::string("Hel") + plain + std::string("loWorld tail"))
        << "the under-cap SVG-looking text must be inserted verbatim at the caret";
    EXPECT_EQ(objectIds(), before) << "the text route must not import the advertised SVG object";
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(destination), text_before);
}

// ===========================================================================
// ROUTE-01: short-colour-string routing, both directions
// ===========================================================================
// The short-colour shortcut is a PLAIN-text convenience (copying "red" from a
// colour picker). A representation with rich capability - native or external
// HTML/RTF - stays a text decision even when its decoded text happens to be a
// short colour string, so an external styled copy can never silently become a
// fill change. Both halves are pinned here; the requirement row is ROUTE-01 in
// requirements-matrix.tsv.
TEST_F(TextPasteTest, RoutePlainShortColourStringStillSetsFillAndFillOpacity)
{
    setRawClipboard({"text/plain;charset=utf-8"}, "red");

    auto *target = item("rect");
    ASSERT_TRUE(target);
    desktop->getSelection()->set(target);
    desktop->setTool("/tools/select");
    auto const before = objectIds();
    std::string const rect_before = subtreeXml(target);

    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    EXPECT_TRUE(textsNotIn(before).empty()) << "a plain colour string must not create a text object";
    EXPECT_EQ(objectIds(), before);
    EXPECT_NE(subtreeXml(target), rect_before) << "the plain colour string must still apply a style";
    EXPECT_TRUE(containsSubstring(subtreeXml(target), "fill:#ff0000"))
        << "plain \"red\" keeps the existing fill/fill-opacity routing";
}

TEST_F(TextPasteTest, RouteExternalRichShortColourStringStaysATextDecision)
{
    PrefRestore ask("/options/textpaste/external-ask");
    Preferences::get()->setBool("/options/textpaste/external-ask", false);
    setRawClipboardMulti({{"text/html", "<p>red</p>"}, {"text/plain;charset=utf-8", "red"}});

    auto *target = item("rect");
    ASSERT_TRUE(target);
    desktop->getSelection()->set(target);
    desktop->setTool("/tools/select");
    auto const before = objectIds();
    std::string const rect_before = subtreeXml(target);

    ASSERT_TRUE(activatePasteAction("paste"));
    document->ensureUpToDate();

    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u)
        << "a rich representation is a text decision even when its decoded text is a colour name";
    EXPECT_EQ(multilineText(created.front()), "red");
    EXPECT_EQ(subtreeXml(target), rect_before) << "the rich paste must not become a fill change on the selection";
    ASSERT_TRUE(DocumentUndo::undo(document));
}

// The native record parser must apply the same numeric guard as the external
// construction path: a non-finite native style rejects the whole native payload
// so the complete plain alternative is used instead of importing NaN/Infinity
// into the SVG style.
TEST_F(TextPasteTest, NativeFragmentWithNonFiniteStyleFallsBackToPlain)
{
    for (char const *bad_style : {"opacity:nan", "opacity:1e999", "font-weight:1e999", "font-size:1e999px",
                                  "font-stretch:1e999%"}) {
        SCOPED_TRACE(bad_style);
        std::string const native = std::string("vac-text-fragment\t2\nP\t\nR\t") + bad_style + "\tBeta\n";
        setRichClipboard(native, std::string("plain alternative"));
        auto const before = objectIds();
        ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
        auto created = textsNotIn(before);
        ASSERT_EQ(created.size(), 1u);
        document->ensureUpToDate();
        EXPECT_EQ(multilineText(created.front()), "plain alternative")
            << "a non-finite native style must fall back to the complete plain text";
        EXPECT_FALSE(documentHasNonFiniteLength(document))
            << "no non-finite length may reach the document from a native payload";
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
    }
}

// A finite unit/percent value is still accepted by the native parser: the guard
// rejects only non-finite magnitudes.
TEST_F(TextPasteTest, NativeFragmentWithFiniteUnitValuesIsAccepted)
{
    std::string const native =
        "vac-text-fragment\t2\nP\t\nR\tfont-size:24px;font-stretch:150%;opacity:0.5\tBeta\n";
    setRichClipboard(native, std::string("plain alternative"));
    auto const before = objectIds();
    ASSERT_TRUE(pasteOutside(UI::TextPasteMode::Automatic));
    auto created = textsNotIn(before);
    ASSERT_EQ(created.size(), 1u);
    document->ensureUpToDate();
    EXPECT_EQ(multilineText(created.front()), "Beta") << "finite values must keep the native rich payload usable";
    EXPECT_NEAR(renderedFontSize(created.front(), 0), 24.0, 0.1);
}

} // namespace
