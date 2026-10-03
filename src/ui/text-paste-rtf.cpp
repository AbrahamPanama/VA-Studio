// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Bounded RTF -> TextPaste::Fragment importer (external clipboard input).
 *
 * Dedicated, single-pass, byte-oriented parser. The design follows
 * internal evidence notes section 3:
 *
 *  - a byte lexer (groups, control words <= 32 letters, control symbols,
 *    \'hh) that never interprets anything inside a \binN payload;
 *  - a copy-on-push group stack carrying the character/paragraph state, the
 *    \uc fallback count, the current destination and the suppression flag;
 *  - bounded \fonttbl / \colortbl tables, code-page resolution through GLib
 *    g_convert (with an explicit CP1252 fallback and UTF-16/32 refusal),
 *    \u surrogate pairing with \uc fallback consumption;
 *  - the paragraph/character mapping tables of REPORT.md 3.7/3.8 and the
 *    destination/lists/tables rules of 3.10/3.11;
 *  - every cap of REPORT.md 3.13 as a hard over_limit (never a truncated
 *    prefix) or a diagnostic where the report declares a loss;
 *  - a documented 16-bit magnitude bound on every RTF parameter that becomes
 *    an SVG/CSS length (\fs, \up/\dn, \expnd/\expndtw, \fi, \sl, \kerning), so
 *    malformed clipboard data can never reach layout as an astronomical (or
 *    invalid negative) length.
 *
 * Only GLib is used (g_convert, g_utf8_get_char_validated and, through
 * text-paste.h, g_utf8_validate/g_ascii_formatd). There is no file, network,
 * process or embedded-object access and no locale, clock or environment
 * input: the same bytes and Limits always produce the same Fragment and
 * counters.
 *
 * The only fragment a caller may use is the one produced by
 * TextPaste::build_fragment(), so the UTF-8/NUL/control/style/numeric
 * validation of the native wire format cannot be bypassed here.
 */

#include "ui/text-paste-rtf.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>

#include "ui/text-paste.h"

namespace Inkscape::UI::TextPaste::Rtf {

namespace {

constexpr gunichar U_REPLACEMENT = 0xFFFD;

/** Longest font name the report keeps (REPORT.md 3.4 / 4.13). */
constexpr std::size_t MAX_FONT_NAME_BYTES = 255;

bool is_ascii_alpha(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_ascii_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

bool is_hex_digit(unsigned char c)
{
    return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    return (c >= 'a' ? c - 'a' : c - 'A') + 10;
}

std::string to_lower_ascii(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char const c : s) {
        out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return out;
}

void append_utf8(std::string &out, gunichar cp)
{
    char buf[7];
    int const n = g_unichar_to_utf8(cp, buf);
    out.append(buf, static_cast<std::size_t>(n));
}

std::string replacement_utf8()
{
    std::string s;
    append_utf8(s, U_REPLACEMENT);
    return s;
}

std::size_t count_codepoints(std::string_view s)
{
    std::size_t n = 0;
    char const *p = s.data();
    char const *const end = p + s.size();
    while (p < end) {
        p = g_utf8_next_char(p);
        ++n;
    }
    return n;
}

/** Windows-1252 mapping for 0x80..0x9F; undefined slots keep their C1 point. */
gunichar cp1252_code_point(unsigned char b)
{
    static constexpr gunichar table[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    };
    return table[b - 0x80];
}

char const *const HEX_DIGITS = "0123456789abcdef";

std::string hex_color(unsigned r, unsigned g, unsigned b)
{
    std::string out = "#";
    unsigned const parts[3] = { r, g, b };
    for (unsigned const v : parts) {
        out += HEX_DIGITS[(v >> 4) & 0xFu];
        out += HEX_DIGITS[v & 0xFu];
    }
    return out;
}

/**
 * Canonical CSS declaration list builder. Anything that would exceed the
 * native style/value bounds of text-paste.h is dropped as a whole declaration
 * (the text is always kept); the caller commits the drop count only when the
 * style is actually used, so a speculative build cannot inflate diagnostics.
 */
struct StyleBuilder
{
    std::string out;
    std::size_t dropped = 0;

    void add(std::string_view name, std::string_view value)
    {
        if (value.empty() || name.empty()) {
            return;
        }
        if (value.size() > TextPaste::MAX_STYLE_VALUE_LENGTH) {
            ++dropped;
            return;
        }
        if (out.size() + name.size() + value.size() + 2 > TextPaste::MAX_STYLE_LENGTH) {
            ++dropped;
            return;
        }
        out.append(name);
        out.push_back(':');
        out.append(value);
        out.push_back(';');
    }

    std::string take() { return std::move(out); }
};

/** A built style plus the declarations the bounds dropped while building it. */
struct BuiltStyle
{
    std::string css;
    std::size_t dropped = 0;
};

std::string format_px(double px)
{
    return TextPaste::detail::format_length(px) + "px";
}

/** Whole points (1/72 in) -> document-space CSS px (96 px/inch). */
double pt_to_px(double pt)
{
    return pt * (96.0 / 72.0);
}

/** Twips (1/20 pt) -> px. */
double twips_to_px(double twips)
{
    return twips / 20.0 * (96.0 / 72.0);
}

/**
 * Largest magnitude accepted for an RTF parameter that becomes an SVG/CSS
 * length: \fs and \up/\dn (half-points), \expnd (quarter-points), \expndtw,
 * \fi and \sl (twips) and \kerning (half-points).
 *
 * The lexer returns the whole signed 32-bit parameter range, but RTF writers
 * store those fields as signed 16-bit values, so 32767 is the largest
 * magnitude that is still a layout request rather than malformed clipboard
 * data. Without this bound \fs2000000000 becomes font-size:1333333333px and
 * the same shape applies to the other controls; such a finite but
 * astronomical length would reach SVG layout. An out-of-bound parameter is
 * rejected as a whole declaration (never clamped, so no metric changes
 * silently and no non-finite or overflowed value is produced).
 */
constexpr std::int32_t MAX_LENGTH_PARAM = 32767;

/** True when an RTF length parameter is inside the documented magnitude bound. */
bool length_param_in_range(std::int32_t p)
{
    return p >= -MAX_LENGTH_PARAM && p <= MAX_LENGTH_PARAM;
}

struct FontEntry
{
    int index = -1;
    std::string name;
    int charset = -1; ///< \fcharsetN, -1 when absent
    int cpg = -1;     ///< \cpgN, -1 when absent
};

struct ColorEntry
{
    bool has_color = false;
    unsigned r = 0;
    unsigned g = 0;
    unsigned b = 0;
};

enum class Align { Left, Center, Right, Justify };
enum class Underline { None, Single, Dotted, Dashed, Double, Wavy };

struct CharState
{
    int font = 0;
    bool size_valid = true;
    int size_half = 24; ///< half-points; RTF default 24 = 12 pt = 16 px
    bool bold = false;
    bool italic = false;
    Underline underline = Underline::None;
    bool strike = false;
    bool caps = false;
    bool scaps = false;
    bool hidden = false;
    int baseline = 0;       ///< 0 none, 1 super, 2 sub, 3 explicit (\up/\dn)
    double baseline_px = 0;
    int color = 0;          ///< \cfN index; 0 = auto
    bool ls_valid = false;  ///< letter-spacing set
    double ls_px = 0;
    int ls_rank = 0;        ///< 0 none, 1 \expnd, 2 \expndtw (wins)
    bool kern_valid = false;
    double kern_px = 0;
    bool kern_none = false;

    bool operator==(CharState const &) const = default;
};

struct ParaState
{
    Align align = Align::Left;
    bool indent_valid = false;
    double indent_px = 0;
    bool sl_valid = false;
    int sl_twips = 0;
    bool sl_mult = false;
    bool in_table = false;
};

/** Copy-on-push group snapshot (REPORT.md 3.3). */
struct Frame
{
    CharState ch;
    ParaState par;
    int uc = 1;
    bool suppressed = false;
    bool in_object = false;
    bool in_result = false;
    int table_mode = 0; ///< 0 body, 1 \fonttbl, 2 \colortbl
    bool star_pending = false;
    FontEntry cur_font; ///< font entry accumulated so far
    bool font_entry_open = false;
    ColorEntry cur_color; ///< colour entry accumulated so far
};

struct ControlToken
{
    bool is_word = false;
    std::string name;
    bool has_param = false;
    std::int32_t param = 0;
    char symbol = 0;
    unsigned char byte = 0; ///< \'hh payload
};

/** Destinations the decoder recognises and skips (REPORT.md 3.10). */
bool is_skip_destination(std::string_view w)
{
    static constexpr std::string_view names[] = {
        "pict", "nonshppict", "shppict",
        "objdata", "objclass", "objname", "objalias", "objsect", "objtime",
        "objhtml", "objocx",
        "datastore", "themedata", "colorschememapping", "latentstyles",
        "stylesheet", "listtable", "listoverridetable", "rsidtbl",
        "info", "filetbl", "revtbl", "xmlnstbl", "generator",
        "mmath", "footnote", "annotation",
        "header", "footer", "headerl", "headerr", "headerf",
        "footerl", "footerr", "footerf",
        "private", "fldinst", "ud", "bkmkstart", "bkmkend",
    };
    for (auto const candidate : names) {
        if (candidate == w) {
            return true;
        }
    }
    return false;
}

/** \fcharsetN -> code page (REPORT.md 3.5 step 2); 0 = document code page. */
int charset_codepage(int charset)
{
    switch (charset) {
        case 77: return 10000;  // MacRoman
        case 128: return 932;   // Shift-JIS
        case 129: return 949;   // UHC
        case 130: return 949;   // Johab: CP1361 is unavailable in iconv, UHC is the fallback
        case 134: return 936;   // GBK
        case 136: return 950;   // Big5
        case 161: return 1253;  // Greek
        case 162: return 1254;  // Turkish
        case 163: return 1258;  // Vietnamese
        case 177: return 1255;  // Hebrew
        case 178: return 1256;  // Arabic
        case 186: return 1257;  // Baltic
        case 204: return 1251;  // Cyrillic
        case 222: return 874;   // Thai
        case 238: return 1250;  // Central European
        case 254: return 437;   // OEM US
        case 255: return 850;   // OEM multilingual
        default: return 0;
    }
}

bool is_unsafe_codepage(int cpg)
{
    return cpg == 1200 || cpg == 1201 || cpg == 12000 || cpg == 12001;
}

std::string collapse_whitespace(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;
    for (unsigned char const c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out += ' ';
            pending_space = false;
        }
        out += static_cast<char>(c);
    }
    return out;
}

/** Font name -> safe CSS font-family value, quoted unless it is a bare word. */
std::string font_family_value(std::string const &name)
{
    std::string clean;
    clean.reserve(name.size());
    for (unsigned char const c : name) {
        if (c < 0x20 || c == 0x7f) {
            continue;
        }
        switch (static_cast<char>(c)) {
            case '\\': case '{': case '}': case ';': case '@':
            case '!': case '<': case '>': case '&':
                continue;
            default:
                break;
        }
        clean += static_cast<char>(c);
    }
    std::size_t begin = 0;
    std::size_t end = clean.size();
    while (begin < end && clean[begin] == ' ') {
        ++begin;
    }
    while (end > begin && clean[end - 1] == ' ') {
        --end;
    }
    clean = clean.substr(begin, end - begin);
    if (clean.empty()) {
        return {};
    }
    bool simple = true;
    for (unsigned char const c : clean) {
        if (!(is_ascii_alpha(c) || is_ascii_digit(c) || c == '_' || c == '-')) {
            simple = false;
            break;
        }
    }
    if (simple) {
        return clean;
    }
    return "'" + clean + "'";
}

/**
 * Single-pass RTF decoder. All state is local to one decode() call, so two
 * calls with the same bytes and Limits are byte-identical.
 */
struct Decoder
{
    std::string_view in;
    Limits limits;
    Diagnostics diag;
    Status status = Status::ok;
    std::string error;

    std::size_t pos = 0;
    std::size_t depth = 0;
    bool header_seen = false;
    bool root_closed = false;

    CharState ch;
    ParaState par;
    int uc = 1;
    bool suppressed = false;
    bool star_pending = false;
    bool in_object = false;
    bool in_result = false;
    int table_mode = 0;
    std::vector<Frame> stack;

    int deff = 0;
    int doc_charset = 0; ///< 0 \ansi, 1 \mac, 2 \pc, 3 \pca
    int doc_cpg = -1;    ///< \ansicpgN, -1 when unset

    FontEntry cur_font;
    bool font_entry_open = false;
    ColorEntry cur_color;
    std::map<int, FontEntry> fonts;
    std::vector<ColorEntry> colors;
    std::map<std::string, bool> codepage_usable; ///< cached g_convert capability probe

    std::vector<unsigned char> pending_bytes;
    unsigned pending_high = 0; ///< pending high surrogate code unit

    std::vector<Paragraph> paragraphs;
    std::vector<Run> cur_runs;
    std::string cur_run_text;
    std::string cur_run_style;
    CharState cur_run_state; ///< character state the current run was opened with
    std::size_t run_count = 0;
    std::size_t text_bytes = 0;
    std::size_t split_run_count = 0;
    std::size_t uc_skip = 0;

    Fragment fragment;

    Decoder(std::string_view rtf, Limits const &lim)
        : in(rtf)
        , limits(lim)
    {}

    // ---------------------------------------------------------------- status

    void fail_malformed(std::string reason)
    {
        if (status == Status::ok) {
            status = Status::malformed;
            error = std::move(reason);
        }
    }

    void fail_limit(std::string reason)
    {
        if (status == Status::ok) {
            status = Status::over_limit;
            error = std::move(reason);
        }
    }

    Result make_result()
    {
        Result result;
        result.status = status;
        result.diagnostics = diag;
        result.error = error;
        if (status == Status::ok) {
            result.fragment = std::move(fragment);
            result.has_meaningful_styles = TextPaste::has_meaningful_styles(result.fragment);
        }
        return result;
    }

    // --------------------------------------------------------------- driver

    Result run()
    {
        if (in.size() > limits.max_input_bytes) {
            fail_limit("input exceeds the byte cap");
            return make_result();
        }

        while (pos < in.size() && status == Status::ok && !root_closed) {
            if (uc_skip > 0) {
                consume_fallback_char();
                continue;
            }
            unsigned char const c = static_cast<unsigned char>(in[pos]);
            if (c == '{') {
                open_group();
                continue;
            }
            if (c == '}') {
                close_group();
                continue;
            }
            if (c == '\\') {
                parse_control();
                continue;
            }
            if (!header_seen) {
                if (c == ' ' || c == '\r' || c == '\n' || c == 0) {
                    ++pos;
                    continue;
                }
                fail_malformed("missing \\rtf header");
                break;
            }
            if (table_mode == 1) {
                font_table_byte(c);
                continue;
            }
            if (table_mode == 2) {
                color_table_byte(c);
                continue;
            }
            star_pending = false;
            if (c == '\r' || c == '\n' || c == 0) {
                ++pos;
                continue;
            }
            if (discarding()) {
                ++pos;
                continue;
            }
            if (c == '\t') {
                emit_special("\t");
                ++pos;
                continue;
            }
            if (c >= 0x80) {
                // Non-ASCII body bytes are code-page bytes (REPORT.md 3.2).
                pending_bytes.push_back(c);
                ++pos;
                continue;
            }
            emit_ascii(c);
            ++pos;
        }

        if (status != Status::ok) {
            return make_result();
        }
        if (!header_seen) {
            fail_malformed("missing \\rtf header");
            return make_result();
        }
        if (!root_closed || depth != 0) {
            fail_malformed("unbalanced braces");
            return make_result();
        }
        // Real pasteboards append a NUL or a line break after the outermost
        // group; any other trailing byte is unbalanced input (REPORT.md 3.2).
        while (pos < in.size()) {
            unsigned char const trailing = static_cast<unsigned char>(in[pos]);
            if (trailing == 0 || trailing == '\r' || trailing == '\n' || trailing == ' ' ||
                trailing == '\t') {
                ++pos;
                continue;
            }
            fail_malformed("trailing data after the outermost group");
            return make_result();
        }
        finish_fragment();
        return make_result();
    }

    bool discarding() const
    {
        return suppressed || (in_object && !in_result);
    }

    // --------------------------------------------------------------- groups

    void open_group()
    {
        if (depth + 1 > limits.max_depth) {
            fail_limit("group depth exceeds the cap");
            return;
        }
        if (diag.groups_visited >= limits.max_groups) {
            fail_limit("visited groups exceed the cap");
            return;
        }
        if (!header_seen && depth > 0) {
            fail_malformed("missing \\rtf header");
            return;
        }
        ++diag.groups_visited;
        flush_bytes();
        if (status != Status::ok) {
            return;
        }
        ++pos;
        Frame frame;
        frame.ch = ch;
        frame.par = par;
        frame.uc = uc;
        frame.suppressed = suppressed;
        frame.in_object = in_object;
        frame.in_result = in_result;
        frame.table_mode = table_mode;
        frame.star_pending = star_pending;
        frame.cur_font = cur_font;
        frame.font_entry_open = font_entry_open;
        frame.cur_color = cur_color;
        stack.push_back(std::move(frame));
        ++depth;
    }

    void close_group()
    {
        if (depth == 0) {
            fail_malformed("unbalanced '}'");
            return;
        }
        flush_bytes();
        if (status != Status::ok) {
            return;
        }
        if (stack.empty()) {
            fail_malformed("group stack underflow");
            return;
        }
        Frame frame = std::move(stack.back());
        // Only the group that opened an entry commits it; a nested
        // {\*\panose}/\*\fname}/\*\falt} close must not (REPORT.md 3.4/3.12).
        if (table_mode == 1) {
            if (font_entry_open && !frame.font_entry_open) {
                commit_font_entry();
            }
        } else if (table_mode == 2) {
            if (cur_color.has_color && !frame.cur_color.has_color) {
                commit_color_entry();
            }
        }
        stack.pop_back();
        ch = frame.ch;
        par = frame.par;
        uc = frame.uc;
        suppressed = frame.suppressed;
        in_object = frame.in_object;
        in_result = frame.in_result;
        table_mode = frame.table_mode;
        star_pending = frame.star_pending;
        cur_font = std::move(frame.cur_font);
        font_entry_open = frame.font_entry_open;
        cur_color = frame.cur_color;
        --depth;
        ++pos;
        if (depth == 0) {
            root_closed = true;
        }
    }

    // ---------------------------------------------------------------- lexer

    bool lex_control(ControlToken &tok)
    {
        ++pos; // consume the backslash
        if (pos >= in.size()) {
            fail_malformed("trailing backslash");
            return false;
        }
        unsigned char const c = static_cast<unsigned char>(in[pos]);
        if (is_ascii_alpha(c)) {
            std::size_t const start = pos;
            while (pos < in.size() && is_ascii_alpha(static_cast<unsigned char>(in[pos]))) {
                ++pos;
            }
            if (pos - start > 32) {
                fail_malformed("control word too long");
                return false;
            }
            tok.is_word = true;
            tok.name = to_lower_ascii(in.substr(start, pos - start));
            if (pos < in.size() && (in[pos] == '-' || is_ascii_digit(static_cast<unsigned char>(in[pos])))) {
                bool const negative = in[pos] == '-';
                if (negative) {
                    ++pos;
                }
                std::size_t const first_digit = pos;
                while (pos < in.size() && is_ascii_digit(static_cast<unsigned char>(in[pos]))) {
                    ++pos;
                }
                std::size_t const digits = pos - first_digit;
                if (digits == 0) {
                    fail_malformed("control word parameter without digits");
                    return false;
                }
                if (digits > 10) {
                    fail_malformed("control word parameter too long");
                    return false;
                }
                long long value = 0;
                for (std::size_t i = first_digit; i < pos; ++i) {
                    value = value * 10 + (in[i] - '0');
                }
                if (negative) {
                    value = -value;
                }
                if (value < INT32_MIN || value > INT32_MAX) {
                    fail_malformed("control word parameter out of range");
                    return false;
                }
                tok.has_param = true;
                tok.param = static_cast<std::int32_t>(value);
            }
            if (pos < in.size() && in[pos] == ' ') {
                ++pos; // the space delimiter is consumed (REPORT.md 3.2)
            }
            return true;
        }

        ++pos; // consume the control symbol
        tok.is_word = false;
        tok.symbol = static_cast<char>(c);
        if (c == '\'') {
            if (in.size() - pos < 2 || !is_hex_digit(static_cast<unsigned char>(in[pos])) ||
                !is_hex_digit(static_cast<unsigned char>(in[pos + 1]))) {
                fail_malformed("bad \\'hh escape");
                return false;
            }
            tok.byte = static_cast<unsigned char>(hex_value(static_cast<unsigned char>(in[pos])) * 16 +
                                                  hex_value(static_cast<unsigned char>(in[pos + 1])));
            pos += 2;
        }
        return true;
    }

    void parse_control()
    {
        ControlToken tok;
        if (!lex_control(tok)) {
            return;
        }
        if (tok.is_word) {
            dispatch_word(tok);
        } else {
            dispatch_symbol(tok);
        }
    }

    /** \uN fallback consumption: one byte, one \'hh, or one control word. */
    void consume_fallback_char()
    {
        if (pos >= in.size()) {
            uc_skip = 0;
            return;
        }
        unsigned char const c = static_cast<unsigned char>(in[pos]);
        if (c == '{' || c == '}') {
            // A brace terminates the skip without consuming it (REPORT.md 3.6).
            uc_skip = 0;
            return;
        }
        if (c == '\\') {
            ControlToken tok;
            if (!lex_control(tok)) {
                return;
            }
            if (tok.is_word && tok.name == "bin" && !skip_bin(tok.has_param, tok.param)) {
                return;
            }
            if (uc_skip > 0) {
                --uc_skip;
            }
            return;
        }
        ++pos;
        if (c == '\r' || c == '\n' || c == 0) {
            return; // delimiters are not fallback characters
        }
        if (uc_skip > 0) {
            --uc_skip;
        }
    }

    bool skip_bin(bool has_param, std::int32_t param)
    {
        if (!has_param || param < 0) {
            fail_malformed("\\bin without a valid byte count");
            return false;
        }
        std::size_t const count = static_cast<std::size_t>(param);
        if (count > in.size() - pos) {
            fail_malformed("\\bin exceeds the remaining input");
            return false;
        }
        pos += count; // byte-exact: no braces, no escapes, no code page
        diag.skipped_bytes_bin += count;
        return true;
    }

    // ------------------------------------------------------------ dispatch

    void dispatch_word(ControlToken const &tok)
    {
        std::string_view const w = tok.name;
        bool const hp = tok.has_param;
        std::int32_t const p = tok.param;

        if (!header_seen) {
            if (depth == 0) {
                // \rtf must open the outermost group (REPORT.md 3.4).
                fail_malformed("missing \\rtf header");
                return;
            }
            if (w == "rtf" && (!hp || p <= 1)) {
                header_seen = true;
                return;
            }
            fail_malformed("missing \\rtf header");
            return;
        }

        if (w == "result") {
            // \object renders only its {\result ...} part (REPORT.md 3.10), so
            // this destination must be honoured while the object is discarding.
            in_result = true;
            return;
        }

        if (discarding()) {
            if (star_pending) {
                star_pending = false;
                if (w == "pict") {
                    ++diag.pictures_skipped;
                } else if (w == "object") {
                    ++diag.objects_skipped;
                }
                if (w == "bin") {
                    skip_bin(hp, p);
                }
                return;
            }
            if (w == "bin") {
                skip_bin(hp, p);
            }
            return;
        }

        if (star_pending) {
            star_pending = false;
            return; // \* already suppressed this group
        }

        if (w == "bin") {
            skip_bin(hp, p);
            return;
        }
        if (table_mode == 1) {
            dispatch_font_table(w, hp, p);
            return;
        }
        if (table_mode == 2) {
            dispatch_color_table(w, hp, p);
            return;
        }
        dispatch_body(w, hp, p);
    }

    void dispatch_symbol(ControlToken const &tok)
    {
        if (!header_seen) {
            fail_malformed("missing \\rtf header");
            return;
        }
        if (tok.symbol == '*') {
            if (!discarding()) {
                suppressed = true;
                ++diag.skipped_destinations;
                star_pending = true;
            }
            return;
        }
        if (discarding()) {
            star_pending = false;
            return;
        }
        if (star_pending) {
            star_pending = false;
            return;
        }
        if (table_mode == 1) {
            if (tok.symbol == '\'') {
                pending_bytes.push_back(tok.byte);
            }
            return;
        }
        if (table_mode == 2) {
            return;
        }
        switch (tok.symbol) {
            case '\\': emit_special("\\"); break;
            case '{': emit_special("{"); break;
            case '}': emit_special("}"); break;
            case '~': emit_special("\xC2\xA0"); break;         // non-breaking space
            case '-': emit_special("\xC2\xAD"); break;         // optional hyphen
            case '_': emit_special("\xE2\x80\x91"); break;     // non-breaking hyphen
            case '\'': pending_bytes.push_back(tok.byte); break;
            case '\r':
            case '\n': paragraph_break(false, false, false); break; // \ + CR/LF == \par
            case '|':
            case ':': break; // ignored control symbols
            default: break;  // unknown symbols are ignored, text is kept
        }
    }

    // -------------------------------------------------------- header tables

    void dispatch_font_table(std::string_view w, bool hp, std::int32_t p)
    {
        flush_bytes(); // pending \'hh bytes belong to the entry text so far
        if (w == "f") {
            if (hp && p >= 0) {
                if (font_entry_open && cur_font.index >= 0) {
                    commit_font_entry();
                    cur_font = FontEntry{};
                }
                cur_font.index = p;
                font_entry_open = true;
            }
            return;
        }
        if (w == "fcharset") {
            if (hp) {
                cur_font.charset = p;
            }
            return;
        }
        if (w == "cpg") {
            if (hp) {
                cur_font.cpg = p;
            }
            return;
        }
        // Family flags (\fnil \froman ...), \fprqN, \ftnil, \fttruetype and any
        // other word are not needed for the family name.
    }

    void commit_font_entry()
    {
        if (!font_entry_open) {
            return;
        }
        font_entry_open = false;
        FontEntry entry = std::move(cur_font);
        cur_font = FontEntry{};
        entry.name = collapse_whitespace(entry.name);
        if (entry.index < 0 || entry.name.empty()) {
            ++diag.dropped_style_declarations;
            return;
        }
        if (entry.name.size() > MAX_FONT_NAME_BYTES) {
            ++diag.truncated_font_names;
            entry.name.clear(); // entry stays for numbering (REPORT.md 3.4)
        }
        if (fonts.find(entry.index) == fonts.end() && fonts.size() >= limits.max_fonts) {
            ++diag.dropped_style_declarations;
            return;
        }
        fonts[entry.index] = std::move(entry);
    }

    void dispatch_color_table(std::string_view w, bool hp, std::int32_t p)
    {
        if (!hp) {
            return;
        }
        int const value = std::clamp(p, 0, 255);
        if (w == "red") {
            cur_color.r = static_cast<unsigned>(value);
            cur_color.has_color = true;
        } else if (w == "green") {
            cur_color.g = static_cast<unsigned>(value);
            cur_color.has_color = true;
        } else if (w == "blue") {
            cur_color.b = static_cast<unsigned>(value);
            cur_color.has_color = true;
        }
    }

    void commit_color_entry()
    {
        ColorEntry const entry = cur_color;
        cur_color = ColorEntry{};
        if (colors.size() >= limits.max_colors) {
            ++diag.dropped_style_declarations;
            return;
        }
        colors.push_back(entry);
    }

    void font_table_byte(unsigned char c)
    {
        ++pos;
        if (discarding()) {
            return; // a suppressed entry sub-group never commits the entry
        }
        if (c == ';') {
            flush_bytes(); // pending \'hh bytes belong to the name being closed
            commit_font_entry();
            return;
        }
        if (c == '\r' || c == '\n' || c == 0) {
            return;
        }
        if (c >= 0x80) {
            pending_bytes.push_back(c);
            return;
        }
        char const buf[1] = { static_cast<char>(c) };
        emit_text_bytes(std::string_view(buf, 1));
    }

    void color_table_byte(unsigned char c)
    {
        ++pos;
        if (discarding()) {
            return;
        }
        if (c == ';') {
            commit_color_entry();
        }
        // Any other byte in a colour table is insignificant.
    }

    // -------------------------------------------------------------- body

    void dispatch_body(std::string_view w, bool hp, std::int32_t p)
    {
        // A control word ends any accumulated \'hh run: those bytes belong to
        // the state in force before it (for example before a \fN switch).
        flush_bytes();
        if (status != Status::ok) {
            return;
        }

        // --- destinations ---------------------------------------------------
        if (is_skip_destination(w)) {
            suppressed = true;
            ++diag.skipped_destinations;
            if (w == "pict") {
                ++diag.pictures_skipped;
            }
            return;
        }
        if (w == "object") {
            ++diag.objects_skipped;
            in_object = true;
            in_result = false;
            return;
        }
        if (w == "result") {
            in_result = true;
            return;
        }
        if (w == "fonttbl") {
            table_mode = 1;
            cur_font = FontEntry{};
            font_entry_open = false;
            return;
        }
        if (w == "colortbl") {
            table_mode = 2;
            cur_color = ColorEntry{};
            colors.clear();
            return;
        }
        if (w == "pntext") {
            ++diag.list_paragraphs;
            return;
        }
        if (w == "field" || w == "fldrslt" || w == "upr") {
            return; // transparent wrappers
        }

        // --- document header ------------------------------------------------
        if (w == "ansi") { doc_charset = 0; return; }
        if (w == "mac") { doc_charset = 1; return; }
        if (w == "pc") { doc_charset = 2; return; }
        if (w == "pca") { doc_charset = 3; return; }
        if (w == "ansicpg") {
            if (hp) {
                doc_cpg = p;
            }
            return;
        }
        if (w == "deff") {
            if (hp && p >= 0) {
                deff = p;
                ch.font = p;
            }
            return;
        }

        // --- character controls (REPORT.md 3.7) -----------------------------
        if (w == "f") {
            if (hp && p >= 0) {
                ch.font = p;
            }
            return;
        }
        if (w == "fs") {
            // \fs is half-points; 0, a missing parameter and anything past the
            // documented length bound leave the size unknown (no declaration).
            if (hp && p > 0 && p <= MAX_LENGTH_PARAM) {
                ch.size_valid = true;
                ch.size_half = p;
            } else {
                ch.size_valid = false;
                ++diag.dropped_style_declarations;
            }
            return;
        }
        if (w == "b") { ch.bold = !hp || p != 0; return; }
        if (w == "i") { ch.italic = !hp || p != 0; return; }
        if (w == "ul") {
            ch.underline = (!hp || p != 0) ? Underline::Single : Underline::None;
            return;
        }
        if (w == "ulw") { ch.underline = Underline::Single; return; }
        if (w == "ulth") { ch.underline = Underline::Single; ++diag.unsupported_controls; return; }
        if (w == "ulnone") { ch.underline = Underline::None; return; }
        if (w == "uld") { ch.underline = Underline::Dotted; ++diag.unsupported_controls; return; }
        if (w == "uldash") { ch.underline = Underline::Dashed; ++diag.unsupported_controls; return; }
        if (w == "uldashd") { ch.underline = Underline::Dashed; ++diag.unsupported_controls; return; }
        if (w == "uldashdd") { ch.underline = Underline::Dashed; ++diag.unsupported_controls; return; }
        if (w == "uldb") { ch.underline = Underline::Double; return; }
        if (w == "ulwave") { ch.underline = Underline::Wavy; return; }
        if (w == "strike") { ch.strike = !hp || p != 0; return; }
        if (w == "strikedl") { ch.strike = true; ++diag.unsupported_controls; return; }
        if (w == "caps") { ch.caps = !hp || p != 0; return; }
        if (w == "scaps") { ch.scaps = !hp || p != 0; return; }
        if (w == "v") { ch.hidden = !hp || p != 0; return; }
        if (w == "super") { ch.baseline = 1; return; }
        if (w == "sub") { ch.baseline = 2; return; }
        if (w == "nosupersub") { ch.baseline = 0; return; }
        if (w == "up") {
            // Half-points; a non-positive value is "no shift" (not a loss),
            // past the bound it is malformed and the shift is dropped.
            if (hp && p > 0 && p <= MAX_LENGTH_PARAM) {
                ch.baseline = 3;
                ch.baseline_px = pt_to_px(p / 2.0);
            } else {
                if (hp && p > MAX_LENGTH_PARAM) {
                    ++diag.dropped_style_declarations;
                }
                ch.baseline = 0;
            }
            return;
        }
        if (w == "dn") {
            if (hp && p > 0 && p <= MAX_LENGTH_PARAM) {
                ch.baseline = 3;
                ch.baseline_px = -pt_to_px(p / 2.0);
            } else {
                if (hp && p > MAX_LENGTH_PARAM) {
                    ++diag.dropped_style_declarations;
                }
                ch.baseline = 0;
            }
            return;
        }
        if (w == "cf") {
            // RTF 1.5: the cf/cb index is positional from 0 and entry 0 is the
            // colour table's "auto" entry. 0 (and a missing/negative parameter)
            // therefore means "no colour", which build_char_style() emits as no
            // declaration at all.
            ch.color = (hp && p >= 0) ? p : 0;
            return;
        }
        if (w == "expnd") {
            // Quarter-points; negative condenses, so only the magnitude is
            // bounded. An out-of-bound value changes no state and emits no
            // declaration (a previously accepted spacing is left in force).
            if (hp && ch.ls_rank < 2) {
                if (length_param_in_range(p)) {
                    ch.ls_rank = 1;
                    ch.ls_valid = true;
                    ch.ls_px = pt_to_px(p / 4.0);
                } else {
                    ++diag.dropped_style_declarations;
                }
            }
            return;
        }
        if (w == "expndtw") {
            // Twips; \expndtw outranks \expnd regardless of order.
            if (hp) {
                if (length_param_in_range(p)) {
                    ch.ls_rank = 2;
                    ch.ls_valid = true;
                    ch.ls_px = twips_to_px(p);
                } else {
                    ++diag.dropped_style_declarations;
                }
            }
            return;
        }
        if (w == "kerning") {
            // Half-points; 0 and any invalid/out-of-bound parameter mean the
            // kerning threshold is unusable, so kerning is turned off (a
            // keyword declaration, never a length).
            if (hp && p > 0 && p <= MAX_LENGTH_PARAM) {
                ch.kern_valid = true;
                ch.kern_px = pt_to_px(p / 2.0);
                ch.kern_none = false;
            } else {
                if (hp && p > MAX_LENGTH_PARAM) {
                    ++diag.dropped_style_declarations;
                }
                ch.kern_valid = false;
                ch.kern_none = true;
            }
            return;
        }
        if (w == "plain") { reset_char_state(); return; }
        if (w == "cs") {
            if (hp) {
                ++diag.character_style_refs;
            }
            ++diag.unsupported_controls;
            return;
        }
        if (w == "charscalex" || w == "outl" || w == "shad" || w == "embo" || w == "impr" ||
            w == "animtext" || w == "lang" || w == "rtlch" || w == "ltrch") {
            ++diag.unsupported_controls;
            return;
        }
        if (w == "highlight" || w == "cb" || w == "chcbpat" || w == "chshdng") {
            ++diag.unsupported_controls; // no background property in the fragment
            return;
        }
        if (w == "u") { handle_unicode(hp, p); return; }
        if (w == "uc") { handle_uc(hp, p); return; }

        // --- paragraph controls (REPORT.md 3.8) -----------------------------
        if (w == "pard") { par = ParaState{}; return; }
        if (w == "ql") { par.align = Align::Left; return; }
        if (w == "qc") { par.align = Align::Center; return; }
        if (w == "qr") { par.align = Align::Right; return; }
        if (w == "qj") { par.align = Align::Justify; return; }
        if (w == "fi") {
            // Twips; negative first-line indents are hanging indents and stay
            // valid, so only the magnitude is bounded.
            if (hp && p != 0 && length_param_in_range(p)) {
                par.indent_valid = true;
                par.indent_px = twips_to_px(p);
            } else {
                if (hp && p != 0) {
                    ++diag.dropped_style_declarations;
                }
                par.indent_valid = false;
            }
            return;
        }
        if (w == "sl") {
            // Twips; \slmult0 is exact line spacing and \slmult1 makes N/240 a
            // multiple. A negative \sl is the RTF "at least" form: it cannot be
            // flattened to an exact CSS line-height without changing the layout
            // meaning, so it is dropped instead of emitted as a negative
            // (invalid) length.
            if (hp && p > 0 && p <= MAX_LENGTH_PARAM) {
                par.sl_valid = true;
                par.sl_twips = p;
            } else {
                if (hp && p != 0) {
                    ++diag.dropped_style_declarations;
                }
                par.sl_valid = false;
            }
            return;
        }
        if (w == "slmult") {
            if (hp) {
                par.sl_mult = p == 1;
            }
            return;
        }
        if (w == "intbl") { par.in_table = true; return; }
        if (w == "s") {
            if (hp) {
                ++diag.paragraph_style_refs;
            }
            return;
        }
        if (w == "li" || w == "ri" || w == "lin" || w == "rin" || w == "sb" || w == "sa" ||
            w == "lisb" || w == "lisa" || w == "tx" || w == "tb" || w == "tqc" || w == "tqr" ||
            w == "tqdec" || w == "keep" || w == "keepn" || w == "widctlpar" || w == "nowidctlpar" ||
            w == "hyphpar" || w == "outlinelevel" || w == "level" || w == "itap" || w == "ls" ||
            w == "ilvl" || w == "rtlpar" || w == "ltrpar") {
            return; // accepted, no representable declaration
        }

        // --- text and breaks (REPORT.md 3.9) --------------------------------
        if (w == "par") { paragraph_break(false, false, false); return; }
        if (w == "line") { paragraph_break(false, true, false); return; }
        if (w == "page") { paragraph_break(true, false, false); return; }
        if (w == "sect") { paragraph_break(true, false, false); return; }
        if (w == "column") { paragraph_break(true, false, false); return; }
        if (w == "row") {
            ++diag.table_rows;
            paragraph_break(false, false, true);
            return;
        }
        if (w == "cell") {
            ++diag.table_cells;
            emit_special("\t");
            return;
        }
        if (w == "tab") { emit_special("\t"); return; }
        if (w == "emdash") { emit_special("\xE2\x80\x94"); return; }
        if (w == "endash") { emit_special("\xE2\x80\x93"); return; }
        if (w == "emspace") { emit_special("\xE2\x80\x83"); return; }
        if (w == "enspace") { emit_special("\xE2\x80\x82"); return; }
        if (w == "bullet") { emit_special("\xE2\x80\xA2"); return; }
        if (w == "lquote") { emit_special("\xE2\x80\x98"); return; }
        if (w == "rquote") { emit_special("\xE2\x80\x99"); return; }
        if (w == "ldblquote") { emit_special("\xE2\x80\x9C"); return; }
        if (w == "rdblquote") { emit_special("\xE2\x80\x9D"); return; }
        if (w == "zwj") { emit_special("\xE2\x80\x8D"); return; }
        if (w == "zwnj") { emit_special("\xE2\x80\x8C"); return; }
        if (w == "chdate" || w == "chtime" || w == "chpgn" || w == "chftn" || w == "chatn" ||
            w == "sectnum" || w == "softpage" || w == "softline" || w == "softcol") {
            return; // recognised, emits nothing
        }

        // Any other control word is ignored and its text kept (REPORT.md 3.3).
    }

    void reset_char_state()
    {
        CharState fresh;
        fresh.font = deff;
        ch = fresh;
    }

    void handle_uc(bool hp, std::int32_t p)
    {
        if (!hp) {
            uc = 1;
            return;
        }
        if (p < 0 || p > 16) {
            ++diag.uc_clamped;
            uc = p < 0 ? 0 : 16;
            return;
        }
        uc = static_cast<int>(p);
    }

    void handle_unicode(bool hp, std::int32_t p)
    {
        // Pending \'hh bytes precede this escape in the input and must keep
        // their position in the run.
        flush_bytes();
        if (status != Status::ok) {
            return;
        }
        if (!hp) {
            emit_replacement(false);
            uc_skip = static_cast<std::size_t>(uc);
            return;
        }
        long long value = p;
        if (value < -32768 || value > 65535) {
            // An out-of-range Unicode value never rejects the document, and its
            // \uc ANSI fallback is still consumed (REPORT.md 3.6).
            emit_replacement(false);
            uc_skip = static_cast<std::size_t>(uc);
            return;
        }
        if (value < 0) {
            value += 65536;
        }
        unicode_unit(static_cast<unsigned>(value));
        uc_skip = static_cast<std::size_t>(uc);
    }

    void unicode_unit(unsigned unit)
    {
        if (pending_high != 0) {
            if (unit >= 0xDC00 && unit <= 0xDFFF) {
                gunichar const cp = 0x10000u + ((pending_high - 0xD800u) << 10) + (unit - 0xDC00u);
                pending_high = 0;
                ++diag.surrogate_pairs_combined;
                emit_codepoint(cp);
                return;
            }
            flush_pending_high();
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            pending_high = unit;
            return;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            emit_replacement(true);
            return;
        }
        emit_codepoint(unit);
    }

    // ------------------------------------------------------------ emission

    void emit_ascii(unsigned char c)
    {
        flush_bytes();
        if (status != Status::ok || discarding()) {
            return;
        }
        char const buf[1] = { static_cast<char>(c) };
        emit_text_bytes(std::string_view(buf, 1));
    }

    void emit_special(std::string_view utf8)
    {
        flush_bytes();
        if (status != Status::ok) {
            return;
        }
        emit_text_bytes(utf8);
    }

    void emit_codepoint(gunichar cp)
    {
        if (discarding()) {
            return;
        }
        flush_pending_high();
        if (status != Status::ok) {
            return;
        }
        if (cp != '\t' && (cp < 0x20 || cp == 0x7f)) {
            // A \u value that cannot appear in run text becomes U+FFFD; it must
            // never reject the whole document (REPORT.md 3.5/3.6).
            emit_replacement(false);
            return;
        }
        std::string utf8;
        append_utf8(utf8, cp);
        emit_text_direct(utf8);
    }

    void emit_replacement(bool surrogate)
    {
        ++diag.replacement_characters;
        if (surrogate) {
            ++diag.surrogate_replacements;
        }
        emit_text_bytes(replacement_utf8());
    }

    /** Flush a pending high surrogate as U+FFFD (REPORT.md 3.6). */
    void flush_pending_high()
    {
        if (pending_high == 0) {
            return;
        }
        pending_high = 0;
        ++diag.replacement_characters;
        ++diag.surrogate_replacements;
        emit_text_direct(replacement_utf8());
    }

    /** Sanitise a converted run: drop NUL/C0 (except TAB) and DEL, repair UTF-8. */
    std::string sanitize_controls(std::string_view utf8, bool count)
    {
        std::string out;
        out.reserve(utf8.size());
        char const *p = utf8.data();
        char const *const end = p + utf8.size();
        while (p < end) {
            gunichar const cp = g_utf8_get_char_validated(p, static_cast<gssize>(end - p));
            if (cp == static_cast<gunichar>(-1) || cp == static_cast<gunichar>(-2)) {
                if (count) {
                    ++diag.replacement_characters;
                }
                break;
            }
            char const *const next = g_utf8_next_char(p);
            if (cp == '\t' || (cp >= 0x20 && cp != 0x7f)) {
                out.append(p, static_cast<std::size_t>(next - p));
            } else if (count) {
                ++diag.replacement_characters;
            }
            p = next;
        }
        return out;
    }

    /** Text arriving from the document (already code-page converted). */
    void emit_text_bytes(std::string_view utf8)
    {
        if (discarding()) {
            return; // includes text inside a {\*\panose}/\*\fname} font entry
        }
        if (table_mode == 1) {
            cur_font.name.append(utf8);
            return;
        }
        if (table_mode == 2) {
            return;
        }
        flush_pending_high();
        if (status != Status::ok) {
            return;
        }
        emit_text_direct(sanitize_controls(utf8, false));
    }

    /** Text already sanitised; hidden text is counted but not emitted. */
    void emit_text_direct(std::string_view utf8)
    {
        if (utf8.empty() || status != Status::ok) {
            return;
        }
        if (ch.hidden) {
            diag.hidden_chars += count_codepoints(utf8);
            return;
        }
        append_run_text(utf8);
    }

    void append_run_text(std::string_view s)
    {
        if (s.empty() || status != Status::ok) {
            return;
        }
        if (text_bytes + s.size() > limits.max_text_bytes) {
            fail_limit("decoded text bytes exceed the cap");
            return;
        }
        // A run is one unchanged canonical style string; a style change closes
        // it (REPORT.md 3.9). The state comparison only avoids rebuilding the
        // style for every appended chunk.
        if (cur_run_text.empty()) {
            cur_run_state = ch;
            BuiltStyle built = build_char_style();
            diag.dropped_style_declarations += built.dropped;
            cur_run_style = std::move(built.css);
        } else if (!(cur_run_state == ch)) {
            BuiltStyle built = build_char_style();
            diag.dropped_style_declarations += built.dropped;
            if (built.css != cur_run_style) {
                flush_run();
                if (status != Status::ok) {
                    return;
                }
                cur_run_style = std::move(built.css);
            }
            cur_run_state = ch;
        }
        std::size_t const effective =
            std::max<std::size_t>(1, std::min(limits.max_run_bytes, TextPaste::MAX_RUN_LENGTH));
        std::size_t off = 0;
        while (off < s.size()) {
            // Length of the next UTF-8 sequence, so a split never cuts one.
            std::size_t length = 1;
            while (off + length < s.size() &&
                   (static_cast<unsigned char>(s[off + length]) & 0xC0) == 0x80) {
                ++length;
            }
            if (!cur_run_text.empty() && cur_run_text.size() + length > effective) {
                flush_run();
                if (status != Status::ok) {
                    return;
                }
                ++split_run_count;
                continue;
            }
            cur_run_text.append(s.data() + off, length);
            off += length;
        }
        text_bytes += s.size();
    }

    void flush_run()
    {
        if (cur_run_text.empty() || status != Status::ok) {
            return;
        }
        if (run_count >= limits.max_runs) {
            fail_limit("runs exceed the cap");
            return;
        }
        ++run_count;
        Run run;
        run.text = std::move(cur_run_text);
        run.style = cur_run_style; // kept for the next split of the same style
        cur_run_text.clear();
        cur_runs.push_back(std::move(run));
    }

    void strip_trailing_tab()
    {
        if (!cur_run_text.empty() && cur_run_text.back() == '\t') {
            cur_run_text.pop_back();
            return;
        }
        if (!cur_runs.empty() && !cur_runs.back().text.empty() && cur_runs.back().text.back() == '\t') {
            cur_runs.back().text.pop_back();
        }
    }

    void paragraph_break(bool layout, bool line, bool row)
    {
        if (discarding()) {
            return; // \par inside a skipped destination is data, not structure
        }
        flush_pending_high();
        if (status != Status::ok) {
            return;
        }
        flush_bytes();
        if (status != Status::ok) {
            return;
        }
        if (row) {
            strip_trailing_tab();
        }
        flush_run();
        if (status != Status::ok) {
            return;
        }
        if (paragraphs.size() >= limits.max_paragraphs) {
            fail_limit("paragraphs exceed the cap");
            return;
        }
        Paragraph paragraph;
        BuiltStyle built_style = build_para_style();
        diag.dropped_style_declarations += built_style.dropped;
        paragraph.style = std::move(built_style.css);
        paragraph.runs = std::move(cur_runs);
        cur_runs.clear();
        paragraphs.push_back(std::move(paragraph));
        if (layout) {
            ++diag.layout_breaks;
        }
        if (line) {
            ++diag.line_breaks_flattened;
        }
    }

    void finish_fragment()
    {
        flush_pending_high();
        if (status != Status::ok) {
            return;
        }
        flush_bytes();
        if (status != Status::ok) {
            return;
        }
        flush_run();
        if (status != Status::ok) {
            return;
        }
        if (!cur_runs.empty()) {
            if (paragraphs.size() >= limits.max_paragraphs) {
                fail_limit("paragraphs exceed the cap");
                return;
            }
            Paragraph paragraph;
            BuiltStyle built_style = build_para_style();
            diag.dropped_style_declarations += built_style.dropped;
            paragraph.style = std::move(built_style.css);
            paragraph.runs = std::move(cur_runs);
            cur_runs.clear();
            paragraphs.push_back(std::move(paragraph));
        }

        auto built = TextPaste::build_fragment(std::move(paragraphs));
        if (built.status != TextPaste::BuildStatus::ok) {
            if (built.error == "too many paragraphs" || built.error == "too many runs" ||
                built.error == "too many characters") {
                fail_limit(built.error);
            } else {
                fail_malformed("fragment rejected: " + built.error);
            }
            return;
        }
        diag.split_runs = split_run_count + built.split_runs;
        fragment = std::move(built.fragment);
    }

    // --------------------------------------------------------------- styles

    BuiltStyle build_char_style()
    {
        StyleBuilder builder;

        auto const font = fonts.find(ch.font);
        if (font != fonts.end() && !font->second.name.empty()) {
            std::string const family = font_family_value(font->second.name);
            if (!family.empty() && TextPaste::is_safe_style_value(family)) {
                builder.add("font-family", family);
            } else {
                ++builder.dropped;
            }
        } else {
            ++builder.dropped;
        }

        if (ch.size_valid) {
            builder.add("font-size", format_px(pt_to_px(ch.size_half / 2.0)));
        }
        if (ch.bold) {
            builder.add("font-weight", "bold");
        }
        if (ch.italic) {
            builder.add("font-style", "italic");
        }
        if (ch.scaps) {
            builder.add("font-variant-caps", "small-caps");
        }
        if (ch.caps) {
            builder.add("text-transform", "uppercase");
        }

        bool const underline = ch.underline != Underline::None;
        if (underline || ch.strike) {
            std::string decoration;
            if (underline) {
                decoration = "underline";
            }
            if (ch.strike) {
                if (!decoration.empty()) {
                    decoration += ' ';
                }
                decoration += "line-through";
            }
            builder.add("text-decoration", decoration);
            if (underline) {
                switch (ch.underline) {
                    case Underline::Dotted: builder.add("text-decoration-style", "dotted"); break;
                    case Underline::Dashed: builder.add("text-decoration-style", "dashed"); break;
                    case Underline::Double: builder.add("text-decoration-style", "double"); break;
                    case Underline::Wavy: builder.add("text-decoration-style", "wavy"); break;
                    case Underline::None:
                    case Underline::Single: break;
                }
            }
        }

        if (ch.baseline == 1) {
            builder.add("baseline-shift", "super");
        } else if (ch.baseline == 2) {
            builder.add("baseline-shift", "sub");
        } else if (ch.baseline == 3) {
            builder.add("baseline-shift", format_px(ch.baseline_px));
        }

        if (ch.color > 0) {
            // RTF 1.5: the colour table is positional from index 0 and entry 0
            // is the "auto" colour (the empty segment before the first ';' of
            // {\colortbl;...}), so \cf1 is colors[1] and \cf0 is auto (handled
            // by the > 0 guard above).
            std::size_t const index = static_cast<std::size_t>(ch.color);
            if (index < colors.size()) {
                // An entry that carries no \red\green\blue (the "auto" entry or
                // an empty segment) is intentionally colourless: no diagnostic.
                if (colors[index].has_color) {
                    builder.add("color", hex_color(colors[index].r, colors[index].g, colors[index].b));
                }
            } else {
                ++builder.dropped; // out of range (REPORT.md 3.7)
            }
        }

        if (ch.kern_none) {
            builder.add("font-kerning", "none");
        }
        if (ch.kern_valid) {
            builder.add("kerning", format_px(ch.kern_px));
        }
        if (ch.ls_valid) {
            builder.add("letter-spacing", format_px(ch.ls_px));
        }
        BuiltStyle built;
        built.dropped = builder.dropped;
        built.css = builder.take();
        return built;
    }

    BuiltStyle build_para_style()
    {
        StyleBuilder builder;
        switch (par.align) {
            case Align::Center: builder.add("text-align", "center"); break;
            case Align::Right: builder.add("text-align", "right"); break;
            case Align::Justify: builder.add("text-align", "justify"); break;
            case Align::Left: break;
        }
        if (par.indent_valid) {
            builder.add("text-indent", format_px(par.indent_px));
        }
        if (par.sl_valid) {
            if (par.sl_mult) {
                builder.add("line-height", TextPaste::detail::format_length(par.sl_twips / 240.0));
            } else {
                builder.add("line-height", format_px(twips_to_px(par.sl_twips)));
                ++diag.line_height_at_least_flattened;
            }
        }
        BuiltStyle built;
        built.dropped = builder.dropped;
        built.css = builder.take();
        return built;
    }

    // ------------------------------------------------------------ code pages

    int document_codepage() const
    {
        if (doc_cpg > 0) {
            return doc_cpg;
        }
        switch (doc_charset) {
            case 1: return 10000; // \mac
            case 2: return 437;   // \pc
            case 3: return 850;   // \pca
            default: return 1252; // \ansi or unspecified
        }
    }

    int resolve_codepage()
    {
        auto const font = fonts.find(ch.font);
        if (font == fonts.end()) {
            ++diag.fonts_referenced_before_table;
            return document_codepage();
        }
        if (font->second.cpg > 0) {
            return font->second.cpg;
        }
        if (font->second.charset > 0) {
            int const cpg = charset_codepage(font->second.charset);
            if (cpg > 0) {
                return cpg;
            }
        }
        return document_codepage();
    }

    std::string convert_bytes(std::vector<unsigned char> const &bytes)
    {
        if (bytes.empty()) {
            return {};
        }
        int const cpg = resolve_codepage();
        if (cpg == 65001) {
            // GLib rejects the CP65001 alias; the bytes are already UTF-8.
            return decode_utf8(bytes);
        }
        if (is_unsafe_codepage(cpg)) {
            // UTF-16/32: refuse, keep the document readable and never inject NUL.
            ++diag.unsafe_codepage;
            return decode_cp1252(bytes);
        }

        std::string const name = (cpg == 10000) ? "MACINTOSH" : ("CP" + std::to_string(cpg));
        if (!codepage_is_usable(name)) {
            // An unavailable code page keeps the document readable through the
            // total CP1252 map (REPORT.md 3.5 step 4).
            ++diag.codepage_fallbacks;
            return decode_cp1252(bytes);
        }

        gsize written = 0;
        gchar *converted = g_convert(reinterpret_cast<gchar const *>(bytes.data()),
                                     static_cast<gssize>(bytes.size()), "UTF-8", name.c_str(), nullptr,
                                     &written, nullptr);
        if (converted) {
            // The explicit length keeps a converted NUL from truncating the
            // rest of the byte run.
            std::string const out(converted, written);
            g_free(converted);
            return sanitize_controls(out, true);
        }

        // A partial multibyte sequence at the end of the byte run keeps the
        // convertible prefix (never a silent truncation of later text).
        for (std::size_t drop = 1; drop <= 3 && drop < bytes.size(); ++drop) {
            gsize prefix_written = 0;
            gchar *prefix = g_convert(reinterpret_cast<gchar const *>(bytes.data()),
                                      static_cast<gssize>(bytes.size() - drop), "UTF-8", name.c_str(),
                                      nullptr, &prefix_written, nullptr);
            if (prefix) {
                std::string out(prefix, prefix_written);
                g_free(prefix);
                out += replacement_utf8();
                ++diag.replacement_characters;
                return sanitize_controls(out, true);
            }
        }
        if (bytes.size() <= 3) {
            // The whole run is an incomplete sequence.
            ++diag.replacement_characters;
            return replacement_utf8();
        }

        // Undecodable sequence: the total CP1252 map keeps the text readable,
        // with C0 controls other than TAB dropped.
        ++diag.codepage_fallbacks;
        return decode_cp1252(bytes);
    }

    /** True when the code page converts a plain ASCII byte (cached g_convert probe). */
    bool codepage_is_usable(std::string const &name)
    {
        auto const cached = codepage_usable.find(name);
        if (cached != codepage_usable.end()) {
            return cached->second;
        }
        gchar *probe = g_convert("A", 1, "UTF-8", name.c_str(), nullptr, nullptr, nullptr);
        bool const usable = probe != nullptr;
        if (probe) {
            g_free(probe);
        }
        codepage_usable[name] = usable;
        return usable;
    }

    std::string decode_utf8(std::vector<unsigned char> const &bytes)
    {
        std::string out;
        out.reserve(bytes.size());
        std::size_t i = 0;
        while (i < bytes.size()) {
            char const *const p = reinterpret_cast<char const *>(bytes.data() + i);
            gssize const remaining = static_cast<gssize>(bytes.size() - i);
            gunichar const cp = g_utf8_get_char_validated(p, remaining);
            if (cp == static_cast<gunichar>(-1)) {
                ++diag.replacement_characters;
                append_utf8(out, U_REPLACEMENT);
                ++i;
                continue;
            }
            if (cp == static_cast<gunichar>(-2)) {
                ++diag.replacement_characters;
                append_utf8(out, U_REPLACEMENT);
                break; // incomplete sequence at the end
            }
            gsize const length = g_utf8_skip[static_cast<unsigned char>(*p)];
            if (length == 0 || i + length > bytes.size()) {
                ++diag.replacement_characters;
                append_utf8(out, U_REPLACEMENT);
                ++i;
                continue;
            }
            if (cp == '\t' || (cp >= 0x20 && cp != 0x7f)) {
                out.append(p, length);
            } else {
                ++diag.replacement_characters;
            }
            i += length;
        }
        return out;
    }

    std::string decode_cp1252(std::vector<unsigned char> const &bytes)
    {
        std::string out;
        out.reserve(bytes.size());
        for (unsigned char const b : bytes) {
            gunichar const cp = (b < 0x80) ? b : (b <= 0x9f ? cp1252_code_point(b) : b);
            if (cp == '\t') {
                out += '\t';
            } else if (cp < 0x20 || cp == 0x7f) {
                ++diag.replacement_characters; // a NUL or C0 control never reaches the text
            } else {
                append_utf8(out, cp);
            }
        }
        return out;
    }

    // -------------------------------------------------------------- bytes

    void flush_bytes()
    {
        if (pending_bytes.empty()) {
            return;
        }
        std::vector<unsigned char> bytes;
        bytes.swap(pending_bytes);
        if (table_mode == 2 || discarding()) {
            return;
        }
        std::string const text = convert_bytes(bytes);
        if (status != Status::ok || text.empty()) {
            return;
        }
        if (table_mode == 1) {
            cur_font.name += text;
            return;
        }
        emit_text_bytes(text);
    }
};

} // namespace

Result decode(std::string_view rtf, Limits const &limits)
{
    // A decoder bug or an allocation failure must never escape into the paste
    // action / GTK loop; the safe status is the same "malformed" the caller
    // already handles by falling back to the plain-text alternative
    // (HtmlImport::decode does the same).
    try {
        Decoder decoder(rtf, limits);
        return decoder.run();
    } catch (...) {
        Result result;
        result.status = Status::malformed;
        result.error = "decoder exception";
        return result;
    }
}

} // namespace Inkscape::UI::TextPaste::Rtf

// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
