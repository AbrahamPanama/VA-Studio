// SPDX-License-Identifier: GPL-2.0-or-later
//
// Clipboard-free, GUI-free unit coverage for the text-paste contracts that were
// previously protected only by the unexecuted live-clipboard integration tests.
// Pure GTest: no display, no application, no clipboard access.
//
// Covered:
//   * src/ui/text-paste.h           plain-text status classification and the
//                                   native parse() non-finite numeric guard
//   * src/ui/text-paste-external.h  decode_external() status mapping
//   * src/ui/clipboard-wait.h       the bounded main-context pump seam, driven
//                                   through a PRIVATE Glib::MainContext
//
// Like the other decoder tests (text-paste-html-test.cpp,
// text-paste-rtf-test.cpp), this file defines no main(): the shared test main
// is testfiles/inkscape-test.cpp, linked through cpp_test_static_library.  That
// main runs Inkscape::Util::Statics, gsl_set_error_handler_off(), Gio::init()
// and Inkscape::GC::init() only; no GUI/application initialization and no
// display are required, so this binary must stay runnable headless.
//
// Every expectation below is a literal computed from the contract (byte
// counts, budget boundaries, CSS values) or taken from reading the
// implementation; production constants are never the only oracle.  Where a
// branch cannot be reached from the public API, that is stated explicitly in a
// comment instead of inventing a value for it.

#include <gtest/gtest.h>

#include <glib.h>
#include <giomm/cancellable.h>
#include <glibmm/main.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "document.h"
#include "ui/clipboard-lease.h"
#include "ui/tools/tool-base.h"
#include <sigc++/scoped_connection.h>
#include "ui/clipboard-wait.h"
#include "ui/text-paste-external.h"
#include "ui/text-paste.h"
#include "text-paste-clipboard-ownership.h"

namespace {

namespace TP = Inkscape::UI::TextPaste;
namespace CW = Inkscape::UI::ClipboardWait;

using TP::PlainTextStatus;
using TP::Representation;

// ===========================================================================
// A. PlainTextStatus / from_plain_text_ex (src/ui/text-paste.h)
// ===========================================================================

TEST(TextPasteUnit, PlainTextOkKeepsExactText)
{
    auto const result = TP::from_plain_text_ex("hello\nworld");

    ASSERT_EQ(result.status, PlainTextStatus::ok) << result.error;
    EXPECT_EQ(result.fragment.plain, "hello\nworld");
    ASSERT_EQ(result.fragment.paragraphs.size(), 2u);
    ASSERT_EQ(result.fragment.paragraphs[0].runs.size(), 1u);
    EXPECT_EQ(result.fragment.paragraphs[0].runs[0].text, "hello");
    ASSERT_EQ(result.fragment.paragraphs[1].runs.size(), 1u);
    EXPECT_EQ(result.fragment.paragraphs[1].runs[0].text, "world");
    // Plain text never invents styling.
    EXPECT_TRUE(result.fragment.paragraphs[0].style.empty());
    EXPECT_TRUE(result.fragment.paragraphs[0].runs[0].style.empty());
    EXPECT_FALSE(result.fragment.empty());
    EXPECT_TRUE(result.error.empty());
}

TEST(TextPasteUnit, PlainTextInvalidUtf8IsInvalidText)
{
    // Observed contract: the byte budget is checked first, then UTF-8
    // validation.  Both payloads below are under every budget, so the only
    // classification is invalid_text - never over_limit and never ok.
    {
        std::string const truncated = std::string("\xC3\x28", 2); // 0xC3 not followed by a continuation byte
        ASSERT_EQ(truncated.size(), 2u);
        ASSERT_FALSE(g_utf8_validate(truncated.data(), static_cast<gssize>(truncated.size()), nullptr));
        auto const result = TP::from_plain_text_ex(truncated);
        EXPECT_EQ(result.status, PlainTextStatus::invalid_text) << result.error;
        EXPECT_FALSE(result.error.empty());
        EXPECT_TRUE(result.fragment.empty());
    }
    {
        std::string const lone_continuation = std::string("\x80", 1); // stray UTF-8 continuation byte
        ASSERT_FALSE(g_utf8_validate(lone_continuation.data(), static_cast<gssize>(lone_continuation.size()), nullptr));
        auto const result = TP::from_plain_text_ex(lone_continuation);
        EXPECT_EQ(result.status, PlainTextStatus::invalid_text) << result.error;
        EXPECT_TRUE(result.fragment.empty());
    }
}

TEST(TextPasteUnit, PlainTextByteBudgetBoundaryIsLiteral65536)
{
    // MAX_CHARS is a byte budget over the UTF-8 wire text and the comparison
    // is strictly greater-than: 65,536 bytes are accepted, 65,537 are not.
    std::string const at_limit(65536, 'a');
    ASSERT_EQ(at_limit.size(), 65536u);
    auto const ok = TP::from_plain_text_ex(at_limit);
    ASSERT_EQ(ok.status, PlainTextStatus::ok) << ok.error;
    EXPECT_EQ(ok.fragment.plain, at_limit);
    EXPECT_EQ(ok.fragment.plain.size(), 65536u);

    std::string const over(65537, 'a');
    ASSERT_EQ(over.size(), 65537u);
    auto const rejected = TP::from_plain_text_ex(over);
    EXPECT_EQ(rejected.status, PlainTextStatus::over_limit) << rejected.error;
    EXPECT_EQ(rejected.error, "plain text exceeds the byte budget");
    EXPECT_TRUE(rejected.fragment.empty()); // whole-paste abort, never a truncated prefix
}

TEST(TextPasteUnit, PlainTextMultibyteByteBudgetCountsEncodedBytes)
{
    // A multi-byte character counts as its full encoded size, and the budget is
    // checked before UTF-8 validation, so an over-budget payload is over_limit
    // even though it is perfectly valid UTF-8.
    std::string const euro = "\xE2\x82\xAC"; // U+20AC, 3 bytes
    std::string const eacute = "\xC3\xA9";   // U+00E9, 2 bytes

    std::string over;
    for (int i = 0; i < 21845; ++i) { // 21,845 * 3 = 65,535
        over += euro;
    }
    over += eacute; // + 2 = 65,537
    ASSERT_EQ(over.size(), 65537u);
    ASSERT_TRUE(g_utf8_validate(over.data(), static_cast<gssize>(over.size()), nullptr));
    auto const rejected = TP::from_plain_text_ex(over);
    EXPECT_EQ(rejected.status, PlainTextStatus::over_limit) << rejected.error;
    EXPECT_EQ(rejected.error, "plain text exceeds the byte budget");
    EXPECT_TRUE(rejected.fragment.empty());

    // Control: the same kind of multibyte content at exactly 65,536 bytes is ok.
    std::string at_limit;
    for (int i = 0; i < 21844; ++i) { // 21,844 * 3 = 65,532
        at_limit += euro;
    }
    at_limit += eacute; // + 2 = 65,534
    at_limit += eacute; // + 2 = 65,536
    ASSERT_EQ(at_limit.size(), 65536u);
    auto const ok = TP::from_plain_text_ex(at_limit);
    ASSERT_EQ(ok.status, PlainTextStatus::ok) << ok.error;
    EXPECT_EQ(ok.fragment.plain, at_limit);
    EXPECT_EQ(ok.fragment.plain.size(), 65536u);
}

TEST(TextPasteUnit, PlainTextParagraphBudgetIsLiteral1024)
{
    // One '\n' starts one paragraph.  The check is
    // paragraphs.size() + 2 > 1024 before a paragraph is pushed, so 1,023
    // newlines (1,024 paragraphs) is the largest accepted payload and the
    // 1,024th newline is over_limit.
    std::string const at_limit(1023, '\n');
    auto const ok = TP::from_plain_text_ex(at_limit);
    ASSERT_EQ(ok.status, PlainTextStatus::ok) << ok.error;
    EXPECT_EQ(ok.fragment.paragraphs.size(), 1024u);
    EXPECT_EQ(ok.fragment.plain, at_limit);

    std::string const over(1024, '\n');
    auto const rejected = TP::from_plain_text_ex(over);
    EXPECT_EQ(rejected.status, PlainTextStatus::over_limit) << rejected.error;
    EXPECT_EQ(rejected.error, "plain text exceeds the paragraph budget");
    EXPECT_TRUE(rejected.fragment.empty()); // no partial import

    // The run-budget branch of from_plain_text_ex() is unreachable from this
    // API: it emits at most one run per paragraph, and MAX_PARAGRAPHS (1,024)
    // is smaller than MAX_RUNS (4,096).  A 4,096-line payload therefore hits
    // the paragraph budget first, but it must still be classified as a
    // whole-paste abort rather than a truncated import.
    std::string many_lines;
    for (int i = 0; i < 4096; ++i) {
        many_lines += "a\n";
    }
    auto const lines = TP::from_plain_text_ex(many_lines);
    EXPECT_EQ(lines.status, PlainTextStatus::over_limit) << lines.error;
    EXPECT_EQ(lines.error, "plain text exceeds the paragraph budget");
    EXPECT_TRUE(lines.fragment.empty());
}

TEST(TextPasteUnit, PlainTextEmptyIsOkWithEmptyPlainText)
{
    // Observed contract (read from from_plain_text_ex): an empty payload passes
    // the byte budget and g_utf8_validate(""), the newline loop appends no run,
    // and one empty paragraph is emitted.  Status is ok (not invalid_text) and
    // the authoritative plain text is empty, so a paste of nothing is a
    // successful no-op rather than a rejection.
    auto const result = TP::from_plain_text_ex("");
    EXPECT_EQ(result.status, PlainTextStatus::ok) << result.error;
    EXPECT_TRUE(result.error.empty());
    EXPECT_TRUE(result.fragment.plain.empty());
    EXPECT_TRUE(result.fragment.empty());
    ASSERT_EQ(result.fragment.paragraphs.size(), 1u);
    EXPECT_TRUE(result.fragment.paragraphs[0].runs.empty());
    EXPECT_TRUE(result.fragment.paragraphs[0].style.empty());
}

// ===========================================================================
// B. Native parse() non-finite numeric guard (src/ui/text-paste.h)
// ===========================================================================

namespace {

// Literal version-2 wire payload.  Grammar read from serialize()/parse() and the
// native fixtures in text-paste-test.cpp:
//   "vac-text-fragment\t2\n" "P\t<style>\n" "R\t<style>\t<text>\n"
std::string native_payload(std::string_view paragraph_style, std::string_view run_style, std::string_view text)
{
    std::string out = "vac-text-fragment\t2\n";
    out += "P\t";
    out += paragraph_style;
    out += '\n';
    out += "R\t";
    out += run_style;
    out += '\t';
    out += text;
    out += '\n';
    return out;
}

} // namespace

TEST(TextPasteUnit, NativeParseRejectsNonFiniteParagraphStyle)
{
    // parse() runs style_lengths_are_valid() and detail::style_numbers_are_finite()
    // on the sanitized paragraph style before the run records are read.
    // line-height is an allow-listed PARAGRAPH property and a length rule, so
    // 1e999px survives sanitize_style() and parses as infinity: the whole
    // payload is rejected (nullopt), never partially accepted.
    std::string const payload = native_payload("line-height:1e999px;", "", "hello");
    std::string error;
    auto const result = TP::parse(payload, &error);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(error, "invalid paragraph style");
}

TEST(TextPasteUnit, NativeParseDropsOutOfScopeParagraphFontSize)
{
    // Observed contract (found by executing this test, then re-reading
    // sanitize_style): font-size is a CHARACTER property, so
    // sanitize_style(..., paragraph_scope=true) drops it from a P record before
    // validation.  A non-finite font-size in the paragraph style therefore
    // never reaches any document style; parse() succeeds with an empty
    // paragraph style and intact text.  The same declaration in a RUN style is
    // rejected - see NativeParseRejectsNonFiniteRunStyle.
    std::string const payload = native_payload("font-size:1e999px;", "", "hello");
    auto const result = TP::parse(payload);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->paragraphs.size(), 1u);
    EXPECT_TRUE(result->paragraphs[0].style.empty());
    EXPECT_EQ(result->plain, "hello");
}

TEST(TextPasteUnit, NativeParseRejectsNonFiniteRunStyle)
{
    // font-size is allow-listed in RUN scope and is a length rule; 1e999px is
    // infinity, so the run style fails the length/numeric guard.
    {
        std::string const payload = native_payload("", "font-size:1e999px;", "hello");
        std::string error;
        auto const result = TP::parse(payload, &error);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(error, "invalid run style");
    }
    // letter-spacing is also an allow-listed length property; "nanpx" is
    // consumed by g_ascii_strtod() as a NaN numeric prefix (verified with a
    // scratch probe), so the run style fails the same guard.
    {
        std::string const payload = native_payload("", "letter-spacing:nanpx;", "hello");
        std::string error;
        auto const result = TP::parse(payload, &error);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(error, "invalid run style");
    }
    // opacity is not one of the ten length properties, so this case is caught by
    // detail::style_numbers_are_finite() specifically, not by the length
    // validator.  Infinity must never reach an SVG style attribute.
    {
        std::string const payload = native_payload("", "opacity:1e999;", "hello");
        std::string error;
        auto const result = TP::parse(payload, &error);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(error, "invalid run style");
    }
}

TEST(TextPasteUnit, NativeParseAcceptsFiniteFontSize24)
{
    // Positive control for the two rejections above: the identical wire shape
    // with a finite value parses, and the run style carries the literal 24.
    auto const result = TP::parse(native_payload("", "font-size:24px;", "hello"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->plain, "hello");
    ASSERT_EQ(result->paragraphs.size(), 1u);
    ASSERT_EQ(result->paragraphs[0].runs.size(), 1u);
    EXPECT_EQ(result->paragraphs[0].runs[0].text, "hello");
    EXPECT_EQ(result->paragraphs[0].runs[0].style, "font-size:24px;");

    // Independent numeric oracle: cut the CSS value out by hand and let the C
    // library parse it, without any production length helper.
    std::string const &style = result->paragraphs[0].runs[0].style;
    auto const colon = style.find(':');
    auto const semi = style.find(';', colon == std::string::npos ? 0 : colon);
    ASSERT_NE(colon, std::string::npos);
    ASSERT_NE(semi, std::string::npos);
    std::string const value = style.substr(colon + 1, semi - colon - 1);
    EXPECT_EQ(value, "24px");
    char *end = nullptr;
    double const number = std::strtod(value.c_str(), &end);
    EXPECT_EQ(number, 24.0);
    EXPECT_EQ(std::string(end), "px");
}

// ===========================================================================
// C. decode_external status mapping (src/ui/text-paste-external.h)
// ===========================================================================

TEST(TextPasteUnit, DecodeExternalHtmlSmallDocumentIsUsable)
{
    // A small, valid HTML document: text plus one allow-listed colour
    // declaration.  usable means a non-empty validated fragment was produced;
    // the colour makes the formatting worth asking the user about.
    auto const result =
        TP::decode_external("<p style=\"color:#ff0000\">hi</p>", Representation::Html);

    EXPECT_TRUE(result.usable);
    EXPECT_FALSE(result.malformed);
    EXPECT_FALSE(result.limit_exceeded);
    EXPECT_TRUE(result.has_meaningful_styles);
    EXPECT_EQ(result.fragment.plain, "hi");
    ASSERT_EQ(result.fragment.paragraphs.size(), 1u);
    ASSERT_EQ(result.fragment.paragraphs[0].runs.size(), 1u);
    EXPECT_EQ(result.fragment.paragraphs[0].runs[0].text, "hi");
    EXPECT_EQ(result.fragment.paragraphs[0].runs[0].style, "color:#ff0000;");
}

TEST(TextPasteUnit, DecodeExternalMalformedUnderCapHtmlIsMalformed)
{
    // Under every cap and containing readable text, but the declared charset is
    // neither UTF-8 nor US-ASCII: the decoder rejects the representation as a
    // whole.  malformed==true with usable==false is exactly the case where the
    // caller must fall back to the complete plain alternative - it must not be
    // confused with limit_exceeded (which aborts the paste).
    std::string const html = "<meta charset=\"iso-8859-1\"><p>hello</p>";
    ASSERT_LT(html.size(), 262145u);
    auto const result = TP::decode_external(html, Representation::Html);

    EXPECT_FALSE(result.usable);
    EXPECT_TRUE(result.malformed);
    EXPECT_FALSE(result.limit_exceeded);
    EXPECT_TRUE(result.fragment.empty());
    EXPECT_FALSE(result.error.empty());
}

TEST(TextPasteUnit, DecodeExternalOverCapHtmlIsLimitExceeded)
{
    // Literal byte bound: 256 KiB = 262,144; one byte more must abort the whole
    // paste (limit_exceeded), not be reported as malformed and not be truncated.
    std::string const payload(262145, 'x');
    ASSERT_EQ(payload.size(), 262145u);
    auto const result = TP::decode_external(payload, Representation::Html);

    EXPECT_TRUE(result.limit_exceeded);
    EXPECT_FALSE(result.usable);
    EXPECT_FALSE(result.malformed);
    EXPECT_TRUE(result.fragment.empty());
}

TEST(TextPasteUnit, DecodeExternalOverCapRtfIsLimitExceeded)
{
    // Same literal 262,145-byte payload through the RTF representation: the RTF
    // decoder's over_limit status must map onto limit_exceeded as well.
    std::string const payload(262145, 'x');
    ASSERT_EQ(payload.size(), 262145u);
    auto const result = TP::decode_external(payload, Representation::Rtf);

    EXPECT_TRUE(result.limit_exceeded);
    EXPECT_FALSE(result.usable);
    EXPECT_FALSE(result.malformed);
    EXPECT_TRUE(result.fragment.empty());
}

TEST(TextPasteUnit, DecodeExternalNativeAndPlainAreMalformed)
{
    // Observed contract (read from decode_external): Native and Plain are not
    // external rich representations.  Both map to malformed==true with the
    // literal error below, so the clipboard reader keeps using its own strict
    // parsers for those representations.
    for (Representation const representation : {Representation::Plain, Representation::Native}) {
        auto const result = TP::decode_external("hello", representation);
        EXPECT_FALSE(result.usable);
        EXPECT_TRUE(result.malformed);
        EXPECT_FALSE(result.limit_exceeded);
        EXPECT_TRUE(result.fragment.empty());
        EXPECT_FALSE(result.has_meaningful_styles);
        EXPECT_EQ(result.error, "not an external rich representation");
    }
}

// ===========================================================================
// D. Nested function / comma non-finite numbers (HTML fixtures)
// ===========================================================================

namespace {

// Independent, test-side numeric scanner.  It walks every byte offset of a CSS
// declaration value and records every numeric prefix that std::strtod()
// consumes; a consumed prefix that is not finite fails the scan.  It
// deliberately does not call TextPaste::style_numbers_are_finite(),
// parse_length_token() or any other production helper.
bool value_has_nonfinite_number(std::string_view value)
{
    for (std::size_t i = 0; i < value.size(); ++i) {
        std::string const rest(value.substr(i));
        char *end = nullptr;
        double const number = std::strtod(rest.c_str(), &end);
        if (end != rest.c_str() && !std::isfinite(number)) {
            return true;
        }
    }
    return false;
}

bool style_has_nonfinite_number(std::string_view css)
{
    std::size_t pos = 0;
    while (pos < css.size()) {
        auto const semi = css.find(';', pos);
        auto const decl = css.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = semi == std::string_view::npos ? css.size() : semi + 1;
        auto const colon = decl.find(':');
        if (colon != std::string_view::npos && value_has_nonfinite_number(decl.substr(colon + 1))) {
            return true;
        }
    }
    return false;
}

void expect_no_nonfinite_styles(TP::ExternalDecode const &result)
{
    for (auto const &paragraph : result.fragment.paragraphs) {
        EXPECT_FALSE(style_has_nonfinite_number(paragraph.style)) << "paragraph style: " << paragraph.style;
        for (auto const &run : paragraph.runs) {
            EXPECT_FALSE(style_has_nonfinite_number(run.style)) << "run style: " << run.style;
        }
    }
}

} // namespace

TEST(TextPasteUnit, NestedFunctionAndCommaNonFiniteNumbersNeverSurvive)
{
    struct Fixture {
        char const *html;
        char const *plain;
    };
    std::vector<Fixture> const fixtures = {
        // Non-finite behind a nested CSS function: dropped as an unsupported
        // font-size (font-size must resolve to one absolute document length).
        {"<p style=\"font-size:calc(1px + 1e999px)\">x</p>", "x"},
        // Non-finite as the only argument of a function.
        {"<p style=\"font-size:calc(1e999px)\">x</p>", "x"},
        // Non-finite after a comma inside a function (not an allow-listed
        // property, so the declaration is dropped before any conversion).
        {"<p style=\"width:calc(1px,1e999px)\">x</p>", "x"},
        // Plain non-finite absolute length.
        {"<p style=\"font-size:1e999px\">x</p>", "x"},
        // Non-finite non-length numeric property: this one is caught by the
        // shared style_numbers_are_finite() guard rather than a length rule.
        {"<p style=\"opacity:1e999\">x</p>", "x"},
        // Non-finite after a comma in an allow-listed comma-list property.
        {"<p style=\"stroke-dasharray:1px,1e999px\">x</p>", "x"},
    };

    for (auto const &fixture : fixtures) {
        auto const result = TP::decode_external(fixture.html, Representation::Html);
        ASSERT_TRUE(result.usable) << fixture.html << " -> " << result.error;
        EXPECT_EQ(result.fragment.plain, fixture.plain) << fixture.html;
        // Independent scanner: whatever style survives must be finite, and the
        // text must survive even when the offending declaration is dropped.
        expect_no_nonfinite_styles(result);
    }
}

TEST(TextPasteUnit, FiniteCalcFontSizeIsDroppedAndFinitePxIsPreserved)
{
    // Rule verified by reading src/ui/text-paste-html.cpp:
    //   apply_declarations() resolves font-size through convert_length(...,
    //   LengthPolicy::FontSize), which first calls the shared
    //   parse_length_token().  "calc(1px + 2px)" has no numeric prefix
    //   (g_ascii_strtod consumes nothing), so the token is not numeric, and for
    //   LengthPolicy::FontSize a non-numeric value returns nullopt with the
    //   comment "no keyword identifies one document-space size".  This decoder
    //   has no CSS calc() evaluation, so the declaration is dropped, not
    //   evaluated; the character keeps the inherited/default style.
    {
        auto const result =
            TP::decode_external("<p style=\"font-size:calc(1px + 2px)\">x</p>", Representation::Html);
        ASSERT_TRUE(result.usable) << result.error;
        EXPECT_EQ(result.fragment.plain, "x");
        ASSERT_EQ(result.fragment.paragraphs.size(), 1u);
        ASSERT_EQ(result.fragment.paragraphs[0].runs.size(), 1u);
        EXPECT_TRUE(result.fragment.paragraphs[0].runs[0].style.empty())
            << "finite calc() is dropped exactly like non-finite calc(): " << result.fragment.paragraphs[0].runs[0].style;
        expect_no_nonfinite_styles(result);
    }
    // Positive half: a plain finite absolute length is preserved verbatim, so
    // the scanner above is not vacuous.
    {
        auto const result = TP::decode_external("<p style=\"font-size:24px\">x</p>", Representation::Html);
        ASSERT_TRUE(result.usable) << result.error;
        EXPECT_EQ(result.fragment.plain, "x");
        ASSERT_EQ(result.fragment.paragraphs.size(), 1u);
        ASSERT_EQ(result.fragment.paragraphs[0].runs.size(), 1u);
        EXPECT_EQ(result.fragment.paragraphs[0].runs[0].style, "font-size:24px;");
        expect_no_nonfinite_styles(result);
    }
}

// ===========================================================================
// E. Bounded wait seam (src/ui/clipboard-wait.h)
// ===========================================================================

namespace {

/** One dispatch counter per attached idle source. */
struct IdlePump {
    explicit IdlePump(std::size_t sources)
        : counts(sources, 0)
    {}

    int total() const
    {
        int sum = 0;
        for (int count : counts) {
            sum += count;
        }
        return sum;
    }

    std::vector<int> counts;
};

gboolean idle_tick(gpointer data)
{
    ++*static_cast<int *>(data);
    return G_SOURCE_CONTINUE; // perpetually re-arming: pending() never becomes false
}

/**
 * Attach @c pump.counts.size() idle sources to a PRIVATE context.  Each source
 * owns its own counter slot; the caller destroys and unrefs the returned
 * sources.  The counters vector must not reallocate while the sources live.
 */
std::vector<GSource *> attach_idle_sources(Glib::MainContext &context, IdlePump &pump)
{
    std::vector<GSource *> sources;
    sources.reserve(pump.counts.size());
    for (std::size_t i = 0; i < pump.counts.size(); ++i) {
        GSource *source = g_idle_source_new();
        g_source_set_callback(source, idle_tick, &pump.counts[i], nullptr);
        g_source_attach(source, context.gobj());
        sources.push_back(source);
    }
    return sources;
}

void destroy_sources(std::vector<GSource *> &sources)
{
    for (GSource *source : sources) {
        g_source_destroy(source);
        g_source_unref(source);
    }
    sources.clear();
}

} // namespace

TEST(TextPasteUnit, PumpStepHonorsExplicitDispatchBudgetWithEndlessSource)
{
    auto context = Glib::MainContext::create();
    ASSERT_TRUE(context);
    // A private context, never the default one used by the application.
    EXPECT_NE(context.get(), Glib::MainContext::get_default().get());

    IdlePump pump(1);
    auto sources = attach_idle_sources(*context, pump);
    ASSERT_TRUE(context->pending());

    gint64 const deadline = g_get_monotonic_time() + 100 * 1000; // explicit 100 ms
    int const dispatch_budget = 4;                                // explicit, NOT the production default

    gint64 const started = g_get_monotonic_time();
    bool const expired = CW::pump_step(*context, deadline, dispatch_budget);
    gint64 const elapsed_us = g_get_monotonic_time() - started;

    // The source re-arms forever, so pending() is still true, yet pump_step()
    // returned before the deadline: its finite budget ran out after exactly
    // dispatch_budget iterations (one ready source => one dispatch per
    // iteration; verified with a scratch GLib probe).
    EXPECT_FALSE(expired);
    EXPECT_EQ(pump.counts[0], dispatch_budget);
    EXPECT_TRUE(context->pending());
    EXPECT_LT(elapsed_us, 2000 * 1000);

    // Repeating the call must still stop at the deadline (checked inside the
    // loop) instead of spinning forever on the endless source.
    bool reached = false;
    int previous = pump.counts[0];
    while (!(reached = CW::pump_step(*context, deadline, dispatch_budget))) {
        EXPECT_LE(pump.counts[0] - previous, dispatch_budget);
        previous = pump.counts[0];
    }
    EXPECT_TRUE(reached);
    EXPECT_LT(g_get_monotonic_time() - started, 3000 * 1000);
    EXPECT_TRUE(context->pending()); // never became idle

    destroy_sources(sources);
}

TEST(TextPasteUnit, PumpStepStaysBoundedWithHundredsOfReadySources)
{
    constexpr std::size_t source_count = 256;
    auto context = Glib::MainContext::create();
    ASSERT_TRUE(context);
    EXPECT_NE(context.get(), Glib::MainContext::get_default().get());

    IdlePump pump(source_count);
    auto sources = attach_idle_sources(*context, pump);
    ASSERT_TRUE(context->pending());

    gint64 const deadline = g_get_monotonic_time() + 100 * 1000; // explicit 100 ms
    int const dispatch_budget = 4;                                // explicit, NOT the production default

    gint64 const started = g_get_monotonic_time();
    bool const expired = CW::pump_step(*context, deadline, dispatch_budget);
    gint64 const elapsed_us = g_get_monotonic_time() - started;

    EXPECT_FALSE(expired);
    // Each iteration dispatches every ready source once, so each per-source
    // counter equals the number of iterations: bounded by (here exactly) the
    // explicit dispatch_budget even with 256 perpetually re-arming sources.
    for (std::size_t i = 0; i < source_count; ++i) {
        EXPECT_LE(pump.counts[i], dispatch_budget) << "source " << i;
        EXPECT_EQ(pump.counts[i], dispatch_budget) << "source " << i;
    }
    EXPECT_TRUE(context->pending());

    bool reached = false;
    while (!(reached = CW::pump_step(*context, deadline, dispatch_budget))) {
        // Each call is bounded by dispatch_budget iterations; the loop as a
        // whole is bounded by the absolute deadline.
    }
    EXPECT_TRUE(reached);
    EXPECT_LT(g_get_monotonic_time() - started, 3000 * 1000);

    destroy_sources(sources);
}

TEST(TextPasteUnit, WaitForRequestInExpiresCancelsAndLeavesStateIntact)
{
    constexpr std::size_t source_count = 256;
    auto context = Glib::MainContext::create();
    ASSERT_TRUE(context);
    EXPECT_NE(context.get(), Glib::MainContext::get_default().get());

    // The state is shared_ptr-owned: the producer holds one owner and the
    // clipboard wait borrows this one.  wait_for_request_in() never copies the
    // object into a stack frame of its own, so a late producer can only reach
    // live, shared memory; this test keeps its owner alive across teardown.
    struct WaitState {
        bool done = false;
        int sentinel = 1234; // detects any overwrite of the object while the context dies
    };
    auto state = std::make_shared<WaitState>();
    auto cancellable = Gio::Cancellable::create();
    ASSERT_FALSE(cancellable->is_cancelled());

    IdlePump pump(source_count);
    auto sources = attach_idle_sources(*context, pump);
    ASSERT_TRUE(context->pending());

    gint64 const deadline = g_get_monotonic_time() + 100 * 1000; // explicit 100 ms
    constexpr gint64 cancel_grace_us = 100 * 1000;               // explicit 100 ms
    int const dispatch_budget = 4;                               // explicit, NOT the production default

    gint64 const started = g_get_monotonic_time();
    bool const completed =
        CW::wait_for_request_in(*context, state, cancellable, deadline, cancel_grace_us, dispatch_budget);
    gint64 const elapsed_us = g_get_monotonic_time() - started;

    // state->done is never set, so only the absolute deadline can end the wait.
    EXPECT_FALSE(completed);
    EXPECT_FALSE(state->done);
    EXPECT_TRUE(cancellable->is_cancelled()); // expiry cancels the producer
    EXPECT_LT(elapsed_us, 3000 * 1000);       // generous wall-clock bound
    EXPECT_TRUE(context->pending());          // sources were still re-arming throughout

    // Lifetime/cancellation safety: the wait has returned, but the state stays
    // shared_ptr-owned and must be intact after the private context and every
    // source are destroyed.  A dangling or freed-memory access would corrupt
    // the sentinel or crash here.
    destroy_sources(sources);
    context.reset();
    EXPECT_FALSE(state->done);
    EXPECT_EQ(state.use_count(), 1);
    EXPECT_EQ(state->sentinel, 1234);
    EXPECT_TRUE(cancellable->is_cancelled());
}

// ===========================================================================
// F. In-process clipboard fixture ownership decision (test support header)
// ===========================================================================
// The integration fixture's cleanup must never overwrite a newer user copy.
// GTK 4 has no change count, so the fixture counts GdkClipboard::signal_changed
// emissions and records the count after each publication it makes. This pure
// decision table is the runnable oracle for the branches the integration
// fixture cannot force deterministically (a third-party write during a test).
TEST(TextPasteUnit, OwnershipDecisionRestoresOnlyWhileTheFixtureOwnsTheClipboard)
{
    using TextPasteTestSupport::RestoreDecision;
    using TextPasteTestSupport::decide_clipboard_restore;

    // Snapshot present, tracking available, clipboard still local, token unchanged -> restore.
    EXPECT_EQ(decide_clipboard_restore(true, true, true, 7u, 7u), RestoreDecision::restore);
    // Token moved: a newer write happened -> never overwrite it.
    EXPECT_EQ(decide_clipboard_restore(true, true, true, 8u, 7u), RestoreDecision::skip_foreign_change);
    // The clipboard is no longer local: another owner holds it -> never overwrite it.
    EXPECT_EQ(decide_clipboard_restore(true, true, false, 7u, 7u), RestoreDecision::skip_foreign_change);
    // No snapshot -> nothing to restore.
    EXPECT_EQ(decide_clipboard_restore(false, true, true, 7u, 7u), RestoreDecision::skip_no_snapshot);
    // Ownership cannot be proven -> conservative skip, never a restore.
    EXPECT_EQ(decide_clipboard_restore(true, false, true, 7u, 7u), RestoreDecision::skip_foreign_change);
    // A missing snapshot wins over a foreign change: still a skip.
    EXPECT_EQ(decide_clipboard_restore(false, false, false, 8u, 7u), RestoreDecision::skip_no_snapshot);
}

// ===========================================================================
// G. Clipboard-paste destination lease (src/ui/clipboard-lease.h)
// ===========================================================================
// The image and object/SVG paste paths capture a desktop + document before a
// bounded clipboard wait that pumps the main context, and revalidate both
// afterwards. Any dead/changed destination must abort mutation-free, so the
// decision is an AND of all four facts (r3 product review P1-2/P1-3 plus r4
// product review P2-1, the active-window fact).
TEST(TextPasteUnit, ClipboardDestinationLeaseRequiresLiveDesktopLiveDocumentAndSameDocument)
{
    using Inkscape::UI::ClipboardLease::DestinationFacts;
    using Inkscape::UI::ClipboardLease::destination_valid;

    EXPECT_TRUE(destination_valid(DestinationFacts{true, true, true, true}));

    // A destroyed desktop invalidates the destination even if the document flag
    // was never cleared (the window close frees both).
    EXPECT_FALSE(destination_valid(DestinationFacts{false, true, true, true}));
    // A destroyed document invalidates it even while the window survives.
    EXPECT_FALSE(destination_valid(DestinationFacts{true, false, true, true}));
    // The same live desktop switched to another document: importing/selecting in
    // the captured document would use the wrong destination.
    EXPECT_FALSE(destination_valid(DestinationFacts{true, true, false, true}));
    // r4 product review P2-1: the captured pair is alive and paired, but the
    // active window is now a different one. file_import() resolves its layer and
    // selection from the active window, so this must abort as well.
    EXPECT_FALSE(destination_valid(DestinationFacts{true, true, true, false}));
    // Every combination with a dead object is false; no accidental promotion of
    // a "same document" pointer comparison.
    EXPECT_FALSE(destination_valid(DestinationFacts{false, false, true, true}));
    EXPECT_FALSE(destination_valid(DestinationFacts{false, true, false, true}));
    EXPECT_FALSE(destination_valid(DestinationFacts{true, false, false, true}));
    EXPECT_FALSE(destination_valid(DestinationFacts{false, false, false, true}));
    // The active fact cannot rescue any other failed fact.
    EXPECT_FALSE(destination_valid(DestinationFacts{true, true, false, false}));
    EXPECT_FALSE(destination_valid(DestinationFacts{false, true, true, false}));
    EXPECT_FALSE(destination_valid(DestinationFacts{true, false, true, false}));
    EXPECT_FALSE(destination_valid(DestinationFacts{false, false, true, false}));
    EXPECT_FALSE(destination_valid(DestinationFacts{false, true, false, false}));
    EXPECT_FALSE(destination_valid(DestinationFacts{true, false, false, false}));
    EXPECT_FALSE(destination_valid(DestinationFacts{false, false, false, false}));

    // The exhaustive form of the same contract: every one of the sixteen fact
    // combinations must be accepted exactly when all four facts hold, so no
    // future edit can make one fact shadow another.
    for (unsigned bits = 0; bits < 16; ++bits) {
        DestinationFacts const facts{(bits & 1u) != 0, (bits & 2u) != 0, (bits & 4u) != 0, (bits & 8u) != 0};
        EXPECT_EQ(destination_valid(facts), bits == 15u)
            << "bits=" << bits << " (desktop_alive, document_alive, same_document, active_is_captured)";
    }
}

// The image route (r4 product review P2-1) asks the pure helper whether the
// captured desktop is still the process active one. It must be a pointer
// identity test only — never a dereference — so the cases below use opaque
// non-null values: a window switch, the last window closing (nullptr) while the
// captured window is alive, and the captured-and-active window being the same.
TEST(TextPasteUnit, ClipboardDestinationActiveFactIsPointerIdentityOnly)
{
    using Inkscape::UI::ClipboardLease::DestinationFacts;
    using Inkscape::UI::ClipboardLease::destination_valid;
    using Inkscape::UI::ClipboardLease::is_active_desktop;

    int captured_storage = 0;
    int other_storage = 0;
    void const *const captured = &captured_storage;
    void const *const other = &other_storage;
    void const *const no_window = nullptr;

    // Same pointer: the paste still belongs to the window it was issued on.
    EXPECT_TRUE(is_active_desktop(captured, captured));
    // Another window of the same process: the active desktop changed.
    EXPECT_FALSE(is_active_desktop(other, captured));
    // The last window closed during the wait while the captured window is alive.
    EXPECT_FALSE(is_active_desktop(no_window, captured));
    // Both null: a captured desktop is never null (the paste returns early
    // otherwise), so this "equal" case must still not be reachable in practice;
    // it is pinned here only to document that identity alone is not the whole
    // decision — `desktop_alive`/`document_alive`/`same_document` still apply.
    EXPECT_TRUE(is_active_desktop(no_window, no_window));
    EXPECT_FALSE(destination_valid(
        DestinationFacts{false, false, false, is_active_desktop(no_window, no_window)}));

    // The image-path fact after a window switch: everything else still holds,
    // only the active desktop moved -> the paste must abort.
    EXPECT_FALSE(destination_valid(DestinationFacts{true, true, true, is_active_desktop(other, captured)}));
    // The last window was closed during the wait while the captured pair is
    // alive and paired -> abort as well.
    EXPECT_FALSE(destination_valid(DestinationFacts{true, true, true, is_active_desktop(no_window, captured)}));
    // Unchanged destination -> the only accepted case.
    EXPECT_TRUE(destination_valid(DestinationFacts{true, true, true, is_active_desktop(captured, captured)}));
}

// ===========================================================================
// H. Fixture publication ledger (test support header, r3 tests review P2-1)
// ===========================================================================
// Every fixture publication must refresh the ownership token exactly once. The
// live helper setRawClipboardMulti() previously refreshed nothing, so the
// fixture's own newest write looked like a foreign change at cleanup. The
// ledger is the accounting the live fixture now drives; this is the
// clipboard-free regression.
TEST(TextPasteUnit, FixturePublicationLedgerNotesExactlyOneTokenPerPublication)
{
    using TextPasteTestSupport::FixturePublicationLedger;
    using TextPasteTestSupport::RestoreDecision;
    using TextPasteTestSupport::decide_clipboard_restore;

    FixturePublicationLedger ledger;
    EXPECT_FALSE(ledger.tracking_available());
    EXPECT_EQ(ledger.publication_count(), 0u);

    ledger.begin_tracking(5);
    ASSERT_TRUE(ledger.tracking_available());
    EXPECT_EQ(ledger.own_publish_token(), 5u);

    int publishes = 0;
    int token_reads = 0;
    bool const recorded =
        ledger.publication([&] { ++publishes; return true; }, [&] { ++token_reads; return 6u; });
    EXPECT_TRUE(recorded);
    EXPECT_EQ(publishes, 1);
    EXPECT_EQ(token_reads, 1); // exactly one counter read per completed publication
    EXPECT_EQ(ledger.publication_count(), 1u);
    EXPECT_EQ(ledger.own_publish_token(), 6u);

    // The P2-1 consequence, positively: with the token refreshed, the fixture's
    // own last write restores. Without the refresh it would look foreign and be
    // refused (the decision table's skip_foreign_change branch).
    EXPECT_EQ(decide_clipboard_restore(true, ledger.tracking_available(), true, 6u,
                                       ledger.own_publish_token()),
              RestoreDecision::restore);
    EXPECT_EQ(decide_clipboard_restore(true, ledger.tracking_available(), true, 7u,
                                       ledger.own_publish_token()),
              RestoreDecision::skip_foreign_change);

    // A failed publication records nothing: no token read, no count, no move.
    bool const failed = ledger.publication([] { return false; }, [&] { ++token_reads; return 9u; });
    EXPECT_FALSE(failed);
    EXPECT_EQ(token_reads, 1);
    EXPECT_EQ(ledger.publication_count(), 1u);
    EXPECT_EQ(ledger.own_publish_token(), 6u);

    // Two more publications: the token always describes the newest one.
    EXPECT_TRUE(ledger.publication([] { return true; }, [] { return 7u; }));
    EXPECT_TRUE(ledger.publication([] { return true; }, [] { return 8u; }));
    EXPECT_EQ(ledger.publication_count(), 3u);
    EXPECT_EQ(ledger.own_publish_token(), 8u);

    // Without change tracking (no display) there is no token to refresh; the
    // publication still runs and nothing is recorded.
    FixturePublicationLedger untracked;
    int untracked_reads = 0;
    EXPECT_TRUE(untracked.publication([] { return true; }, [&] { ++untracked_reads; return 3u; }));
    EXPECT_EQ(untracked_reads, 0);
    EXPECT_EQ(untracked.publication_count(), 0u);
}

} // namespace

namespace {
// Drives a real bounded clipboard wait on a private main context. `during_wait`
// runs inside the wait, exactly where a queued window close or a second Ctrl+V
// would be dispatched.
template <typename F>
void run_wait_with(F &&during_wait)
{
    namespace CW = Inkscape::UI::ClipboardWait;
    auto context = Glib::MainContext::create();
    struct State { bool done = false; };
    auto state = std::make_shared<State>();
    struct Dispatch { std::function<void()> body; State *state; } dispatch{std::forward<F>(during_wait), state.get()};
    auto source = g_idle_source_new();
    g_source_set_callback(source, [](gpointer data) -> gboolean {
        auto &d = *static_cast<Dispatch *>(data);
        d.body();
        d.state->done = true;
        return G_SOURCE_REMOVE;
    }, &dispatch, nullptr);
    g_source_attach(source, context->gobj());
    EXPECT_TRUE(CW::wait_for_request_in(*context, state, {}, g_get_monotonic_time() + 1000000, 0));
    g_source_destroy(source);
    g_source_unref(source);
}
} // namespace

// ST-P item 1: exercises the real DestinationLease used by all clipboard waits.
TEST(TextPasteUnit, ClipboardDestinationLeaseDetectsDocumentDestroyedDuringWait)
{
    using Inkscape::UI::ClipboardLease::DestinationLease;
    std::string_view const svg =
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='1' height='1'/></svg>";

    // Positive case: a live destination stays valid across a whole wait.
    {
        auto doc = SPDocument::createNewDocFromMem(svg);
        ASSERT_TRUE(doc);
        DestinationLease const lease(nullptr, doc.get());
        EXPECT_TRUE(lease.valid());
        run_wait_with([] {});
        EXPECT_TRUE(lease.valid());
    }
    // A null destination (no desktop, no document) has nothing to lose.
    {
        DestinationLease const lease(nullptr, nullptr);
        run_wait_with([] {});
        EXPECT_TRUE(lease.valid());
    }
    // The document is destroyed while the wait is pumping: the lease must report it
    // and the caller must perform no destination access and record no Undo step.
    {
        auto doc = SPDocument::createNewDocFromMem(svg);
        ASSERT_TRUE(doc);
        auto lease = std::make_unique<DestinationLease>(nullptr, doc.get());
        ASSERT_TRUE(lease->valid());
        run_wait_with([&] { doc.reset(); });
        EXPECT_EQ(doc, nullptr);
        EXPECT_FALSE(lease->valid());
        int accesses = 0;
        int undo_steps = 0;
        if (lease->valid()) { ++accesses; ++undo_steps; }
        EXPECT_EQ(accesses, 0);
        EXPECT_EQ(undo_steps, 0);
        lease.reset(); // the scoped connections must disconnect cleanly after the document is gone
    }
    // Lease destroyed first, then the document: no dangling slot may fire.
    {
        auto doc = SPDocument::createNewDocFromMem(svg);
        ASSERT_TRUE(doc);
        { DestinationLease const lease(nullptr, doc.get()); }
        doc.reset();
    }
}

// ST-P item 2 (K7): a second paste() that starts while the first one is still
// waiting for a slow owner is refused, and only the outermost call clears the flag.
TEST(TextPasteUnit, ClipboardPasteReentryIsRefusedDuringWait)
{
    using Inkscape::UI::ClipboardLease::ReentryGuard;
    bool in_progress = false;
    int nested_entered = 0;
    int nested_refused = 0;
    {
        ReentryGuard const outer(in_progress);
        ASSERT_TRUE(outer.entered());
        EXPECT_TRUE(in_progress);
        run_wait_with([&] {
            // A held Ctrl+V dispatched inside the wait.
            ReentryGuard const inner(in_progress);
            (inner.entered() ? nested_entered : nested_refused)++;
        });
        // The refused inner call must not have cleared the outer's flag.
        EXPECT_TRUE(in_progress);
    }
    EXPECT_EQ(nested_entered, 0);
    EXPECT_EQ(nested_refused, 1);
    EXPECT_FALSE(in_progress);
    // The next paste after the first finished is accepted again.
    ReentryGuard const next(in_progress);
    EXPECT_TRUE(next.entered());
}

// ST-P item 3 (K8): the 100 megapixel paste budget (same as the destructive clip).
TEST(TextPasteUnit, ClipboardImagePasteLimitRefusesHugeTextures)
{
    using Inkscape::UI::ClipboardLease::image_within_paste_limit;
    using Inkscape::UI::ClipboardLease::MAX_PASTE_IMAGE_PIXELS;
    EXPECT_EQ(MAX_PASTE_IMAGE_PIXELS, 100'000'000LL);
    EXPECT_TRUE(image_within_paste_limit(1920, 1080));
    EXPECT_TRUE(image_within_paste_limit(10000, 10000));      // exactly 100 MP
    EXPECT_FALSE(image_within_paste_limit(10000, 10001));     // one row above
    EXPECT_FALSE(image_within_paste_limit(16384, 16384));     // 268 MP
    EXPECT_FALSE(image_within_paste_limit(100000, 100000));   // would overflow a 32-bit product
    EXPECT_FALSE(image_within_paste_limit(0, 100));
    EXPECT_FALSE(image_within_paste_limit(-5, 100));
    // The scaled seam: a tiny budget accepts below and refuses just above.
    EXPECT_TRUE(image_within_paste_limit(10, 10, 100));
    EXPECT_FALSE(image_within_paste_limit(10, 11, 100));
}

TEST(TextPasteUnit, LatinKeyvalWithoutDisplayPreservesEventAndClearsConsumedModifiers)
{
    ASSERT_EQ(gdk_display_get_default(), nullptr) << "This test requires a headless process";
    unsigned consumed = 0xffffffff;
    EXPECT_EQ(Inkscape::UI::Tools::get_latin_keyval_impl(GDK_KEY_a, 0, GDK_CONTROL_MASK, 0, &consumed), GDK_KEY_a);
    EXPECT_EQ(consumed, 0u);
}
