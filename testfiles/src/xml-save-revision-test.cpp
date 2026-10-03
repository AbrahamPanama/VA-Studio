// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Tests for XML::Document::contentRevision(): a read-only, non-persistent,
 * monotonic owner-thread XML content generation for snapshot save validation.
 *
 * Design contract under test:
 *  - every successful native XML mutation advances the generation, whether or
 *    not it happens inside an Undo transaction;
 *  - unchanged setters (whose native notification is suppressed) do not;
 *  - commit/rollback/Undo-replay never restore an older generation;
 *  - the value is not serialized and reading it changes nothing;
 *  - equality proves no notified mutation; a change may include reverted or
 *    detached work and never by itself establishes a visual difference.
 *
 * Scope of a stamp: the generation tracks one document's own mutation timeline,
 * so a stamp is only meaningful compared with another stamp read from the SAME
 * live XML::Document. Equality or inequality between stamps from independently
 * constructed documents is never content identity.
 *
 * All source-mutation checks here use a raw, const, recursive structural
 * fingerprint over the live tree (native getters only, never the serializer):
 * sp_repr_save_buf may clean/sort the live XML through preferences, so it cannot
 * prove that a read or the fingerprinting itself left the source unchanged. The
 * one test that must serialize does so only on independent copies.
 *
 * The generation is intentionally not exposed as an exact mutation count: one
 * broad action may publish several native notifications. These tests therefore
 * assert monotonic advance/equality, never a specific delta.
 *
 * The saturating uint64_t-exhaustion branch has no public test backdoor and is
 * reviewed from source only (see deepseek-work/all-plans-20260922/xml-save-revision).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <glib.h>

#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/event-fns.h"
#include "xml/node-observer.h"
#include "xml/repr.h"

namespace {

namespace XML = Inkscape::XML;

using Revision = std::optional<uint64_t>;
using Log = std::unique_ptr<XML::Event, decltype(&sp_repr_free_log)>;

std::shared_ptr<XML::Document> load(char const *xml)
{
    return std::shared_ptr<XML::Document>(sp_repr_read_buf(xml, SP_SVG_NS_URI));
}

// Raw, nonmutating XML snapshot: node type, name, content, every attribute in
// its actual order (both name and value length-prefixed so concatenation stays
// unambiguous), then children in document order. Uses only const native getters
// and deliberately never serializes or lays out the tree.
void fingerprintNode(XML::Node const *node, std::string &out)
{
    auto field = [&](char const *text) {
        std::string_view value = text ? text : "";
        out += std::to_string(value.size()) + ":";
        out.append(value);
    };
    out += "[" + std::to_string(static_cast<int>(node->type()));
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

// duplicate() hands back a GC-anchored Document with its own anchor reference.
// Release that anchor through the GC, matching the adjacent native tests that
// own a duplicate() result (e.g. destructive-bitmap-clip-test.cpp).
void releaseDocument(XML::Document *document)
{
    Inkscape::GC::release(document);
}

void expectAdvanced(Revision before, Revision after)
{
    EXPECT_TRUE(before.has_value());
    EXPECT_TRUE(after.has_value());
    if (before && after) {
        EXPECT_GT(*after, *before);
    }
}

TEST(XmlSaveRevision, ReadingStampDoesNotMutateDocumentOrBytes)
{
    auto doc = load("<svg><g id='art'/></svg>");
    ASSERT_TRUE(doc);

    auto const content_before = rawFingerprint(*doc);
    auto const revision_before = doc->contentRevision();
    ASSERT_TRUE(revision_before.has_value());

    // Repeated reads are non-mutating and deterministic.
    EXPECT_EQ(doc->contentRevision(), revision_before);
    EXPECT_EQ(doc->contentRevision(), revision_before);

    // The raw tree is untouched and the generation did not advance, so reading
    // the stamp is not itself a mutation.
    EXPECT_EQ(rawFingerprint(*doc), content_before);
    EXPECT_EQ(doc->contentRevision(), revision_before);
}

TEST(XmlSaveRevision, NewDocumentsHaveIndependentCounters)
{
    auto a = load("<svg><g/></svg>");
    auto b = load("<svg><g/></svg>");
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(a->contentRevision().has_value());
    ASSERT_TRUE(b->contentRevision().has_value());
    // A stamp only describes its own document's timeline. These two documents
    // are built from the same literal (a known identical construction), so their
    // independent counters happen to agree now; that equality is not evidence of
    // content identity.
    EXPECT_EQ(*a->contentRevision(), *b->contentRevision());

    auto a_before = a->contentRevision();
    auto b_before = b->contentRevision();
    a->root()->setAttribute("data-independent", "x");
    // Same-document proof that a's timeline advanced...
    expectAdvanced(a_before, a->contentRevision());
    // ...and that b's timeline is untouched, which is what "independent" means.
    EXPECT_EQ(b->contentRevision(), b_before);
    // Different counters on different documents; again, not content identity.
    EXPECT_NE(a->contentRevision(), b->contentRevision());
}

TEST(XmlSaveRevision, AttributeAddChangeRemoveAndUnchangedAdvanceOnlyOnChange)
{
    auto doc = load("<svg><g id='art'/></svg>");
    auto node = doc->root()->firstChild();
    ASSERT_TRUE(node);

    auto r0 = doc->contentRevision();
    node->setAttribute("data-a", "1");
    auto r1 = doc->contentRevision();
    expectAdvanced(r0, r1);

    node->setAttribute("data-a", "2"); // real change
    auto r2 = doc->contentRevision();
    expectAdvanced(r1, r2);

    node->setAttribute("data-a", "2"); // unchanged -> native notification suppressed
    EXPECT_EQ(doc->contentRevision(), r2);

    node->removeAttribute("data-a");
    auto r3 = doc->contentRevision();
    expectAdvanced(r2, r3);
    EXPECT_EQ(node->attribute("data-a"), nullptr);
}

TEST(XmlSaveRevision, ContentMutationsForTextCommentAndPi)
{
    auto doc = load("<svg><g/></svg>");
    auto root = doc->root();
    ASSERT_TRUE(root);

    auto text = doc->createTextNode("hello");
    root->appendChild(text);
    auto r_add = doc->contentRevision();
    text->setContent("world");
    auto r_text = doc->contentRevision();
    expectAdvanced(r_add, r_text);

    auto comment = doc->createComment("c1");
    root->appendChild(comment);
    auto r_comment_before = doc->contentRevision();
    comment->setContent("c2");
    auto r_comment_after = doc->contentRevision();
    expectAdvanced(r_comment_before, r_comment_after);

    auto pi = doc->createPI("xml-stylesheet", "href='a'");
    root->appendChild(pi);
    auto r_pi_before = doc->contentRevision();
    pi->setContent("href='b'");
    auto r_pi_after = doc->contentRevision();
    expectAdvanced(r_pi_before, r_pi_after);

    // These nodes are now parent-owned; the creation anchor is dropped once at
    // the end, as in the adjacent native tests.
    Inkscape::GC::release(text);
    Inkscape::GC::release(comment);
    Inkscape::GC::release(pi);
}

TEST(XmlSaveRevision, ElementRenameAdvances)
{
    auto doc = load("<svg><g id='art'/></svg>");
    auto node = doc->root()->firstChild();
    ASSERT_TRUE(node);

    auto r0 = doc->contentRevision();
    node->setCodeUnsafe(g_quark_from_string("svg:rect"));
    auto r1 = doc->contentRevision();
    expectAdvanced(r0, r1);
    EXPECT_STREQ(node->name(), "svg:rect");
}

TEST(XmlSaveRevision, ChildAddRemoveReorderAdvance)
{
    auto doc = load("<svg><g id='a'/><g id='b'/><g id='c'/></svg>");
    auto root = doc->root();
    auto first = root->firstChild();
    auto second = first->next();
    auto third = second->next();
    ASSERT_TRUE(first && second && third);

    auto r0 = doc->contentRevision();
    auto extra = doc->createElement("svg:path");
    root->appendChild(extra);
    auto r1 = doc->contentRevision();
    expectAdvanced(r0, r1);

    root->removeChild(extra);
    auto r2 = doc->contentRevision();
    expectAdvanced(r1, r2);
    Inkscape::GC::release(extra);

    // The fingerprint encodes child order, so a reorder is directly visible
    // without serializing the live tree.
    auto ordered_before = rawFingerprint(*doc);
    root->changeOrder(third, nullptr); // move last to first
    auto r3 = doc->contentRevision();
    expectAdvanced(r2, r3);
    EXPECT_NE(rawFingerprint(*doc), ordered_before);
    EXPECT_EQ(root->firstChild(), third);
}

TEST(XmlSaveRevision, TransactionCommitAndRollbackDoNotResetRevision)
{
    auto doc = load("<svg><g id='art'/></svg>");
    auto node = doc->root()->firstChild();
    ASSERT_TRUE(node);

    auto r0 = doc->contentRevision();
    doc->beginTransaction();
    node->setAttribute("data-x", "1");
    auto r1 = doc->contentRevision();
    expectAdvanced(r0, r1);
    doc->commit();
    EXPECT_EQ(doc->contentRevision(), r1); // commit does not reset

    doc->beginTransaction();
    node->setAttribute("data-y", "2");
    auto r2 = doc->contentRevision();
    expectAdvanced(r1, r2);
    doc->rollback();
    auto r3 = doc->contentRevision();
    expectAdvanced(r2, r3); // rollback's reverse notifications only advance
    EXPECT_EQ(node->attribute("data-y"), nullptr);
    ASSERT_TRUE(doc->contentRevision().has_value());
}

TEST(XmlSaveRevision, UndoAndReplayDoNotRestoreRevision)
{
    auto doc = load("<svg><g id='art'/></svg>");
    auto node = doc->root()->firstChild();
    ASSERT_TRUE(node);

    doc->beginTransaction();
    node->setAttribute("data-z", "9");
    auto after_mutation = doc->contentRevision();
    Log log(doc->commitUndoable(), sp_repr_free_log);
    ASSERT_TRUE(log);
    auto after_commit = doc->contentRevision();
    EXPECT_EQ(after_commit, after_mutation);

    sp_repr_undo_log(log.get());
    auto after_undo = doc->contentRevision();
    expectAdvanced(after_commit, after_undo);
    EXPECT_EQ(node->attribute("data-z"), nullptr);

    sp_repr_replay_log(log.get());
    auto after_replay = doc->contentRevision();
    expectAdvanced(after_undo, after_replay);
    EXPECT_STREQ(node->attribute("data-z"), "9");
}

TEST(XmlSaveRevision, MutationObserverReadsAdvancedStamp)
{
    auto doc = load("<svg><g/></svg>");
    auto node = doc->root()->firstChild();
    ASSERT_TRUE(node);

    struct Observer : XML::NodeObserver {
        XML::Document &doc;
        XML::Node &node;
        Revision baseline;
        bool saw_advanced = false;
        Observer(XML::Document &doc, XML::Node &node) : doc(doc), node(node) { node.addObserver(*this); }
        ~Observer() override { node.removeObserver(*this); }
        void notifyAttributeChanged(XML::Node &, GQuark, Inkscape::Util::ptr_shared,
                                    Inkscape::Util::ptr_shared) override {
            auto now = doc.contentRevision();
            saw_advanced = baseline.has_value() && now.has_value() && *now > *baseline;
        }
    };

    Observer observer(*doc, *node);
    observer.baseline = doc->contentRevision();
    node->setAttribute("data-observer", "seen");
    // The logger advances before observer callbacks, so the observer sees it.
    EXPECT_TRUE(observer.saw_advanced);
}

TEST(XmlSaveRevision, DetachedNodeMutationAdvancesDocumentRevision)
{
    auto doc = load("<svg><g/></svg>");
    auto detached = doc->createElement("svg:path");
    ASSERT_TRUE(detached);

    auto r0 = doc->contentRevision();
    detached->setAttribute("id", "detached-1");
    auto r1 = doc->contentRevision();
    expectAdvanced(r0, r1);
    Inkscape::GC::release(detached);
}

TEST(XmlSaveRevision, DuplicateProducesIndependentCounterWithoutTouchingOriginal)
{
    auto doc = load("<svg><g id='art'/></svg>");
    ASSERT_TRUE(doc);
    auto original_content = rawFingerprint(*doc);
    auto original_revision = doc->contentRevision();
    ASSERT_TRUE(original_revision.has_value());

    std::unique_ptr<XML::Document, decltype(&releaseDocument)> dup(doc->duplicate(nullptr), &releaseDocument);
    ASSERT_TRUE(dup);

    // Duplication does not mutate the original's raw content or generation.
    EXPECT_EQ(doc->contentRevision(), original_revision);
    EXPECT_EQ(rawFingerprint(*doc), original_content);

    auto dup_before = dup->contentRevision();
    dup->root()->setAttribute("data-dup", "1");
    auto dup_after = dup->contentRevision();
    expectAdvanced(dup_before, dup_after);
    EXPECT_EQ(doc->contentRevision(), original_revision);
}

TEST(XmlSaveRevision, RevisionIsNotSerialized)
{
    auto a = load("<svg><g id='art'/></svg>");
    auto b = load("<svg><g id='art'/></svg>");
    ASSERT_TRUE(a && b);

    auto const a_content_before = rawFingerprint(*a);
    auto const b_content_before = rawFingerprint(*b);
    auto const a_revision_before = a->contentRevision();
    auto const b_revision_before = b->contentRevision();
    ASSERT_TRUE(a_revision_before.has_value());
    ASSERT_TRUE(b_revision_before.has_value());

    // Serialization is only used on independent copies, so the live originals
    // are never serialized/normalized. The copies are GC-released, matching the
    // adjacent native tests.
    std::unique_ptr<XML::Document, decltype(&releaseDocument)> a_copy(a->duplicate(nullptr), &releaseDocument);
    std::unique_ptr<XML::Document, decltype(&releaseDocument)> b_copy(b->duplicate(nullptr), &releaseDocument);
    ASSERT_TRUE(a_copy && b_copy);

    // Advance a_copy's generation while returning its XML content to the same
    // shape as b_copy. This comparison stays within a single document's timeline.
    auto const a_copy_revision_before = a_copy->contentRevision();
    a_copy->root()->firstChild()->setAttribute("data-temp", "x");
    a_copy->root()->firstChild()->removeAttribute("data-temp");
    expectAdvanced(a_copy_revision_before, a_copy->contentRevision());
    ASSERT_TRUE(a_copy->contentRevision().has_value());
    ASSERT_TRUE(b_copy->contentRevision().has_value());
    // The copies now hold different generations. That difference belongs to
    // their separate timelines and is not a content-identity statement.
    EXPECT_NE(a_copy->contentRevision(), b_copy->contentRevision());

    // Equal serialized bytes despite different generations show the generation
    // is not persisted. This is serialization evidence only, never source-
    // mutation evidence.
    EXPECT_EQ(sp_repr_save_buf(a_copy.get()), sp_repr_save_buf(b_copy.get()));

    // The originals' raw content and generations were untouched throughout.
    EXPECT_EQ(rawFingerprint(*a), a_content_before);
    EXPECT_EQ(rawFingerprint(*b), b_content_before);
    EXPECT_EQ(a->contentRevision(), a_revision_before);
    EXPECT_EQ(b->contentRevision(), b_revision_before);
}

TEST(XmlSaveRevision, AtomicBatchAdvancesPerChangeWithoutExactDeltaAssumption)
{
    auto doc = load("<svg><g/></svg>");
    auto node = doc->root()->firstChild();
    ASSERT_TRUE(node);

    auto r0 = doc->contentRevision();
    ASSERT_TRUE(node->setAttributesAtomically({{"a", "1"}, {"b", "2"}, {"c", "3"}}));
    auto r1 = doc->contentRevision();
    expectAdvanced(r0, r1);
    // Deliberately no assertion that the delta is exactly 3: one broad action may
    // publish several notifications, and the counter is not a per-action count.

    node->setAttribute("a", "1"); // unchanged -> suppressed, no advance
    EXPECT_EQ(doc->contentRevision(), r1);
}

} // namespace
