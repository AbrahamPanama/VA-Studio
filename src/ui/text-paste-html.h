// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Bounded HTML -> TextPaste::Fragment importer (external clipboard input).
 *
 * Pure decoder: no GTK, no desktop, no document, no clipboard, no I/O and no
 * network. Markup is parsed with libxml2's HTML SAX2 push parser and CSS
 * declarations with the vendored libcroco; both are already linked into the GUI
 * binary, so this adds no dependency. Every resource cap is enforced *during*
 * parsing (xmlStopParser on a depth/node/text/paragraph/run trip), never after
 * building an unbounded tree. No DTD load, no entity expansion, no XInclude, no
 * external stylesheet, script or image is ever fetched or executed.
 *
 * The only fragment a caller may use is the one returned with
 * `Status::ok`; it was produced through TextPaste::build_fragment(), so the
 * UTF-8/NUL/control/style/numeric/resource checks cannot be bypassed here.
 *
 * Every CSS declaration text (inline `style`, `<style>` rule bodies) passes a
 * deterministic byte-level lexical pre-scan before the libcroco call: comments,
 * strings and escapes are honoured, unbalanced parenthesis/function nesting
 * above the decoder's documented literal cap is rejected as
 * `Status::malformed` (never `limit_exceeded`, so the caller can still run the
 * complete plain fallback), and an unterminated string/comment/escape rejects
 * the declaration text instead of reaching libcroco's error-recovery re-parse.
 * An independent absolute bound on the number of `(` bytes backs the lexical
 * model up, because libcroco's character-based recovery can re-parse constructs
 * a lexical scanner cannot see through. libcroco's declaration parser recurses
 * per nested function without a depth guard, so these checks are what make the
 * hostile-CSS route stack-safe.
 *
 * Interface frozen by the integration owner; the implementation lives in
 * text-paste-html.cpp.
 */

#ifndef INKSCAPE_UI_TEXT_PASTE_HTML_H
#define INKSCAPE_UI_TEXT_PASTE_HTML_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ui/text-paste.h"

namespace Inkscape::UI::TextPaste::HtmlImport {

enum class Status {
    ok,            ///< Fragment is complete and usable (may still be empty: no text)
    malformed,     ///< under every cap, no usable rich text -> caller uses the complete plain alternative
    limit_exceeded ///< a declared cap was hit -> caller aborts the whole paste
};

/** Production defaults; tests may shrink every value. */
struct Limits {
    std::size_t max_input_bytes = MAX_PAYLOAD_BYTES; ///< 256 KiB representation budget
    std::size_t max_text_bytes = MAX_CHARS;          ///< 65,536 decoded UTF-8 text bytes
    std::size_t max_paragraphs = MAX_PARAGRAPHS;     ///< 1,024
    std::size_t max_runs = MAX_RUNS;                 ///< 4,096
    std::size_t max_run_bytes = MAX_RUN_LENGTH;      ///< 8,192
    std::size_t max_depth = 64;                      ///< element nesting
    std::size_t max_nodes = 32768;                   ///< visited elements
    std::size_t max_css_bytes = 65536;               ///< <style> source budget
    std::size_t max_css_rules = 2048;                ///< parsed rules
    std::size_t max_css_declarations = 8192;         ///< parsed declarations
};

/** Byte range of the HTML document inside a CF_HTML (or bare HTML) payload. */
struct Envelope {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool cf_html = false; ///< a CF_HTML header was detected and validated
    bool valid = false;   ///< the range may be used
};

struct Result {
    Status status = Status::malformed;
    Fragment fragment;                   ///< only meaningful for Status::ok
    bool has_meaningful_styles = false;  ///< TextPaste::has_meaningful_styles(fragment)
    std::vector<std::string> losses;     ///< bounded reason codes (never user-facing strings)
    std::size_t consumed_envelope_bytes = 0;
    long long decode_microseconds = 0;   ///< recorded, never used to extend the clipboard deadline
    std::string error;                   ///< machine-facing reason for non-ok results
};

/**
 * Detect and validate a Windows CF_HTML byte-offset envelope (also accepted for
 * a bare document, in which case cf_html is false and the whole payload is the
 * range). Pure and unit-testable; never throws.
 */
Envelope unwrap_html_envelope(std::string_view bytes);

/**
 * Decode one HTML representation. `bytes` is the exact clipboard payload: bare
 * HTML (macOS `public.html`, Linux `text/html`) or a CF_HTML envelope. The
 * optional MIME alias is only used for diagnostics and alias-specific
 * unwrapping; the content is sniffed, never trusted from the MIME spelling.
 */
Result decode(std::string_view bytes, Limits const &limits = {});
Result decode_representation(std::string_view bytes, std::string_view mime_alias, Limits const &limits = {});

} // namespace Inkscape::UI::TextPaste::HtmlImport

#endif // INKSCAPE_UI_TEXT_PASTE_HTML_H
