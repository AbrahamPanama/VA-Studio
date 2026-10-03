// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Outcome tests for the bounded W5 resource policy over `ParsedPreviewXml`.
 *
 * Every case parses through the accepted bounded parser and then calls
 * `admit_preview_resources`; no policy is re-implemented here. Native bounding
 * only: the tests never load a document, touch the filesystem/network, call
 * SPDocument/SPFactory, or serialize. `Candidate` is asserted only as "no
 * policy escape found", never as fidelity or safety.
 */
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "io/preview-svg-resource-policy.h"
#include "io/preview-xml-input.h"

using Inkscape::IO::admit_preview_resources;
using Inkscape::IO::Cancelled;
using Inkscape::IO::parse_preview_xml;
using Inkscape::IO::ParsedPreviewXml;
using Inkscape::IO::ParseResult;
using Inkscape::IO::PreviewResourcePolicyLimits;
using Inkscape::IO::PreviewResourcePolicyReason;
using Inkscape::IO::PreviewResourcePolicyResult;
using Inkscape::IO::PreviewResourcePolicyStatus;
using Inkscape::IO::PreviewXmlLimits;
using Inkscape::IO::PreviewXmlReason;
using Inkscape::IO::PreviewXmlStatus;

namespace {

char const *const SVG_NS = "http://www.w3.org/2000/svg";

struct Audited {
    ParseResult parsed;
    PreviewResourcePolicyResult result;
};

Audited audit(std::string const &xml, PreviewResourcePolicyLimits limits = {},
              Cancelled cancelled = {}) {
    Audited a;
    a.parsed = parse_preview_xml(
        std::span<unsigned char const>(reinterpret_cast<unsigned char const *>(xml.data()),
                                       xml.size()),
        PreviewXmlLimits{}, {});
    if (a.parsed.parsed) {
        a.result = admit_preview_resources(*a.parsed.parsed, limits, std::move(cancelled));
    }
    return a;
}

// Every status must hand back the exact same bytes owner and leave the const DOM
// and original bytes untouched.
void check_invariants(Audited const &a, std::string const &xml) {
    if (!a.parsed.parsed) {
        return;
    }
    ASSERT_NE(a.parsed.original_bytes, nullptr);
    EXPECT_EQ(a.result.original_bytes.get(), a.parsed.original_bytes.get());
    EXPECT_EQ(a.result.original_bytes.get(), a.parsed.parsed->original_bytes().get());
    EXPECT_EQ(*a.parsed.parsed->original_bytes(), xml);
    EXPECT_NE(a.parsed.parsed->document(), nullptr);
    EXPECT_NE(a.parsed.parsed->root(), nullptr);
}

std::string svg_open() {
    return std::string("<svg xmlns=\"") + SVG_NS + "\">";
}

// `levels` nested groups, each holding two `<use>` clones of the next group.
// Unique nodes/edges are O(levels); the expanded `use` clone count is 2^levels.
std::string two_use_per_level(int levels) {
    std::string x = std::string("<svg xmlns=\"") + SVG_NS +
                    "\" xmlns:xlink=\"http://www.w3.org/1999/xlink\"><defs>";
    for (int i = 0; i < levels; ++i) {
        x += "<g id=\"g" + std::to_string(i) + "\">";
        x += "<use xlink:href=\"#g" + std::to_string(i + 1) + "\"/>";
        x += "<use xlink:href=\"#g" + std::to_string(i + 1) + "\"/>";
        x += "</g>";
    }
    x += "<g id=\"g" + std::to_string(levels) + "\"><rect width=\"1\" height=\"1\"/></g>";
    x += "</defs><use xlink:href=\"#g0\"/></svg>";
    return x;
}

} // namespace

// --- Candidate first subset -------------------------------------------------

TEST(PreviewSvgResourcePolicy, PlainSvgIsCandidate) {
    std::string const xml =
        svg_open() + "<rect x=\"1\" y=\"2\" width=\"3\" height=\"4\"/><text>hi</text></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Candidate);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::Ok);
    EXPECT_TRUE(a.result.candidate());
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, CanonicalKnownAliasesAreCandidate) {
    std::string const xml =
        "<s:svg xmlns:s=\"" + std::string(SVG_NS) +
        "\" xmlns:k=\"http://www.inkscape.org/namespaces/inkscape\">"
        "<s:g k:label=\"layer\"><s:rect width=\"1\" height=\"1\"/></s:g></s:svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Candidate);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, StandardXmlSpaceAndLangAreCandidate) {
    std::string const xml =
        "<svg xmlns=\"" + std::string(SVG_NS) +
        "\" xmlns:xml=\"http://www.w3.org/XML/1998/namespace\" xml:space=\"preserve\" "
        "xml:lang=\"en\"><text>hi</text></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Candidate);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, OtherXmlNamespaceFeatureIsUnsupported) {
    std::string const xml = "<svg xmlns=\"" + std::string(SVG_NS) +
                            "\" xmlns:xml=\"http://www.w3.org/XML/1998/namespace\" "
                            "xml:base=\"http://example.com/\"><rect/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::XmlNamespaceFeatureUnsupported);
    check_invariants(a, xml);
}

// --- E1: reserved prefix / alias abuse is refused by the bounded parser -----

TEST(PreviewSvgResourcePolicy, ReservedWrongXmlUriIsRejectedByParser) {
    std::string const xml = "<svg xmlns=\"" + std::string(SVG_NS) +
                            "\" xmlns:xml=\"http://evil.example/\"><rect/></svg>";
    auto const a = audit(xml);
    EXPECT_FALSE(a.parsed.parsed_ok());
    EXPECT_EQ(a.parsed.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(a.parsed.reason, PreviewXmlReason::MalformedXml);
}

TEST(PreviewSvgResourcePolicy, AltPrefixBoundToXmlUriIsRejectedByParser) {
    std::string const xml = "<svg xmlns=\"" + std::string(SVG_NS) +
                            "\" xmlns:x=\"http://www.w3.org/XML/1998/namespace\"><rect/></svg>";
    auto const a = audit(xml);
    EXPECT_FALSE(a.parsed.parsed_ok());
    EXPECT_EQ(a.parsed.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(a.parsed.reason, PreviewXmlReason::MalformedXml);
}

// --- Realistic saved metadata / scaffolding ---------------------------------

TEST(PreviewSvgResourcePolicy, RealisticNamedviewGridGuideMetadataIsCandidate) {
    std::string const xml = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n") +
        "<svg xmlns=\"" + SVG_NS +
        "\" xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\""
        " xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\""
        " xmlns:xlink=\"http://www.w3.org/1999/xlink\""
        " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
        " xmlns:cc=\"http://creativecommons.org/ns#\""
        " xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\""
        " version=\"1.1\" width=\"100\" height=\"100\" viewBox=\"0 0 100 100\""
        " xml:space=\"preserve\">"
        "<sodipodi:namedview id=\"base\" pagecolor=\"#ffffff\" bordercolor=\"#666666\""
        " borderopacity=\"1.0\" inkscape:pageopacity=\"0.0\" inkscape:pageshadow=\"2\""
        " inkscape:zoom=\"0.35\" inkscape:cx=\"0\" inkscape:cy=\"0\""
        " inkscape:document-units=\"mm\" inkscape:current-layer=\"layer1\""
        " showgrid=\"true\" inkscape:window-width=\"100\" inkscape:window-height=\"100\""
        " inkscape:window-x=\"0\" inkscape:window-y=\"0\" inkscape:window-maximized=\"0\">"
        "<inkscape:grid id=\"grid1\" units=\"in\" originx=\"0\" originy=\"0\""
        " spacingx=\"1.00032\" spacingy=\"1.00032\" enabled=\"true\" visible=\"true\"/>"
        "<sodipodi:guide position=\"150.63908,48.578758\" orientation=\"1,0\" id=\"guide1\"/>"
        "</sodipodi:namedview>"
        "<defs>"
        "<linearGradient id=\"grad1\"><stop offset=\"0\""
        " style=\"stop-color:#000000;stop-opacity:1\"/></linearGradient>"
        "<pattern id=\"pat1\" patternUnits=\"userSpaceOnUse\" width=\"4\" height=\"4\">"
        "<rect width=\"2\" height=\"2\"/></pattern>"
        "<view id=\"view1\" viewBox=\"0 0 100 100\"/>"
        "</defs>"
        "<metadata id=\"metadata1\"><rdf:RDF><cc:Work rdf:about=\"\">"
        "<dc:title>Title</dc:title><dc:creator>Creator</dc:creator>"
        "<dc:date>2020-01-01</dc:date>"
        "<cc:license rdf:resource=\"http://creativecommons.org/licenses/by/4.0/\"/>"
        "</cc:Work></rdf:RDF></metadata>"
        "<g id=\"layer1\" inkscape:label=\"Layer 1\" inkscape:groupmode=\"layer\">"
        "<rect id=\"r1\" x=\"1\" y=\"2\" width=\"3\" height=\"4\"/>"
        "<text id=\"t1\" x=\"0\" y=\"10\">hi</text>"
        "</g></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Candidate)
        << to_string(a.result.reason);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, ForeignMetadataElementIsUnsupported) {
    std::string const xml = svg_open() + "<metadata><rect/></metadata></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::ForeignMetadata);
    check_invariants(a, xml);
}

// --- Namespaces, elements, attributes, dispatch -----------------------------

TEST(PreviewSvgResourcePolicy, UnknownNamespaceIsUnsupported) {
    std::string const xml = "<foo:svg xmlns:foo=\"http://example.com/ns\"><foo:rect/></foo:svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::UnknownNamespace);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, NoNamespaceElementIsNamespaceRepair) {
    std::string const xml = "<svg><rect/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::NamespaceRepair);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, UnknownElementIsUnsupported) {
    std::string const xml = svg_open() + "<blink/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::UnknownElement);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, UnknownAttributeIsUnsupported) {
    std::string const xml = svg_open() + "<rect bogus=\"1\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::UnknownAttribute);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, SodipodiTypeIsUnknownDispatch) {
    std::string const xml =
        "<svg xmlns=\"" + std::string(SVG_NS) +
        "\" xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\">"
        "<path sodipodi:type=\"star\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::UnknownDispatch);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, BareStarKeyIsUnsupported) {
    // arc/star/spiral need later scoped support; no guessed mapping here.
    std::string const xml = svg_open() + "<star/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, ImageIsBitmapPlaceholder) {
    std::string const xml = svg_open() + "<image href=\"data:image/png;base64,AAAA\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::BitmapOrImageFeature);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, ColorProfileIsIccPlaceholder) {
    std::string const xml = svg_open() + "<color-profile href=\"x.icc\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::IccColorProfile);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, PathEffectIsActivePlaceholder) {
    std::string const xml =
        "<svg xmlns=\"" + std::string(SVG_NS) +
        "\" xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\">"
        "<inkscape:path-effect id=\"pe1\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::ActivePathEffect);
    check_invariants(a, xml);
}

// --- Duplicate / missing ids ------------------------------------------------

TEST(PreviewSvgResourcePolicy, DuplicateIdIsRejected) {
    std::string const xml = svg_open() + "<rect id=\"a\"/><rect id=\"a\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::DuplicateId);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, MissingReferencedIdIsRejected) {
    std::string const xml = svg_open() + "<use href=\"#nope\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::UnresolvedReference);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, CanonicalNameAt256BytesIsRejected) {
    std::string const local(252, 'a'); // "svg:" + 252 = 256 canonical bytes
    std::string const xml = svg_open() + "<" + local + "/></svg>";
    // The bounded parser charges the unprefixed document name (252 < 256).
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::NameBytesBudget);
    check_invariants(a, xml);
}

// --- href / CSS references --------------------------------------------------

TEST(PreviewSvgResourcePolicy, ExternalUseHrefIsRejected) {
    std::string const xml = svg_open() + "<use href=\"other.svg#g\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::AmbiguousReference);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, ExternalCssUrlIsRejected) {
    std::string const xml =
        svg_open() + "<rect style=\"fill:url(http://evil.example/x.svg#g)\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::CssRejected);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, SimpleGradientAndPresentationAreCandidate) {
    std::string const xml = svg_open() +
        "<defs><linearGradient id=\"grad\"><stop offset=\"0\" stop-color=\"#000\"/>"
        "</linearGradient></defs>"
        "<rect width=\"1\" height=\"1\" fill=\"url(#grad)\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Candidate)
        << to_string(a.result.reason);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, StylesheetLocalReferenceIsUnsupported) {
    std::string const xml = svg_open() +
        "<style>#a { fill: url(#grad); }</style>"
        "<linearGradient id=\"grad\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::UnsupportedStylesheetReferences);
    check_invariants(a, xml);
}

// --- use interaction (E2) ---------------------------------------------------

TEST(PreviewSvgResourcePolicy, PlainLocalUseIsCandidate) {
    std::string const xml = svg_open() +
        "<defs><g id=\"g\"><rect id=\"r\" width=\"1\" height=\"1\"/></g></defs>"
        "<use href=\"#g\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Candidate)
        << to_string(a.result.reason);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, UseWithLocalInlineResourceIsUnsupported) {
    std::string const xml = svg_open() +
        "<defs><linearGradient id=\"g\"/></defs>"
        "<use href=\"#g\" style=\"fill:url(#g)\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Unsupported);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::UnsupportedUseResourceInheritance);
    check_invariants(a, xml);
}

// --- Graph: cycles, fanout, budgets -----------------------------------------

TEST(PreviewSvgResourcePolicy, HrefCycleCannotBecomeCandidate) {
    std::string const xml =
        "<svg xmlns=\"" + std::string(SVG_NS) +
        "\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
        "<linearGradient id=\"a\" xlink:href=\"#b\"/>"
        "<linearGradient id=\"b\" xlink:href=\"#a\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::ReferenceCycle);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, InheritedPatternCycleCannotBecomeCandidate) {
    std::string const xml = svg_open() +
        "<pattern id=\"p\"><rect width=\"1\" height=\"1\" fill=\"url(#p)\"/></pattern></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::ReferenceCycle);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, BoundedAcyclicFanoutIsEnforced) {
    std::string const xml = svg_open() + "<g><rect/><rect/><rect/></g></svg>";
    PreviewResourcePolicyLimits limits;
    limits.max_fanout = 2;
    auto const a = audit(xml, limits);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::ReferenceFanoutBudget);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, ReferenceDepthBudgetIsEnforced) {
    std::string const xml =
        "<svg xmlns=\"" + std::string(SVG_NS) +
        "\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">";
    std::string body;
    for (int i = 0; i < 40; ++i) {
        body += "<linearGradient id=\"g" + std::to_string(i) + "\" xlink:href=\"#g" +
                std::to_string(i + 1) + "\"/>";
    }
    body += "<linearGradient id=\"g40\"/>";
    std::string const full = xml + body + "</svg>";
    PreviewResourcePolicyLimits limits;
    limits.max_reference_depth = 8;
    auto const a = audit(full, limits);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::ReferenceDepth);
    check_invariants(a, full);
}

TEST(PreviewSvgResourcePolicy, AggregateCssByteBudgetIsEnforced) {
    std::string const xml = svg_open() + "<rect style=\"fill:#ff0000\"/>"
                                         "<rect style=\"fill:#00ff00\"/></svg>";
    PreviewResourcePolicyLimits limits;
    limits.css.max_bytes = 20;
    auto const a = audit(xml, limits);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::CssRejected);
    check_invariants(a, xml);
}

// --- Cancellation and byte/DOM ownership ------------------------------------

TEST(PreviewSvgResourcePolicy, CancellationIsObserved) {
    std::string const xml = svg_open() + "<rect/></svg>";
    PreviewResourcePolicyLimits limits;
    auto const a = audit(xml, limits, [] { return true; });
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Cancelled);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::Cancelled);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, BytesOwnerAndConstDomPreservedForAllStatuses) {
    std::string const candidate = svg_open() + "<rect/></svg>";
    std::string const unsupported = svg_open() + "<image href=\"x.png\"/></svg>";
    std::string const rejected = svg_open() + "<rect id=\"a\"/><rect id=\"a\"/></svg>";
    for (std::string const *xml : {&candidate, &unsupported, &rejected}) {
        auto const a = audit(*xml);
        ASSERT_TRUE(a.parsed.parsed_ok());
        xmlDoc const *doc = a.parsed.parsed->document();
        xmlNode const *root = a.parsed.parsed->root();
        ASSERT_NE(doc, nullptr);
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(a.result.original_bytes.get(), a.parsed.original_bytes.get());
        EXPECT_EQ(*a.parsed.original_bytes, *xml);
        EXPECT_EQ(a.parsed.parsed->document(), doc);
        EXPECT_EQ(a.parsed.parsed->root(), root);
    }
}

// --- Corrected expanded-work DP and reference-depth DP ----------------------

TEST(PreviewSvgResourcePolicy, RepeatedUseDagOverBudgetIsRejected) {
    // Unique nodes/edges are O(levels), but the expanded `use` clone count is
    // 2^levels. The DP must count duplicate edges separately and reject.
    std::string const xml = two_use_per_level(20);
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::ExpandedWorkBudget);
    // The reported metric is the expansion, not the small unique traversal.
    EXPECT_GT(a.result.expanded_work, 20000u);
    EXPECT_LT(a.result.validation_work, 1000u);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, SmallDagExpandedWorkHasKnownValue) {
    // svg(0) -> g=a(1) -> rect(2); svg -> rect c(3); svg -> use(4) -> a.
    // expanded: rect=1, a=2, c=1, use=3, svg=1+2+1+3=7; depth: use->a=1.
    std::string const xml = svg_open() +
        "<g id=\"a\"><rect/></g>"
        "<rect id=\"c\"/>"
        "<use href=\"#a\"/></svg>";
    auto const a = audit(xml);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Candidate)
        << to_string(a.result.reason);
    EXPECT_EQ(a.result.expanded_work, 7u);
    EXPECT_EQ(a.result.max_reference_depth, 1u);
    EXPECT_GT(a.result.validation_work, 0u);
    EXPECT_NE(a.result.validation_work, a.result.expanded_work);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, ExpandedWorkBudgetBoundaryIsExact) {
    // Same known-sum-7 DAG as SmallDagExpandedWorkHasKnownValue. A limit equal
    // to the exact sum admits (Candidate); one below it rejects. Nothing is
    // unrolled: the check is a DP sum over unique edges, and no loader is
    // invoked by this policy.
    std::string const xml = svg_open() +
        "<g id=\"a\"><rect/></g>"
        "<rect id=\"c\"/>"
        "<use href=\"#a\"/></svg>";
    PreviewResourcePolicyLimits exact;
    exact.max_expanded_work = 7;
    auto const at = audit(xml, exact);
    ASSERT_TRUE(at.parsed.parsed_ok());
    EXPECT_EQ(at.result.status, PreviewResourcePolicyStatus::Candidate)
        << to_string(at.result.reason);
    EXPECT_EQ(at.result.expanded_work, 7u);

    PreviewResourcePolicyLimits one_over;
    one_over.max_expanded_work = 6;
    auto const over = audit(xml, one_over);
    ASSERT_TRUE(over.parsed.parsed_ok());
    EXPECT_EQ(over.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(over.result.reason, PreviewResourcePolicyReason::ExpandedWorkBudget);
    EXPECT_EQ(over.result.expanded_work, 7u);
    check_invariants(at, xml);
    check_invariants(over, xml);
}

TEST(PreviewSvgResourcePolicy, ReorderedReferenceGraphHasIdenticalStatus) {
    // `a` refs b and c; `c` refs b: longest chain a->c->b is depth 2. Only the
    // attribute order on `a` changes; the status must not depend on it.
    std::string const head = std::string("<svg xmlns=\"") + SVG_NS +
                             "\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">";
    std::string const tail = "<linearGradient id=\"b\"/></svg>";
    std::string const v1 = head +
        "<linearGradient id=\"a\" xlink:href=\"#b\" href=\"#c\"/>"
        "<linearGradient id=\"c\" xlink:href=\"#b\"/>" + tail;
    std::string const v2 = head +
        "<linearGradient id=\"a\" href=\"#c\" xlink:href=\"#b\"/>"
        "<linearGradient id=\"c\" xlink:href=\"#b\"/>" + tail;
    PreviewResourcePolicyLimits limits;
    limits.max_reference_depth = 1;
    auto const a1 = audit(v1, limits);
    auto const a2 = audit(v2, limits);
    ASSERT_TRUE(a1.parsed.parsed_ok());
    ASSERT_TRUE(a2.parsed.parsed_ok());
    for (auto const *a : {&a1, &a2}) {
        EXPECT_EQ(a->result.status, PreviewResourcePolicyStatus::Rejected);
        EXPECT_EQ(a->result.reason, PreviewResourcePolicyReason::ReferenceDepth);
    }
    check_invariants(a1, v1);
    check_invariants(a2, v2);
}

TEST(PreviewSvgResourcePolicy, BottomUpAndTopDownChainsShareMaxReferenceDepth) {
    // The same a->b->c reference chain written top-down and bottom-up must give
    // the same longest reference depth (2) and the same expanded root (7).
    std::string const head = std::string("<svg xmlns=\"") + SVG_NS +
                             "\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">";
    std::string const top_down = head +
        "<linearGradient id=\"a\" xlink:href=\"#b\"/>"
        "<linearGradient id=\"b\" xlink:href=\"#c\"/>"
        "<linearGradient id=\"c\"/></svg>";
    std::string const bottom_up = head +
        "<linearGradient id=\"c\"/>"
        "<linearGradient id=\"b\" xlink:href=\"#c\"/>"
        "<linearGradient id=\"a\" xlink:href=\"#b\"/></svg>";
    auto const t = audit(top_down);
    auto const b = audit(bottom_up);
    ASSERT_TRUE(t.parsed.parsed_ok());
    ASSERT_TRUE(b.parsed.parsed_ok());
    for (auto const *a : {&t, &b}) {
        EXPECT_EQ(a->result.status, PreviewResourcePolicyStatus::Candidate)
            << to_string(a->result.reason);
        EXPECT_EQ(a->result.max_reference_depth, 2u);
        EXPECT_EQ(a->result.expanded_work, 7u);
    }
    check_invariants(t, top_down);
    check_invariants(b, bottom_up);
}

TEST(PreviewSvgResourcePolicy, SaturatingExpandedWorkDoesNotOverflow) {
    // 2^70 clone paths exceed every `size_t` bound. Saturating the DP metric
    // must not authorize Candidate merely because the configured ceiling is
    // `SIZE_MAX`: the true expansion is larger than the limit, so the
    // independent overflow check must reject. The reported metric is the
    // saturated lower bound, never a wrapped value.
    std::string const xml = two_use_per_level(70);
    PreviewResourcePolicyLimits limits;
    limits.max_reference_depth = std::numeric_limits<std::size_t>::max();
    limits.max_expanded_work = std::numeric_limits<std::size_t>::max();
    auto const a = audit(xml, limits);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected)
        << to_string(a.result.reason);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::ExpandedWorkBudget);
    EXPECT_EQ(a.result.expanded_work, std::numeric_limits<std::size_t>::max());
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, ValidationWorkBudgetIsEnforced) {
    // Eight nested groups give nine containment edges and no references, so the
    // conservative expanded pre-charge (default 20000) and the fanout bound
    // (default 1024) are both far from being hit. A low unique-validation
    // ceiling must reject first, proving `max_validation_work` is enforced
    // independently of expansion/fanout.
    std::string xml = svg_open();
    for (int i = 0; i < 8; ++i) {
        xml += "<g>";
    }
    xml += "<rect/>";
    for (int i = 0; i < 8; ++i) {
        xml += "</g>";
    }
    xml += "</svg>";
    PreviewResourcePolicyLimits limits;
    limits.max_validation_work = 5;
    auto const a = audit(xml, limits);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::ValidationWorkBudget);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, TightPolicyNodeBudgetIsEnforcedBeforeWalk) {
    // The parser sees two element nodes; the tighter policy bound must reject
    // before the walk/allocations, and preserves the bytes owner.
    std::string const xml = svg_open() + "<rect/></svg>";
    PreviewResourcePolicyLimits limits;
    limits.max_nodes = 1;
    auto const a = audit(xml, limits);
    ASSERT_TRUE(a.parsed.parsed_ok());
    EXPECT_EQ(a.result.status, PreviewResourcePolicyStatus::Rejected);
    EXPECT_EQ(a.result.reason, PreviewResourcePolicyReason::NodeBudget);
    check_invariants(a, xml);
}

TEST(PreviewSvgResourcePolicy, CancellationIsCheckedBeforeCandidate) {
    // First run counts the deterministic poll sequence of a Candidate document.
    // Second run cancels exactly at the final poll, before Candidate is set.
    std::string const xml = svg_open() + "<rect/></svg>";
    std::size_t calls = 0;
    auto count_only = [&calls]() {
        ++calls;
        return false;
    };
    auto const first = audit(xml, {}, count_only);
    ASSERT_EQ(first.result.status, PreviewResourcePolicyStatus::Candidate);
    ASSERT_GT(calls, 0u);
    std::size_t const total = calls;
    std::size_t n = 0;
    auto cancel_last = [&n, total]() { return ++n >= total; };
    auto const second = audit(xml, {}, cancel_last);
    EXPECT_EQ(second.result.status, PreviewResourcePolicyStatus::Cancelled);
    EXPECT_EQ(second.result.reason, PreviewResourcePolicyReason::Cancelled);
    check_invariants(second, xml);
}
