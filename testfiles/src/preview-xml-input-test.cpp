// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Outcome tests for the bounded, non-loading XML input parse prerequisite.
 *
 * Every case calls the production helper in src/io/preview-xml-input.cpp; no
 * parser policy is re-implemented here. The helper performs no network or
 * filesystem access and never loads a native document, so these tests exercise
 * only in-memory libxml2 parsing. No test claims an SVG is admitted or safe for
 * native rendering: resource/namespace/graph policy is a later consumer.
 */
#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "io/preview-xml-input.h"

#include <libxml/tree.h>

using Inkscape::IO::Cancelled;
using Inkscape::IO::parse_preview_xml;
using Inkscape::IO::ParseResult;
using Inkscape::IO::ParsedPreviewXml;
using Inkscape::IO::PreviewXmlLimits;
using Inkscape::IO::PreviewXmlReason;
using Inkscape::IO::PreviewXmlStatus;

// The tree is exposed read-only; a mutating pointer type would fail to build.
static_assert(std::is_same_v<decltype(std::declval<ParsedPreviewXml const &>().document()),
                             xmlDoc const *>,
              "document() must return a read-only xmlDoc pointer");

namespace {

char const *const SVG_NS = "http://www.w3.org/2000/svg";
char const *const INK_NS = "http://www.inkscape.org/namespaces/inkscape";

ParseResult parse(std::string const &xml, PreviewXmlLimits limits = {}, Cancelled cancelled = {}) {
    return parse_preview_xml(
        std::span<unsigned char const>(reinterpret_cast<unsigned char const *>(xml.data()),
                                       xml.size()),
        limits, std::move(cancelled));
}

// Parse raw bytes (used for invalid UTF-8 / NUL cases).
ParseResult parse_bytes(std::vector<unsigned char> const &bytes, PreviewXmlLimits limits = {}) {
    return parse_preview_xml(std::span<unsigned char const>(bytes.data(), bytes.size()), limits);
}

std::string attr_value(xmlNode const *node, char const *name) {
    xmlChar *v = xmlGetProp(const_cast<xmlNode *>(node), BAD_CAST name);
    if (!v) {
        return {};
    }
    std::string out(reinterpret_cast<char const *>(v));
    xmlFree(v);
    return out;
}

xmlAttr const *find_attr(xmlNode const *node, char const *ns_href, char const *name) {
    for (xmlAttr const *a = node->properties; a; a = a->next) {
        if (std::strcmp(reinterpret_cast<char const *>(a->name), name) != 0) {
            continue;
        }
        if (ns_href) {
            if (!a->ns || !a->ns->href ||
                std::strcmp(reinterpret_cast<char const *>(a->ns->href), ns_href) != 0) {
                continue;
            }
        }
        return a;
    }
    return nullptr;
}

xmlNode const *find_child(xmlNode const *parent, char const *local) {
    for (xmlNode const *n = parent->children; n; n = n->next) {
        if (n->type == XML_ELEMENT_NODE &&
            std::strcmp(reinterpret_cast<char const *>(n->name), local) == 0) {
            return n;
        }
    }
    return nullptr;
}

std::size_t count_children_of_type(xmlNode const *parent, xmlElementType type) {
    std::size_t n = 0;
    for (xmlNode const *c = parent->children; c; c = c->next) {
        if (c->type == type) {
            ++n;
        }
    }
    return n;
}

// --- Parsed: ordinary vector/text, coordinates, metadata --------------------

TEST(PreviewXmlInput, OrdinaryVectorAndTextParsedWithUntouchedSnapshot) {
    std::string const xml =
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect x=\"1\" y=\"2\" width=\"3\" "
        "height=\"4\"/><text>hi</text></svg>";
    ParseResult const r = parse(xml);
    ASSERT_EQ(r.status, PreviewXmlStatus::Parsed);
    ASSERT_EQ(r.reason, PreviewXmlReason::Ok);
    ASSERT_TRUE(r.parsed_ok());
    ASSERT_NE(r.parsed->document(), nullptr);
    ASSERT_NE(r.parsed->root(), nullptr);
    EXPECT_STREQ(reinterpret_cast<char const *>(r.parsed->root()->name), "svg");
    ASSERT_NE(r.parsed->root()->ns, nullptr);
    EXPECT_STREQ(reinterpret_cast<char const *>(r.parsed->root()->ns->href), SVG_NS);
    EXPECT_NE(find_child(r.parsed->root(), "rect"), nullptr);
    EXPECT_NE(find_child(r.parsed->root(), "text"), nullptr);
    ASSERT_NE(r.original_bytes, nullptr);
    EXPECT_EQ(*r.original_bytes, xml);
    EXPECT_EQ(*r.parsed->original_bytes(), xml);
}

TEST(PreviewXmlInput, OffPageCoordinatesAreRetainedByteForByte) {
    std::string const xml =
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect x=\"-2500000\" y=\"9999999.5\" "
        "width=\"3e6\" height=\"4\"/></svg>";
    ParseResult const r = parse(xml);
    ASSERT_TRUE(r.parsed_ok());
    xmlNode const *rect = find_child(r.parsed->root(), "rect");
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(attr_value(rect, "x"), "-2500000");
    EXPECT_EQ(attr_value(rect, "y"), "9999999.5");
    EXPECT_EQ(attr_value(rect, "width"), "3e6");
    EXPECT_EQ(*r.original_bytes, xml);
}

TEST(PreviewXmlInput, DefaultTemplateNamedviewGuidesGridAndPageRetained) {
    std::string const xml =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        "xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\" "
        "xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" width=\"100\" "
        "height=\"100\">"
        "<sodipodi:namedview id=\"base\" pagecolor=\"#ffffff\" inkscape:zoom=\"3.62\" "
        "inkscape:cx=\"72.14\" showgrid=\"true\">"
        "<inkscape:grid id=\"grid1\" units=\"in\" originx=\"0\" spacingx=\"1.00032\" "
        "enabled=\"true\"/>"
        "<sodipodi:guide position=\"150.63908,48.578758\" orientation=\"1,0\" id=\"guide1\"/>"
        "</sodipodi:namedview>"
        "</svg>";
    ParseResult const r = parse(xml);
    ASSERT_TRUE(r.parsed_ok()) << "reason=" << static_cast<int>(r.reason);
    xmlNode const *nv = find_child(r.parsed->root(), "namedview");
    ASSERT_NE(nv, nullptr);
    EXPECT_EQ(attr_value(nv, "id"), "base");
    EXPECT_EQ(attr_value(nv, "pagecolor"), "#ffffff");
    EXPECT_EQ(attr_value(nv, "showgrid"), "true");
    // Prefixed attribute value and namespace survive (no policy decision made).
    EXPECT_EQ(attr_value(nv, "zoom"), "3.62");
    ASSERT_NE(find_attr(nv, INK_NS, "zoom"), nullptr);
    xmlNode const *grid = find_child(nv, "grid");
    xmlNode const *guide = find_child(nv, "guide");
    ASSERT_NE(grid, nullptr);
    ASSERT_NE(guide, nullptr);
    EXPECT_EQ(attr_value(grid, "spacingx"), "1.00032");
    EXPECT_EQ(attr_value(guide, "position"), "150.63908,48.578758");
    // No admission/resource decision is asserted here.
    EXPECT_EQ(*r.original_bytes, xml);
}

TEST(PreviewXmlInput, AliasNamespacesAndPrefixesAreRetained) {
    std::string const xml =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        "xmlns:foo=\"http://www.inkscape.org/namespaces/inkscape\">"
        "<foo:path foo:label=\"layer-1\"/></svg>";
    ParseResult const r = parse(xml);
    ASSERT_TRUE(r.parsed_ok());
    xmlNode const *path = find_child(r.parsed->root(), "path");
    ASSERT_NE(path, nullptr);
    ASSERT_NE(path->ns, nullptr);
    EXPECT_STREQ(reinterpret_cast<char const *>(path->ns->href), INK_NS);
    EXPECT_STREQ(reinterpret_cast<char const *>(path->ns->prefix), "foo");
    xmlAttr const *label = find_attr(path, INK_NS, "label");
    ASSERT_NE(label, nullptr);
    ASSERT_NE(label->ns, nullptr);
    EXPECT_STREQ(reinterpret_cast<char const *>(label->ns->prefix), "foo");
    EXPECT_EQ(attr_value(path, "label"), "layer-1");
}

TEST(PreviewXmlInput, UnicodeAndBomAcceptedAndBomPreserved) {
    std::string const xml =
        "\xEF\xBB\xBF<svg xmlns=\"http://www.w3.org/2000/svg\"><text>caf\xC3\xA9 "
        "\xF0\x9F\x98\x80</text></svg>";
    ParseResult const r = parse(xml);
    ASSERT_TRUE(r.parsed_ok());
    EXPECT_EQ(r.original_bytes->size(), xml.size());
    EXPECT_EQ(std::memcmp(r.original_bytes->data(), xml.data(), xml.size()), 0);
    EXPECT_EQ(static_cast<unsigned char>((*r.original_bytes)[0]), 0xEF);
    xmlNode const *text = find_child(r.parsed->root(), "text");
    ASSERT_NE(text, nullptr);
    ASSERT_NE(text->children, nullptr);
    std::string const content(reinterpret_cast<char const *>(text->children->content));
    EXPECT_NE(content.find("caf\xC3\xA9"), std::string::npos);
}

TEST(PreviewXmlInput, CommentsTextCdataAndPredefinedEntitiesAreCounted) {
    std::string const xml =
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><text>a<b/>"
        "<!--c--><![CDATA[d]]>&amp;&#65;&#x42;</text></svg>";
    ParseResult const r = parse(xml);
    ASSERT_TRUE(r.parsed_ok()) << static_cast<int>(r.reason);
    xmlNode const *text = find_child(r.parsed->root(), "text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(count_children_of_type(text, XML_COMMENT_NODE), 1u);
    EXPECT_EQ(count_children_of_type(text, XML_CDATA_SECTION_NODE), 1u);
    bool cdata_content_ok = false;
    for (xmlNode const *c = text->children; c; c = c->next) {
        if (c->type == XML_CDATA_SECTION_NODE && c->content &&
            std::strcmp(reinterpret_cast<char const *>(c->content), "d") == 0) {
            cdata_content_ok = true;
        }
    }
    EXPECT_TRUE(cdata_content_ok);
    // Comment, text and CDATA callbacks each charge one node.
    EXPECT_GE(r.node_count, 4u);
    EXPECT_EQ(r.parsed->node_count(), r.node_count);
    EXPECT_GE(r.max_depth, 2u);
    EXPECT_EQ(r.parsed->max_depth(), r.max_depth);
    EXPECT_EQ(*r.original_bytes, xml);
}

TEST(PreviewXmlInput, PredefinedEntitiesAndNumericReferencesResolve) {
    ParseResult const r = parse(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" a=\"&lt;&amp;&gt;&quot;&apos;\">&#65;&#x42;"
        "</svg>");
    ASSERT_TRUE(r.parsed_ok());
    EXPECT_EQ(attr_value(r.parsed->root(), "a"), "<&>\"'");
    xmlNode const *text = r.parsed->root()->children;
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<char const *>(text->content)), "AB");
}

// Existing coverage has predefined references only in the attribute and numeric
// references only in the text; this exercises both kinds in both places.
TEST(PreviewXmlInput, PredefinedAndNumericReferencesResolveInAttributeAndText) {
    ParseResult const r = parse("<svg xmlns=\"u\" a=\"&lt;&#65;&amp;\">&#x42;&amp;</svg>");
    ASSERT_TRUE(r.parsed_ok());
    EXPECT_EQ(attr_value(r.parsed->root(), "a"), "<A&");
    xmlNode const *text = r.parsed->root()->children;
    ASSERT_NE(text, nullptr);
    ASSERT_NE(text->content, nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<char const *>(text->content)), "B&");
}

TEST(PreviewXmlInput, Utf8EncodingDeclarationsAccepted) {
    for (char const *decl : {"UTF-8", "UTF8", "utf-8"}) {
        std::string const xml = std::string("<?xml version=\"1.0\" encoding=\"") + decl +
                                "\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        ParseResult const r = parse(xml);
        EXPECT_TRUE(r.parsed_ok()) << decl << " reason=" << static_cast<int>(r.reason);
    }
}

TEST(PreviewXmlInput, NamespaceDeclarationsAndAttributesAreCounted) {
    ParseResult const r = parse(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:a=\"u:a\" xmlns:b=\"u:b\" x=\"1\" "
        "y=\"2\"/>");
    ASSERT_TRUE(r.parsed_ok());
    EXPECT_EQ(r.namespace_count, 3u); // default + a + b
    EXPECT_EQ(r.attribute_count, 2u);
    EXPECT_EQ(r.parsed->namespace_count(), 3u);
    EXPECT_EQ(r.parsed->attribute_count(), 2u);
}

// --- Malformed XML ----------------------------------------------------------

TEST(PreviewXmlInput, MismatchedTagsRejected) {
    ParseResult const r = parse("<svg><rect></svg>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::MalformedXml);
    EXPECT_EQ(r.parsed, nullptr);
    EXPECT_NE(r.original_bytes, nullptr);
}

TEST(PreviewXmlInput, TruncatedDocumentRejected) {
    ParseResult const r = parse("<svg><rect/>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::MalformedXml);
}

TEST(PreviewXmlInput, MultipleRootsRejected) {
    ParseResult const r = parse("<a/><b/>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::MalformedXml);
}

TEST(PreviewXmlInput, GarbageRejected) { 
    ParseResult const r = parse("<svg><&*></svg>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::MalformedXml);
}

// --- Byte admission ---------------------------------------------------------

TEST(PreviewXmlInput, EmptyInputHasNoSnapshot) {
    ParseResult const r = parse("");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::EmptyInput);
    EXPECT_EQ(r.original_bytes, nullptr);
}

TEST(PreviewXmlInput, EmbeddedNulRejectedAfterIdenticalCopy) {
    std::string xml = "<svg xmlns=\"http://www.w3.org/2000/svg\">";
    xml.push_back('\0');
    xml += "</svg>";
    ParseResult const r = parse(xml);
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::EmbeddedNul);
    ASSERT_NE(r.original_bytes, nullptr);
    EXPECT_EQ(r.original_bytes->size(), xml.size());
}

TEST(PreviewXmlInput, InvalidUtf8Rejected) {
    std::vector<unsigned char> bytes = {'<', 's', 'v', 'g', '>', 0xC3, 0x28, '<', '/', 's', 'v', 'g', '>'};
    ParseResult const r = parse_bytes(bytes);
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::InvalidUtf8);
    ASSERT_NE(r.original_bytes, nullptr);
}

TEST(PreviewXmlInput, TooManyBytesRejectedBeforeCopy) {
    PreviewXmlLimits limits;
    limits.max_bytes = 8;
    ParseResult const r = parse("<svg></svg>", limits);
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::TooManyBytes);
    EXPECT_EQ(r.original_bytes, nullptr);
}

// A real buffer just over the default 4 MiB ceiling is refused at the byte
// stage, before the snapshot copy.
TEST(PreviewXmlInput, RealOverFourMibBufferRejectedBeforeCopy) {
    std::string big(4u * 1024u * 1024u + 1u, 'a');
    big[0] = '<';
    ParseResult const r = parse(big);
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::TooManyBytes);
    EXPECT_EQ(r.original_bytes, nullptr);
    EXPECT_EQ(r.parsed, nullptr);
}

TEST(PreviewXmlInput, ByteBudgetBoundaryIsInclusive) {
    std::string const xml = "<a/>";
    PreviewXmlLimits limits;
    limits.max_bytes = xml.size();
    EXPECT_TRUE(parse(xml, limits).parsed_ok());
    limits.max_bytes = xml.size() - 1;
    EXPECT_EQ(parse(xml, limits).reason, PreviewXmlReason::TooManyBytes);
}

TEST(PreviewXmlInput, InvalidLimitsRejected) {
    PreviewXmlLimits limits;
    limits.max_bytes = 0;
    ParseResult const r = parse("<svg/>", limits);
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::InvalidLimits);
    limits = PreviewXmlLimits{};
    limits.max_depth = 65; // hard ceiling is 64
    EXPECT_EQ(parse("<svg/>", limits).reason, PreviewXmlReason::InvalidLimits);
}

TEST(PreviewXmlInput, NonUtf8DeclaredEncodingRejected) {
    ParseResult const r =
        parse("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><svg xmlns=\"u\"/>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::UnsupportedEncoding);
}

// UTF-16 is refused by the native UTF-8 push parser before the declared-encoding
// metadata check can run, so the reason is MalformedXml rather than
// UnsupportedEncoding. The safety outcome is what matters here.
TEST(PreviewXmlInput, Utf16DeclaredEncodingRejectedWithoutReasonAssumption) {
    ParseResult const r =
        parse("<?xml version=\"1.0\" encoding=\"UTF-16\"?><svg xmlns=\"u\"/>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.parsed, nullptr);
    ASSERT_NE(r.original_bytes, nullptr);
    EXPECT_TRUE(r.reason == PreviewXmlReason::MalformedXml ||
                r.reason == PreviewXmlReason::UnsupportedEncoding)
        << "unexpected reason " << static_cast<int>(r.reason);
}

// --- Forbidden constructs ---------------------------------------------------

TEST(PreviewXmlInput, DoctypeRejected) {
    ParseResult const r = parse("<!DOCTYPE svg><svg/>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::ForbiddenDoctype);
    EXPECT_EQ(r.parsed, nullptr);
}

TEST(PreviewXmlInput, InternalSubsetEntityDeclarationRejected) {
    ParseResult const r = parse("<!DOCTYPE svg [<!ENTITY x \"y\">]><svg>&x;</svg>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::ForbiddenDoctype);
}

TEST(PreviewXmlInput, ExternalSubsetRejectedWithoutFetching) {
    ParseResult const r =
        parse("<!DOCTYPE svg SYSTEM \"http://example.invalid/never.dtd\"><svg/>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::ForbiddenDoctype);
}

TEST(PreviewXmlInput, CustomEntityReferenceRejected) {
    ParseResult const r = parse("<svg xmlns=\"http://www.w3.org/2000/svg\">&x;</svg>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::ForbiddenEntity);
}

TEST(PreviewXmlInput, ProcessingInstructionRejected) {
    ParseResult const r =
        parse("<?xml-stylesheet href=\"http://example.invalid/never.css\"?><svg/>");
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
    EXPECT_EQ(r.reason, PreviewXmlReason::ForbiddenProcessingInstruction);
}

// --- Budget boundaries ------------------------------------------------------

TEST(PreviewXmlInput, NodeBudgetBoundary) {
    PreviewXmlLimits limits;
    limits.max_nodes = 2;
    EXPECT_TRUE(parse("<a><!--x--></a>", limits).parsed_ok());
    limits.max_nodes = 1;
    ParseResult const r = parse("<a><!--x--></a>", limits);
    EXPECT_EQ(r.reason, PreviewXmlReason::NodeBudget);
    EXPECT_EQ(r.status, PreviewXmlStatus::Rejected);
}

TEST(PreviewXmlInput, DepthBudgetBoundary) {
    PreviewXmlLimits limits;
    limits.max_depth = 2;
    EXPECT_TRUE(parse("<a><b/></a>", limits).parsed_ok());
    limits.max_depth = 1;
    EXPECT_EQ(parse("<a><b/></a>", limits).reason, PreviewXmlReason::DepthBudget);
}

TEST(PreviewXmlInput, AttributeBudgetBoundary) {
    PreviewXmlLimits limits;
    limits.max_attributes_per_node = 2;
    EXPECT_TRUE(parse("<a x=\"1\" y=\"2\"/>", limits).parsed_ok());
    limits.max_attributes_per_node = 1;
    EXPECT_EQ(parse("<a x=\"1\" y=\"2\"/>", limits).reason, PreviewXmlReason::AttributeBudget);
}

TEST(PreviewXmlInput, NamespaceBudgetBoundary) {
    PreviewXmlLimits limits;
    limits.max_namespaces_per_element = 1;
    EXPECT_TRUE(parse("<a xmlns:x=\"u:x\"/>", limits).parsed_ok());
    limits.max_namespaces_per_element = 0; // invalid -> cannot lower to zero
    EXPECT_EQ(parse("<a xmlns:x=\"u:x\"/>", limits).reason, PreviewXmlReason::InvalidLimits);
    limits.max_namespaces_per_element = 1;
    EXPECT_EQ(parse("<a xmlns:x=\"u:x\" xmlns:y=\"u:y\"/>", limits).reason,
              PreviewXmlReason::NamespaceBudget);
}

TEST(PreviewXmlInput, NameBytesBudgetBoundary) {
    PreviewXmlLimits limits;
    limits.max_name_bytes = 8;
    // 7-byte name is admitted; the budget rejects at >= 8.
    EXPECT_TRUE(parse("<abcdefg/>", limits).parsed_ok());
    EXPECT_EQ(parse("<abcdefgh/>", limits).reason, PreviewXmlReason::NameBytesBudget);
    // Composed prefix:local is measured, not just the local part.
    EXPECT_TRUE(parse("<a xmlns:pre=\"u:p\" pre:x=\"1\"/>", limits).parsed_ok());
    EXPECT_EQ(parse("<a xmlns:prefix=\"u:p\" prefix:x=\"1\"/>", limits).reason,
              PreviewXmlReason::NameBytesBudget);
}

TEST(PreviewXmlInput, BudgetIsChargedBeforeDomAllocation) {
    PreviewXmlLimits limits;
    limits.max_nodes = 1;
    ParseResult const r = parse("<a><b/></a>", limits);
    EXPECT_EQ(r.reason, PreviewXmlReason::NodeBudget);
    // Rejection happens while charging the second element; no tree is returned.
    EXPECT_EQ(r.parsed, nullptr);
}

// --- Cancellation -----------------------------------------------------------

TEST(PreviewXmlInput, CancelledBeforeParse) {
    int calls = 0;
    Cancelled const cancelled = [&] {
        ++calls;
        return true;
    };
    ParseResult const r = parse("<svg xmlns=\"http://www.w3.org/2000/svg\"/>", {}, cancelled);
    EXPECT_EQ(r.status, PreviewXmlStatus::Cancelled);
    EXPECT_EQ(r.reason, PreviewXmlReason::Cancelled);
    EXPECT_EQ(r.parsed, nullptr);
    EXPECT_EQ(calls, 1);
}

TEST(PreviewXmlInput, CancelledDuringParse) {
    std::string xml = "<svg xmlns=\"http://www.w3.org/2000/svg\">";
    for (int i = 0; i < 500; ++i) {
        xml += "<g>";
    }
    for (int i = 0; i < 500; ++i) {
        xml += "</g>";
    }
    xml += "</svg>";
    int calls = 0;
    Cancelled const cancelled = [&] {
        return ++calls >= 5;
    };
    ParseResult const r = parse(xml, {}, cancelled);
    EXPECT_EQ(r.status, PreviewXmlStatus::Cancelled);
    EXPECT_EQ(r.reason, PreviewXmlReason::Cancelled);
    EXPECT_EQ(r.parsed, nullptr);
    EXPECT_GE(calls, 5);
}

TEST(PreviewXmlInput, RepeatedParseAndRejectDoesNotCrashOrRetain) {
    for (int i = 0; i < 200; ++i) {
        ParseResult const ok = parse("<svg xmlns=\"http://www.w3.org/2000/svg\"><rect/></svg>");
        EXPECT_TRUE(ok.parsed_ok());
        ParseResult const bad = parse("<!DOCTYPE svg><svg>&x;</svg>");
        EXPECT_EQ(bad.status, PreviewXmlStatus::Rejected);
    }
}

// --- Snapshot independence --------------------------------------------------

TEST(PreviewXmlInput, SnapshotSurvivesCallerBufferMutation) {
    std::string mutable_input =
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect x=\"111\"/></svg>";
    std::string const original = mutable_input;
    ParseResult r = parse(mutable_input);
    ASSERT_TRUE(r.parsed_ok());
    // Mutate the caller's buffer after the call; the snapshot and tree are
    // independent copies.
    mutable_input.assign(mutable_input.size(), 'X');
    EXPECT_EQ(*r.original_bytes, original);
    xmlNode const *rect = find_child(r.parsed->root(), "rect");
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(attr_value(rect, "x"), "111");
}

TEST(PreviewXmlInput, ToStringCoversStatusAndReason) {
    EXPECT_EQ(Inkscape::IO::to_string(PreviewXmlStatus::Parsed), "Parsed");
    EXPECT_EQ(Inkscape::IO::to_string(PreviewXmlReason::ForbiddenDoctype), "ForbiddenDoctype");
    EXPECT_EQ(Inkscape::IO::to_string(PreviewXmlReason::Cancelled), "Cancelled");
}

} // namespace
