// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Bounded RTF -> TextPaste::Fragment importer (external clipboard input).
 *
 * Dedicated, single-pass, byte-oriented parser. Only GLib is used (g_convert
 * for code pages, g_utf8_validate, g_ascii_formatd); there is no new dependency,
 * no network, no file, no process and no embedded-object execution. Every brace
 * group, control word, binary skip, table and emitted run is bounded, and the
 * only fragment a caller may use is produced through
 * TextPaste::build_fragment(), so UTF-8/NUL/control/style/numeric/resource
 * checks cannot be bypassed here.
 *
 * Interface frozen by the integration owner; the implementation lives in
 * text-paste-rtf.cpp.
 */

#ifndef INKSCAPE_UI_TEXT_PASTE_RTF_H
#define INKSCAPE_UI_TEXT_PASTE_RTF_H

#include <cstddef>
#include <string>
#include <string_view>

#include "ui/text-paste.h"

namespace Inkscape::UI::TextPaste::Rtf {

enum class Status {
    ok,          ///< decoded under every cap (the fragment may still be empty)
    malformed,   ///< under every cap but not readable -> caller uses the complete plain alternative
    over_limit   ///< a declared cap was hit -> caller aborts the whole paste
};

/** Production defaults; tests may shrink every value. */
struct Limits {
    std::size_t max_input_bytes = MAX_PAYLOAD_BYTES; ///< 256 KiB
    std::size_t max_text_bytes = MAX_CHARS;          ///< 65,536 decoded UTF-8 text bytes
    std::size_t max_paragraphs = MAX_PARAGRAPHS;     ///< 1,024
    std::size_t max_runs = MAX_RUNS;                 ///< 4,096
    std::size_t max_run_bytes = MAX_RUN_LENGTH;      ///< 8,192
    std::size_t max_depth = 64;                      ///< group nesting
    std::size_t max_groups = 32768;                  ///< visited groups
    std::size_t max_fonts = 256;                     ///< font table entries
    std::size_t max_colors = 256;                    ///< colour table entries
};

/** Stable diagnostic counters (no user-visible strings in the parser). */
struct Diagnostics {
    std::size_t groups_visited = 0;
    std::size_t skipped_destinations = 0;
    std::size_t pictures_skipped = 0;
    std::size_t objects_skipped = 0;
    std::size_t table_cells = 0;
    std::size_t table_rows = 0;
    std::size_t list_paragraphs = 0;
    std::size_t hidden_chars = 0;
    std::size_t line_breaks_flattened = 0;
    std::size_t layout_breaks = 0;
    std::size_t line_height_at_least_flattened = 0;
    std::size_t paragraph_style_refs = 0;
    std::size_t character_style_refs = 0;
    std::size_t unsupported_controls = 0;
    std::size_t dropped_style_declarations = 0;
    std::size_t truncated_font_names = 0;
    std::size_t codepage_fallbacks = 0;
    std::size_t unsafe_codepage = 0;
    std::size_t replacement_characters = 0;
    std::size_t surrogate_pairs_combined = 0;
    std::size_t surrogate_replacements = 0;
    std::size_t uc_clamped = 0;
    std::size_t fonts_referenced_before_table = 0;
    std::size_t split_runs = 0;
    std::size_t skipped_bytes_bin = 0;
};

struct Result {
    Status status = Status::malformed;
    Fragment fragment;                   ///< only meaningful for Status::ok
    bool has_meaningful_styles = false;  ///< TextPaste::has_meaningful_styles(fragment)
    Diagnostics diagnostics;
    std::string error;                   ///< machine-facing reason for non-ok results
};

/** Decode one RTF representation. Deterministic: same bytes + limits -> same result. */
Result decode(std::string_view rtf, Limits const &limits = {});

} // namespace Inkscape::UI::TextPaste::Rtf

#endif // INKSCAPE_UI_TEXT_PASTE_RTF_H
