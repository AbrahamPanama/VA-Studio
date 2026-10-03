// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Outcome tests for the bounded W5 CSS admission primitive.
 *
 * Every case calls the production helper in src/io/preview-css-admission.cpp;
 * no parser policy is re-implemented here. The helper performs no network or
 * filesystem access, so these tests exercise only native in-memory parsing.
 */

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "io/preview-css-admission.h"

using Inkscape::IO::admit_inline_declarations;
using Inkscape::IO::admit_stylesheet;
using Inkscape::IO::CssAdmissionReason;
using Inkscape::IO::CssAdmissionResult;
using Inkscape::IO::CssAdmissionStatus;
using Inkscape::IO::CssLimits;

namespace {

bool has_fragment(CssAdmissionResult const &r, std::string const &fragment) {
    for (auto const &f : r.local_fragment_ids) {
        if (f == fragment) {
            return true;
        }
    }
    return false;
}

} // namespace

// --- Accepted first subset -------------------------------------------------

TEST(PreviewCssAdmission, OrdinaryStylesheetSelectorsRgbAndLocalGradient) {
    std::string const css =
        ".a { fill: rgb(10, 20, 30); }\n"
        ".b { fill: url(#grad); }\n"
        ".c { stroke: #ff0000; }";
    char const *before = css.data();
    CssAdmissionResult const r = admit_stylesheet(css);
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
    EXPECT_EQ(r.reason, CssAdmissionReason::Ok);
    EXPECT_TRUE(has_fragment(r, "#grad"));
    EXPECT_GE(r.reference_count, 1u);
    EXPECT_GT(r.term_count, 0u);
    EXPECT_EQ(css.data(), before); // source bytes untouched
}

TEST(PreviewCssAdmission, InlineFontFamilyImportantAndLocalUrl) {
    std::string const css =
        "font-family: \"My Font\", sans-serif; fill: url(#g) !important;";
    std::string const copy = css;
    CssAdmissionResult const r = admit_inline_declarations(css);
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
    EXPECT_TRUE(has_fragment(r, "#g"));
    EXPECT_EQ(css, copy); // unchanged bytes
}

// --- External / escaped-function references are rejected --------------------

TEST(PreviewCssAdmission, PlainExternalUrlRejected) {
    CssAdmissionResult const r = admit_inline_declarations("fill:url(http://evil.example/x)");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
}

TEST(PreviewCssAdmission, QuotedEscapedFunctionUrlRejected) {
    CssAdmissionResult const r =
        admit_inline_declarations("fill:u\\72 l(\"http://evil.example/x\")");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
}

TEST(PreviewCssAdmission, MixedExternalPlusValidRejected) {
    CssAdmissionResult const r =
        admit_inline_declarations("fill:url(#ok); stroke:url(http://evil.example/x)");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
}

TEST(PreviewCssAdmission, EscapedFunctionInStylesheetRejected) {
    CssAdmissionResult const r =
        admit_stylesheet(".a { fill: u\\72 l(\"http://evil.example/x\") }");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
}

TEST(PreviewCssAdmission, ParsableUnknownFunctionUnsupported) {
    // T1: a parsable unknown function exercises the real UnknownFunction path.
    CssAdmissionResult const r = admit_inline_declarations("fill: rgba(1,2,3,0.5)");
    EXPECT_EQ(r.status, CssAdmissionStatus::Unsupported);
    EXPECT_EQ(r.reason, CssAdmissionReason::UnknownFunction);
}

TEST(PreviewCssAdmission, UnparsedFunctionInputNotParsed) {
    // T1: this input makes the native declaration parser return NULL, so it must
    // assert only NotParsed rather than being named as an unknown-function case.
    CssAdmissionResult const r = admit_inline_declarations("fill:myFn(http://evil.example/x)");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::NotParsed);
}

// --- At-rules, escaped or not, are Unsupported ------------------------------

TEST(PreviewCssAdmission, EscapedImportUnsupported) {
    CssAdmissionResult const r = admit_stylesheet("@\\69 mport \"http://evil.example/a.css\";");
    EXPECT_EQ(r.status, CssAdmissionStatus::Unsupported);
    EXPECT_EQ(r.reason, CssAdmissionReason::AtRule);
}

TEST(PreviewCssAdmission, EscapedFontFaceUnsupported) {
    CssAdmissionResult const r =
        admit_stylesheet("@font-\\66 ace { src: url(http://evil.example/f.woff); }");
    EXPECT_EQ(r.status, CssAdmissionStatus::Unsupported);
}

TEST(PreviewCssAdmission, UnknownAtRuleUnsupported) {
    CssAdmissionResult const r = admit_stylesheet("@bogus; .a { fill: red; }");
    EXPECT_EQ(r.status, CssAdmissionStatus::Unsupported);
    EXPECT_EQ(r.reason, CssAdmissionReason::AtRule);
}

TEST(PreviewCssAdmission, MixedValidRulesAndAtRuleUnsupported) {
    CssAdmissionResult const r =
        admit_stylesheet(".a { fill: red; } @bogus; .b { stroke: blue; }");
    EXPECT_EQ(r.status, CssAdmissionStatus::Unsupported);
}

// --- Malformed input is refused even when native status is CR_OK ------------

TEST(PreviewCssAdmission, UnterminatedStringRefused) {
    CssAdmissionResult const r = admit_stylesheet(".a { fill: \"unterminated; }");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
}

TEST(PreviewCssAdmission, UnterminatedCommentRefused) {
    CssAdmissionResult const r = admit_stylesheet(".a { fill: red; } /* oops");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
}

TEST(PreviewCssAdmission, UnterminatedBlockRefused) {
    CssAdmissionResult const r = admit_stylesheet(".a { fill: red;");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
}

// --- Pre-parse and budget limits -------------------------------------------

TEST(PreviewCssAdmission, EmbeddedNulRejected) {
    std::string const raw(".a{}\0.b{}", 8);
    CssAdmissionResult const r = admit_stylesheet(raw);
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::EmbeddedNul);
}

TEST(PreviewCssAdmission, InvalidUtf8Rejected) {
    std::string const raw(1, static_cast<char>(0xFF));
    CssAdmissionResult const r = admit_inline_declarations(raw);
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::InvalidUtf8);
}

TEST(PreviewCssAdmission, ByteLimitRejectedBeforeParse) {
    CssLimits limits;
    limits.max_bytes = 4;
    CssAdmissionResult const r = admit_stylesheet(".a { fill: red; }", limits);
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::TooManyBytes);
}

TEST(PreviewCssAdmission, TermLimitRejected) {
    CssLimits limits;
    limits.max_terms = 1;
    CssAdmissionResult const r = admit_inline_declarations("margin: 1px 2px", limits);
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::TooManyTerms);
}

TEST(PreviewCssAdmission, DepthLimitRejected) {
    CssLimits limits;
    limits.max_depth = 0;
    CssAdmissionResult const r = admit_inline_declarations("fill: red", limits);
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::TooDeep);
}

TEST(PreviewCssAdmission, ReferenceLimitRejected) {
    CssLimits limits;
    limits.max_references = 0;
    CssAdmissionResult const r = admit_inline_declarations("fill: url(#a)", limits);
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::TooManyReferences);
}

// --- F1: primary counters are charged once and exact budgets bind -----------

TEST(PreviewCssAdmission, InlineCountsChargedOnce) {
    CssAdmissionResult const r = admit_inline_declarations("fill:url(#a)");
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
    EXPECT_EQ(r.term_count, 1u);
    EXPECT_EQ(r.reference_count, 1u);
}

TEST(PreviewCssAdmission, InlineExactBudgetsAcceptSingleLocalReference) {
    CssLimits limits;
    limits.max_terms = 1;
    limits.max_references = 1;
    CssAdmissionResult const r = admit_inline_declarations("fill:url(#a)", limits);
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
    EXPECT_EQ(r.term_count, 1u);
    EXPECT_EQ(r.reference_count, 1u);
    EXPECT_TRUE(has_fragment(r, "#a"));
}

TEST(PreviewCssAdmission, InlineSecondReferenceRejectedAtBudgetOne) {
    CssLimits limits;
    limits.max_references = 1;
    CssAdmissionResult const r = admit_inline_declarations("fill:url(#a) url(#b)", limits);
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::TooManyReferences);
}

TEST(PreviewCssAdmission, PrimaryFragmentIdsOwnedUnique) {
    CssAdmissionResult const r = admit_inline_declarations("fill:url(#a) url(#a) url(#b)");
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
    EXPECT_EQ(r.reference_count, 3u);
    EXPECT_EQ(r.local_fragment_ids.size(), 2u); // unique ids only
    EXPECT_TRUE(has_fragment(r, "#a"));
    EXPECT_TRUE(has_fragment(r, "#b"));
}

// --- F2: malformed empty selector refused; comment/whitespace preserved -----

TEST(PreviewCssAdmission, EmptySelectorRejected) {
    CssAdmissionResult const r = admit_stylesheet("{}");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::NativeParseError);
}

TEST(PreviewCssAdmission, EmptySelectorBeforeValidRuleRejected) {
    CssAdmissionResult const r = admit_stylesheet("{} .a{fill:red}");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
}

TEST(PreviewCssAdmission, CommentOnlyStylesheetAccepted) {
    CssAdmissionResult const r = admit_stylesheet("/* hi */");
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
    EXPECT_EQ(r.reason, CssAdmissionReason::Ok);
}

TEST(PreviewCssAdmission, WhitespaceOnlyStylesheetAccepted) {
    CssAdmissionResult const r = admit_stylesheet("  \t\n ");
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
}

TEST(PreviewCssAdmission, OrdinarySelectorStillAccepted) {
    CssAdmissionResult const r = admit_stylesheet(".a{fill:red}");
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
}

// --- F3: nested url argument refused; escaped local function preserved ------

TEST(PreviewCssAdmission, NestedUrlArgumentRejected) {
    CssAdmissionResult const r = admit_inline_declarations("fill: url(url(#x))");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::NestedAmbiguousReference);
}

TEST(PreviewCssAdmission, NestedUrlArgumentInStylesheetRejected) {
    CssAdmissionResult const r = admit_stylesheet(".a{fill:url(url(#x))}");
    EXPECT_EQ(r.status, CssAdmissionStatus::Rejected);
    EXPECT_EQ(r.reason, CssAdmissionReason::NestedAmbiguousReference);
}

TEST(PreviewCssAdmission, EscapedLocalFunctionPreserved) {
    CssAdmissionResult const r = admit_inline_declarations("fill: u\\72 l(\"#g\")");
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
    EXPECT_TRUE(has_fragment(r, "#g"));
}

TEST(PreviewCssAdmission, OrdinaryTopLevelUriPreserved) {
    CssAdmissionResult const r = admit_inline_declarations("fill:url(#g)");
    EXPECT_EQ(r.status, CssAdmissionStatus::Accepted);
    EXPECT_TRUE(has_fragment(r, "#g"));
}

// --- Native probe corpus (16 literals) plus the missing stylesheet case -----

namespace {

enum class Entry { Inline, Stylesheet };

struct ProbeCase {
    char const *name;
    char const *css;
    Entry entry;
    CssAdmissionStatus expected;
};

} // namespace

TEST(PreviewCssAdmission, NativeProbeCorpusAndMissingStylesheetCase) {
    std::vector<ProbeCase> const cases = {
        {"01_ordinary_stylesheet", ".a { fill: red; }", Entry::Stylesheet,
         CssAdmissionStatus::Accepted},
        {"02_inline_local_url", "fill:url(#g)", Entry::Inline, CssAdmissionStatus::Accepted},
        {"03_inline_external_url_plain", "fill:url(http://evil.example/x)", Entry::Inline,
         CssAdmissionStatus::Rejected},
        {"04_inline_external_url_escaped", "fill:url(\\68 ttp://evil.example/x)", Entry::Inline,
         CssAdmissionStatus::Rejected},
        {"05_inline_escaped_function", "fill:u\\72 l(\"http://evil.example/x\")", Entry::Inline,
         CssAdmissionStatus::Rejected},
        {"06_inline_escaped_func_scheme", "fill:u\\72 l(\\68 ttp://evil.example/x)", Entry::Inline,
         CssAdmissionStatus::Rejected},
        {"07_import_plain", "@import \"http://evil.example/a.css\";", Entry::Stylesheet,
         CssAdmissionStatus::Unsupported},
        {"08_import_escaped", "@\\69 mport \"http://evil.example/a.css\";", Entry::Stylesheet,
         CssAdmissionStatus::Unsupported},
        {"09_font_face_plain", "@font-face { src: url(http://evil.example/f.woff); }",
         Entry::Stylesheet, CssAdmissionStatus::Unsupported},
        {"10_font_face_escaped",
         "@font-\\66 ace { src: url(http://evil.example/f.woff); }", Entry::Stylesheet,
         CssAdmissionStatus::Unsupported},
        {"11_malformed_leading", "@bogus; .a { fill: red; }", Entry::Stylesheet,
         CssAdmissionStatus::Unsupported},
        {"12_malformed_trailing", ".a { fill: red;", Entry::Stylesheet,
         CssAdmissionStatus::Rejected},
        {"13_unterminated_comment", ".a { fill: red; } /* oops", Entry::Stylesheet,
         CssAdmissionStatus::Rejected},
        {"14_unterminated_string", ".a { fill: \"unterminated; }", Entry::Stylesheet,
         CssAdmissionStatus::Rejected},
        {"15_unparsed_function_input", "fill:myFn(http://evil.example/x)", Entry::Inline,
         CssAdmissionStatus::Rejected},
        {"16_mixed_valid_invalid", ".a { fill: red; } @bogus; .b { stroke: blue; }",
         Entry::Stylesheet, CssAdmissionStatus::Unsupported},
        // Missing from the native probe: the escaped function inside a real
        // stylesheet rule, not just an inline declaration list.
        {"17_stylesheet_escaped_function",
         ".a { fill: u\\72 l(\"http://evil.example/x\") }", Entry::Stylesheet,
         CssAdmissionStatus::Rejected},
    };

    for (auto const &c : cases) {
        CssAdmissionResult const r = (c.entry == Entry::Inline) ? admit_inline_declarations(c.css)
                                                                : admit_stylesheet(c.css);
        EXPECT_EQ(r.status, c.expected) << c.name;
        if (c.expected != CssAdmissionStatus::Accepted) {
            EXPECT_NE(r.status, CssAdmissionStatus::Accepted) << c.name;
        }
    }
}

// --- Callback cleanup: adversarial corpus never throws or reports Accepted ---

TEST(PreviewCssAdmission, AdversarialCorpusNeverAcceptedAndReusable) {
    std::vector<std::string> const corpus = {
        "fill:url(http://x)",
        "fill:url(//x)",
        "fill:url(data:text/plain,a)",
        "fill:url(#)",
        "fill:url(#a#b)",
        "fill:url(#a b)",
        "fill:u\\72 l(\"http://x\")",
        "fill:rgb(1,2,3",
        ".a{fill:red;",
        "@import \"http://x\";",
        "/*",
        "\"",
        "@font-face{src:url(http://x)}",
    };
    for (auto const &css : corpus) {
        CssAdmissionResult const ri = admit_inline_declarations(css);
        CssAdmissionResult const rs = admit_stylesheet(css);
        EXPECT_NE(ri.status, CssAdmissionStatus::Accepted) << css;
        EXPECT_NE(rs.status, CssAdmissionStatus::Accepted) << css;
    }
    // A valid input still works after the adversarial corpus, proving callbacks
    // and native objects were released.
    CssAdmissionResult const ok = admit_inline_declarations("fill:url(#g)");
    EXPECT_EQ(ok.status, CssAdmissionStatus::Accepted);
}
