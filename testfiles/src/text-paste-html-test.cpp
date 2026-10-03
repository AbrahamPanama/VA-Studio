// SPDX-License-Identifier: GPL-2.0-or-later
//
// Outcome-based tests for the bounded HTML -> TextPaste::Fragment decoder.
//
// Pure GTest: no GTK, no clipboard, no document, no network.  Every expected
// value is a literal derived from
// internal evidence notes sections 4-10 and
// FORMAT_CHOICE_TABLE.md section 4, never from the decoder's own output.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "ui/text-paste-html.h"

namespace {

using Inkscape::UI::TextPaste::Fragment;
using Inkscape::UI::TextPaste::HtmlImport::decode;
using Inkscape::UI::TextPaste::HtmlImport::decode_representation;
using Inkscape::UI::TextPaste::HtmlImport::Envelope;
using Inkscape::UI::TextPaste::HtmlImport::Limits;
using Inkscape::UI::TextPaste::HtmlImport::Result;
using Inkscape::UI::TextPaste::HtmlImport::Status;
using Inkscape::UI::TextPaste::HtmlImport::unwrap_html_envelope;
using Inkscape::UI::TextPaste::MAX_RUN_LENGTH;
using Inkscape::UI::TextPaste::sanitize_style;
using Inkscape::UI::TextPaste::serialize;

/** decode() that must succeed; yields a default Result on failure. */
Result decode_ok(std::string_view html)
{
    Result result = decode(html);
    EXPECT_EQ(result.status, Status::ok) << "unexpected status, error: " << result.error;
    return result;
}

std::string plain_of(std::string_view html)
{
    return decode_ok(html).fragment.plain;
}

std::string run_style_at(std::string_view html, std::size_t paragraph, std::size_t run)
{
    Result const result = decode_ok(html);
    if (result.fragment.paragraphs.size() <= paragraph ||
        result.fragment.paragraphs[paragraph].runs.size() <= run) {
        return "<missing run>";
    }
    return result.fragment.paragraphs[paragraph].runs[run].style;
}

std::string run_text_at(std::string_view html, std::size_t paragraph, std::size_t run)
{
    Result const result = decode_ok(html);
    if (result.fragment.paragraphs.size() <= paragraph ||
        result.fragment.paragraphs[paragraph].runs.size() <= run) {
        return "<missing run>";
    }
    return result.fragment.paragraphs[paragraph].runs[run].text;
}

std::string paragraph_style_at(std::string_view html, std::size_t paragraph)
{
    Result const result = decode_ok(html);
    if (result.fragment.paragraphs.size() <= paragraph) {
        return "<missing paragraph>";
    }
    return result.fragment.paragraphs[paragraph].style;
}

/** Every returned style must already be canonical and allow-listed. */
void expect_style_invariants(Result const &result)
{
    for (auto const &paragraph : result.fragment.paragraphs) {
        EXPECT_EQ(sanitize_style(paragraph.style, true), paragraph.style)
            << "paragraph style is not canonical: " << paragraph.style;
        for (auto const &run : paragraph.runs) {
            EXPECT_EQ(sanitize_style(run.style, false), run.style)
                << "run style is not canonical: " << run.style;
        }
    }
}

/** serialize() output must round-trip through the strict native parser. */
void expect_round_trip(Result const &result)
{
    ASSERT_EQ(result.status, Status::ok);
    std::string const wire = serialize(result.fragment);
    auto const parsed = Inkscape::UI::TextPaste::parse(wire);
    ASSERT_TRUE(parsed.has_value()) << "native parse rejected: " << wire;
    EXPECT_EQ(parsed->plain, result.fragment.plain);
    ASSERT_EQ(parsed->paragraphs.size(), result.fragment.paragraphs.size());
    for (std::size_t p = 0; p < parsed->paragraphs.size(); ++p) {
        EXPECT_EQ(parsed->paragraphs[p].style, result.fragment.paragraphs[p].style);
        ASSERT_EQ(parsed->paragraphs[p].runs.size(), result.fragment.paragraphs[p].runs.size());
        for (std::size_t r = 0; r < parsed->paragraphs[p].runs.size(); ++r) {
            EXPECT_EQ(parsed->paragraphs[p].runs[r].style, result.fragment.paragraphs[p].runs[r].style);
            EXPECT_EQ(parsed->paragraphs[p].runs[r].text, result.fragment.paragraphs[p].runs[r].text);
        }
    }
}

/**
 * True when a style value carries a token whose NUMERIC PREFIX is non-finite.
 *
 * The numeric prefix decides, so "1e999%" and "nan%" are caught even though
 * g_ascii_strtod() stops before the unit or '%'.  This is deliberately the
 * test-side oracle, independent of TextPaste::detail::style_numbers_are_finite().
 */
bool style_value_has_nonfinite_number(std::string_view value)
{
    std::size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (g_ascii_isspace(value[pos]) || value[pos] == ',')) {
            ++pos;
        }
        std::size_t const begin = pos;
        while (pos < value.size() && !g_ascii_isspace(value[pos]) && value[pos] != ',') {
            ++pos;
        }
        if (pos == begin) {
            continue;
        }
        std::string const token(value.substr(begin, pos - begin));
        char *end = nullptr;
        double const number = g_ascii_strtod(token.c_str(), &end);
        if (end != token.c_str() && !std::isfinite(number)) {
            return true;
        }
    }
    return false;
}

/** True when a canonical "name:value;" list declares @a name. */
bool style_has_declaration(std::string_view style, std::string_view name)
{
    std::size_t pos = 0;
    while (pos <= style.size()) {
        auto const semi = style.find(';', pos);
        auto const decl = style.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = semi == std::string_view::npos ? style.size() + 1 : semi + 1;
        auto const colon = decl.find(':');
        if (colon != std::string_view::npos && decl.substr(0, colon) == name) {
            return true;
        }
    }
    return false;
}

/** No run or paragraph style may carry a non-finite numeric value. */
void expect_no_nonfinite_numbers(Result const &result)
{
    auto check_style = [](std::string_view style) {
        std::size_t pos = 0;
        while (pos <= style.size()) {
            auto const semi = style.find(';', pos);
            auto const decl =
                style.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
            pos = semi == std::string_view::npos ? style.size() + 1 : semi + 1;
            auto const colon = decl.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            EXPECT_FALSE(style_value_has_nonfinite_number(decl.substr(colon + 1)))
                << "non-finite value imported: " << std::string(decl);
        }
    };
    for (auto const &paragraph : result.fragment.paragraphs) {
        check_style(paragraph.style);
        for (auto const &run : paragraph.runs) {
            check_style(run.style);
        }
    }
}

/** True when any returned run or paragraph style declares @a name. */
bool fragment_has_declaration(Result const &result, std::string_view name)
{
    for (auto const &paragraph : result.fragment.paragraphs) {
        if (style_has_declaration(paragraph.style, name)) {
            return true;
        }
        for (auto const &run : paragraph.runs) {
            if (style_has_declaration(run.style, name)) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Drop-semantics contract: the offending declaration is rejected but the rest
 * of the rich import survives -- no non-finite value reaches any fragment
 * style, the property is gone, and the text still decodes completely.
 */
void expect_nonfinite_declaration_dropped(std::string_view html, std::string_view property)
{
    Result const result = decode(html);
    ASSERT_EQ(result.status, Status::ok) << result.error;
    EXPECT_EQ(result.fragment.plain, "x");
    expect_no_nonfinite_numbers(result);
    EXPECT_FALSE(fragment_has_declaration(result, property))
        << std::string(property) << " survived the non-finite guard";
}

// ---------------------------------------------------------------------------
// CF_HTML fixture construction (FORMAT_CHOICE_TABLE.md section 4 fixture 8)
// ---------------------------------------------------------------------------

constexpr char const *CF_FRAGMENT = "<p>hi <b>there</b></p>";

std::string cf_body()
{
    return std::string("<html><body><!--StartFragment-->") + CF_FRAGMENT +
           "<!--EndFragment--></body></html>";
}

/** Build a valid CF_HTML envelope whose fragment is CF_FRAGMENT. */
std::string cf_payload(std::size_t *start_fragment_out = nullptr)
{
    std::string const body = cf_body();
    char probe[512];
    int const header_len = std::snprintf(
        probe, sizeof probe,
        "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n", 0,
        0, 0, 0);
    long long const start_html = header_len;
    long long const end_html = header_len + static_cast<long long>(body.size());
    std::size_t const fragment_at = body.find(CF_FRAGMENT);
    long long const start_fragment = header_len + static_cast<long long>(fragment_at);
    long long const end_fragment = start_fragment + static_cast<long long>(std::strlen(CF_FRAGMENT));
    if (start_fragment_out) {
        *start_fragment_out = static_cast<std::size_t>(start_fragment);
    }
    char header[512];
    std::snprintf(header, sizeof header,
                  "Version:0.9\r\nStartHTML:%010lld\r\nEndHTML:%010lld\r\n"
                  "StartFragment:%010lld\r\nEndFragment:%010lld\r\n",
                  start_html, end_html, start_fragment, end_fragment);
    return std::string(header) + body;
}

/** Replace the (fixed width) value of a header field with exactly 10 characters. */
bool patch_offset(std::string &payload, std::string const &field, std::string const &value)
{
    std::size_t const pos = payload.find(field + ":");
    if (pos == std::string::npos || value.size() != 10) {
        return false;
    }
    payload.replace(pos + field.size() + 1, 10, value);
    return true;
}

} // namespace

// ===========================================================================
// Fixture group 1: inline-style font matrix
// ===========================================================================

TEST(TextPasteHtml, InlineFontMatrixExactStyles)
{
    std::string const html =
        "<p style=\"font-family:Arial; font-size:12pt; font-weight:bold; font-style:italic; "
        "color:#ff0000; text-decoration:underline; line-height:1.5\">x</p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "x");
    ASSERT_EQ(result.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(result.fragment.paragraphs[0].runs.size(), 1u);
    // Canonical order follows the ui/text-paste.h allow-list.
    EXPECT_EQ(result.fragment.paragraphs[0].runs[0].style,
              "font-family:Arial;font-style:italic;font-weight:bold;font-size:16px;"
              "text-decoration:underline;color:#ff0000;");
    EXPECT_EQ(result.fragment.paragraphs[0].style, "line-height:1.5;");
    EXPECT_TRUE(result.has_meaningful_styles);
    expect_style_invariants(result);
    expect_round_trip(result);
}

TEST(TextPasteHtml, FontSizeUnitsNormalizeToCssPixelsOnce)
{
    // REPORT.md 7.3: px/pt/pc/in/mm/cm/q convert through absolute_unit_px() once,
    // `rem` resolves against the documented 16 px root and `em`/`%` against the
    // nearest ancestor font-size.  101.6q == 25.4mm == 1in, 1.5em/150% of 16px.
    EXPECT_EQ(run_style_at("<p style=\"font-size:20px\">x</p>", 0, 0), "font-size:20px;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:12pt\">x</p>", 0, 0), "font-size:16px;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:1pc\">x</p>", 0, 0), "font-size:16px;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:1in\">x</p>", 0, 0), "font-size:96px;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:25.4mm\">x</p>", 0, 0), "font-size:96px;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:2.54cm\">x</p>", 0, 0), "font-size:96px;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:101.6q\">x</p>", 0, 0), "font-size:96px;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:1.5em\">x</p>", 0, 0), "font-size:24px;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:150%\">x</p>", 0, 0), "font-size:24px;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:2rem\">x</p>", 0, 0), "font-size:32px;");
    // `rem` is root relative, not parent relative, and the stylesheet path
    // recovers the unit exactly like the inline path.
    EXPECT_EQ(run_style_at("<div style=\"font-size:40px\"><p style=\"font-size:2rem\">x</p></div>", 0, 0),
              "font-size:32px;");
    EXPECT_EQ(run_style_at("<style>p { font-size: 2rem }</style><p>x</p>", 0, 0), "font-size:32px;");
    EXPECT_EQ(run_style_at("<style>p { font-size: 101.6q }</style><p>x</p>", 0, 0), "font-size:96px;");
}

TEST(TextPasteHtml, UnsupportedLengthUnitsAndSizesAreDropped)
{
    // REPORT.md 7.3: ex/ch/vw/vh/vmin/vmax and unknown units are dropped (never
    // treated as unitless); a unitless, non-positive or non-finite size is
    // rejected instead of guessed.
    EXPECT_EQ(run_style_at("<p style=\"font-size:2ex\">x</p>", 0, 0), "");
    EXPECT_EQ(run_style_at("<p style=\"font-size:5vw\">x</p>", 0, 0), "");
    EXPECT_EQ(run_style_at("<p style=\"font-size:12\">x</p>", 0, 0), "");
    EXPECT_EQ(run_style_at("<p style=\"font-size:-4px\">x</p>", 0, 0), "");
    // The unknown unit must be dropped for non-font-size lengths too, not kept
    // as a unitless number.
    EXPECT_EQ(run_style_at("<p style=\"letter-spacing:2vw\">x</p>", 0, 0), "");
    EXPECT_EQ(run_style_at("<p style=\"word-spacing:3ch\">x</p>", 0, 0), "");
    EXPECT_EQ(run_style_at("<p style=\"font-size:2ex; letter-spacing:5vh\">x</p>", 0, 0), "");
    EXPECT_EQ(run_style_at("<p style=\"letter-spacing:2px; word-spacing:3pt\">x</p>", 0, 0),
              "letter-spacing:2px;word-spacing:4px;");
    // A declaration that cannot be converted is dropped without losing the others.
    EXPECT_EQ(run_style_at("<p style=\"font-size:3ex; color:red\">x</p>", 0, 0), "color:red;");
}

TEST(TextPasteHtml, RelativeFontSizeInheritsFromNearestAncestor)
{
    // 1em of a 20px parent is 20px; the grandchild keeps the resolved context.
    EXPECT_EQ(run_style_at("<div style=\"font-size:20px\"><p style=\"font-size:1em\">x</p></div>", 0, 0),
              "font-size:20px;");
    EXPECT_EQ(run_style_at("<div style=\"font-size:20px\"><span style=\"font-size:50%\">x</span></div>", 0, 0),
              "font-size:10px;");
}

// ===========================================================================
// Fixture group 2: semantic-only markup
// ===========================================================================

TEST(TextPasteHtml, SemanticElementsMapToAllowListedProperties)
{
    std::string const html =
        "<p><b>b</b><i>i</i><u>u</u><s>s</s><sub>sub</sub><sup>sup</sup><code>c</code></p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "biussubsupc");
    ASSERT_EQ(result.fragment.paragraphs.size(), 1u);
    std::vector<std::string> const expected_styles = {
        "font-weight:bold;",  "font-style:italic;",   "text-decoration:underline;",
        "text-decoration:line-through;", "baseline-shift:sub;", "baseline-shift:super;",
        "font-family:monospace;",
    };
    ASSERT_EQ(result.fragment.paragraphs[0].runs.size(), expected_styles.size());
    for (std::size_t i = 0; i < expected_styles.size(); ++i) {
        EXPECT_EQ(result.fragment.paragraphs[0].runs[i].style, expected_styles[i]);
    }
    EXPECT_TRUE(result.has_meaningful_styles);
    expect_style_invariants(result);
    expect_round_trip(result);
}

TEST(TextPasteHtml, SemanticTextDecorationDoesNotInherit)
{
    // text-decoration is not an inherited property (REPORT.md 7.1 item 5).
    std::string const html = "<u><s>x</s></u>";
    Result const result = decode_ok(html);
    ASSERT_EQ(result.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(result.fragment.paragraphs[0].runs.size(), 1u);
    EXPECT_EQ(result.fragment.paragraphs[0].runs[0].style, "text-decoration:line-through;");
}

TEST(TextPasteHtml, PresentationalAttributesMapToAllowListedProperties)
{
    std::string const html = "<p align=\"center\"><font face=\"Arial\" color=\"#123456\">x</font></p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "x");
    EXPECT_EQ(paragraph_style_at(html, 0), "text-align:center;");
    EXPECT_EQ(run_text_at(html, 0, 0), "x");
    EXPECT_EQ(run_style_at(html, 0, 0), "font-family:Arial;color:#123456;");
    expect_style_invariants(result);
    expect_round_trip(result);
}

TEST(TextPasteHtml, LegacyFontSizeIsDroppedAndReported)
{
    std::string const html = "<p><font size=\"5\">x</font></p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "x");
    EXPECT_EQ(run_style_at(html, 0, 0), "");
    bool saw_loss = false;
    for (auto const &loss : result.losses) {
        saw_loss = saw_loss || loss == "legacy-font-size-ignored";
    }
    EXPECT_TRUE(saw_loss);
}

// ===========================================================================
// Fixture group 3: entity table
// ===========================================================================

TEST(TextPasteHtml, EntitiesResolveFromTheBuiltInTable)
{
    std::string const html = "<p>a &amp; b &nbsp; &copy; &bogus; &#233; &#x1F600; c</p>";
    Result const result = decode_ok(html);
    // &nbsp; is preserved literally and never collapsed; &bogus; stays as written;
    // decimal and hexadecimal numeric references resolve.
    EXPECT_EQ(result.fragment.plain, "a & b \u00A0 \u00A9 &bogus; \u00E9 \U0001F600 c");
    EXPECT_FALSE(result.has_meaningful_styles);
}

TEST(TextPasteHtml, NonBreakingSpaceDoesNotCollapse)
{
    EXPECT_EQ(plain_of("<p>a&nbsp;&nbsp;b</p>"), "a\u00A0\u00A0b");
    EXPECT_EQ(plain_of("<p>a &nbsp;b</p>"), "a \u00A0b");
}

// ===========================================================================
// Fixture group 4: block and whitespace rules
// ===========================================================================

TEST(TextPasteHtml, NestedBlocksProduceNoPhantomBlankLines)
{
    EXPECT_EQ(plain_of("<div><p>a</p></div><div><p>b</p></div>"), "a\nb");
    EXPECT_EQ(plain_of("<p>a</p><p></p><p>b</p>"), "a\nb");
    EXPECT_EQ(plain_of("<div>a</div><div>b</div>"), "a\nb");
    EXPECT_EQ(plain_of("<div>a<span>b</span></div>"), "ab");
}

TEST(TextPasteHtml, ExplicitLineBreaksKeepInteriorEmptyParagraphs)
{
    EXPECT_EQ(plain_of("<p>a<br><br>b</p>"), "a\n\nb");
    EXPECT_EQ(plain_of("<p>a<br></p>"), "a");      // trailing empty paragraph dropped
    EXPECT_EQ(plain_of("<p><br>a</p>"), "a");      // leading empty paragraph dropped
    EXPECT_EQ(plain_of("<p>a<br>b</p>"), "a\nb");
    EXPECT_EQ(plain_of("<div>a<hr>b</div>"), "a\nb");
}

TEST(TextPasteHtml, WhitespaceCollapsesOutsidePreformattedContent)
{
    EXPECT_EQ(plain_of("<p>a   b\n  c</p>"), "a b c");
    EXPECT_EQ(plain_of("<p>  leading  </p>"), "leading");
    EXPECT_EQ(plain_of("<p>a\t\tb</p>"), "a b");
    EXPECT_EQ(plain_of("<div>a<span> </span>b</div>"), "a b");
    // U+2028 / U+2029 / U+0085 are line breaks; in normal mode they collapse to a space.
    EXPECT_EQ(plain_of("<p>a\u2028b</p>"), "a b");
    EXPECT_EQ(plain_of("<p>a\u2029b</p>"), "a b");
    EXPECT_EQ(plain_of("<p>a\u0085b</p>"), "a b");
}

TEST(TextPasteHtml, PreformattedContentKeepsTabsLinesAndSpaces)
{
    std::string const html = "<pre>a\tb\n  c</pre>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "a\tb\n  c");
    ASSERT_EQ(result.fragment.paragraphs.size(), 2u);
    EXPECT_EQ(result.fragment.paragraphs[0].runs[0].text, "a\tb");
    EXPECT_EQ(result.fragment.paragraphs[1].runs[0].text, "  c");
    EXPECT_EQ(result.fragment.paragraphs[0].style, "white-space:pre;");
    expect_style_invariants(result);
}

TEST(TextPasteHtml, PreLineKeepsBreaksAndCollapsesSpaces)
{
    std::string const html = "<p style=\"white-space:pre-line\">a   b\nc</p>";
    EXPECT_EQ(plain_of(html), "a b\nc");
}

// ===========================================================================
// Fixture group 5: lists and tables
// ===========================================================================

TEST(TextPasteHtml, UnorderedListsEmitLiteralMarkers)
{
    EXPECT_EQ(plain_of("<ul><li>a</li><li>b</li></ul>"), "\u2022 a\n\u2022 b");
    EXPECT_EQ(plain_of("<ul><li>a<ul><li>b</li></ul></li></ul>"), "\u2022 a\n\u25E6 b");
    EXPECT_EQ(plain_of("<ul><li>a<ul><li>b<ul><li>c</li></ul></li></ul></li></ul>"),
              "\u2022 a\n\u25E6 b\n\u25AA c");
}

TEST(TextPasteHtml, OrderedListsHonourStartAndValue)
{
    EXPECT_EQ(plain_of("<ol><li>a</li><li>b</li></ol>"), "1. a\n2. b");
    EXPECT_EQ(plain_of("<ol start=\"3\"><li>a</li><li value=\"7\">b</li><li>c</li></ol>"),
              "3. a\n7. b\n8. c");
}

TEST(TextPasteHtml, TablesUseOneParagraphPerRowAndTabSeparatedCells)
{
    std::string const html = "<table><tr><td>1</td><td>2</td></tr><tr><td>3</td><td></td></tr></table>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "1\t2\n3\t");
    ASSERT_EQ(result.fragment.paragraphs.size(), 2u);
    expect_style_invariants(result);
    expect_round_trip(result);
}

TEST(TextPasteHtml, EmptyCellsAndCaption)
{
    EXPECT_EQ(plain_of("<table><caption>cap</caption><tr><td>a</td><td>b</td></tr></table>"),
              "cap\na\tb");
}

// ===========================================================================
// Fixture group 6: skipped subtrees
// ===========================================================================

TEST(TextPasteHtml, ScriptAndStyleBodiesAreNeverEmittedAsText)
{
    std::string const html =
        "<p>one</p><script>var x = '<div>no</div>';</script><style>p{color:red}</style>"
        "<noscript>n</noscript><template>t</template><!-- <div>hidden</div> -->"
        "<svg><text>s</text></svg><iframe>f</iframe><img src=\"x\" alt=\"alt\"><p>two</p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "one\ntwo");
}

TEST(TextPasteHtml, ImageAltTextIsNotInvented)
{
    Result const result = decode_ok("<p>a<img src=\"x.png\" alt=\"description\">b</p>");
    EXPECT_EQ(result.fragment.plain, "ab");
    bool saw_loss = false;
    for (auto const &loss : result.losses) {
        saw_loss = saw_loss || loss == "no-image-alt-text";
    }
    EXPECT_TRUE(saw_loss);
}

TEST(TextPasteHtml, HeadAndTitleContributeNothing)
{
    std::string const html = "<html><head><title>t</title><meta charset=\"utf-8\"></head><body><p>x</p></body></html>";
    EXPECT_EQ(plain_of(html), "x");
}

// ===========================================================================
// Fixture group 7: CSS
// ===========================================================================

TEST(TextPasteHtml, StylesheetSelectorsCascadeBySpecificity)
{
    std::string const html =
        "<style>p { color: #00ff00 } .cls { font-weight: bold } #ident { font-style: italic } "
        "@media print { p { color: red } } @font-face { font-family: Evil } "
        ".broken: { color: blue }</style><p class=\"cls\" id=\"ident\">x</p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "x");
    EXPECT_EQ(run_style_at(html, 0, 0), "font-style:italic;font-weight:bold;color:#00ff00;");
    expect_style_invariants(result);
}

TEST(TextPasteHtml, DescendantAndChildCombinators)
{
    // REPORT.md 7.1 item 2: compounds joined by a descendant (whitespace) or a
    // child (">") combinator.  The span is a child of p and a descendant of div,
    // so "p > span" (child, order 1) and "div span" (descendant, order 2) both
    // match it, while only "div p" matches the paragraph.  Same specificity, so
    // the later rule is applied last and the two declarations coexist.
    std::string const html =
        "<style>div p { color: red } p > span { color: blue } div span { font-weight: bold }</style>"
        "<div><p>a<span>b</span></p></div>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "ab");
    EXPECT_EQ(run_style_at(html, 0, 0), "color:red;");
    EXPECT_EQ(run_style_at(html, 0, 1), "font-weight:bold;color:blue;");

    // A child combinator must NOT match a deeper descendant: here the span's
    // parent is p, not div, so "div > span" must not apply (a descendant
    // combinator would wrongly match it).
    std::string const nested =
        "<style>div > span { font-weight: bold }</style><div><p><span>x</span></p></div>";
    EXPECT_EQ(run_style_at(nested, 0, 0), "");
}

TEST(TextPasteHtml, ImportantIsStrippedAndInlineStyleWins)
{
    EXPECT_EQ(run_style_at("<style>p { color: red !important }</style><p>x</p>", 0, 0), "color:red;");
    EXPECT_EQ(run_style_at("<style>p { color: red }</style><p style=\"color:#0000ff\">x</p>", 0, 0),
              "color:#0000ff;");
    // The stylesheet rule outranks the semantic tag mapping, inline outranks both.
    EXPECT_EQ(run_style_at("<style>b { font-weight: normal }</style><b>x</b>", 0, 0),
              "font-weight:normal;");
    EXPECT_EQ(run_style_at("<b style=\"font-weight:normal\">x</b>", 0, 0), "font-weight:normal;");
}

TEST(TextPasteHtml, AtRulesAndUnsafeValuesAreNeverFetchedOrEmitted)
{
    std::string const html =
        "<style>@import url(\"http://evil.invalid/a.css\"); @font-face { font-family: X } "
        "p { color: green; background: url(http://evil.invalid/b.png) }</style>"
        "<p onclick=\"javascript:alert(1)\" "
        "style=\"color:#0000ff; background-image:url(http://evil.invalid/c.png)\">x</p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, "x");
    EXPECT_EQ(run_style_at(html, 0, 0), "color:#0000ff;");
    for (auto const &paragraph : result.fragment.paragraphs) {
        EXPECT_EQ(paragraph.style.find("url("), std::string::npos);
        EXPECT_EQ(paragraph.style.find("javascript"), std::string::npos);
        EXPECT_EQ(paragraph.style.find("evil.invalid"), std::string::npos);
        for (auto const &run : paragraph.runs) {
            EXPECT_EQ(run.style.find("url("), std::string::npos);
            EXPECT_EQ(run.style.find("javascript"), std::string::npos);
            EXPECT_EQ(run.style.find("expression("), std::string::npos);
            EXPECT_EQ(run.style.find("evil.invalid"), std::string::npos);
        }
    }
    // The ignored @import is reported, never resolved.
    bool saw_at_rule_loss = false;
    for (auto const &loss : result.losses) {
        saw_at_rule_loss = saw_at_rule_loss || loss == "at-rule-ignored";
    }
    EXPECT_TRUE(saw_at_rule_loss);
    expect_style_invariants(result);
}

TEST(TextPasteHtml, UnsupportedSelectorsAndDeclarationsAreDropped)
{
    std::string const html =
        "<style>p:hover { color: red } p[title] { color: red } p + p { color: red } "
        "p { width: 10px; color: green }</style><p>x</p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(run_style_at(html, 0, 0), "color:green;");
    bool saw_selector_loss = false;
    for (auto const &loss : result.losses) {
        saw_selector_loss = saw_selector_loss || loss == "unsupported-selector";
    }
    EXPECT_TRUE(saw_selector_loss);
}

TEST(TextPasteHtml, NonFiniteUnitBearingStyleValuesAreNeverImported)
{
    // The review gap: g_ascii_strtod() stops at the unit or '%', so the numeric
    // prefix of "1e999%" escaped the old exact-parse guard.  Drop-semantics
    // oracle: the offending declaration is dropped (with a loss) and no
    // non-finite value reaches a fragment style, while the text survives.
    expect_nonfinite_declaration_dropped("<p style=\"font-stretch:1e999%\">x</p>", "font-stretch");
    expect_nonfinite_declaration_dropped("<p style=\"font-size:1e999px\">x</p>", "font-size");
    expect_nonfinite_declaration_dropped("<p style=\"letter-spacing:1e999pt\">x</p>", "letter-spacing");
    expect_nonfinite_declaration_dropped("<p style=\"opacity:nan%\">x</p>", "opacity");
}

TEST(TextPasteHtml, NonFiniteUnitlessStyleValuesAreNeverImported)
{
    expect_nonfinite_declaration_dropped("<p style=\"opacity:nan\">x</p>", "opacity");
    expect_nonfinite_declaration_dropped("<p style=\"opacity:1e999\">x</p>", "opacity");
    expect_nonfinite_declaration_dropped("<p style=\"stroke-miterlimit:1e999\">x</p>", "stroke-miterlimit");
    expect_nonfinite_declaration_dropped("<p style=\"font-weight:1e999\">x</p>", "font-weight");
}

TEST(TextPasteHtml, FiniteUnitAndPercentStyleValuesReachTheFragment)
{
    // The same property names with finite values keep their value intact; the
    // guard rejects non-finite numbers only.  2pt == 96/72*2 px, formatted with
    // the decoder's "%.10g" length formatter.
    EXPECT_EQ(run_style_at("<p style=\"font-stretch:150%\">x</p>", 0, 0), "font-stretch:150%;");
    EXPECT_EQ(run_style_at("<p style=\"font-size:12px\">x</p>", 0, 0), "font-size:12px;");
    EXPECT_EQ(run_style_at("<p style=\"letter-spacing:2pt\">x</p>", 0, 0), "letter-spacing:2.666666667px;");
    EXPECT_EQ(run_style_at("<p style=\"opacity:0.5\">x</p>", 0, 0), "opacity:0.5;");
    EXPECT_EQ(run_style_at("<p style=\"stroke-miterlimit:4\">x</p>", 0, 0), "stroke-miterlimit:4;");
    EXPECT_EQ(run_style_at("<p style=\"font-weight:600\">x</p>", 0, 0), "font-weight:600;");
}

// ===========================================================================
// Fixture group 8: CF_HTML envelopes
// ===========================================================================

TEST(TextPasteHtml, CfHtmlValidEnvelope)
{
    std::size_t start_fragment = 0;
    std::string const payload = cf_payload(&start_fragment);
    Envelope const envelope = unwrap_html_envelope(payload);
    EXPECT_TRUE(envelope.cf_html);
    EXPECT_TRUE(envelope.valid);
    EXPECT_EQ(envelope.begin, start_fragment);
    EXPECT_EQ(envelope.end, start_fragment + std::strlen(CF_FRAGMENT));

    Result const result = decode(payload);
    ASSERT_EQ(result.status, Status::ok) << result.error;
    EXPECT_EQ(result.fragment.plain, "hi there");
    EXPECT_EQ(result.consumed_envelope_bytes, start_fragment);
    EXPECT_TRUE(result.has_meaningful_styles);
}

TEST(TextPasteHtml, BareHtmlHasNoEnvelope)
{
    Envelope const envelope = unwrap_html_envelope("<p>x</p>");
    EXPECT_FALSE(envelope.cf_html);
    EXPECT_TRUE(envelope.valid);
    EXPECT_EQ(envelope.begin, 0u);
    EXPECT_EQ(envelope.end, 8u);
}

TEST(TextPasteHtml, Utf8BomIsExcludedFromTheDocumentRange)
{
    std::string const payload = "\xEF\xBB\xBF<p>x</p>";
    Envelope const envelope = unwrap_html_envelope(payload);
    EXPECT_FALSE(envelope.cf_html);
    EXPECT_TRUE(envelope.valid);
    EXPECT_EQ(envelope.begin, 3u);
    EXPECT_EQ(envelope.end, payload.size());
    EXPECT_EQ(plain_of(payload), "x");
}

TEST(TextPasteHtml, CfHtmlWithUtf8BomUsesTheRawByteOffsetBase)
{
    // unwrap_html_envelope() skips a leading UTF-8 BOM for detection and header
    // parsing only (text-paste-html.cpp:306-309); the four offsets are then
    // applied to the original byte string with no BOM adjustment (lines 383-397).
    // The base the implementation chose is therefore the raw payload INCLUDING
    // the BOM, so a producer that counts the BOM decodes normally.  The fixture
    // is the 105-byte header + 86-byte body of cf_payload() (191 bytes) with the
    // three BOM bytes prepended and every offset shifted by three.
    std::string payload = std::string("\xEF\xBB\xBF") + cf_payload();
    ASSERT_TRUE(patch_offset(payload, "StartHTML", "0000000108"));
    ASSERT_TRUE(patch_offset(payload, "EndHTML", "0000000194"));
    ASSERT_TRUE(patch_offset(payload, "StartFragment", "0000000140"));
    ASSERT_TRUE(patch_offset(payload, "EndFragment", "0000000162"));
    EXPECT_EQ(payload.size(), 194u);

    Envelope const envelope = unwrap_html_envelope(payload);
    EXPECT_TRUE(envelope.cf_html);
    EXPECT_TRUE(envelope.valid);
    EXPECT_EQ(envelope.begin, 140u);
    EXPECT_EQ(envelope.end, 162u);

    Result const result = decode(payload);
    ASSERT_EQ(result.status, Status::ok) << result.error;
    EXPECT_EQ(result.fragment.plain, "hi there");
    EXPECT_EQ(result.consumed_envelope_bytes, 140u);

    // Offsets that exclude the three BOM bytes are NOT rebased: they are used
    // verbatim against the raw payload, so only the envelope geometry is pinned.
    // The decoded content of this inconsistent fixture depends on libxml2
    // recovery of a fragment sliced mid-header and is deliberately not asserted.
    std::string const not_rebased = std::string("\xEF\xBB\xBF") + cf_payload();
    Envelope const raw = unwrap_html_envelope(not_rebased);
    EXPECT_TRUE(raw.cf_html);
    EXPECT_TRUE(raw.valid);
    EXPECT_EQ(raw.begin, 137u);
    EXPECT_EQ(raw.end, 159u);
}

TEST(TextPasteHtml, CfHtmlMinusOneFallsBackToTheHtmlPair)
{
    std::string const body = cf_body();
    std::string payload = cf_payload();
    ASSERT_TRUE(patch_offset(payload, "StartFragment", "-000000001"));
    ASSERT_TRUE(patch_offset(payload, "EndFragment", "-000000001"));
    // Detection still succeeds through StartHTML.
    Envelope const envelope = unwrap_html_envelope(payload);
    EXPECT_TRUE(envelope.cf_html);
    EXPECT_TRUE(envelope.valid) << "html pair should be used when the fragment pair is -1";
    EXPECT_EQ(payload.substr(envelope.begin, envelope.end - envelope.begin), body);

    // With both pairs unavailable the envelope is invalid and decode falls back.
    std::string const no_pair = "<p>x</p>";
    std::string const enveless = "Version:0.9\r\nStartHTML:-1\r\nEndHTML:-1\r\n"
                                 "StartFragment:-1\r\nEndFragment:-1\r\n" +
                                 no_pair;
    Envelope const rejected = unwrap_html_envelope(enveless);
    EXPECT_TRUE(rejected.cf_html);
    EXPECT_FALSE(rejected.valid);
    EXPECT_EQ(decode(enveless).status, Status::malformed);
}

TEST(TextPasteHtml, CfHtmlInvalidOffsetsAreRejected)
{
    // Reversed fragment offsets.
    {
        std::string payload = cf_payload();
        ASSERT_TRUE(patch_offset(payload, "StartFragment", "0000000150"));
        ASSERT_TRUE(patch_offset(payload, "EndFragment", "0000000110"));
        EXPECT_FALSE(unwrap_html_envelope(payload).valid);
        Result const result = decode(payload);
        EXPECT_EQ(result.status, Status::malformed);
        EXPECT_EQ(result.error, "invalid CF_HTML envelope");
    }
    // Offset past the end of the payload.
    {
        std::string payload = cf_payload();
        ASSERT_TRUE(patch_offset(payload, "EndFragment", "9999999999"));
        EXPECT_FALSE(unwrap_html_envelope(payload).valid);
        EXPECT_EQ(decode(payload).status, Status::malformed);
    }
    // StartHTML after StartFragment while both pairs are valid and in range.
    {
        std::string payload = cf_payload();
        ASSERT_TRUE(patch_offset(payload, "StartHTML", "0000000150"));
        ASSERT_TRUE(patch_offset(payload, "EndHTML", "0000000180"));
        ASSERT_TRUE(patch_offset(payload, "StartFragment", "0000000110"));
        ASSERT_TRUE(patch_offset(payload, "EndFragment", "0000000170"));
        // The fixture is 191 bytes (105-byte header + 86-byte body), so every
        // patched offset above is in range: it is the
        // StartHTML > StartFragment cross-check of REPORT.md 4.3 that rejects
        // this envelope, not the 0 <= begin <= end <= size range check.
        ASSERT_GT(payload.size(), 180u);
        EXPECT_FALSE(unwrap_html_envelope(payload).valid);
        EXPECT_EQ(decode(payload).status, Status::malformed);
    }
    // A value the grammar rejects ("+" is not an optional single '-', and there
    // is trailing junk) invalidates that field; when both pairs fail the whole
    // envelope is invalid.  No atoi, no overflow.
    {
        std::string payload = cf_payload();
        ASSERT_TRUE(patch_offset(payload, "StartHTML", "00000000+1"));
        ASSERT_TRUE(patch_offset(payload, "StartFragment", "00000000+2"));
        EXPECT_FALSE(unwrap_html_envelope(payload).valid);
        EXPECT_EQ(decode(payload).status, Status::malformed);
    }
    // A broken fragment pair alone falls back to a valid HTML pair.
    {
        std::string payload = cf_payload();
        ASSERT_TRUE(patch_offset(payload, "StartFragment", "00000000+2"));
        Envelope const envelope = unwrap_html_envelope(payload);
        EXPECT_TRUE(envelope.cf_html);
        EXPECT_TRUE(envelope.valid);
        EXPECT_EQ(payload.substr(envelope.begin, envelope.end - envelope.begin), cf_body());
    }
    // 'Version:' without any StartHTML/StartFragment line is bare HTML.
    {
        std::string const not_envelope = "Version:0.9\r\n<p>x</p>";
        Envelope const envelope = unwrap_html_envelope(not_envelope);
        EXPECT_FALSE(envelope.cf_html);
    }
}

TEST(TextPasteHtml, CfHtmlOffsetInsideAMultibyteSequenceIsRejected)
{
    std::string const fragment = "<p>\u00E9</p>";
    std::string const body = "<html><body><!--StartFragment-->" + fragment + "<!--EndFragment--></body></html>";
    char probe[512];
    int const header_len = std::snprintf(
        probe, sizeof probe,
        "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n", 0,
        0, 0, 0);
    long long const start_html = header_len;
    long long const end_html = header_len + static_cast<long long>(body.size());
    std::size_t const fragment_at = body.find(fragment);
    // Point StartFragment at the continuation byte of U+00E9 (the second byte).
    long long const start_fragment = header_len + static_cast<long long>(fragment_at) + 4;
    long long const end_fragment = start_fragment + 1;
    char header[512];
    std::snprintf(header, sizeof header,
                  "Version:0.9\r\nStartHTML:%010lld\r\nEndHTML:%010lld\r\n"
                  "StartFragment:%010lld\r\nEndFragment:%010lld\r\n",
                  start_html, end_html, start_fragment, end_fragment);
    std::string const payload = std::string(header) + body;
    ASSERT_EQ(payload[start_fragment], static_cast<char>(0xA9));

    Envelope const envelope = unwrap_html_envelope(payload);
    EXPECT_TRUE(envelope.cf_html);
    EXPECT_FALSE(envelope.valid);
    EXPECT_EQ(decode(payload).status, Status::malformed);
}

TEST(TextPasteHtml, CfHtmlHeaderBeyondTheScanWindowIsBareHtml)
{
    std::string payload = "Version:0.9\r\n";
    payload += "Pad:" + std::string(1100, 'y') + "\r\n";
    payload += "StartHTML:0000000120\r\nEndHTML:0000000200\r\n"
               "StartFragment:0000000130\r\nEndFragment:0000000190\r\n";
    ASSERT_GT(payload.find("StartHTML:"), 1024u);
    Envelope const envelope = unwrap_html_envelope(payload);
    EXPECT_FALSE(envelope.cf_html);
    EXPECT_TRUE(envelope.valid);
    EXPECT_EQ(envelope.begin, 0u);
    EXPECT_EQ(envelope.end, payload.size());
}

// ===========================================================================
// Fixture group 9: limits at the boundary and boundary + 1
// ===========================================================================

TEST(TextPasteHtml, TextByteLimitBoundaryAndOver)
{
    std::string const at_limit = "<p>" + std::string(65536, 'a') + "</p>";
    Result const ok = decode(at_limit);
    ASSERT_EQ(ok.status, Status::ok) << ok.error;
    EXPECT_EQ(ok.fragment.plain.size(), 65536u);

    std::string const over = "<p>" + std::string(65537, 'a') + "</p>";
    Result const rejected = decode(over);
    EXPECT_EQ(rejected.status, Status::limit_exceeded);
    EXPECT_EQ(rejected.error, "limit: text-bytes");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
}

TEST(TextPasteHtml, TextByteLimitWithMultibyteEnding)
{
    std::string const at_limit = "<p>" + std::string(65532, 'a') + "\U0001F600" + "</p>";
    Result const ok = decode(at_limit);
    ASSERT_EQ(ok.status, Status::ok) << ok.error;
    EXPECT_EQ(ok.fragment.plain.size(), 65536u);
    EXPECT_TRUE(ok.fragment.plain.ends_with("\U0001F600"));

    std::string const over = "<p>" + std::string(65533, 'a') + "\U0001F600" + "</p>";
    Result const rejected = decode(over);
    EXPECT_EQ(rejected.status, Status::limit_exceeded);
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
}

TEST(TextPasteHtml, ParagraphLimitBoundaryAndOver)
{
    std::string html;
    for (int i = 0; i < 1024; ++i) {
        html += "<p>a</p>";
    }
    Result const ok = decode(html);
    ASSERT_EQ(ok.status, Status::ok) << ok.error;
    EXPECT_EQ(ok.fragment.paragraphs.size(), 1024u);
    EXPECT_EQ(ok.fragment.plain.size(), 2047u);

    html += "<p>a</p>";
    Result const rejected = decode(html);
    EXPECT_EQ(rejected.status, Status::limit_exceeded);
    EXPECT_EQ(rejected.error, "limit: paragraphs");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
}

TEST(TextPasteHtml, RunLimitBoundaryAndOver)
{
    auto build = [](int count) {
        std::string html = "<p>";
        for (int i = 1; i <= count; ++i) {
            html += "<span style=\"font-size:" + std::to_string(i) + "px\">a</span>";
        }
        return html + "</p>";
    };
    Result const ok = decode(build(4096));
    ASSERT_EQ(ok.status, Status::ok) << ok.error;
    ASSERT_EQ(ok.fragment.paragraphs.size(), 1u);
    EXPECT_EQ(ok.fragment.paragraphs[0].runs.size(), 4096u);
    EXPECT_EQ(ok.fragment.plain.size(), 4096u);

    Result const rejected = decode(build(4097));
    EXPECT_EQ(rejected.status, Status::limit_exceeded);
    EXPECT_EQ(rejected.error, "limit: runs");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
}

TEST(TextPasteHtml, DepthLimitBoundaryAndOver)
{
    // libxml2 reports the implicit <html> and <body>, so N nested divs sit at
    // depth N + 2 (spike-output.txt "B.small depth=4").
    auto build = [](int depth) {
        int const divs = depth - 2;
        std::string html;
        for (int i = 0; i < divs; ++i) {
            html += "<div>";
        }
        html += "x";
        for (int i = 0; i < divs; ++i) {
            html += "</div>";
        }
        return html;
    };
    Result const ok = decode(build(64));
    ASSERT_EQ(ok.status, Status::ok) << ok.error;
    EXPECT_EQ(ok.fragment.plain, "x");

    Result const rejected = decode(build(65));
    EXPECT_EQ(rejected.status, Status::limit_exceeded);
    EXPECT_EQ(rejected.error, "limit: depth");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
}

TEST(TextPasteHtml, NodeLimitBoundaryAndOver)
{
    // 7 bytes per empty <b>; the implicit <html> and <body> add two nodes.
    auto build = [](int extra) {
        std::string html;
        html.reserve(32766 * 7 + 16);
        for (int i = 0; i < 32766; ++i) {
            html += "<b></b>";
        }
        html += extra == 0 ? "x" : "<b>x</b>";
        return html;
    };
    Result const ok = decode(build(0));
    ASSERT_EQ(ok.status, Status::ok) << ok.error;
    EXPECT_EQ(ok.fragment.plain, "x");

    Result const rejected = decode(build(1));
    EXPECT_EQ(rejected.status, Status::limit_exceeded);
    EXPECT_EQ(rejected.error, "limit: nodes");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
}

TEST(TextPasteHtml, InputByteLimitAndShrunkenLimits)
{
    // Every limit rejection must leave no partial fragment behind.
    auto expect_limit_rejected = [](Result const &result) {
        EXPECT_EQ(result.status, Status::limit_exceeded);
        EXPECT_TRUE(result.fragment.paragraphs.empty());
    };

    expect_limit_rejected(decode(std::string(256u * 1024u + 1u, 'a')));

    Limits small;
    small.max_input_bytes = 8;
    EXPECT_EQ(decode("<p>x</p>", small).status, Status::ok);
    expect_limit_rejected(decode("<p>xxxxx</p>", small));

    Limits tiny_text;
    tiny_text.max_text_bytes = 3;
    EXPECT_EQ(decode("<p>abc</p>", tiny_text).status, Status::ok);
    expect_limit_rejected(decode("<p>abcd</p>", tiny_text));

    Limits tiny_paragraphs;
    tiny_paragraphs.max_paragraphs = 2;
    EXPECT_EQ(decode("<p>a</p><p>b</p>", tiny_paragraphs).status, Status::ok);
    expect_limit_rejected(decode("<p>a</p><p>b</p><p>c</p>", tiny_paragraphs));

    // The implicit <html> and <body> count, so max_depth 3 admits exactly three
    // open elements (spike-output.txt "B.small depth=4").
    Limits tiny_depth;
    tiny_depth.max_depth = 3;
    EXPECT_EQ(decode("<p>x</p>", tiny_depth).status, Status::ok);
    expect_limit_rejected(decode("<div><span>x</span></div>", tiny_depth));

    Limits tiny_nodes;
    tiny_nodes.max_nodes = 3;
    EXPECT_EQ(decode("<p>x</p>", tiny_nodes).status, Status::ok);
    expect_limit_rejected(decode("<p><b>x</b></p>", tiny_nodes));

    Limits tiny_css_bytes;
    tiny_css_bytes.max_css_bytes = 16; // "p{color:red}" is 12 bytes
    EXPECT_EQ(decode("<style>p{color:red}</style><p>x</p>", tiny_css_bytes).status, Status::ok);
    expect_limit_rejected(decode("<style>p{color:red;font-weight:bold}</style><p>x</p>", tiny_css_bytes));

    Limits tiny_css_rules;
    tiny_css_rules.max_css_rules = 1;
    EXPECT_EQ(decode("<style>p{color:red}</style><p>x</p>", tiny_css_rules).status, Status::ok);
    expect_limit_rejected(decode("<style>p{color:red}b{color:blue}</style><p>x</p>", tiny_css_rules));

    Limits tiny_css_declarations;
    tiny_css_declarations.max_css_declarations = 1;
    EXPECT_EQ(decode("<p style=\"color:red\">x</p>", tiny_css_declarations).status, Status::ok);
    expect_limit_rejected(decode("<p style=\"color:red;font-weight:bold\">x</p>", tiny_css_declarations));
}

TEST(TextPasteHtml, StyleValueLengthCapAt256BytesIsAcceptedAnd257IsDropped)
{
    // MAX_STYLE_VALUE_LENGTH is 256 bytes: a 256-byte value stays, a 257-byte
    // one is dropped while the other declaration in the same style survives.
    std::string const at_limit = "<p style=\"font-variant-ligatures:" + std::string(256, 'a') + "\">x</p>";
    Result const accepted = decode(at_limit);
    ASSERT_EQ(accepted.status, Status::ok) << accepted.error;
    EXPECT_EQ(run_style_at(at_limit, 0, 0), "font-variant-ligatures:" + std::string(256, 'a') + ";");
    EXPECT_EQ(accepted.fragment.plain, "x");
    expect_style_invariants(accepted);

    std::string const over =
        "<p style=\"font-variant-ligatures:" + std::string(257, 'a') + "; font-weight:bold\">x</p>";
    Result const dropped = decode(over);
    ASSERT_EQ(dropped.status, Status::ok) << dropped.error;
    EXPECT_EQ(run_style_at(over, 0, 0), "font-weight:bold;");
    EXPECT_EQ(dropped.fragment.plain, "x");
}

TEST(TextPasteHtml, StyleLengthCapAt2048BytesIsAcceptedAnd2049DropsTheList)
{
    // MAX_STYLE_LENGTH is 2048 bytes of canonical "name:value;" declarations:
    // eight long allow-listed declarations (174 bytes of names plus 16 bytes of
    // ':' and ';') with 233+233+232*6 = 1858 bytes of values hit exactly 2048.
    // One value byte more must drop the whole style list, never a partial one.
    auto fixture = [](std::size_t extra_byte) {
        struct LongDeclaration {
            char const *name;
            std::size_t value_bytes;
        };
        LongDeclaration const declarations[] = {
            {"font-variant-alternates", 233 + extra_byte},
            {"font-variant-east-asian", 233},
            {"font-variant-ligatures", 232},
            {"font-variant-numeric", 232},
            {"font-variant-position", 232},
            {"font-variation-settings", 232},
            {"font-feature-settings", 232},
            {"text-decoration-style", 232},
        };
        std::string style;
        for (auto const &declaration : declarations) {
            style += declaration.name;
            style += ':';
            style += std::string(declaration.value_bytes, 'a');
            style += ';';
        }
        return "<p style=\"" + style + "\">x</p>";
    };

    std::string const at_limit = fixture(0);
    Result const accepted = decode(at_limit);
    ASSERT_EQ(accepted.status, Status::ok) << accepted.error;
    std::string const style = run_style_at(at_limit, 0, 0);
    EXPECT_EQ(style.size(), 2048u);
    EXPECT_NE(style.find("font-variant-alternates:"), std::string::npos);
    EXPECT_NE(style.find("text-decoration-style:"), std::string::npos);
    EXPECT_EQ(accepted.fragment.plain, "x");
    expect_style_invariants(accepted);

    std::string const over = fixture(1);
    Result const dropped = decode(over);
    ASSERT_EQ(dropped.status, Status::ok) << dropped.error;
    EXPECT_EQ(run_style_at(over, 0, 0), "");
    EXPECT_EQ(dropped.fragment.plain, "x");
}

// ===========================================================================
// Fixture group 10: UTF-8 splits
// ===========================================================================

TEST(TextPasteHtml, MultibyteCharacterStraddlingTheParserChunkBoundary)
{
    // The decoder feeds 4096-byte chunks; place U+1F600 across that boundary.
    std::string const html = "<p>" + std::string(4091, 'a') + "\U0001F600" + "</p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, std::string(4091, 'a') + "\U0001F600");
    EXPECT_EQ(result.fragment.plain.size(), 4091u + 4u);
}

TEST(TextPasteHtml, MultibyteCharacterAtARunLengthSplitPoint)
{
    std::string const html = "<p>" + std::string(8191, 'a') + "\u00E9" + "b" + "</p>";
    Result const result = decode_ok(html);
    EXPECT_EQ(result.fragment.plain, std::string(8191, 'a') + "\u00E9" + "b");
    ASSERT_EQ(result.fragment.paragraphs.size(), 1u);
    ASSERT_GE(result.fragment.paragraphs[0].runs.size(), 2u);
    std::string rebuilt;
    for (auto const &run : result.fragment.paragraphs[0].runs) {
        EXPECT_LE(run.text.size(), MAX_RUN_LENGTH);
        EXPECT_TRUE(g_utf8_validate(run.text.data(), static_cast<gssize>(run.text.size()), nullptr))
            << "run cut a UTF-8 sequence";
        rebuilt += run.text;
    }
    EXPECT_EQ(rebuilt, result.fragment.plain);
}

TEST(TextPasteHtml, ShrunkenRunLengthStillSplitsOnCharacterBoundaries)
{
    Limits tiny;
    tiny.max_run_bytes = 4;
    std::string const html = "<p>\u00E9\u00E9\u00E9\u00E9</p>"; // four 2-byte characters
    Result const result = decode(html, tiny);
    ASSERT_EQ(result.status, Status::ok) << result.error;
    EXPECT_EQ(result.fragment.plain, "\u00E9\u00E9\u00E9\u00E9");
    for (auto const &run : result.fragment.paragraphs[0].runs) {
        EXPECT_TRUE(g_utf8_validate(run.text.data(), static_cast<gssize>(run.text.size()), nullptr));
    }
}

// ===========================================================================
// Prompt gate: plain content packaged as HTML yields no meaningful styles
// ===========================================================================

TEST(TextPasteHtml, PlainContentAsHtmlHasNoMeaningfulStyles)
{
    Result const result = decode_ok("<p>hello world</p>");
    EXPECT_EQ(result.fragment.plain, "hello world");
    EXPECT_FALSE(result.fragment.has_styles());
    EXPECT_FALSE(result.has_meaningful_styles);
}

TEST(TextPasteHtml, BoldMarkupHasMeaningfulStyles)
{
    Result const result = decode_ok("<b>x</b>");
    EXPECT_TRUE(result.fragment.has_styles());
    EXPECT_TRUE(result.has_meaningful_styles);
}

TEST(TextPasteHtml, UniformFamilyAndSizeIsNotMeaningfulButVariationIs)
{
    Result const uniform = decode_ok("<p style=\"font-family:Arial; font-size:12pt\">x</p>");
    EXPECT_TRUE(uniform.fragment.has_styles());
    EXPECT_FALSE(uniform.has_meaningful_styles);

    Result const varying = decode_ok("<p style=\"font-family:Arial\">a</p><p style=\"font-family:Times\">b</p>");
    EXPECT_TRUE(varying.has_meaningful_styles);

    Result const sized = decode_ok("<p style=\"font-size:12pt\">a</p><p style=\"font-size:18pt\">b</p>");
    EXPECT_TRUE(sized.has_meaningful_styles);
}

// ===========================================================================
// Failure classification (REPORT.md 10.2)
// ===========================================================================

TEST(TextPasteHtml, InvalidInputsAreMalformed)
{
    EXPECT_EQ(decode("").status, Status::malformed);
    EXPECT_EQ(decode("    ").status, Status::malformed);
    EXPECT_EQ(decode("<p>\xFF\xFE</p>").status, Status::malformed);
    EXPECT_EQ(decode(std::string("<p>a\0b</p>", 9)).status, Status::malformed);
    EXPECT_EQ(decode("<!-- only a comment -->").status, Status::malformed);
    EXPECT_EQ(decode("<script>var x = 1;</script>").status, Status::malformed);
    EXPECT_EQ(decode("<br>").status, Status::malformed);

    // The malformed results carry no fragment at all.
    Result const invalid = decode("<p>\xFF\xFE</p>");
    EXPECT_EQ(invalid.error, "payload is not valid UTF-8");
    EXPECT_TRUE(invalid.fragment.paragraphs.empty());
    EXPECT_FALSE(invalid.has_meaningful_styles);

    Result const nul = decode(std::string("<p>a\0b</p>", 9));
    EXPECT_EQ(nul.error, "payload contains a NUL byte");
}

TEST(TextPasteHtml, CharsetDeclarationsOtherThanUtf8AreMalformed)
{
    EXPECT_EQ(decode("<html><head><meta charset=\"iso-8859-1\"></head><body><p>x</p></body></html>").status,
              Status::malformed);
    EXPECT_EQ(decode("<html><head><meta http-equiv=\"Content-Type\" "
                     "content=\"text/html; charset=windows-1252\"></head><body><p>x</p></body></html>")
                  .status,
              Status::malformed);
    EXPECT_EQ(decode("<html><head><meta charset=\"utf-8\"></head><body><p>x</p></body></html>").status,
              Status::ok);
    EXPECT_EQ(decode("<html><head><meta charset=utf8></head><body><p>x</p></body></html>").status, Status::ok);
    EXPECT_EQ(decode("<html><head><meta charset=\"US-ASCII\"></head><body><p>x</p></body></html>").status,
              Status::ok);
}

TEST(TextPasteHtml, DecodeNeverThrowsOnAdversarialInput)
{
    std::string const hostile =
        "<table><tr><td><p><b><i><u><s><sub><sup><code><pre><div><span><a href=\"x\">"
        "&#x0;&#x1;&#x7f;&#x80;&#x9f;&bogus;&amp;"
        "</a></span></div></pre></code></sup></sub></s></u></i></b></p></td></tr></table>";
    Result const result = decode(hostile);
    EXPECT_TRUE(result.status == Status::ok || result.status == Status::malformed);
    if (result.status == Status::ok) {
        expect_style_invariants(result);
    }
}

// ===========================================================================
// MIME alias handling
// ===========================================================================

TEST(TextPasteHtml, DecodeRepresentationSniffsContentRegardlessOfAlias)
{
    Result const plain = decode_representation("<p>x</p>", "text/html");
    ASSERT_EQ(plain.status, Status::ok) << plain.error;
    EXPECT_EQ(plain.fragment.plain, "x");

    Result const mangled = decode_representation("<p>x</p>", "application/x.windows.HTML Format");
    ASSERT_EQ(mangled.status, Status::ok) << mangled.error;
    EXPECT_EQ(mangled.fragment.plain, "x");

    Result const no_alias = decode_representation("<p>x</p>", "");
    ASSERT_EQ(no_alias.status, Status::ok) << no_alias.error;
    EXPECT_EQ(no_alias.fragment.plain, "x");

    // The alias appears only as a bounded diagnostic code.
    ASSERT_FALSE(mangled.losses.empty());
    EXPECT_EQ(mangled.losses.back(), "alias:application/x.windows.html format");
    for (auto const &loss : plain.losses) {
        EXPECT_EQ(loss, "alias:text/html");
    }

    // A CF_HTML payload decodes identically through any alias.
    std::string const payload = cf_payload();
    Result const via_alias = decode_representation(payload, "text/html");
    ASSERT_EQ(via_alias.status, Status::ok) << via_alias.error;
    EXPECT_EQ(via_alias.fragment.plain, "hi there");
}

// ===========================================================================
// Cross-cutting invariants over a corpus of fixtures
// ===========================================================================

TEST(TextPasteHtml, EveryFixtureKeepsStyleAndRoundTripInvariants)
{
    std::vector<std::string> const fixtures = {
        "<p style=\"font-family:Arial; font-size:12pt; color:#ff0000\">x</p>",
        "<div style=\"text-align:center\"><p>a</p><p style=\"line-height:2\">b</p></div>",
        "<ul><li>a</li><li><ol><li>b</li></ol></li></ul>",
        "<table><tr><th>h1</th><th>h2</th></tr><tr><td>1</td><td>2</td></tr></table>",
        "<p><b>b</b><i>i</i><u>u</u><s>s</s><sub>x</sub><sup>y</sup><code>c</code></p>",
        "<pre>a\tb\n  c</pre><p>d   e</p>",
        "<p>a<br><br>b</p>",
        "<style>p { color: #00ff00 } .c { font-weight: bold }</style><p class=\"c\">x</p>",
        "<p style=\"letter-spacing:2px; word-spacing:2pt; line-height:150%; text-indent:2em\">x</p>",
        "<p>a &amp; b &nbsp; &copy; &#233; &#x1F600;</p>",
    };
    for (auto const &fixture : fixtures) {
        Result const result = decode(fixture);
        ASSERT_EQ(result.status, Status::ok) << fixture << " -> " << result.error;
        expect_style_invariants(result);
        expect_round_trip(result);
    }
}

// ===========================================================================
// Fixture group 19: bounded ordered-list counters
//
// The accepted range is a documented literal: magnitudes up to 1000000000 are
// numbered as written, anything larger (or any literal that overflows strtol)
// is ignored so the list keeps its default numbering.  These expectations are
// literal strings; the bound is intentionally not imported from production
// code, so a silent change to it fails here.
// ===========================================================================

TEST(TextPasteHtml, OrderedListOverflowingStartFallsBackToDefaultNumbering)
{
    // LONG_MAX is representable but outside the accepted magnitude, so start is
    // ignored instead of numbering items from 9223372036854775807 (the old
    // emitter then incremented into signed overflow on the next item).
    EXPECT_EQ(plain_of("<ol start=\"9223372036854775807\"><li>a</li><li>b</li></ol>"),
              "1. a\n2. b");
    // One past LONG_MAX overflows strtol (ERANGE); the literal must not be
    // adopted as LONG_MAX either.
    EXPECT_EQ(plain_of("<ol start=\"9223372036854775808\"><li>a</li><li>b</li></ol>"),
              "1. a\n2. b");
}

TEST(TextPasteHtml, OrderedListItemValueOverflowFallsBackToTheEnclosingNumbering)
{
    // A rejected item value leaves the running counter untouched, so the item
    // continues the list's default numbering and the following item follows it.
    EXPECT_EQ(plain_of("<ol><li>a</li><li value=\"9223372036854775807\">b</li><li>c</li></ol>"),
              "1. a\n2. b\n3. c");
    EXPECT_EQ(plain_of("<ol><li>a</li><li value=\"-9223372036854775808\">b</li><li>c</li></ol>"),
              "1. a\n2. b\n3. c");
}

TEST(TextPasteHtml, OrderedListThirtyDigitCounterIsRejectedThroughErange)
{
    // A 30-digit decimal cannot be represented in a long; strtol reports ERANGE
    // and returns LONG_MAX, which must not become a list counter.
    EXPECT_EQ(plain_of("<ol start=\"123456789012345678901234567890\"><li>a</li><li>b</li></ol>"),
              "1. a\n2. b");
    EXPECT_EQ(plain_of("<ol><li value=\"123456789012345678901234567890\">a</li><li>b</li></ol>"),
              "1. a\n2. b");
}

TEST(TextPasteHtml, OrderedListWithinBoundCountersKeepLiteralNumbering)
{
    // Control: normal start/value arithmetic, including negative numbering, is
    // unchanged by the bound.
    EXPECT_EQ(plain_of("<ol start=\"7\"><li>a</li><li value=\"-3\">b</li><li>c</li></ol>"),
              "7. a\n-3. b\n-2. c");
    EXPECT_EQ(plain_of("<ol start=\"-4\"><li>a</li><li>b</li></ol>"), "-4. a\n-3. b");
    // Zero is inside the bound and remains a legal item value.
    EXPECT_EQ(plain_of("<ol><li value=\"0\">z</li><li>y</li></ol>"), "0. z\n1. y");
}

TEST(TextPasteHtml, OrderedListCounterSaturatesAtTheLiteralBound)
{
    // The bound itself is accepted; incrementing past it saturates instead of
    // overflowing, so every following item repeats the bound.
    EXPECT_EQ(plain_of("<ol start=\"1000000000\"><li>a</li><li>b</li></ol>"),
              "1000000000. a\n1000000000. b");
    EXPECT_EQ(plain_of("<ol><li>a</li><li value=\"1000000000\">b</li><li>c</li></ol>"),
              "1. a\n1000000000. b\n1000000000. c");
    // The negative bound is inclusive; one past it is rejected.
    EXPECT_EQ(plain_of("<ol><li value=\"-1000000000\">a</li><li>b</li></ol>"),
              "-1000000000. a\n-999999999. b");
    EXPECT_EQ(plain_of("<ol start=\"1000000001\"><li>a</li></ol>"), "1. a");
    EXPECT_EQ(plain_of("<ol><li value=\"-1000000001\">a</li></ol>"), "1. a");
}

// ===========================================================================
// Fixture group 11: default-limit literal boundaries
// ===========================================================================
//
// ui/text-paste-html.h documents these production defaults:
//   max_run_bytes        = MAX_RUN_LENGTH = 8,192  (run text bytes, not chars)
//   max_css_bytes        = 65,536          (<style> source bytes)
//   max_css_rules        = 2,048           (parsed rules)
//   max_css_declarations = 8,192           (parsed declarations)
// Each fixture below is built to sit exactly on the literal boundary and the
// next case adds exactly one unit. The assertions use literal numbers instead
// of the imported production constants: a test written as
// `<= MAX_RUN_LENGTH` would keep passing if the default changed, and the
// existing 8,191-'a'+e-acute fixture would also pass with a smaller cap.

TEST(TextPasteHtml, RunByteCapSplitsAt8192NotBefore)
{
    // 8,192 text bytes are one complete run; 8,193 bytes split into exactly
    // 8,192 + 1. Both stay Status::ok (a split is lossless, not a rejection).
    std::string const at_limit = "<p>" + std::string(8192, 'a') + "</p>";
    Result const single = decode(at_limit);
    ASSERT_EQ(single.status, Status::ok) << single.error;
    ASSERT_EQ(single.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(single.fragment.paragraphs[0].runs.size(), 1u);
    EXPECT_EQ(single.fragment.paragraphs[0].runs[0].text.size(), 8192u);
    EXPECT_EQ(single.fragment.paragraphs[0].runs[0].text, std::string(8192, 'a'));
    EXPECT_EQ(single.fragment.plain, std::string(8192, 'a'));
    expect_style_invariants(single);
    expect_round_trip(single);

    std::string const over = "<p>" + std::string(8193, 'a') + "</p>";
    Result const split = decode(over);
    ASSERT_EQ(split.status, Status::ok) << split.error;
    ASSERT_EQ(split.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(split.fragment.paragraphs[0].runs.size(), 2u);
    EXPECT_EQ(split.fragment.paragraphs[0].runs[0].text.size(), 8192u);
    EXPECT_EQ(split.fragment.paragraphs[0].runs[1].text.size(), 1u);
    EXPECT_EQ(split.fragment.paragraphs[0].runs[0].text, std::string(8192, 'a'));
    EXPECT_EQ(split.fragment.paragraphs[0].runs[1].text, "a");
    EXPECT_EQ(split.fragment.plain, std::string(8193, 'a'));
    EXPECT_TRUE(split.error.empty());
}

TEST(TextPasteHtml, RunByteCapCountsUtf8BytesNotCharacters)
{
    // 4,096 two-byte characters are exactly 8,192 text bytes: one run. 4,097
    // characters are 8,194 bytes and must split after 8,192 bytes (4,096
    // characters), keeping the last sequence whole in a 2-byte second run --
    // a character-counted cap would have admitted all 4,097 characters.
    std::string at_limit;
    for (int i = 0; i < 4096; ++i) {
        at_limit += "\u00E9";
    }
    Result const single = decode("<p>" + at_limit + "</p>");
    ASSERT_EQ(single.status, Status::ok) << single.error;
    ASSERT_EQ(single.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(single.fragment.paragraphs[0].runs.size(), 1u);
    EXPECT_EQ(single.fragment.paragraphs[0].runs[0].text.size(), 8192u);
    EXPECT_EQ(single.fragment.paragraphs[0].runs[0].text, at_limit);

    std::string over;
    for (int i = 0; i < 4097; ++i) {
        over += "\u00E9";
    }
    Result const split = decode("<p>" + over + "</p>");
    ASSERT_EQ(split.status, Status::ok) << split.error;
    ASSERT_EQ(split.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(split.fragment.paragraphs[0].runs.size(), 2u);
    EXPECT_EQ(split.fragment.paragraphs[0].runs[0].text.size(), 8192u);
    EXPECT_EQ(split.fragment.paragraphs[0].runs[0].text, at_limit);
    EXPECT_EQ(split.fragment.paragraphs[0].runs[1].text.size(), 2u);
    EXPECT_EQ(split.fragment.paragraphs[0].runs[1].text, "\u00E9");
    EXPECT_EQ(split.fragment.plain, over);
    for (auto const &run : split.fragment.paragraphs[0].runs) {
        EXPECT_TRUE(g_utf8_validate(run.text.data(), static_cast<gssize>(run.text.size()), nullptr))
            << "run cut a UTF-8 sequence";
    }
}

TEST(TextPasteHtml, CssByteLimitAt65536AndOver)
{
    // The <style> source budget counts the raw bytes between <style> and
    // </style>: 65,536 are accepted, 65,537 trip "limit: css-bytes" and leave
    // no fragment at all.
    auto fixture = [](std::size_t css_bytes) {
        return "<style>" + std::string(css_bytes, 'a') + "</style><p>x</p>";
    };
    std::string const at_limit = fixture(65536);
    ASSERT_EQ(at_limit.size(), 7u + 65536u + 8u + 8u);
    Result const accepted = decode(at_limit);
    ASSERT_EQ(accepted.status, Status::ok) << accepted.error;
    EXPECT_EQ(accepted.fragment.plain, "x");

    Result const rejected = decode(fixture(65537));
    EXPECT_EQ(rejected.status, Status::limit_exceeded);
    EXPECT_EQ(rejected.error, "limit: css-bytes");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
    EXPECT_TRUE(rejected.fragment.plain.empty());
}

TEST(TextPasteHtml, CssRuleLimitAt2048AndOver)
{
    // 2,048 single-selector rules are parsed and applied (the last one wins for
    // equal specificity); the 2,049th trips "limit: css-rules" and no rule
    // survives into a fragment.
    auto fixture = [](std::size_t rules) {
        std::string css;
        css.reserve(rules * 12);
        for (std::size_t i = 0; i < rules; ++i) {
            css += "p{color:red}";
        }
        return "<style>" + css + "</style><p>x</p>";
    };
    std::string const at_limit = fixture(2048);
    ASSERT_EQ(at_limit.size(), 7u + 2048u * 12u + 8u + 8u);
    Result const accepted = decode(at_limit);
    ASSERT_EQ(accepted.status, Status::ok) << accepted.error;
    EXPECT_EQ(accepted.fragment.plain, "x");
    EXPECT_EQ(run_style_at(at_limit, 0, 0), "color:red;");
    expect_style_invariants(accepted);

    Result const rejected = decode(fixture(2049));
    EXPECT_EQ(rejected.status, Status::limit_exceeded);
    EXPECT_EQ(rejected.error, "limit: css-rules");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
    EXPECT_TRUE(rejected.fragment.plain.empty());
}

TEST(TextPasteHtml, CssDeclarationLimitAt8192AndOver)
{
    // 8,192 declarations in one inline style are parsed and the cascade leaves
    // the winning "color:red;"; the 8,193rd trips "limit: css-declarations".
    // The cap counts declarations, so the whole style element is abandoned
    // rather than truncated.
    auto fixture = [](std::size_t declarations) {
        std::string style;
        style.reserve(declarations * 10);
        for (std::size_t i = 0; i < declarations; ++i) {
            style += "color:red;";
        }
        return "<p style=\"" + style + "\">x</p>";
    };
    std::string const at_limit = fixture(8192);
    ASSERT_EQ(at_limit.size(), 10u + 8192u * 10u + 7u);
    Result const accepted = decode(at_limit);
    ASSERT_EQ(accepted.status, Status::ok) << accepted.error;
    EXPECT_EQ(accepted.fragment.plain, "x");
    EXPECT_EQ(run_style_at(at_limit, 0, 0), "color:red;");
    expect_style_invariants(accepted);

    Result const rejected = decode(fixture(8193));
    EXPECT_EQ(rejected.status, Status::limit_exceeded);
    EXPECT_EQ(rejected.error, "limit: css-declarations");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
    EXPECT_TRUE(rejected.fragment.plain.empty());
}

// ---------------------------------------------------------------------------
// Hostile CSS nesting (r3 product review P1-1)
// ---------------------------------------------------------------------------
// libcroco's declaration parser recurses per nested function
// (cr_parser_parse_expr -> cr_parser_parse_term -> cr_parser_parse_function)
// with no depth guard, so a payload inside every byte budget (inline `style`
// bounded by the 256 KiB representation cap, `<style>` source by the
// 65,536-byte CSS cap) could exhaust the main-thread stack and SIGSEGV the
// process; catch(...) cannot catch that. The decoder now pre-scans each
// declaration text lexically and rejects hostile or unparsable CSS as
// Status::malformed, which is what lets the caller run the complete plain
// alternative (limit_exceeded would abort the whole paste instead).

namespace {

std::string hostile_inline_style(std::size_t levels)
{
    std::string css = "color:";
    for (std::size_t i = 0; i < levels; ++i) {
        css += "a(";
    }
    return "<p style=\"" + css + "\">x</p>";
}

std::string hostile_style_element(std::size_t levels)
{
    std::string css = "p{color:";
    for (std::size_t i = 0; i < levels; ++i) {
        css += "a(";
    }
    css += "}";
    return "<style>" + css + "</style><p>x</p>";
}

bool has_loss(Result const &result, std::string_view code)
{
    return std::find(result.losses.begin(), result.losses.end(), code) != result.losses.end();
}

} // namespace

TEST(TextPasteHtml, CssNestingAtTheLiteralLimitIsAcceptedAndOneMoreRejected)
{
    // 32 unbalanced functions is the documented literal cap: the declaration
    // list is still handed to libcroco (its later value predicates drop the
    // unusable value), and the document keeps its text. 33 rejects the whole
    // rich decode instead of risking the libcroco recursion.
    Result const accepted = decode(hostile_inline_style(32));
    ASSERT_EQ(accepted.status, Status::ok) << accepted.error;
    EXPECT_EQ(accepted.fragment.plain, "x");

    Result const rejected = decode(hostile_inline_style(33));
    EXPECT_EQ(rejected.status, Status::malformed);
    EXPECT_EQ(rejected.error, "css function nesting exceeds the decoder limit");
    EXPECT_TRUE(has_loss(rejected, "css-nesting-limit"));
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
    EXPECT_TRUE(rejected.fragment.plain.empty());
}

TEST(TextPasteHtml, ThousandLevelCssNestingIsRejectedWithoutCrashing)
{
    // Both entry points: the inline style attribute and the <style> rule body.
    // 1,000 levels is far past the cap and far below the ~28,000 levels measured
    // to overflow the stack, so the rejection is deterministic rather than a
    // race with stack exhaustion.
    for (auto make : {hostile_inline_style, hostile_style_element}) {
        Result const nested = decode(make(1000));
        EXPECT_EQ(nested.status, Status::malformed) << nested.error;
        EXPECT_EQ(nested.error, "css function nesting exceeds the decoder limit");
        EXPECT_TRUE(has_loss(nested, "css-nesting-limit"));
        EXPECT_TRUE(nested.fragment.paragraphs.empty());
        EXPECT_TRUE(nested.fragment.plain.empty());
    }
}

TEST(TextPasteHtml, ThousandLevelParensInStringsCommentsAndEscapesAreNotNesting)
{
    // A CSS string is opaque to the parenthesis count: 1,000 literal '(' inside
    // quotes is one depth-0 token, not 1,000 nested functions.
    std::string quoted = "<p style=\"font-family:'";
    quoted.append(1000, '(');
    quoted += "'\">x</p>";
    Result const in_string = decode(quoted);
    ASSERT_EQ(in_string.status, Status::ok) << in_string.error;
    EXPECT_EQ(in_string.fragment.plain, "x");

    // Comment content is stripped before libcroco (and skipped by the scanner).
    std::string commented = "<p style=\"color:red/*";
    commented.append(1000, '(');
    commented += "*/;font-size:12px\">x</p>";
    Result const in_comment = decode(commented);
    ASSERT_EQ(in_comment.status, Status::ok) << in_comment.error;
    EXPECT_EQ(in_comment.fragment.plain, "x");

    // `\(` is a two-byte escape pair for the scanner, so 1,000 of them are not
    // 1,000 nested functions.
    std::string escaped = "<p style=\"font-family:";
    for (int i = 0; i < 1000; ++i) {
        escaped += "\\(";
    }
    escaped += "\">x</p>";
    Result const in_escape = decode(escaped);
    ASSERT_EQ(in_escape.status, Status::ok) << in_escape.error;
    EXPECT_EQ(in_escape.fragment.plain, "x");
}

TEST(TextPasteHtml, ThousandFlatFunctionsAreAcceptedNotCountedAsNesting)
{
    // The cap counts open depth, not the number of function tokens: 1,000
    // sibling functions never exceed depth 1.
    std::string css = "font-family:";
    for (int i = 0; i < 1000; ++i) {
        css += "a(a)";
    }
    Result const flat = decode("<p style=\"" + css + "\">x</p>");
    ASSERT_EQ(flat.status, Status::ok) << flat.error;
    EXPECT_EQ(flat.fragment.plain, "x");
}

TEST(TextPasteHtml, SixtyKilobyteHostileCssPayloadsAreRejectedWithoutCrashing)
{
    // Exact r3 crash-probe shapes: 28,000 / 30,000 / 32,000 nested functions are
    // 56-64 KiB of CSS, all inside the 65,536-byte <style> cap and far inside
    // the 262,144-byte representation cap, and all measured to SIGSEGV before
    // the fix. Every one must return malformed with an intact empty fragment.
    auto const style_css_bytes = [](std::size_t levels) {
        return std::string("p{color:").size() + 2 * levels + 1;
    };
    EXPECT_LT(style_css_bytes(32000), 65536u) << "the byte cap must not be the reason for the rejection";
    EXPECT_LT(hostile_style_element(32000).size(), Inkscape::UI::TextPaste::MAX_PAYLOAD_BYTES);
    EXPECT_LT(hostile_inline_style(30000).size(), Inkscape::UI::TextPaste::MAX_PAYLOAD_BYTES);

    std::vector<std::pair<char const *, std::string>> const cases{
        {"inline-30000", hostile_inline_style(30000)},
        {"style-28000", hostile_style_element(28000)},
        {"style-32000", hostile_style_element(32000)},
    };
    for (auto const &c : cases) {
        Result const r = decode(c.second);
        EXPECT_EQ(r.status, Status::malformed) << c.first << ": " << r.error;
        EXPECT_EQ(r.error, "css function nesting exceeds the decoder limit") << c.first;
        EXPECT_TRUE(has_loss(r, "css-nesting-limit")) << c.first;
        EXPECT_TRUE(r.fragment.paragraphs.empty()) << c.first;
        EXPECT_TRUE(r.fragment.plain.empty()) << c.first;
    }
}

TEST(TextPasteHtml, UnterminatedCssStringIsRejectedInsteadOfRecovered)
{
    // libcroco's tokenizer errors on an unterminated string and the declaration
    // list then re-scans the remainder, which is exactly where a lexical
    // under-approximation could hide nested functions. The scanner refuses to
    // model that recovery: the declaration text is never handed to libcroco.
    Result const r = decode("<p style=\"font-family:'red\">x</p>");
    EXPECT_EQ(r.status, Status::malformed);
    EXPECT_EQ(r.error, "css declaration text is not lexically well formed");
    EXPECT_TRUE(has_loss(r, "css-lexical-malformed"));
    EXPECT_TRUE(r.fragment.paragraphs.empty());
    EXPECT_TRUE(r.fragment.plain.empty());
}

TEST(TextPasteHtml, CssRejectionIsMalformedNotLimitExceededSoPlainFallbackRuns)
{
    // The classification is part of the contract: clipboard.cpp runs the
    // complete plain read for malformed rich data and aborts the whole paste for
    // limit_exceeded. A hostile style must not be able to suppress the plain
    // alternative.
    Result const r = decode("<p style=\"font-family:'unterminated\">PLAIN TEXT HERE</p>");
    EXPECT_EQ(r.status, Status::malformed);
    EXPECT_NE(r.status, Status::limit_exceeded);
    // The fragment of a rejected document is never partially usable.
    EXPECT_TRUE(r.fragment.plain.empty());
}

TEST(TextPasteHtml, CssStringEscapeThatLibcrocoCannotParseIsRejected)
{
    // libcroco's string token accepts only space, non-ASCII, hex/unicode and
    // escaped-newline sequences (plus verbatim quotes/backslash). Any other
    // escape fails the STRING token, after which its tokenizer re-scans the
    // string body as ordinary tokens; a ';' inside that body then restarts
    // declaration parsing. Hiding function nesting behind `\(;` bypassed a first
    // version of the lexical guard and SIGSEGV'd the probe at 30,000 levels, so
    // the whole declaration text must be rejected instead of being modelled.
    std::string attack = "<p style=\"font-family:'\\(;color:";
    for (int i = 0; i < 1000; ++i) {
        attack += "a(";
    }
    attack += "'\">x</p>";
    Result const hidden = decode(attack);
    EXPECT_EQ(hidden.status, Status::malformed);
    EXPECT_EQ(hidden.error, "css declaration text is not lexically well formed");
    EXPECT_TRUE(has_loss(hidden, "css-lexical-malformed"));
    EXPECT_TRUE(hidden.fragment.plain.empty());

    // The same failure with a declaration libcroco would have recovered and
    // applied (`font-size`) is rejected as a whole, never partially applied.
    Result const recovered = decode("<p style=\"font-family:'\\(;font-size:37px'\">x</p>");
    EXPECT_EQ(recovered.status, Status::malformed);
    EXPECT_EQ(recovered.error, "css declaration text is not lexically well formed");
    EXPECT_TRUE(recovered.fragment.paragraphs.empty());
    EXPECT_TRUE(recovered.fragment.plain.empty());
}

TEST(TextPasteHtml, CssStringEscapesThatLibcrocoAcceptsStillDecode)
{
    // The guard must not over-reject the escapes libcroco's string token really
    // accepts: verbatim quote, verbatim backslash, escaped space, a hex/unicode
    // escape, and an escaped-newline continuation (an HTML parser may normalize
    // the literal newline to a space, which is also an accepted escape).
    std::vector<std::string> const accepted{
        "<p style=\"font-family:'a\\'b'\">x</p>",
        "<p style=\"font-family:'a\\\\b'\">x</p>",
        "<p style=\"font-family:'a\\ b'\">x</p>",
        "<p style=\"font-family:'\\28 9'\">x</p>",
        "<p style=\"font-family:'a\\\nb'\">x</p>",
    };
    for (auto const &html : accepted) {
        Result const r = decode(html);
        EXPECT_EQ(r.status, Status::ok) << html << ": " << r.error;
        EXPECT_EQ(r.fragment.plain, "x") << html;
    }
}

TEST(TextPasteHtml, CssFunctionBudgetIsAnAbsoluteBoundHidingCannotBypass)
{
    // libcroco's declaration recovery is character-based: after a failed
    // declaration it scans raw characters for the next ';' and re-parses from
    // there, even when the ';' sits inside a string the tokenizer would have
    // accepted. The absolute `(` budget must therefore reject nesting that the
    // lexical nesting cap cannot see. This exact shape SIGSEGV'd a version of
    // the guard that relied on the lexical model alone.
    auto flat_functions = [](std::size_t count) {
        std::string value;
        for (std::size_t i = 0; i < count; ++i) {
            value += "a(a)";
        }
        return value;
    };
    // 1,024 raw '(' in one declaration text is the documented absolute bound.
    Result const at_budget = decode("<p style=\"font-family:" + flat_functions(1024) + "\">x</p>");
    ASSERT_EQ(at_budget.status, Status::ok) << at_budget.error;
    EXPECT_EQ(at_budget.fragment.plain, "x");

    Result const over_budget = decode("<p style=\"font-family:" + flat_functions(1025) + "\">x</p>");
    EXPECT_EQ(over_budget.status, Status::malformed);
    EXPECT_EQ(over_budget.error, "css declaration text declares too many functions");
    EXPECT_TRUE(has_loss(over_budget, "css-function-budget"));
    EXPECT_TRUE(over_budget.fragment.plain.empty());

    // The hiding shape with 2,000 nested functions behind a failed string
    // escape and a character-recovery ';': rejected by the absolute bound.
    std::string hidden = "<p style=\"x:)';color:";
    for (int i = 0; i < 2000; ++i) {
        hidden += "a(";
    }
    hidden += "'\">x</p>";
    Result const hidden_result = decode(hidden);
    EXPECT_EQ(hidden_result.status, Status::malformed);
    EXPECT_TRUE(has_loss(hidden_result, "css-function-budget")) << hidden_result.error;
    EXPECT_TRUE(hidden_result.fragment.plain.empty());
}
