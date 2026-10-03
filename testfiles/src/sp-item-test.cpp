// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * SPGroup test
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL version 2 or later, read the file 'COPYING' for more information
 */

#include <gtest/gtest.h>

#include <2geom/affine.h>
#include <2geom/pathvector.h>

#include "document.h"
#include "document-undo.h"
#include "enums.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "svg/svg.h"

using namespace Inkscape;
using namespace std::literals;

namespace {

constexpr auto image_data =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJ"
    "AAAADUlEQVQIHWP4z8DwHwAFgAI/ScLx9QAAAABJRU5ErkJggg==";

std::unique_ptr<SPDocument> make_image_document(char const *preserve_aspect_ratio = nullptr)
{
    auto const aspect = preserve_aspect_ratio
        ? Glib::ustring::compose(R"( preserveAspectRatio="%1")", preserve_aspect_ratio)
        : Glib::ustring{};
    auto const svg = Glib::ustring::compose(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <image id="image" x="3" y="5" width="10" height="20"%1 href="%2"/>
</svg>)svg",
        aspect, image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    if (document) document->ensureUpToDate();
    return document;
}

void expect_affine_near(Geom::Affine const &actual, Geom::Affine const &expected)
{
    for (unsigned i = 0; i < 6; ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1e-12) << "affine component " << i;
    }
}

} // namespace

class SPItemTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // setup hidden dependency
        if (!Application::exists()) Application::create(false);
    }
};

TEST_F(SPItemTest, ImagePreserveAspectRatioRetainsNonUniformTransform)
{
    auto document = make_image_document();
    ASSERT_TRUE(document);
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);
    ASSERT_NE(image->aspect_align, SP_ASPECT_NONE);

    Geom::Affine const stretch(2.0, 0.0, 0.0, 0.5, 7.0, 11.0);
    image->doWriteTransform(stretch, nullptr, false);
    document->ensureUpToDate();

    EXPECT_DOUBLE_EQ(image->x.computed, 3.0);
    EXPECT_DOUBLE_EQ(image->y.computed, 5.0);
    EXPECT_DOUBLE_EQ(image->width.computed, 10.0);
    EXPECT_DOUBLE_EQ(image->height.computed, 20.0);
    expect_affine_near(image->transform, stretch);
    EXPECT_NE(image->getRepr()->attribute("transform"), nullptr);
}

TEST_F(SPItemTest, ImagePreserveAspectRatioNoneStillAbsorbsNonUniformTransform)
{
    auto document = make_image_document("none");
    ASSERT_TRUE(document);
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);
    ASSERT_EQ(image->aspect_align, SP_ASPECT_NONE);

    Geom::Affine const stretch(2.0, 0.0, 0.0, 0.5, 7.0, 11.0);
    image->doWriteTransform(stretch, nullptr, false);
    document->ensureUpToDate();

    EXPECT_DOUBLE_EQ(image->x.computed, 13.0);
    EXPECT_DOUBLE_EQ(image->y.computed, 13.5);
    EXPECT_DOUBLE_EQ(image->width.computed, 20.0);
    EXPECT_DOUBLE_EQ(image->height.computed, 10.0);
    EXPECT_TRUE(image->transform.isIdentity());
    EXPECT_EQ(image->getRepr()->attribute("transform"), nullptr);
}

TEST_F(SPItemTest, ImagePreserveAspectRatioStillAbsorbsUniformTransform)
{
    auto document = make_image_document();
    ASSERT_TRUE(document);
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    Geom::Affine const scale(2.0, 0.0, 0.0, 2.0, 7.0, 11.0);
    image->doWriteTransform(scale, nullptr, false);
    document->ensureUpToDate();

    EXPECT_DOUBLE_EQ(image->x.computed, 13.0);
    EXPECT_DOUBLE_EQ(image->y.computed, 21.0);
    EXPECT_DOUBLE_EQ(image->width.computed, 20.0);
    EXPECT_DOUBLE_EQ(image->height.computed, 40.0);
    EXPECT_TRUE(image->transform.isIdentity());
}

TEST_F(SPItemTest, ImageNonUniformTransformRoundTripsThroughUndoRedo)
{
    auto document = make_image_document();
    ASSERT_TRUE(document);
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    Geom::Affine const stretch(0.4, 0.0, 0.0, 1.0, 12.0, 0.0);
    image->doWriteTransform(stretch, nullptr, false);
    DocumentUndo::done(document.get(),
                       Util::Internal::ContextString{"Stretch image"},
                       "transform-scale");
    document->ensureUpToDate();
    expect_affine_near(image->transform, stretch);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    auto undone = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(undone);
    EXPECT_TRUE(undone->transform.isIdentity());
    EXPECT_DOUBLE_EQ(undone->width.computed, 10.0);
    EXPECT_DOUBLE_EQ(undone->height.computed, 20.0);

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    auto redone = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(redone);
    expect_affine_near(redone->transform, stretch);
    EXPECT_DOUBLE_EQ(redone->width.computed, 10.0);
    EXPECT_DOUBLE_EQ(redone->height.computed, 20.0);
}

TEST_F(SPItemTest, getClipPathVector)
{
    constexpr auto svg = R"""(<?xml version="1.0"?>
<svg width="100" height="100">
  <defs id="defs1">
    <clipPath clipPathUnits="userSpaceOnUse" id="clipPath1">
      <rect id="cliprect1" width="34.33456" height="33.829079" x="13.165109" y="13.165109" transform="translate(10,10)" />
    </clipPath>
    <clipPath clipPathUnits="userSpaceOnUse" id="clipPath2">
      <rect id="cliprect2" width="33.794209" height="33.794209" x="0" y="0" transform="translate(10,10)" />
    </clipPath>
    <clipPath clipPathUnits="userSpaceOnUse" id="clipPath3">
      <rect id="cliprect3" width="30.837675" height="30.837675" x="0" y="0" transform="translate(10,10)" />
    </clipPath>
  </defs>
  <g id="group1" transform="translate(10,10)" clip-path="url(#clipPath1)">
    <g id="group2" transform="translate(10,10)" clip-path="url(#clipPath2)">
      <rect id="rect1" x="0" y="0" width="50" height="50" clip-path="url(#clipPath3)" style="fill: blue" />
    </g>
  </g>
  <g id="group3" transform="translate(-10,-10)" clip-path="url(#clipPath1)">
    <rect id="rect2" x="0" y="0" width="50" height="50" style="fill: red" />
  </g>
</svg>)"""sv;

    auto doc = SPDocument::createNewDocFromMem(svg);

    // This has to be run or all the path vectors are empty.
    doc->ensureUpToDate();

    // Item with no clip.
    auto no_item = cast<SPItem>(doc->getObjectById("rect2"));
    ASSERT_FALSE(no_item->getClipPathVector().has_value());
    
    auto parent = cast<SPItem>(doc->getObjectById("group3"));
    auto pathv1 = no_item->getClipPathVector(parent);
    auto pathv2 = parent->getClipPathVector();
    ASSERT_FALSE(pathv1->empty());
    ASSERT_FALSE(pathv2->empty());
    ASSERT_EQ(sp_svg_write_path(*pathv1), sp_svg_write_path(*pathv2));

    auto r_item = cast<SPItem>(doc->getObjectById("rect1"));
    auto pathv3 = r_item->getClipPathVector();
    ASSERT_EQ(sp_svg_write_path(*pathv3), "M 10,10 H 40.837675 V 40.837675 H 10 Z");

    auto r_parent = cast<SPItem>(doc->getObjectById("group1"));
    auto pathv4 = r_item->getClipPathVector(r_parent);
    ASSERT_EQ(sp_svg_write_path(*pathv4), "M 13.16601563,13.16601563 V 40.83789062 H 40.83789062 V 13.16601563 Z");
}
