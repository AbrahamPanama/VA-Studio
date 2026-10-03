// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Focused native tests for Inkscape::XML::PreparedContentProof.
 *
 * Contract under test (see src/xml/prepared-content-proof.h):
 *  - capture() duplicates a live native XML::Document through the ordinary
 *    SimpleNode copy, anchors the duplicate via GC::release-RAII, and reports
 *    unavailable (nullptr) on any exact mismatch, including the DOCUMENT node
 *    fields that SimpleDocument::duplicate does not copy;
 *  - matches() repeats the exact const comparison plus filename/base nullness;
 *  - the snapshot shares the source's immutable content/attribute buffers, so
 *    pointer identity is a valid oracle while both owners are alive;
 *  - exact comparison is type/name/content/nullness/child-count/preorder plus an
 *    unordered exact attribute map (duplicate keys are invalid, not a multiset).
 *
 * Scope limits, stated deliberately:
 *  - no serializer fingerprint is used anywhere; sp_repr_save_buf can normalise
 *    live XML, so it cannot prove the source was untouched. Structure checks use
 *    const native getters only.
 *  - test 9 is native ownership/retention coverage for a modest multi-MiB payload.
 *    It is NOT a 250 MiB peak/latency gate and makes no prompt-reclamation claim:
 *    a conservative collector cannot promise that a forced collection frees the
 *    detached working copy, only that the anchored proof keeps its bytes valid.
 *  - all data is generated in-process; no fixture files are read or written.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <glib.h>

#include "gc-anchored.h"
#include "inkgc/gc-core.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/prepared-content-proof.h"
#include "xml/repr.h"
#include "xml/text-node.h"

namespace {

namespace XML = Inkscape::XML;
using Proof = XML::PreparedContentProof;

struct ReleaseDocument
{
    void operator()(XML::Document *document) const
    {
        if (document) {
            Inkscape::GC::release(document);
        }
    }
};
using OwnedDocument = std::unique_ptr<XML::Document, ReleaseDocument>;

OwnedDocument load(char const *xml)
{
    return OwnedDocument(sp_repr_read_buf(xml, SP_SVG_NS_URI));
}

// Raw, non-mutating structural snapshot: node type, name, content, every
// attribute in its actual order (key then value, length-prefixed), then children
// in document order. Uses const native getters only and never serialises.
void fingerprintNode(XML::Node const *node, std::string &out)
{
    auto field = [&](char const *text) {
        if (!text) {
            out += "N;";
            return;
        }
        std::string value = text;
        out += "V" + std::to_string(value.size()) + ":";
        out += value;
    };
    out += "[" + std::to_string(static_cast<int>(node->type()));
    if (auto const *text = dynamic_cast<XML::TextNode const *>(node)) {
        out += text->is_CData() ? "C;" : "T;";
    }
    field(node->name());
    field(node->content());
    for (auto const &attr : node->attributeList()) {
        field(g_quark_to_string(attr.key));
        field(static_cast<char const *>(attr.value));
    }
    out += ";";
    for (auto child = node->firstChild(); child; child = child->next()) {
        fingerprintNode(child, out);
    }
    out += "]";
}

std::string rawFingerprint(XML::Document const &document)
{
    std::string result;
    fingerprintNode(&document, result);
    return result;
}

// Append a freshly created native node and drop the creation anchor, matching
// the adjacent native tests. The parent owns the node from here on.
void appendOwned(XML::Document &document, XML::Node *node)
{
    document.root()->appendChild(node);
    Inkscape::GC::release(node);
}

// Ordinary heap holder: only the proof and a boolean survive the helper. No
// external Node/Document or payload pointer becomes a conservative stack root.
struct RetainedProof
{
    std::unique_ptr<Proof> proof;
    bool shared_payload_while_owners_alive = false;
};

std::unique_ptr<RetainedProof> makeRetainedProof(std::string const &payload)
{
    auto result = std::make_unique<RetainedProof>();
    auto original = load("<svg><g/></svg>");
    if (!original) {
        return result;
    }
    original->root()->firstChild()->setAttribute("data-big", payload.c_str());

    OwnedDocument working(original->duplicate(nullptr));
    if (!working) {
        return result;
    }
    result->proof = Proof::capture(*working, "big.svg", "big-base");
    if (!result->proof) {
        return result;
    }
    // Compare addresses only while all native owners are alive; retain no raw
    // payload address across the forced collection in the calling test.
    auto const original_value = original->root()->firstChild()->attribute("data-big");
    auto const working_value = working->root()->firstChild()->attribute("data-big");
    auto const snapshot_value = result->proof->preparedDocument().root()->firstChild()->attribute("data-big");
    result->shared_payload_while_owners_alive = original_value &&
        original_value == working_value && working_value == snapshot_value;
    return result; // Both external creation anchors release through RAII.
}

// Static evidence that the public surface is const-only and exact.
using CaptureSignature =
    std::unique_ptr<Proof> (*)(XML::Document const &, char const *, char const *);
static_assert(std::is_same_v<decltype(&Proof::capture), CaptureSignature>,
              "capture must take a const source Document and nullable filename/base");

// 1. Read-only capture and match.
TEST(PreparedContentProof, ReadOnlyCaptureAndMatch)
{
    auto source = load("<svg><g id='a' data-z='1' class='c'/></svg>");
    ASSERT_TRUE(source);

    using PreparedDocumentResult =
        decltype(std::declval<Proof const &>().preparedDocument());
    static_assert(std::is_same_v<PreparedDocumentResult, XML::Document const &>,
                  "preparedDocument() must expose only a const Document view");

    std::string const before = rawFingerprint(*source);
    std::optional<uint64_t> const revision_before = source->contentRevision();
    ASSERT_TRUE(revision_before.has_value());

    auto proof = Proof::capture(*source, "file.svg", "base");
    ASSERT_TRUE(proof);
    EXPECT_TRUE(proof->matches(*source, "file.svg", "base"));

    // Repeated matches and a second capture leave the live source byte-identical
    // in raw structure/attribute order and on the same contentRevision.
    EXPECT_TRUE(proof->matches(*source, "file.svg", "base"));
    auto proof_again = Proof::capture(*source, "file.svg", "base");
    ASSERT_TRUE(proof_again);
    EXPECT_EQ(rawFingerprint(*source), before);
    EXPECT_EQ(source->contentRevision(), revision_before);

    // The owned snapshot is structurally identical and const.
    XML::Document const &prepared = proof->preparedDocument();
    EXPECT_EQ(rawFingerprint(prepared), before);
    EXPECT_TRUE(proof_again->matches(prepared, "file.svg", "base"));
}

// 2. Attribute order, equal byte buffers, and shared native payload.
TEST(PreparedContentProof, AttributeOrderAndEqualByteBuffers)
{
    auto ordered = load("<svg><g/></svg>");
    auto reordered = load("<svg><g/></svg>");
    ASSERT_TRUE(ordered && reordered);
    // Insert the same map in opposite native orders so the buffer order is
    // guaranteed, not merely parser-dependent.
    ordered->root()->firstChild()->setAttribute("a", "1");
    ordered->root()->firstChild()->setAttribute("b", "2");
    reordered->root()->firstChild()->setAttribute("b", "2");
    reordered->root()->firstChild()->setAttribute("a", "1");
    auto const &ordered_attrs = ordered->root()->firstChild()->attributeList();
    auto const &reordered_attrs = reordered->root()->firstChild()->attributeList();
    ASSERT_EQ(ordered_attrs.size(), 2u);
    ASSERT_EQ(reordered_attrs.size(), 2u);
    // The two sources really do differ in native attribute order.
    EXPECT_NE(ordered_attrs[0].key, reordered_attrs[0].key);

    auto ordered_proof = Proof::capture(*ordered, nullptr, nullptr);
    ASSERT_TRUE(ordered_proof);
    // Same attribute map in a different order is exactly equal.
    EXPECT_TRUE(ordered_proof->matches(*reordered, nullptr, nullptr));

    // Independently created equal values live at different native addresses...
    auto x = load("<svg><g/></svg>");
    auto y = load("<svg><g/></svg>");
    ASSERT_TRUE(x && y);
    x->root()->firstChild()->setAttribute("data-v", "same-value");
    y->root()->firstChild()->setAttribute("data-v", "same-value");
    char const *xv = x->root()->firstChild()->attribute("data-v");
    char const *yv = y->root()->firstChild()->attribute("data-v");
    ASSERT_NE(xv, nullptr);
    ASSERT_NE(yv, nullptr);
    EXPECT_NE(xv, yv);

    auto x_proof = Proof::capture(*x, nullptr, nullptr);
    ASSERT_TRUE(x_proof);
    // ...and still match through the byte comparison fallback.
    EXPECT_TRUE(x_proof->matches(*y, nullptr, nullptr));

    // The snapshot shares the source's immutable value buffer (native payload reuse).
    char const *snapshot_v = x_proof->preparedDocument().root()->firstChild()->attribute("data-v");
    EXPECT_EQ(snapshot_v, xv);
}

// 3. Exact node type and name/target identity.
TEST(PreparedContentProof, TypeAndNameDifferences)
{
    // Element name difference with identical content/shape.
    auto group = load("<svg><g id='n'/></svg>");
    auto rect = load("<svg><rect id='n'/></svg>");
    ASSERT_TRUE(group && rect);
    auto group_proof = Proof::capture(*group, nullptr, nullptr);
    ASSERT_TRUE(group_proof);
    EXPECT_FALSE(group_proof->matches(*rect, nullptr, nullptr));

    // PI target difference with identical PI content.
    auto pi_a = load("<svg/>");
    auto pi_b = load("<svg/>");
    ASSERT_TRUE(pi_a && pi_b);
    appendOwned(*pi_a, pi_a->createPI("target-a", "payload"));
    appendOwned(*pi_b, pi_b->createPI("target-b", "payload"));
    auto pi_proof = Proof::capture(*pi_a, nullptr, nullptr);
    ASSERT_TRUE(pi_proof);
    EXPECT_FALSE(pi_proof->matches(*pi_b, nullptr, nullptr));

    // Type isolation: a text node and a comment node forced to the same name,
    // content and shape differ only by node type.
    auto text_doc = load("<svg/>");
    auto comment_doc = load("<svg/>");
    ASSERT_TRUE(text_doc && comment_doc);
    auto text = text_doc->createTextNode("same");
    text->setCodeUnsafe(g_quark_from_static_string("shared-name"));
    appendOwned(*text_doc, text);
    auto comment = comment_doc->createComment("same");
    comment->setCodeUnsafe(g_quark_from_static_string("shared-name"));
    appendOwned(*comment_doc, comment);

    XML::Node *text_node = text_doc->root()->firstChild();
    XML::Node *comment_node = comment_doc->root()->firstChild();
    ASSERT_TRUE(text_node && comment_node);
    EXPECT_STREQ(text_node->name(), comment_node->name());
    EXPECT_STREQ(text_node->content(), comment_node->content());
    EXPECT_EQ(text_node->childCount(), comment_node->childCount());
    EXPECT_NE(text_node->type(), comment_node->type());

    auto text_proof = Proof::capture(*text_doc, nullptr, nullptr);
    ASSERT_TRUE(text_proof);
    EXPECT_FALSE(text_proof->matches(*comment_doc, nullptr, nullptr));
}

// CDATA participates in native output even though both subtypes are TEXT_NODE.
TEST(PreparedContentProof, TextCDataFlagIsExactAndPreserved)
{
    auto plain = load("<svg/>");
    auto cdata = load("<svg/>");
    ASSERT_TRUE(plain && cdata);
    appendOwned(*plain, plain->createTextNode("same < bytes", false));
    appendOwned(*cdata, cdata->createTextNode("same < bytes", true));
    auto const *plain_text = dynamic_cast<XML::TextNode const *>(plain->root()->firstChild());
    auto const *cdata_text = dynamic_cast<XML::TextNode const *>(cdata->root()->firstChild());
    ASSERT_TRUE(plain_text && cdata_text);
    ASSERT_EQ(plain_text->type(), cdata_text->type());
    ASSERT_STREQ(plain_text->name(), cdata_text->name());
    ASSERT_STREQ(plain_text->content(), cdata_text->content());
    ASSERT_FALSE(plain_text->is_CData());
    ASSERT_TRUE(cdata_text->is_CData());

    auto plain_proof = Proof::capture(*plain, nullptr, nullptr);
    auto cdata_proof = Proof::capture(*cdata, nullptr, nullptr);
    ASSERT_TRUE(plain_proof && cdata_proof);
    EXPECT_TRUE(plain_proof->matches(*plain, nullptr, nullptr));
    EXPECT_TRUE(cdata_proof->matches(*cdata, nullptr, nullptr));
    EXPECT_FALSE(plain_proof->matches(*cdata, nullptr, nullptr));
    EXPECT_FALSE(cdata_proof->matches(*plain, nullptr, nullptr));
    auto const *snapshot_text = dynamic_cast<XML::TextNode const *>(
        cdata_proof->preparedDocument().root()->firstChild());
    ASSERT_TRUE(snapshot_text);
    EXPECT_TRUE(snapshot_text->is_CData());
    EXPECT_EQ(snapshot_text->content(), cdata_text->content());
}

// 4. Ordered children and hierarchy shape.
TEST(PreparedContentProof, ChildOrderAndShape)
{
    auto order_a = load("<svg><g id='a'/><g id='b'/></svg>");
    auto order_b = load("<svg><g id='b'/><g id='a'/></svg>");
    ASSERT_TRUE(order_a && order_b);
    auto order_proof = Proof::capture(*order_a, nullptr, nullptr);
    ASSERT_TRUE(order_proof);
    EXPECT_FALSE(order_proof->matches(*order_b, nullptr, nullptr));

    auto shape_a = load("<svg><g id='a'><circle/></g></svg>");
    auto shape_b = load("<svg><g id='a'/><circle/></svg>");
    ASSERT_TRUE(shape_a && shape_b);
    auto shape_proof = Proof::capture(*shape_a, nullptr, nullptr);
    ASSERT_TRUE(shape_proof);
    EXPECT_FALSE(shape_proof->matches(*shape_b, nullptr, nullptr));

    // Comment and PI children participate in ordered shape, not just elements.
    auto mix_a = load("<svg><g id='a'/></svg>");
    auto mix_b = load("<svg><g id='a'/></svg>");
    ASSERT_TRUE(mix_a && mix_b);
    appendOwned(*mix_a, mix_a->createComment("c"));
    appendOwned(*mix_b, mix_b->createPI("t", "c"));
    auto mix_proof = Proof::capture(*mix_a, nullptr, nullptr);
    ASSERT_TRUE(mix_proof);
    EXPECT_FALSE(mix_proof->matches(*mix_b, nullptr, nullptr));
}

// 5. Attribute key/value presence and emptiness.
TEST(PreparedContentProof, AttributeKeysValuesMissingEmpty)
{
    // Missing attribute is distinct from an attribute holding the empty string.
    auto missing = load("<svg><g/></svg>");
    auto empty = load("<svg><g/></svg>");
    ASSERT_TRUE(missing && empty);
    empty->root()->firstChild()->setAttribute("id", "");
    auto missing_proof = Proof::capture(*missing, nullptr, nullptr);
    ASSERT_TRUE(missing_proof);
    EXPECT_FALSE(missing_proof->matches(*empty, nullptr, nullptr));

    // Same attribute count, different key.
    auto key_a = load("<svg><g a='1'/></svg>");
    auto key_b = load("<svg><g b='1'/></svg>");
    ASSERT_TRUE(key_a && key_b);
    auto key_proof = Proof::capture(*key_a, nullptr, nullptr);
    ASSERT_TRUE(key_proof);
    EXPECT_FALSE(key_proof->matches(*key_b, nullptr, nullptr));

    // Same key, different exact value.
    auto value_a = load("<svg><g a='1'/></svg>");
    auto value_b = load("<svg><g a='2'/></svg>");
    ASSERT_TRUE(value_a && value_b);
    auto value_proof = Proof::capture(*value_a, nullptr, nullptr);
    ASSERT_TRUE(value_proof);
    EXPECT_FALSE(value_proof->matches(*value_b, nullptr, nullptr));

    // Native insertion keeps the fixture exactly valid and matchable.
    auto native_doc = load("<svg><g/></svg>");
    ASSERT_TRUE(native_doc);
    native_doc->root()->firstChild()->setAttribute("id", "");
    auto native_proof = Proof::capture(*native_doc, nullptr, nullptr);
    ASSERT_TRUE(native_proof);
    EXPECT_TRUE(native_proof->matches(*native_doc, nullptr, nullptr));
    EXPECT_STREQ(native_doc->root()->firstChild()->attribute("id"), "");
}

// 6. Content nullness, emptiness and exact bytes.
TEST(PreparedContentProof, ContentNullEmptyAndBytes)
{
    auto null_doc = load("<svg><g/></svg>");
    auto empty_doc = load("<svg><g/></svg>");
    auto x_doc = load("<svg><g/></svg>");
    auto y_doc = load("<svg><g/></svg>");
    ASSERT_TRUE(null_doc && empty_doc && x_doc && y_doc);

    // Element content is compared exactly even though it is not serialised.
    null_doc->root()->firstChild()->setContent(nullptr);
    empty_doc->root()->firstChild()->setContent("");
    x_doc->root()->firstChild()->setContent("x");
    y_doc->root()->firstChild()->setContent("y");

    EXPECT_EQ(null_doc->root()->firstChild()->content(), nullptr);
    ASSERT_NE(empty_doc->root()->firstChild()->content(), nullptr);
    EXPECT_STREQ(empty_doc->root()->firstChild()->content(), "");

    auto null_proof = Proof::capture(*null_doc, nullptr, nullptr);
    ASSERT_TRUE(null_proof);
    EXPECT_TRUE(null_proof->matches(*null_doc, nullptr, nullptr));
    EXPECT_FALSE(null_proof->matches(*empty_doc, nullptr, nullptr));

    auto x_proof = Proof::capture(*x_doc, nullptr, nullptr);
    ASSERT_TRUE(x_proof);
    EXPECT_FALSE(x_proof->matches(*empty_doc, nullptr, nullptr));
    EXPECT_FALSE(x_proof->matches(*y_doc, nullptr, nullptr));
}

// 7. Owned filename/base identity.
TEST(PreparedContentProof, OwnedFilenameBaseIdentity)
{
    auto doc = load("<svg><g/></svg>");
    ASSERT_TRUE(doc);

    // Null and empty are distinct for both fields.
    auto none = Proof::capture(*doc, nullptr, nullptr);
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->matches(*doc, nullptr, nullptr));
    EXPECT_FALSE(none->matches(*doc, "", nullptr));
    EXPECT_FALSE(none->matches(*doc, nullptr, ""));
    EXPECT_FALSE(none->matches(*doc, "", ""));

    auto named = Proof::capture(*doc, "file.svg", "base");
    ASSERT_TRUE(named);
    EXPECT_TRUE(named->matches(*doc, "file.svg", "base"));
    EXPECT_FALSE(named->matches(*doc, "other.svg", "base"));
    EXPECT_FALSE(named->matches(*doc, "file.svg", "other-base"));
    EXPECT_FALSE(named->matches(*doc, nullptr, "base"));

    // Capture owns its inputs: changing caller-owned strings later must not
    // change proof identity.
    std::string filename = "owned.svg";
    std::string base = "owned-base";
    auto owned = Proof::capture(*doc, filename.c_str(), base.c_str());
    ASSERT_TRUE(owned);
    filename.assign("mutated.svg");
    base.assign("mutated-base");
    EXPECT_TRUE(owned->matches(*doc, "owned.svg", "owned-base"));
    EXPECT_FALSE(owned->matches(*doc, filename.c_str(), base.c_str()));
}

// 8. Non-canonical DOCUMENT fields are unavailable, never silently dropped.
TEST(PreparedContentProof, NoncanonicalDocumentUnavailable)
{
    // Ordinary document captures successfully.
    auto ordinary = load("<svg><g/></svg>");
    ASSERT_TRUE(ordinary);
    EXPECT_TRUE(Proof::capture(*ordinary, nullptr, nullptr));

    // SimpleDocument::duplicate does not copy the DOCUMENT node's own name.
    auto named = load("<svg><g/></svg>");
    ASSERT_TRUE(named);
    named->setCodeUnsafe(g_quark_from_static_string("not-xml"));
    EXPECT_EQ(Proof::capture(*named, nullptr, nullptr), nullptr);
    EXPECT_STREQ(named->name(), "not-xml");

    // ...nor its own content, including the empty string versus null.
    auto empty_content = load("<svg><g/></svg>");
    ASSERT_TRUE(empty_content);
    empty_content->setContent("");
    EXPECT_EQ(Proof::capture(*empty_content, nullptr, nullptr), nullptr);
    EXPECT_STREQ(empty_content->content(), "");

    auto content = load("<svg><g/></svg>");
    ASSERT_TRUE(content);
    content->setContent("x");
    EXPECT_EQ(Proof::capture(*content, nullptr, nullptr), nullptr);

    // ...nor its own attributes.
    auto attributed = load("<svg><g/></svg>");
    ASSERT_TRUE(attributed);
    attributed->setAttribute("data-doc", "1");
    EXPECT_EQ(Proof::capture(*attributed, nullptr, nullptr), nullptr);
    EXPECT_STREQ(attributed->attribute("data-doc"), "1");
}

// 9. Anchored payload survives destruction of the external native owners.
TEST(PreparedContentProof, RetainedLargePayloadAfterWorkingCopyDestroyed)
{
    std::string const payload(4u * 1024u * 1024u, 'Q'); // 4 MiB synthetic payload

    auto retained = makeRetainedProof(payload);
    ASSERT_TRUE(retained->proof);
    EXPECT_TRUE(retained->shared_payload_while_owners_alive);

    // The external original/working owners were released inside the helper. Force
    // a collection; the anchored proof must still expose the full payload.
    Inkscape::GC::Core::gcollect();

    char const *value = retained->proof->preparedDocument().root()->firstChild()->attribute("data-big");
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(std::strlen(value), payload.size());
    EXPECT_EQ(std::string(value), payload);

    // Fresh, independently generated native XML with the same payload still matches.
    auto expected = load("<svg><g/></svg>");
    ASSERT_TRUE(expected);
    expected->root()->firstChild()->setAttribute("data-big", payload.c_str());
    EXPECT_TRUE(retained->proof->matches(*expected, "big.svg", "big-base"));
    // No reclamation claim: conservative collection need not free the detached
    // working copy; only retention of the proof's bytes is asserted.
}

// 10. Later source mutation does not alias the retained proof.
TEST(PreparedContentProof, LaterSourceMutationDoesNotAliasProof)
{
    char const *const expected_xml = "<svg><g id='orig'><circle r='1'/></g></svg>";
    auto source = load(expected_xml);
    ASSERT_TRUE(source);

    auto proof = Proof::capture(*source, nullptr, nullptr);
    ASSERT_TRUE(proof);
    std::string const retained_before = rawFingerprint(proof->preparedDocument());

    // Mutate the live source in several native ways after capture.
    XML::Node *group = source->root()->firstChild();
    ASSERT_TRUE(group);
    group->setAttribute("id", "changed");
    group->setAttribute("data-temp", "v");
    group->removeAttribute("data-temp");
    group->setContent("content-added");
    XML::Node *circle = group->firstChild();
    ASSERT_TRUE(circle);
    group->removeChild(circle);
    appendOwned(*source, source->createElement("svg:rect"));

    // The proof no longer matches the changed source...
    EXPECT_FALSE(proof->matches(*source, nullptr, nullptr));
    // ...but its retained bytes are unchanged and still match an independent
    // fixture built from the original XML.
    EXPECT_EQ(rawFingerprint(proof->preparedDocument()), retained_before);
    auto old_fixture = load(expected_xml);
    ASSERT_TRUE(old_fixture);
    EXPECT_TRUE(proof->matches(*old_fixture, nullptr, nullptr));
}

} // namespace
