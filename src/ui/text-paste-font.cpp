// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Paste-scoped missing-font diagnosis: pure policy plus the FontFactory oracle.
 *
 * Policy (never touches the font system, deterministic with any Availability):
 *
 *  - Only runs that actually request a family are inspected; paragraph styles
 *    cannot carry font-family in the fragment allow-list, and a run with no
 *    font-family declaration is skipped.
 *  - A CSS font-family list is resolved by the first installed *concrete* token
 *    (REPORT.md 2.1.3). Generic CSS families and the Pango aliases are exempt
 *    (is_generic_family). Missing concrete tokens before the first installed
 *    token are reported (the copied webfont `Inter, sans-serif` case); tokens
 *    after it are inert. When no concrete token is installed, only the first
 *    concrete token is the reported requested identity.
 *  - Dedupe is by requested family (casefolded): one MissingFamily issue per
 *    distinct requested family, one MissingFace/UnavailableVariation issue per
 *    distinct (family, declared face request), one MissingGlyphs issue per
 *    affected run. Distinct concrete families are capped at MAX_FAMILIES; when a
 *    family cannot be looked up because of the cap, `truncated` is set and no
 *    claim is made about it.
 *  - Never a per-run font load: every lookup goes through the injected oracle,
 *    and `inspect(fragment, cap)` creates one oracle per paste whose per-request
 *    memo bounds resolution work to distinct requests.
 *
 * `Availability::run_style` is the run's canonical "name:value;" CSS list; the
 * real oracle parses the face-relevant declarations (font-weight, font-style,
 * font-stretch, font-variation-settings) out of it. `glyphs_available` receives
 * the run text with the ignorable code points already removed by the policy; it
 * is a predicate over the whole text, so an unavailable result proves "at least
 * one non-ignorable code point has no glyph" and the policy records the proven
 * lower bound 1 for that run.
 *
 * The real oracle uses only FontFactory/FontInstance plus
 * pango_font_describe()/pango_font_face_is_synthesized() on the resolved face.
 * It adds no cache of its own (the memo lives for one inspect() call) and does
 * not change FontFactory or global font resolution. `Face` is always handed a
 * private description copy because it mutates the description and silently
 * falls back to sans-serif.
 */

#include "ui/text-paste-font.h"

#include <cstddef>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>
#include <glibmm/i18n.h>
#include <glibmm/ustring.h>
#include <pango/pango-font.h>
#include <pango/pango-types.h>

#include "libnrtype/OpenTypeUtil.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#include "ui/text-paste.h"

namespace Inkscape::UI::TextPaste::FontCheck {

namespace {

// ---------------------------------------------------------------------------
// Small shared helpers (pure: no font system use)
// ---------------------------------------------------------------------------

std::string casefold(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
        return std::string(text);
    }
    char *folded = g_utf8_casefold(text.data(), static_cast<gssize>(text.size()));
    std::string out = folded ? std::string(folded) : std::string(text);
    g_free(folded);
    return out;
}

std::string_view trim(std::string_view text)
{
    auto const space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/** Value of the first canonical "name:value;" declaration called @a name. */
std::string_view declaration_value(std::string_view style, std::string_view name)
{
    std::size_t pos = 0;
    while (pos < style.size()) {
        auto const semi = style.find(';', pos);
        auto const decl = style.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = semi == std::string_view::npos ? style.size() : semi + 1;
        auto const colon = decl.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        if (to_ascii_lower(trim(decl.substr(0, colon))) == std::string(name)) {
            return trim(decl.substr(colon + 1));
        }
    }
    return {};
}

/** The same canonical style list without a declaration called @a name. */
std::string strip_declaration(std::string_view style, std::string_view name)
{
    std::string out;
    std::size_t pos = 0;
    while (pos < style.size()) {
        auto const semi = style.find(';', pos);
        auto const decl = style.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = semi == std::string_view::npos ? style.size() : semi + 1;
        if (decl.empty()) {
            continue;
        }
        auto const colon = decl.find(':');
        if (colon != std::string_view::npos && to_ascii_lower(trim(decl.substr(0, colon))) == std::string(name)) {
            continue;
        }
        out.append(decl.data(), decl.size());
        out += ';';
    }
    return out;
}

/** Code points that must not raise a missing-glyph finding (REPORT.md 2.1.5). */
bool ignorable_code_point(gunichar code_point)
{
    if (code_point > 0x10FFFF) {
        return true;
    }
    if (code_point >= 0x200B && code_point <= 0x200D) { // ZWSP, ZWNJ, ZWJ
        return true;
    }
    if (code_point == 0x2060 || code_point == 0xFEFF) { // word joiner, BOM
        return true;
    }
    if (code_point >= 0xFE00 && code_point <= 0xFE0F) { // variation selectors
        return true;
    }
    if (code_point >= 0xE0100 && code_point <= 0xE01EF) { // variation selectors supplement
        return true;
    }
    switch (g_unichar_type(code_point)) {
        case G_UNICODE_CONTROL:
        case G_UNICODE_FORMAT:
        case G_UNICODE_UNASSIGNED:
            return true;
        default:
            return false;
    }
}

/** Run text without its ignorable code points; invalid UTF-8 yields an empty result. */
std::string filter_ignorable(std::string_view text)
{
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
        return {};
    }
    std::string out;
    out.reserve(text.size());
    char const *cursor = text.data();
    char const *const end = cursor + text.size();
    while (cursor < end) {
        char const *const start = cursor;
        gunichar const code_point = g_utf8_get_char_validated(cursor, end - cursor);
        cursor = g_utf8_next_char(cursor);
        if (code_point == static_cast<gunichar>(-1) || code_point == static_cast<gunichar>(-2)) {
            return {};
        }
        if (ignorable_code_point(code_point)) {
            continue;
        }
        out.append(start, cursor - start);
    }
    return out;
}

/** Requested family plus the declared face keyword(s), for Issue::requested. */
std::string describe_face_request(std::string const &family, std::string_view style)
{
    static constexpr std::string_view names[] = {
        "font-weight", "font-style", "font-stretch", "font-variation-settings",
    };
    std::string out = family;
    for (auto const name : names) {
        auto const value = declaration_value(style, name);
        if (value.empty() || casefold(value) == "normal") {
            continue;
        }
        out += ' ';
        out.append(value.data(), value.size());
    }
    return out;
}

bool style_has_variation_request(std::string_view style)
{
    auto const value = declaration_value(style, "font-variation-settings");
    return !value.empty() && casefold(value) != "normal";
}

// ---------------------------------------------------------------------------
// Real availability oracle
// ---------------------------------------------------------------------------

struct FaceRequest
{
    bool has_weight = false;
    PangoWeight weight = PANGO_WEIGHT_NORMAL;
    bool has_style = false;
    PangoStyle style = PANGO_STYLE_NORMAL;
    bool has_stretch = false;
    PangoStretch stretch = PANGO_STRETCH_NORMAL;
    bool has_variations = false;
    std::string variations;                           ///< Pango syntax, "wght=700,wdth=75"
    std::vector<std::pair<std::string, double>> axes; ///< requested axis tags and values
    std::string key = "-";                            ///< memo key for this face request
};

bool parse_css_weight(std::string_view value, PangoWeight &out)
{
    auto const lower = casefold(value);
    if (lower == "normal") {
        out = PANGO_WEIGHT_NORMAL;
        return true;
    }
    if (lower == "bold") {
        out = PANGO_WEIGHT_BOLD;
        return true;
    }
    if (lower == "bolder" || lower == "lighter") {
        return false; // relative to an inherited weight: not resolvable from the fragment
    }
    std::string const token(lower);
    char *end = nullptr;
    long const number = std::strtol(token.c_str(), &end, 10);
    if (!end || end == token.c_str() || *end != '\0' || number < 1 || number > 1000) {
        return false;
    }
    out = static_cast<PangoWeight>(number);
    return true;
}

bool parse_css_style(std::string_view value, PangoStyle &out)
{
    auto const lower = casefold(value);
    if (lower == "normal") {
        out = PANGO_STYLE_NORMAL;
        return true;
    }
    if (lower == "italic") {
        out = PANGO_STYLE_ITALIC;
        return true;
    }
    if (lower.compare(0, 7, "oblique") == 0) { // "oblique" or "oblique <angle>"
        out = PANGO_STYLE_OBLIQUE;
        return true;
    }
    return false;
}

bool parse_css_stretch(std::string_view value, PangoStretch &out)
{
    auto const lower = casefold(value);
    struct Keyword
    {
        char const *name;
        PangoStretch stretch;
    };
    static constexpr Keyword keywords[] = {
        {"ultra-condensed", PANGO_STRETCH_ULTRA_CONDENSED},
        {"extra-condensed", PANGO_STRETCH_EXTRA_CONDENSED},
        {"semi-condensed", PANGO_STRETCH_SEMI_CONDENSED},
        {"condensed", PANGO_STRETCH_CONDENSED},
        {"normal", PANGO_STRETCH_NORMAL},
        {"semi-expanded", PANGO_STRETCH_SEMI_EXPANDED},
        {"extra-expanded", PANGO_STRETCH_EXTRA_EXPANDED},
        {"ultra-expanded", PANGO_STRETCH_ULTRA_EXPANDED},
        {"expanded", PANGO_STRETCH_EXPANDED},
    };
    for (auto const &keyword : keywords) {
        if (lower == keyword.name) {
            out = keyword.stretch;
            return true;
        }
    }
    if (lower.size() > 1 && lower.back() == '%') { // CSS Fonts 4 percentage form
        std::string const number(lower.substr(0, lower.size() - 1));
        char *end = nullptr;
        double const percent = g_ascii_strtod(number.c_str(), &end);
        if (!end || end == number.c_str() || *end != '\0' || !(percent > 0.0)) {
            return false;
        }
        if (percent <= 56.25) {
            out = PANGO_STRETCH_ULTRA_CONDENSED;
        } else if (percent <= 68.75) {
            out = PANGO_STRETCH_EXTRA_CONDENSED;
        } else if (percent <= 81.25) {
            out = PANGO_STRETCH_CONDENSED;
        } else if (percent <= 93.75) {
            out = PANGO_STRETCH_SEMI_CONDENSED;
        } else if (percent <= 106.25) {
            out = PANGO_STRETCH_NORMAL;
        } else if (percent <= 118.75) {
            out = PANGO_STRETCH_SEMI_EXPANDED;
        } else if (percent <= 137.5) {
            out = PANGO_STRETCH_EXPANDED;
        } else if (percent <= 175.0) {
            out = PANGO_STRETCH_EXTRA_EXPANDED;
        } else {
            out = PANGO_STRETCH_ULTRA_EXPANDED;
        }
        return true;
    }
    return false;
}

/** CSS font-variation-settings value -> axis tags and values; empty when unusable. */
std::vector<std::pair<std::string, double>> parse_variation_settings(std::string_view value)
{
    std::vector<std::pair<std::string, double>> out;
    if (value.empty() || casefold(value) == "normal") {
        return out;
    }
    std::size_t pos = 0;
    while (pos < value.size()) {
        auto const comma = value.find(',', pos);
        auto item = trim(value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
        pos = comma == std::string_view::npos ? value.size() : comma + 1;
        if (item.empty()) {
            return {};
        }
        std::string tag;
        if (item.front() == '\'' || item.front() == '"') {
            char const quote = item.front();
            auto const close = item.find(quote, 1);
            if (close == std::string_view::npos) {
                return {};
            }
            tag = std::string(item.substr(1, close - 1));
            item = trim(item.substr(close + 1));
        } else {
            auto const separator = item.find_first_of(" \t=");
            tag = std::string(item.substr(0, separator));
            item = separator == std::string_view::npos ? std::string_view{} : trim(item.substr(separator));
        }
        if (!item.empty() && item.front() == '=') {
            item = trim(item.substr(1));
        }
        if (tag.size() != 4 || item.empty()) {
            return {};
        }
        std::string const number(item);
        char *end = nullptr;
        double const axis_value = g_ascii_strtod(number.c_str(), &end);
        if (!end || end == number.c_str() || *end != '\0') {
            return {};
        }
        out.emplace_back(tag, axis_value);
    }
    return out;
}

std::string pango_variations_string(std::vector<std::pair<std::string, double>> const &axes)
{
    std::string out;
    for (auto const &axis : axes) {
        if (!out.empty()) {
            out += ',';
        }
        char buffer[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(buffer, sizeof buffer, "%.10g", axis.second);
        out += axis.first;
        out += '=';
        out += buffer;
    }
    return out;
}

FaceRequest parse_face_request(std::string_view style)
{
    FaceRequest request;

    auto const weight = declaration_value(style, "font-weight");
    if (!weight.empty()) {
        PangoWeight parsed = PANGO_WEIGHT_NORMAL;
        if (parse_css_weight(weight, parsed)) {
            request.has_weight = true;
            request.weight = parsed;
            request.key += "w";
            request.key += std::to_string(static_cast<int>(parsed));
        }
    }
    auto const font_style = declaration_value(style, "font-style");
    if (!font_style.empty()) {
        PangoStyle parsed = PANGO_STYLE_NORMAL;
        if (parse_css_style(font_style, parsed)) {
            request.has_style = true;
            request.style = parsed;
            request.key += "s";
            request.key += std::to_string(static_cast<int>(parsed));
        }
    }
    auto const stretch = declaration_value(style, "font-stretch");
    if (!stretch.empty()) {
        PangoStretch parsed = PANGO_STRETCH_NORMAL;
        if (parse_css_stretch(stretch, parsed)) {
            request.has_stretch = true;
            request.stretch = parsed;
            request.key += "t";
            request.key += std::to_string(static_cast<int>(parsed));
        }
    }
    auto const variations = declaration_value(style, "font-variation-settings");
    if (!variations.empty()) {
        auto const axes = parse_variation_settings(variations);
        if (!axes.empty()) {
            request.has_variations = true;
            request.axes = axes;
            request.variations = pango_variations_string(axes);
            request.key += "v";
            request.key += request.variations;
        }
    }
    return request;
}

bool variations_available(FaceRequest const &request, std::vector<OTVarAxis> const &axes)
{
    for (auto const &wanted : request.axes) {
        bool found = false;
        for (auto const &axis : axes) {
            if (axis.tag != wanted.first) {
                continue;
            }
            found = true;
            if (wanted.second < axis.minimum - 1e-6 || wanted.second > axis.maximum + 1e-6) {
                return false;
            }
            break;
        }
        if (!found) {
            return false; // the resolved face has no such axis
        }
    }
    return true;
}

class FontFactoryAvailability final : public Availability
{
public:
    bool family_installed(std::string const &family) const override
    {
        return installed_families().count(casefold(family)) != 0;
    }

    bool face_available(std::string const &family, std::string const &run_style) const override
    {
        Resolved const &resolved = resolve(family, run_style);
        if (!resolved.loaded) {
            return false;
        }
        // Pango's resolved family is the authority: a family that is enumerated
        // but best-matches another family is still a substitution.
        if (casefold(resolved.resolved_family) != casefold(family)) {
            return false;
        }
        if (resolved.request.has_weight && resolved.resolved_weight != resolved.request.weight) {
            return false;
        }
        if (resolved.request.has_style && resolved.resolved_style != resolved.request.style) {
            return false;
        }
        if (resolved.request.has_stretch && resolved.resolved_stretch != resolved.request.stretch) {
            return false;
        }
        // A faux face is not an installed exact face; only a declared bold/italic
        // request can be substituted this way.
        if (resolved.synthesized && (resolved.request.has_weight || resolved.request.has_style)) {
            return false;
        }
        if (resolved.request.has_variations && !resolved.variations_satisfied) {
            return false;
        }
        return true;
    }

    std::string substitute_for(std::string const &family) const override
    {
        return resolve(family, {}).resolved_family;
    }

    bool glyphs_available(std::string const &family, std::string const &run_style,
                          std::string_view text) const override
    {
        Resolved const &resolved = resolve(family, run_style);
        if (!resolved.loaded || !resolved.instance) {
            return false; // not proven: no resolved face to prove anything about
        }
        char const *cursor = text.data();
        char const *const end = cursor + text.size();
        while (cursor < end) {
            gunichar const code_point = g_utf8_get_char_validated(cursor, end - cursor);
            if (code_point == static_cast<gunichar>(-1) || code_point == static_cast<gunichar>(-2)) {
                return false; // unusable text: coverage is not claimed
            }
            cursor = g_utf8_next_char(cursor);
            if (ignorable_code_point(code_point)) {
                continue;
            }
            if (resolved.instance->MapUnicodeChar(code_point) == 0) {
                return false;
            }
        }
        return true;
    }

private:
    struct Resolved
    {
        std::shared_ptr<FontInstance> instance; ///< keeps the resolved face alive for this paste
        bool loaded = false;
        std::string resolved_family;
        PangoWeight resolved_weight = PANGO_WEIGHT_NORMAL;
        PangoStyle resolved_style = PANGO_STYLE_NORMAL;
        PangoStretch resolved_stretch = PANGO_STRETCH_NORMAL;
        bool synthesized = false;
        bool variations_satisfied = true;
        FaceRequest request;
    };

    std::set<std::string> const &installed_families() const
    {
        if (!installed_loaded_) {
            installed_loaded_ = true; // one enumeration per paste, never a per-run lookup
            try {
                for (auto const &entry : FontFactory::get().GetUIFamilies()) {
                    installed_.insert(casefold(entry.first));
                }
            } catch (...) {
                // An unusable font configuration leaves the set empty; resolution
                // below still reports the family Pango actually falls back to.
            }
        }
        return installed_;
    }

    Resolved const &resolve(std::string const &family, std::string_view style) const
    {
        FaceRequest request = parse_face_request(style);
        std::string key = casefold(family);
        key += '\x1f';
        key += request.key;
        if (auto const it = memo_.find(key); it != memo_.end()) {
            return it->second;
        }

        Resolved resolved;
        resolved.request = request;

        PangoFontDescription *descr = pango_font_description_new();
        pango_font_description_set_family(descr, family.c_str());
        if (request.has_weight) {
            pango_font_description_set_weight(descr, request.weight);
        }
        if (request.has_style) {
            pango_font_description_set_style(descr, request.style);
        }
        if (request.has_stretch) {
            pango_font_description_set_stretch(descr, request.stretch);
        }
        if (request.has_variations) {
            pango_font_description_set_variations(descr, request.variations.c_str());
        }

        std::shared_ptr<FontInstance> instance;
        try {
            // Face() mutates the description it is handed (size and, on load
            // failure, the family) and silently falls back to sans-serif, so it
            // receives this private copy.
            instance = FontFactory::get().Face(descr, true);
        } catch (...) {
            instance.reset();
        }
        pango_font_description_free(descr);

        if (instance && instance->get_font()) {
            PangoFontDescription *described = pango_font_describe(instance->get_font());
            if (described) {
                char const *name = sp_font_description_get_family(described);
                resolved.resolved_family = name ? name : "";
                resolved.resolved_weight = pango_font_description_get_weight(described);
                resolved.resolved_style = pango_font_description_get_style(described);
                resolved.resolved_stretch = pango_font_description_get_stretch(described);
                pango_font_description_free(described);
            }
            if (PangoFontFace *face = pango_font_get_face(instance->get_font())) {
                resolved.synthesized = pango_font_face_is_synthesized(face);
            }
            resolved.instance = std::move(instance);
            resolved.loaded = true;
            resolved.variations_satisfied =
                request.axes.empty() || variations_available(request, resolved.instance->get_opentype_varaxes());
        }

        return memo_.emplace(std::move(key), std::move(resolved)).first->second;
    }

    mutable std::map<std::string, Resolved> memo_;
    mutable std::set<std::string> installed_;
    mutable bool installed_loaded_ = false;
};

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

struct FamilyState
{
    bool over_cap = false;  ///< not looked up because MAX_FAMILIES was reached
    bool installed = false;
    std::string requested;  ///< first-seen spelling of the requested token
    std::string substitute; ///< family Pango resolves for it, may be empty
};

class Inspector
{
public:
    Inspector(Report &report, Availability const &oracle, std::size_t max_families)
        : _report(report)
        , _oracle(oracle)
        , _max_families(max_families)
    {}

    void inspect_run(std::size_t paragraph_index, std::size_t run_index, Run const &run)
    {
        if (run.text.empty()) {
            return;
        }
        auto const family_list = declaration_value(run.style, "font-family");
        if (family_list.empty()) {
            return; // no family requested by this run
        }

        FamilyState const *chosen = nullptr;
        std::string chosen_token;
        std::vector<std::string> missing_prefix;

        for (auto const &token : family_tokens(std::string(family_list))) {
            if (is_generic_family(token)) {
                continue; // an intentional fallback request, never a missing font
            }
            FamilyState const &state = lookup(token);
            if (state.over_cap) {
                continue; // no claim can be made about a family the cap skipped
            }
            if (state.installed) {
                chosen = &state;
                chosen_token = token;
                break;
            }
            missing_prefix.push_back(token);
        }

        if (!chosen) {
            // No concrete token is installed: the first concrete token is the
            // reported requested identity (REPORT.md 2.1.3).
            if (!missing_prefix.empty()) {
                auto const &state = lookup(missing_prefix.front());
                record(IssueKind::MissingFamily, missing_prefix.front(), state.substitute, paragraph_index, run_index, 0);
            }
            return;
        }

        for (auto const &token : missing_prefix) {
            auto const &state = lookup(token);
            record(IssueKind::MissingFamily, token, state.substitute, paragraph_index, run_index, 0);
        }

        if (!chosen->substitute.empty() && casefold(chosen->substitute) != casefold(chosen_token)) {
            // Enumerated as installed but Pango best-matches another family.
            record(IssueKind::MissingFamily, chosen_token, chosen->substitute, paragraph_index, run_index, 0);
            return;
        }

        std::string const resolved_family = chosen->substitute.empty() ? chosen_token : chosen->substitute;
        std::string const base_style = strip_declaration(run.style, "font-variation-settings");
        bool const variation_requested = style_has_variation_request(run.style);

        if (!face_verdict(chosen_token, base_style)) {
            record(IssueKind::MissingFace, describe_face_request(chosen_token, base_style), resolved_family,
                   paragraph_index, run_index, 0);
            return;
        }
        if (variation_requested && !face_verdict(chosen_token, run.style)) {
            record(IssueKind::UnavailableVariation, describe_face_request(chosen_token, run.style), resolved_family,
                   paragraph_index, run_index, 0);
            return;
        }

        if (_glyph_issues >= MAX_FAMILIES) {
            return; // recorded glyph findings are bounded per paste
        }
        std::string const filtered = filter_ignorable(run.text);
        if (filtered.empty()) {
            return; // nothing but ignorable code points
        }
        // The oracle is a predicate over the whole text, so a negative answer
        // proves "at least one non-ignorable code point has no glyph": report the
        // first missing code point's count as the proven lower bound 1.
        if (!_oracle.glyphs_available(chosen_token, run.style, filtered)) {
            if (record(IssueKind::MissingGlyphs, chosen_token, resolved_family, paragraph_index, run_index, 1)) {
                ++_glyph_issues;
            }
        }
    }

private:
    /**
     * Face availability depends only on the requested family and the declared
     * face values, so runs that differ in size or colour share one oracle call.
     */
    bool face_verdict(std::string const &family, std::string const &style)
    {
        std::string key = casefold(describe_face_request(family, style));
        if (auto const it = _face_verdicts.find(key); it != _face_verdicts.end()) {
            return it->second;
        }
        bool const verdict = _oracle.face_available(family, style);
        _face_verdicts.emplace(std::move(key), verdict);
        return verdict;
    }

    FamilyState const &lookup(std::string const &token)
    {
        std::string const key = casefold(token);
        if (auto const it = _families.find(key); it != _families.end()) {
            return it->second;
        }

        FamilyState state;
        state.requested = token;
        if (_families.size() >= _max_families) {
            // The distinct-family cap was reached: nothing is claimed about this
            // family and the report records that the analysis was truncated.
            state.over_cap = true;
            _report.truncated = true;
        } else {
            ++_report.families_checked;
            state.installed = _oracle.family_installed(token);
            state.substitute = _oracle.substitute_for(token);
            if (!state.installed && !state.substitute.empty() && casefold(state.substitute) == key) {
                // The resolved family IS the requested token (casefolded): the
                // family exists even if the oracle did not enumerate it.
                state.installed = true;
            }
        }
        ++_report.distinct_families;
        return _families.emplace(std::move(key), std::move(state)).first->second;
    }

    bool record(IssueKind kind, std::string requested, std::string substitute, std::size_t paragraph_index,
                std::size_t run_index, std::size_t missing_code_points)
    {
        std::string key = std::to_string(static_cast<int>(kind));
        key += '\x1f';
        key += casefold(requested);
        key += '\x1f';
        key += casefold(substitute);
        if (kind == IssueKind::MissingGlyphs) {
            // One glyph finding per affected run; the other kinds are one finding
            // per distinct request (fragment order keeps the first occurrence).
            key += '\x1f';
            key += std::to_string(paragraph_index);
            key += ':';
            key += std::to_string(run_index);
        }
        if (!_recorded.insert(std::move(key)).second) {
            return false;
        }

        Issue issue;
        issue.kind = kind;
        issue.requested = std::move(requested);
        issue.substitute = std::move(substitute);
        issue.paragraph_index = paragraph_index;
        issue.run_index = run_index;
        issue.missing_code_points = missing_code_points;
        _report.issues.push_back(std::move(issue));
        return true;
    }

    Report &_report;
    Availability const &_oracle;
    std::size_t _max_families;
    std::map<std::string, FamilyState> _families;
    std::map<std::string, bool> _face_verdicts;
    std::set<std::string> _recorded;
    std::size_t _glyph_issues = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool is_generic_family(std::string const &family)
{
    // CSS generic families plus the Pango aliases Sans/Serif/Monospace
    // (REPORT.md 2.1.2). Comparison is case-insensitive, so "Serif" and
    // "Monospace" are covered by their lower-case spellings.
    static constexpr std::string_view generic[] = {
        "serif", "sans-serif", "sans", "monospace", "cursive", "fantasy", "system-ui",
        "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded", "math", "emoji", "fangsong",
    };

    auto token = trim(family);
    if (token.size() >= 2 && ((token.front() == '"' && token.back() == '"') ||
                              (token.front() == '\'' && token.back() == '\''))) {
        token = trim(token.substr(1, token.size() - 2));
    }
    if (token.empty()) {
        return false;
    }
    auto const folded = casefold(token);
    for (auto const candidate : generic) {
        if (folded == candidate) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> family_tokens(std::string const &family_list)
{
    std::vector<std::string> tokens;
    std::size_t pos = 0;
    while (pos < family_list.size()) {
        auto const comma = family_list.find(',', pos);
        auto token = trim(std::string_view(family_list)
                              .substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        pos = comma == std::string::npos ? family_list.size() : comma + 1;
        if (token.size() >= 2 && ((token.front() == '"' && token.back() == '"') ||
                                  (token.front() == '\'' && token.back() == '\''))) {
            token = trim(token.substr(1, token.size() - 2));
        }
        if (token.empty()) {
            continue; // never emit an empty token
        }
        tokens.emplace_back(token);
    }
    return tokens;
}

std::string first_concrete_family(std::string const &family_list)
{
    for (auto const &token : family_tokens(family_list)) {
        if (!is_generic_family(token)) {
            return token;
        }
    }
    return {};
}

Report inspect(Fragment const &fragment, Availability const &oracle, std::size_t max_families)
{
    Report report;
    Inspector inspector(report, oracle, max_families);
    for (std::size_t paragraph_index = 0; paragraph_index < fragment.paragraphs.size(); ++paragraph_index) {
        auto const &paragraph = fragment.paragraphs[paragraph_index];
        for (std::size_t run_index = 0; run_index < paragraph.runs.size(); ++run_index) {
            inspector.inspect_run(paragraph_index, run_index, paragraph.runs[run_index]);
        }
    }
    return report;
}

Report inspect(Fragment const &fragment, std::size_t max_families)
{
    // One oracle per paste: its memo bounds font resolution to distinct requests
    // and it is discarded when this call returns. No global cache is touched.
    FontFactoryAvailability oracle;
    return inspect(fragment, oracle, max_families);
}

std::string summarize(Report const &report)
{
    if (report.empty()) {
        return {};
    }

    std::vector<std::pair<std::string, std::string>> pairs;
    std::set<std::string> seen;
    std::size_t other_issues = 0;

    for (auto const &issue : report.issues) {
        std::string key = std::to_string(static_cast<int>(issue.kind));
        key += '\x1f';
        key += casefold(issue.requested);
        key += '\x1f';
        key += casefold(issue.substitute);
        if (!seen.insert(std::move(key)).second) {
            continue;
        }
        if (issue.kind == IssueKind::MissingFamily) {
            pairs.emplace_back(issue.requested, issue.substitute);
        } else {
            ++other_issues;
            if (!issue.substitute.empty() && casefold(issue.substitute) != casefold(issue.requested)) {
                pairs.emplace_back(issue.requested, issue.substitute);
            }
        }
    }

    Glib::ustring text;
    if (!pairs.empty()) {
        Glib::ustring list;
        for (auto const &pair : pairs) {
            if (!list.empty()) {
                list += ", ";
            }
            list += pair.first;
            list += " \xE2\x86\x92 "; // U+2192, requested -> substitute
            list += pair.second.empty() ? Glib::ustring(_("system fallback")) : Glib::ustring(pair.second);
        }
        text = Glib::ustring::compose(
            _("Fonts used by the pasted text are unavailable: %1. Select the pasted text and choose a replacement in "
              "the Text panel."),
            list);
    }
    if (other_issues > 0) {
        if (!text.empty()) {
            text += " ";
        }
        text += _("Some pasted runs use an unavailable face or missing glyphs; check them in the Text panel.");
    }
    return text.raw();
}

} // namespace Inkscape::UI::TextPaste::FontCheck
