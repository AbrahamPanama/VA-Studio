// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Test for https://gitlab.com/inkscape/inkscape/-/issues/3393
 *//*
 *
 * Authors:
 *   Thomas Holder
 *
 * Copyright (C) 2022 Authors
 *
 * Released under GNU GPL version 2 or later, read the file 'COPYING' for more information
 */

#include <doc-per-case-test.h>
#include <gtest/gtest.h>
#include <src/object/object-set.h>
#include <src/object/sp-shape.h>
#include <src/object/sp-use.h>
#include <src/document-undo.h>
#include <src/live_effects/effect.h>
#include <src/live_effects/lpeobject.h>
#include <src/live_effects/lpeobject-reference.h>
#include <src/xml/repr.h>
#include <src/style.h>
#include <src/object/sp-lpe-item.h>

using namespace Inkscape;
using namespace std::literals;

constexpr auto docString = R"""(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg"
   xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <defs>
    <inkscape:path-effect effect="skeletal" copytype="repeated" 
       id="lpe1" pattern="M 0,0 5,5 0,10" />
  </defs>
  <path id="path1"
     inkscape:path-effect="#lpe1"
     inkscape:original-d="M 5,10 H 15"
     d="M 5,5 10,10 5,15 M 10,5 15,10 10,15" />
</svg>
)"""sv;

TEST_F(DocPerCaseTest, PathReverse)
{
    auto doc = SPDocument::createNewDocFromMem(docString);
    doc->ensureUpToDate();

    auto path1 = cast<SPShape>(doc->getObjectById("path1"));
    auto oset = ObjectSet(doc.get());
    oset.add(path1);

    ASSERT_EQ(path1->curve()->initialPoint(), Geom::Point(5, 5));

    oset.pathReverse();

    ASSERT_EQ(path1->curve()->initialPoint(), Geom::Point(15, 15));
}

namespace {
std::unique_ptr<SPDocument> removalDoc(std::string const &effects, std::string const &items)
{
    return SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg' "
        "xmlns:xlink='http://www.w3.org/1999/xlink' "
        "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'>"
        "<defs>" + effects + "</defs>" + items + "</svg>");
}

constexpr auto holder = "<path id='A' inkscape:path-effect='#fx' "
                        "inkscape:original-d='M 0,0 L 10,10' d='M 0,0 L 10,10'/>";
constexpr auto source = "<path id='S' d='M 20,0 L 20,20'/>";

void beginRemovalUndo(SPDocument *doc)
{
    DocumentUndo::done(doc, Util::Internal::ContextString("Fixture"), "");
    DocumentUndo::clearUndo(doc);
    DocumentUndo::clearRedo(doc);
}

void deleteSelection(SPDocument *doc, std::initializer_list<char const *> ids)
{
    ObjectSet selection(doc);
    for (auto id : ids) {
        selection.add(cast<SPItem>(doc->getObjectById(id)));
    }
    selection.deleteItems();
    DocumentUndo::done(doc, Util::Internal::ContextString("Delete"), "");
}
} // namespace

TEST_F(DocPerCaseTest, RemovalDuplicateFillBetweenStrokesUndoRedo)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='fill_between_strokes' "
                          "linkedpath='#S' secondpath='#S' lpeversion='1'/>", std::string(holder) + source);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    beginRemovalUndo(doc.get());
    int deleted = 0;
    doc->getObjectById("S")->connectDelete([&](SPObject *) { ++deleted; });
    deleteSelection(doc.get(), {"A"});
    EXPECT_EQ(doc->getObjectById("A"), nullptr);
    EXPECT_NE(doc->getObjectById("S"), nullptr);
    EXPECT_EQ(deleted, 0); // Both references name a user-owned source.
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    for (auto id : {"A", "S", "fx"}) EXPECT_NE(doc->getObjectById(id), nullptr) << id;
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_EQ(doc->getObjectById("A"), nullptr);
    EXPECT_NE(doc->getObjectById("S"), nullptr);
}

TEST_F(DocPerCaseTest, RemovalDuplicateFillBetweenManyKeepsSource)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='fill_between_many' "
                          "linkedpaths='#S,0,1;#S,0,1' lpeversion='1'/>", std::string(holder) + source);
    ASSERT_TRUE(doc);
    deleteSelection(doc.get(), {"A"});
    EXPECT_NE(doc->getObjectById("S"), nullptr);
}

TEST_F(DocPerCaseTest, RemovalCloneThenOriginalUndo)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='fill_between_strokes' "
                          "linkedpath='#S' secondpath='#S' lpeversion='1'/>",
                          std::string(holder) + source + "<use id='U' xlink:href='#A'/>");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto *effect = cast<SPLPEItem>(doc->getObjectById("A"))->getCurrentLPE();
    auto *use = cast<SPUse>(doc->getObjectById("U"));
    ASSERT_TRUE(use);
    auto *copy = cast<SPLPEItem>(use->firstChild());
    ASSERT_TRUE(copy);
    ASSERT_TRUE(copy->cloned);
    // A hidden copy really can be the last item to run this shared effect.
    effect->doBeforeEffect_impl(copy);
    beginRemovalUndo(doc.get());
    deleteSelection(doc.get(), {"U", "A"});
    EXPECT_EQ(doc->getObjectById("U"), nullptr);
    EXPECT_EQ(doc->getObjectById("A"), nullptr);
    EXPECT_NE(doc->getObjectById("S"), nullptr);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    for (auto id : {"A", "U", "S", "fx"}) EXPECT_NE(doc->getObjectById(id), nullptr) << id;
}

TEST_F(DocPerCaseTest, RemovalCloneOriginalLoopTerminates)
{
    auto doc = removalDoc(
        "<inkscape:path-effect id='fx' effect='clone_original' linkeditem='#B' method='none'/>"
        "<inkscape:path-effect id='fy' effect='clone_original' linkeditem='#A' method='none'/>",
        std::string(holder) + "<path id='B' inkscape:path-effect='#fy' "
        "inkscape:original-d='M 20,0 L 20,20' d='M 20,0 L 20,20'/>");
    ASSERT_TRUE(doc);
    deleteSelection(doc.get(), {"A"});
    EXPECT_EQ(doc->getObjectById("A"), nullptr);
    EXPECT_NE(doc->getObjectById("B"), nullptr);
}

TEST_F(DocPerCaseTest, RemovalSharedEffectKeepsOtherUserWorkingUndo)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='fill_between_strokes' "
                          "linkedpath='#S' secondpath='#S' lpeversion='1'/>",
        std::string(holder) + source + "<path id='B' inkscape:path-effect='#fx' "
        "inkscape:original-d='M 5,5 L 10,10' d='M 5,5 L 10,10'/>");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    beginRemovalUndo(doc.get());
    deleteSelection(doc.get(), {"A"});
    auto *b = cast<SPLPEItem>(doc->getObjectById("B"));
    ASSERT_TRUE(b);
    ASSERT_TRUE(b->getCurrentLPE());
    EXPECT_FALSE(b->getCurrentLPE()->getLPEObj()->deleted);
    EXPECT_NE(doc->getObjectById("S"), nullptr);
    // Changing the source must still update the surviving effect's geometry.
    auto *s = doc->getObjectById("S");
    ASSERT_TRUE(s);
    auto const before = std::string(b->getAttribute("d"));
    s->setAttribute("d", "M 40,0 L 40,40");
    doc->ensureUpToDate();
    sp_lpe_item_update_patheffect(b, true, true);
    EXPECT_NE(std::string(b->getAttribute("d")), before);
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Source edit"), "");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    for (auto id : {"A", "B", "S", "fx"}) EXPECT_NE(doc->getObjectById(id), nullptr) << id;
    EXPECT_FALSE(cast<SPLPEItem>(doc->getObjectById("A"))->getCurrentLPE()->getLPEObj()->deleted);
}

TEST_F(DocPerCaseTest, RemovalCloneOriginalKeepsUserSource)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='clone_original' "
                          "linkeditem='#S' method='originald'/>", std::string(holder) + source);
    ASSERT_TRUE(doc);
    auto const d = std::string(doc->getObjectById("S")->getAttribute("d"));
    deleteSelection(doc.get(), {"A"});
    auto *s = doc->getObjectById("S");
    ASSERT_TRUE(s);
    EXPECT_EQ(std::string(s->getAttribute("d")), d);
}

TEST_F(DocPerCaseTest, RemovalDuplicateGeneratedSatelliteDeletedOnceUndoRedo)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='copy_rotate' "
                          "lpesatellites='#S,1;#S,1' lpeversion='1'/>", std::string(holder) + source);
    ASSERT_TRUE(doc);
    beginRemovalUndo(doc.get());
    int deleted = 0;
    doc->getObjectById("S")->connectDelete([&](SPObject *) { ++deleted; });
    deleteSelection(doc.get(), {"A"});
    EXPECT_EQ(deleted, 1);
    EXPECT_EQ(doc->getObjectById("S"), nullptr);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    for (auto id : {"A", "S", "fx"}) EXPECT_NE(doc->getObjectById(id), nullptr) << id;
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_EQ(doc->getObjectById("A"), nullptr);
    EXPECT_EQ(doc->getObjectById("S"), nullptr);
}

// URIReference refuses a link that would close a loop (or name an ancestor of the
// holder), so mutually linked effects cannot be built from XML. Deleting the holder must
// still terminate and only touch objects its own effect lists.
TEST_F(DocPerCaseTest, RemovalMutualSatelliteLinksAreRefusedAndDeleteTerminates)
{
    auto doc = removalDoc(
        "<inkscape:path-effect id='fx' effect='copy_rotate' lpesatellites='#B,1;#A,1;#G,1'/>"
        "<inkscape:path-effect id='fy' effect='copy_rotate' lpesatellites='#A,1'/>",
        "<g id='G'>" + std::string(holder) + "<path id='B' inkscape:path-effect='#fy' "
        "inkscape:original-d='M 20,0 L 20,20' d='M 20,0 L 20,20'/></g>");
    ASSERT_TRUE(doc);
    auto *fx = cast<LivePathEffectObject>(doc->getObjectById("fx"));
    ASSERT_TRUE(fx);
    for (auto *satellite : fx->get_lpe()->effect_get_satellites()) {
        EXPECT_NE(satellite, doc->getObjectById("A"));
        EXPECT_NE(satellite, doc->getObjectById("G"));
    }
    deleteSelection(doc.get(), {"A"});
    EXPECT_EQ(doc->getObjectById("A"), nullptr);
    EXPECT_NE(doc->getObjectById("G"), nullptr);
}

TEST_F(DocPerCaseTest, RemovalSharedGeneratedSatelliteSurvivesUntilLastUser)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='copy_rotate' "
                          "lpesatellites='#S,1'/>", std::string(holder) + source +
        "<path id='B' inkscape:path-effect='#fx' inkscape:original-d='M 5,5 L 10,10' d='M 5,5 L 10,10'/>");
    ASSERT_TRUE(doc);
    beginRemovalUndo(doc.get());
    deleteSelection(doc.get(), {"A"});
    EXPECT_NE(doc->getObjectById("S"), nullptr);
    EXPECT_FALSE(cast<SPLPEItem>(doc->getObjectById("B"))->getCurrentLPE()->getLPEObj()->deleted);
    deleteSelection(doc.get(), {"B"});
    EXPECT_EQ(doc->getObjectById("S"), nullptr);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(doc->getObjectById("B"));
    EXPECT_FALSE(cast<SPLPEItem>(doc->getObjectById("B"))->getCurrentLPE()->getLPEObj()->deleted);
}

TEST_F(DocPerCaseTest, RemovalCloneReleaseClearsCachedItem)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='simplify'/>",
                          std::string(holder) + "<use id='U' xlink:href='#A'/>");
    ASSERT_TRUE(doc);
    auto *effect = cast<SPLPEItem>(doc->getObjectById("A"))->getCurrentLPE();
    auto *copy = cast<SPLPEItem>(doc->getObjectById("U")->firstChild());
    ASSERT_TRUE(copy);
    ASSERT_TRUE(copy->cloned);
    effect->doBeforeEffect_impl(copy);
    deleteSelection(doc.get(), {"U"});
    EXPECT_EQ(effect->sp_lpe_item, nullptr);
    EXPECT_EQ(effect->getCurrrentLPEItems().size(), 1);
}

TEST_F(DocPerCaseTest, RemovalEmptyReferenceListIsSafe)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='measure_segments'/>", source);
    ASSERT_TRUE(doc);
    auto *fx = cast<LivePathEffectObject>(doc->getObjectById("fx"));
    ASSERT_TRUE(fx);
    ASSERT_TRUE(fx->get_lpe());
    ASSERT_TRUE(fx->hrefList.empty());
    fx->get_lpe()->doOnRemove_impl(nullptr);
    fx->get_lpe()->processObjects(LivePathEffect::LPE_ERASE);
    EXPECT_NE(doc->getObjectById("S"), nullptr);
}

TEST_F(DocPerCaseTest, RemovalBooleanHiddenOperandIsCleanedUp)
{
    auto doc = removalDoc("<filter id='selectable_hidder_filter'/>"
                          "<inkscape:path-effect id='fx' effect='bool_op' "
                          "operand-path='#S' operation='union' lpeversion='1'/>",
                          std::string(holder) +
                          "<path id='S' d='M 20,0 L 20,20' style='filter:url(#selectable_hidder_filter)'/>");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    beginRemovalUndo(doc.get());
    deleteSelection(doc.get(), {"A"});
    EXPECT_EQ(doc->getObjectById("S"), nullptr);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    for (auto id : {"A", "S"}) EXPECT_NE(doc->getObjectById(id), nullptr) << id;
}

TEST_F(DocPerCaseTest, RemovalBooleanVisibleOperandIsKept)
{
    auto doc = removalDoc("<inkscape:path-effect id='fx' effect='bool_op' "
                          "operand-path='#S' operation='union' lpeversion='1'/>",
                          std::string(holder) + source);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    deleteSelection(doc.get(), {"A"});
    EXPECT_NE(doc->getObjectById("S"), nullptr);
}
