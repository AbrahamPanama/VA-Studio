// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Test for boolean operations with fill-rule as attributes
 *
 * https://gitlab.com/inkscape/inkscape/-/issues/5437
 */
/*
 * Authors:
 *   Thomas Holder
 *
 * Copyright (C) 2024 Authors
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <tuple>
#include "object/sp-lpe-item.h"
#include "object/sp-rect.h"
#include "object/sp-ellipse.h"
#include "xml/repr.h"

#include "doc-per-case-test.h"
#include "document-undo.h"
#include "object/object-set.h"
#include "object/sp-item.h"

using namespace Inkscape;
using namespace std::literals;

class BoolopAttrTest : public DocPerCaseTest
{
public:
    BoolopAttrTest()
    {
        constexpr auto docString = R"A(
<svg viewBox="0 0 210 110" xmlns="http://www.w3.org/2000/svg">
  <g id="union">
    <path fill-rule="evenodd" d="M 20,40 H 40 V 20 H 20 Z M 10,10 H 50 V 50 H 10 Z" />
    <path fill-rule="evenodd" d="M 70,20 H 90 V 40 H 70 Z M 60,10 h 40 V 50 H 60 Z" />
    <path fill-rule="nonzero" d="m 120,40 h 20 V 20 H 120 Z M 110,10 h 40 v 40 h -40 z " />
    <path fill-rule="nonzero" d="m 170,20 h 20 V 40 H 170 Z M 160,10 h 40 v 40 h -40 z " />
    <path style="fill-rule:evenodd" d="M 20,90 H 40 V 70 H 20 Z M 10,60 h 40 v 40 H 10 Z" />
    <path style="fill-rule:evenodd" d="M 70,70 H 90 V 90 H 70 Z M 60,60 h 40 v 40 H 60 Z" />
    <path style="fill-rule:nonzero" d="m 120,90 h 20 V 70 H 120 Z M 110,60 h 40 v 40 h -40 z " />
    <path style="fill-rule:nonzero" d="m 170,70 h 20 V 90 H 170 Z M 160,60 h 40 v 40 h -40 z " />
  </g>
</svg>
        )A"sv;
        doc = SPDocument::createNewDocFromMem(docString);
    }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(BoolopAttrTest, Union)
{
    auto const d_combined = //
        "M 10 10 L 10 50 L 50 50 L 50 10 L 10 10 z "
        "M 60 10 L 60 50 L 100 50 L 100 10 L 60 10 z "
        "M 110 10 L 110 50 L 150 50 L 150 10 L 110 10 z "
        "M 160 10 L 160 50 L 200 50 L 200 10 L 160 10 z "
        "M 20 20 L 40 20 L 40 40 L 20 40 L 20 20 z "
        "M 70 20 L 90 20 L 90 40 L 70 40 L 70 20 z "
        "M 120 20 L 140 20 L 140 40 L 120 40 L 120 20 z "
        "M 10 60 L 10 100 L 50 100 L 50 60 L 10 60 z "
        "M 60 60 L 60 100 L 100 100 L 100 60 L 60 60 z "
        "M 110 60 L 110 100 L 150 100 L 150 60 L 110 60 z "
        "M 160 60 L 160 100 L 200 100 L 200 60 L 160 60 z "
        "M 20 70 L 40 70 L 40 90 L 20 90 L 20 70 z "
        "M 70 70 L 90 70 L 90 90 L 70 90 L 70 70 z "
        "M 120 70 L 140 70 L 140 90 L 120 90 L 120 70 z ";

    auto const paths = doc->getObjectsBySelector("#union path");
    ASSERT_EQ(paths.size(), 8);

    auto object_set = ObjectSet(doc.get());
    object_set.setList(paths);
    object_set.pathUnion(true);

    auto combined = object_set.single();
    ASSERT_TRUE(combined);
    ASSERT_STREQ(combined->getAttribute("d"), d_combined);
}

TEST(BoolopReverseDifferenceTest, KeepsUpperMinusLower)
{
    constexpr auto doc_string = R"A(
<svg viewBox="0 0 150 100" xmlns="http://www.w3.org/2000/svg">
  <rect id="lower" x="0" y="0" width="100" height="100" />
  <rect id="upper" x="50" y="0" width="100" height="100" />
</svg>
        )A"sv;
    auto doc = SPDocument::createNewDocFromMem(doc_string);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();

    auto const operands = doc->getObjectsBySelector("rect");
    ASSERT_EQ(operands.size(), 2);

    ObjectSet object_set(doc.get());
    object_set.setList(operands);
    object_set.pathDiffReverse(true);

    auto result = object_set.singleItem();
    ASSERT_TRUE(result);
    auto const bounds = result->documentGeometricBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 100.0, 1e-6);
    EXPECT_NEAR(bounds->right(), 150.0, 1e-6);
    EXPECT_NEAR(bounds->top(), 0.0, 1e-6);
    EXPECT_NEAR(bounds->bottom(), 100.0, 1e-6);
}

TEST(BoolopMultiDifferenceTest, KeepsBottomMinusUnionOfAllOthers)
{
    constexpr auto doc_string = R"A(
<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
  <rect id="bottom" x="0" y="0" width="100" height="100" />
  <rect id="left" x="0" y="0" width="20" height="100" />
  <rect id="right" x="80" y="0" width="20" height="100" />
</svg>
        )A"sv;
    auto doc = SPDocument::createNewDocFromMem(doc_string);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();

    auto const objects = doc->getObjectsBySelector("rect");
    ASSERT_EQ(objects.size(), 3);

    ObjectSet object_set(doc.get());
    object_set.setList(std::vector<SPObject *>{objects[1], objects[2], objects[0]});
    object_set.pathDiffMany(false);

    auto result = object_set.singleItem();
    ASSERT_TRUE(result);
    auto const bounds = result->documentGeometricBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 20.0, 1e-6);
    EXPECT_NEAR(bounds->right(), 80.0, 1e-6);
    EXPECT_NEAR(bounds->top(), 0.0, 1e-6);
    EXPECT_NEAR(bounds->bottom(), 100.0, 1e-6);

    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(doc->getObjectsBySelector("rect").size(), 3);
}

TEST(BoolopMultiDifferenceTest, KeepsTopMinusUnionOfAllOthers)
{
    constexpr auto doc_string = R"A(
<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
  <rect id="left" x="0" y="0" width="20" height="100" />
  <rect id="right" x="80" y="0" width="20" height="100" />
  <rect id="top" x="0" y="0" width="100" height="100" />
</svg>
        )A"sv;
    auto doc = SPDocument::createNewDocFromMem(doc_string);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();

    auto const objects = doc->getObjectsBySelector("rect");
    ASSERT_EQ(objects.size(), 3);

    ObjectSet object_set(doc.get());
    object_set.setList(std::vector<SPObject *>{objects[2], objects[0], objects[1]});
    object_set.pathDiffMany(true);

    auto result = object_set.singleItem();
    ASSERT_TRUE(result);
    auto const bounds = result->documentGeometricBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 20.0, 1e-6);
    EXPECT_NEAR(bounds->right(), 80.0, 1e-6);
    EXPECT_NEAR(bounds->top(), 0.0, 1e-6);
    EXPECT_NEAR(bounds->bottom(), 100.0, 1e-6);

    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(doc->getObjectsBySelector("rect").size(), 3);
}

// These call the same ObjectSet entry points as BooleanAssistant::commit,
// without the command-line action's preliminary conversion to paths.
class BoolopLiveShapeTest : public DocPerCaseTest,
                          public testing::WithParamInterface<std::tuple<int, int, int, bool>> {};

TEST_P(BoolopLiveShapeTest, BakeEffectsAndSupportUndoRedo)
{
    auto [shape, operation, offset, reverse_selection] = GetParam();
    std::string native;
    if (shape == 0) {
        native = "sodipodi:type='rect' x='0' y='0' width='100' height='100'";
    } else if (shape == 1) {
        native = "sodipodi:type='arc' sodipodi:cx='50' sodipodi:cy='50' sodipodi:rx='50' sodipodi:ry='35'";
    } else {
        native = "inkscape:original-d='M0,0 H100 V100 H0 Z'";
    }
    auto svg = std::string(R"(<svg xmlns='http://www.w3.org/2000/svg'
        xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'
        xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' width='300' height='200'>
        <defs><inkscape:path-effect id='fx' effect='fillet_chamfer' lpeversion='1' is_visible='true'
        method='auto' flexible='false' radius='0' unit='px' mode='F'
        nodesatellites_param='F,0,0,1,0,5,100,1 @ F,0,0,1,0,5,100,1 @ F,0,0,1,0,5,100,1 @ F,0,0,1,0,5,100,1'/></defs>
        <path id='a' d='M0,0 H100 V100 H0 Z' transform='translate(5,5)' inkscape:path-effect='#fx' )") + native +
        "/><rect id='b' x='" + std::to_string(offset) + "' y='25' width='80' height='100'/></svg>";
    auto doc = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto a = cast<SPLPEItem>(doc->getObjectById("a"));
    ASSERT_TRUE(a);
    ASSERT_TRUE(a->hasPathEffect());
    ASSERT_EQ(is<SPRect>(a), shape == 0);
    ASSERT_EQ(is<SPGenericEllipse>(a), shape == 1);
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Boolean fixture"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    ObjectSet selection(doc.get());
    std::vector<SPObject *> objects{a, doc->getObjectById("b")};
    if (reverse_selection) std::reverse(objects.begin(), objects.end());
    selection.setList(objects);
    switch (operation) {
        case 0: selection.pathUnion(); break;
        case 1: selection.pathIntersect(); break;
        case 2: selection.pathDiff(); break;
        case 3: selection.pathDiffReverse(); break;
        case 4: selection.pathSymDiff(); break;
        case 5: selection.pathCut(); break;
        case 6: selection.pathSlice(); break;
        case 7: selection.pathDiffMany(false); break;
        case 8: selection.pathDiffMany(true); break;
    }
    doc->ensureUpToDate();
    for (auto item : selection.items()) {
        auto lpe = cast<SPLPEItem>(item);
        EXPECT_FALSE(lpe && lpe->hasPathEffect());
    }
    auto after = sp_repr_save_buf(doc->getReprDoc());
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    a = cast<SPLPEItem>(doc->getObjectById("a"));
    ASSERT_TRUE(a);
    EXPECT_TRUE(a->hasPathEffect());
    EXPECT_EQ(is<SPRect>(a), shape == 0);
    EXPECT_EQ(is<SPGenericEllipse>(a), shape == 1);
    EXPECT_TRUE(doc->getObjectById("b"));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), after);
}

INSTANTIATE_TEST_SUITE_P(NativeEffects, BoolopLiveShapeTest,
    testing::Combine(testing::Range(0, 3), testing::Range(0, 9),
                     testing::Values(0, 50, 150), testing::Bool()));

// P5: an operand that cannot take part must not leave the other operands
// permanently stripped of their live path effects, or add an Undo step.
class BoolopFailedOperandTest : public DocPerCaseTest, public testing::WithParamInterface<char const *> {};

TEST_P(BoolopFailedOperandTest, FailureAfterEffectOperandLeavesDocumentUntouched)
{
    auto const svg = std::string(R"(<svg xmlns='http://www.w3.org/2000/svg'
        xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' width='300' height='200'>
        <defs><inkscape:path-effect id='fx' effect='fillet_chamfer' lpeversion='1' is_visible='true'
        method='auto' flexible='false' radius='0' unit='px' mode='F'
        nodesatellites_param='F,0,0,1,0,5,100,1 @ F,0,0,1,0,5,100,1 @ F,0,0,1,0,5,100,1 @ F,0,0,1,0,5,100,1'/></defs>
        <path id='a' d='M0,0 H100 V100 H0 Z' inkscape:path-effect='#fx' inkscape:original-d='M0,0 H100 V100 H0 Z'/>
        <path id='b' d=')") + GetParam() + "'/></svg>";
    auto doc = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto a = cast<SPLPEItem>(doc->getObjectById("a"));
    ASSERT_TRUE(a);
    ASSERT_TRUE(a->hasPathEffect());
    ASSERT_TRUE(doc->getObjectById("b"));
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Boolean fixture"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    auto const before = sp_repr_save_buf(doc->getReprDoc());

    ObjectSet selection(doc.get());
    selection.setList(std::vector<SPObject *>{doc->getObjectById("a"), doc->getObjectById("b")});
    selection.pathUnion();
    doc->ensureUpToDate();

    auto const after = sp_repr_save_buf(doc->getReprDoc());
    EXPECT_EQ(after, before) << "BEFORE:\n" << before.raw() << "\nAFTER:\n" << after.raw();
    a = cast<SPLPEItem>(doc->getObjectById("a"));
    ASSERT_TRUE(a);
    EXPECT_TRUE(a->hasPathEffect());
    EXPECT_FALSE(DocumentUndo::undo(doc.get())) << "a failed boolean operation must not add an Undo step";
}

INSTANTIATE_TEST_SUITE_P(Operands, BoolopFailedOperandTest, testing::Values("", "M 5 5"));

namespace {
constexpr char const *kFilletDefs = R"(<defs><inkscape:path-effect id='fx' effect='fillet_chamfer' lpeversion='1' is_visible='true'
        method='auto' flexible='false' radius='0' unit='px' mode='F'
        nodesatellites_param='F,0,0,1,0,5,100,1 @ F,0,0,1,0,5,100,1 @ F,0,0,1,0,5,100,1 @ F,0,0,1,0,5,100,1'/>
        <inkscape:path-effect id='co' effect='clone_original' lpeversion='1.1' is_visible='true'
        linkeditem='#src' method='d' attributes='' css_properties='' allow_transforms='false'/></defs>)";

std::string boolop_svg(std::string const &body)
{
    return std::string(R"(<svg xmlns='http://www.w3.org/2000/svg'
        xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' width='300' height='200'>)") +
           kFilletDefs + body + "</svg>";
}
} // namespace

class BoolopEffectOperand : public DocPerCaseTest {};

// An operand whose own path is empty but whose live effect generates geometry
// (clone original) must still take part in the operation.
TEST_F(BoolopEffectOperand, EmptyOwnPathWithGeneratingEffectStillUnions)
{
    auto doc = SPDocument::createNewDocFromMem(boolop_svg(
        "<path id='src' d='M0,0 H50 V50 H0 Z' transform='translate(0,100)'/>"
        "<path id='clone' d='' inkscape:path-effect='#co' inkscape:original-d=''/>"
        "<rect id='r' x='100' y='0' width='40' height='40'/>"));
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto clone = cast<SPLPEItem>(doc->getObjectById("clone"));
    ASSERT_TRUE(clone);
    ASSERT_TRUE(clone->hasPathEffect());
    sp_lpe_item_update_patheffect(clone, true, true);
    doc->ensureUpToDate();
    // Precondition: the effect, not the clone's own path, provides the geometry.
    char const *generated = clone->getRepr()->attribute("d");
    ASSERT_TRUE(generated && *generated) << "clone_original did not generate geometry";
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"fixture"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());

    ObjectSet selection(doc.get());
    selection.setList(std::vector<SPObject *>{doc->getObjectById("clone"), doc->getObjectById("r")});
    selection.pathUnion();
    doc->ensureUpToDate();

    auto result = selection.singleItem();
    ASSERT_TRUE(result) << "union of an effect-generated operand must produce a result";
    EXPECT_FALSE(doc->getObjectById("r"));
    auto bounds = result->documentGeometricBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 0.0, 1e-3);
    EXPECT_NEAR(bounds->right(), 140.0, 1e-3);
    EXPECT_NEAR(bounds->bottom(), 150.0, 1e-3);
    EXPECT_TRUE(DocumentUndo::undo(doc.get()));
}

// Backstop: the second effect operand bakes to nothing, after the first was
// already baked. Only this call's changes roll back; the caller's own pending
// (uncommitted) change survives, and no Undo step is added by the failure.
TEST_F(BoolopEffectOperand, PostBakeFailureRollsBackOnlyThisCallsChanges)
{
    auto doc = SPDocument::createNewDocFromMem(boolop_svg(
        "<path id='mark' d='M0,0 H1 V1 Z'/>"
        "<path id='a' d='M0,0 H100 V100 H0 Z' inkscape:path-effect='#fx' inkscape:original-d='M0,0 H100 V100 H0 Z'/>"
        "<path id='b' d='M5,5' inkscape:path-effect='#fx' inkscape:original-d='M5,5'/>"));
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    ASSERT_TRUE(cast<SPLPEItem>(doc->getObjectById("a"))->hasPathEffect());
    ASSERT_TRUE(cast<SPLPEItem>(doc->getObjectById("b"))->hasPathEffect());
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"fixture"}, "");
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    auto const original_before = std::string(doc->getObjectById("a")->getRepr()->attribute("inkscape:original-d"));

    // The caller's open transaction: one pending, uncommitted change.
    doc->getObjectById("mark")->getRepr()->setAttribute("data-pending", "yes");

    ObjectSet selection(doc.get());
    selection.setList(std::vector<SPObject *>{doc->getObjectById("a"), doc->getObjectById("b")});
    selection.pathUnion(true /* skip_undo: the caller owns the transaction */);
    doc->ensureUpToDate();

    ASSERT_TRUE(doc->getObjectById("a"));
    ASSERT_TRUE(doc->getObjectById("b"));
    EXPECT_TRUE(cast<SPLPEItem>(doc->getObjectById("a"))->hasPathEffect());
    EXPECT_TRUE(cast<SPLPEItem>(doc->getObjectById("b"))->hasPathEffect());
    EXPECT_EQ(std::string(doc->getObjectById("a")->getRepr()->attribute("inkscape:original-d")), original_before);
    auto const *pending = doc->getObjectById("mark")->getRepr()->attribute("data-pending");
    ASSERT_NE(pending, nullptr) << "the caller's pending change was rolled back";
    EXPECT_STREQ(pending, "yes");

    // The caller still commits its own change as exactly one Undo step.
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"caller step"}, "");
    EXPECT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(doc->getObjectById("mark")->getRepr()->attribute("data-pending"), nullptr);
    EXPECT_TRUE(cast<SPLPEItem>(doc->getObjectById("a"))->hasPathEffect());
    EXPECT_FALSE(DocumentUndo::undo(doc.get())) << "the failed operation added no Undo step of its own";
}
