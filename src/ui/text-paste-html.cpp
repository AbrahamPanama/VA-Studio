// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Bounded HTML -> TextPaste::Fragment decoder for external clipboard input.
 *
 * Implements the interface frozen in ui/text-paste-html.h exactly as designed in
 * internal evidence notes sections 4-10:
 *
 *   - markup: libxml2 HTML SAX2 push parser (htmlCreatePushParserCtxt /
 *     htmlParseChunk / htmlCtxtUseOptions / xmlStopParser) with exactly the flags
 *     HTML_PARSE_NONET | HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
 *     HTML_PARSE_NOWARNING.  No DTDLOAD, no NOENT, no HUGE, no XInclude, no
 *     external entity loader hook, no process-global state.
 *   - CSS declarations: vendored libcroco buffer API
 *     (cr_declaration_parse_list_from_buf) only.  cr_parser_parse_file is never
 *     called, so @import / @font-face / url() can never be fetched or resolved.
 *   - every cap is enforced *during* parsing; a trip calls xmlStopParser() and
 *     yields Status::limit_exceeded.
 *   - the returned fragment always comes from TextPaste::build_fragment(), so the
 *     UTF-8/NUL/control/style/numeric/resource checks of text-paste.h cannot be
 *     bypassed here.
 *
 * The module is pure: no GTK, no document, no clipboard, no I/O, no network and
 * no logging of clipboard bytes.
 */

#include "ui/text-paste-html.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>

#include "3rdparty/libcroco/src/cr-declaration.h"
#include "3rdparty/libcroco/src/cr-term.h"

namespace Inkscape::UI::TextPaste::HtmlImport {

namespace {

using TextPaste::Fragment;
using TextPaste::Paragraph;
using TextPaste::Run;

// ---------------------------------------------------------------------------
// Shared constants
// ---------------------------------------------------------------------------

constexpr std::size_t MAX_DIAGNOSTICS = 64;
constexpr std::size_t MAX_DIAGNOSTIC_BYTES = 128;
constexpr std::size_t CF_HTML_HEADER_WINDOW = 1024;
constexpr std::size_t CHARSET_SCAN_WINDOW = 1024;
constexpr std::size_t MAX_SELECTOR_BYTES = 256;
constexpr std::size_t PARSE_CHUNK_BYTES = 4096;
constexpr std::size_t MAX_LIST_MARKER_DEPTH = 16;
/**
 * Conservative cap on CSS parenthesis/function nesting accepted by the decoder.
 *
 * libcroco's declaration parser recurses per nested function
 * (`cr_parser_parse_expr` -> `cr_parser_parse_term` -> `cr_parser_parse_function`)
 * with no depth guard, so a hostile value inside the decoder's own byte budgets
 * can exhaust the stack and SIGSEGV the process (r3 product review P1-1). Before
 * any declaration text reaches `cr_declaration_parse_list_from_buf` it is
 * pre-scanned by scan_css_lexical(), which rejects more than this many
 * *unbalanced* opening parentheses outside comments, strings and escapes.
 *
 * The value is deliberately a small literal: real-world CSS nests two or three
 * functions (`rgb(calc(...))`), so 32 cannot reject a legitimate declaration,
 * while it stays far below the ~28,000 nesting levels measured to overflow the
 * main thread stack. Counting every `(` outside strings and comments rather than
 * only `ident(` is an over-approximation of libcroco's recursion in the safe
 * direction. MAX_CSS_FUNCTION_BUDGET below is the independent absolute bound
 * that does not depend on this lexical model at all.
 */
constexpr std::size_t MAX_CSS_NESTING_DEPTH = 32;
/**
 * Absolute bound on the number of `(` bytes in one CSS declaration list.
 *
 * This is the soundness backstop for the lexical scan below. libcroco's
 * declaration recursion consumes exactly one raw `(` per nested function token,
 * and a function token always consumes a raw `(` from the input, so the stack
 * depth can never exceed the number of `(` bytes -- whatever the tokenizer's
 * error recovery re-parses (its recovery loop is character-based, so it can even
 * re-scan the body of a string that failed to tokenize). A hostile payload
 * cannot hide nesting from this count.
 *
 * 1,024 keeps the worst case around a third of a 1 MiB thread stack (the
 * measured overflow needs ~24,000 nested levels on an 8 MiB stack) while staying
 * far above real markup: the whole <style> budget is 64 KiB and a single rule
 * body with more than a thousand function calls is already pathological.
 */
constexpr std::size_t MAX_CSS_FUNCTION_BUDGET = 1024;
/**
 * Largest magnitude accepted for an HTML list counter ("ol start", "li value").
 *
 * The value is deliberately a documented literal rather than LONG_MAX: it keeps
 * the counter far away from the arithmetic limits of long, so emitting and
 * incrementing markers cannot overflow.  A literal outside this range (including
 * one that overflows strtol and sets ERANGE) is ignored, so the list keeps its
 * default numbering; it is never clamped toward the hostile boundary value.
 */
constexpr long MAX_LIST_COUNTER = 1000000000L;

/** Element sets (REPORT.md sections 6 and 8), space delimited for bounded lookup. */
constexpr std::string_view BLOCK_ELEMENTS =
    " address article aside blockquote caption dd details dialog div dl dt fieldset figcaption figure "
    "footer form h1 h2 h3 h4 h5 h6 header hr li main nav ol p pre section summary table tbody td "
    "tfoot th thead tr ul ";
constexpr std::string_view SKIP_ELEMENTS =
    " script style noscript template head title meta link base iframe frame frameset noframes object "
    "embed applet param svg math canvas audio video source track map area img picture input select "
    "option optgroup textarea button datalist output progress meter marquee ";
/** Exactly the union of libxml2's HTML 4.01 and HTML5 no-content element sets. */
constexpr std::string_view VOID_ELEMENTS =
    " area base basefont br col embed frame hr img input isindex keygen link menuitem meta param "
    "source track wbr ";
constexpr std::string_view TABLE_WRAPPERS = " table tbody thead tfoot ";
constexpr std::string_view TABLE_CELLS = " td th ";
constexpr std::string_view NAMED_COLOURS =
    " aliceblue antiquewhite aqua aquamarine azure beige bisque black blanchedalmond blue blueviolet "
    "brown burlywood cadetblue chartreuse chocolate coral cornflowerblue cornsilk crimson cyan darkblue "
    "darkcyan darkgoldenrod darkgray darkgreen darkgrey darkkhaki darkmagenta darkolivegreen darkorange "
    "darkorchid darkred darksalmon darkseagreen darkslateblue darkslategray darkslategrey darkturquoise "
    "darkviolet deeppink deepskyblue dimgray dimgrey dodgerblue firebrick floralwhite forestgreen fuchsia "
    "gainsboro ghostwhite gold goldenrod gray green greenyellow grey honeydew hotpink indianred indigo "
    "ivory khaki lavender lavenderblush lawngreen lemonchiffon lightblue lightcoral lightcyan "
    "lightgoldenrodyellow lightgray lightgreen lightgrey lightpink lightsalmon lightseagreen lightskyblue "
    "lightslategray lightslategrey lightsteelblue lightyellow lime limegreen linen magenta maroon "
    "mediumaquamarine mediumblue mediumorchid mediumpurple mediumseagreen mediumslateblue mediumspringgreen "
    "mediumturquoise mediumvioletred midnightblue mintcream mistyrose moccasin navajowhite navy oldlace "
    "olive olivedrab orange orangered orchid palegoldenrod palegreen paleturquoise palevioletred papayawhip "
    "peachpuff peru pink plum powderblue purple rebeccapurple red rosybrown royalblue saddlebrown salmon "
    "sandybrown seagreen seashell sienna silver skyblue slateblue slategray slategrey snow springgreen "
    "steelblue tan teal thistle tomato turquoise violet wheat white whitesmoke yellow yellowgreen ";

/** Canonical declaration order; mirrors the allow-lists in ui/text-paste.h. */
constexpr std::string_view CHAR_PROPERTY_ORDER[] = {
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
constexpr std::string_view PARA_PROPERTY_ORDER[] = {
    "text-align", "text-align-last", "text-indent", "line-height", "white-space",
    "writing-mode", "direction", "unicode-bidi", "text-anchor",
};

constexpr std::string_view INHERITED_PROPERTIES[] = {
    "font-family", "font-size", "font-style", "font-weight", "font-variant",
    "font-variant-alternates", "font-variant-caps", "font-variant-east-asian",
    "font-variant-ligatures", "font-variant-numeric", "font-variant-position", "font-stretch",
    "color", "letter-spacing", "word-spacing", "line-height", "text-align", "text-transform",
    "direction", "white-space",
};

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

bool has_utf8_bom(std::string_view s)
{
    return s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
           static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF;
}

std::string to_lower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return out;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r' ||
                          s.front() == '\f')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r' ||
                          s.back() == '\f')) {
        s.remove_suffix(1);
    }
    return s;
}

/** Space delimited word lookup: the haystack must be " word word ". */
bool in_word_list(std::string_view haystack, std::string_view word)
{
    if (word.empty() || word.size() + 2 > haystack.size()) {
        return false;
    }
    std::size_t pos = 0;
    while ((pos = haystack.find(word, pos)) != std::string_view::npos) {
        std::size_t const end = pos + word.size();
        bool const left_ok = pos == 0 || haystack[pos - 1] == ' ';
        bool const right_ok = end >= haystack.size() || haystack[end] == ' ';
        if (left_ok && right_ok) {
            return true;
        }
        pos = end;
    }
    return false;
}

bool is_block_element(std::string_view tag) { return in_word_list(BLOCK_ELEMENTS, tag); }
bool is_skip_element(std::string_view tag) { return in_word_list(SKIP_ELEMENTS, tag); }
bool is_void_element(std::string_view tag) { return in_word_list(VOID_ELEMENTS, tag); }
bool is_table_wrapper(std::string_view tag) { return in_word_list(TABLE_WRAPPERS, tag); }
bool is_table_cell(std::string_view tag) { return in_word_list(TABLE_CELLS, tag); }

bool is_inherited_property(std::string_view name)
{
    for (auto candidate : INHERITED_PROPERTIES) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// CF_HTML envelope (REPORT.md 4.2 / 4.3)
// ---------------------------------------------------------------------------

struct OffsetField {
    bool seen = false;
    bool valid = false;
    long long value = 0;
};

struct CfHtmlHeader {
    OffsetField start_html;
    OffsetField end_html;
    OffsetField start_fragment;
    OffsetField end_fragment;
    OffsetField start_selection;
    OffsetField end_selection;
};

/**
 * Value grammar: optional single '-', 1..10 ASCII digits, optional surrounding
 * whitespace, terminated by the end of the line.  No atoi, no sign other than
 * '-', no overflow past ten digits, no trailing junk.
 */
bool parse_offset_value(std::string_view line, std::size_t begin, long long &out)
{
    std::size_t i = begin;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    bool const negative = i < line.size() && line[i] == '-';
    if (negative) {
        ++i;
    }
    unsigned digits = 0;
    long long value = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        if (digits == 10) {
            return false; // more than ten digits: invalid, never overflowing
        }
        value = value * 10 + (line[i] - '0');
        ++digits;
        ++i;
    }
    if (digits == 0) {
        return false;
    }
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i != line.size()) {
        return false;
    }
    out = negative ? -value : value;
    return true;
}

void parse_cf_html_line(std::string_view line, CfHtmlHeader &header)
{
    struct FieldRef {
        std::string_view name;
        OffsetField *field;
    };
    FieldRef const fields[] = {
        {"StartHTML", &header.start_html},
        {"EndHTML", &header.end_html},
        {"StartFragment", &header.start_fragment},
        {"EndFragment", &header.end_fragment},
        {"StartSelection", &header.start_selection},
        {"EndSelection", &header.end_selection},
    };
    for (auto const &ref : fields) {
        if (line.size() > ref.name.size() && line.compare(0, ref.name.size(), ref.name) == 0 &&
            line[ref.name.size()] == ':') {
            if (ref.field->seen) {
                return; // first occurrence wins
            }
            ref.field->seen = true;
            long long value = 0;
            if (parse_offset_value(line, ref.name.size() + 1, value)) {
                ref.field->valid = true;
                ref.field->value = value;
            }
            return;
        }
    }
}

std::string_view header_line_at(std::string_view window, std::size_t pos)
{
    std::size_t eol = pos;
    while (eol < window.size() && window[eol] != '\r' && window[eol] != '\n') {
        ++eol;
    }
    return window.substr(pos, eol - pos);
}

std::size_t next_header_line(std::string_view window, std::size_t pos)
{
    while (pos < window.size() && window[pos] != '\r' && window[pos] != '\n') {
        ++pos;
    }
    while (pos < window.size() && (window[pos] == '\r' || window[pos] == '\n')) {
        ++pos;
    }
    return pos;
}

} // namespace

Envelope unwrap_html_envelope(std::string_view bytes)
{
    Envelope env;
    std::size_t const bom = has_utf8_bom(bytes) ? 3 : 0;
    std::string_view const view = bytes.substr(bom);
    std::string_view const window =
        view.substr(0, std::min<std::size_t>(CF_HTML_HEADER_WINDOW, view.size()));

    bool looks_cf_html = window.size() >= 8 && window.compare(0, 8, "Version:") == 0;
    if (looks_cf_html) {
        // Detection also requires a line-start StartHTML:/StartFragment: in the window.
        bool anchor_seen = false;
        for (std::size_t pos = 0; pos < window.size();) {
            std::string_view const line = header_line_at(window, pos);
            if ((line.size() > 10 && line.compare(0, 10, "StartHTML:") == 0) ||
                (line.size() > 14 && line.compare(0, 14, "StartFragment:") == 0)) {
                anchor_seen = true;
                break;
            }
            std::size_t const next = next_header_line(window, pos);
            if (next == pos) {
                break;
            }
            pos = next;
        }
        looks_cf_html = anchor_seen;
    }

    if (!looks_cf_html) {
        // The whole payload is the document; an empty or whitespace-only one is invalid.
        env.begin = bom;
        env.end = bytes.size();
        env.cf_html = false;
        env.valid = !trim(view).empty();
        return env;
    }

    env.cf_html = true;

    CfHtmlHeader header;
    for (std::size_t pos = 0; pos < window.size();) {
        parse_cf_html_line(header_line_at(window, pos), header);
        std::size_t const next = next_header_line(window, pos);
        if (next == pos) {
            break;
        }
        pos = next;
    }

    auto usable = [](OffsetField const &field) {
        return field.seen && field.valid && field.value != -1; // -1 means "not available"
    };
    bool const fragment_pair = usable(header.start_fragment) && usable(header.end_fragment);
    bool const html_pair = usable(header.start_html) && usable(header.end_html);

    long long begin = 0;
    long long end = 0;
    if (fragment_pair) {
        begin = header.start_fragment.value;
        end = header.end_fragment.value;
    } else if (html_pair) {
        begin = header.start_html.value;
        end = header.end_html.value;
    } else {
        return env; // no usable offset pair
    }

    // 0 <= begin <= end <= size is checked before any subtraction.
    if (begin < 0 || end < 0 || begin > end) {
        return env;
    }
    if (static_cast<unsigned long long>(end) > static_cast<unsigned long long>(bytes.size())) {
        return env;
    }
    if (fragment_pair && html_pair) {
        if (header.start_html.value > header.start_fragment.value ||
            header.end_fragment.value > header.end_html.value) {
            return env;
        }
    }
    auto const begin_index = static_cast<std::size_t>(begin);
    auto const end_index = static_cast<std::size_t>(end);
    if (begin_index < bytes.size() && (static_cast<unsigned char>(bytes[begin_index]) & 0xC0) == 0x80) {
        return env; // offset splits a UTF-8 sequence
    }
    if (end_index < bytes.size() && (static_cast<unsigned char>(bytes[end_index]) & 0xC0) == 0x80) {
        return env; // offset splits a UTF-8 sequence
    }
    if (trim(bytes.substr(begin_index, end_index - begin_index)).empty()) {
        return env; // empty fragment
    }

    env.begin = begin_index;
    env.end = end_index;
    env.valid = true;
    return env;
}

namespace {

// ---------------------------------------------------------------------------
// Charset declaration (REPORT.md 4.2): UTF-8 / US-ASCII only, first 1024 bytes
// ---------------------------------------------------------------------------

std::string_view charset_value_in(std::string_view tag, std::size_t charset_pos)
{
    std::size_t i = charset_pos + 7; // strlen("charset")
    while (i < tag.size() && g_ascii_isspace(tag[i])) {
        ++i;
    }
    if (i >= tag.size() || tag[i] != '=') {
        return {};
    }
    ++i;
    while (i < tag.size() && g_ascii_isspace(tag[i])) {
        ++i;
    }
    if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
        char const quote = tag[i];
        ++i;
        std::size_t end = i;
        while (end < tag.size() && tag[end] != quote) {
            ++end;
        }
        return tag.substr(i, end - i);
    }
    std::size_t end = i;
    while (end < tag.size() &&
           (g_ascii_isalnum(tag[end]) || tag[end] == '-' || tag[end] == '_' || tag[end] == '.')) {
        ++end;
    }
    return end == i ? std::string_view{} : tag.substr(i, end - i);
}

bool charset_is_supported(std::string_view document)
{
    std::string_view const window =
        document.substr(0, std::min<std::size_t>(CHARSET_SCAN_WINDOW, document.size()));
    std::size_t pos = 0;
    while (pos < window.size()) {
        std::string const lowered = to_lower(window.substr(pos));
        std::size_t const relative = lowered.find("<meta");
        if (relative == std::string::npos) {
            return true;
        }
        std::size_t const meta_at = pos + relative;
        std::size_t gt = window.find('>', meta_at);
        if (gt == std::string::npos) {
            gt = window.size();
        }
        std::string const tag = to_lower(window.substr(meta_at, gt - meta_at));
        std::size_t const charset_at = tag.find("charset");
        if (charset_at != std::string::npos) {
            bool const boundary =
                charset_at == 0 || !(g_ascii_isalnum(tag[charset_at - 1]) || tag[charset_at - 1] == '-' ||
                                     tag[charset_at - 1] == '_');
            if (boundary) {
                std::string const value = to_lower(charset_value_in(tag, charset_at));
                if (!value.empty()) {
                    return value == "utf-8" || value == "utf8" || value == "us-ascii";
                }
            }
        }
        if (gt >= window.size()) {
            break;
        }
        pos = gt;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Style declaration model
// ---------------------------------------------------------------------------

struct Declaration {
    std::string name;
    std::string value;
};

using Declarations = std::vector<Declaration>;

void set_declaration(Declarations &list, std::string name, std::string value)
{
    for (auto &decl : list) {
        if (decl.name == name) {
            decl.value = std::move(value);
            return;
        }
    }
    list.push_back({std::move(name), std::move(value)});
}

std::string const *find_declaration(Declarations const &list, std::string_view name)
{
    for (auto const &decl : list) {
        if (decl.name == name) {
            return &decl.value;
        }
    }
    return nullptr;
}

/** Declarations in the canonical order of @a order, unknown names appended. */
template <std::size_t N>
void serialize_in_order(Declarations const &list, std::string_view const (&order)[N], std::string &out)
{
    for (auto name : order) {
        auto const *value = find_declaration(list, name);
        if (value && !value->empty()) {
            out.append(name.data(), name.size());
            out += ':';
            out += *value;
            out += ';';
        }
    }
    for (auto const &decl : list) {
        bool known = false;
        for (auto name : order) {
            if (name == decl.name) {
                known = true;
                break;
            }
        }
        if (!known && !decl.value.empty()) {
            out += decl.name;
            out += ':';
            out += decl.value;
            out += ';';
        }
    }
}

std::string serialize_declarations(Declarations const &list, bool paragraph_scope)
{
    std::string out;
    if (paragraph_scope) {
        serialize_in_order(list, PARA_PROPERTY_ORDER, out);
    } else {
        serialize_in_order(list, CHAR_PROPERTY_ORDER, out);
    }
    return out;
}

/** Final gate before a declaration may enter a style context. */
bool usable_declaration(std::string_view name, std::string_view value)
{
    if (value.empty() || value.size() > TextPaste::MAX_STYLE_VALUE_LENGTH) {
        return false;
    }
    if (!TextPaste::is_safe_style_value(value)) {
        return false;
    }
    std::string decl;
    decl.reserve(name.size() + value.size() + 2);
    decl.append(name);
    decl += ':';
    decl.append(value);
    decl += ';';
    if (!TextPaste::style_lengths_are_valid(decl)) {
        return false;
    }
    // Reuse the shared non-finite numeric guard (opacity, font-weight, ...).
    return TextPaste::detail::style_numbers_are_finite(decl);
}

std::string strip_important(std::string_view value)
{
    std::string_view const trimmed = trim(value);
    std::size_t const bang = trimmed.rfind('!');
    if (bang == std::string_view::npos) {
        return std::string(trimmed);
    }
    if (to_lower(trim(trimmed.substr(bang + 1))) == "important") {
        return std::string(trim(trimmed.substr(0, bang)));
    }
    return std::string(trimmed);
}

bool is_colour_property(std::string_view name)
{
    return name == "color" || name == "fill" || name == "stroke" || name == "text-decoration-color";
}

bool is_hex_colour(std::string_view value)
{
    if (value.size() != 4 && value.size() != 7) {
        return false;
    }
    if (value.front() != '#') {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        if (!g_ascii_isxdigit(value[i])) {
            return false;
        }
    }
    return true;
}

bool is_colour_function(std::string_view value)
{
    static constexpr std::string_view FUNCTIONS[] = {"rgb(", "rgba(", "hsl(", "hsla("};
    for (auto function : FUNCTIONS) {
        if (value.size() > function.size() + 1 && value.compare(0, function.size(), function) == 0 &&
            value.back() == ')') {
            for (std::size_t i = function.size(); i + 1 < value.size(); ++i) {
                char const c = value[i];
                if (!(g_ascii_isdigit(c) || c == '.' || c == ',' || c == '%' || c == ' ' || c == '/' ||
                      c == '+' || c == '-' || c == 'e' || c == 'E')) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

/** Self-contained colour grammar; see the design notes for the deviation. */
bool is_valid_colour(std::string_view value)
{
    std::string const lower = to_lower(trim(value));
    if (lower.empty()) {
        return false;
    }
    if (lower == "none" || lower == "transparent" || lower == "currentcolor" || lower == "inherit") {
        return true;
    }
    if (is_hex_colour(lower) || is_colour_function(lower)) {
        return true;
    }
    return in_word_list(NAMED_COLOURS, lower);
}

enum class LengthPolicy { Plain, FontSize, LineHeight };

std::optional<std::string> convert_length(std::string_view raw, LengthPolicy policy, double base_font_size,
                                          double *px_out = nullptr)
{
    auto const token = TextPaste::detail::parse_length_token(raw);
    if (!token.numeric) {
        if (policy == LengthPolicy::FontSize) {
            return std::nullopt; // no keyword identifies one document-space size
        }
        return std::string(raw);
    }
    if (!token.finite) {
        return std::nullopt;
    }

    if (token.unit.empty()) {
        if (!token.tail.empty()) {
            if (token.tail.front() != '%' || token.tail.size() != 1) {
                return std::nullopt;
            }
            if (policy == LengthPolicy::LineHeight) {
                return std::string(raw); // a percentage line-height stays a multiplier
            }
            double const px = token.number * base_font_size / 100.0;
            if (!std::isfinite(px)) {
                return std::nullopt;
            }
            if (px_out) {
                *px_out = px;
            }
            std::string out(token.leading);
            out += TextPaste::detail::format_length(px);
            out += "px";
            return out;
        }
        // Unitless: a multiplier for line-height, a user unit for the API-scaled
        // properties, never a resolved font size.
        if (policy == LengthPolicy::FontSize) {
            return std::nullopt;
        }
        return std::string(raw);
    }

    if (token.unit == "em" || token.unit == "rem") {
        if (!token.tail.empty()) {
            return std::nullopt;
        }
        double const base = token.unit == "rem" ? 16.0 : base_font_size;
        double const px = token.number * base;
        if (!std::isfinite(px) || (policy == LengthPolicy::FontSize && px <= 0.0)) {
            return std::nullopt;
        }
        if (px_out) {
            *px_out = px;
        }
        std::string out(token.leading);
        out += TextPaste::detail::format_length(px);
        out += "px";
        return out;
    }

    double factor = 0.0;
    if (!TextPaste::detail::absolute_unit_px(token.unit, factor) || !token.tail.empty()) {
        return std::nullopt; // ex, ch, vw, vh, vmin, vmax, unknown units
    }
    double const px = token.number * factor;
    if (!std::isfinite(px) || (policy == LengthPolicy::FontSize && px <= 0.0)) {
        return std::nullopt;
    }
    if (px_out) {
        *px_out = px;
    }
    std::string out(token.leading);
    out += TextPaste::detail::format_length(px);
    out += "px";
    return out;
}

std::optional<std::string> convert_list_length(std::string_view raw, double base_font_size)
{
    std::string out;
    std::size_t begin = 0;
    for (;;) {
        std::size_t const comma = raw.find(',', begin);
        std::string_view const item =
            comma == std::string_view::npos ? raw.substr(begin) : raw.substr(begin, comma - begin);
        auto converted = convert_length(item, LengthPolicy::Plain, base_font_size);
        if (!converted) {
            return std::nullopt;
        }
        out += *converted;
        if (comma == std::string_view::npos) {
            break;
        }
        out += ", ";
        begin = comma + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// CSS selectors and rules (REPORT.md 7.1 / 10.1 L13-L15)
// ---------------------------------------------------------------------------

struct Compound {
    std::string tag; // empty for the universal selector
    std::string id;
    std::vector<std::string> classes;
    bool universal = false;
};

struct SelectorPart {
    Compound compound;
    bool child_combinator = false;
};

struct Selector {
    std::vector<SelectorPart> parts;
    int specificity = 0;
};

struct CssRule {
    Selector selector;
    std::size_t declarations = 0;
    std::size_t order = 0;
};

bool parse_compound(std::string_view text, Compound &out)
{
    out = Compound{};
    std::size_t i = 0;
    if (i < text.size() && text[i] == '*') {
        out.universal = true;
        ++i;
    } else if (i < text.size() && g_ascii_isalpha(text[i])) {
        std::size_t const begin = i;
        while (i < text.size() && (g_ascii_isalnum(text[i]) || text[i] == '-' || text[i] == '_')) {
            ++i;
        }
        out.tag = to_lower(text.substr(begin, i - begin));
    }
    while (i < text.size()) {
        char const kind = text[i];
        if (kind != '.' && kind != '#') {
            return false; // ':', '[', '(', '+', '~', '|', non-ASCII, ...
        }
        ++i;
        std::size_t const begin = i;
        while (i < text.size() && (g_ascii_isalnum(text[i]) || text[i] == '-' || text[i] == '_')) {
            ++i;
        }
        if (i == begin) {
            return false;
        }
        std::string name(text.substr(begin, i - begin));
        if (kind == '.') {
            out.classes.push_back(std::move(name));
        } else {
            if (!out.id.empty()) {
                return false;
            }
            out.id = std::move(name);
        }
    }
    return out.universal || !out.tag.empty() || !out.classes.empty() || !out.id.empty();
}

bool parse_selector(std::string_view text, Selector &out)
{
    out = Selector{};
    std::size_t i = 0;
    bool pending_child = false;
    bool first = true;
    while (i < text.size()) {
        while (i < text.size() && g_ascii_isspace(text[i])) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        if (text[i] == '>') {
            if (first || pending_child) {
                return false;
            }
            pending_child = true;
            ++i;
            continue;
        }
        std::size_t const begin = i;
        while (i < text.size() && !g_ascii_isspace(text[i]) && text[i] != '>') {
            ++i;
        }
        Compound compound;
        if (!parse_compound(text.substr(begin, i - begin), compound)) {
            return false;
        }
        SelectorPart part;
        part.child_combinator = pending_child;
        part.compound = std::move(compound);
        pending_child = false;
        first = false;
        if (!part.compound.id.empty()) {
            out.specificity += 100;
        }
        out.specificity += 10 * static_cast<int>(part.compound.classes.size());
        if (!part.compound.tag.empty()) {
            out.specificity += 1;
        }
        out.parts.push_back(std::move(part));
    }
    return !pending_child && !out.parts.empty();
}

std::vector<std::string_view> split_commas(std::string_view text)
{
    std::vector<std::string_view> out;
    std::size_t begin = 0;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            if (depth > 0) {
                --depth;
            }
        } else if (text[i] == ',' && depth == 0) {
            out.push_back(text.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    out.push_back(text.substr(begin));
    return out;
}

std::string strip_css_comments(std::string_view css)
{
    std::string out;
    out.reserve(css.size());
    std::size_t i = 0;
    while (i < css.size()) {
        if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/')) {
                ++i;
            }
            i = std::min(css.size(), i + 2);
            out += ' ';
            continue;
        }
        out += css[i++];
    }
    return out;
}

/** Verdict of the byte-level CSS pre-scan that protects the libcroco call. */
enum class CssLexicalVerdict {
    safe,            ///< well formed and within the nesting cap: libcroco may parse it
    nesting_limit,   ///< more than the allowed unbalanced `(` outside strings/comments
    function_budget, ///< more `(` bytes than the absolute recursion bound
    malformed,       ///< unterminated string/comment or a dangling escape: never parsed
};

/**
 * Deterministic byte-level lexical pre-scan of on-the-wire CSS text.
 *
 * This runs on the exact byte sequence handed to
 * `cr_declaration_parse_list_from_buf()` and only decides whether that call is
 * allowed; libcroco stays the parser of record for everything it accepts. One
 * left-to-right pass, no allocation, no recursion, early exit on the cap, so the
 * work is O(bytes) with a tiny constant.
 *
 * Tracked explicitly because each construct can hide a raw `(` from the
 * parenthesis count:
 *  - CSS block comments (the caller has already stripped them, but the scan
 *    stays correct on raw CSS as well);
 *  - `"..."`/`'...'` strings, but only while their escapes are ones libcroco
 *    itself accepts (space, non-ASCII, hex/unicode, escaped newline, verbatim
 *    quote/backslash). libcroco rejects every other escape, fails the STRING
 *    token and re-scans the body as ordinary tokens, so `\(`, unescaped control
 *    bytes, newlines and unterminated strings reject the declaration text
 *    instead of being modelled;
 *  - `\` escapes outside strings (libcroco consumes the escaped byte as
 *    identifier content, so the scanner skips the pair; a byte escaped this way
 *    can never be a function opener).
 *
 * An unterminated construct is rejected instead of modelled: libcroco's own
 * error recovery rescans malformed input, which is exactly where a lexical
 * under-approximation could reappear. Rejecting is safe because valid CSS never
 * contains an unterminated string, comment or trailing escape.
 *
 * Two independent limits are applied. The absolute function budget counts every
 * `(` byte in the text (including string bodies, because libcroco's character-
 * based recovery can re-parse them); it cannot be bypassed by any lexical trick
 * and alone guarantees the recursion bound. The nesting cap is the tight,
 * documented policy on top of it and rejects the classic bomb early with a
 * precise reason.
 */
CssLexicalVerdict scan_css_lexical(std::string_view css, std::size_t nesting_limit, std::size_t function_budget)
{
    std::size_t depth = 0;
    std::size_t i = 0;
    while (i < css.size()) {
        unsigned char const c = static_cast<unsigned char>(css[i]);
        if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            i += 2;
            bool closed = false;
            while (i + 1 < css.size()) {
                if (css[i] == '*' && css[i + 1] == '/') {
                    i += 2;
                    closed = true;
                    break;
                }
                ++i;
            }
            if (!closed) {
                return CssLexicalVerdict::malformed;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            unsigned char const quote = c;
            ++i;
            bool closed = false;
            while (i < css.size()) {
                unsigned char const s = static_cast<unsigned char>(css[i]);
                if (s == '\\') {
                    if (i + 1 >= css.size()) {
                        return CssLexicalVerdict::malformed; // dangling escape
                    }
                    unsigned char const n = static_cast<unsigned char>(css[i + 1]);
                    bool const verbatim = n == '\'' || n == '"' || n == '\\' || n == ' ';
                    bool const non_ascii = n >= 0x80;
                    bool const hex = (n >= '0' && n <= '9') || (n >= 'a' && n <= 'f') || (n >= 'A' && n <= 'F');
                    bool const line_continuation = n == '\n' || n == '\r' || n == '\f';
                    if (!verbatim && !non_ascii && !hex && !line_continuation) {
                        // libcroco's cr_tknzr_parse_escape() accepts only space,
                        // non-ASCII and hex/unicode escapes. Any other byte (for
                        // example `\(`) fails the STRING token; its tokenizer then
                        // falls back to a delimiter and re-scans the string body as
                        // ordinary tokens, so the content we would skip as a string
                        // can become live declarations containing nested functions.
                        // Never model that recovery: reject the declaration text.
                        return CssLexicalVerdict::malformed;
                    }
                    i += 2;
                    if (n == '\r' && i < css.size() && css[i] == '\n') {
                        ++i; // an escaped CRLF line continuation is consumed as one unit
                    }
                    continue;
                }
                if (s == quote) {
                    ++i;
                    closed = true;
                    break;
                }
                // libcroco's string token accepts only tab, 0x20-0x7E and
                // non-ASCII bytes; anything else aborts the token and starts a
                // recovery re-parse the scanner cannot mirror.
                if (s == '\n' || s == '\r' || s == '\f' || s == 0x7f || (s < 0x20 && s != '\t')) {
                    return CssLexicalVerdict::malformed;
                }
                ++i;
            }
            if (!closed) {
                return CssLexicalVerdict::malformed; // unterminated string
            }
            continue;
        }
        if (c == '\\') {
            if (i + 1 >= css.size()) {
                return CssLexicalVerdict::malformed; // dangling escape
            }
            // Outside a string libcroco consumes `\` plus the escaped byte as
            // identifier content. A byte escaped this way can never be a function
            // opener (the preceding ident would have to end at the backslash), so
            // skipping the pair cannot hide recursion.
            i += 2;
            continue;
        }
        if (c == '(') {
            if (++depth > nesting_limit) {
                return CssLexicalVerdict::nesting_limit;
            }
        } else if (c == ')') {
            if (depth > 0) {
                --depth;
            }
        }
        ++i;
    }

    // Absolute recursion bound (see MAX_CSS_FUNCTION_BUDGET). It runs after the
    // detailed scan so a classic bomb keeps its precise "nesting" reason, and it
    // is what makes the guard sound when the lexical model cannot see through a
    // construct libcroco's character-based recovery re-parses.
    std::size_t functions = 0;
    for (char const ch : css) {
        if (ch == '(' && ++functions > function_budget) {
            return CssLexicalVerdict::function_budget;
        }
    }
    return CssLexicalVerdict::safe;
}

std::size_t skip_at_rule(std::string_view css, std::size_t start)
{
    std::size_t i = start;
    int depth = 0;
    while (i < css.size()) {
        char const c = css[i];
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth <= 0) {
                return i + 1;
            }
        } else if (c == ';' && depth == 0) {
            return i + 1;
        }
        ++i;
    }
    return css.size();
}

/** ASCII case insensitive search for an already lowercased @a needle. */
std::size_t find_ascii_lower(std::string_view haystack, std::string_view needle, std::size_t from)
{
    if (needle.empty() || haystack.size() < needle.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (g_ascii_tolower(haystack[i + j]) != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return std::string_view::npos;
}

/**
 * Raw spelling of a declaration value, read in source order.
 *
 * libcroco's tokenizer turns a number followed by a CSS3 dimension unit it does
 * not know (`rem`, `q`, `ch`, `vw`, `vh`, ...) into a DIMEN_TK token
 * (cr-tknzr.c, NUM_UNKNOWN_TYPE), and cr_parser_parse_term() then stores only the
 * number for that token (cr-parser.c) -- the unit is destroyed before any caller
 * can see it and cr_num_to_string() prints the bare number.  REPORT.md 7.3 still
 * requires those units to be converted (`q`), resolved (`rem`) or dropped (`vw`),
 * so the value spelling is recovered here from the source text.  libcroco remains
 * the parser of record: it decides which declarations exist, in which order and
 * whether the list is syntactically valid.
 */
std::string_view raw_declaration_value(std::string_view css, std::size_t &cursor, std::string_view name)
{
    while (cursor < css.size()) {
        std::size_t const pos = find_ascii_lower(css, name, cursor);
        if (pos == std::string_view::npos) {
            return {};
        }
        bool const boundary =
            pos == 0 || g_ascii_isspace(css[pos - 1]) || css[pos - 1] == ';' || css[pos - 1] == '{';
        std::size_t colon = pos + name.size();
        while (colon < css.size() && g_ascii_isspace(css[colon])) {
            ++colon;
        }
        if (!boundary || colon >= css.size() || css[colon] != ':') {
            cursor = pos + name.size();
            continue;
        }
        std::size_t const begin = colon + 1;
        std::size_t end = begin;
        int depth = 0;
        while (end < css.size()) {
            char const c = css[end];
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (depth > 0) {
                    --depth;
                }
            } else if (depth == 0 && (c == ';' || c == '}')) {
                break;
            }
            ++end;
        }
        cursor = end;
        return trim(css.substr(begin, end - begin));
    }
    return {};
}

struct ElementInfo {
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
};

bool compound_matches(Compound const &compound, ElementInfo const &info)
{
    if (!compound.id.empty() && compound.id != info.id) {
        return false;
    }
    if (!compound.tag.empty() && compound.tag != info.tag) {
        return false;
    }
    for (auto const &wanted : compound.classes) {
        bool found = false;
        for (auto const &have : info.classes) {
            if (have == wanted) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The SAX decoder
// ---------------------------------------------------------------------------

struct ElementContext {
    Declarations inherited_char;
    Declarations local_char;
    Declarations inherited_para;
    Declarations local_para;
    double font_size_px = 16.0;
};

struct Frame {
    std::string tag;
    bool suppress = false;
    bool in_suppressed_subtree = false;
    bool style_element = false;
    bool list_element = false;
    ElementContext saved;
    ElementInfo info;
};

/**
 * Parses an HTML list counter attribute ("ol start", "li value").
 *
 * Accepts only a complete decimal literal in [-MAX_LIST_COUNTER, MAX_LIST_COUNTER].
 * Rejects trailing junk, empty input and anything out of range, including a
 * literal that overflows long: strtol() is called with errno cleared and ERANGE
 * is rejected, so a hostile literal cannot masquerade as LONG_MAX/LONG_MIN.
 * On rejection the caller keeps the existing/default numbering.
 */
bool parse_list_counter(std::string const &text, long &out)
{
    errno = 0;
    char *end = nullptr;
    long const parsed = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE) {
        return false;
    }
    if (!end || end == text.c_str() || *end != '\0') {
        return false;
    }
    if (parsed < -MAX_LIST_COUNTER || parsed > MAX_LIST_COUNTER) {
        return false;
    }
    out = parsed;
    return true;
}

struct ListState {
    bool ordered = false;
    long counter = 1;
};

struct RuleIndex {
    std::vector<CssRule> rules;
    std::vector<Declarations> declaration_pool;
    std::map<std::string, std::vector<std::size_t>> buckets;

    void add(CssRule rule, Declarations const &declarations)
    {
        std::size_t const index = rules.size();
        rule.declarations = declaration_pool.size();
        declaration_pool.push_back(declarations);
        auto const &compound = rule.selector.parts.back().compound;
        std::string key = "*";
        if (!compound.id.empty()) {
            key = "#" + compound.id;
        } else if (!compound.classes.empty()) {
            key = "." + compound.classes.front();
        } else if (!compound.tag.empty()) {
            key = "t:" + compound.tag;
        }
        buckets[key].push_back(index);
        rules.push_back(std::move(rule));
    }

    std::vector<std::size_t> candidates(ElementInfo const &info) const
    {
        std::vector<std::size_t> out;
        auto append = [&](std::string const &key) {
            auto const it = buckets.find(key);
            if (it != buckets.end()) {
                out.insert(out.end(), it->second.begin(), it->second.end());
            }
        };
        if (!info.id.empty()) {
            append("#" + info.id);
        }
        for (auto const &cls : info.classes) {
            append("." + cls);
        }
        append("t:" + info.tag);
        append("*");
        return out;
    }
};

class SaxDecoder
{
public:
    explicit SaxDecoder(Limits const &limits)
        : limits_(limits)
    {}

    void attach(htmlParserCtxtPtr ctxt) { ctxt_ = ctxt; }

    bool stopped() const { return limit_hit_; }
    bool limit_hit() const { return limit_hit_; }
    bool css_rejected() const { return css_rejected_; }
    std::string const &css_error() const { return css_error_; }
    std::string const &error() const { return error_; }

    std::vector<Paragraph> take_paragraphs() { return std::move(paragraphs_); }
    std::vector<std::string> take_losses() { return std::move(losses_); }

    void start_element(xmlChar const *name, xmlChar const **atts);
    void end_element(xmlChar const *name);
    void characters(xmlChar const *ch, int len);
    void finish();

private:
    // -- caps -----------------------------------------------------------------
    void trip(std::string_view reason)
    {
        if (limit_hit_) {
            return;
        }
        limit_hit_ = true;
        error_ = std::string("limit: ") + std::string(reason);
        if (ctxt_) {
            xmlStopParser(ctxt_);
        }
    }

    void add_loss(std::string_view code)
    {
        if (losses_.size() >= MAX_DIAGNOSTICS) {
            return;
        }
        std::string bounded(code.substr(0, std::min(code.size(), MAX_DIAGNOSTIC_BYTES)));
        for (auto const &existing : losses_) {
            if (existing == bounded) {
                return;
            }
        }
        losses_.push_back(std::move(bounded));
    }

    /**
     * Reject one CSS declaration text before the libcroco call.
     *
     * The rejection is deliberately *not* a cap trip: `limit_exceeded` aborts
     * the whole paste and would let hostile CSS suppress the plain alternative,
     * while `malformed` at the document level lets the caller fall back to the
     * complete plain representation. Sticky: the first rejected declaration
     * list fails the whole document's rich decode.
     */
    void reject_css(std::string_view loss_code, std::string_view error)
    {
        add_loss(loss_code);
        if (!css_rejected_) {
            css_rejected_ = true;
            css_error_.assign(error.substr(0, std::min(error.size(), MAX_DIAGNOSTIC_BYTES * 2)));
        }
    }

    // -- style plumbing -------------------------------------------------------
    std::string current_char_style() const;
    std::string current_paragraph_style() const;
    std::string_view current_white_space() const;
    bool white_space_is_pre() const;
    bool white_space_is_pre_line() const;

    void apply_declarations(Declarations const &source, double resolve_font_size);
    void apply_one(Declaration const &declaration);
    void apply_inline_style(std::string_view css, double resolve_font_size);
    void apply_stylesheet_rules(ElementInfo const &info, double resolve_font_size);
    bool selector_matches(Selector const &selector) const;
    Declarations parse_css_declarations(std::string_view css);
    void parse_stylesheet(std::string_view css);

    // -- fragment emission ----------------------------------------------------
    void process_text(std::string_view text);
    void process_utf8(std::string_view text);
    void append_text(std::string_view text);
    void append_to_run(std::string_view bytes);
    void flush_run();
    void emit_run(std::string const &style, std::string const &text);
    void append_run_piece(std::string const &style, std::string piece);
    void block_break();
    void hard_break();
    bool paragraph_has_text() const { return !current_.runs.empty() || !run_text_.empty(); }

    void push_frame(Frame frame);

    Limits const &limits_;
    htmlParserCtxtPtr ctxt_ = nullptr;

    bool limit_hit_ = false;
    bool css_rejected_ = false;
    std::string error_;
    std::string css_error_;

    std::size_t nodes_ = 0;
    std::size_t runs_ = 0;
    std::size_t text_bytes_ = 0;
    std::size_t css_bytes_ = 0;
    std::size_t css_rules_ = 0;
    std::size_t css_declarations_ = 0;

    std::vector<Frame> stack_;
    std::size_t suppress_depth_ = 0;
    long style_index_ = -1;
    std::string css_buffer_;

    std::vector<ListState> lists_;

    ElementContext ctx_;
    RuleIndex rules_;

    Paragraph current_;
    std::vector<Paragraph> paragraphs_;

    std::string run_text_;
    std::string pending_bytes_;
    bool pending_space_ = false;
    bool pending_cr_ = false;

    std::vector<std::string> losses_;
};

// --- styles ----------------------------------------------------------------

std::string SaxDecoder::current_char_style() const
{
    Declarations merged = ctx_.inherited_char;
    for (auto const &decl : ctx_.local_char) {
        set_declaration(merged, decl.name, decl.value);
    }
    return TextPaste::sanitize_style(serialize_declarations(merged, false), false);
}

std::string SaxDecoder::current_paragraph_style() const
{
    Declarations merged = ctx_.inherited_para;
    for (auto const &decl : ctx_.local_para) {
        set_declaration(merged, decl.name, decl.value);
    }
    return TextPaste::sanitize_style(serialize_declarations(merged, true), true);
}

std::string_view SaxDecoder::current_white_space() const
{
    if (auto const *value = find_declaration(ctx_.local_para, "white-space")) {
        return *value;
    }
    if (auto const *value = find_declaration(ctx_.inherited_para, "white-space")) {
        return *value;
    }
    return {};
}

bool SaxDecoder::white_space_is_pre() const
{
    std::string const value = to_lower(current_white_space());
    return value == "pre" || value == "pre-wrap" || value == "break-spaces";
}

bool SaxDecoder::white_space_is_pre_line() const
{
    return to_lower(current_white_space()) == "pre-line";
}

void SaxDecoder::apply_one(Declaration const &declaration)
{
    std::string const name = declaration.name;
    if (name.empty()) {
        return;
    }
    std::string value = strip_important(declaration.value);
    if (value.empty()) {
        return;
    }

    bool paragraph_scope = false;
    if (!TextPaste::is_allowed_style_property(name, false)) {
        if (!TextPaste::is_allowed_style_property(name, true)) {
            return; // not allow-listed: dropped silently (background, margin, ...)
        }
        paragraph_scope = true;
    }

    if (auto const *rule = TextPaste::detail::length_rule(name)) {
        std::optional<std::string> converted;
        if (rule->list) {
            converted = convert_list_length(value, ctx_.font_size_px);
        } else {
            LengthPolicy const policy = name == "line-height" ? LengthPolicy::LineHeight : LengthPolicy::Plain;
            converted = convert_length(value, policy, ctx_.font_size_px);
        }
        if (!converted) {
            add_loss("unsupported-length-value");
            return;
        }
        value = std::move(*converted);
    } else if (is_colour_property(name)) {
        if (!is_valid_colour(value)) {
            add_loss("unsupported-colour");
            return;
        }
    }

    if (!usable_declaration(name, value)) {
        add_loss("unsafe-style-value");
        return;
    }

    bool const inherited = is_inherited_property(name);
    Declarations &target = paragraph_scope ? (inherited ? ctx_.inherited_para : ctx_.local_para)
                                           : (inherited ? ctx_.inherited_char : ctx_.local_char);
    set_declaration(target, name, std::move(value));
}

void SaxDecoder::apply_declarations(Declarations const &source, double resolve_font_size)
{
    // Font size first: em/% of the sibling properties resolve against the
    // element's own size, while the size itself resolves against the parent's.
    for (auto const &declaration : source) {
        if (declaration.name != "font-size") {
            continue;
        }
        std::string const value = strip_important(declaration.value);
        double px = 0.0;
        auto converted = convert_length(value, LengthPolicy::FontSize, resolve_font_size, &px);
        if (!converted || !usable_declaration("font-size", *converted)) {
            add_loss("unsupported-font-size");
            continue;
        }
        ctx_.font_size_px = px;
        set_declaration(ctx_.inherited_char, "font-size", std::move(*converted));
    }
    for (auto const &declaration : source) {
        if (declaration.name == "font-size") {
            continue;
        }
        apply_one(declaration);
    }
}

Declarations SaxDecoder::parse_css_declarations(std::string_view css)
{
    Declarations out;
    if (css.empty() || css_rejected_) {
        return out;
    }
    std::string const cleaned = strip_css_comments(css);
    // P1-1 (r3 product review): libcroco's declaration parser has no depth guard
    // in its expr/term/function recursion, so hostile function nesting inside the
    // decoder's own byte budgets can exhaust the stack before any per-value cap
    // runs. This lexical pre-scan is the only gate; it runs on the exact bytes
    // handed to libcroco below and never calls the library on a rejection.
    switch (scan_css_lexical(cleaned, MAX_CSS_NESTING_DEPTH, MAX_CSS_FUNCTION_BUDGET)) {
        case CssLexicalVerdict::nesting_limit:
            reject_css("css-nesting-limit", "css function nesting exceeds the decoder limit");
            return out;
        case CssLexicalVerdict::function_budget:
            reject_css("css-function-budget", "css declaration text declares too many functions");
            return out;
        case CssLexicalVerdict::malformed:
            reject_css("css-lexical-malformed", "css declaration text is not lexically well formed");
            return out;
        case CssLexicalVerdict::safe:
            break;
    }
    CRDeclaration *list =
        cr_declaration_parse_list_from_buf(reinterpret_cast<guchar const *>(cleaned.c_str()), CR_UTF_8);
    std::size_t cursor = 0;
    for (CRDeclaration *decl = list; decl; decl = decl->next) {
        if (++css_declarations_ > limits_.max_css_declarations) {
            trip("css-declarations");
            break;
        }
        if (!decl->property || !decl->property->stryng || !decl->property->stryng->str) {
            continue;
        }
        std::string name = to_lower(trim(reinterpret_cast<char const *>(decl->property->stryng->str)));
        std::string value;
        std::string_view const raw = raw_declaration_value(cleaned, cursor, name);
        if (!raw.empty()) {
            value.assign(raw.data(), raw.size());
        } else {
            // Fallback for declarations whose spelling cannot be located (for
            // example a property written with different capitalisation inside a
            // construct the scanner does not model): libcroco's serialization.
            guchar *serialized = decl->value ? cr_term_to_string(decl->value) : nullptr;
            if (serialized) {
                value.assign(reinterpret_cast<char const *>(serialized));
            }
            g_free(serialized);
        }
        out.push_back({std::move(name), std::move(value)});
    }
    if (list) {
        cr_declaration_destroy(list);
    }
    return out;
}

void SaxDecoder::apply_inline_style(std::string_view css, double resolve_font_size)
{
    Declarations parsed = parse_css_declarations(css);
    if (limit_hit_) {
        return;
    }
    apply_declarations(parsed, resolve_font_size);
}

bool SaxDecoder::selector_matches(Selector const &selector) const
{
    if (selector.parts.empty() || stack_.empty()) {
        return false;
    }
    std::size_t index = stack_.size();
    if (!compound_matches(selector.parts.back().compound, stack_[index - 1].info)) {
        return false;
    }
    --index;
    for (std::size_t part = selector.parts.size() - 1; part-- > 0;) {
        if (selector.parts[part + 1].child_combinator) {
            if (index == 0) {
                return false;
            }
            --index;
            if (!compound_matches(selector.parts[part].compound, stack_[index].info)) {
                return false;
            }
        } else {
            bool found = false;
            while (index > 0) {
                --index;
                if (compound_matches(selector.parts[part].compound, stack_[index].info)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
    }
    return true;
}

void SaxDecoder::apply_stylesheet_rules(ElementInfo const &info, double resolve_font_size)
{
    if (rules_.rules.empty()) {
        return;
    }
    std::vector<std::size_t> candidates = rules_.candidates(info);
    std::stable_sort(candidates.begin(), candidates.end(), [this](std::size_t a, std::size_t b) {
        CssRule const &left = rules_.rules[a];
        CssRule const &right = rules_.rules[b];
        if (left.selector.specificity != right.selector.specificity) {
            return left.selector.specificity < right.selector.specificity;
        }
        return left.order < right.order;
    });
    for (auto index : candidates) {
        if (limit_hit_) {
            return;
        }
        CssRule const &rule = rules_.rules[index];
        if (!selector_matches(rule.selector)) {
            continue;
        }
        apply_declarations(rules_.declaration_pool[rule.declarations], resolve_font_size);
    }
}

void SaxDecoder::parse_stylesheet(std::string_view css)
{
    std::string const cleaned = strip_css_comments(css);
    std::size_t i = 0;
    while (i < cleaned.size()) {
        if (limit_hit_ || css_rejected_) {
            return;
        }
        while (i < cleaned.size() && g_ascii_isspace(cleaned[i])) {
            ++i;
        }
        if (i >= cleaned.size()) {
            break;
        }
        if (cleaned[i] == '@') {
            add_loss("at-rule-ignored");
            i = skip_at_rule(cleaned, i);
            continue;
        }
        std::size_t const brace = cleaned.find('{', i);
        if (brace == std::string::npos) {
            break;
        }
        std::size_t close = cleaned.find('}', brace + 1);
        if (close == std::string::npos) {
            close = cleaned.size();
        }
        std::string_view const selector_text(cleaned.data() + i, brace - i);
        std::string_view const declaration_text(cleaned.data() + brace + 1, close - brace - 1);
        i = close + 1;

        std::vector<Selector> selectors;
        for (auto piece : split_commas(selector_text)) {
            std::string_view const trimmed = trim(piece);
            if (trimmed.empty()) {
                continue;
            }
            if (trimmed.size() > MAX_SELECTOR_BYTES) {
                add_loss("selector-too-long");
                continue;
            }
            Selector selector;
            if (!parse_selector(trimmed, selector)) {
                add_loss("unsupported-selector");
                continue;
            }
            selectors.push_back(std::move(selector));
        }
        if (selectors.empty()) {
            continue;
        }

        Declarations declarations = parse_css_declarations(declaration_text);
        if (limit_hit_) {
            return;
        }
        if (declarations.empty()) {
            continue;
        }
        for (auto &selector : selectors) {
            if (++css_rules_ > limits_.max_css_rules) {
                trip("css-rules");
                return;
            }
            CssRule rule;
            rule.selector = selector;
            rule.order = rules_.rules.size();
            rules_.add(std::move(rule), declarations);
        }
    }
}

// --- text and fragment emission --------------------------------------------

void SaxDecoder::append_to_run(std::string_view bytes)
{
    if (bytes.empty() || limit_hit_) {
        return;
    }
    text_bytes_ += bytes.size();
    if (text_bytes_ > limits_.max_text_bytes) {
        trip("text-bytes");
        return;
    }
    run_text_.append(bytes.data(), bytes.size());
}

void SaxDecoder::append_text(std::string_view bytes)
{
    if (bytes.empty() || limit_hit_) {
        return;
    }
    bool const pre = white_space_is_pre();
    if (pre) {
        pending_space_ = false;
    } else if (pending_space_) {
        pending_space_ = false;
        if (paragraph_has_text()) {
            append_to_run(" ");
        }
    }
    append_to_run(bytes);
}

void SaxDecoder::process_text(std::string_view text)
{
    if (pending_bytes_.empty()) {
        process_utf8(text);
        return;
    }
    // A truncated sequence from the previous callback is completed by this one.
    std::string buffer;
    buffer.swap(pending_bytes_);
    buffer.append(text.data(), text.size());
    process_utf8(buffer);
}

void SaxDecoder::process_utf8(std::string_view text)
{
    bool const pre = white_space_is_pre();
    bool const pre_line = white_space_is_pre_line();
    auto line_break = [&]() {
        if (pre || pre_line) {
            hard_break();
        } else {
            pending_space_ = true;
        }
    };

    char const *p = text.data();
    char const *const end = p + text.size();
    while (p < end) {
        if (limit_hit_) {
            return;
        }
        gunichar const cp = g_utf8_get_char_validated(p, static_cast<gssize>(end - p));
        if (cp == static_cast<gunichar>(-2)) {
            // Truncated sequence at the end of this chunk: keep it for the next one.
            if (static_cast<std::size_t>(end - p) <= 4) {
                pending_bytes_.assign(p, static_cast<std::size_t>(end - p));
            }
            return;
        }
        if (cp == static_cast<gunichar>(-1)) {
            ++p; // invalid byte inside pre-validated input: drop defensively
            continue;
        }
        std::size_t const advance = static_cast<std::size_t>(g_utf8_next_char(p) - p);
        p += advance;

        if (pending_cr_) {
            pending_cr_ = false;
            if (cp == '\n') {
                continue; // CRLF is one line break
            }
        }
        if (cp == '\r') {
            pending_cr_ = true;
            line_break();
            continue;
        }
        if (cp == '\n' || cp == 0x2028 || cp == 0x2029 || cp == 0x85) {
            line_break();
            continue;
        }

        if (cp == '\t' || cp == ' ' || cp == 0x0C) {
            if (pre) {
                char const literal = cp == '\t' ? '\t' : ' ';
                append_to_run(std::string_view(&literal, 1));
            } else {
                pending_space_ = true;
            }
            continue;
        }
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
            continue; // C0/C1 controls and DEL are dropped
        }

        char encoded[8];
        int const length = g_unichar_to_utf8(cp, encoded);
        append_text(std::string_view(encoded, static_cast<std::size_t>(length)));
    }
}

void SaxDecoder::append_run_piece(std::string const &style, std::string piece)
{
    if (!current_.runs.empty() && current_.runs.back().style == style &&
        current_.runs.back().text.size() + piece.size() <= limits_.max_run_bytes) {
        current_.runs.back().text += piece;
        return;
    }
    if (runs_ >= limits_.max_runs) {
        trip("runs");
        return;
    }
    ++runs_;
    Run run;
    run.style = style;
    run.text = std::move(piece);
    current_.runs.push_back(std::move(run));
}

void SaxDecoder::emit_run(std::string const &style, std::string const &text)
{
    std::size_t const cap = std::max<std::size_t>(1, limits_.max_run_bytes);
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = std::min(text.size(), begin + cap);
        if (end < text.size()) {
            while (end > begin && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                --end;
            }
            if (end == begin) {
                // A single character longer than the cap is emitted whole, never cut.
                end = begin + 1;
                while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                    ++end;
                }
            }
        }
        append_run_piece(style, text.substr(begin, end - begin));
        if (limit_hit_) {
            return;
        }
        begin = end;
    }
}

void SaxDecoder::flush_run()
{
    if (run_text_.empty()) {
        return;
    }
    std::string text;
    text.swap(run_text_);
    emit_run(current_char_style(), text);
}

void SaxDecoder::block_break()
{
    flush_run();
    if (limit_hit_) {
        return;
    }
    if (!current_.runs.empty()) {
        current_.style = current_paragraph_style();
        paragraphs_.push_back(std::move(current_));
        current_ = Paragraph{};
        if (paragraphs_.size() > limits_.max_paragraphs) {
            trip("paragraphs");
        }
    }
    pending_space_ = false;
}

void SaxDecoder::hard_break()
{
    flush_run();
    if (limit_hit_) {
        return;
    }
    current_.style = current_paragraph_style();
    paragraphs_.push_back(std::move(current_));
    current_ = Paragraph{};
    pending_space_ = false;
    if (paragraphs_.size() > limits_.max_paragraphs) {
        trip("paragraphs");
    }
}

void SaxDecoder::finish()
{
    if (limit_hit_) {
        return;
    }
    pending_bytes_.clear(); // a trailing incomplete sequence is dropped, never guessed
    flush_run();
    if (limit_hit_) {
        return;
    }
    if (!current_.runs.empty()) {
        current_.style = current_paragraph_style();
        paragraphs_.push_back(std::move(current_));
        current_ = Paragraph{};
        if (paragraphs_.size() > limits_.max_paragraphs) {
            trip("paragraphs");
        }
    }
}

void SaxDecoder::push_frame(Frame frame)
{
    frame.saved = ctx_;
    stack_.push_back(std::move(frame));
    if (stack_.back().suppress) {
        ++suppress_depth_;
    }
}

// --- SAX entry points -------------------------------------------------------

void SaxDecoder::start_element(xmlChar const *name, xmlChar const **atts)
{
    if (limit_hit_) {
        return;
    }
    ++nodes_;
    if (nodes_ > limits_.max_nodes) {
        trip("nodes");
        return;
    }
    if (stack_.size() >= limits_.max_depth) {
        trip("depth");
        return;
    }

    std::string const tag = to_lower(reinterpret_cast<char const *>(name));

    ElementInfo info;
    info.tag = tag;
    std::vector<std::pair<std::string, std::string>> attributes;
    if (atts) {
        for (std::size_t i = 0; atts[i]; i += 2) {
            std::string attr_name = to_lower(reinterpret_cast<char const *>(atts[i]));
            std::string attr_value =
                atts[i + 1] ? std::string(reinterpret_cast<char const *>(atts[i + 1])) : std::string();
            if (attr_name == "class") {
                std::size_t pos = 0;
                while (pos < attr_value.size()) {
                    while (pos < attr_value.size() && g_ascii_isspace(attr_value[pos])) {
                        ++pos;
                    }
                    std::size_t const begin = pos;
                    while (pos < attr_value.size() && !g_ascii_isspace(attr_value[pos])) {
                        ++pos;
                    }
                    if (pos > begin) {
                        info.classes.push_back(attr_value.substr(begin, pos - begin));
                    }
                }
            } else if (attr_name == "id") {
                info.id = attr_value;
            }
            attributes.emplace_back(std::move(attr_name), std::move(attr_value));
        }
    }
    auto attribute = [&attributes](std::string_view wanted) -> std::string const * {
        for (auto const &entry : attributes) {
            if (entry.first == wanted) {
                return &entry.second;
            }
        }
        return nullptr;
    };

    bool const suppressed = suppress_depth_ > 0;

    // Void elements never own a frame: libxml2 emits no matching end for them.
    if (is_void_element(tag)) {
        if (suppressed) {
            return;
        }
        if (tag == "br") {
            hard_break();
        } else if (tag == "hr") {
            block_break();
        } else if (tag == "img") {
            add_loss("no-image-alt-text");
        }
        return;
    }

    flush_run();
    if (limit_hit_) {
        return;
    }

    if (is_skip_element(tag) || suppressed) {
        Frame frame;
        frame.tag = tag;
        frame.suppress = true;
        frame.in_suppressed_subtree = true;
        frame.info = std::move(info);
        if (tag == "style") {
            frame.style_element = true;
            style_index_ = static_cast<long>(stack_.size());
            css_buffer_.clear();
        }
        push_frame(std::move(frame));
        return;
    }

    bool const is_block = is_block_element(tag);
    bool const breaks_here = is_block && !is_table_wrapper(tag) && !is_table_cell(tag);

    if (breaks_here) {
        block_break();
        if (limit_hit_) {
            return;
        }
    }

    Frame frame;
    frame.tag = tag;
    frame.info = std::move(info);
    push_frame(std::move(frame));

    double const parent_font_size = stack_.back().saved.font_size_px;

    // 1. presentational attributes (lowest priority)
    Declarations presentational;
    if (auto const *value = attribute("align")) {
        std::string const lower = to_lower(trim(*value));
        if (lower == "left" || lower == "right" || lower == "center" || lower == "justify") {
            presentational.push_back({"text-align", lower});
        }
    }
    if (auto const *value = attribute("face")) {
        presentational.push_back({"font-family", *value});
    }
    if (auto const *value = attribute("color")) {
        presentational.push_back({"color", *value});
    }
    if (auto const *value = attribute("white-space")) {
        presentational.push_back({"white-space", *value});
    }
    apply_declarations(presentational, parent_font_size);

    // 2. semantic element mappings (same priority as presentational attributes)
    Declarations semantic;
    if (tag == "b" || tag == "strong") {
        semantic.push_back({"font-weight", "bold"});
    }
    if (tag == "i" || tag == "em") {
        semantic.push_back({"font-style", "italic"});
    }
    if (tag == "u" || tag == "ins") {
        semantic.push_back({"text-decoration", "underline"});
    }
    if (tag == "s" || tag == "strike" || tag == "del") {
        semantic.push_back({"text-decoration", "line-through"});
    }
    if (tag == "sub") {
        semantic.push_back({"baseline-shift", "sub"});
    }
    if (tag == "sup") {
        semantic.push_back({"baseline-shift", "super"});
    }
    if (tag == "code" || tag == "kbd" || tag == "samp" || tag == "tt") {
        semantic.push_back({"font-family", "monospace"});
    }
    if (tag == "pre") {
        semantic.push_back({"white-space", "pre"});
    }
    apply_declarations(semantic, parent_font_size);

    // 3. stylesheet rules, lowest specificity first
    apply_stylesheet_rules(stack_.back().info, parent_font_size);
    if (limit_hit_) {
        return;
    }

    // 4. inline style (highest priority)
    if (auto const *value = attribute("style")) {
        apply_inline_style(*value, parent_font_size);
        if (limit_hit_) {
            return;
        }
    }

    if (tag == "ul" || tag == "ol") {
        ListState state;
        state.ordered = tag == "ol";
        if (auto const *value = attribute("start")) {
            long parsed = 0;
            // The pre-existing zero guard is kept: start="0" leaves the default
            // numbering in place.  Out-of-range literals are ignored the same way.
            if (parse_list_counter(*value, parsed) && parsed != 0) {
                state.counter = parsed;
            }
        }
        lists_.push_back(state);
        stack_.back().list_element = true;
    } else if (tag == "li") {
        if (!lists_.empty() && lists_.size() <= MAX_LIST_MARKER_DEPTH) {
            ListState &state = lists_.back();
            if (auto const *value = attribute("value")) {
                long parsed = 0;
                // An out-of-range value is ignored; the item then continues the
                // enclosing list's numbering instead of adopting a hostile value.
                if (parse_list_counter(*value, parsed)) {
                    state.counter = parsed;
                }
            }
            std::string marker;
            if (state.ordered) {
                marker = std::to_string(state.counter);
                marker += ". ";
                // Saturate instead of incrementing past the bound: the counter is
                // always within +/-MAX_LIST_COUNTER here, so ++ can never overflow.
                if (state.counter < MAX_LIST_COUNTER) {
                    ++state.counter;
                }
            } else {
                static constexpr char const *BULLETS[] = {"\u2022 ", "\u25E6 ", "\u25AA "};
                marker = BULLETS[(lists_.size() - 1) % 3];
            }
            append_to_run(marker);
        }
    } else if (is_table_cell(tag)) {
        if (paragraph_has_text()) {
            pending_space_ = false;
            append_to_run("\t");
        }
    }

    if (tag == "font" && attribute("size")) {
        add_loss("legacy-font-size-ignored");
    }
}

void SaxDecoder::end_element(xmlChar const *name)
{
    if (limit_hit_) {
        return;
    }
    std::string const tag = to_lower(reinterpret_cast<char const *>(name));

    std::size_t index = stack_.size();
    for (std::size_t i = stack_.size(); i-- > 0;) {
        if (stack_[i].tag == tag) {
            index = i;
            break;
        }
    }
    if (index == stack_.size()) {
        return; // void element, or markup recovery already closed it
    }

    bool const inside_suppressed = stack_[index].in_suppressed_subtree;
    bool const breaks_here =
        !inside_suppressed && is_block_element(tag) && !is_table_wrapper(tag) && !is_table_cell(tag);

    flush_run();
    if (limit_hit_) {
        return;
    }
    if (breaks_here) {
        block_break();
        if (limit_hit_) {
            return;
        }
    }

    // Restore the context saved before this element and unwind every frame above it.
    ctx_ = stack_[index].saved;
    for (std::size_t i = stack_.size(); i-- > index;) {
        Frame &frame = stack_[i];
        if (frame.style_element && style_index_ == static_cast<long>(i)) {
            style_index_ = -1;
            if (!css_buffer_.empty()) {
                std::string css;
                css.swap(css_buffer_);
                parse_stylesheet(css);
            }
        }
        if (frame.list_element && !lists_.empty()) {
            lists_.pop_back();
        }
        if (frame.suppress && suppress_depth_ > 0) {
            --suppress_depth_;
        }
    }
    stack_.resize(index);
}

void SaxDecoder::characters(xmlChar const *ch, int len)
{
    if (limit_hit_ || len <= 0) {
        return;
    }
    std::string_view const text(reinterpret_cast<char const *>(ch), static_cast<std::size_t>(len));
    if (style_index_ >= 0) {
        if (css_bytes_ + text.size() > limits_.max_css_bytes) {
            trip("css-bytes");
            return;
        }
        css_bytes_ += text.size();
        css_buffer_.append(text.data(), text.size());
        return;
    }
    if (suppress_depth_ > 0) {
        return;
    }
    process_text(text);
}

// --- SAX trampolines --------------------------------------------------------

void sax_start_element(void *user_data, xmlChar const *name, xmlChar const **atts)
{
    static_cast<SaxDecoder *>(user_data)->start_element(name, atts);
}

void sax_end_element(void *user_data, xmlChar const *name)
{
    static_cast<SaxDecoder *>(user_data)->end_element(name);
}

void sax_characters(void *user_data, xmlChar const *ch, int len)
{
    static_cast<SaxDecoder *>(user_data)->characters(ch, len);
}

// ---------------------------------------------------------------------------
// Document parse (REPORT.md 5)
// ---------------------------------------------------------------------------

struct ParseOutcome {
    bool limit_exceeded = false;
    bool css_rejected = false;
    std::string error;
    std::string css_error;
    std::vector<Paragraph> paragraphs;
    std::vector<std::string> losses;
};

ParseOutcome parse_document(std::string_view document, Limits const &limits)
{
    ParseOutcome outcome;
    SaxDecoder decoder(limits);

    xmlSAXHandler sax;
    std::memset(&sax, 0, sizeof sax);
    sax.startElement = &sax_start_element;
    sax.endElement = &sax_end_element;
    sax.characters = &sax_characters;
    sax.cdataBlock = &sax_characters;

    htmlParserCtxtPtr ctxt =
        htmlCreatePushParserCtxt(&sax, &decoder, nullptr, 0, "clipboard", XML_CHAR_ENCODING_UTF8);
    if (!ctxt) {
        outcome.error = "libxml2 HTML parser context unavailable";
        return outcome;
    }
    decoder.attach(ctxt);
    htmlCtxtUseOptions(ctxt, HTML_PARSE_NONET | HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);

    std::size_t offset = 0;
    while (offset < document.size() && !decoder.stopped()) {
        auto const chunk = static_cast<int>(std::min<std::size_t>(PARSE_CHUNK_BYTES, document.size() - offset));
        htmlParseChunk(ctxt, document.data() + offset, chunk, 0);
        offset += static_cast<std::size_t>(chunk);
    }
    if (!decoder.stopped()) {
        htmlParseChunk(ctxt, nullptr, 0, 1); // terminate
        decoder.finish();
    }

    outcome.limit_exceeded = decoder.limit_hit();
    outcome.css_rejected = decoder.css_rejected();
    outcome.error = decoder.error();
    outcome.css_error = decoder.css_error();
    outcome.paragraphs = decoder.take_paragraphs();
    outcome.losses = decoder.take_losses();
    htmlFreeParserCtxt(ctxt);
    return outcome;
}

bool is_cap_rejection(std::string_view error)
{
    return error == "too many paragraphs" || error == "too many runs" || error == "too many characters";
}

void decode_into(std::string_view bytes, Limits const &limits, Result &result)
{
    if (bytes.size() > limits.max_input_bytes) {
        result.status = Status::limit_exceeded;
        result.error = "input exceeds max_input_bytes";
        return;
    }

    std::size_t const bom = has_utf8_bom(bytes) ? 3 : 0;
    std::string_view const payload = bytes.substr(bom);

    if (payload.find('\0') != std::string_view::npos) {
        result.status = Status::malformed;
        result.error = "payload contains a NUL byte";
        return;
    }
    if (!g_utf8_validate(payload.data(), static_cast<gssize>(payload.size()), nullptr)) {
        result.status = Status::malformed;
        result.error = "payload is not valid UTF-8";
        return;
    }

    Envelope const envelope = unwrap_html_envelope(bytes);
    if (!envelope.valid) {
        result.status = Status::malformed;
        result.error = envelope.cf_html ? "invalid CF_HTML envelope" : "no HTML content";
        return;
    }
    result.consumed_envelope_bytes = envelope.cf_html ? envelope.begin : 0;

    std::string_view const document = bytes.substr(envelope.begin, envelope.end - envelope.begin);
    if (!charset_is_supported(document)) {
        result.status = Status::malformed;
        result.error = "unsupported charset declaration";
        return;
    }

    ParseOutcome outcome = parse_document(document, limits);
    result.losses = std::move(outcome.losses);
    if (outcome.limit_exceeded) {
        result.status = Status::limit_exceeded;
        result.error = outcome.error.empty() ? "limit exceeded" : outcome.error;
        return;
    }
    if (outcome.css_rejected) {
        // Hostile or unparsable CSS: never hand it to libcroco and never keep a
        // partial rich fragment. `malformed` (not `limit_exceeded`) is what makes
        // the caller run the complete plain alternative instead of aborting the
        // whole paste.
        result.status = Status::malformed;
        result.error = outcome.css_error.empty() ? "css declaration rejected" : outcome.css_error;
        result.fragment = Fragment{};
        result.has_meaningful_styles = false;
        return;
    }

    // Leading and trailing empty paragraphs are dropped; interior ones produced
    // by explicit line breaks are kept.
    std::vector<Paragraph> &paragraphs = outcome.paragraphs;
    std::size_t first = 0;
    std::size_t last = paragraphs.size();
    while (first < last && paragraphs[first].runs.empty()) {
        ++first;
    }
    while (last > first && paragraphs[last - 1].runs.empty()) {
        --last;
    }
    std::vector<Paragraph> trimmed(paragraphs.begin() + static_cast<std::ptrdiff_t>(first),
                                   paragraphs.begin() + static_cast<std::ptrdiff_t>(last));
    if (trimmed.empty()) {
        result.status = Status::malformed;
        result.error = "no usable text";
        return;
    }

    TextPaste::BuildResult built = TextPaste::build_fragment(std::move(trimmed));
    if (built.status != TextPaste::BuildStatus::ok) {
        result.status = is_cap_rejection(built.error) ? Status::limit_exceeded : Status::malformed;
        result.error = built.error.empty() ? "fragment rejected" : built.error;
        return;
    }
    if (built.fragment.empty()) {
        result.status = Status::malformed;
        result.error = "empty fragment";
        return;
    }

    result.fragment = std::move(built.fragment);
    result.has_meaningful_styles = TextPaste::has_meaningful_styles(result.fragment);
    result.status = Status::ok;
}

} // namespace

Result decode(std::string_view bytes, Limits const &limits)
{
    auto const started = std::chrono::steady_clock::now();
    Result result;
    try {
        decode_into(bytes, limits, result);
    } catch (...) {
        result.status = Status::malformed;
        result.error = "decoder exception";
        result.fragment = Fragment{};
        result.has_meaningful_styles = false;
    }
    result.decode_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    return result;
}

Result decode_representation(std::string_view bytes, std::string_view mime_alias, Limits const &limits)
{
    Result result = decode(bytes, limits);
    if (!mime_alias.empty() && result.losses.size() < MAX_DIAGNOSTICS) {
        // Diagnostics only: the alias never selects a code path; content is sniffed.
        std::string code = "alias:";
        code += to_lower(mime_alias.substr(0, std::min<std::size_t>(mime_alias.size(), 96)));
        if (code.size() > MAX_DIAGNOSTIC_BYTES) {
            code.resize(MAX_DIAGNOSTIC_BYTES);
        }
        result.losses.push_back(std::move(code));
    }
    return result;
}

} // namespace Inkscape::UI::TextPaste::HtmlImport
