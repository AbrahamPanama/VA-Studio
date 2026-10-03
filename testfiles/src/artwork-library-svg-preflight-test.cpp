// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/artwork-library-svg-preflight.h"
#include "io/artwork-library-package.h"
#include "object/filters/slot-resolver.h"
#include <gtest/gtest.h>
#include <libxml/parser.h>
#include <png.h>
#include <glib.h>
#include <zlib.h>
#include <algorithm>
#include <limits>
#include <type_traits>

using namespace Inkscape::IO::ArtworkLibrary;
namespace {
Bytes bytes(std::string const &s) { return {s.begin(), s.end()}; }
std::string svg(std::string const &body, std::string const &extra = "")
{
    return "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
           "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
           "width='25.4mm' height='25.4mm' viewBox='0 0 96 96' " + extra + ">" + body + "</svg>";
}
SvgPreflightExpectation expected(Bytes const &input)
{
    return {"b203e8e9-640c-4195-b0ea-f9b790245678", artwork_sha256(input), 25.4, 25.4};
}
ValidatedSvg admit(std::string const &s, SvgPreflightLimits l = {})
{
    auto input = bytes(s); return preflight_svg(input, expected(input), l);
}
void rejects(std::string const &s, SvgPreflightLimits l = {})
{
    EXPECT_THROW((void)admit(s, l), SvgPreflightError) << s.substr(0, 160);
}
void rejects_as(std::string const &s, SvgPreflightFailure reason, SvgPreflightLimits l = {})
{
    try { (void)admit(s, l); FAIL() << "Unexpected admission"; }
    catch (SvgPreflightError const &e) { EXPECT_EQ(e.failure(), reason) << e.what(); }
}
void put32(Bytes &out, std::uint32_t n)
{
    out.push_back(n >> 24); out.push_back(n >> 16); out.push_back(n >> 8); out.push_back(n);
}
Bytes png()
{
    png_image image {}; image.version = PNG_IMAGE_VERSION;
    image.width = 1; image.height = 1; image.format = PNG_FORMAT_RGBA;
    unsigned char rgba[] = {255, 0, 128, 127}; png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&image, nullptr, &size, 0, rgba, 0, nullptr)) throw std::runtime_error("PNG fixture size");
    Bytes result(size);
    if (!png_image_write_to_memory(&image, result.data(), &size, 0, rgba, 0, nullptr)) throw std::runtime_error("PNG fixture");
    result.resize(size); png_image_free(&image); return result;
}
void chunk_after_ihdr(Bytes &image, char const *type, Bytes const &payload)
{
    Bytes chunk; put32(chunk, payload.size());
    chunk.insert(chunk.end(), type, type + 4);
    chunk.insert(chunk.end(), payload.begin(), payload.end());
    put32(chunk, crc32(0, chunk.data() + 4, static_cast<uInt>(payload.size() + 4)));
    image.insert(image.begin() + 33, chunk.begin(), chunk.end());
}
std::string uri(Bytes const &b)
{
    auto value = g_base64_encode(b.data(), b.size());
    std::string result = "data:image/png;base64,"; result += value; g_free(value); return result;
}
std::string image_svg(Bytes const &b) { return svg("<image width='96' height='96' href='" + uri(b) + "'/>"); }
unsigned loads = 0;
xmlParserInputPtr external_loader(char const *, char const *, xmlParserCtxtPtr) { ++loads; return nullptr; }
struct LoaderGuard {
    xmlExternalEntityLoader prior = xmlGetExternalEntityLoader();
    LoaderGuard() { loads = 0; xmlSetExternalEntityLoader(external_loader); }
    ~LoaderGuard() { xmlSetExternalEntityLoader(prior); }
};
} // namespace

TEST(ArtworkLibrarySvgPreflight, NativeToneMetadataTupleIsPreservedWithTransferChildren)
{
    auto s = svg("<defs><filter id='f'><feComponentTransfer inkscape:bitmap-adjustment='tone-v1' "
                 "inkscape:brightness='20' inkscape:contrast='-10' inkscape:intensity='15' "
                 "inkscape:highlights='7' inkscape:shadows='-4' inkscape:midtones='11'>"
                 "<feFuncR type='table' tableValues='0 0.5 1'/><feFuncG type='identity'/>"
                 "<feFuncB type='identity'/><feFuncA type='identity'/></feComponentTransfer></filter></defs>"
                 "<image width='96' height='96' filter='url(#f)' href='" + uri(png()) + "'/>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
}
TEST(ArtworkLibrarySvgPreflight, ToneMetadataRejectsWrongOwnerIncompleteAndNonfiniteParameters)
{
    for (auto attributes : {"inkscape:bitmap-adjustment='tone-v2'", "inkscape:brightness='NaN'",
                            "inkscape:brightness='101'", "inkscape:brightness='1e99'",
                            "inkscape:brightness='0'", "inkscape:bitmap-adjustment='tone-v1'"})
        rejects(svg(std::string("<filter><feComponentTransfer ") + attributes + "/></filter>"));
    rejects(svg("<rect width='2' height='2' inkscape:bitmap-adjustment='tone-v1'/>"));
}
TEST(ArtworkLibrarySvgPreflight, NativeTextMetricsAndParagraphRolesRemainImmutable)
{
    auto s = svg("<text xmlns:s='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' x='4' y='20' "
                 "style='font-family:sans-serif;font-size:12px;line-height:150%;inline-size:80px;white-space:pre-wrap;"
                 "text-indent:1em;shape-padding:2px;shape-margin:0;-inkscape-language-spacing:110%;"
                 "-inkscape-paragraph-spacing-before:2px;-inkscape-paragraph-spacing-after:3px'>"
                 "<tspan s:role='paragraph' inkscape:list-style='number'><tspan inkscape:list-marker='true'>3. </tspan>"
                 "<tspan style='font-size:150%;letter-spacing:.2em;word-spacing:.5ex;baseline-shift:-10%'>Café</tspan></tspan></text>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
}
TEST(ArtworkLibrarySvgPreflight, RelativeFontCascadeAndTextMetricBudgetsCannotBeResetBySmallChildren)
{
    std::string body;
    for (unsigned i = 0; i < 12; ++i) body += "<g style='font-size:400%'>";
    body += "<text style='font-size:1px'>x</text>";
    for (unsigned i = 0; i < 12; ++i) body += "</g>";
    rejects_as(svg(body), SvgPreflightFailure::LimitExceeded);
    for (auto metric : {"font-size:1e-20px", "line-height:1700%", "inline-size:1e99px",
                        "baseline-shift:NaN", "word-spacing:100em", "shape-padding:-1px",
                        "font-size:12px junk"})
        rejects(svg(std::string("<text style='") + metric + "'>x</text>"));
}
TEST(ArtworkLibrarySvgPreflight, RelativeFontReferencesAlsoConsumeExpandedCascadeBudget)
{
    std::string body = "<defs><text id='t' style='font-size:400%'>x</text>";
    for (unsigned i = 0; i < 10; ++i)
        body += "<use id='u" + std::to_string(i) + "' href='#" + (i ? "u" + std::to_string(i - 1) : "t") + "' style='font-size:400%'/>";
    body += "</defs><use href='#u9'/>";
    rejects_as(svg(body), SvgPreflightFailure::LimitExceeded);
}
TEST(ArtworkLibrarySvgPreflight, FontUnitsAreDistinctFromScientificNotation)
{
    for (auto metric : {"1em", ".5ex", "1e-1em", "1E-1ex", "1e1px"}) {
        SCOPED_TRACE(metric);
        auto s = svg(std::string("<text style='font-size:12px;letter-spacing:") + metric + "'>abc</text>");
        EXPECT_EQ(*admit(s).svg_bytes(), s);
    }
    for (auto metric : {"1e", "1e+", "1e-em", "1e999em", "1emjunk", "1exjunk"}) {
        SCOPED_TRACE(metric);
        rejects(svg(std::string("<text style='letter-spacing:") + metric + "'>abc</text>"));
    }
    // Path/transform contexts must not gain permission to use font units.
    rejects(svg("<path d='M1em 0L2 2'/>"));
    rejects(svg("<rect width='1' height='1' transform='translate(1em)'/>"));
}
TEST(ArtworkLibrarySvgPreflight, GeneratedTextColumnsResolveAllTargetsAndBudgetEveryReference)
{
    auto s = svg("<defs><rect id='a' width='30' height='80' inkscape:text-frame-owner='t'/>"
                 "<rect id='b' x='40' width='30' height='80' inkscape:text-frame-owner='t'/></defs>"
                 "<text id='t' inkscape:text-frame-generated='true' inkscape:text-frame-width='70' "
                 "inkscape:text-frame-height='80' inkscape:text-frame-columns='2' inkscape:text-frame-gap='10' "
                 "inkscape:text-frame-align='top' style='font-size:10px;shape-inside:url(#a) url(#b);white-space:pre-wrap'>Columns</text>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
    SvgPreflightLimits l; l.references = 1;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
    rejects(svg("<defs><rect id='a'/></defs><text style='shape-inside:url(#a) url(#missing)'>x</text>"));
    rejects(svg("<defs><rect id='a'/></defs><text style='shape-inside:url(#a) url(file:///never)'>x</text>"));
    rejects(svg("<text inkscape:text-frame-columns='21'>x</text>"));
}
TEST(ArtworkLibrarySvgPreflight, ParagraphMarkersDoNotPermitArbitraryMetadataOrActiveContent)
{
    for (auto attr : {"inkscape:drop-cap-lines='1000000000'", "inkscape:hyphenation='file:///dict'",
                      "inkscape:list-style='bogus'", "inkscape:auto-hyphen='url(file:///x)'",
                      "inkscape:unknown-extension='true'"})
        rejects(svg(std::string("<text ") + attr + ">x</text>"));
    rejects(svg("<text xmlns:s='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'><tspan s:role='script'>x</tspan></text>"));
}
TEST(ArtworkLibrarySvgPreflight, RichTextAxesAndDecorationsPreservedWithoutFontLoading)
{
    auto automatic = svg("<text style='-inkscape-font-variant-caps-mode:'>Automatic</text>");
    EXPECT_EQ(*admit(automatic).svg_bytes(), automatic);
    auto s = svg("<text style=\"font-family:serif;font-size:12px;font-variation-settings:'wght' 650, 'wdth' 90;"
                 "font-feature-settings:'liga' 0, 'ss01' 1;font-variant-alternates:historical-forms;"
                 "text-orientation:mixed;-inkscape-font-variant-caps-mode:synthesized;"
                 "text-decoration-line:underline;text-decoration-fill:#345678;text-decoration-stroke:none\">Editable</text>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
    rejects(svg("<text style=\"font-variation-settings:url(file:///font)\">x</text>"));
}
TEST(ArtworkLibrarySvgPreflight, InheritedDecorationPaintCannotHideInvalidOverrides)
{
    auto defs = "<defs><linearGradient id='p'><stop offset='0' stop-color='red'/></linearGradient></defs>";
    rejects(svg(std::string(defs) + "<g style='text-decoration-fill:url(#p)'><text text-decoration-fill='bogus'>x</text></g>"));
    rejects(svg(std::string(defs) + "<g style='text-decoration-fill:url(#p)'><text text-decoration-fill='none '>x</text></g>"));
    SvgPreflightLimits l; l.references = 2;
    rejects_as(svg(std::string(defs) + "<g style='text-decoration-fill:url(#p)'><text>x</text><text>y</text></g>"),
               SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, LegacyFlowTextUsesTheSameGeometryAndTextBudgets)
{
    auto s = svg("<flowRoot style='font-family:serif;font-size:12px;line-height:150%'>"
                 "<flowRegion><rect width='90' height='90'/></flowRegion>"
                 "<flowRegionExclude><circle cx='80' cy='80' r='5'/></flowRegionExclude>"
                 "<flowDiv><flowPara>Invitation <flowSpan style='font-weight:bold'>editable</flowSpan>"
                 "<flowLine/>second line</flowPara></flowDiv></flowRoot>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
    SvgPreflightLimits l; l.geometry_commands = 10;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
    rejects(svg("<flowRegion><rect/></flowRegion>"));
    rejects(svg("<flowRoot><flowRegion><script/></flowRegion></flowRoot>"));
    rejects(svg("<flowRoot inkscape:layoutOptions='par-indent:1e999'><flowPara>x</flowPara></flowRoot>"));
}

TEST(ArtworkLibrarySvgPreflight, ImmutableResultHasNoPublicDefaultOrMutableBytes)
{
    static_assert(!std::is_default_constructible_v<ValidatedSvg>);
    static_assert(std::is_same_v<decltype(std::declval<ValidatedSvg>().svg_bytes()), std::shared_ptr<std::string const>>);
    auto original = svg("<path d='M0 0L96 0L96 96Z'/>"); auto input = bytes(original);
    auto e = expected(input);
    auto accepted = preflight_svg(input, e, {}, [&] {
        input.assign(4, '!'); e.sha256.clear(); e.width_mm = 0; return false;
    });
    EXPECT_EQ(*accepted.svg_bytes(), original);
    EXPECT_DOUBLE_EQ(accepted.width_mm(), 25.4);
    auto owner = accepted.svg_bytes();
    accepted = admit(svg("<rect width='12' height='12'/>"));
    EXPECT_EQ(*owner, original);
}
TEST(ArtworkLibrarySvgPreflight, ConfiguredDepthDoesNotScaleNeutralTextOrPatternWork)
{
    auto s = svg("<defs><pattern id='p' patternUnits='userSpaceOnUse' width='16' height='16'>"
                 "<rect width='8' height='8'/></pattern></defs>"
                 "<rect width='96' height='96' fill='url(#p)'/><text x='4' y='20'>Neutral text</text>");
    SvgPreflightLimits shallow; shallow.depth = 8;
    auto a = admit(s, shallow);
    auto b = admit(s);
    EXPECT_EQ(a.stats().expanded_nodes, b.stats().expanded_nodes);
    EXPECT_EQ(a.stats().geometry_commands, b.stats().geometry_commands);
    EXPECT_EQ(*a.svg_bytes(), s); EXPECT_EQ(*b.svg_bytes(), s);
}

TEST(ArtworkLibrarySvgPreflight, DeepNeutralXmlKeepsTextExtentButStillChecksActualDepth)
{
    std::string body;
    for (unsigned i = 0; i < 48; ++i) body += "<g>";
    body += "<text>x</text>";
    for (unsigned i = 0; i < 48; ++i) body += "</g>";
    SvgPreflightLimits l; l.coordinate_magnitude = 96;
    EXPECT_NO_THROW((void)admit(svg(body), l));
    l.depth = 32;
    rejects_as(svg(body), SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, ExplicitBaselineDeltasAccumulateAlongActualXmlAncestry)
{
    // Existing conservative glyph envelope: 1 byte * 16 * 1.25 * 2 = 40.
    // Three explicit shifts add 60, not 60 per glyph or 64 maximum levels.
    auto s = svg("<g baseline-shift='20px'><g baseline-shift='20px'><g baseline-shift='20px'>"
                 "<text baseline-shift='baseline'>x</text></g></g></g>");
    SvgPreflightLimits l; l.coordinate_magnitude = 100;
    EXPECT_NO_THROW((void)admit(s, l));
    l.coordinate_magnitude = 99;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, UnsetAndInheritedBaselineFollowNativeCopyThenAddRecurrence)
{
    // Native SPIBaselineShift::cascade: 5 -> 10 -> 20 -> 40 -> 80;
    // the single glyph contributes the existing 40-unit envelope.
    auto s = svg("<g baseline-shift='5px'><g><g baseline-shift='inherit'><g><text>x</text></g></g></g></g>");
    SvgPreflightLimits l; l.coordinate_magnitude = 120;
    EXPECT_NO_THROW((void)admit(s, l));
    l.coordinate_magnitude = 119;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, BaselineSiblingsDoNotAccumulateIntoOneAnother)
{
    auto s = svg("<g baseline-shift='20px'><text baseline-shift='baseline'>x</text></g>"
                 "<g baseline-shift='20px'><text baseline-shift='baseline'>y</text></g>"
                 "<g baseline-shift='20px'><text baseline-shift='baseline'>z</text></g>");
    SvgPreflightLimits l; l.coordinate_magnitude = 96;
    EXPECT_NO_THROW((void)admit(s, l));
}

TEST(ArtworkLibrarySvgPreflight, ReferencedTextRechecksEachBaselineContextInsteadOfMemoizingById)
{
    auto s = svg("<defs><text id='t'>x</text></defs><use href='#t'/>"
                 "<g baseline-shift='20px'><use href='#t'/></g>");
    SvgPreflightLimits l; l.coordinate_magnitude = 120;
    EXPECT_NO_THROW((void)admit(s, l));
    l.coordinate_magnitude = 119;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, RelativeBaselineKeepsParentFontBasisAndSignedEnvelope)
{
    for (auto value : {"400%", "-400%"}) {
        auto s = svg(std::string("<g baseline-shift='") + value + "'><text baseline-shift='baseline'>x</text></g>");
        SvgPreflightLimits l; l.coordinate_magnitude = 104;
        EXPECT_NO_THROW((void)admit(s, l)); // 16 * 4 + 40.
        l.coordinate_magnitude = 103;
        rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
    }
}

TEST(ArtworkLibrarySvgPreflight, TextBaselineEnvelopeStillPassesThroughAncestorTransforms)
{
    auto s = svg("<g transform='scale(2)' baseline-shift='20px'><text baseline-shift='baseline'>x</text></g>");
    SvgPreflightLimits l; l.coordinate_magnitude = 120;
    EXPECT_NO_THROW((void)admit(s, l)); // (20 + 40) * 2.
    l.coordinate_magnitude = 119;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, TextFreeBranchesDoNotInflateAnUnrelatedRun)
{
    std::string body = "<text>x</text><g baseline-shift='1px'>";
    for (unsigned i = 0; i < 48; ++i) body += "<g>";
    body += "<rect width='1' height='1'/>";
    for (unsigned i = 0; i < 48; ++i) body += "</g>";
    body += "</g>";
    SvgPreflightLimits l; l.coordinate_magnitude = 96;
    EXPECT_NO_THROW((void)admit(svg(body), l));
}

TEST(ArtworkLibrarySvgPreflight, DirectFlowRootWhitespaceAndCdataConsumeGeometryBudget)
{
    for (bool cdata : {false, true}) {
        auto whitespace = std::string(33, ' ');
        auto body = "<flowRoot xml:space='preserve'>" +
                    (cdata ? "<![CDATA[" + whitespace + "]]>" : whitespace) + "</flowRoot>";
        SvgPreflightLimits l; l.geometry_commands = 32;
        rejects_as(svg(body), SvgPreflightFailure::LimitExceeded, l);
        l.geometry_commands = 33;
        EXPECT_EQ(admit(svg(body), l).stats().geometry_commands, 33u);
    }
}
TEST(ArtworkLibrarySvgPreflight, OrdinaryContainersVisitedByNativeFlowAlsoChargeWhitespace)
{
    auto s = svg("<flowRoot xml:space='preserve'><g>" + std::string(33, ' ') + "</g></flowRoot>");
    SvgPreflightLimits l; l.geometry_commands = 32;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
    l.geometry_commands = 33;
    EXPECT_EQ(admit(s, l).stats().geometry_commands, 33u);
}
namespace {
std::string viewport_inline_svg(std::string const &viewbox, std::string const &body)
{
    return "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
           "width='25.4mm' height='25.4mm' viewBox='" + viewbox + "'>" + body + "</svg>";
}
}
TEST(ArtworkLibrarySvgPreflight, InlinePercentUsesViewportNotFontForBothOrientations)
{
    for (auto mode : {"lr-tb", "tb-rl"}) {
        auto text = std::string("<text x='0' y='12' style='font-size:12px;writing-mode:") + mode +
                    ";inline-size:1600%'>x</text>";
        rejects_as(viewport_inline_svg("0 0 1000000000 1000000000", text), SvgPreflightFailure::LimitExceeded);
        auto s = viewport_inline_svg("0 0 400 200",
            std::string("<text x='0' y='12' style='writing-mode:") + mode + ";inline-size:50%'>x</text>");
        EXPECT_EQ(*admit(s).svg_bytes(), s);
    }
}
TEST(ArtworkLibrarySvgPreflight, InlinePercentUsesNestedViewboxAndInheritedDeclaration)
{
    auto s = svg("<g style='inline-size:1600%'><svg width='40' height='40' viewBox='0 0 10000 10000'>"
                 "<text x='0' y='12' style='inline-size:inherit'>x</text></svg></g>");
    rejects_as(s, SvgPreflightFailure::LimitExceeded);
    auto safe = svg("<g style='inline-size:1600%'><svg width='40' height='40' viewBox='0 0 10000 10000'>"
                    "<text x='0' y='12' style='inline-size:12px'>x</text></svg></g>");
    EXPECT_EQ(*admit(safe).svg_bytes(), safe); // Local nonpercentage override wins.
}
TEST(ArtworkLibrarySvgPreflight, InlinePercentMetricCapAndTinyFontCannotMaskViewportExtent)
{
    auto make = [](unsigned extent) {
        return viewport_inline_svg("0 0 " + std::to_string(extent) + " 96",
                                   "<text x='0' y='1' style='font-size:0.001px;inline-size:100%'>x</text>");
    };
    EXPECT_NO_THROW((void)admit(make(65536)));
    rejects_as(make(65537), SvgPreflightFailure::LimitExceeded);
}
TEST(ArtworkLibrarySvgPreflight, ReferencedNestedSvgRechecksViewportAtEachUse)
{
    // The nested SVG has no viewBox: width/height percentages depend on the
    // consuming viewport, not the original defs context. The second use must
    // not reuse the first successful context's result.
    auto s = svg("<defs><g id='frame'><svg width='100%' height='100%'>"
                 "<text x='0' y='12' style='inline-size:100%'>x</text></svg></g></defs>"
                 "<use href='#frame'/><svg width='40' height='40' viewBox='0 0 80000 80000'>"
                 "<use href='#frame'/></svg>");
    rejects_as(s, SvgPreflightFailure::LimitExceeded);
}
TEST(ArtworkLibrarySvgPreflight, MalformedInlinePercentStillFailsAndNoBudgetIsRaised)
{
    for (auto value : {"-1%", "1601%", "NaN%", "50%junk", "50%%", "1e999%"})
        rejects(svg(std::string("<text style='inline-size:") + value + "'>x</text>"));
}

TEST(ArtworkLibrarySvgPreflight, ReferencedWhitespaceIsChargedInBothNativeTextContexts)
{
    for (auto root : {"text", "flowRoot"}) for (bool cdata : {false, true}) {
        SCOPED_TRACE(root);
        SCOPED_TRACE(cdata);
        auto spaces = std::string(33, ' ');
        auto data = cdata ? "<![CDATA[" + spaces + "]]>" : spaces;
        auto s = svg("<defs><g id='words' xml:space='preserve'>" + data + "</g></defs><" + root +
                     "><use href='#words'/></" + root + ">");
        SvgPreflightLimits l; l.geometry_commands = 32;
        rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
        l.geometry_commands = 65; // Definition + one clone each conservatively count.
        rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
        l.geometry_commands = 66;
        auto token = admit(s, l);
        EXPECT_EQ(token.stats().geometry_commands, 66u);
        EXPECT_EQ(*token.svg_bytes(), s);
    }
}

TEST(ArtworkLibrarySvgPreflight, OrdinaryTextContainersConsumeWhitespaceAndVisibleText)
{
    for (auto data : {std::string(33, ' '), std::string(33, 'x')}) {
        auto s = svg("<text xml:space='preserve'><g><g>" + data + "</g></g></text>");
        SvgPreflightLimits l; l.geometry_commands = 32;
        rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
        l.geometry_commands = 33;
        auto token = admit(s, l);
        EXPECT_EQ(token.stats().geometry_commands, 33u); EXPECT_EQ(*token.svg_bytes(), s);
    }
}

TEST(ArtworkLibrarySvgPreflight, LaterTextUseContextIsNotPrunedByEarlierNonTextVisit)
{
    auto s = svg("<defs><g id='words' xml:space='preserve'>" + std::string(33, ' ') + "</g></defs>"
                 "<use href='#words'/><flowRoot><use href='#words'/><use href='#words'/></flowRoot>");
    SvgPreflightLimits l; l.geometry_commands = 132; // Definition plus all three references.
    EXPECT_EQ(admit(s, l).stats().geometry_commands, 132u);
    --l.geometry_commands;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, ReferencedVisibleTextNeedsAConsumingContext)
{
    auto definition = std::string("<defs><g id='words'>Editable</g></defs>");
    rejects_as(svg(definition + "<use href='#words'/>"), SvgPreflightFailure::Unsupported);
    for (auto root : {"text", "flowRoot"}) {
        auto s = svg(definition + "<" + root + "><use href='#words'/></" + root + ">");
        auto token = admit(s);
        EXPECT_EQ(token.stats().geometry_commands, 16u); EXPECT_EQ(*token.svg_bytes(), s);
    }
}

TEST(ArtworkLibrarySvgPreflight, NonTextFormattingAndPaintReferencesDoNotBecomeTextContexts)
{
    auto spaces = std::string(100, ' ');
    auto s = svg("<defs><clipPath id='clip'><g xml:space='preserve'>" + spaces + "</g></clipPath></defs>"
                 "<metadata>" + spaces + "Metadata</metadata><g>" + spaces + "</g>"
                 "<rect width='10' height='10' clip-path='url(#clip)'/>");
    SvgPreflightLimits l; l.geometry_commands = 8;
    auto token = admit(s, l);
    EXPECT_EQ(token.stats().geometry_commands, 8u); EXPECT_EQ(*token.svg_bytes(), s);
    // Legacy region whitespace is not traversed as flow text; its rect is geometry.
    auto flow = svg("<flowRoot><flowRegion>" + spaces + "<rect width='10' height='10'/></flowRegion></flowRoot>");
    EXPECT_EQ(admit(flow, l).stats().geometry_commands, 8u);
}

TEST(ArtworkLibrarySvgPreflight, TextUseContextRetainsExpandedDepthAndWorkLimits)
{
    auto s = svg("<defs><g id='a' xml:space='preserve'> </g><g id='b'><use href='#a'/></g>"
                 "<g id='c'><use href='#b'/><use href='#b'/></g></defs>"
                 "<text><use href='#c'/></text>");
    auto token = admit(s);
    SvgPreflightLimits l; l.expanded_nodes = token.stats().expanded_nodes;
    EXPECT_NO_THROW((void)admit(s, l));
    --l.expanded_nodes;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
    l = {}; l.depth = 5;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, DeferredTextContextAccountingRemainsCancellable)
{
    auto s = svg("<defs><g id='words' xml:space='preserve'>   </g></defs>"
                 "<use href='#words'/><text><use href='#words'/></text><flowRoot><use href='#words'/></flowRoot>");
    auto input = bytes(s); auto expectation = expected(input);
    std::size_t total = 0;
    auto token = preflight_svg(input, expectation, {}, [&] { ++total; return false; });
    ASSERT_GT(total, 0u);
    for (std::size_t threshold = 1; threshold <= total; ++threshold) {
        SCOPED_TRACE(threshold);
        std::size_t polls = 0;
        try {
            (void)preflight_svg(input, expectation, {}, [&] { return ++polls >= threshold; });
            FAIL() << "Cancellation returned a token";
        } catch (SvgPreflightError const &e) {
            EXPECT_EQ(e.failure(), SvgPreflightFailure::Cancelled); EXPECT_EQ(polls, threshold);
        }
    }
    EXPECT_EQ(*token.svg_bytes(), s); EXPECT_EQ(*admit(s).svg_bytes(), s);
}

TEST(ArtworkLibrarySvgPreflight, DescendantContractionCannotShrinkNativeTextLayoutBudget)
{
    for (auto root : {"text", "flowRoot"}) {
        SCOPED_TRACE(root);
        auto s = svg(std::string("<") + root + " transform='scale(100)' baseline-shift='baseline'>"
                     "<g transform='scale(.01)' baseline-shift='1000px'>M</g></" + root + ">");
        SvgPreflightLimits l; l.coordinate_magnitude = 2000;
        rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
        // The existing one-glyph envelope is 40; native layout ignores the
        // child contraction: (1000 + 40) * root scale 100 = 104000.
        l.coordinate_magnitude = 104000;
        EXPECT_EQ(*admit(s, l).svg_bytes(), s);
        --l.coordinate_magnitude;
        rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
    }
}

TEST(ArtworkLibrarySvgPreflight, UseAndNestedTextKeepUntransformedLayoutEnvelope)
{
    for (bool clone : {false, true}) {
        SCOPED_TRACE(clone);
        auto definition = clone ? "<defs><g id='words' transform='scale(.01)' baseline-shift='1000px'>M</g></defs>" : "";
        auto content = clone ? "<use href='#words' transform='scale(.01)' baseline-shift='baseline'/>" :
            "<text transform='scale(.01)' baseline-shift='baseline'><g baseline-shift='1000px'>M</g></text>";
        auto s = svg(std::string(definition) + "<text transform='scale(100)' baseline-shift='baseline'>" + content + "</text>");
        SvgPreflightLimits l; l.coordinate_magnitude = 2000;
        rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
        l.coordinate_magnitude = 104000;
        EXPECT_EQ(*admit(s, l).svg_bytes(), s);
    }
}

TEST(ArtworkLibrarySvgPreflight, RealTextRootAndOuterGeometryTransformsStillApply)
{
    // The contraction belongs to the actual layout root here, not one of its
    // ordinary descendants. The outside group then expands the whole item.
    auto s = svg("<g transform='scale(100)' baseline-shift='baseline'>"
                 "<text transform='scale(.01)' baseline-shift='baseline'>"
                 "<g baseline-shift='1000px'>M</g></text></g>");
    SvgPreflightLimits l; l.coordinate_magnitude = 1040;
    EXPECT_EQ(*admit(s, l).svg_bytes(), s);
    --l.coordinate_magnitude;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, SeparateLayoutEnvelopeDoesNotSkipOrdinaryGeometryCost)
{
    auto s = svg("<text><g transform='scale(1000)'><rect width='10' height='10'/><tspan>M</tspan></g></text>");
    SvgPreflightLimits l; l.coordinate_magnitude = 2000;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l); // Generic transformed geometry still audited.
    l.coordinate_magnitude = 50000; l.geometry_commands = 8;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
    l.geometry_commands = 9;
    auto token = admit(s, l);
    EXPECT_EQ(token.stats().geometry_commands, 9u); EXPECT_EQ(*token.svg_bytes(), s);
}

TEST(ArtworkLibrarySvgPreflight, ResourceTextLayoutIsProtectedBeforePatternCost)
{
    auto s = svg("<defs><pattern id='p' patternUnits='userSpaceOnUse' width='16' height='16'>"
                 "<text transform='scale(100)' baseline-shift='baseline'>"
                 "<g transform='scale(.01)' baseline-shift='1000px'>M</g></text>"
                 "</pattern></defs><rect width='10' height='10' fill='url(#p)'/>");
    SvgPreflightLimits l; l.coordinate_magnitude = 2000;
    rejects_as(s, SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, NativeLayoutBoundPropagationRemainsCancellable)
{
    auto s = svg("<defs><g id='words' transform='scale(.01)' baseline-shift='10px'>M</g></defs>"
                 "<text transform='scale(2)' baseline-shift='baseline'><use href='#words'/></text>");
    auto input = bytes(s); auto expectation = expected(input);
    std::size_t total = 0;
    auto token = preflight_svg(input, expectation, {}, [&] { ++total; return false; });
    ASSERT_GT(total, 0u);
    for (std::size_t threshold = 1; threshold <= total; ++threshold) {
        SCOPED_TRACE(threshold);
        std::size_t polls = 0;
        try {
            (void)preflight_svg(input, expectation, {}, [&] { return ++polls >= threshold; });
            FAIL() << "Cancellation returned a token";
        } catch (SvgPreflightError const &e) {
            EXPECT_EQ(e.failure(), SvgPreflightFailure::Cancelled); EXPECT_EQ(polls, threshold);
        }
    }
    EXPECT_EQ(*token.svg_bytes(), s); EXPECT_EQ(*admit(s).svg_bytes(), s);
}

TEST(ArtworkLibrarySvgPreflight, RepresentativeEditableVectorResourcesPreservedExactly)
{
    auto source = svg(
        "<metadata xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#' "
        "xmlns:dc='http://purl.org/dc/elements/1.1/'><rdf:RDF><rdf:Description><dc:title>Tarjeta 🌸</dc:title>"
        "</rdf:Description></rdf:RDF></metadata>"
        "<defs><linearGradient id='linear'><stop offset='0' stop-color='#ff0080'/><stop offset='100%' stop-color='blue'/></linearGradient>"
        "<radialGradient id='radial' cx='50%' cy='50%' r='50%'><stop offset='0' stop-color='white'/></radialGradient>"
        "<clipPath id='clip'><rect width='96' height='96'/></clipPath>"
        "<mask id='mask' x='-10%' y='-10%' width='120%' height='120%'><rect width='96' height='96' fill='white'/></mask>"
        "<pattern id='tile' patternUnits='userSpaceOnUse' width='16' height='16'><rect width='8' height='8' fill='url(#radial)'/></pattern></defs>"
        "<g inkscape:label='Editable' inkscape:groupmode='group' inkscape:nesting-contour-version='1' transform='translate(1,2)'>"
        "<path inkscape:nesting-contour='true' d='M0 0 C10 0 10 20 20 20 Q30 30 40 20 A5 5 0 0 1 50 20 Z' "
        "style='fill:url(#linear);stroke:#008080;stroke-width:1;opacity:0.5;clip-path:url(#clip);mask:url(#mask)'/>"
        "<rect x='1' y='1' width='80' height='80' fill='url(#tile)'/>"
        "<text x='4' y='20' style='font-family:Arial;font-size:12px'>Editable &amp; native<tspan dx='1'> text</tspan></text></g>");
    auto a = admit(source);
    EXPECT_EQ(*a.svg_bytes(), source);
    EXPECT_GE(a.stats().references, 5u);
    EXPECT_FALSE(a.requested_font_families().empty());
    EXPECT_FALSE(a.warnings().empty());
}
TEST(ArtworkLibrarySvgPreflight, LocalUsesGradientInheritanceAndTextPath)
{
    auto s = svg("<defs><path id='p' d='M0 0L20 20'/><linearGradient id='a'><stop offset='0' stop-color='red'/></linearGradient>"
                 "<linearGradient id='b' href='#a'/></defs><use href='#p' transform='translate(1,1)'/>"
                 "<text style='font-size:10px'><textPath href='#p'>Words</textPath></text><rect width='10' height='10' fill='url(#b)'/>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
}
TEST(ArtworkLibrarySvgPreflight, AbsoluteUnitsAndLegitimateLetterbox)
{
    auto s = "<svg xmlns='http://www.w3.org/2000/svg' width='1in' height='72pt' viewBox='-2 -3 48 96' preserveAspectRatio='xMidYMid meet'><rect width='1' height='1'/></svg>";
    auto a = admit(s); EXPECT_DOUBLE_EQ(a.width_mm(), 25.4); EXPECT_DOUBLE_EQ(a.view_box().width, 48);
}
TEST(ArtworkLibrarySvgPreflight, PredefinedAndNumericEntitiesAreTextNotMarkup)
{
    auto s = svg("<text>&amp;&lt;&gt;&apos;&quot;&#65;&#x42;</text><!-- literal &lt;!DOCTYPE and url(file:///x) -->");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
}
TEST(ArtworkLibrarySvgPreflight, DoctypesAndEntitiesNeverInvokeExternalLoader)
{
    LoaderGuard guard;
    for (auto declaration : {"<!DOCTYPE svg SYSTEM 'file:///must-not-be-opened'>",
                            "<!DOCTYPE svg SYSTEM 'https://example.invalid/never'>",
                            "<!DOCTYPE svg [<!ENTITY x SYSTEM 'file:///never'>]>",
                            "<!DOCTYPE svg [<!ENTITY % x SYSTEM 'https://example.invalid/never'>%x;]>",
                            "<!DOCTYPE svg [<!ENTITY a 'AAAA'><!ENTITY b '&a;&a;&a;&a;'>]>"}) {
        rejects_as(std::string(declaration) + svg("<text>&x;</text>"), SvgPreflightFailure::ForbiddenContent);
    }
    EXPECT_EQ(loads, 0u);
}
TEST(ArtworkLibrarySvgPreflight, StylesheetsRejectedAtEveryNamespaceAwareLocation)
{
    rejects_as(svg("<style>rect{fill:red}</style>"), SvgPreflightFailure::ForbiddenContent);
    rejects_as(svg("<defs><s:style xmlns:s='http://www.w3.org/2000/svg'>*{fill:red}</s:style></defs>"), SvgPreflightFailure::ForbiddenContent);
    rejects_as("<?xml-stylesheet href='https://example.invalid/x'?>" + svg(""), SvgPreflightFailure::ForbiddenContent);
    rejects_as(svg("<metadata><style>*{display:none}</style></metadata>"), SvgPreflightFailure::ForbiddenContent);
}
TEST(ArtworkLibrarySvgPreflight, ActiveAndForeignContentRejected)
{
    for (auto body : {"<script/>", "<foreignObject/>", "<animate/>", "<set/>", "<animateMotion/>",
                      "<g onload='x()'/>", "<g ONCLICK='x()'/>", "<g xml:base='file:///tmp/'/>",
                      "<g><html xmlns='http://www.w3.org/1999/xhtml'/></g>"}) rejects(svg(body));
}
TEST(ArtworkLibrarySvgPreflight, EscapedExternalReferencesAndConflictingHref)
{
    for (auto body : {"<use href='&#x68;ttps://example.invalid/x'/>", "<use href='file:///x'/>", "<use href='//example.invalid/x'/>",
                      "<use href='data:image/svg+xml;base64,AAA='/>", "<use href='#a' xlink:href='#b'/>",
                      "<rect fill='url(file:///x)'/>", "<rect style='fill:u\\72l(https://example.invalid/x)'/>",
                      "<rect style='fill:url/**/(https://example.invalid/x)'/>"}) rejects(svg(body));
}
TEST(ArtworkLibrarySvgPreflight, MalformedXmlAndEncodingAreNeverRecovered)
{
    rejects(svg("<path>")); rejects(svg("<rect width='1' width='2'/>"));
    rejects("<svg/>"); rejects(svg("") + svg(""));
    auto nul = svg(""); nul.insert(8, 1, '\0'); rejects(nul);
    rejects("<?xml version='1.0' encoding='ISO-8859-1'?>" + svg(""));
    auto invalid = svg("<text>"); invalid.insert(20, 1, char(0xff)); rejects(invalid);
}
TEST(ArtworkLibrarySvgPreflight, MissingDuplicateAndWrongResourceTypes)
{
    rejects(svg("<use href='#absent'/>"));
    rejects(svg("<path id='x'/><rect id='x'/>"));
    rejects(svg("<rect id='shape' fill='url(#shape)'/>"));
    rejects(svg("<defs><linearGradient id='paint'/></defs><use href='#paint'/>"));
}
TEST(ArtworkLibrarySvgPreflight, CyclesAndCachedDepthAreBounded)
{
    rejects(svg("<g id='a'><use href='#a'/></g>"));
    rejects(svg("<defs><linearGradient id='a' href='#b'/><linearGradient id='b' href='#a'/></defs>"));
    std::string body = "<defs><g id='n0'><rect width='1' height='1'/></g>";
    for (int i = 1; i < 12; ++i) body += "<g id='n" + std::to_string(i) + "'><use href='#n" + std::to_string(i-1) + "'/></g>";
    body += "</defs>";
    // Short-to-long definition order primes caches shallowly before deeper reuse.
    SvgPreflightLimits l; l.depth = 8;
    rejects_as(svg(body), SvgPreflightFailure::LimitExceeded, l);
}
TEST(ArtworkLibrarySvgPreflight, RepeatedDagChargesExpandedGeometry)
{
    auto body = "<defs><path id='p' d='M0 0L1 1L2 2L3 3'/></defs>";
    std::string s = body;
    for (int i = 0; i < 20; ++i) s += "<use href='#p'/>";
    SvgPreflightLimits l; l.geometry_commands = 20;
    rejects_as(svg(s), SvgPreflightFailure::LimitExceeded, l);
}
TEST(ArtworkLibrarySvgPreflight, NodeDepthAttributeAndByteLimits)
{
    SvgPreflightLimits l; l.nodes = 2; rejects_as(svg("<g><rect/></g>"), SvgPreflightFailure::LimitExceeded, l);
    l = {}; l.depth = 2; rejects_as(svg("<g><g/></g>"), SvgPreflightFailure::LimitExceeded, l);
    l = {}; l.attributes_per_node = 2; rejects_as(svg(""), SvgPreflightFailure::LimitExceeded, l);
    l = {}; l.xml_bytes = 20; rejects_as(svg(""), SvgPreflightFailure::LimitExceeded, l);
    l = {}; l.nodes = 4; rejects_as(svg("<text><![CDATA[a]]><![CDATA[b]]><![CDATA[c]]><![CDATA[d]]></text>"), SvgPreflightFailure::LimitExceeded, l);
}
TEST(ArtworkLibrarySvgPreflight, CallerCannotRaisePolicyCeilings)
{
    SvgPreflightLimits l; l.depth = 65; rejects_as(svg(""), SvgPreflightFailure::LimitExceeded, l);
    l = {}; l.coordinate_magnitude = std::numeric_limits<double>::infinity(); rejects(svg(""), l);
}
TEST(ArtworkLibrarySvgPreflight, NumericAndGeometryOverflowRejected)
{
    for (auto body : {"<path d='M1e999 0'/>", "<path d='M0 0L1e-999 1'/>", "<path d='M0 0LNaN 1'/>",
                      "<rect width='-1' height='2'/>", "<circle r='Infinity'/>", "<g transform='scale(0)'/>",
                      "<g transform='scale(1000000)'><g transform='scale(1000000)'><path d='M1 1L2 2'/></g></g>",
                      "<polyline points='0,0,1'/>", "<path d='M0 0L?'/>",
                      "<text style='font-size:1000000000px'>many</text>",
                      "<text style='font-weight:1e999'>x</text>"}) rejects(svg(body));
}
TEST(ArtworkLibrarySvgPreflight, DimensionsMustMatchExactSvgSemantics)
{
    auto input = bytes(svg("")); auto e = expected(input); e.width_mm += 0.02;
    EXPECT_THROW((void)preflight_svg(input, e), SvgPreflightError);
    rejects("<svg xmlns='http://www.w3.org/2000/svg' width='25.4 mm' height='25.4mm' viewBox='0 0 96 96'/>");
    rejects("<svg xmlns='http://www.w3.org/2000/svg' width='100%' height='25.4mm' viewBox='0 0 96 96'/>");
    rejects("<svg xmlns='http://www.w3.org/2000/svg' width='25.4mm' height='25.4mm' viewBox='0 0 0 96'/>");
}
TEST(ArtworkLibrarySvgPreflight, PhysicalResultReportsParsedDimensionsNotToleranceRoundedManifest)
{
    auto input = bytes(svg("")); auto e = expected(input); e.width_mm += 0.005;
    auto a = preflight_svg(input, e); EXPECT_DOUBLE_EQ(a.width_mm(), 25.4);
}
TEST(ArtworkLibrarySvgPreflight, HashBindingCannotBeSkipped)
{
    auto input = bytes(svg("")); auto e = expected(input); e.sha256[0] = e.sha256[0] == '0' ? '1' : '0';
    EXPECT_THROW((void)preflight_svg(input, e), SvgPreflightError);
    e.sha256.clear(); EXPECT_THROW((void)preflight_svg(input, e), SvgPreflightError);
}
TEST(ArtworkLibrarySvgPreflight, CancellationBeforeDuringAndAfterParsing)
{
    auto input = bytes(svg("<text>" + std::string(20000, 'x') + "</text>")); auto e = expected(input);
    unsigned complete_polls = 0;
    (void)preflight_svg(input, e, {}, [&] { ++complete_polls; return false; });
    ASSERT_GT(complete_polls, 3u);
    // Determine the actual successful path's last admission poll, rather than
    // assuming a fixed poll number corresponds to post-parse validation.
    for (unsigned threshold : {1u, complete_polls / 2, complete_polls}) {
        unsigned polls = 0;
        try { (void)preflight_svg(input, e, {}, [&] { return ++polls >= threshold; }); FAIL(); }
        catch (SvgPreflightError const &error) { EXPECT_EQ(error.failure(), SvgPreflightFailure::Cancelled); }
    }
}
TEST(ArtworkLibrarySvgPreflight, PngAlphaAndDensityBytesPreserved)
{
    auto p = png(); Bytes density; put32(density, 3780); put32(density, 3780); density.push_back(1);
    chunk_after_ihdr(p, "pHYs", density);
    auto s = image_svg(p); auto a = admit(s);
    EXPECT_EQ(*a.svg_bytes(), s); EXPECT_EQ(a.stats().image_pixels, 1u);
}
TEST(ArtworkLibrarySvgPreflight, PngDimensionsAreCheckedBeforeFullDecode)
{
    auto p = png(); p[16] = 0x7f; p[17] = 0xff; p[18] = 0xff; p[19] = 0xff;
    auto crc = crc32(0, p.data() + 12, 17);
    p[29] = crc >> 24; p[30] = crc >> 16; p[31] = crc >> 8; p[32] = crc;
    rejects_as(image_svg(p), SvgPreflightFailure::LimitExceeded);
}
TEST(ArtworkLibrarySvgPreflight, PngCrcTruncationAndAnimationRejected)
{
    auto p = png(); auto bad = p; bad.back() ^= 1; rejects(image_svg(bad));
    p.resize(p.size()-2); rejects(image_svg(p));
    p = png(); Bytes control; put32(control, 2); put32(control, 0); chunk_after_ihdr(p, "acTL", control);
    rejects(image_svg(p));
}
TEST(ArtworkLibrarySvgPreflight, OversizedCompressedPngMetadataCannotBeIgnored)
{
    auto p = png(); Bytes raw(2u * 1024 * 1024, 'A');
    uLongf size = compressBound(raw.size()); Bytes compressed(size);
    ASSERT_EQ(compress2(compressed.data(), &size, raw.data(), raw.size(), Z_BEST_COMPRESSION), Z_OK);
    compressed.resize(size); Bytes data{'k', 0, 0}; data.insert(data.end(), compressed.begin(), compressed.end());
    chunk_after_ihdr(p, "zTXt", data); rejects(image_svg(p));
}
TEST(ArtworkLibrarySvgPreflight, RasterReferencesRemainExplicitlyBounded)
{
    auto p = png(); SvgPreflightLimits l; l.image_pixels = 0; rejects(image_svg(p), l);
    l = {}; l.total_image_pixels = 1;
    auto image = "<image width='1' height='1' href='" + uri(p) + "'/>";
    rejects(svg(image + image), l);
    rejects(svg("<image width='1' height='1' href='data:image/jpeg;base64,AAAA'/>"));
    rejects(svg("<image width='1' height='1' href='file:///x.png'/>"));
    rejects(svg("<image width='1' height='1' href='data:image/png;base64,===='/>"));
}
TEST(ArtworkLibrarySvgPreflight, UnsupportedResourcesAreRefusedNotFlattened)
{
    rejects(svg("<filter id='f'><feTurbulence baseFrequency='10'/></filter>"));
    rejects(svg("<pattern id='p' width='0.000000000001' height='1' patternUnits='userSpaceOnUse'/>"));
    rejects(svg("<pattern id='p' width='1' height='1' patternUnits='objectBoundingBox'/>"));
    rejects(svg("<path inkscape:path-effect='#live' d='M0 0L1 1'/>"));
    rejects(svg("<svg width='0' height='1'/>"));
    rejects(svg("<rect style='width:calc(100% - 1px)'/>"));
}
TEST(ArtworkLibrarySvgPreflight, BoundingBoxResourceRangesAreNotUnboundedMultipliers)
{
    rejects(svg("<defs><linearGradient id='g' x2='1000000'/></defs><rect width='1' height='1' fill='url(#g)'/>"));
    rejects(svg("<mask id='m' width='1000000'><rect width='1' height='1'/></mask>"));
    rejects(svg("<clipPath id='c' clipPathUnits='objectBoundingBox'><rect width='1000' height='1000'/></clipPath>"));
}
TEST(ArtworkLibrarySvgPreflight, PathSinkFailureAndMalformedNativePathAreTyped)
{
    SvgPreflightLimits l; l.geometry_commands = 2;
    // Numeric pre-scan permits these eight numbers; the sink exceeds two events
    // while _pushCurve is feeding its prior curve. No exception crosses that feed.
    rejects_as(svg("<path d='M0 0L1 1L2 2L3 3'/>"), SvgPreflightFailure::LimitExceeded, l);
    rejects_as(svg("<path d='M0 0L1'/>"), SvgPreflightFailure::Unsupported);
}
TEST(ArtworkLibrarySvgPreflight, GroupPaintIsChargedOnEveryDrawableDescendant)
{
    auto definitions = "<defs><linearGradient id='g'><stop offset='0'/></linearGradient></defs>";
    auto source = svg(std::string(definitions) + "<g fill='url(#g)'><rect/><rect/><rect/></g>");
    auto a = admit(source);
    EXPECT_EQ(a.stats().references, 4u);
    SvgPreflightLimits l; l.references = 3;
    rejects_as(source, SvgPreflightFailure::LimitExceeded, l);
}
TEST(ArtworkLibrarySvgPreflight, InlineInheritRestoresParentPaintAndSolidOverrideClearsIt)
{
    auto definitions = "<defs><linearGradient id='g'/></defs>";
    auto inherited = admit(svg(std::string(definitions) +
        "<g fill='url(#g)'><g fill='red' style='fill:inherit'><rect/></g></g>"));
    EXPECT_EQ(inherited.stats().references, 2u);
    auto solid = admit(svg(std::string(definitions) +
        "<g fill='url(#g)'><g style='fill:blue'><rect/></g></g>"));
    EXPECT_EQ(solid.stats().references, 1u);
}
TEST(ArtworkLibrarySvgPreflight, ResourceBearingUseAndContextPaintAreExplicitlyUnsupported)
{
    auto definitions = "<defs><linearGradient id='g'/><path id='p' d='M0 0L1 1'/></defs>";
    rejects_as(svg(std::string(definitions) + "<g fill='url(#g)'><use href='#p'/></g>"), SvgPreflightFailure::Unsupported);
    rejects_as(svg("<path fill='context-fill'/>"), SvgPreflightFailure::Unsupported);
    rejects_as(svg("<path fill='Context-Fill'/>"), SvgPreflightFailure::Unsupported);
    rejects_as(svg("<path fill='INHERIT'/>"), SvgPreflightFailure::Unsupported);
    rejects_as(svg("<defs><linearGradient id='g'/></defs><path fill='url(#g) context-stroke'/>"), SvgPreflightFailure::Unsupported);
}
TEST(ArtworkLibrarySvgPreflight, DuplicateImportantCascadeCannotHideInheritedResourceWork)
{
    rejects_as(svg("<defs><linearGradient id='g'/></defs><g style='fill:url(#g)!important;fill:red'><rect/></g>"),
               SvgPreflightFailure::Unsupported);
}
TEST(ArtworkLibrarySvgPreflight, SymmetricNodeMetadataAndUtf8BomArePreserved)
{
    auto source = std::string("\xef\xbb\xbf") + svg(
        "<path xmlns:s='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' s:nodetypes='csaz' d='M0 0L1 1L2 2L3 3'/>"
        "<text>Árbol 日本語</text>");
    EXPECT_EQ(*admit(source).svg_bytes(), source);
}
TEST(ArtworkLibrarySvgPreflight, PatternRepetitionConsumesExpandedWorkBudget)
{
    auto source = svg("<defs><pattern id='p' patternUnits='userSpaceOnUse' width='1' height='1'>"
                      "<rect width='1' height='1'/></pattern></defs><rect width='96' height='96' fill='url(#p)'/>");
    SvgPreflightLimits l; l.expanded_nodes = 1000;
    rejects_as(source, SvgPreflightFailure::LimitExceeded, l);
}
TEST(ArtworkLibrarySvgPreflight, InvalidPaintNeverSuppressesInheritedResources)
{
    for (auto value : {"bogus", "initial", "unset", "currentcolor", "None", "none ", "currentColor ", "inherit ",
                       "rgb(1,2)", "rgb(1,,2,3)", "red junk"}) {
        SCOPED_TRACE(value);
        auto body = std::string("<defs><pattern id='p' patternUnits='userSpaceOnUse' width='24' height='24'><rect width='12' height='12'/></pattern></defs>") +
                    "<g fill='url(#p)'><g fill='" + value + "'><rect/><rect/><rect/></g></g>";
        rejects_as(svg(body), SvgPreflightFailure::Unsupported);
        rejects_as(svg("<rect stroke='" + std::string(value) + "'/>"), SvgPreflightFailure::Unsupported);
    }
}
TEST(ArtworkLibrarySvgPreflight, RealNativeColorsAndExactPaintControlsRemainUsable)
{
    for (auto value : {"red", "AliceBlue", "#123", "#11223344", "rgb(10,20,30)", "rgba(10,20,30,0.5)",
                       "hsl(120,50%,50%)", "none", "currentColor", "inherit"}) {
        SCOPED_TRACE(value);
        auto s = svg("<rect width='20' height='10' fill='" + std::string(value) + "'/>");
        EXPECT_EQ(*admit(s).svg_bytes(), s);
    }
    auto s = svg("<defs><linearGradient id='g'/></defs><rect fill='url(#g) red'/>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
    rejects_as(svg("<defs><linearGradient id='g'/></defs><rect fill='url(#g) bogus'/>"), SvgPreflightFailure::Unsupported);
}
TEST(ArtworkLibrarySvgPreflight, NestedViewportsAndInertNativeScaffoldingArePreserved)
{
    auto s = svg("<s:namedview xmlns:s='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' pagecolor='#ffffff' "
                 "bordercolor='' borderopacity='' inkscape:showpageshadow='2' inkscape:pageopacity='0.0' "
                 "inkscape:pagecheckerboard='0' inkscape:deskcolor='#d1d1d1' inkscape:document-units='px'/>"
                 "<svg width='240' height='120' viewBox='-10 -5 120 60' overflow='visible'>"
                 "<svg x='10%' y='20%' width='50%' height='50%' viewBox='0 0 12 6' preserveAspectRatio='xMaxYMin slice'>"
                 "<rect width='10' height='5' fill='red'/></svg></svg>");
    auto a = admit(s); EXPECT_EQ(*a.svg_bytes(), s); EXPECT_EQ(a.policy_version(), 3u);
    EXPECT_DOUBLE_EQ(a.width_mm(), 25.4); // Nested dimensions don't replace the manifest viewport.
}
TEST(ArtworkLibrarySvgPreflight, NestedViewportMappingHasFiniteAggregateBounds)
{
    rejects(svg("<svg width='0' height='10'><rect/></svg>"));
    rejects(svg("<svg width='100' height='100' viewBox='0 0 1e-20 1'><rect/></svg>"));
    rejects(svg("<svg width='1000000' height='1000000' viewBox='0 0 1 1'>"
                "<svg width='1000000' height='1000000' viewBox='0 0 1 1'><rect width='1000' height='1000'/></svg></svg>"));
    rejects(svg("<svg width='10' height='10' preserveAspectRatio='bogus'><rect/></svg>"));
    rejects(svg("<svg width='10' height='10' xml:base='https://example.invalid/'><rect/></svg>"));
}
TEST(ArtworkLibrarySvgPreflight, NamedviewIsNotAnArbitraryExtensionOrResourceBypass)
{
    for (auto body : {
        "<s:namedview xmlns:s='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' href='file:///x'/>",
        "<s:namedview xmlns:s='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'><image href='file:///x'/></s:namedview>",
        "<g><s:namedview xmlns:s='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'/></g>",
        "<s:namedview xmlns:s='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' pagecolor='url(#p)'/>"}) rejects(svg(body));
}
TEST(ArtworkLibrarySvgPreflight, GradientUnitsCoordinatesAndTransformResolveThroughHref)
{
    auto s = svg("<defs><linearGradient id='base' gradientUnits='userSpaceOnUse' x1='10' y1='20' x2='160' y2='80' "
                 "gradientTransform='translate(4,5)'><stop offset='0' stop-color='red'/></linearGradient>"
                 "<linearGradient id='mid' href='#base'/><linearGradient id='g' href='#mid' x2='180'/></defs>"
                 "<rect width='90' height='90' fill='url(#g)'/>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
    auto radial = svg("<defs><radialGradient id='base' gradientUnits='userSpaceOnUse' cx='40' cy='40' r='30'/>"
                      "<radialGradient id='r' href='#base' fx='35'/></defs><circle cx='40' cy='40' r='20' fill='url(#r)'/>");
    EXPECT_EQ(*admit(radial).svg_bytes(), radial);
}
TEST(ArtworkLibrarySvgPreflight, GradientLocalOverridesBeatPrototypeWithoutChargingStaleCoordinatesAsBbox)
{
    auto s = svg("<defs><linearGradient id='base' gradientUnits='userSpaceOnUse' x1='10' y1='20' x2='160' y2='80'/>"
                 "<linearGradient id='g' href='#base' gradientUnits='objectBoundingBox' x1='0' y1='0' x2='1' y2='1'/></defs>"
                 "<rect width='90' height='90' fill='url(#g)'/>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
    rejects(svg("<defs><linearGradient id='base' gradientUnits='userSpaceOnUse' x2='1000000'/>"
                "<linearGradient id='g' href='#base' gradientUnits='objectBoundingBox'/></defs><rect fill='url(#g)'/>"));
}
TEST(ArtworkLibrarySvgPreflight, BoundedFilterChainPreservesEveryNativePrimitive)
{
    auto s = svg("<defs><filter id='f' x='-20%' y='-20%' width='140%' height='140%' filterRes='128 128'>"
                 "<feGaussianBlur in='SourceGraphic' stdDeviation='1 2' result='blur'/>"
                 "<feOffset in='blur' dx='2' dy='3' result='shift'/>"
                 "<feFlood flood-color='#123456' flood-opacity='0.5' result='ink'/>"
                 "<feComposite in='ink' in2='shift' operator='in' result='shadow'/>"
                 "<feMerge><feMergeNode in='shadow'/><feMergeNode in='SourceGraphic'/></feMerge></filter></defs>"
                 "<rect width='20' height='10' filter='url(#f)'/>");
    auto a = admit(s); EXPECT_EQ(*a.svg_bytes(), s); EXPECT_EQ(a.stats().filter_primitives, 7u);
    EXPECT_FALSE(a.warnings().empty());
}
TEST(ArtworkLibrarySvgPreflight, FilterColorMatrixTransferBlendAndMorphologyRemainEditable)
{
    auto s = svg("<defs><filter id='f'><feColorMatrix type='matrix' values='1 0 0 0 0 0 1 0 0 0 0 0 1 0 0 0 0 0 1 0'/>"
                 "<feComponentTransfer><feFuncR type='linear' slope='0.5' intercept='0.2'/><feFuncA type='table' tableValues='0 1'/></feComponentTransfer>"
                 "<feMorphology operator='dilate' radius='1' result='m'/><feBlend in='SourceGraphic' in2='m' mode='multiply'/></filter></defs>"
                 "<rect width='20' height='10' filter='url(#f)'/>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
}
TEST(ArtworkLibrarySvgPreflight, FiltersRejectUnknownExternalCyclicAndExcessiveWorkInputs)
{
    for (auto body : {
        "<filter id='f'><feImage href='file:///x'/></filter>",
        "<filter id='f'><feGaussianBlur in='later'/><feOffset result='later'/></filter>",
        "<filter id='f'><feBlend in='BackgroundImage'/></filter>",
        "<filter id='f'><feGaussianBlur stdDeviation='1e999'/></filter>",
        "<filter id='f' filterRes='1000000000'><feGaussianBlur/></filter>",
        "<filter id='f' primitiveUnits='objectBoundingBox'><feGaussianBlur stdDeviation='100'/></filter>",
        "<filter id='f'><feColorMatrix values='1 2 3'/></filter>",
        "<filter id='f'><feOffset result='x'/><feOffset result='x'/></filter>",
        "<filter id='f'><feComponentTransfer><feFuncR/></feComponentTransfer></filter>",
        "<filter id='f'><feGaussianBlur><rect/></feGaussianBlur></filter>",
        "<feGaussianBlur stdDeviation='1'/>",
        "<filter id='f' style='filter:url(#f)'><feGaussianBlur/></filter>"}) rejects(svg(body));
    SvgPreflightLimits l; l.filter_primitives = 1;
    rejects_as(svg("<filter id='f'><feGaussianBlur/><feOffset/></filter>"), SvgPreflightFailure::LimitExceeded, l);
}

TEST(ArtworkLibrarySvgPreflight, ReservedFilterResultsCannotAliasNativeContextInputs)
{
    for (auto name : {"BackgroundImage", "BackgroundAlpha", "FillPaint", "StrokePaint", "SourceGraphic", "SourceAlpha"}) {
        SCOPED_TRACE(name);
        SlotResolver native;
        auto written = native.write(std::string(name));
        EXPECT_GT(written, 0);
        EXPECT_NE(native.read(std::string(name)), written); // Native built-ins take precedence.
        auto prefix = std::string("<defs><filter id='f'><feFlood result='") + name + "'/>";
        for (auto consumer : {std::string("<feOffset in='") + name + "'/>",
                              std::string("<feBlend in='SourceGraphic' in2='") + name + "'/>",
                              std::string("<feMerge><feMergeNode in='") + name + "'/></feMerge>"}) {
            rejects_as(svg(prefix + consumer + "</filter></defs><rect width='10' height='10' filter='url(#f)'/>"),
                       SvgPreflightFailure::Unsupported);
        }
    }
    auto ordinary = svg("<defs><filter id='f'><feFlood result='previous'/><feOffset in='previous'/>"
                        "</filter></defs><rect width='10' height='10' filter='url(#f)'/>");
    EXPECT_EQ(*admit(ordinary).svg_bytes(), ordinary);
}

TEST(ArtworkLibrarySvgPreflight, PreservedWhitespaceConsumesTextAndExpandedGeometryBudgets)
{
    SvgPreflightLimits small; small.geometry_commands = 1;
    for (auto content : {std::string(10000, ' '), std::string("\t\n\t\n"),
                         std::string("<![CDATA[    ]]>")}) {
        rejects_as(svg("<text xml:space='preserve' style='white-space:pre;font-size:12px'>" + content + "</text>"),
                   SvgPreflightFailure::LimitExceeded, small);
    }
    rejects_as(svg("<text xml:space='preserve'>" + std::string(1000001, ' ') + "</text>"),
               SvgPreflightFailure::LimitExceeded);
    small.geometry_commands = 4;
    rejects_as(svg("<defs><text id='spaces' xml:space='preserve'>   </text></defs>"
                   "<use href='#spaces'/><use href='#spaces'/>"), SvgPreflightFailure::LimitExceeded, small);
    auto indentation = svg("\n  <g>\n    </g>\n");
    EXPECT_EQ(*admit(indentation, small).svg_bytes(), indentation);
    // The same accounting must survive admission of native legacy flow text.
    for (auto tag : {"flowPara", "flowSpan", "flowDiv"}) {
        rejects_as(svg(std::string("<flowRoot><") + tag + " xml:space='preserve'>"
                       + std::string(10000, ' ') + "</" + tag + "></flowRoot>"),
                   SvgPreflightFailure::LimitExceeded, small);
    }
}

TEST(ArtworkLibrarySvgPreflight, InheritedPatternTransformUsesLocalTileSizeAndExplicitOverrides)
{
    auto input = [](std::string const &local, bool two_hops) {
        // Four-unit local tiles keep the two-hop identity control inside the
        // independent expanded graph budget. Inheriting 1/1024 still requires
        // 2560 repeats/axis over the ten-unit target, well above the 512 cap.
        auto tile = two_hops ? "4" : "1";
        return std::string("<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' viewBox='0 0 10 10'>"
            "<defs><pattern id='base' patternUnits='userSpaceOnUse' width='10240' height='10240'"
            " patternTransform='scale(0.0009765625)'><rect width='1' height='1'/></pattern>") +
            (two_hops ? "<pattern id='mid' href='#base' patternUnits='userSpaceOnUse' width='10240' height='10240'/>" : "") +
            "<pattern id='p' href='#" + (two_hops ? "mid" : "base") +
            "' patternUnits='userSpaceOnUse' width='" + tile + "' height='" + tile + "' " + local +
            "/></defs><rect width='10' height='10' fill='url(#p)'/></svg>";
    };
    auto check = [](std::string const &s) {
        auto b = bytes(s);
        auto e = expected(b); e.width_mm = e.height_mm = 10 * 25.4 / 96;
        return preflight_svg(b, e);
    };
    for (bool two_hops : {false, true}) {
        SCOPED_TRACE(two_hops ? "two href hops" : "one href hop");
        try { (void)check(input("", two_hops)); FAIL() << "Inherited tiny tile was admitted"; }
        catch (SvgPreflightError const &e) { EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded) << e.what(); }
        auto identity = input("patternTransform='matrix(1 0 0 1 0 0)'", two_hops);
        EXPECT_EQ(*check(identity).svg_bytes(), identity);
    }
}


namespace {
std::string pattern_review_svg(std::string const &body)
{
    return "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
           "width='10' height='10' viewBox='0 0 10 10'>" + body + "</svg>";
}
ValidatedSvg admit_pattern_review(std::string const &s, SvgPreflightLimits limits = {}, Cancelled cancel = {})
{
    auto input = bytes(s); auto e = expected(input);
    e.width_mm = e.height_mm = 10 * 25.4 / 96;
    return preflight_svg(input, e, limits, cancel);
}
std::string pattern_review_chain(unsigned count, bool reverse, bool local_transform)
{
    std::vector<std::string> definitions;
    for (unsigned i = 0; i < count; ++i) {
        auto p = "<pattern id='p" + std::to_string(i) +
            "' patternUnits='userSpaceOnUse' width='16' height='16'";
        if (local_transform) p += " patternTransform='matrix(1 0 0 1 0 0)'";
        if (i) p += " xlink:href='#p" + std::to_string(i - 1) + "'/>";
        else p += "><rect width='1' height='1'/></pattern>";
        definitions.push_back(p);
    }
    if (reverse) std::reverse(definitions.begin(), definitions.end());
    std::string body = "<defs>";
    for (auto const &p : definitions) body += p;
    return pattern_review_svg(body + "</defs><rect width='10' height='10' fill='url(#p" +
                              std::to_string(count - 1) + ")'/>");
}
} // namespace

TEST(ArtworkLibrarySvgPreflight, ExpandingPatternAliasDoesNotCompoundPrototypeGain)
{
    auto s = pattern_review_svg(
        "<defs><pattern id='base' patternUnits='userSpaceOnUse' width='1' height='1' "
        "patternTransform='scale(16)'><rect width='1' height='1'/></pattern>"
        "<pattern id='p' xlink:href='#base' patternUnits='userSpaceOnUse' width='1' height='1'/>"
        "</defs><rect width='10' height='10' fill='url(#p)'/>");
    auto admitted = admit_pattern_review(s);
    EXPECT_EQ(*admitted.svg_bytes(), s);
    // Both servers still pay their complete conservative graph/tile work.
    EXPECT_EQ(admitted.stats().expanded_nodes, 1091u);
    EXPECT_EQ(admitted.stats().geometry_commands, 4232u);
    SvgPreflightLimits small; small.expanded_nodes = 1000;
    try { (void)admit_pattern_review(s, small); FAIL() << "Prototype work was omitted"; }
    catch (SvgPreflightError const &e) { EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded); }
}

TEST(ArtworkLibrarySvgPreflight, PatternAliasBoundsKeepOwnContentAndRebaseInheritedContent)
{
    auto check_coordinate_rejection = [](std::string const &body) {
        SvgPreflightLimits small; small.coordinate_magnitude = 1000;
        try { (void)admit_pattern_review(pattern_review_svg(body), small); FAIL() << "Lost transformed content"; }
        catch (SvgPreflightError const &e) {
            EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded);
            // Catch this in the first geometry pass, not a later tiling/work
            // limit which could mask a missing inherited-content envelope.
            EXPECT_NE(std::string(e.what()).find("Coordinate budget"), std::string::npos);
        }
    };
    // Own alias children must still receive inherited scale(16): 100 -> 1600.
    check_coordinate_rejection(
        "<defs><pattern id='base' patternUnits='userSpaceOnUse' width='16' height='16' "
        "patternTransform='scale(16)'><rect width='1' height='1'/></pattern>"
        "<pattern id='p' xlink:href='#base' patternUnits='userSpaceOnUse' width='16' height='16'>"
        "<rect width='100' height='1'/></pattern></defs><rect width='10' height='10' fill='url(#p)'/>");
    // Local scale overrides the prototype's contraction. Inherit raw content
    // (100), not its contracted bound (6.25): 100 * 16 exceeds 1000.
    check_coordinate_rejection(
        "<defs><pattern id='base' patternUnits='userSpaceOnUse' width='16' height='16' "
        "patternTransform='scale(0.0625)'><rect width='100' height='1'/></pattern>"
        "<pattern id='p' xlink:href='#base' patternUnits='userSpaceOnUse' width='16' height='16' "
        "patternTransform='scale(16)'/></defs><rect width='10' height='10' fill='url(#p)'/>");
    // Origin inheritance is independent of prototype content/local tile sizes.
    for (auto axis : {"x", "y"}) {
        SCOPED_TRACE(axis);
        check_coordinate_rejection(std::string(
            "<defs><pattern id='base' patternUnits='userSpaceOnUse' width='16' height='16' ") +
            axis + "='100' patternTransform='scale(0.0625)'><rect width='1' height='1'/></pattern>"
            "<pattern id='p' xlink:href='#base' patternUnits='userSpaceOnUse' width='16' height='16' "
            "patternTransform='scale(16)'/></defs><rect width='10' height='10' fill='url(#p)'/>");
    }
}

TEST(ArtworkLibrarySvgPreflight, PatternPrototypeAndPaintEdgesRemainDistinct)
{
    // A paint edge is still a resource dependency, even when href targets the
    // same server. No local children introduce another copy of this paint edge.
    auto s = pattern_review_svg(
        "<defs><pattern id='base' patternUnits='userSpaceOnUse' width='16' height='16' "
        "patternTransform='scale(16)'><rect width='1' height='1'/></pattern>"
        "<pattern id='p' xlink:href='#base' fill='url(#base)' patternUnits='userSpaceOnUse' "
        "width='16' height='16' patternTransform='scale(16)'/>"
        "</defs><rect width='10' height='10' fill='url(#p)'/>");
    SvgPreflightLimits small; small.coordinate_magnitude = 1000;
    try { (void)admit_pattern_review(s, small); FAIL() << "Paint edge was treated as prototype"; }
    catch (SvgPreflightError const &e) {
        EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded);
        EXPECT_NE(std::string(e.what()).find("Coordinate budget"), std::string::npos);
    }
}

TEST(ArtworkLibrarySvgPreflight, PatternCyclesFailWithAndWithoutExplicitTransforms)
{
    for (bool local : {false, true}) for (bool two : {false, true}) {
        SCOPED_TRACE(local);
        SCOPED_TRACE(two);
        // Local origins let the explicit-transform cycle reach the graph
        // detector; the transform-free version still hits the walk depth cap.
        auto attr = std::string(" x='0' y='0'") + (local ? " patternTransform='scale(1)'" : "");
        auto body = "<defs><pattern id='a' patternUnits='userSpaceOnUse' width='16' height='16'" +
            attr + " xlink:href='#" + (two ? "b" : "a") + "'/>";
        if (two) body += "<pattern id='b' patternUnits='userSpaceOnUse' width='16' height='16'" +
                         attr + " xlink:href='#a'/>";
        body += "</defs>";
        try { (void)admit_pattern_review(pattern_review_svg(body)); FAIL() << "Pattern cycle admitted"; }
        catch (SvgPreflightError const &e) {
            EXPECT_EQ(e.failure(), local ? SvgPreflightFailure::ForbiddenContent : SvgPreflightFailure::LimitExceeded);
        }
    }
}

TEST(ArtworkLibrarySvgPreflight, PatternDepthChecksCachedAndUncachedChainsAtTheBoundary)
{
    for (bool reverse : {false, true}) for (bool local : {false, true}) {
        SCOPED_TRACE(reverse);
        SCOPED_TRACE(local);
        auto s = pattern_review_chain(3, reverse, local);
        SvgPreflightLimits limit; limit.depth = 6; // root/defs/p2/p1/p0/rect
        EXPECT_EQ(*admit_pattern_review(s, limit).svg_bytes(), s);
        limit.depth = 5;
        try { (void)admit_pattern_review(s, limit); FAIL() << "Expanded pattern depth bypass"; }
        catch (SvgPreflightError const &e) { EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded); }
    }
    // Longer than the hard inheritance walk, despite shallow XML nesting.
    for (bool reverse : {false, true}) {
        try { (void)admit_pattern_review(pattern_review_chain(65, reverse, false)); FAIL() << "Unbounded pattern walk"; }
        catch (SvgPreflightError const &e) {
            EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded);
            EXPECT_NE(std::string(e.what()).find("Pattern inheritance depth"), std::string::npos);
        }
    }
}

TEST(ArtworkLibrarySvgPreflight, CancellationAtEveryPatternAdmissionPollReturnsNoToken)
{
    auto s = pattern_review_chain(3, true, false);
    std::size_t total = 0;
    auto admitted = admit_pattern_review(s, {}, [&] { ++total; return false; });
    ASSERT_GT(total, 0u);
    // Cover every actual callback boundary, including inheritance and all cost
    // passes, without a parser-dependent ordinal or production instrumentation.
    for (std::size_t threshold = 1; threshold <= total; ++threshold) {
        SCOPED_TRACE(threshold);
        std::size_t polls = 0;
        try {
            (void)admit_pattern_review(s, {}, [&] { return ++polls >= threshold; });
            FAIL() << "Cancellation returned a token";
        } catch (SvgPreflightError const &e) {
            EXPECT_EQ(e.failure(), SvgPreflightFailure::Cancelled);
            EXPECT_EQ(polls, threshold);
        }
    }
    EXPECT_EQ(*admit_pattern_review(s).svg_bytes(), s);
    EXPECT_EQ(*admitted.svg_bytes(), s); // Existing independent token is unchanged.
}


TEST(ArtworkLibrarySvgPreflight, PatternPaintDoesNotInheritConsumerBaseline)
{
    auto make = [](char const *shift) {
        return pattern_review_svg(std::string(
            "<defs><pattern id='p' patternUnits='userSpaceOnUse' width='16' height='16'>"
            "<text baseline-shift='baseline'>x</text></pattern></defs>"
            "<rect width='10' height='10' fill='url(#p)' baseline-shift='") + shift + "'/>");
    };
    SvgPreflightLimits l; l.coordinate_magnitude = 100;
    auto neutral = admit_pattern_review(make("0px"), l);
    auto shifted = admit_pattern_review(make("100px"), l);
    EXPECT_EQ(*shifted.svg_bytes(), make("100px"));
    EXPECT_EQ(neutral.stats().expanded_nodes, shifted.stats().expanded_nodes);
    EXPECT_EQ(neutral.stats().geometry_commands, shifted.stats().geometry_commands);
    // Resource work is still charged through BOTH its XML definition and paint
    // reference. Raw input has one text byte plus eight rectangle commands.
    l.geometry_commands = 9;
    try { (void)admit_pattern_review(make("100px"), l); FAIL() << "Paint work was skipped"; }
    catch (SvgPreflightError const &e) { EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded); }
}

TEST(ArtworkLibrarySvgPreflight, ResourceTextKeepsItsActualXmlBaselineAncestry)
{
    auto s = pattern_review_svg(
        "<g baseline-shift='20px'><defs>"
        "<pattern id='p' patternUnits='userSpaceOnUse' width='16' height='16'>"
        "<text baseline-shift='baseline'>x</text></pattern></defs></g>"
        "<rect width='10' height='10' fill='url(#p)'/>");
    // Actual ancestry: g=20, defs=40, pattern=80, explicit text baseline=80.
    // One glyph's existing envelope is 40, so the coordinate boundary is 120.
    SvgPreflightLimits l; l.coordinate_magnitude = 120;
    EXPECT_EQ(*admit_pattern_review(s, l).svg_bytes(), s);
    l.coordinate_magnitude = 119;
    try { (void)admit_pattern_review(s, l); FAIL() << "Resource XML ancestry was lost"; }
    catch (SvgPreflightError const &e) { EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded); }
}

TEST(ArtworkLibrarySvgPreflight, ResourceRelativeBaselineDoesNotUseConsumerFont)
{
    auto s = pattern_review_svg(
        "<defs><clipPath id='c'><text baseline-shift='200%'>x</text></clipPath></defs>"
        "<rect width='10' height='10' font-size='65536px' clip-path='url(#c)'/>");
    // The resource keeps its XML parent's font; the paint consumer is not a
    // parent and must not supply a spurious 131072-unit local baseline shift.
    // The existing global glyph-metric overestimate is intentionally unchanged.
    EXPECT_EQ(*admit_pattern_review(s).svg_bytes(), s);
}

TEST(ArtworkLibrarySvgPreflight, UseRelativeBaselineRechecksLocalMetricCapInCloneContext)
{
    auto make = [](char const *shift) {
        return pattern_review_svg(std::string("<defs><text id='t' baseline-shift='") + shift +
            "'>x</text></defs><use xlink:href='#t' font-size='65536px'/>");
    };
    for (auto shift : {"100%", "-100%"}) {
        SCOPED_TRACE(shift);
        auto s = make(shift);
        EXPECT_EQ(*admit_pattern_review(s).svg_bytes(), s); // |local shift| = 65536.
    }
    for (auto shift : {"200%", "-200%"}) {
        SCOPED_TRACE(shift);
        try { (void)admit_pattern_review(make(shift)); FAIL() << "Contextual local metric cap bypass"; }
        catch (SvgPreflightError const &e) {
            EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded);
            EXPECT_NE(std::string(e.what()).find("Contextual baseline metric budget"), std::string::npos);
        }
    }
    // The cap is LOCAL, not a new 65536 cap on the accumulated baseline.
    auto accumulated = pattern_review_svg(
        "<g baseline-shift='65536px'><text baseline-shift='65536px'>x</text></g>");
    EXPECT_EQ(*admit_pattern_review(accumulated).svg_bytes(), accumulated);
}

TEST(ArtworkLibrarySvgPreflight, PatternOriginAddsBeforeTransformAtExactCoordinateBoundary)
{
    for (auto axis : {"x", "y"}) for (bool alias : {false, true}) {
        SCOPED_TRACE(axis);
        SCOPED_TRACE(alias);
        auto body = std::string("<defs><pattern id='base' patternUnits='userSpaceOnUse' width='16' height='16' ") +
            axis + "='8' patternTransform='matrix(2 0 0 2 3 3)'>"
            "<rect width='16' height='16'/></pattern>";
        if (alias) body += "<pattern id='p' xlink:href='#base' patternUnits='userSpaceOnUse' width='16' height='16'/>";
        auto s = pattern_review_svg(body + "</defs><rect width='10' height='10' fill='url(#" +
                                   (alias ? "p" : "base") + ")'/>");
        // max-axis envelope: (16 content/tile + 8 origin) * 2 + 3 = 51.
        // max(content, origin) would wrongly admit the 50-unit case.
        SvgPreflightLimits l; l.coordinate_magnitude = 51;
        EXPECT_EQ(*admit_pattern_review(s, l).svg_bytes(), s);
        l.coordinate_magnitude = 50;
        try { (void)admit_pattern_review(s, l); FAIL() << "Origin treated as maximum, not translation"; }
        catch (SvgPreflightError const &e) {
            EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded);
            EXPECT_NE(std::string(e.what()).find("Coordinate budget"), std::string::npos);
        }
    }
}

TEST(ArtworkLibrarySvgPreflight, LocalZeroPatternOriginOverridesPrototypeWithoutDoubleCharging)
{
    for (auto axis : {"x", "y"}) {
        SCOPED_TRACE(axis);
        auto s = pattern_review_svg(std::string(
            "<defs><pattern id='base' patternUnits='userSpaceOnUse' width='16' height='16' ") +
            axis + "='8'><rect width='16' height='16'/></pattern>"
            "<pattern id='p' xlink:href='#base' patternUnits='userSpaceOnUse' width='16' height='16' " +
            axis + "='0' patternTransform='scale(4)'/></defs><rect width='10' height='10' fill='url(#p)'/>");
        // Prototype's own checked bound is 24. Alias overrides origin to zero:
        // (16 + 0) * 4 = 64, not 96 or a second transform of the prototype.
        SvgPreflightLimits l; l.coordinate_magnitude = 64;
        EXPECT_EQ(*admit_pattern_review(s, l).svg_bytes(), s);
        l.coordinate_magnitude = 63;
        try { (void)admit_pattern_review(s, l); FAIL() << "Alias transform was omitted"; }
        catch (SvgPreflightError const &e) { EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded); }
    }
}

TEST(ArtworkLibrarySvgPreflight, OwnPatternContentUsesItsLocalOriginAndTransform)
{
    auto s = pattern_review_svg(
        "<defs><pattern id='base' patternUnits='userSpaceOnUse' width='16' height='16' x='8'>"
        "<rect width='16' height='16'/></pattern>"
        "<pattern id='p' xlink:href='#base' patternUnits='userSpaceOnUse' width='16' height='16' "
        "x='4' y='0' patternTransform='scale(2)'><rect width='20' height='1'/></pattern>"
        "</defs><rect width='10' height='10' fill='url(#p)'/>");
    SvgPreflightLimits l; l.coordinate_magnitude = 48; // (20 + 4) * 2.
    EXPECT_EQ(*admit_pattern_review(s, l).svg_bytes(), s);
    l.coordinate_magnitude = 47;
    try { (void)admit_pattern_review(s, l); FAIL() << "Own content/origin was omitted"; }
    catch (SvgPreflightError const &e) { EXPECT_EQ(e.failure(), SvgPreflightFailure::LimitExceeded); }
}

TEST(ArtworkLibrarySvgPreflight, XmlUseAndResourceTextTraversalRemainsCancellable)
{
    auto s = pattern_review_svg(
        "<defs><text id='t' baseline-shift='baseline'>x</text>"
        "<pattern id='p' patternUnits='userSpaceOnUse' width='16' height='16'>"
        "<use xlink:href='#t'/></pattern></defs>"
        "<rect width='10' height='10' baseline-shift='100px' fill='url(#p)'/>"
        "<g baseline-shift='1px'><use xlink:href='#t'/></g>");
    std::size_t total = 0;
    auto token = admit_pattern_review(s, {}, [&] { ++total; return false; });
    ASSERT_GT(total, 0u);
    for (std::size_t threshold = 1; threshold <= total; ++threshold) {
        SCOPED_TRACE(threshold);
        std::size_t polls = 0;
        try {
            (void)admit_pattern_review(s, {}, [&] { return ++polls >= threshold; });
            FAIL() << "Cancellation returned a token";
        } catch (SvgPreflightError const &e) {
            EXPECT_EQ(e.failure(), SvgPreflightFailure::Cancelled);
            EXPECT_EQ(polls, threshold);
        }
    }
    EXPECT_EQ(*admit_pattern_review(s).svg_bytes(), s);
    EXPECT_EQ(*token.svg_bytes(), s);
}

TEST(ArtworkLibrarySvgPreflight, TextPathPercentStartOffsetAndLargeHueRotationAreAdmitted)
{
    auto s = svg("<defs><path id='p' d='M0 0L20 20'/>"
                 "<filter id='f'><feColorMatrix type='hueRotate' values='540'/></filter></defs>"
                 "<text style='font-size:10px'><textPath href='#p' startOffset='50%'>Words</textPath></text>"
                 "<rect width='10' height='10' filter='url(#f)'/>");
    EXPECT_EQ(*admit(s).svg_bytes(), s);
    rejects(svg("<defs><path id='p' d='M0 0L20 20'/></defs>"
                "<text style='font-size:10px'><textPath href='#p' startOffset='50em'>W</textPath></text>"));
    rejects(svg("<filter id='f'><feColorMatrix type='hueRotate' values='1e7'/></filter><rect width='1' height='1' filter='url(#f)'/>"));
    rejects(svg("<filter id='f'><feColorMatrix type='saturate' values='2'/></filter><rect width='1' height='1' filter='url(#f)'/>"));
}
