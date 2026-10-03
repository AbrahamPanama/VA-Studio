// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Paste-scoped missing-font diagnosis (external rich text and native fragments).
 *
 * Pure policy plus one injectable availability oracle. The policy part (generic
 * families, CSS family-list prefix rule, dedupe/cap, run scoping) never touches
 * the font system and is unit-testable with a fake oracle; the real oracle uses
 * the existing FontFactory/FontInstance resolution and adds no cache and no
 * change to global font resolution.
 *
 * Only source styles that will actually be applied are inspected: the caller
 * passes the fragment only when plan_insertion(...).source_run_styles is true,
 * so a destination-mode paste never warns about discarded source fonts.
 * The requested family is never rewritten by this module.
 *
 * Interface frozen by the integration owner; the implementation lives in
 * text-paste-font.cpp.
 */

#ifndef INKSCAPE_UI_TEXT_PASTE_FONT_H
#define INKSCAPE_UI_TEXT_PASTE_FONT_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ui/text-paste.h"

namespace Inkscape::UI::TextPaste::FontCheck {

enum class IssueKind {
    MissingFamily,        ///< no installed family matched the requested concrete token(s)
    MissingFace,          ///< family installed, requested weight/style/stretch face is not
    UnavailableVariation, ///< family installed, requested variation instance is not available
    MissingGlyphs,        ///< family/face installed, some code points have no glyph in that face
};

struct Issue {
    IssueKind kind = IssueKind::MissingFamily;
    std::string requested;          ///< requested family (or requested family + face keyword)
    std::string substitute;         ///< resolved family used for rendering, when known
    std::size_t paragraph_index = 0;
    std::size_t run_index = 0;      ///< index within its paragraph
    std::size_t missing_code_points = 0; ///< MissingGlyphs only
};

struct Report {
    std::vector<Issue> issues;
    bool truncated = false;           ///< the distinct-family cap was reached
    std::size_t families_checked = 0;
    std::size_t distinct_families = 0;
    bool empty() const { return issues.empty(); }
};

/** Injection point for deterministic policy tests; the real one lives in the .cpp. */
class Availability {
public:
    virtual ~Availability() = default;
    /// True when a concrete family name is installed (case-insensitive comparison is the caller's contract).
    virtual bool family_installed(std::string const &family) const = 0;
    /// True when the requested weight/style/stretch face (not a synthesized one) is available.
    virtual bool face_available(std::string const &family, std::string const &run_style) const = 0;
    /// The family the renderer falls back to; may be empty when unknown.
    virtual std::string substitute_for(std::string const &family) const = 0;
    /// True when every non-ignorable code point of @a text maps in the resolved face.
    virtual bool glyphs_available(std::string const &family, std::string const &run_style,
                                  std::string_view text) const = 0;
};

/// Per-paste cap on distinct requested concrete families that are looked up.
inline constexpr std::size_t MAX_FAMILIES = 64;

/// Generic CSS families and the Pango aliases: an intentional fallback request.
bool is_generic_family(std::string const &family);

/// Split a CSS font-family list into trimmed, unquoted tokens (never empty tokens).
std::vector<std::string> family_tokens(std::string const &family_list);

/// First token that is not generic, or empty when the whole list is generic.
std::string first_concrete_family(std::string const &family_list);

/// Analyse a fragment against @a oracle. Deterministic for a deterministic oracle.
Report inspect(Fragment const &fragment, Availability const &oracle, std::size_t max_families = MAX_FAMILIES);

/// Analyse against the real FontFactory environment (no new cache, no global change).
Report inspect(Fragment const &fragment, std::size_t max_families = MAX_FAMILIES);

/// One aggregated, localized, nonblocking message; empty for an empty report.
std::string summarize(Report const &report);

} // namespace Inkscape::UI::TextPaste::FontCheck

#endif // INKSCAPE_UI_TEXT_PASTE_FONT_H
