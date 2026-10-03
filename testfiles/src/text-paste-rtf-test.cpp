// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Outcome-based tests for the bounded RTF -> TextPaste::Fragment decoder.
 *
 * Pure parser tests: no GTK, no clipboard, no document. Every expectation is
 * derived from internal evidence notes
 * section 3 (semantics) and section 5 (fixtures F01-F20, B01-B18) plus the
 * frozen interface of ui/text-paste-rtf.h. Two REPORT.md fixture expectations
 * are internally inconsistent with the section 3.2 delimiter rule and are
 * marked in place (F03, F17); the spec-derived value is asserted there. F03's
 * colour indexes are also read per RTF 1.5 (positional from the "auto" entry
 * at index 0), not per the former one-based-after-auto mapping.
 *
 * Every accepted result is additionally checked for:
 *  - sanitize_style(style, scope) == style for every emitted style;
 *  - TextPaste::serialize()/parse() round-tripping to an identical fragment;
 *  - the absence of NUL/C0/DEL/newlines and invalid UTF-8 in run text.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <glib.h>

#include "ui/text-paste-rtf.h"
#include "ui/text-paste.h"

namespace TP = Inkscape::UI::TextPaste;
namespace Rtf = Inkscape::UI::TextPaste::Rtf;

namespace {

using Rtf::Diagnostics;
using Rtf::Limits;
using Rtf::Result;
using Rtf::Status;

/** One run: text and canonical style. */
using Runs = std::vector<std::pair<std::string, std::string>>;

// ---------------------------------------------------------------------------
// Header shorthands used by REPORT.md section 5.
// ---------------------------------------------------------------------------

std::string const H =
    R"RTF({\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fnil\fcharset0 Helvetica;}})RTF";
std::string const HA =
    R"RTF({\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}})RTF";
std::string const HT =
    R"RTF({\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\froman\fcharset0 Times New Roman;}{\f1\fswiss\fcharset0 Arial;}})RTF";

std::string const ARIAL16 = "font-family:Arial;font-size:16px;";
std::string const HELV16 = "font-family:Helvetica;font-size:16px;";
std::string const ARIAL_13 = "font-family:Arial;font-size:13.33333333px;";
std::string const ARIAL_14 = "font-family:Arial;font-size:14.66666667px;";

// UTF-8 literals (ASCII-only source file).
std::string const E_ACUTE = "\xC3\xA9";                 // U+00E9
std::string const LAMBDA = "\xCE\xBB";                  // U+03BB
std::string const SMILE = "\xF0\x9F\x98\x80";           // U+1F600
std::string const YAO = "\xE8\x80\x80";                 // U+8000
std::string const PRIVET = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
std::string const KONNICHIWA = "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF";
std::string const MIDDLE_DOT = "\xC2\xB7";              // U+00B7
std::string const REPLACEMENT = "\xEF\xBF\xBD";         // U+FFFD
std::string const NONCHARACTER = "\xEF\xBF\xBF";        // U+FFFF

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Result decode(std::string_view rtf)
{
    return Rtf::decode(rtf);
}

void expect_paragraph(Result const &result, std::size_t index, std::string const &style, Runs const &runs)
{
    ASSERT_LT(index, result.fragment.paragraphs.size()) << "paragraph " << index << " is missing";
    auto const &paragraph = result.fragment.paragraphs[index];
    EXPECT_EQ(paragraph.style, style) << "paragraph " << index << " style";
    ASSERT_EQ(paragraph.runs.size(), runs.size()) << "paragraph " << index << " run count";
    for (std::size_t i = 0; i < runs.size(); ++i) {
        EXPECT_EQ(paragraph.runs[i].text, runs[i].first) << "paragraph " << index << " run " << i << " text";
        EXPECT_EQ(paragraph.runs[i].style, runs[i].second) << "paragraph " << index << " run " << i << " style";
    }
}

/** No NUL, control character (except TAB), DEL, line break or invalid UTF-8. */
void expect_safe_text(Result const &result)
{
    for (auto const &paragraph : result.fragment.paragraphs) {
        for (auto const &run : paragraph.runs) {
            EXPECT_FALSE(run.text.empty()) << "empty runs must never be emitted";
            EXPECT_EQ(run.text.find('\n'), std::string::npos);
            EXPECT_EQ(run.text.find('\r'), std::string::npos);
            EXPECT_TRUE(g_utf8_validate(run.text.data(), static_cast<gssize>(run.text.size()), nullptr))
                << "run text is not valid UTF-8";
            for (unsigned char const c : run.text) {
                EXPECT_NE(c, 0);
                if (c < 0x20) {
                    EXPECT_EQ(c, '\t');
                }
                EXPECT_NE(c, 0x7f);
            }
        }
    }
    EXPECT_TRUE(g_utf8_validate(result.fragment.plain.data(),
                                static_cast<gssize>(result.fragment.plain.size()), nullptr));
    EXPECT_EQ(result.fragment.plain.find('\r'), std::string::npos);
}

void expect_same_fragment(TP::Fragment const &a, TP::Fragment const &b)
{
    ASSERT_EQ(a.paragraphs.size(), b.paragraphs.size());
    for (std::size_t i = 0; i < a.paragraphs.size(); ++i) {
        EXPECT_EQ(a.paragraphs[i].style, b.paragraphs[i].style) << "paragraph " << i;
        ASSERT_EQ(a.paragraphs[i].runs.size(), b.paragraphs[i].runs.size()) << "paragraph " << i;
        for (std::size_t j = 0; j < a.paragraphs[i].runs.size(); ++j) {
            EXPECT_EQ(a.paragraphs[i].runs[j].style, b.paragraphs[i].runs[j].style);
            EXPECT_EQ(a.paragraphs[i].runs[j].text, b.paragraphs[i].runs[j].text);
        }
    }
    EXPECT_EQ(a.plain, b.plain);
}

/** The section 5 invariants for one accepted decode. */
void expect_invariants(Result const &result)
{
    ASSERT_EQ(result.status, Status::ok);
    for (auto const &paragraph : result.fragment.paragraphs) {
        EXPECT_EQ(TP::sanitize_style(paragraph.style, true), paragraph.style);
    }
    for (auto const &paragraph : result.fragment.paragraphs) {
        for (auto const &run : paragraph.runs) {
            EXPECT_EQ(TP::sanitize_style(run.style, false), run.style);
        }
    }
    EXPECT_TRUE(TP::fragment_lengths_are_valid(result.fragment));
    expect_safe_text(result);

    std::string const payload = TP::serialize(result.fragment);
    ASSERT_LE(payload.size(), TP::MAX_PAYLOAD_BYTES) << "fragment does not fit the native wire format";
    auto parsed = TP::parse(payload);
    ASSERT_TRUE(parsed.has_value()) << "serialize() output does not parse back";
    expect_same_fragment(*parsed, result.fragment);
}

bool status_is_valid(Status status)
{
    return status == Status::ok || status == Status::malformed || status == Status::over_limit;
}

auto diagnostics_tuple(Diagnostics const &d)
{
    return std::make_tuple(
        d.groups_visited, d.skipped_destinations, d.pictures_skipped, d.objects_skipped, d.table_cells,
        d.table_rows, d.list_paragraphs, d.hidden_chars, d.line_breaks_flattened, d.layout_breaks,
        d.line_height_at_least_flattened, d.paragraph_style_refs, d.character_style_refs,
        d.unsupported_controls, d.dropped_style_declarations, d.truncated_font_names, d.codepage_fallbacks,
        d.unsafe_codepage, d.replacement_characters, d.surrogate_pairs_combined, d.surrogate_replacements,
        d.uc_clamped, d.fonts_referenced_before_table, d.split_runs, d.skipped_bytes_bin);
}

// ---------------------------------------------------------------------------
// Fixture bytes (REPORT.md section 5)
// ---------------------------------------------------------------------------

std::string const F01 = H + R"RTF(\f0\fs24 Hello world\par})RTF";
std::string const F02 = HA + R"RTF(\fs24 Normal {\b bold} normal\par})RTF";
std::string const F03 =
    HT + R"RTF({\colortbl;\red255\green0\blue0;\red0\green0\blue255;}\f1\fs20 red\cf2 RED\cf3 BLUE\par})RTF";
std::string const F04 = HA + R"RTF(\fs24 caf\'e9 \u233? \u955? \uc0\u55357\u56832\par})RTF";
std::string const F05 = HA + R"RTF(\fs24 \u-32768?\par})RTF";
std::string const F06 =
    R"RTF({\rtf1\ansi\ansicpg1251\deff0{\fonttbl{\f0\fnil\fcharset204 Arial;}}\f0\fs24 \'cf\'f0\'e8\'e2\'e5\'f2\par})RTF";
std::string const F07 =
    R"RTF({\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fnil\fcharset128 MS Gothic;}}\f0\fs24 \'82\'b1\'82\'f1\'82\'c9\'82\'bf\'82\'cd\par})RTF";
std::string const F08 =
    R"RTF({\rtf1\mac\deff0{\fonttbl{\f0\fnil\fcharset0 Helvetica;}}\f0\fs24 \'8e\par})RTF";
std::string const HA9999 =
    R"RTF({\rtf1\ansi\ansicpg9999\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}})RTF";
std::string const F09 = HA9999 + R"RTF(\fs24 \'e9\par})RTF";
std::string const F10 =
    HA + R"RTF(\pard\qc\li720\fi360\sb120\sa240\sl276\slmult1\f0\fs24 Title\par\pard\ql\fs20 Body\par})RTF";
std::string const F11 =
    HA + R"RTF(\pard\fs24 A\tab B\line C\emdash D\~E\-\_F\bullet G\lquote q\rdblquote\par})RTF";
std::string const F12 =
    R"RTF({\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 Real {\*\htmltag <b>x</b>}text{\*\generator Riched20;}{\info{\title Secret}}\par})RTF";
std::string const F13 =
    R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 A{\pict\pngblip\picw10\pich10 89504e470d0a1a0a}B{\*\datastore \bin9 {}Visible}C\par})RTF";
std::string const F14 =
    R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 Before {\object\objemb{\*\objclass Equation.DSMT4}{\*\objdata 0105000002000000}{\result{\fs20 x=1}}} after\par})RTF";
std::string const F15 =
    R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\trowd\cellx2000\cellx4000\pard\intbl\fs24 A\cell B\cell\row\trowd\cellx2000\cellx4000\pard\intbl\fs24 C\cell D\cell\row\pard\fs24 End\par})RTF";
std::string const F16 =
    R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs22{\pntext\f0 \'b7\tab}{\*\pn\pnlvlblt\pnf0{\pntxtb\'b7}}\pard\fi-360\li720 Item one\par})RTF";
std::string const F17 =
    R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 visible \v hidden\v0 tail\par})RTF";
std::string const F18 =
    R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 Lean {\field{\*\fldinst HYPERLINK "http://example.com/x"}{\fldrslt link}} text\par})RTF";
std::string const F19 =
    R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 \upr{ANSI}{\*\ud{Unicode \u233?}}\par})RTF";
std::string const F20 =
    R"RTF({\rtf1\ansi\ansicpg65001\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\f0\fs24 caf\'c3\'a9\par})RTF";

std::string const B07 =
    R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 A\bin10{\b X}\par B\par})RTF";
std::string const B09B = HA + R"RTF(\fs24 \u99999?\par})RTF";
std::string const B13 =
    R"RTF({\rtf1\ansi\ansicpg1200\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\f0\fs24 \'41\'00\'42\'00\par})RTF";
std::string const B17 =
    R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil A;}}\fs24 \par})RTF";

std::string deep_document(std::size_t depth)
{
    // \rtf1 must be the first control word of the outermost group
    // (REPORT.md 3.4), so the nesting starts inside the root group; `depth` is
    // the maximum number of simultaneously open groups.
    std::string rtf = R"RTF({\rtf1\ansi\deff0\fs24 )RTF";
    rtf.append(depth - 1, '{');
    rtf += 'X';
    rtf.append(depth - 1, '}');
    rtf += '}';
    return rtf;
}

std::string sequential_groups(std::size_t count)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil A;}}\fs24 X)RTF";
    for (std::size_t i = 0; i < count; ++i) {
        rtf += "{}";
    }
    rtf += "}";
    return rtf;
}

std::string bold_alternations(std::size_t count)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 )RTF";
    for (std::size_t i = 0; i < count; ++i) {
        rtf += R"RTF(\b x\b0 x)RTF";
    }
    rtf += R"RTF(\par})RTF";
    return rtf;
}

std::string one_long_run(std::size_t length)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 )RTF";
    rtf.append(length, 'A');
    rtf += R"RTF(\par})RTF";
    return rtf;
}

std::string emoji_run(std::size_t count)
{
    std::string rtf =
        R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\uc0\fs24 )RTF";
    for (std::size_t i = 0; i < count; ++i) {
        rtf += R"RTF(\u55357\u56832)RTF";
    }
    rtf += R"RTF(\par})RTF";
    return rtf;
}

std::string font_table(std::size_t entries)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0{\fonttbl)RTF";
    for (std::size_t i = 0; i < entries; ++i) {
        rtf += "{\\f" + std::to_string(i) + "\\fnil F" + std::to_string(i) + ";}";
    }
    rtf += R"RTF(}\f299\fs24 Text\par})RTF";
    return rtf;
}

/** Fixtures used by the determinism and fuzz sweeps. */
std::vector<std::string> fixture_corpus()
{
    return {
        F01, F02, F03, F04, F05, F06, F07, F08, F09, F10, F11, F12, F13, F14, F15, F16, F17, F18, F19, F20,
        B07, B09B, B13, B17,
        deep_document(8), sequential_groups(32), bold_alternations(8), one_long_run(64), emoji_run(4),
        font_table(3),
    };
}

} // namespace

// ---------------------------------------------------------------------------
// F01-F20
// ---------------------------------------------------------------------------

TEST(RtfFixture, F01DefaultFontAndSize)
{
    Result const r = decode(F01);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Hello world");
    ASSERT_EQ(r.fragment.paragraphs.size(), 1u);
    expect_paragraph(r, 0, "", { { "Hello world", HELV16 } });
    EXPECT_FALSE(r.has_meaningful_styles) << "one uniform family+size is not meaningful formatting";
    expect_invariants(r);
}

TEST(RtfFixture, F02GroupRestoresCharacterState)
{
    Result const r = decode(F02);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Normal bold normal");
    expect_paragraph(r, 0, "", {
        { "Normal ", ARIAL16 },
        { "bold", ARIAL16 + "font-weight:bold;" },
        { " normal", ARIAL16 },
    });
    expect_invariants(r);
}

TEST(RtfFixture, F03ColorTableEntryZeroIsTheAutoColor)
{
    Result const r = decode(F03);
    ASSERT_EQ(r.status, Status::ok);
    // REPORT.md section 5 shows "red RED BLUE" with runs " RED"/" BLUE". The
    // space after a control word is a consumed delimiter (REPORT.md 3.2, and
    // F02/F05/F10/F15/F16 all rely on it), so that expectation is a typo; the
    // spec-derived text is "redREDBLUE".
    EXPECT_EQ(r.fragment.plain, "redREDBLUE");
    // {\colortbl;\red255\green0\blue0;\red0\green0\blue255;} is positional from
    // index 0 and its empty first segment is the "auto" entry, so \cf1 is
    // #ff0000, \cf2 is #0000ff and \cf3 is past the end of the table (it keeps
    // the default colour and is counted as a dropped declaration).
    expect_paragraph(r, 0, "", {
        { "red", ARIAL_13 },
        { "RED", ARIAL_13 + "color:#0000ff;" },
        { "BLUE", ARIAL_13 },
    });
    EXPECT_GT(r.diagnostics.dropped_style_declarations, 0u) << "\\cf3 is out of range";
    expect_invariants(r);
}

TEST(RtfFixture, F04CodePageEscapeUnicodeEscapeAndSurrogatePair)
{
    Result const r = decode(F04);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "caf" + E_ACUTE + " " + E_ACUTE + " " + LAMBDA + " " + SMILE);
    expect_paragraph(r, 0, "", { { r.fragment.plain, ARIAL16 } });
    EXPECT_EQ(r.diagnostics.surrogate_pairs_combined, 1u);
    EXPECT_EQ(r.diagnostics.surrogate_replacements, 0u);
    EXPECT_EQ(r.diagnostics.replacement_characters, 0u);
    expect_invariants(r);
}

TEST(RtfFixture, F05NegativeUnicodeParameter)
{
    Result const r = decode(F05);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, YAO);
    expect_paragraph(r, 0, "", { { YAO, ARIAL16 } });
    EXPECT_EQ(r.diagnostics.replacement_characters, 0u);
    expect_invariants(r);
}

TEST(RtfFixture, F06DocumentCodePage1251)
{
    Result const r = decode(F06);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, PRIVET);
    expect_paragraph(r, 0, "", { { PRIVET, ARIAL16 } });
    EXPECT_EQ(r.diagnostics.codepage_fallbacks, 0u);
    expect_invariants(r);
}

TEST(RtfFixture, F07FontCodePageOverridesTheDocumentPage)
{
    Result const r = decode(F07);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, KONNICHIWA);
    expect_paragraph(r, 0, "", { { KONNICHIWA, "font-family:'MS Gothic';font-size:16px;" } });
    expect_invariants(r);
}

TEST(RtfFixture, F08MacRomanDocumentCharset)
{
    Result const r = decode(F08);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, E_ACUTE);
    expect_paragraph(r, 0, "", { { E_ACUTE, HELV16 } });
    EXPECT_EQ(r.diagnostics.codepage_fallbacks, 0u);
    expect_invariants(r);
}

TEST(RtfFixture, F09UnknownCodePageFallsBackToCp1252)
{
    Result const r = decode(F09);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, E_ACUTE);
    expect_paragraph(r, 0, "", { { E_ACUTE, ARIAL16 } });
    EXPECT_EQ(r.diagnostics.codepage_fallbacks, 1u);
    expect_invariants(r);
}

TEST(RtfFixture, F10ParagraphProperties)
{
    Result const r = decode(F10);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Title\nBody");
    ASSERT_EQ(r.fragment.paragraphs.size(), 2u);
    expect_paragraph(r, 0, "text-align:center;text-indent:24px;line-height:1.15;", { { "Title", ARIAL16 } });
    expect_paragraph(r, 1, "", { { "Body", ARIAL_13 } });
    EXPECT_EQ(r.diagnostics.line_height_at_least_flattened, 0u);
    expect_invariants(r);
}

TEST(RtfFixture, F11BreaksTabsAndSpecialCharacters)
{
    Result const r = decode(F11);
    ASSERT_EQ(r.status, Status::ok);
    std::string const second = "C\xE2\x80\x94" "D" "\xC2\xA0" "E" "\xC2\xAD" "\xE2\x80\x91" "F" "\xE2\x80\xA2"
                               "G" "\xE2\x80\x98" "q" "\xE2\x80\x9D";
    EXPECT_EQ(r.fragment.plain, "A\tB\n" + second);
    ASSERT_EQ(r.fragment.paragraphs.size(), 2u);
    expect_paragraph(r, 0, "", { { "A\tB", ARIAL16 } });
    expect_paragraph(r, 1, "", { { second, ARIAL16 } });
    EXPECT_EQ(r.diagnostics.line_breaks_flattened, 1u);
    expect_invariants(r);
}

TEST(RtfFixture, F12StarDestinationsAreSkipped)
{
    Result const r = decode(F12);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Real text");
    expect_paragraph(r, 0, "", { { "Real text", ARIAL16 } });
    EXPECT_EQ(r.diagnostics.skipped_destinations, 3u) << "htmltag, generator and info";
    EXPECT_EQ(r.fragment.plain.find("Secret"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("<b>"), std::string::npos);
    expect_invariants(r);
}

TEST(RtfFixture, F13PictureAndBinaryPayloadNeverBecomeText)
{
    Result const r = decode(F13);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "ABC");
    expect_paragraph(r, 0, "", { { "ABC", ARIAL16 } });
    EXPECT_EQ(r.diagnostics.pictures_skipped, 1u);
    EXPECT_EQ(r.diagnostics.skipped_bytes_bin, 9u);
    EXPECT_EQ(r.diagnostics.skipped_destinations, 2u) << "pict and \\*\\datastore";
    EXPECT_EQ(r.fragment.plain.find("Visible"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("89504e47"), std::string::npos);
    expect_invariants(r);
}

TEST(RtfFixture, F14ObjectRendersOnlyItsResult)
{
    Result const r = decode(F14);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Before x=1 after");
    expect_paragraph(r, 0, "", {
        { "Before ", ARIAL16 },
        { "x=1", ARIAL_13 },
        { " after", ARIAL16 },
    });
    EXPECT_EQ(r.diagnostics.objects_skipped, 1u);
    EXPECT_EQ(r.fragment.plain.find("objdata"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("0105000002000000"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("Equation"), std::string::npos);
    expect_invariants(r);
}

TEST(RtfFixture, F15TableRowsFlattenToTabSeparatedParagraphs)
{
    Result const r = decode(F15);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "A\tB\nC\tD\nEnd");
    ASSERT_EQ(r.fragment.paragraphs.size(), 3u);
    expect_paragraph(r, 0, "", { { "A\tB", ARIAL16 } });
    expect_paragraph(r, 1, "", { { "C\tD", ARIAL16 } });
    expect_paragraph(r, 2, "", { { "End", ARIAL16 } });
    EXPECT_EQ(r.diagnostics.table_cells, 4u);
    EXPECT_EQ(r.diagnostics.table_rows, 2u);
    expect_invariants(r);
}

TEST(RtfFixture, F16ListMarkerIsTheLiteralCodePageCharacter)
{
    Result const r = decode(F16);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, MIDDLE_DOT + "\tItem one");
    ASSERT_EQ(r.fragment.paragraphs.size(), 1u);
    expect_paragraph(r, 0, "text-indent:-24px;", { { MIDDLE_DOT + "\tItem one", ARIAL_14 } });
    EXPECT_EQ(r.diagnostics.list_paragraphs, 1u);
    expect_invariants(r);
}

TEST(RtfFixture, F17HiddenTextIsCountedButNotEmitted)
{
    Result const r = decode(F17);
    ASSERT_EQ(r.status, Status::ok);
    // REPORT.md section 5 shows two spaces; the space after \v0 is the control
    // word delimiter consumed by REPORT.md 3.2, so the spec-derived text has
    // one space.
    EXPECT_EQ(r.fragment.plain, "visible tail");
    expect_paragraph(r, 0, "", { { "visible tail", ARIAL16 } });
    EXPECT_EQ(r.diagnostics.hidden_chars, 6u);
    expect_invariants(r);
}

TEST(RtfFixture, F18FieldInstructionIsSkippedResultIsKept)
{
    Result const r = decode(F18);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Lean link text");
    expect_paragraph(r, 0, "", { { "Lean link text", ARIAL16 } });
    EXPECT_EQ(r.fragment.plain.find("http"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("HYPERLINK"), std::string::npos);
    expect_invariants(r);
}

TEST(RtfFixture, F19UnicodeAlternativeReadsTheAnsiBranch)
{
    Result const r = decode(F19);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "ANSI");
    expect_paragraph(r, 0, "", { { "ANSI", ARIAL16 } });
    EXPECT_EQ(r.fragment.plain.find("Unicode"), std::string::npos);
    expect_invariants(r);
}

TEST(RtfFixture, F20AnsiCodePage65001IsDirectUtf8)
{
    Result const r = decode(F20);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "caf" + E_ACUTE);
    expect_paragraph(r, 0, "", { { "caf" + E_ACUTE, ARIAL16 } });
    EXPECT_EQ(r.diagnostics.codepage_fallbacks, 0u);
    EXPECT_EQ(r.diagnostics.replacement_characters, 0u);
    expect_invariants(r);
}

// ---------------------------------------------------------------------------
// B01-B18
// ---------------------------------------------------------------------------

TEST(RtfBoundary, B01GroupDepth)
{
    Result const ok = decode(deep_document(64));
    ASSERT_EQ(ok.status, Status::ok);
    EXPECT_EQ(ok.fragment.plain, "X");

    Result const over = decode(deep_document(65));
    EXPECT_EQ(over.status, Status::over_limit);
    EXPECT_TRUE(over.fragment.paragraphs.empty());
    EXPECT_FALSE(over.error.empty());
}

TEST(RtfBoundary, B02VisitedGroups)
{
    Result const r = decode(sequential_groups(40000));
    EXPECT_EQ(r.status, Status::over_limit);
    EXPECT_TRUE(r.fragment.paragraphs.empty());
    EXPECT_GE(r.diagnostics.groups_visited, std::size_t(32768));
}

TEST(RtfBoundary, B03RunCountIsAHardLimit)
{
    Result const r = decode(bold_alternations(5000));
    EXPECT_EQ(r.status, Status::over_limit);
    EXPECT_TRUE(r.fragment.paragraphs.empty()) << "never a truncated prefix";
}

TEST(RtfBoundary, B04LongRunSplitsWithoutLoss)
{
    Result const r = decode(one_long_run(9000));
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain.size(), 9000u);
    EXPECT_EQ(r.fragment.plain, std::string(9000, 'A'));
    ASSERT_EQ(r.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(r.fragment.paragraphs[0].runs.size(), 2u);
    EXPECT_EQ(r.fragment.paragraphs[0].runs[0].text.size(), 8192u);
    EXPECT_EQ(r.fragment.paragraphs[0].runs[1].text.size(), 808u);
    EXPECT_EQ(r.fragment.paragraphs[0].runs[0].style, r.fragment.paragraphs[0].runs[1].style);
    EXPECT_EQ(r.diagnostics.split_runs, 1u);
    expect_invariants(r);
}

TEST(RtfBoundary, B05TextByteCap)
{
    Result const at_cap = decode(emoji_run(16384));
    ASSERT_EQ(at_cap.status, Status::ok);
    EXPECT_EQ(at_cap.fragment.plain.size(), 65536u);
    EXPECT_GT(at_cap.diagnostics.split_runs, 0u);

    Result const over = decode(emoji_run(16385));
    EXPECT_EQ(over.status, Status::over_limit);
    EXPECT_TRUE(over.fragment.paragraphs.empty());
}

TEST(RtfBoundary, B06SplitNeverCutsAUtf8Sequence)
{
    std::string rtf =
        R"RTF({\rtf1\ansi\ansicpg65001\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 )RTF";
    rtf.append(8191, 'A');
    rtf += R"RTF(\'f0\'9f\'98\'80\par})RTF";

    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, std::string(8191, 'A') + SMILE);
    ASSERT_EQ(r.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(r.fragment.paragraphs[0].runs.size(), 2u);
    EXPECT_EQ(r.fragment.paragraphs[0].runs[0].text.size(), 8191u);
    EXPECT_EQ(r.fragment.paragraphs[0].runs[1].text, SMILE);
    EXPECT_EQ(r.diagnostics.split_runs, 1u);
    expect_invariants(r);
}

TEST(RtfBoundary, B07BinSkipsBracesAndControlsByteExactly)
{
    Result const r = decode(B07);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "A B");
    ASSERT_EQ(r.fragment.paragraphs.size(), 1u);
    expect_paragraph(r, 0, "", { { "A B", ARIAL16 } });
    EXPECT_EQ(r.diagnostics.skipped_bytes_bin, 10u);
    expect_invariants(r);
}

TEST(RtfBoundary, B08BinLongerThanTheInputIsMalformed)
{
    std::string const rtf =
        R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil A;}}\fs24 X\bin999999ABCDEFGHIJ})RTF";
    Result const r = decode(rtf);
    EXPECT_EQ(r.status, Status::malformed);
    EXPECT_TRUE(r.fragment.paragraphs.empty());
}

TEST(RtfBoundary, B09BinWithoutCountAndOutOfRangeUnicode)
{
    std::string const no_count =
        R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil A;}}\fs24 X\bin})RTF";
    EXPECT_EQ(decode(no_count).status, Status::malformed);

    Result const unicode = decode(B09B);
    ASSERT_EQ(unicode.status, Status::ok);
    EXPECT_EQ(unicode.fragment.plain, REPLACEMENT);
    EXPECT_EQ(unicode.diagnostics.replacement_characters, 1u);
    expect_invariants(unicode);
}

TEST(RtfBoundary, B10OverlongControlWordIsMalformed)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil A;}}\fs24 X\)RTF";
    rtf.append(33, 'a');
    rtf += R"RTF( Y\par})RTF";
    EXPECT_EQ(decode(rtf).status, Status::malformed);
}

TEST(RtfBoundary, B11UnbalancedBracesAreMalformed)
{
    EXPECT_EQ(decode(R"RTF({\rtf1\ansi)RTF").status, Status::malformed);
    EXPECT_EQ(decode("{\\rtf1\\ansi}}").status, Status::malformed);
    EXPECT_EQ(decode(R"RTF(\rtf1\ansi)RTF").status, Status::malformed);
    EXPECT_EQ(decode("").status, Status::malformed);
}

TEST(RtfBoundary, B12UcClampNeverSwallowsTheDocument)
{
    std::string rtf = HA + R"RTF(\fs24 \uc50\u233)RTF";
    rtf.append(20, '?');
    rtf += R"RTF(\par})RTF";

    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, E_ACUTE + "????");
    EXPECT_EQ(r.diagnostics.uc_clamped, 1u);
    expect_invariants(r);
}

TEST(RtfBoundary, B13Utf16CodePageNeverInjectsNul)
{
    Result const r = decode(B13);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "AB");
    EXPECT_EQ(r.diagnostics.unsafe_codepage, 1u);
    expect_invariants(r);
}

TEST(RtfBoundary, B14FontTableCap)
{
    Result const r = decode(font_table(300));
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Text");
    expect_paragraph(r, 0, "", { { "Text", "font-size:16px;" } });
    EXPECT_GT(r.diagnostics.dropped_style_declarations, 0u);
    expect_invariants(r);
}

TEST(RtfBoundary, B15ColorIndexOutOfRange)
{
    std::string const rtf =
        R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}{\colortbl;\red255\green0\blue0;\red0\green0\blue255;}\f0\fs24 A\cf999 B\par})RTF";
    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "AB");
    expect_paragraph(r, 0, "", { { "AB", ARIAL16 } });
    EXPECT_GT(r.diagnostics.dropped_style_declarations, 0u);
    expect_invariants(r);
}

TEST(RtfBoundary, B16OverlongFontNameIsDropped)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil )RTF";
    rtf.append(300, 'a');
    rtf += R"RTF(;}}\f0\fs24 Text\par})RTF";

    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Text");
    expect_paragraph(r, 0, "", { { "Text", "font-size:16px;" } });
    EXPECT_EQ(r.diagnostics.truncated_font_names, 1u);
    expect_invariants(r);
}

TEST(RtfBoundary, B17NoTextYieldsAnEmptyButValidFragment)
{
    Result const r = decode(B17);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_TRUE(r.fragment.empty());
    EXPECT_FALSE(r.has_meaningful_styles);
    expect_invariants(r);
}

TEST(RtfBoundary, B18StyleBudgetPressureKeepsTextAndCoreStyles)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil )RTF";
    // A name outside [A-Za-z0-9_-] (here a space) must be emitted quoted per
    // REPORT.md 3.7; a bare-word name stays unquoted.
    rtf += std::string(249, 'A') + " B";
    rtf += R"RTF(;}}{\colortbl;\red255\green0\blue0;}\f0\fs28\b\i\uldb\strike\caps\scaps\super\cf5\expndtw40\kerning24)RTF";
    for (int i = 0; i < 200; ++i) {
        rtf += R"RTF(\highlight1)RTF"; // ~2 KB of unsupported highlighting pressure
    }
    rtf += R"RTF( Text\par})RTF";

    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Text");
    ASSERT_EQ(r.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(r.fragment.paragraphs[0].runs.size(), 1u);
    std::string const &style = r.fragment.paragraphs[0].runs[0].style;
    EXPECT_LE(style.size(), TP::MAX_STYLE_LENGTH);
    EXPECT_NE(style.find("font-size:18.66666667px;"), std::string::npos) << style;
    EXPECT_NE(style.find("font-family:'"), std::string::npos) << style;
    EXPECT_GT(r.diagnostics.dropped_style_declarations, 0u) << "out-of-range \\cf5 must be dropped, not emitted";
    EXPECT_GT(r.diagnostics.unsupported_controls, 0u);
    expect_invariants(r);
}

// ---------------------------------------------------------------------------
// Security-review repairs: length-parameter bounds and colour-table indexing
// ---------------------------------------------------------------------------

TEST(RtfBoundary, LengthParameterMagnitudeBoundIsInclusive)
{
    // The documented bound is 32767 (the largest magnitude an RTF writer stores
    // in the signed 16-bit twips/half-points/quarter-points fields). The bound
    // itself is still a layout request and must convert. Every expected number
    // below is the literal "%.10g" CSS conversion of 32767: half-points ->
    // 21844.66667px, quarter-points -> 10922.33333px, twips -> 2184.466667px
    // and \slmult1 -> 32767/240 = 136.5291667.
    struct Case
    {
        std::string body;
        std::string paragraph_style;
        std::string run_style;
    };
    std::vector<Case> const cases = {
        { R"RTF(\f0\fs32767 X\par})RTF", "", "font-family:Arial;font-size:21844.66667px;" },
        { R"RTF(\f0\fs24\up32767 X\par})RTF", "", ARIAL16 + "baseline-shift:21844.66667px;" },
        { R"RTF(\f0\fs24\dn32767 X\par})RTF", "", ARIAL16 + "baseline-shift:-21844.66667px;" },
        { R"RTF(\f0\fs24\expnd32767 X\par})RTF", "", ARIAL16 + "letter-spacing:10922.33333px;" },
        { R"RTF(\f0\fs24\expndtw32767 X\par})RTF", "", ARIAL16 + "letter-spacing:2184.466667px;" },
        { R"RTF(\f0\fs24\kerning32767 X\par})RTF", "", ARIAL16 + "kerning:21844.66667px;" },
        { R"RTF(\f0\fs24\fi32767 X\par})RTF", "text-indent:2184.466667px;", ARIAL16 },
        { R"RTF(\f0\fs24\fi-32767 X\par})RTF", "text-indent:-2184.466667px;", ARIAL16 },
        { R"RTF(\f0\fs24\sl32767 X\par})RTF", "line-height:2184.466667px;", ARIAL16 },
        { R"RTF(\f0\fs24\sl32767\slmult1 X\par})RTF", "line-height:136.5291667;", ARIAL16 },
    };
    for (auto const &c : cases) {
        SCOPED_TRACE(c.body);
        Result const r = decode(HA + c.body);
        ASSERT_EQ(r.status, Status::ok);
        expect_paragraph(r, 0, c.paragraph_style, { { "X", c.run_style } });
        expect_invariants(r);
    }
}

TEST(RtfBoundary, LengthParameterOverTheBoundIsDropped)
{
    // 32768, -32768 and every astronomically large value named in the review
    // (\fs2000000000 and its \up/\dn/\expnd/\expndtw/\fi/\sl/\kerning shapes)
    // are past the bound: no length declaration may be emitted, the text keeps
    // its surrounding style and nothing may wrap around into a small or
    // negative length.
    struct Case
    {
        std::string body;
        std::string run_style;
    };
    std::vector<Case> const cases = {
        { R"RTF(\f0\fs32768 X\par})RTF", "font-family:Arial;" },
        { R"RTF(\f0\fs2000000000 X\par})RTF", "font-family:Arial;" },
        { R"RTF(\f0\fs2147483647 X\par})RTF", "font-family:Arial;" },
        { R"RTF(\f0\fs24\up32768 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\up2000000000 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\dn32768 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\dn2000000000 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\expnd32768 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\expnd2000000000 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\expndtw32768 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\expndtw-32768 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\expndtw2000000000 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\kerning32768 X\par})RTF", ARIAL16 + "font-kerning:none;" },
        { R"RTF(\f0\fs24\kerning2000000000 X\par})RTF", ARIAL16 + "font-kerning:none;" },
        { R"RTF(\f0\fs24\fi32768 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\fi-32768 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\fi2000000000 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\sl32768 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\sl32768\slmult1 X\par})RTF", ARIAL16 },
        { R"RTF(\f0\fs24\sl2000000000 X\par})RTF", ARIAL16 },
    };
    for (auto const &c : cases) {
        SCOPED_TRACE(c.body);
        Result const r = decode(HA + c.body);
        ASSERT_EQ(r.status, Status::ok);
        expect_paragraph(r, 0, "", { { "X", c.run_style } });
        EXPECT_GT(r.diagnostics.dropped_style_declarations, 0u) << "the rejected value must be counted";
        expect_invariants(r);
    }
}

TEST(RtfBoundary, NegativeAndMalformedLengthParametersEmitNoDeclaration)
{
    // Negative values are only meaningful for condensing (\expnd/\expndtw) and
    // hanging first-line indents (\fi). A negative \sl is the RTF "at least"
    // form, which cannot be flattened to an exact CSS line-height, so it is
    // dropped rather than emitted as a negative (invalid) length.
    Result const fs = decode(HA + R"RTF(\f0\fs-24 X\par})RTF");
    ASSERT_EQ(fs.status, Status::ok);
    expect_paragraph(fs, 0, "", { { "X", "font-family:Arial;" } });
    EXPECT_GT(fs.diagnostics.dropped_style_declarations, 0u);
    expect_invariants(fs);

    Result const absent = decode(HA + R"RTF(\f0\fs X\par})RTF");
    ASSERT_EQ(absent.status, Status::ok);
    expect_paragraph(absent, 0, "", { { "X", "font-family:Arial;" } });
    EXPECT_GT(absent.diagnostics.dropped_style_declarations, 0u);
    expect_invariants(absent);

    Result const up = decode(HA + R"RTF(\f0\fs24\up-12 X\par})RTF");
    ASSERT_EQ(up.status, Status::ok);
    expect_paragraph(up, 0, "", { { "X", ARIAL16 } });
    expect_invariants(up);

    Result const dn = decode(HA + R"RTF(\f0\fs24\dn-12 X\par})RTF");
    ASSERT_EQ(dn.status, Status::ok);
    expect_paragraph(dn, 0, "", { { "X", ARIAL16 } });
    expect_invariants(dn);

    Result const sl = decode(HA + R"RTF(\f0\fs24\sl-240 X\par})RTF");
    ASSERT_EQ(sl.status, Status::ok);
    expect_paragraph(sl, 0, "", { { "X", ARIAL16 } });
    EXPECT_GT(sl.diagnostics.dropped_style_declarations, 0u);
    expect_invariants(sl);

    Result const kerning = decode(HA + R"RTF(\f0\fs24\kerning-24 X\par})RTF");
    ASSERT_EQ(kerning.status, Status::ok);
    expect_paragraph(kerning, 0, "", { { "X", ARIAL16 + "font-kerning:none;" } });
    expect_invariants(kerning);

    // Bounded negative values keep their meaning.
    Result const condense = decode(HA + R"RTF(\f0\fs24\expnd-400 X\par})RTF");
    ASSERT_EQ(condense.status, Status::ok);
    expect_paragraph(condense, 0, "", { { "X", ARIAL16 + "letter-spacing:-133.3333333px;" } });
    expect_invariants(condense);

    Result const hanging = decode(HA + R"RTF(\f0\fs24\fi-360 X\par})RTF");
    ASSERT_EQ(hanging.status, Status::ok);
    expect_paragraph(hanging, 0, "text-indent:-24px;", { { "X", ARIAL16 } });
    expect_invariants(hanging);
}

TEST(RtfBoundary, ColorTableEntryZeroIsAutoAndIndexesArePositional)
{
    // {\colortbl;\red255\green0\blue0;\red0\green0\blue255;} is positional from
    // index 0 per RTF 1.5: 0 = auto (the empty first segment), 1 = #ff0000,
    // 2 = #0000ff. \cf0 must emit no colour at all and must not count as a
    // dropped declaration.
    std::string const rtf = HA +
        R"RTF({\colortbl;\red255\green0\blue0;\red0\green0\blue255;}\f0\fs24 \cf0 A\cf1 B\cf2 C\par})RTF";
    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "ABC");
    expect_paragraph(r, 0, "", {
        { "A", ARIAL16 },
        { "B", ARIAL16 + "color:#ff0000;" },
        { "C", ARIAL16 + "color:#0000ff;" },
    });
    EXPECT_EQ(r.diagnostics.dropped_style_declarations, 0u)
        << "\\cf0 is auto and \\cf1/\\cf2 are inside the table";
    expect_invariants(r);

    // A negative index is malformed and behaves like auto.
    Result const negative = decode(
        HA + R"RTF({\colortbl;\red255\green0\blue0;}\f0\fs24 \cf-1 A\par})RTF");
    ASSERT_EQ(negative.status, Status::ok);
    expect_paragraph(negative, 0, "", { { "A", ARIAL16 } });
    expect_invariants(negative);
}

TEST(RtfBoundary, WordDefaultColorTableIsIndexedFromTheAutoEntry)
{
    // Word's classic colour table starts with the auto entry and then the
    // standard colours; these are its first five entries (auto, black, blue,
    // green, red), with \cf5 past the end of the table.
    std::string const rtf = HA +
        R"RTF({\colortbl;\red0\green0\blue0;\red0\green0\blue255;\red0\green255\blue0;\red255\green0\blue0;}\f0\fs24 \cf0 A\cf1 B\cf2 C\cf3 D\cf4 E\cf5 F\par})RTF";
    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "ABCDEF");
    expect_paragraph(r, 0, "", {
        { "A", ARIAL16 },
        { "B", ARIAL16 + "color:#000000;" },
        { "C", ARIAL16 + "color:#0000ff;" },
        { "D", ARIAL16 + "color:#00ff00;" },
        { "E", ARIAL16 + "color:#ff0000;" },
        { "F", ARIAL16 },
    });
    EXPECT_GT(r.diagnostics.dropped_style_declarations, 0u) << "\\cf5 is past the end of the table";
    expect_invariants(r);
}

TEST(RtfBoundary, ColorIndexPastTheEndOfTheTableIsDropped)
{
    std::string const rtf = HA +
        R"RTF({\colortbl;\red255\green0\blue0;\red0\green0\blue255;}\f0\fs24 A\cf99 B\par})RTF";
    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "AB");
    expect_paragraph(r, 0, "", { { "AB", ARIAL16 } });
    EXPECT_GT(r.diagnostics.dropped_style_declarations, 0u) << "\\cf99 is past the 3-entry table";
    expect_invariants(r);
}

// ---------------------------------------------------------------------------
// Explicit adversarial cases from the task contract
// ---------------------------------------------------------------------------

TEST(RtfAdversarial, UnicodeValueEdges)
{
    Result const minus_one = decode(HA + R"RTF(\fs24 \u-1?\par})RTF");
    ASSERT_EQ(minus_one.status, Status::ok);
    EXPECT_EQ(minus_one.fragment.plain, NONCHARACTER);
    expect_invariants(minus_one);

    // Without \uc0 the second escape is the (skipped) ANSI fallback of the
    // first, so the unpaired high surrogate becomes U+FFFD at the break.
    Result const pair = decode(HA + R"RTF(\fs24 \u55357\u56832\par})RTF");
    ASSERT_EQ(pair.status, Status::ok);
    EXPECT_EQ(pair.fragment.plain, REPLACEMENT);
    EXPECT_EQ(pair.diagnostics.surrogate_replacements, 1u);
    expect_invariants(pair);

    Result const paired = decode(HA + R"RTF(\fs24 \uc0\u55357\u56832\par})RTF");
    ASSERT_EQ(paired.status, Status::ok);
    EXPECT_EQ(paired.fragment.plain, SMILE);
    EXPECT_EQ(paired.diagnostics.surrogate_pairs_combined, 1u);
    expect_invariants(paired);

    Result const huge = decode(HA + R"RTF(\fs24 \u999999999?\par})RTF");
    ASSERT_EQ(huge.status, Status::ok);
    EXPECT_EQ(huge.fragment.plain, REPLACEMENT);
    EXPECT_EQ(huge.diagnostics.replacement_characters, 1u);
    expect_invariants(huge);
}

TEST(RtfAdversarial, FontEntryDestinationsDoNotBreakTheFontTable)
{
    // Word emits {\*\panose ...} and {\*\fname ...} inside a font entry; their
    // close/semicolon must not commit or clobber the entry (REPORT.md 3.4, 3.12).
    std::string const rtf =
        R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\froman\fcharset0\fprq2{\*\panose 02020603050405020304}{\*\fname Times New Roman;}Times New Roman;}}\f0\fs24 Text\par})RTF";
    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "Text");
    expect_paragraph(r, 0, "", { { "Text", "font-family:'Times New Roman';font-size:16px;" } });
    EXPECT_EQ(r.fragment.plain.find("0202"), std::string::npos);
    expect_invariants(r);
}

TEST(RtfAdversarial, NulInsideAByteRunNeverTruncatesLaterText)
{
    // \'00 in a normal code page converts to a NUL that must be dropped
    // (REPORT.md 3.5), never truncate the rest of the byte run.
    Result const r = decode(HA + R"RTF(\f0\fs24 A\'00B\par})RTF");
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "AB");
    EXPECT_GT(r.diagnostics.replacement_characters, 0u);
    expect_invariants(r);
}

TEST(RtfAdversarial, UnicodeEscapeProducingAControlIsReplaced)
{
    // \u13 / \u0 cannot appear in run text; they become U+FFFD instead of
    // rejecting the whole document (REPORT.md 3.5/3.6).
    Result const r = decode(HA + R"RTF(\fs24 A\u13?\u0?B\par})RTF");
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "A" + REPLACEMENT + REPLACEMENT + "B");
    EXPECT_EQ(r.diagnostics.replacement_characters, 2u);
    expect_invariants(r);
}

TEST(RtfAdversarial, ByteRunBeforeUnicodeEscapeKeepsInputOrder)
{
    // A pending \'hh byte run must keep its position when a \u escape follows.
    Result const r = decode(HA + R"RTF(\fs24 \'e9\u233?\'e9\u99999?\par})RTF");
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, E_ACUTE + E_ACUTE + E_ACUTE + REPLACEMENT);
    EXPECT_EQ(r.diagnostics.replacement_characters, 1u);
    expect_invariants(r);
}

TEST(RtfAdversarial, UcClampAndFontSizeEdges)
{
    std::string capped = HA + R"RTF(\fs24 \uc65535\u233)RTF";
    capped.append(20, '?');
    capped += R"RTF(\par})RTF";
    Result const clamp = decode(capped);
    ASSERT_EQ(clamp.status, Status::ok);
    EXPECT_EQ(clamp.fragment.plain, E_ACUTE + "????");
    EXPECT_EQ(clamp.diagnostics.uc_clamped, 1u);
    expect_invariants(clamp);

    Result const zero = decode(HA + R"RTF(\f0\fs0 X\par})RTF");
    ASSERT_EQ(zero.status, Status::ok);
    EXPECT_EQ(zero.fragment.plain, "X");
    expect_paragraph(zero, 0, "", { { "X", "font-family:Arial;" } });
    EXPECT_GT(zero.diagnostics.dropped_style_declarations, 0u);
    expect_invariants(zero);

    Result const huge = decode(HA + R"RTF(\f0\fs2000000000 X\par})RTF");
    ASSERT_EQ(huge.status, Status::ok);
    EXPECT_EQ(huge.fragment.plain, "X");
    // 2000000000 half-points is far past the documented 32767 magnitude bound,
    // so the declaration is dropped (no font-size at all) instead of becoming
    // the former font-size:1333333333px.
    expect_paragraph(huge, 0, "", { { "X", "font-family:Arial;" } });
    EXPECT_GT(huge.diagnostics.dropped_style_declarations, 0u);
    expect_invariants(huge);
}

TEST(RtfAdversarial, PictPayloadWithBracesAndBackslashes)
{
    std::string const rtf =
        R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 A{\pict\wmetafile8\picw100\pich100 010009000003{\b X}\par\bin3 abc}B\par})RTF";
    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "AB");
    EXPECT_EQ(r.diagnostics.pictures_skipped, 1u);
    EXPECT_EQ(r.fragment.plain.find("X"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("abc"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("0100"), std::string::npos);
    expect_invariants(r);
}

TEST(RtfAdversarial, FieldInstructionAndObjectDataNeverLeak)
{
    std::string const rtf =
        R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 {\field{\*\fldinst INCLUDEPICTURE "http://example.com/x.png"}{\fldrslt X}} {\object\objemb{\*\objclass Equation.DSMT4}{\*\objdata 0105000002000000}{\result Y}} Z\par})RTF";
    Result const r = decode(rtf);
    ASSERT_EQ(r.status, Status::ok);
    EXPECT_EQ(r.fragment.plain, "X Y Z");
    EXPECT_EQ(r.fragment.plain.find("INCLUDEPICTURE"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("http"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("objdata"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("0105"), std::string::npos);
    EXPECT_EQ(r.fragment.plain.find("Equation"), std::string::npos);
    expect_invariants(r);
}

TEST(RtfAdversarial, MalformedInputsStillUseOnlyTheThreeValuedStatus)
{
    std::vector<std::string> const inputs = {
        "",
        "{",
        "}",
        "{\\rtf1\\ansi",
        "{\\rtf1\\ansi}}",
        "{\\rtf1\\ansi\\'zz}",
        "{\\rtf1\\ansi\\'4}",
        "{\\rtf1\\ansi\\bin5}",
        "{\\rtf1\\ansi\\bin99999999999999}",
        "{\\rtf1\\ansi\\u}",
        "{\\rtf1\\ansi\\fs}",
        "{\\rtf1\\ansi\\fs-}",
        "{\\rtf1\\ansi 12345678901234}",
        "{\\rtf2\\ansi}",
        "{\\ansi\\rtf1}",
    };
    for (auto const &rtf : inputs) {
        Result const r = decode(rtf);
        EXPECT_TRUE(status_is_valid(r.status)) << rtf;
        if (r.status == Status::ok) {
            expect_invariants(r);
        } else {
            EXPECT_FALSE(r.error.empty()) << rtf;
        }
    }
}

// ---------------------------------------------------------------------------
// Cross-cutting invariants
// ---------------------------------------------------------------------------

TEST(RtfInvariants, EveryFixtureIsSanitizerStableAndRoundTrips)
{
    for (auto const &rtf : fixture_corpus()) {
        Result const r = decode(rtf);
        ASSERT_EQ(r.status, Status::ok) << rtf;
        expect_invariants(r);
    }
}

TEST(RtfInvariants, SameBytesAndLimitsAreByteIdentical)
{
    for (auto const &rtf : fixture_corpus()) {
        Result const a = decode(rtf);
        Result const b = decode(rtf);
        EXPECT_EQ(a.status, b.status);
        EXPECT_EQ(a.error, b.error);
        EXPECT_EQ(a.has_meaningful_styles, b.has_meaningful_styles);
        EXPECT_EQ(diagnostics_tuple(a.diagnostics), diagnostics_tuple(b.diagnostics));
        EXPECT_EQ(TP::serialize(a.fragment), TP::serialize(b.fragment));
        EXPECT_EQ(a.fragment.plain, b.fragment.plain);
    }
}

TEST(RtfInvariants, ShrunkLimitsStillProduceSafeResults)
{
    Limits small;
    small.max_input_bytes = 4096;
    small.max_text_bytes = 64;
    small.max_paragraphs = 4;
    small.max_runs = 8;
    small.max_run_bytes = 16;
    small.max_depth = 6;
    small.max_groups = 32;
    small.max_fonts = 2;
    small.max_colors = 2;

    for (auto const &rtf : fixture_corpus()) {
        Result const r = Rtf::decode(rtf, small);
        EXPECT_TRUE(status_is_valid(r.status)) << rtf;
        if (r.status != Status::ok) {
            EXPECT_TRUE(r.fragment.paragraphs.empty());
            continue;
        }
        EXPECT_LE(r.fragment.plain.size(), small.max_text_bytes);
        EXPECT_LE(r.fragment.paragraphs.size(), small.max_paragraphs);
        std::size_t runs = 0;
        for (auto const &paragraph : r.fragment.paragraphs) {
            runs += paragraph.runs.size();
            for (auto const &run : paragraph.runs) {
                EXPECT_LE(run.text.size(), small.max_run_bytes);
            }
        }
        EXPECT_LE(runs, small.max_runs);
        expect_invariants(r);
    }

    Limits no_input;
    no_input.max_input_bytes = 4;
    for (auto const &rtf : fixture_corpus()) {
        Result const r = Rtf::decode(rtf, no_input);
        if (rtf.size() > no_input.max_input_bytes) {
            EXPECT_EQ(r.status, Status::over_limit);
            EXPECT_TRUE(r.fragment.paragraphs.empty());
        }
    }
}

TEST(RtfFuzz, SeededByteMutationKeepsStatusThreeValuedAndTextSafe)
{
    std::mt19937_64 rng(0xC0FFEE5EEDull);
    auto const corpus = fixture_corpus();
    Limits const small = [] {
        Limits limits;
        limits.max_input_bytes = 8192;
        limits.max_text_bytes = 256;
        limits.max_paragraphs = 8;
        limits.max_runs = 16;
        limits.max_run_bytes = 32;
        limits.max_depth = 8;
        limits.max_groups = 64;
        limits.max_fonts = 4;
        limits.max_colors = 4;
        return limits;
    }();

    for (auto const &base : corpus) {
        for (int iteration = 0; iteration < 24; ++iteration) {
            std::string mutated = base;
            std::size_t const mutations = 1 + static_cast<std::size_t>(rng() % 6);
            for (std::size_t i = 0; i < mutations && !mutated.empty(); ++i) {
                std::size_t const index = static_cast<std::size_t>(rng() % mutated.size());
                mutated[index] = static_cast<char>(rng() & 0xFFu);
            }

            Result const plain = decode(mutated);
            EXPECT_TRUE(status_is_valid(plain.status));
            if (plain.status == Status::ok) {
                expect_invariants(plain);
            }

            Result const bounded = Rtf::decode(mutated, small);
            EXPECT_TRUE(status_is_valid(bounded.status));
            if (bounded.status == Status::ok) {
                EXPECT_LE(bounded.fragment.plain.size(), small.max_text_bytes);
                expect_invariants(bounded);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Literal default-limit boundaries
// ---------------------------------------------------------------------------
//
// The default Limits of ui/text-paste-rtf.h are documented as
//   max_input_bytes = MAX_PAYLOAD_BYTES = 262,144 (256 KiB)
//   max_paragraphs  = MAX_PARAGRAPHS    = 1,024
//   max_runs        = MAX_RUNS          = 4,096
//   max_fonts       = 256 ;  max_colors = 256
// and the value bounds are MAX_FONT_NAME_BYTES = 255 (decoder-local),
// MAX_STYLE_VALUE_LENGTH = 256 and MAX_STYLE_LENGTH = 2048 (text-paste.h).
//
// Every expectation below counts the accepted units with a literal (262144,
// 1024, 4096, 256, 255, 256) and is not written as a comparison with the
// production constant: a test that reads MAX_RUNS would silently follow a
// changed default and would still pass with a much smaller cap. The
// over-boundary cases assert the exact over_limit status, the exact rejecting
// reason and an unusable (empty) fragment.
//
// Unpinned gap, deliberately not invented here: MAX_STYLE_LENGTH (2048) is
// UNREACHABLE through the public RTF decode API. build_char_style() emits at
// most one declaration per name (13 possible names) and the only
// variable-length one, font-family, is itself capped at
// MAX_STYLE_VALUE_LENGTH = 256 bytes by StyleBuilder::add(); the union of every
// declaration it can emit is under 600 bytes, and build_para_style() adds at
// most three more. No RTF input
// can produce a canonical style near 2048 bytes, so there is no honest
// boundary fixture for that default in this suite (the HTML suite pins it for
// the HTML path).

namespace {

/** `{\rtf1...\fs24 X{\*\generator AAAA...}\par}` with `filler` skipped bytes. */
std::string input_cap_fixture(std::size_t filler)
{
    std::string rtf =
        R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 X{\*\generator )RTF";
    rtf.append(filler, 'A');
    rtf += R"RTF(}\par})RTF";
    return rtf;
}

/** One paragraph holding the text "x", ended by \par, per unit. */
std::string paragraph_cap_fixture(std::size_t paragraphs)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}\fs24 )RTF";
    for (std::size_t i = 0; i < paragraphs; ++i) {
        rtf += R"RTF(x\par )RTF";
    }
    rtf += '}';
    return rtf;
}

/**
 * Exactly one run per unit. Unit i switches bold (\b for even i, \b0 for odd i)
 * and then emits one character, so every unit changes the canonical run style
 * and append_run_text() must close the previous run; the trailing \par closes
 * the last one. Run count == unit count (verified by the assertions).
 */
std::string run_cap_fixture(std::size_t units)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0\fs24 )RTF";
    for (std::size_t i = 0; i < units; ++i) {
        rtf += (i % 2 == 0) ? R"RTF(\b a)RTF" : R"RTF(\b0 a)RTF";
    }
    rtf += R"RTF(\par})RTF";
    return rtf;
}

/** `entries` font-table entries ({\fN\fnil FN;}), then `\f<reference>` text. */
std::string font_cap_fixture(std::size_t entries, std::size_t reference)
{
    std::string rtf = R"RTF({\rtf1\ansi\deff0{\fonttbl)RTF";
    for (std::size_t i = 0; i < entries; ++i) {
        rtf += "{\\f" + std::to_string(i) + "\\fnil F" + std::to_string(i) + ";}";
    }
    rtf += "}\\f" + std::to_string(reference) + R"RTF(\fs24 X\par})RTF";
    return rtf;
}

/** `entries` coloured entries after the auto entry, then `\cf<reference>` text. */
std::string color_cap_fixture(std::size_t entries, std::size_t reference)
{
    std::string rtf =
        R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Arial;}}{\colortbl;)RTF";
    for (std::size_t i = 1; i <= entries; ++i) {
        rtf += "\\red" + std::to_string(i % 256) + "\\green0\\blue0;";
    }
    rtf += "}\\f0\\fs24 \\cf" + std::to_string(reference) + R"RTF( X\par})RTF";
    return rtf;
}

/** Single font entry whose family name is exactly @a name bytes. */
std::string named_font_fixture(std::string const &name)
{
    return R"RTF({\rtf1\ansi\deff0{\fonttbl{\f0\fnil )RTF" + name +
           R"RTF(;}}\f0\fs24 X\par})RTF";
}

} // namespace

TEST(RtfDefaultLimits, InputByteCapAt262144And262145)
{
    // 262,144 representation bytes are accepted (the 261,000+ bytes inside the
    // {\*\generator ...} destination are skipped, so the decoded text stays one
    // byte); 262,145 bytes are over_limit with no usable fragment.
    std::size_t const overhead = input_cap_fixture(0).size();
    ASSERT_LT(overhead, 262144u);

    std::string const at_cap = input_cap_fixture(262144u - overhead);
    ASSERT_EQ(at_cap.size(), 262144u);
    Result const accepted = decode(at_cap);
    ASSERT_EQ(accepted.status, Status::ok) << accepted.error;
    EXPECT_EQ(accepted.fragment.plain, "X");
    ASSERT_EQ(accepted.fragment.paragraphs.size(), 1u);
    expect_paragraph(accepted, 0, "", { { "X", ARIAL16 } });
    EXPECT_EQ(accepted.diagnostics.skipped_destinations, 1u);
    expect_invariants(accepted);

    std::string const over = input_cap_fixture(262144u - overhead + 1u);
    ASSERT_EQ(over.size(), 262145u);
    Result const rejected = decode(over);
    EXPECT_EQ(rejected.status, Status::over_limit);
    EXPECT_EQ(rejected.error, "input exceeds the byte cap");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
    EXPECT_TRUE(rejected.fragment.plain.empty());
    EXPECT_TRUE(rejected.fragment.empty());
}

TEST(RtfDefaultLimits, ParagraphCapAt1024And1025)
{
    // 1,024 \par-separated paragraphs are accepted (their runs stay inside the
    // run and text budgets); the 1,025th \par is over_limit and the fragment is
    // discarded as a whole, never truncated to a prefix.
    Result const accepted = decode(paragraph_cap_fixture(1024));
    ASSERT_EQ(accepted.status, Status::ok) << accepted.error;
    ASSERT_EQ(accepted.fragment.paragraphs.size(), 1024u);
    EXPECT_EQ(accepted.fragment.plain.size(), 2047u); // 1024 x 'x' + 1023 '\n'
    expect_paragraph(accepted, 0, "", { { "x", ARIAL16 } });
    expect_paragraph(accepted, 1023, "", { { "x", ARIAL16 } });
    expect_invariants(accepted);

    Result const rejected = decode(paragraph_cap_fixture(1025));
    EXPECT_EQ(rejected.status, Status::over_limit);
    EXPECT_EQ(rejected.error, "paragraphs exceed the cap");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
    EXPECT_TRUE(rejected.fragment.plain.empty());
}

TEST(RtfDefaultLimits, RunCapAt4096And4097)
{
    // The fixture changes the canonical run style once per unit, so it really
    // produces one committed run per unit (no run-splitting involved: every run
    // is one byte). 4,096 runs are accepted; the 4,097th is over_limit.
    Result const accepted = decode(run_cap_fixture(4096));
    ASSERT_EQ(accepted.status, Status::ok) << accepted.error;
    ASSERT_EQ(accepted.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(accepted.fragment.paragraphs[0].runs.size(), 4096u);
    EXPECT_EQ(accepted.fragment.plain.size(), 4096u);
    EXPECT_EQ(accepted.fragment.paragraphs[0].runs[0].style, "font-size:16px;font-weight:bold;");
    EXPECT_EQ(accepted.fragment.paragraphs[0].runs[1].style, "font-size:16px;");
    EXPECT_EQ(accepted.fragment.paragraphs[0].runs[4095].style, "font-size:16px;");
    for (auto const &run : accepted.fragment.paragraphs[0].runs) {
        EXPECT_EQ(run.text, "a");
    }
    EXPECT_EQ(accepted.diagnostics.split_runs, 0u);
    expect_invariants(accepted);

    Result const rejected = decode(run_cap_fixture(4097));
    EXPECT_EQ(rejected.status, Status::over_limit);
    EXPECT_EQ(rejected.error, "runs exceed the cap");
    EXPECT_TRUE(rejected.fragment.paragraphs.empty());
    EXPECT_TRUE(rejected.fragment.plain.empty());
}

TEST(RtfDefaultLimits, ColourTableCapAt256EntriesAndOneOver)
{
    // The colour table is positional from the auto entry at index 0, so 255
    // coloured entries fill the 256-entry budget exactly; entry index 255 is
    // \red255 and decodes to #ff0000. One more entry is DROPPED (the document
    // stays ok, only the diagnostic counter moves) because commit_color_entry()
    // documents a loss, not a rejection.
    Result const at_cap = decode(color_cap_fixture(255, 255));
    ASSERT_EQ(at_cap.status, Status::ok) << at_cap.error;
    EXPECT_EQ(at_cap.fragment.plain, "X");
    expect_paragraph(at_cap, 0, "", { { "X", ARIAL16 + "color:#ff0000;" } });
    EXPECT_EQ(at_cap.diagnostics.dropped_style_declarations, 0u);
    expect_invariants(at_cap);

    Result const one_over = decode(color_cap_fixture(256, 255));
    ASSERT_EQ(one_over.status, Status::ok) << one_over.error;
    expect_paragraph(one_over, 0, "", { { "X", ARIAL16 + "color:#ff0000;" } });
    EXPECT_EQ(one_over.diagnostics.dropped_style_declarations, 1u)
        << "exactly the 257th colour entry is dropped";

    // The dropped entry is not addressable any more, while entry 255 still is.
    Result const dropped_reference = decode(color_cap_fixture(256, 256));
    ASSERT_EQ(dropped_reference.status, Status::ok) << dropped_reference.error;
    expect_paragraph(dropped_reference, 0, "", { { "X", ARIAL16 } });
    EXPECT_EQ(dropped_reference.diagnostics.dropped_style_declarations, 2u)
        << "the dropped entry plus the out-of-range \\cf256 reference";
}

TEST(RtfDefaultLimits, FontTableCapAt256EntriesAndOneOver)
{
    // 256 font entries are kept and \f255 resolves to family F255; the 257th
    // entry is DROPPED as documented (status stays ok, the counter moves).
    Result const at_cap = decode(font_cap_fixture(256, 255));
    ASSERT_EQ(at_cap.status, Status::ok) << at_cap.error;
    EXPECT_EQ(at_cap.fragment.plain, "X");
    expect_paragraph(at_cap, 0, "", { { "X", "font-family:F255;font-size:16px;" } });
    EXPECT_EQ(at_cap.diagnostics.dropped_style_declarations, 0u);
    expect_invariants(at_cap);

    Result const one_over = decode(font_cap_fixture(257, 255));
    ASSERT_EQ(one_over.status, Status::ok) << one_over.error;
    expect_paragraph(one_over, 0, "", { { "X", "font-family:F255;font-size:16px;" } });
    EXPECT_EQ(one_over.diagnostics.dropped_style_declarations, 1u)
        << "exactly the 257th font entry is dropped";

    Result const dropped_reference = decode(font_cap_fixture(257, 256));
    ASSERT_EQ(dropped_reference.status, Status::ok) << dropped_reference.error;
    expect_paragraph(dropped_reference, 0, "", { { "X", "font-size:16px;" } });
    EXPECT_EQ(dropped_reference.diagnostics.dropped_style_declarations, 2u)
        << "the dropped entry plus the missing-font declaration";
}

TEST(RtfDefaultLimits, FontName255AndStyleValue256ByteBoundaries)
{
    // MAX_FONT_NAME_BYTES is inclusive (255 bytes kept, 256 cleared and counted
    // in truncated_font_names); MAX_STYLE_VALUE_LENGTH is inclusive too, and a
    // quoted family name reaches it at 254 name bytes. The 255-byte name whose
    // quoted value is 257 bytes is dropped as a declaration while the name
    // itself passes the font-name cap.
    std::string const name255(255, 'a');
    Result const kept = decode(named_font_fixture(name255));
    ASSERT_EQ(kept.status, Status::ok) << kept.error;
    EXPECT_EQ(kept.diagnostics.truncated_font_names, 0u);
    EXPECT_EQ(kept.diagnostics.dropped_style_declarations, 0u);
    expect_paragraph(kept, 0, "", { { "X", "font-family:" + name255 + ";font-size:16px;" } });
    expect_invariants(kept);

    std::string const name256(256, 'a');
    Result const cleared = decode(named_font_fixture(name256));
    ASSERT_EQ(cleared.status, Status::ok) << cleared.error;
    EXPECT_EQ(cleared.diagnostics.truncated_font_names, 1u);
    EXPECT_EQ(cleared.diagnostics.dropped_style_declarations, 1u);
    expect_paragraph(cleared, 0, "", { { "X", "font-size:16px;" } });
    expect_invariants(cleared);

    std::string const name254_quoted = std::string(252, 'a') + " b"; // 254 bytes
    Result const value_at_cap = decode(named_font_fixture(name254_quoted));
    ASSERT_EQ(value_at_cap.status, Status::ok) << value_at_cap.error;
    std::string const value256 = "'" + name254_quoted + "'";
    EXPECT_EQ(value256.size(), 256u);
    EXPECT_EQ(value_at_cap.diagnostics.dropped_style_declarations, 0u);
    expect_paragraph(value_at_cap, 0, "",
                     { { "X", "font-family:" + value256 + ";font-size:16px;" } });
    expect_invariants(value_at_cap);

    std::string const name255_quoted = std::string(253, 'a') + " b"; // 255 bytes
    Result const value_over = decode(named_font_fixture(name255_quoted));
    ASSERT_EQ(value_over.status, Status::ok) << value_over.error;
    EXPECT_EQ(("'" + name255_quoted + "'").size(), 257u);
    EXPECT_EQ(value_over.diagnostics.truncated_font_names, 0u);
    EXPECT_EQ(value_over.diagnostics.dropped_style_declarations, 1u);
    expect_paragraph(value_over, 0, "", { { "X", "font-size:16px;" } });
    expect_invariants(value_over);
}
