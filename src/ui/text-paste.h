// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Context-aware rich text clipboard fragment: bounded representation,
 * validation and insertion policy.
 *
 * This header is intentionally header-only (core ownership, no CMake change).
 * The rich payload is clipboard input, never trusted memory: it is size-bounded,
 * versioned, validated field-by-field and restricted to an allow-list of CSS
 * style properties. Anything unsafe is dropped; a malformed payload is rejected
 * as a whole and the caller falls back to plain text.
 *
 * Wire format (version 2), one record per line, fields escaped with backslash
 * escapes (\\, \t, \n, \r so records stay line/tab delimited):
 *   vac-text-fragment\t2
 *   P\t<paragraph style>
 *   R\t<run style>\t<run text>
 *   ...
 *
 * Version 2 changes the meaning of absolute typography lengths: they are
 * normalized to DOCUMENT-SPACE CSS PIXELS at capture (see the unit contract
 * below). Version 1 carried raw local computed lengths with no source
 * coordinate identity, so it cannot be reinterpreted exactly and is rejected as
 * a rich payload; the complete plain-text representation is used instead.
 */

#ifndef INKSCAPE_UI_TEXT_PASTE_H
#define INKSCAPE_UI_TEXT_PASTE_H

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>

#include "ui/clipboard.h" // Inkscape::UI::TextPasteMode

namespace Inkscape::UI::TextPaste {

inline constexpr char const *MIME_TYPE = "application/x-vac-studio-text-fragment";
inline constexpr unsigned FORMAT_VERSION = 2;
/** Oldest rich format this build can consume as normalized document-space lengths. */
inline constexpr unsigned MIN_SUPPORTED_FORMAT_VERSION = 2;
/** Legacy rich format: raw local computed lengths, no source coordinate identity. */
inline constexpr unsigned LEGACY_LOCAL_LENGTH_VERSION = 1;

/** True when a scalar is usable for a local <-> document unit conversion. */
inline bool is_usable_scale(double scale)
{
    return std::isfinite(scale) && scale > 0.0;
}

// Hard bounds applied before and during parsing. MAX_PAYLOAD_BYTES and
// MAX_CHARS are byte budgets over the UTF-8 wire/plain text (a multi-byte
// character counts as its full encoded size); they are never applied by
// cutting a UTF-8 sequence or silently dropping a valid prefix.
inline constexpr std::size_t MAX_PAYLOAD_BYTES = 256u * 1024u;
inline constexpr std::size_t MAX_PARAGRAPHS = 1024u;
inline constexpr std::size_t MAX_RUNS = 4096u;
inline constexpr std::size_t MAX_CHARS = 65536u;
inline constexpr std::size_t MAX_RUN_LENGTH = 8192u;
inline constexpr std::size_t MAX_STYLE_LENGTH = 2048u;
inline constexpr std::size_t MAX_STYLE_VALUE_LENGTH = 256u;

/** A run of characters sharing one computed character style. */
struct Run {
    std::string text;  ///< UTF-8, never contains a newline or control char (a tab is allowed)
    std::string style; ///< canonical "name:value;" list of allowed character props
};

/** One logical paragraph: newline-separated block of styled runs. */
struct Paragraph {
    std::string style; ///< allowed paragraph-level properties only
    std::vector<Run> runs;
};

struct Fragment {
    std::vector<Paragraph> paragraphs;
    std::string plain; ///< authoritative logical characters, '\n' between paragraphs

    bool empty() const
    {
        for (auto const &p : paragraphs) {
            for (auto const &r : p.runs) {
                if (!r.text.empty()) {
                    return false;
                }
            }
        }
        // A fragment may legitimately hold line breaks only (for example a
        // clipboard containing just "\n"): the authoritative plain text still
        // carries those characters, so such a fragment is not empty.
        return plain.empty();
    }

    bool has_styles() const
    {
        for (auto const &p : paragraphs) {
            if (!p.style.empty()) {
                return true;
            }
            for (auto const &r : p.runs) {
                if (!r.style.empty()) {
                    return true;
                }
            }
        }
        return false;
    }
};

/** What the paste operation should do at the destination. */
struct InsertionPlan {
    bool create_object = false;             ///< create a new editable text object
    bool source_run_styles = false;         ///< apply copied per-run character styles
    bool source_paragraph_style = false;    ///< apply copied paragraph properties
    bool destination_typing_style = false;  ///< adopt destination typing style
};

/**
 * Pure client-side policy (no GUI, no clipboard): decides how a fragment is
 * applied. Testable in isolation; the clipboard path still must be exercised.
 */
inline InsertionPlan plan_insertion(TextPasteMode mode, bool inside_existing_text, bool have_source_styles)
{
    InsertionPlan plan;
    switch (mode) {
        case TextPasteMode::Automatic:
            plan.create_object = !inside_existing_text;
            plan.source_run_styles = !inside_existing_text && have_source_styles;
            plan.source_paragraph_style = !inside_existing_text && have_source_styles;
            plan.destination_typing_style = inside_existing_text;
            break;
        case TextPasteMode::Source:
            plan.create_object = !inside_existing_text;
            plan.source_run_styles = have_source_styles;
            plan.source_paragraph_style = !inside_existing_text && have_source_styles;
            // Missing rich format falls back to destination/default style.
            plan.destination_typing_style = !have_source_styles;
            break;
        case TextPasteMode::Destination:
            plan.create_object = !inside_existing_text;
            plan.destination_typing_style = true;
            break;
    }
    return plan;
}

// ---------------------------------------------------------------------------
// Unit contract for absolute typography lengths
// ---------------------------------------------------------------------------
//
// A native fragment carries absolute typography lengths in DOCUMENT-SPACE CSS
// PIXELS. Extraction multiplies the computed local length L by the effective
// item-to-document scalar s = i2doc_affine().descrim() exactly once; the
// receiving run-style API (sp_te_apply_style) then divides by the destination's
// descrim() exactly once. A round trip inside one document is therefore the
// identity, and a cross-document paste keeps the physical size. Unitless
// numbers already in document space are unchanged by s == 1.
//
// The receiving API is not a perfect inverse for every property, so the
// receiving site (a paste-local adaptation, never a change to the shared
// scaler) establishes the two real destination contexts and prepares the CSS:
//   * s_div: the scalar sp_te_apply_style() will divide by. It is
//     descrim(get_common_ancestor(first, last)) of the *inserted* range, which
//     is established from the live layout at the receiving site, not assumed
//     from the outer text root or the source anchor.
//   * s_render: the scalar of the coordinate context that will own the inserted
//     characters (nearest item of the first inserted character).
// prepare_for_receiving_api() then writes V = D * s_div / s_render for absolute
// lengths, so stored local = V / s_div = D / s_render and the visible size is D.
//
// Properties that sp_css_attr_scale (src/style.cpp) scales numerically without
// requiring a unit (letter-spacing, word-spacing, kerning, baseline-shift,
// stroke-width, stroke-dashoffset, stroke-dasharray) keep their relative form
// (`em`, `ex`, `%`, other units): the numeric part is pre-multiplied by s_div so
// the shared division cancels and the unit survives unchanged. font-size and
// line-height are only scaled by that API when a unit is present and it is not
// `%` or `e*`, so their relative forms pass through untouched; a unitless
// line-height stays a multiplier. text-indent is not in the shared scaler at
// all, so its absolute/unitless forms are converted straight to the render
// context and its relative forms are preserved. Keywords (normal, auto, sub,
// super, none, inherit) always pass through unchanged.

enum class LengthGroup { Compensated, Uncompensated };

/** How sp_css_attr_scale() treats a numeric token of this property. */
enum class ApiScaling {
    AbsoluteOrUnitless, ///< only_with_units == false: every numeric token is scaled
    AbsoluteUnitsOnly,  ///< only_with_units == true: unitless, '%' and 'e*' units are skipped
    Never,              ///< property is not in the shared scaler at all (text-indent)
};

namespace detail {

struct LengthPropertyRule {
    std::string_view name;
    ApiScaling api;
    bool list; ///< comma-separated value list (stroke-dasharray)
    LengthGroup group;
};

inline constexpr LengthPropertyRule LENGTH_PROPERTY_RULES[] = {
    {"font-size", ApiScaling::AbsoluteUnitsOnly, false, LengthGroup::Compensated},
    {"line-height", ApiScaling::AbsoluteUnitsOnly, false, LengthGroup::Compensated},
    {"letter-spacing", ApiScaling::AbsoluteOrUnitless, false, LengthGroup::Compensated},
    {"word-spacing", ApiScaling::AbsoluteOrUnitless, false, LengthGroup::Compensated},
    {"kerning", ApiScaling::AbsoluteOrUnitless, false, LengthGroup::Compensated},
    {"baseline-shift", ApiScaling::AbsoluteOrUnitless, false, LengthGroup::Compensated},
    {"stroke-width", ApiScaling::AbsoluteOrUnitless, false, LengthGroup::Compensated},
    {"stroke-dashoffset", ApiScaling::AbsoluteOrUnitless, false, LengthGroup::Compensated},
    {"stroke-dasharray", ApiScaling::AbsoluteOrUnitless, true, LengthGroup::Compensated},
    {"text-indent", ApiScaling::Never, false, LengthGroup::Uncompensated},
};

inline LengthPropertyRule const *length_rule(std::string_view name)
{
    for (auto const &rule : LENGTH_PROPERTY_RULES) {
        if (rule.name == name) {
            return &rule;
        }
    }
    return nullptr;
}

/** Locale-independent fixed formatting without early rounding (10 significant digits). */
inline std::string format_length(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buffer, sizeof buffer, "%.10g", value);
    return std::string(buffer);
}

/** Absolute CSS length unit to CSS px (CSS Values 3: 96 px per inch). */
inline bool absolute_unit_px(std::string_view unit, double &factor)
{
    if (unit == "px") { factor = 1.0; return true; }
    if (unit == "pt") { factor = 96.0 / 72.0; return true; }
    if (unit == "pc") { factor = 16.0; return true; }
    if (unit == "in") { factor = 96.0; return true; }
    if (unit == "mm") { factor = 96.0 / 25.4; return true; }
    if (unit == "cm") { factor = 96.0 / 2.54; return true; }
    if (unit == "q") { factor = 96.0 / 25.4 / 4.0; return true; }
    return false;
}

/** One parsed CSS value item: number (optional), unit name and remainder.
 *
 * The parts are owned strings, not views: g_ascii_strtod() needs a
 * NUL-terminated buffer, so parsing copies the token into a local string whose
 * lifetime ends before the caller uses the result. Keeping views into that
 * buffer read freed stack memory (observed as "0.5em" -> "0.5unknown" and
 * "0.5m"), so every part is copied out while the buffer is alive.
 */
struct LengthToken {
    std::string leading; ///< whitespace before the number
    double number = 0.0;
    bool numeric = false;     ///< false for a keyword (normal, sub, none, ...)
    bool finite = true;
    std::string unit;    ///< alphabetic unit after the number, empty when none
    std::string tail;    ///< remainder after the unit ('%' for percentages)
};

inline LengthToken parse_length_token(std::string_view token)
{
    LengthToken out;
    std::size_t begin = 0;
    while (begin < token.size() && g_ascii_isspace(token[begin])) {
        ++begin;
    }
    out.leading = std::string(token.substr(0, begin));
    std::string const rest(token.substr(begin));
    char *end = nullptr;
    double const number = g_ascii_strtod(rest.c_str(), &end);
    if (end == rest.c_str()) {
        return out; // keyword or non-numeric: preserved verbatim
    }
    out.numeric = true;
    out.number = number;
    out.finite = std::isfinite(number);
    std::string_view const after = std::string_view(rest).substr(static_cast<std::size_t>(end - rest.c_str()));
    std::size_t unit_length = 0;
    while (unit_length < after.size() && g_ascii_isalpha(after[unit_length])) {
        ++unit_length;
    }
    out.unit = std::string(after.substr(0, unit_length));
    out.tail = std::string(after.substr(unit_length));
    return out;
}

/** Mirrors only_with_units == false: a unitless number is a local user unit. */
inline bool unitless_is_length(ApiScaling api)
{
    return api != ApiScaling::AbsoluteUnitsOnly;
}

/** True when the shared scaler multiplies this token's number by 1/ex. */
inline bool api_scales_token(LengthToken const &token, ApiScaling api)
{
    switch (api) {
        case ApiScaling::AbsoluteOrUnitless:
            return true;
        case ApiScaling::AbsoluteUnitsOnly:
            // sp_css_attr_scale_property_single(..., only_with_units=true) skips
            // a unitless value, a percentage and any unit starting with 'e'.
            if (token.unit.empty() && token.tail.empty()) {
                return false;
            }
            if (token.unit.empty() && !token.tail.empty() && token.tail.front() == '%') {
                return false;
            }
            if (!token.unit.empty() && token.unit.front() == 'e') {
                return false;
            }
            return true;
        case ApiScaling::Never:
            return false;
    }
    return false;
}

/** True when the token is a length in the element's own user space. */
inline bool token_is_absolute_length(LengthToken const &token, ApiScaling api)
{
    if (!token.unit.empty()) {
        double factor = 0.0;
        return absolute_unit_px(token.unit, factor);
    }
    return token.tail.empty() && unitless_is_length(api);
}

/** Local source lengths -> document-space lengths (one multiplication). */
inline std::optional<std::string> capture_length_token(std::string_view token, LengthPropertyRule const &rule,
                                                       double source_scale)
{
    auto const parsed = parse_length_token(token);
    if (!parsed.numeric) {
        return std::string(token);
    }
    if (!parsed.finite) {
        return std::nullopt;
    }
    if (!token_is_absolute_length(parsed, rule.api)) {
        return std::string(token); // relative: never multiplied by a length scale
    }
    double unit_factor = 1.0;
    if (!parsed.unit.empty() && !absolute_unit_px(parsed.unit, unit_factor)) {
        return std::string(token);
    }
    double const px = parsed.number * unit_factor * source_scale;
    if (!std::isfinite(px)) {
        return std::nullopt;
    }
    std::string out(parsed.leading);
    out += format_length(px);
    out += "px";
    out.append(parsed.tail.data(), parsed.tail.size());
    return out;
}

/**
 * Document-space lengths -> the value sp_te_apply_style() must receive so that
 * its own division by @a division_scale lands in the render context
 * @a render_scale. API-scaled relative forms are pre-multiplied so the shared
 * numeric scaling cancels and the unit keeps its relative meaning.
 */
inline std::optional<std::string> receiving_length_token(std::string_view token, LengthPropertyRule const &rule,
                                                         double division_scale, double render_scale)
{
    auto const parsed = parse_length_token(token);
    if (!parsed.numeric) {
        return std::string(token);
    }
    if (!parsed.finite) {
        return std::nullopt;
    }
    bool const scaled_by_api = api_scales_token(parsed, rule.api);
    if (token_is_absolute_length(parsed, rule.api)) {
        double unit_factor = 1.0;
        if (!parsed.unit.empty() && !absolute_unit_px(parsed.unit, unit_factor)) {
            return std::nullopt;
        }
        double const local = parsed.number * unit_factor / render_scale;
        double const value = scaled_by_api ? local * division_scale : local;
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
        std::string out(parsed.leading);
        out += format_length(value);
        out += "px";
        return out;
    }
    if (!scaled_by_api) {
        return std::string(token); // relative and untouched by the API
    }
    double const value = parsed.number * division_scale;
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    std::string out(parsed.leading);
    out += format_length(value);
    out.append(parsed.unit.data(), parsed.unit.size());
    out.append(parsed.tail.data(), parsed.tail.size());
    return out;
}

/** Document-space lengths -> destination local for a direct root style write. */
inline std::optional<std::string> root_write_length_token(std::string_view token, LengthPropertyRule const &rule,
                                                          double render_scale)
{
    auto const parsed = parse_length_token(token);
    if (!parsed.numeric) {
        return std::string(token);
    }
    if (!parsed.finite) {
        return std::nullopt;
    }
    if (!token_is_absolute_length(parsed, rule.api)) {
        return std::string(token);
    }
    double unit_factor = 1.0;
    if (!parsed.unit.empty() && !absolute_unit_px(parsed.unit, unit_factor)) {
        return std::string(token);
    }
    double const local = parsed.number * unit_factor / render_scale;
    if (!std::isfinite(local)) {
        return std::nullopt;
    }
    std::string out(parsed.leading);
    out += format_length(local);
    out += "px";
    return out;
}

/**
 * Rewrite the allow-listed length declarations of a canonical "name:value;"
 * list with @a token_fn. Declaration order is preserved, untouched declarations
 * are copied verbatim, and a non-finite value or an over-bound result rejects
 * the whole list instead of emitting a partial conversion.
 */
template <typename TokenFn>
inline std::optional<std::string> rewrite_lengths_in_css(std::string_view css, TokenFn token_fn)
{
    std::string out;
    out.reserve(css.size());
    std::size_t pos = 0;
    while (pos < css.size()) {
        auto const semi = css.find(';', pos);
        auto const decl = css.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = semi == std::string_view::npos ? css.size() : semi + 1;
        if (decl.empty()) {
            continue;
        }
        auto const colon = decl.find(':');
        auto const rule = colon == std::string_view::npos ? nullptr : length_rule(decl.substr(0, colon));
        if (!rule) {
            out.append(decl.data(), decl.size());
            out += ';';
        } else {
            auto const value = decl.substr(colon + 1);
            std::string rewritten;
            if (!rule->list) {
                auto converted = token_fn(value, *rule);
                if (!converted) {
                    return std::nullopt;
                }
                rewritten = std::move(*converted);
            } else {
                // Mirror the shared list scaler: a keyword item makes the whole
                // declaration a no-op there, so it is preserved verbatim here.
                bool keyword_item = false;
                for (std::size_t item_pos = 0; item_pos <= value.size();) {
                    auto const comma = value.find(',', item_pos);
                    auto const item = comma == std::string_view::npos ? value.substr(item_pos)
                                                                      : value.substr(item_pos, comma - item_pos);
                    if (!detail::parse_length_token(item).numeric) {
                        keyword_item = true;
                        break;
                    }
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    item_pos = comma + 1;
                }
                if (keyword_item) {
                    rewritten.assign(value.data(), value.size());
                } else {
                    std::size_t item_pos = 0;
                    for (;;) {
                        auto const comma = value.find(',', item_pos);
                        auto const item = comma == std::string_view::npos ? value.substr(item_pos)
                                                                          : value.substr(item_pos, comma - item_pos);
                        auto converted = token_fn(item, *rule);
                        if (!converted) {
                            return std::nullopt;
                        }
                        rewritten += *converted;
                        if (comma == std::string_view::npos) {
                            break;
                        }
                        rewritten += ',';
                        item_pos = comma + 1;
                    }
                }
            }
            out.append(decl.substr(0, colon));
            out += ':';
            out += rewritten;
            out += ';';
        }
        if (out.size() > MAX_STYLE_LENGTH) {
            return std::nullopt;
        }
    }
    return out;
}

/** True when every numeric token of a length declaration is finite and usable. */
inline bool length_value_is_valid(std::string_view value, LengthPropertyRule const &rule)
{
    auto const parsed = parse_length_token(value);
    if (!parsed.numeric) {
        // font-size has no keyword that identifies one document-space size.
        return rule.name != std::string_view("font-size");
    }
    if (!parsed.finite) {
        return false;
    }
    // Reject trailing junk after the unit or the percentage sign.
    bool const percent = parsed.unit.empty() && !parsed.tail.empty() && parsed.tail.front() == '%';
    std::string_view const tail = parsed.tail; // owns the bytes; the view stays valid
    std::string_view const rest = percent ? tail.substr(1) : tail;
    for (char c : rest) {
        if (!g_ascii_isspace(c)) {
            return false;
        }
    }
    if (rule.name == std::string_view("font-size")) {
        // Version 2 carries absolute document-space font sizes. A relative or
        // unitless font size cannot be resolved without the source context and
        // the shared scaler would silently leave it unscaled (REVIEW_DESIGN
        // RD-06), so it is rejected instead of pasted at a wrong size.
        double unit_factor = 0.0;
        if (parsed.unit.empty() || !absolute_unit_px(parsed.unit, unit_factor)) {
            return false;
        }
        return parsed.number > 0.0;
    }
    return true;
}

} // namespace detail

/**
 * Local source lengths -> normalized document-space fragment lengths.
 * Applies to every allow-listed length property (compensated and not), so the
 * fragment alone is self-contained and no original object is needed later.
 * Relative forms are never multiplied (their unit keeps the meaning).
 */
inline std::optional<std::string> normalize_fragment_lengths(std::string_view css, double source_scale)
{
    if (!is_usable_scale(source_scale)) {
        return std::nullopt;
    }
    return detail::rewrite_lengths_in_css(css, [source_scale](std::string_view token, detail::LengthPropertyRule const &rule) {
        return detail::capture_length_token(token, rule, source_scale);
    });
}

/**
 * Normalized document-space lengths -> destination local lengths for a direct
 * root style write that does NOT pass through sp_te_apply_style (both groups).
 */
inline std::optional<std::string> localize_for_root_write(std::string_view css, double render_scale)
{
    if (!is_usable_scale(render_scale)) {
        return std::nullopt;
    }
    return detail::rewrite_lengths_in_css(css, [render_scale](std::string_view token, detail::LengthPropertyRule const &rule) {
        return detail::root_write_length_token(token, rule, render_scale);
    });
}

/**
 * Prepare normalized document-space lengths for sp_te_apply_style at the actual
 * receiving site. @a division_scale is the descrim() that API will divide by
 * (the deepest common ancestor of the inserted range); @a render_scale is the
 * descrim() of the coordinate context that will own the inserted characters.
 * Both must be established from the live layout; neither is assumed.
 */
inline std::optional<std::string> prepare_for_receiving_api(std::string_view css, double division_scale,
                                                            double render_scale)
{
    if (!is_usable_scale(division_scale) || !is_usable_scale(render_scale)) {
        return std::nullopt;
    }
    return detail::rewrite_lengths_in_css(
        css, [division_scale, render_scale](std::string_view token, detail::LengthPropertyRule const &rule) {
            return detail::receiving_length_token(token, rule, division_scale, render_scale);
        });
}

/**
 * Rigorous, context-independent upper bound for the length of the style
 * returned by prepare_for_receiving_api(): every numeric length token is
 * replaced by format_length() output, and "%.10g" of any finite double is at
 * most 24 characters. Non-length declarations and the non-numeric parts of a
 * declaration are copied verbatim, so the prepared string can never exceed
 * css.size() + 24 * (number of numeric length tokens). Checking this bound
 * before mutation proves that no measured receiving context can make
 * rewrite_lengths_in_css() reject the style for exceeding MAX_STYLE_LENGTH,
 * independently of how the number formatter renders each individual magnitude
 * (string length is not monotone in the value, so a per-context sample alone
 * cannot prove the bound).
 */
inline std::size_t numeric_length_token_count(std::string_view css)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < css.size()) {
        auto const semi = css.find(';', pos);
        auto const decl = css.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = semi == std::string_view::npos ? css.size() : semi + 1;
        if (decl.empty()) {
            continue;
        }
        auto const colon = decl.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        auto const rule = detail::length_rule(decl.substr(0, colon));
        if (!rule) {
            continue;
        }
        auto const value = decl.substr(colon + 1);
        if (!rule->list) {
            if (detail::parse_length_token(value).numeric) {
                ++count;
            }
        } else {
            for (std::size_t item_pos = 0; item_pos <= value.size();) {
                auto const comma = value.find(',', item_pos);
                auto const item = comma == std::string_view::npos ? value.substr(item_pos)
                                                                  : value.substr(item_pos, comma - item_pos);
                if (detail::parse_length_token(item).numeric) {
                    ++count;
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                item_pos = comma + 1;
            }
        }
    }
    return count;
}

/** True when every receiving context's prepared style stays within MAX_STYLE_LENGTH. */
inline bool receiving_preparation_fits(std::string_view css)
{
    return css.size() + std::size_t(24) * numeric_length_token_count(css) <= MAX_STYLE_LENGTH;
}

/**
 * Validate every allow-listed length declaration of one canonical style list
 * before any document mutation: non-finite numbers, malformed values and a
 * relative/unitless/non-positive font size are rejected as a whole.
 */
inline bool style_lengths_are_valid(std::string_view css)
{
    std::size_t pos = 0;
    while (pos <= css.size()) {
        auto const semi = css.find(';', pos);
        auto const decl = css.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = semi == std::string_view::npos ? css.size() + 1 : semi + 1;
        if (decl.empty()) {
            continue;
        }
        auto const colon = decl.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        auto const rule = detail::length_rule(decl.substr(0, colon));
        if (!rule) {
            continue;
        }
        auto const value = decl.substr(colon + 1);
        if (!rule->list) {
            if (!detail::length_value_is_valid(value, *rule)) {
                return false;
            }
            continue;
        }
        for (std::size_t item_pos = 0; item_pos <= value.size();) {
            auto const comma = value.find(',', item_pos);
            auto const item = comma == std::string_view::npos ? value.substr(item_pos)
                                                              : value.substr(item_pos, comma - item_pos);
            if (!detail::length_value_is_valid(item, *rule)) {
                return false;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            item_pos = comma + 1;
        }
    }
    return true;
}

/** Validate a whole parsed fragment before the first mutation of a paste. */
inline bool fragment_lengths_are_valid(Fragment const &fragment, std::string *error = nullptr)
{
    for (auto const &paragraph : fragment.paragraphs) {
        if (!style_lengths_are_valid(paragraph.style)) {
            if (error) {
                *error = "non-finite or malformed paragraph length";
            }
            return false;
        }
        for (auto const &run : paragraph.runs) {
            if (!style_lengths_are_valid(run.style)) {
                if (error) {
                    *error = "non-finite or malformed run length";
                }
                return false;
            }
        }
    }
    return true;
}

namespace detail {

inline char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string to_ascii_lower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out += ascii_lower(c);
    }
    return out;
}

inline std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

inline bool is_character_property(std::string_view name)
{
    static constexpr std::string_view allowed[] = {
        "font-family", "font-style", "font-variant", "font-variant-alternates", "font-variant-caps",
        "font-variant-east-asian", "font-variant-ligatures", "font-variant-numeric", "font-variant-position",
        "font-weight", "font-stretch", "font-size", "font-size-adjust", "font-feature-settings",
        "font-variation-settings", "font-kerning", "kerning", "letter-spacing", "word-spacing",
        "text-decoration", "text-decoration-line", "text-decoration-style", "text-decoration-color",
        "text-transform", "baseline-shift", "dominant-baseline", "alignment-baseline", "fill", "fill-opacity",
        "fill-rule", "stroke", "stroke-opacity", "stroke-width", "stroke-linecap", "stroke-linejoin",
        "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "paint-order", "color", "opacity",
        "text-anchor",
    };
    for (auto candidate : allowed) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

inline bool is_paragraph_property(std::string_view name)
{
    static constexpr std::string_view allowed[] = {
        "text-align", "text-align-last", "text-indent", "line-height", "white-space",
        "writing-mode", "direction", "unicode-bidi", "text-anchor",
    };
    for (auto candidate : allowed) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

/** Value must be a plain CSS term list: no URLs, expressions, markup or controls. */
inline bool is_safe_style_value(std::string_view value)
{
    if (value.empty() || value.size() > MAX_STYLE_VALUE_LENGTH) {
        return false;
    }
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
        switch (static_cast<char>(c)) {
            case '<':
            case '>':
            case '\\':
            case '{':
            case '}':
            case '@':
            case '!':
            case ';':
            case '&':
                return false;
            default:
                break;
        }
    }
    auto const lower = to_ascii_lower(value);
    static constexpr std::string_view forbidden[] = {"url(", "javascript", "expression(", "&#", "<!--"};
    for (auto needle : forbidden) {
        if (lower.find(needle) != std::string::npos) {
            return false;
        }
    }
    return true;
}

} // namespace detail

using detail::is_safe_style_value;
using detail::to_ascii_lower;

inline bool is_allowed_style_property(std::string_view name, bool paragraph_scope)
{
    auto const lower = to_ascii_lower(name);
    if (paragraph_scope) {
        return detail::is_paragraph_property(lower);
    }
    return detail::is_character_property(lower);
}

/**
 * Return a canonical, allow-listed "name:value;" declaration list.
 * Unknown properties, unsafe values, duplicates and oversized results are dropped.
 */
inline std::string sanitize_style(std::string_view css, bool paragraph_scope)
{
    std::string out;
    std::size_t pos = 0;
    while (pos <= css.size()) {
        auto const semi = css.find(';', pos);
        auto decl = semi == std::string_view::npos ? css.substr(pos) : css.substr(pos, semi - pos);
        pos = semi == std::string_view::npos ? css.size() + 1 : semi + 1;

        decl = detail::trim(decl);
        if (decl.empty()) {
            continue;
        }
        auto const colon = decl.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        auto const name = detail::trim(decl.substr(0, colon));
        auto const value = detail::trim(decl.substr(colon + 1));
        if (name.empty() || !is_allowed_style_property(name, paragraph_scope) || !detail::is_safe_style_value(value)) {
            continue;
        }
        auto const canonical = to_ascii_lower(name) + ":";
        if (out.compare(0, canonical.size(), canonical) == 0 ||
            out.find(";" + canonical) != std::string::npos) {
            continue; // first declaration wins
        }
        out += canonical;
        out.append(value.data(), value.size());
        out += ';';
        if (out.size() > MAX_STYLE_LENGTH) {
            return {};
        }
    }
    return out;
}

namespace detail {

inline void append_escaped(std::string &out, std::string_view s)
{
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
}

inline bool unescape_field(std::string_view in, std::string &out)
{
    for (std::size_t i = 0; i < in.size(); ++i) {
        char const c = in[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= in.size()) {
            return false;
        }
        switch (in[i]) {
            case '\\': out += '\\'; break;
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            default: return false;
        }
    }
    return true;
}

/** Run text may hold tabs and printable Unicode, never line breaks or controls. */
inline bool is_valid_run_text(std::string_view text)
{
    for (unsigned char c : text) {
        if (c == 0 || (c < 0x20 && c != '\t') || c == 0x7f) {
            return false;
        }
    }
    return g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr);
}

inline std::string plain_from(Fragment const &fragment)
{
    std::string plain;
    for (std::size_t i = 0; i < fragment.paragraphs.size(); ++i) {
        if (i != 0) {
            plain += '\n';
        }
        for (auto const &run : fragment.paragraphs[i].runs) {
            plain += run.text;
        }
    }
    return plain;
}

/**
 * True when one CSS term carries a non-finite number, including a number that
 * hides behind a unit or percent suffix.
 *
 * A whole-term parse alone misses `1e999%`, `1e999px`, `nan%` and numbers inside
 * a function such as `calc(1e999px)`: `g_ascii_strtod` parses those prefixes and
 * reports infinity, but the whole term never parses as a complete number. Every
 * term start and every position after `(` or `,` is therefore parsed as a numeric
 * prefix; a non-finite prefix rejects the declaration regardless of its suffix.
 * A term whose first character cannot start a number (a family name, a keyword)
 * never matches because `g_ascii_strtod` consumes nothing.
 */
inline bool term_has_nonfinite_number(std::string_view term)
{
    auto const prefix_is_nonfinite = [&term] (std::size_t at) {
        if (at >= term.size()) {
            return false;
        }
        std::string rest(term.substr(at));
        char *end = nullptr;
        double const number = g_ascii_strtod(rest.c_str(), &end);
        return end != rest.c_str() && !std::isfinite(number);
    };
    if (prefix_is_nonfinite(0)) {
        return true;
    }
    for (std::size_t i = 0; i < term.size(); ++i) {
        if (term[i] != '(' && term[i] != ',') {
            continue;
        }
        std::size_t j = i + 1;
        while (j < term.size() && g_ascii_isspace(term[j])) {
            ++j;
        }
        if (prefix_is_nonfinite(j)) {
            return true;
        }
    }
    return false;
}

/**
 * True when every numeric term of a style declaration list is finite.
 *
 * The length-property validator only covers the ten absolute/relative length
 * properties. External decoders may emit other numeric allow-listed properties
 * (opacity, font-weight, stroke-miterlimit, font-stretch, ...); a non-finite,
 * NaN or overflowing numeric value there would reach the SVG style unchanged.
 * Unit- and percent-bearing spellings (`font-stretch:1e999%`,
 * `font-size:1e999px`) are checked by numeric prefix, so a suffix cannot hide an
 * infinite magnitude; family names and keywords without a numeric prefix are
 * unaffected.
 */
inline bool style_numbers_are_finite(std::string_view css)
{
    std::size_t pos = 0;
    while (pos <= css.size()) {
        auto const semi = css.find(';', pos);
        auto const decl = css.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = semi == std::string_view::npos ? css.size() + 1 : semi + 1;
        auto const colon = decl.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        auto const value = decl.substr(colon + 1);
        std::size_t item = 0;
        while (item < value.size()) {
            while (item < value.size() && (g_ascii_isspace(value[item]) || value[item] == ',')) {
                ++item;
            }
            auto const begin = item;
            while (item < value.size() && !g_ascii_isspace(value[item]) && value[item] != ',') {
                ++item;
            }
            if (item == begin) {
                continue;
            }
            if (term_has_nonfinite_number(value.substr(begin, item - begin))) {
                return false;
            }
        }
    }
    return true;
}

} // namespace detail

/** Outcome of the single validated fragment-construction path below. */
enum class BuildStatus {
    ok,       ///< every bound, encoding, style and numeric check passed
    rejected, ///< a bound or validation failed; no fragment is returned
};

struct BuildResult {
    BuildStatus status = BuildStatus::rejected;
    Fragment fragment;
    std::string error;          ///< machine-facing reason (never shown to the user)
    std::size_t text_bytes = 0; ///< decoded UTF-8 run bytes actually accepted
    std::size_t split_runs = 0; ///< runs split at a UTF-8 boundary for MAX_RUN_LENGTH
};

/**
 * The single validated construction path for fragments produced outside this
 * header (external HTML/RTF decoders and their tests).
 *
 * Clipboard input is never trusted memory, and the insertion sites consume
 * Run::text as a NUL-terminated C string: a decoder that filled Fragment fields
 * directly could bypass the style allow-list, the run-length bound or the
 * UTF-8/NUL/control rules with no compiler or test signal. Every decoder must
 * therefore hand its paragraphs to this function and use only the returned
 * fragment. It enforces, in order:
 *
 *   - paragraph count (MAX_PARAGRAPHS) and run count (MAX_RUNS);
 *   - run text must be UTF-8 with no NUL, no control character except TAB and no
 *     line break (a line break is a paragraph boundary by contract);
 *   - runs longer than MAX_RUN_LENGTH are split at a UTF-8 boundary into further
 *     runs, charged against MAX_RUNS (never a partial or cut sequence);
 *   - total decoded text bytes (MAX_CHARS);
 *   - sanitize_style() for run and paragraph scope, so only allow-listed,
 *     length- and value-bounded declarations survive;
 *   - style_lengths_are_valid() and style_numbers_are_finite(), so a non-finite
 *     or out-of-scale numeric value never reaches the SVG style;
 *   - the authoritative plain text (plain_from()).
 *
 * A rejection returns no fragment: the caller must use the complete plain
 * alternative (or report an explanatory no-op), never a partial rich insert.
 * `status == ok` with an empty fragment means "readable but no usable text".
 */
inline BuildResult build_fragment(std::vector<Paragraph> paragraphs)
{
    BuildResult result;

    if (paragraphs.size() > MAX_PARAGRAPHS) {
        result.error = "too many paragraphs";
        return result;
    }

    Fragment fragment;
    std::size_t runs = 0;
    std::size_t text_bytes = 0;
    std::size_t split_runs = 0;

    for (auto &paragraph : paragraphs) {
        Paragraph out;
        out.style = sanitize_style(paragraph.style, true);
        if (!style_lengths_are_valid(out.style) || !detail::style_numbers_are_finite(out.style)) {
            result.error = "invalid paragraph style";
            return result;
        }
        for (auto &run : paragraph.runs) {
            std::string text = std::move(run.text);
            if (text.find('\n') != std::string::npos || text.find('\r') != std::string::npos) {
                result.error = "run text contains a line break";
                return result;
            }
            if (!detail::is_valid_run_text(text)) {
                result.error = "invalid run text"; // NUL, control character or invalid UTF-8
                return result;
            }
            std::string const style = sanitize_style(run.style, false);
            if (!style_lengths_are_valid(style) || !detail::style_numbers_are_finite(style)) {
                result.error = "invalid run style";
                return result;
            }
            if (text.empty()) {
                continue; // empty runs are never emitted
            }
            std::size_t begin = 0;
            while (begin < text.size()) {
                std::size_t end = std::min(text.size(), begin + MAX_RUN_LENGTH);
                if (end < text.size()) {
                    while (end > begin && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                        --end;
                    }
                    if (end == begin) {
                        result.error = "run split is not on a UTF-8 boundary";
                        return result;
                    }
                }
                if (runs >= MAX_RUNS) {
                    result.error = "too many runs";
                    return result;
                }
                text_bytes += end - begin;
                if (text_bytes > MAX_CHARS) {
                    result.error = "too many characters";
                    return result;
                }
                if (end < text.size()) {
                    ++split_runs;
                }
                Run out_run;
                out_run.style = style;
                out_run.text = text.substr(begin, end - begin);
                out.runs.push_back(std::move(out_run));
                ++runs;
                begin = end;
            }
        }
        fragment.paragraphs.push_back(std::move(out));
    }

    fragment.plain = detail::plain_from(fragment);
    result.fragment = std::move(fragment);
    result.text_bytes = text_bytes;
    result.split_runs = split_runs;
    result.status = BuildStatus::ok;
    return result;
}

/**
 * True when a fragment carries formatting worth asking the user about.
 *
 * The external formatting question must not appear for plain or effectively
 * unstyled imports (plain content packaged as HTML, or an RTF whose only
 * declarations are the uniform default family and size that every real writer
 * emits). Such a fragment is not "meaningful": no prompt. Any paragraph
 * property, any other character property, or a non-uniform family/size is
 * meaningful. The decoder never synthesizes a default, so this is exact.
 */
inline bool has_meaningful_styles(Fragment const &fragment)
{
    bool have_family = false;
    bool have_size = false;
    std::string family;
    std::string size;

    for (auto const &paragraph : fragment.paragraphs) {
        if (!paragraph.style.empty()) {
            return true;
        }
        for (auto const &run : paragraph.runs) {
            std::size_t pos = 0;
            while (pos <= run.style.size()) {
                auto const semi = run.style.find(';', pos);
                auto const decl = run.style.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
                pos = semi == std::string::npos ? run.style.size() + 1 : semi + 1;
                if (decl.empty()) {
                    continue;
                }
                auto const colon = decl.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                auto const name = decl.substr(0, colon);
                auto const value = decl.substr(colon + 1);
                if (name == "font-family") {
                    if (!have_family) {
                        have_family = true;
                        family = value;
                    } else if (family != value) {
                        return true;
                    }
                } else if (name == "font-size") {
                    if (!have_size) {
                        have_size = true;
                        size = value;
                    } else if (size != value) {
                        return true;
                    }
                } else {
                    return true;
                }
            }
        }
    }
    return false;
}

inline std::string serialize(Fragment const &fragment)
{
    std::string out = "vac-text-fragment\t";
    out += std::to_string(FORMAT_VERSION);
    out += '\n';
    for (auto const &paragraph : fragment.paragraphs) {
        out += "P\t";
        detail::append_escaped(out, sanitize_style(paragraph.style, true));
        out += '\n';
        for (auto const &run : paragraph.runs) {
            out += "R\t";
            detail::append_escaped(out, sanitize_style(run.style, false));
            out += '\t';
            detail::append_escaped(out, run.text);
            out += '\n';
        }
    }
    return out;
}

/**
 * Parse and validate a native rich payload. Returns nullopt on any structural,
 * version, bound or encoding violation; the caller then uses plain fallback.
 */
inline std::optional<Fragment> parse(std::string_view payload, std::string *error = nullptr)
{
    auto fail = [&] (char const *reason) -> std::optional<Fragment> {
        if (error) {
            *error = reason;
        }
        return std::nullopt;
    };

    if (payload.empty() || payload.size() > MAX_PAYLOAD_BYTES) {
        return fail("payload size out of bounds");
    }
    if (!g_utf8_validate(payload.data(), static_cast<gssize>(payload.size()), nullptr)) {
        return fail("payload is not valid UTF-8");
    }

    Fragment fragment;
    std::size_t runs = 0;
    std::size_t chars = 0;
    bool header_seen = false;
    std::size_t pos = 0;
    while (pos <= payload.size()) {
        auto const nl = payload.find('\n', pos);
        auto line = nl == std::string_view::npos ? payload.substr(pos) : payload.substr(pos, nl - pos);
        pos = nl == std::string_view::npos ? payload.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() && pos > payload.size()) {
            break;
        }
        if (!header_seen) {
            if (line == "vac-text-fragment\t" + std::to_string(FORMAT_VERSION)) {
                header_seen = true;
                continue;
            }
            if (line == "vac-text-fragment\t" + std::to_string(LEGACY_LOCAL_LENGTH_VERSION)) {
                // Version 1 stored raw local computed lengths without the source
                // coordinate context; guessing a scale would silently change the
                // visible size. Reject the rich payload so the complete plain-text
                // fallback is used instead.
                return fail("unsupported legacy version 1 fragment");
            }
            return fail("unsupported fragment version");
        }
        if (line.empty()) {
            continue;
        }
        auto const tab1 = line.find('\t');
        if (tab1 == std::string_view::npos) {
            return fail("malformed record");
        }
        auto const kind = line.substr(0, tab1);
        if (kind == "P") {
            if (fragment.paragraphs.size() >= MAX_PARAGRAPHS) {
                return fail("too many paragraphs");
            }
            std::string style;
            if (!detail::unescape_field(line.substr(tab1 + 1), style)) {
                return fail("bad escape in paragraph style");
            }
            Paragraph paragraph;
            paragraph.style = sanitize_style(style, true);
            // The native payload is untrusted clipboard data too: the same
            // numeric guard as the external construction path, so a fragment
            // carrying `opacity:nan`/`font-size:1e999px` can never reach the SVG
            // style. A rejection falls back to the complete plain alternative.
            if (!style_lengths_are_valid(paragraph.style) ||
                !detail::style_numbers_are_finite(paragraph.style)) {
                return fail("invalid paragraph style");
            }
            fragment.paragraphs.push_back(std::move(paragraph));
        } else if (kind == "R") {
            if (fragment.paragraphs.empty()) {
                return fail("run before paragraph");
            }
            if (runs >= MAX_RUNS) {
                return fail("too many runs");
            }
            auto const tab2 = line.find('\t', tab1 + 1);
            if (tab2 == std::string_view::npos) {
                return fail("malformed run record");
            }
            std::string style;
            std::string text;
            if (!detail::unescape_field(line.substr(tab1 + 1, tab2 - tab1 - 1), style) ||
                !detail::unescape_field(line.substr(tab2 + 1), text)) {
                return fail("bad escape in run record");
            }
            if (text.size() > MAX_RUN_LENGTH) {
                return fail("run too long");
            }
            if (text.find('\n') != std::string::npos || !detail::is_valid_run_text(text)) {
                return fail("invalid run text");
            }
            chars += text.size();
            if (chars > MAX_CHARS) {
                return fail("too many characters");
            }
            Run run;
            run.style = sanitize_style(style, false);
            if (!style_lengths_are_valid(run.style) || !detail::style_numbers_are_finite(run.style)) {
                return fail("invalid run style");
            }
            run.text = std::move(text);
            fragment.paragraphs.back().runs.push_back(std::move(run));
            ++runs;
        } else {
            return fail("unknown record type");
        }
    }
    if (!header_seen) {
        return fail("missing header");
    }
    fragment.plain = detail::plain_from(fragment);
    return fragment;
}

/**
 * Structured outcome of the plain-text conversion.
 *
 * The caller must classify by this status, never by re-validating the bytes it
 * already handed over: `invalid_text` means "nothing usable here, other paste
 * targets may still apply", `over_limit` means "the whole paste must abort".
 * Re-deriving the class from the payload (for example re-running
 * g_utf8_validate) silently changes the abort/fallback decision whenever an
 * explicit size check is reordered or removed.
 */
enum class PlainTextStatus {
    ok,           ///< fragment returned (may still be empty for empty input)
    invalid_text, ///< unusable content: invalid UTF-8; nothing may be inserted
    over_limit,   ///< byte/run/paragraph budget exceeded; abort the whole paste
};

struct PlainTextResult {
    PlainTextStatus status = PlainTextStatus::invalid_text;
    Fragment fragment;
    std::string error; ///< machine-facing reason (never shown to the user)
};

/**
 * Plain-text-only fragment with a structured rejection reason (external
 * clipboard, no native rich payload).
 *
 * The bounds are applied to the payload as a whole: an input exceeding
 * MAX_CHARS bytes, MAX_PARAGRAPHS paragraphs or MAX_RUNS runs yields
 * @c over_limit (and @a error when provided) instead of being truncated or
 * partly imported. A valid prefix is never silently pasted and a UTF-8 sequence
 * is never cut; the caller reports the over-limit condition to the user.
 */
inline PlainTextResult from_plain_text_ex(std::string_view utf8)
{
    auto fail = [] (PlainTextStatus status, char const *reason) {
        PlainTextResult result;
        result.status = status;
        result.error = reason;
        return result;
    };

    std::string text(utf8);
    if (text.size() > MAX_CHARS) {
        return fail(PlainTextStatus::over_limit, "plain text exceeds the byte budget");
    }
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
        return fail(PlainTextStatus::invalid_text, "plain text is not valid UTF-8");
    }
    Fragment fragment;
    Paragraph paragraph;
    std::size_t runs = 0;
    std::size_t begin = 0;
    for (;;) {
        auto const nl = text.find('\n', begin);
        auto const end = nl == std::string::npos ? text.size() : nl;
        if (end > begin) {
            if (runs >= MAX_RUNS) {
                return fail(PlainTextStatus::over_limit, "plain text exceeds the run budget");
            }
            Run run;
            run.text = text.substr(begin, end - begin);
            if (!detail::is_valid_run_text(run.text)) {
                std::string cleaned;
                for (unsigned char c : run.text) {
                    if (c == '\t' || (c >= 0x20 && c != 0x7f)) {
                        cleaned += static_cast<char>(c);
                    }
                }
                run.text = std::move(cleaned);
            }
            paragraph.runs.push_back(std::move(run));
            ++runs;
        }
        if (nl == std::string::npos) {
            break;
        }
        // Bounds apply to the plain fallback too: a payload made only of line
        // breaks must not allocate an unbounded number of paragraphs. Reject
        // as a whole rather than dropping the remaining paragraphs.
        if (fragment.paragraphs.size() + 2 > MAX_PARAGRAPHS) {
            return fail(PlainTextStatus::over_limit, "plain text exceeds the paragraph budget");
        }
        fragment.paragraphs.push_back(std::move(paragraph));
        paragraph = Paragraph{};
        begin = nl + 1;
    }
    fragment.paragraphs.push_back(std::move(paragraph));
    fragment.plain = detail::plain_from(fragment);
    PlainTextResult result;
    result.status = PlainTextStatus::ok;
    result.fragment = std::move(fragment);
    return result;
}

/** Backwards-compatible wrapper: empty fragment means rejection (@a error set). */
inline Fragment from_plain_text(std::string_view utf8, std::string *error = nullptr)
{
    auto result = from_plain_text_ex(utf8);
    if (error) {
        *error = result.error;
    }
    return std::move(result.fragment);
}

} // namespace Inkscape::UI::TextPaste

#endif // INKSCAPE_UI_TEXT_PASTE_H

// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
