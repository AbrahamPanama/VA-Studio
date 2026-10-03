// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Paste-scoped missing-font diagnosis: pure policy tests with a fake oracle.
 *
 * These tests need no display and no font configuration: every oracle answer is
 * supplied by FakeOracle, so classification, list/CSS policy, dedupe, the
 * distinct-family cap, run scoping and the localized summary are asserted
 * exactly. The `TextPasteFontRealOracle` tests at the end exercise the
 * FontFactory-backed oracle; they are clearly separated and the case-sensitive
 * family one needs the isolated font fixture (INKSCAPE_FONTCONFIG) and skips
 * without it. No test asserts a concrete substitute family name.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>
#include <glibmm/init.h>

#include "libnrtype/font-factory.h"
#include "ui/text-paste-font.h"
#include "ui/text-paste.h"

namespace Inkscape::UI::TextPaste::FontCheck {

std::ostream &operator<<(std::ostream &out, IssueKind kind)
{
    switch (kind) {
        case IssueKind::MissingFamily:
            return out << "MissingFamily";
        case IssueKind::MissingFace:
            return out << "MissingFace";
        case IssueKind::UnavailableVariation:
            return out << "UnavailableVariation";
        case IssueKind::MissingGlyphs:
            return out << "MissingGlyphs";
    }
    return out << "IssueKind(" << static_cast<int>(kind) << ")";
}

} // namespace Inkscape::UI::TextPaste::FontCheck

namespace {

using namespace Inkscape::UI::TextPaste;
using namespace Inkscape::UI::TextPaste::FontCheck;

/** testing::Test declares a private member function Run(), so tests use this alias. */
using PasteRun = Inkscape::UI::TextPaste::Run;

/** ASCII case folding for the fake oracle's own bookkeeping. */
std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return out;
}

/**
 * Scriptable Availability. Defaults: no family installed, every face available,
 * no substitute known, every glyph mapped. The recorded call tables let a test
 * assert that work is bounded by distinct requests, not by runs.
 */
class FakeOracle final : public Availability
{
public:
    std::function<bool(std::string const &)> installed = [](std::string const &) { return false; };
    std::function<bool(std::string const &, std::string const &)> face =
        [](std::string const &, std::string const &) { return true; };
    std::function<std::string(std::string const &)> substitute = [](std::string const &) { return std::string{}; };
    std::function<bool(std::string const &, std::string const &, std::string_view)> glyphs =
        [](std::string const &, std::string const &, std::string_view) { return true; };

    mutable std::map<std::string, int> family_lookups;    ///< casefolded family -> calls
    mutable std::map<std::string, int> substitute_lookups; ///< casefolded family -> calls
    mutable std::vector<std::pair<std::string, std::string>> face_lookups;
    mutable std::vector<std::string> glyph_texts;

    bool family_installed(std::string const &family) const override
    {
        ++family_lookups[lower(family)];
        return installed(family);
    }

    bool face_available(std::string const &family, std::string const &run_style) const override
    {
        face_lookups.emplace_back(family, run_style);
        return face(family, run_style);
    }

    std::string substitute_for(std::string const &family) const override
    {
        ++substitute_lookups[lower(family)];
        return substitute(family);
    }

    bool glyphs_available(std::string const &family, std::string const &run_style,
                          std::string_view text) const override
    {
        glyph_texts.emplace_back(text);
        return glyphs(family, run_style, text);
    }
};

Run make_run(std::string style, std::string text)
{
    Run run;
    run.style = std::move(style);
    run.text = std::move(text);
    return run;
}

Fragment one_paragraph(std::vector<Run> runs)
{
    Fragment fragment;
    Paragraph paragraph;
    paragraph.runs = std::move(runs);
    fragment.paragraphs.push_back(std::move(paragraph));
    return fragment;
}

/** A family that is installed, resolves to itself and has an OK face. */
void install(FakeOracle &oracle, std::string const &family)
{
    std::string const key = lower(family);
    oracle.installed = [key](std::string const &candidate) { return lower(candidate) == key; };
    oracle.substitute = [key, family](std::string const &candidate) {
        return lower(candidate) == key ? family : std::string{};
    };
}

std::size_t count_of(std::string const &haystack, std::string const &needle)
{
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

/**
 * Value of the first canonical "name:value;" declaration of a fragment style
 * list. first_concrete_family() takes a font-family VALUE, not a whole
 * declaration list, so the test extracts the value before calling it.
 */
std::string style_value(std::string const &style, std::string const &name)
{
    std::size_t pos = 0;
    while (pos < style.size()) {
        auto const semi = style.find(';', pos);
        auto const declaration = style.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = semi == std::string::npos ? style.size() : semi + 1;
        auto const colon = declaration.find(':');
        if (colon == std::string::npos || declaration.substr(0, colon) != name) {
            continue;
        }
        return declaration.substr(colon + 1);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Declaration policy
// ---------------------------------------------------------------------------

TEST(TextPasteFontPolicy, GenericFamiliesAndPangoAliasesAreExempt)
{
    EXPECT_TRUE(is_generic_family("serif"));
    EXPECT_TRUE(is_generic_family("sans-serif"));
    EXPECT_TRUE(is_generic_family("Sans"));
    EXPECT_TRUE(is_generic_family("SERIF"));
    EXPECT_TRUE(is_generic_family("Monospace"));
    EXPECT_TRUE(is_generic_family("cursive"));
    EXPECT_TRUE(is_generic_family("fantasy"));
    EXPECT_TRUE(is_generic_family("system-ui"));
    EXPECT_TRUE(is_generic_family("ui-serif"));
    EXPECT_TRUE(is_generic_family("ui-sans-serif"));
    EXPECT_TRUE(is_generic_family("ui-monospace"));
    EXPECT_TRUE(is_generic_family("ui-rounded"));
    EXPECT_TRUE(is_generic_family("math"));
    EXPECT_TRUE(is_generic_family("emoji"));
    EXPECT_TRUE(is_generic_family("fangsong"));
    EXPECT_TRUE(is_generic_family("  \"Fantasy\"  ")); // trimmed and unquoted before comparison

    EXPECT_FALSE(is_generic_family("Lavi"));
    EXPECT_FALSE(is_generic_family("Helvetica Neue"));
    // The Pango aliases are the single words Sans/Serif/Monospace; a two-word
    // family literally called "Sans Serif" is a concrete request.
    EXPECT_FALSE(is_generic_family("Sans Serif"));
    EXPECT_FALSE(is_generic_family(""));
}

TEST(TextPasteFontPolicy, FamilyListIsSplitTrimmedAndUnquoted)
{
    auto const tokens = family_tokens("  'Vac No Such Font' , \"Noto Sans\" ,serif, ");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "Vac No Such Font");
    EXPECT_EQ(tokens[1], "Noto Sans");
    EXPECT_EQ(tokens[2], "serif");

    EXPECT_TRUE(family_tokens("").empty());
    EXPECT_TRUE(family_tokens(" , , ").empty());

    EXPECT_EQ(first_concrete_family("serif, monospace"), "");
    EXPECT_EQ(first_concrete_family("serif, VacNoSuchFont, Lavi"), "VacNoSuchFont");
    EXPECT_EQ(first_concrete_family("Inter, sans-serif"), "Inter");
}

TEST(TextPasteFontPolicy, GenericOnlyFamilyListProducesNoReport)
{
    FakeOracle oracle;
    Fragment const fragment =
        one_paragraph({make_run("font-family:serif, sans-serif;font-size:12px;", "abc")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    EXPECT_TRUE(report.empty());
    EXPECT_EQ(report.issues.size(), 0u);
    EXPECT_EQ(report.families_checked, 0u);
    EXPECT_EQ(report.distinct_families, 0u);
    EXPECT_FALSE(report.truncated);
    EXPECT_TRUE(oracle.family_lookups.empty());
    EXPECT_TRUE(oracle.substitute_lookups.empty());
    EXPECT_TRUE(oracle.face_lookups.empty());
    EXPECT_TRUE(oracle.glyph_texts.empty());
    EXPECT_EQ(summarize(report), std::string{});
}

TEST(TextPasteFontPolicy, WrongCaseFamilyIsNotMissing)
{
    {
        // A contract-respecting oracle compares family names case-insensitively.
        FakeOracle oracle;
        install(oracle, "Lavi");
        Fragment const fragment = one_paragraph({make_run("font-family:LAVI;font-size:12px;", "abc")});

        auto const report = inspect(fragment, oracle, MAX_FAMILIES);

        EXPECT_TRUE(report.empty());
        EXPECT_EQ(report.families_checked, 1u);
        EXPECT_EQ(report.distinct_families, 1u);
    }
    {
        // Even a case-sensitive oracle cannot cause a false report: the resolved
        // family equals the requested token (casefolded), so it is installed.
        FakeOracle oracle;
        oracle.installed = [](std::string const &family) { return family == "Lavi"; };
        oracle.substitute = [](std::string const &family) {
            return lower(family) == "lavi" ? std::string("Lavi") : std::string{};
        };
        Fragment const fragment = one_paragraph({make_run("font-family:LAVI;font-size:12px;", "abc")});

        auto const report = inspect(fragment, oracle, MAX_FAMILIES);

        EXPECT_TRUE(report.empty());
        ASSERT_EQ(oracle.family_lookups.size(), 1u);
        EXPECT_EQ(oracle.family_lookups.at("lavi"), 1);
    }
}

TEST(TextPasteFontPolicy, WebfontBeforeGenericIsReported)
{
    FakeOracle oracle;
    oracle.substitute = [](std::string const &family) {
        return lower(family) == "inter" ? std::string("FreeSans") : std::string{};
    };
    Fragment const fragment =
        one_paragraph({make_run("font-family:Inter, sans-serif;font-size:16px;", "hello")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    auto const &issue = report.issues[0];
    EXPECT_EQ(issue.kind, IssueKind::MissingFamily);
    EXPECT_EQ(issue.requested, "Inter");
    EXPECT_EQ(issue.substitute, "FreeSans");
    EXPECT_EQ(issue.paragraph_index, 0u);
    EXPECT_EQ(issue.run_index, 0u);
    EXPECT_EQ(issue.missing_code_points, 0u);
    EXPECT_EQ(report.families_checked, 1u);
    EXPECT_EQ(report.distinct_families, 1u);
    EXPECT_FALSE(report.truncated);
    EXPECT_TRUE(oracle.face_lookups.empty());
    EXPECT_TRUE(oracle.glyph_texts.empty());

    auto const summary = summarize(report);
    EXPECT_NE(summary.find("Inter"), std::string::npos);
    EXPECT_NE(summary.find("FreeSans"), std::string::npos); // requested -> substitute
    EXPECT_NE(summary.find("Text panel"), std::string::npos);
    EXPECT_EQ(summary.find('\n'), std::string::npos);
}

TEST(TextPasteFontPolicy, MissingFamilyBeforeGenericSerifIsReported)
{
    FakeOracle oracle;
    Fragment const fragment = one_paragraph({make_run("font-family:VacNoSuchFont, serif;", "abc")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].kind, IssueKind::MissingFamily);
    EXPECT_EQ(report.issues[0].requested, "VacNoSuchFont");
    EXPECT_EQ(report.issues[0].substitute, "");
    EXPECT_EQ(report.issues[0].run_index, 0u);

    auto const summary = summarize(report);
    EXPECT_NE(summary.find("VacNoSuchFont"), std::string::npos);
    EXPECT_NE(summary.find("system fallback"), std::string::npos);
    EXPECT_NE(summary.find("Text panel"), std::string::npos);
}

TEST(TextPasteFontPolicy, ConcreteTokenAfterInstalledTokenIsInert)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    Fragment const fragment = one_paragraph({make_run("font-family:Lavi, VacNoSuchFont;", "abc")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    EXPECT_TRUE(report.empty());
    ASSERT_EQ(oracle.family_lookups.size(), 1u);
    EXPECT_EQ(oracle.family_lookups.count("vacnosuchfont"), 0u);
    EXPECT_EQ(oracle.family_lookups.at("lavi"), 1);
}

TEST(TextPasteFontPolicy, QuotedMultiWordRequestKeepsItsIdentity)
{
    FakeOracle oracle;
    Fragment const fragment = one_paragraph({make_run("font-family:  'Vac No Such Font' , serif ;", "abc")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].kind, IssueKind::MissingFamily);
    EXPECT_EQ(report.issues[0].requested, "Vac No Such Font");
    EXPECT_NE(summarize(report).find("Vac No Such Font"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Run scoping and dedupe
// ---------------------------------------------------------------------------

TEST(TextPasteFontPolicy, MixedFragmentReportsOnlyTheAffectedRun)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    Fragment const fragment = one_paragraph({
        make_run("font-family:Lavi;font-size:12px;", "a"),
        make_run("font-family:VacNoSuchFont;font-size:12px;", "b"),
        make_run("font-family:Lavi;font-size:24px;", "c"),
    });

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].kind, IssueKind::MissingFamily);
    EXPECT_EQ(report.issues[0].requested, "VacNoSuchFont");
    EXPECT_EQ(report.issues[0].paragraph_index, 0u);
    EXPECT_EQ(report.issues[0].run_index, 1u);
    EXPECT_EQ(report.families_checked, 2u);
    EXPECT_EQ(report.distinct_families, 2u);
}

TEST(TextPasteFontPolicy, SameMissingFamilyInSeveralRunsIsOneIssue)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    Fragment const fragment = one_paragraph({
        make_run("font-family:Lavi;font-size:12px;", "a"),
        make_run("font-family:VacNoSuchFont;font-size:12px;", "b"),
        make_run("font-family:VacNoSuchFont;font-size:24px;", "c"),
        make_run("font-family:VACNOSUCHFONT;font-size:48px;", "d"), // same family, other case
    });

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].requested, "VacNoSuchFont"); // first spelling wins
    EXPECT_EQ(report.issues[0].run_index, 1u);
    EXPECT_EQ(report.distinct_families, 2u); // Lavi and VacNoSuchFont, not 4 runs
    ASSERT_EQ(oracle.family_lookups.size(), 2u);
    EXPECT_EQ(oracle.family_lookups.at("vacnosuchfont"), 1);
}

TEST(TextPasteFontPolicy, TwoHundredRunsShareOneLookupPerDistinctFamily)
{
    FakeOracle oracle;
    std::vector<PasteRun> runs;
    for (int i = 0; i < 200; ++i) {
        runs.push_back(make_run("font-family:VacNoSuchFont;font-size:12px;", "x"));
    }
    Fragment const fragment = one_paragraph(std::move(runs));

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.families_checked, 1u);
    EXPECT_EQ(report.distinct_families, 1u);
    ASSERT_EQ(oracle.family_lookups.size(), 1u);
    EXPECT_EQ(oracle.family_lookups.at("vacnosuchfont"), 1);
    ASSERT_EQ(oracle.substitute_lookups.size(), 1u);
    EXPECT_EQ(oracle.substitute_lookups.at("vacnosuchfont"), 1);
    EXPECT_TRUE(oracle.face_lookups.empty()); // a missing family is never face-probed
}

// ---------------------------------------------------------------------------
// Face and variation classification
// ---------------------------------------------------------------------------

TEST(TextPasteFontPolicy, MissingFaceIsDistinguishedFromMissingFamily)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    oracle.face = [](std::string const &, std::string const &style) {
        return style.find("font-weight:bold") == std::string::npos;
    };
    Fragment const fragment =
        one_paragraph({make_run("font-family:Lavi;font-weight:bold;font-size:12px;", "x")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    auto const &issue = report.issues[0];
    EXPECT_EQ(issue.kind, IssueKind::MissingFace);
    EXPECT_NE(issue.kind, IssueKind::MissingFamily);
    EXPECT_EQ(issue.requested, "Lavi bold"); // family + the requested face keyword
    EXPECT_EQ(issue.substitute, "Lavi");
    EXPECT_EQ(issue.paragraph_index, 0u);
    EXPECT_EQ(issue.run_index, 0u);
    ASSERT_EQ(oracle.face_lookups.size(), 1u);
    EXPECT_EQ(oracle.face_lookups[0].first, "Lavi");
    EXPECT_TRUE(oracle.glyph_texts.empty()); // no glyph claim without an available face
}

TEST(TextPasteFontPolicy, MissingFamilySuppressesFaceAndGlyphChecks)
{
    FakeOracle oracle;
    oracle.face = [](std::string const &, std::string const &) { return false; };
    oracle.glyphs = [](std::string const &, std::string const &, std::string_view) { return false; };
    Fragment const fragment =
        one_paragraph({make_run("font-family:GhostFont;font-weight:bold;", "x")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].kind, IssueKind::MissingFamily);
    EXPECT_EQ(report.issues[0].requested, "GhostFont");
    EXPECT_TRUE(oracle.face_lookups.empty());
    EXPECT_TRUE(oracle.glyph_texts.empty());
}

TEST(TextPasteFontPolicy, IdenticalFaceRequestIsLookedUpOnce)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    oracle.face = [](std::string const &, std::string const &style) {
        return style.find("font-weight:bold") == std::string::npos;
    };
    Fragment const fragment = one_paragraph({
        make_run("font-family:Lavi;font-weight:bold;font-size:12px;", "x"),
        make_run("font-family:Lavi;font-weight:bold;font-size:24px;", "y"),
    });

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].kind, IssueKind::MissingFace);
    EXPECT_EQ(report.issues[0].run_index, 0u);
    EXPECT_EQ(report.issues[0].requested, "Lavi bold"); // font size is not part of the request
    EXPECT_EQ(oracle.face_lookups.size(), 1u);
}

TEST(TextPasteFontPolicy, UnavailableVariationIsReported)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    oracle.face = [](std::string const &, std::string const &style) {
        return style.find("font-variation-settings") == std::string::npos;
    };
    Fragment const fragment = one_paragraph(
        {make_run("font-family:Lavi;font-variation-settings:'wght' 700;font-size:12px;", "x")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    auto const &issue = report.issues[0];
    EXPECT_EQ(issue.kind, IssueKind::UnavailableVariation);
    EXPECT_NE(issue.kind, IssueKind::MissingFace);
    EXPECT_EQ(issue.requested, "Lavi 'wght' 700");
    EXPECT_EQ(issue.substitute, "Lavi");
    EXPECT_EQ(issue.run_index, 0u);
    // The base face is resolved first, then the variation request: one call each.
    ASSERT_EQ(oracle.face_lookups.size(), 2u);
    EXPECT_EQ(oracle.face_lookups[0].second.find("font-variation-settings"), std::string::npos);
    EXPECT_NE(oracle.face_lookups[1].second.find("font-variation-settings"), std::string::npos);
    EXPECT_TRUE(oracle.glyph_texts.empty());
}

TEST(TextPasteFontPolicy, UnavailableBaseFaceWinsOverVariation)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    oracle.face = [](std::string const &, std::string const &) { return false; };
    Fragment const fragment = one_paragraph(
        {make_run("font-family:Lavi;font-variation-settings:'wght' 700;font-size:12px;", "x")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].kind, IssueKind::MissingFace);
    EXPECT_EQ(report.issues[0].requested, "Lavi"); // the base face request has no variation
    EXPECT_EQ(oracle.face_lookups.size(), 1u);
}

TEST(TextPasteFontPolicy, ReflectedVariationProducesNoIssue)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    Fragment const fragment = one_paragraph(
        {make_run("font-family:Lavi;font-variation-settings:'wght' 700;font-size:12px;", "x")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    EXPECT_TRUE(report.empty());
    EXPECT_EQ(oracle.face_lookups.size(), 2u);
    ASSERT_EQ(oracle.glyph_texts.size(), 1u);
    EXPECT_EQ(oracle.glyph_texts[0], "x");
}

// ---------------------------------------------------------------------------
// Glyph coverage
// ---------------------------------------------------------------------------

TEST(TextPasteFontPolicy, MissingGlyphsAreReportedOnceForTheRun)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    oracle.glyphs = [](std::string const &, std::string const &, std::string_view) { return false; };
    Fragment const fragment =
        one_paragraph({make_run("font-family:Lavi;font-size:12px;", "abc \u05e9\u05dc\u05d5\u05dd")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    auto const &issue = report.issues[0];
    EXPECT_EQ(issue.kind, IssueKind::MissingGlyphs);
    EXPECT_EQ(issue.requested, "Lavi");
    EXPECT_EQ(issue.substitute, "Lavi");
    EXPECT_EQ(issue.paragraph_index, 0u);
    EXPECT_EQ(issue.run_index, 0u);
    EXPECT_EQ(issue.missing_code_points, 1u); // proven lower bound of the boolean oracle
    ASSERT_EQ(oracle.glyph_texts.size(), 1u);
    EXPECT_EQ(oracle.glyph_texts[0], std::string("abc \u05e9\u05dc\u05d5\u05dd"));
}

TEST(TextPasteFontPolicy, IgnorableCodePointsAloneProduceNoGlyphIssue)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    oracle.glyphs = [](std::string const &, std::string const &, std::string_view) { return false; };
    // ZWSP, ZWNJ, ZWJ, word joiner, BOM, soft hyphen and variation selectors.
    Fragment const fragment =
        one_paragraph({make_run("font-family:Lavi;", "\u200B\u200C\u200D\u2060\uFEFF\u00AD\uFE00\uFE0F")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    EXPECT_TRUE(report.empty());
    EXPECT_TRUE(oracle.glyph_texts.empty()); // nothing non-ignorable was probed
}

TEST(TextPasteFontPolicy, IgnorableCodePointsAreStrippedBeforeTheProbe)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    oracle.glyphs = [](std::string const &, std::string const &, std::string_view) { return false; };
    Fragment const fragment =
        one_paragraph({make_run("font-family:Lavi;", "a\u200B\uFEFFb\u00AD")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].kind, IssueKind::MissingGlyphs);
    ASSERT_EQ(oracle.glyph_texts.size(), 1u);
    EXPECT_EQ(oracle.glyph_texts[0], "ab");
}

TEST(TextPasteFontPolicy, GlyphFindingsArePerAffectedRun)
{
    FakeOracle oracle;
    install(oracle, "Lavi");
    oracle.glyphs = [](std::string const &, std::string const &, std::string_view) { return false; };
    Fragment const fragment = one_paragraph({
        make_run("font-family:Lavi;", "abc"),
        make_run("font-family:Lavi;", "def"),
    });

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 2u);
    EXPECT_EQ(report.issues[0].kind, IssueKind::MissingGlyphs);
    EXPECT_EQ(report.issues[0].run_index, 0u);
    EXPECT_EQ(report.issues[1].kind, IssueKind::MissingGlyphs);
    EXPECT_EQ(report.issues[1].run_index, 1u);
}

// ---------------------------------------------------------------------------
// Cap, empty input and summary
// ---------------------------------------------------------------------------

TEST(TextPasteFontPolicy, FamilyCapSetsTruncated)
{
    FakeOracle oracle;
    std::vector<PasteRun> runs;
    for (int i = 0; i < 70; ++i) {
        runs.push_back(make_run("font-family:CapFont" + std::to_string(i) + ";", "x"));
    }
    Fragment const fragment = one_paragraph(std::move(runs));

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    EXPECT_TRUE(report.truncated);
    EXPECT_EQ(report.families_checked, MAX_FAMILIES);
    EXPECT_EQ(report.distinct_families, 70u);
    EXPECT_EQ(report.issues.size(), MAX_FAMILIES);
    for (auto const &issue : report.issues) {
        EXPECT_EQ(issue.kind, IssueKind::MissingFamily);
    }
    EXPECT_EQ(oracle.family_lookups.size(), MAX_FAMILIES); // capped lookups, not one per run
}

TEST(TextPasteFontPolicy, ExactlyAtTheCapIsNotTruncated)
{
    FakeOracle oracle;
    std::vector<PasteRun> runs;
    for (std::size_t i = 0; i < MAX_FAMILIES; ++i) {
        runs.push_back(make_run("font-family:CapFont" + std::to_string(i) + ";", "x"));
    }
    Fragment const fragment = one_paragraph(std::move(runs));

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);

    EXPECT_FALSE(report.truncated);
    EXPECT_EQ(report.families_checked, MAX_FAMILIES);
    EXPECT_EQ(report.distinct_families, MAX_FAMILIES);
    EXPECT_EQ(report.issues.size(), MAX_FAMILIES);
}

TEST(TextPasteFontPolicy, ZeroCapMakesNoClaims)
{
    FakeOracle oracle;
    Fragment const fragment = one_paragraph({make_run("font-family:VacNoSuchFont;", "x")});

    auto const report = inspect(fragment, oracle, 0);

    EXPECT_TRUE(report.truncated);
    EXPECT_EQ(report.families_checked, 0u);
    EXPECT_EQ(report.distinct_families, 1u);
    EXPECT_TRUE(report.empty());
    EXPECT_TRUE(oracle.family_lookups.empty());
}

TEST(TextPasteFontPolicy, ParagraphStyleOnlyAndEmptyFragmentsHaveNoIssues)
{
    FakeOracle oracle;

    Fragment paragraph_style_only;
    Paragraph paragraph;
    paragraph.style = "text-align:center;line-height:1.2;";
    paragraph.runs.push_back(make_run("", "text"));
    paragraph_style_only.paragraphs.push_back(std::move(paragraph));

    EXPECT_TRUE(inspect(paragraph_style_only, oracle, MAX_FAMILIES).empty());

    Fragment empty_fragment;
    auto const empty_report = inspect(empty_fragment, oracle, MAX_FAMILIES);
    EXPECT_TRUE(empty_report.empty());
    EXPECT_EQ(empty_report.families_checked, 0u);
    EXPECT_EQ(empty_report.distinct_families, 0u);
    EXPECT_FALSE(empty_report.truncated);
    EXPECT_EQ(summarize(empty_report), std::string{});

    Fragment empty_run = one_paragraph({make_run("font-family:VacNoSuchFont;", "")});
    EXPECT_TRUE(inspect(empty_run, oracle, MAX_FAMILIES).empty());
    EXPECT_TRUE(oracle.family_lookups.empty());
}

TEST(TextPasteFontPolicy, PasteStylePipelineKeepsRequestedFamilyIdentity)
{
    // F02 identity guarantee, observable at the runnable seam the paste path
    // actually uses. inspect() takes the fragment by const reference, so
    // asserting the fragment after the call could never fail; these are the
    // transformations that can really drop or rewrite a family on the way to the
    // document: sanitize_style() then prepare_for_receiving_api(). Whatever
    // substitute the font check reports, the style handed to the receiving API
    // must still name the REQUESTED family.
    FakeOracle oracle;
    oracle.substitute = [](std::string const &family) {
        return lower(family) == "inter" ? std::string("FreeSans") : std::string{};
    };
    Fragment const fragment = one_paragraph({make_run("font-family:Inter, sans-serif;font-size:12px;", "hi")});

    auto const report = inspect(fragment, oracle, MAX_FAMILIES);
    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].requested, "Inter");
    EXPECT_EQ(report.issues[0].substitute, "FreeSans"); // renders with the substitute...

    auto const sanitized = sanitize_style(fragment.paragraphs[0].runs[0].style, false);
    auto const prepared = prepare_for_receiving_api(sanitized, 1.0, 1.0);
    ASSERT_TRUE(prepared);
    auto const applied_family = style_value(*prepared, "font-family");
    EXPECT_EQ(applied_family, "Inter, sans-serif"); // ...but the requested identity is what applies
    EXPECT_EQ(applied_family.find("FreeSans"), std::string::npos);
    EXPECT_EQ(first_concrete_family(applied_family), "Inter");
}

TEST(TextPasteFontPolicy, SummarizeIsEmptyForAnEmptyReport)
{
    EXPECT_EQ(summarize(Report{}), std::string{});
}

TEST(TextPasteFontPolicy, SummarizeAggregatesRequestedToSubstitute)
{
    Report report;
    report.issues.push_back(Issue{IssueKind::MissingFamily, "Inter", "FreeSans", 0, 0, 0});
    report.issues.push_back(Issue{IssueKind::MissingFamily, "Inter", "FreeSans", 0, 1, 0}); // duplicate
    report.issues.push_back(Issue{IssueKind::MissingFamily, "VacNoSuchFont", "", 0, 2, 0});
    report.issues.push_back(Issue{IssueKind::MissingGlyphs, "Lavi", "Lavi", 0, 3, 1});

    auto const summary = summarize(report);

    EXPECT_FALSE(summary.empty());
    EXPECT_EQ(summary.find('\n'), std::string::npos); // one aggregated line
    EXPECT_EQ(count_of(summary, "Inter"), 1u);        // duplicate collapsed
    EXPECT_NE(summary.find("FreeSans"), std::string::npos);
    EXPECT_NE(summary.find("VacNoSuchFont"), std::string::npos);
    EXPECT_NE(summary.find("system fallback"), std::string::npos);
    EXPECT_NE(summary.find("Text panel"), std::string::npos);
}

TEST(TextPasteFontPolicy, SummarizeCoversNonFamilyIssues)
{
    Report report;
    report.issues.push_back(Issue{IssueKind::MissingFace, "Lavi bold", "Lavi", 0, 0, 0});
    report.issues.push_back(Issue{IssueKind::UnavailableVariation, "Lavi 'wght' 700", "Lavi", 0, 1, 0});
    report.issues.push_back(Issue{IssueKind::MissingGlyphs, "Lavi", "Lavi", 0, 2, 1});

    auto const summary = summarize(report);

    EXPECT_FALSE(summary.empty());
    EXPECT_EQ(summary.find('\n'), std::string::npos);
    EXPECT_NE(summary.find("Text panel"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Real oracle (FontFactory). No substitute family name is asserted anywhere.
// ---------------------------------------------------------------------------

TEST(TextPasteFontRealOracle, GenericOnlyListIsExempt)
{
    Glib::init();
    Fragment const fragment = one_paragraph({make_run("font-family:serif, sans-serif;font-size:12px;", "abc")});

    auto const report = inspect(fragment, MAX_FAMILIES);

    EXPECT_TRUE(report.empty());
    EXPECT_EQ(report.families_checked, 0u);
}

TEST(TextPasteFontRealOracle, EmptyFragmentResolvesNoFont)
{
    Glib::init();
    Fragment const fragment;

    auto const report = inspect(fragment, MAX_FAMILIES);

    EXPECT_TRUE(report.empty());
    EXPECT_EQ(report.families_checked, 0u);
    EXPECT_EQ(report.distinct_families, 0u);
}

TEST(TextPasteFontRealOracle, UnknownFamilyIsReportedAsMissingFamily)
{
    Glib::init();
    Fragment const fragment = one_paragraph({make_run("font-family:VacNoSuchFont;font-size:12px;", "abc")});

    auto const report = inspect(fragment, MAX_FAMILIES);

    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].kind, IssueKind::MissingFamily);
    EXPECT_EQ(report.issues[0].requested, "VacNoSuchFont");
    // The resolved fallback is reported but its name is environment-dependent and
    // is deliberately not asserted.
    EXPECT_FALSE(report.issues[0].substitute.empty());
    EXPECT_EQ(report.families_checked, 1u);
    EXPECT_EQ(report.distinct_families, 1u);
    EXPECT_FALSE(report.truncated);
}

TEST(TextPasteFontRealOracle, IsolatedFixtureExemptsCaseMismatchedInstalledFamily)
{
    if (!g_getenv("INKSCAPE_FONTCONFIG")) {
        GTEST_SKIP() << "needs the isolated font fixture: set INKSCAPE_FONTCONFIG to "
                        "testfiles/rendering_tests/fonts/isolated.conf";
    }
    Glib::init();
    Fragment const fragment = one_paragraph({make_run("font-family:LAVI;font-size:12px;", "abc")});

    auto const report = inspect(fragment, MAX_FAMILIES);

    EXPECT_TRUE(report.empty()); // Lavi is installed; the case difference is not a missing family
    EXPECT_EQ(report.families_checked, 1u);
}

} // namespace
