// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * SPGroup test
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2020 Authors
 *
 * Released under GNU GPL version 2 or later, read the file 'COPYING' for more information
 */

#include <gtest/gtest.h>

#include "document.h"
#include "document-undo.h"
#include "object/object-set.h"
#include "object/sp-clippath.h"
#include "object/sp-defs.h"
#include "object/sp-mask.h"
#include "object/sp-shape.h"
#include "preferences.h"
#include "display/cairo-utils.h"
#include "display/drawing.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <span>
#include <functional>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cstring>
#include "inkscape.h"
#include "live_effects/effect.h"
#include "object/sp-item-group.h"
#include "object/sp-lpe-item.h"
#include "object/sp-root.h"
#include "xml/repr.h"
#include <string>

using namespace Inkscape;
using namespace Inkscape::LivePathEffect;
using namespace std::literals;

class SPGroupTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // setup hidden dependency
        Application::create(false);
    }
};

TEST_F(SPGroupTest, applyingPowerClipEffectToGroupWithoutClipIsIgnored)
{
    constexpr auto svg = R"A(
<svg width='100' height='100'>
    <g id='group1'>
        <rect id='rect1' width='100' height='50' />
        <rect id='rect2' y='50' width='100' height='50' />
    </g>
</svg>)A"sv;

    auto doc = SPDocument::createNewDocFromMem(svg);

    auto group = cast<SPGroup>(doc->getObjectById("group1"));
    Effect::createAndApply(POWERCLIP, doc.get(), group);

    ASSERT_FALSE(group->hasPathEffect());
}


TEST_F(SPGroupTest, NestedSvgViewportBoundsAgreeWithNativeLeafAndRootQueries)
{
    struct Case { char const *par; double left, top, width, height; };
    for (auto const &c : {
            Case{"none", 23, 26, 288, 96},
            Case{"xMidYMid meet", 95, 26, 144, 96},
            Case{"xMidYMid slice", 23, -22, 288, 192}}) {
        SCOPED_TRACE(c.par);
        auto xml = std::string(
            "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='200' viewBox='0 0 200 100'>"
            "<g id='wrapper' transform='matrix(1.5 0 0 .5 7 11)'>"
            "<svg id='viewport' x='3' y='4' width='96' height='96' "
            "viewBox='-10 -20 48 96' preserveAspectRatio='") + c.par +
            "'><rect id='leaf' x='-10' y='-20' width='48' height='96'/></svg></g></svg>";
        auto doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
        ASSERT_TRUE(doc);
        auto verify = [&](SPDocument &d) {
            d.ensureUpToDate();
            // Direct viewport/leaf queries already traverse c2p via i2anc_affine.
            // Wrapper/root aggregation must produce the same result, once only.
            for (auto id : {"wrapper", "viewport", "leaf"}) {
                SCOPED_TRACE(id);
                auto item = cast<SPItem>(d.getObjectById(id));
                ASSERT_TRUE(item);
                for (auto type : {SPItem::GEOMETRIC_BBOX, SPItem::VISUAL_BBOX}) {
                    auto box = item->documentBounds(type);
                    ASSERT_TRUE(box);
                    EXPECT_NEAR(box->left(), c.left, 1e-8);
                    EXPECT_NEAR(box->top(), c.top, 1e-8);
                    EXPECT_NEAR(box->width(), c.width, 1e-8);
                    EXPECT_NEAR(box->height(), c.height, 1e-8);
                }
            }
            auto box = d.getRoot()->documentVisualBounds();
            ASSERT_TRUE(box);
            EXPECT_NEAR(box->left(), c.left, 1e-8);
            EXPECT_NEAR(box->top(), c.top, 1e-8);
            EXPECT_NEAR(box->width(), c.width, 1e-8);
            EXPECT_NEAR(box->height(), c.height, 1e-8);
        };
        verify(*doc);
        auto saved = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto reopened = SPDocument::createNewDocFromMem(std::span<char const>(saved.data(), saved.size()));
        ASSERT_TRUE(reopened);
        verify(*reopened);
    }
}

TEST_F(SPGroupTest, NestedSvgBoundsComposeAcrossTwoViewportsAndOrdinaryGroups)
{
    constexpr auto xml = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='1000' height='1000'>
  <g id='wrapper' transform='translate(7 11)'>
    <svg x='3' y='4' width='80' height='60' viewBox='-10 -20 40 20' preserveAspectRatio='none'>
      <g transform='translate(5 6)'>
        <svg x='1' y='2' width='20' height='10' viewBox='-4 -2 10 5' preserveAspectRatio='none'>
          <rect id='leaf' x='-4' y='-2' width='10' height='5'/>
        </svg>
      </g>
    </svg>
  </g>
</svg>)SVG"sv;
    auto doc = SPDocument::createNewDocFromMem(xml);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    // Inner viewport: (1,2)..(21,12); group: (6,8)..(26,18);
    // outer viewport: (35,88)..(75,118); wrapper: (42,99)..(82,129).
    for (auto id : {"wrapper", "leaf"}) {
        SCOPED_TRACE(id);
        auto item = cast<SPItem>(doc->getObjectById(id));
        ASSERT_TRUE(item);
        auto box = item->documentVisualBounds();
        ASSERT_TRUE(box);
        EXPECT_NEAR(box->left(), 42, 1e-8);
        EXPECT_NEAR(box->top(), 99, 1e-8);
        EXPECT_NEAR(box->width(), 40, 1e-8);
        EXPECT_NEAR(box->height(), 30, 1e-8);
    }
}


namespace {
// Independent of i2anc_affine(): expected rectangles below are hand-computed,
// and pixels come from the live native Drawing, not a bounds-driven crop.
struct PixelRect {
    int left, top, width, height;
};

void expect_parent_bounds_and_pixels(SPDocument &doc, SPGroup &parent,
                                     Drawing &drawing, PixelRect expected)
{
    doc.ensureUpToDate();
    for (auto type : {SPItem::GEOMETRIC_BBOX, SPItem::VISUAL_BBOX}) {
        auto box = parent.documentBounds(type);
        ASSERT_TRUE(box);
        EXPECT_NEAR(box->left(), expected.left, 1e-8);
        EXPECT_NEAR(box->top(), expected.top, 1e-8);
        EXPECT_NEAR(box->width(), expected.width, 1e-8);
        EXPECT_NEAR(box->height(), expected.height, 1e-8);
    }

    drawing.update();
    // Fixed, uncropped surface: never derive the render area from the bbox
    // being tested. Integer edges and solid fill permit exact alpha checks.
    constexpr int width = 192, height = 160;
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, width, height);
    {
        DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
        DrawingContext context(target);
        context.setOperator(CAIRO_OPERATOR_CLEAR);
        context.paint();
        context.setOperator(CAIRO_OPERATOR_OVER);
        drawing.render(context, Geom::IntRect::from_xywh(0, 0, width, height));
        ASSERT_EQ(cairo_status(context.raw()), CAIRO_STATUS_SUCCESS);
    }
    surface->flush();
    ASSERT_EQ(cairo_surface_status(surface->cobj()), CAIRO_STATUS_SUCCESS);

    int left = width, top = height, right = -1, bottom = -1;
    unsigned painted = 0, alpha_mismatches = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint32_t pixel;
            std::memcpy(&pixel, surface->get_data() + y * surface->get_stride() + 4 * x, sizeof(pixel));
            auto alpha = pixel >> 24; // Cairo ARGB32 is native-endian.
            bool inside = x >= expected.left && x < expected.left + expected.width &&
                          y >= expected.top && y < expected.top + expected.height;
            alpha_mismatches += alpha != (inside ? 255u : 0u);
            if (alpha) {
                ++painted;
                left = std::min(left, x); top = std::min(top, y);
                right = std::max(right, x); bottom = std::max(bottom, y);
            }
        }
    }
    ASSERT_GT(painted, 0u);
    EXPECT_EQ(left, expected.left);
    EXPECT_EQ(top, expected.top);
    EXPECT_EQ(right + 1 - left, expected.width);
    EXPECT_EQ(bottom + 1 - top, expected.height);
    EXPECT_EQ(painted, static_cast<unsigned>(expected.width * expected.height));
    EXPECT_EQ(alpha_mismatches, 0u); // No threshold, edge allowance or tolerance.
}

// Declared after Drawing and before assertions: every exit hides native views
// before Drawing/document destruction, including a fatal expectation in a test.
struct HideGroupTestView {
    SPRoot *root;
    unsigned key;
    ~HideGroupTestView() { root->invoke_hide(key); }
};
}

TEST_F(SPGroupTest, LiveNestedSvgTranslationAtOneAndTwoTimesViewportMatchesDrawing)
{
    for (int scale : {1, 2}) {
        SCOPED_TRACE(scale);
        auto xml = std::string(
            "<svg xmlns='http://www.w3.org/2000/svg' width='192' height='160'>"
            "<g id='parent'><svg id='viewport' x='0' y='0' width='") +
            std::to_string(20 * scale) + "' height='" + std::to_string(20 * scale) +
            "' viewBox='0 0 20 20' preserveAspectRatio='none'>"
            "<rect width='10' height='6' fill='#ff0000' stroke='none'/>"
            "</svg></g></svg>";
        auto doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        auto parent = cast<SPGroup>(doc->getObjectById("parent"));
        auto viewport = cast<SPRoot>(doc->getObjectById("viewport"));
        ASSERT_TRUE(parent);
        ASSERT_TRUE(viewport);
        EXPECT_FALSE(viewport->getRepr()->attribute("transform"));
        EXPECT_DOUBLE_EQ(viewport->c2p[0], scale);
        EXPECT_DOUBLE_EQ(viewport->c2p[3], scale);

        Drawing drawing;
        auto key = SPItem::display_key_new(1);
        auto shown = doc->getRoot()->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
        HideGroupTestView hide{doc->getRoot(), key};
        ASSERT_TRUE(shown);
        drawing.setRoot(shown);
        expect_parent_bounds_and_pixels(*doc, *parent, drawing, {0, 0, 10 * scale, 6 * scale});

        // Set after build: SPRoot::build clears initially parsed transforms.
        // Keep the existing Drawing alive so native update/invalidation is real.
        viewport->getRepr()->setAttribute("transform", "translate(30,20)");
        expect_parent_bounds_and_pixels(*doc, *parent, drawing, {30, 20, 10 * scale, 6 * scale});
        EXPECT_DOUBLE_EQ(viewport->transform[4], 30);
        EXPECT_DOUBLE_EQ(viewport->transform[5], 20);
        EXPECT_DOUBLE_EQ(viewport->c2p[0], scale);
        EXPECT_DOUBLE_EQ(viewport->c2p[3], scale);

        viewport->getRepr()->removeAttribute("transform");
        expect_parent_bounds_and_pixels(*doc, *parent, drawing, {0, 0, 10 * scale, 6 * scale});
        EXPECT_DOUBLE_EQ(viewport->transform[4], 0);
        EXPECT_DOUBLE_EQ(viewport->transform[5], 0);
    }
}

TEST_F(SPGroupTest, LiveNestedSvgTransformFollowsViewportAndPrecedesOrdinaryParent)
{
    constexpr auto xml = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='192' height='160'>
  <g id='parent' transform='matrix(2 0 0 3 7 11)'>
    <svg id='viewport' x='3' y='4' width='40' height='40'
         viewBox='0 0 20 20' preserveAspectRatio='none'>
      <rect width='10' height='6' fill='#ff0000' stroke='none'/>
    </svg>
  </g>
</svg>)SVG"sv;
    auto doc = SPDocument::createNewDocFromMem(xml);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto parent = cast<SPGroup>(doc->getObjectById("parent"));
    auto viewport = cast<SPRoot>(doc->getObjectById("viewport"));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(viewport);

    Drawing drawing;
    auto key = SPItem::display_key_new(1);
    auto shown = doc->getRoot()->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
    HideGroupTestView hide{doc->getRoot(), key};
    ASSERT_TRUE(shown);
    drawing.setRoot(shown);
    // Leaf (0,0)..(10,6) -> viewport (3,4)..(23,16)
    // -> ordinary parent (13,23)..(53,59).
    expect_parent_bounds_and_pixels(*doc, *parent, drawing, {13, 23, 40, 36});

    viewport->getRepr()->setAttribute("transform", "translate(30,10)");
    // Viewport -> live translation (33,14)..(53,26)
    // -> ordinary parent (73,53)..(113,89).
    // Swapping viewport/live order gives left/top 133/83;
    // moving the live translation after the parent gives 43/33.
    expect_parent_bounds_and_pixels(*doc, *parent, drawing, {73, 53, 40, 36});

    viewport->getRepr()->setAttribute("transform", "translate(10,20)");
    expect_parent_bounds_and_pixels(*doc, *parent, drawing, {33, 83, 40, 36});

    viewport->getRepr()->removeAttribute("transform");
    expect_parent_bounds_and_pixels(*doc, *parent, drawing, {13, 23, 40, 36});
}


// ---------------------------------------------------------------------------
// Ungroup / Ungroup All must keep nested PowerClips (ST-N, N1/N2/N3).
//
// Operation mode: ungroup is a group operation on one selection with one Undo
// step; a clipped group passes its clip to each child, a child that already
// has its own clip or mask keeps it (wrapped), and Ungroup All keeps the
// groups it cannot dissolve without changing the picture.
namespace {
constexpr int kRasterSize = 200;

std::vector<std::uint8_t> render_document(SPDocument &doc)
{
    doc.ensureUpToDate();
    Drawing drawing;
    auto key = SPItem::display_key_new(1);
    auto shown = doc.getRoot()->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
    HideGroupTestView hide{doc.getRoot(), key};
    EXPECT_TRUE(shown);
    std::vector<std::uint8_t> pixels;
    if (!shown) {
        return pixels;
    }
    drawing.setRoot(shown);
    drawing.update();
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, kRasterSize, kRasterSize);
    {
        DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
        DrawingContext context(target);
        context.setOperator(CAIRO_OPERATOR_CLEAR);
        context.paint();
        context.setOperator(CAIRO_OPERATOR_OVER);
        drawing.render(context, Geom::IntRect::from_xywh(0, 0, kRasterSize, kRasterSize));
    }
    surface->flush();
    for (int y = 0; y < kRasterSize; ++y) {
        auto const *row = surface->get_data() + y * surface->get_stride();
        pixels.insert(pixels.end(), row, row + 4 * kRasterSize);
    }
    return pixels;
}

unsigned painted_pixels(std::vector<std::uint8_t> const &pixels)
{
    unsigned count = 0;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        count += pixels[i + 3] != 0;
    }
    return count;
}

// Shapes that draw: not inside <defs> or a <clipPath>/<mask>.
std::vector<std::string> visible_shape_ids(SPObject *root)
{
    std::vector<std::string> ids;
    std::function<void(SPObject *)> walk = [&](SPObject *obj) {
        if (is<SPDefs>(obj) || is<SPClipPath>(obj) || is<SPMask>(obj)) {
            return;
        }
        if (is<SPShape>(obj)) {
            ids.emplace_back(obj->getId() ? obj->getId() : "");
        }
        for (auto &child : obj->children) {
            walk(&child);
        }
    };
    walk(root);
    std::sort(ids.begin(), ids.end());
    return ids;
}

struct PrefGuard {
    PrefGuard(char const *path, bool value) : path(path)
    {
        auto prefs = Preferences::get();
        old = prefs->getBool(path, true);
        prefs->setBool(path, value);
    }
    ~PrefGuard() { Preferences::get()->setBool(path, old); }
    char const *path;
    bool old;
};

// clipPaths without inkscape:collect, as CorelDRAW imports have them.
constexpr auto kNestedClipSvg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'>
  <defs>
    <clipPath id='C1'><rect id='c1r' x='0' y='0' width='100' height='100'/></clipPath>
    <clipPath id='C2'><rect id='c2r' x='0' y='0' width='50' height='200'/></clipPath>
    <clipPath id='C3'><rect id='c3r' x='0' y='0' width='40' height='40'/></clipPath>
  </defs>
  <g id='outer' clip-path='url(#C1)' transform='translate(10 10)'>
    <rect id='a' x='0' y='0' width='150' height='150' fill='#ff0000' clip-path='url(#C2)'/>
    <rect id='b' x='60' y='60' width='120' height='120' fill='#0000ff'/>
    <g id='inner' clip-path='url(#C3)'>
      <rect id='c' x='-20' y='-20' width='80' height='80' fill='#00aa00'/>
    </g>
  </g>
</svg>)SVG"sv;

constexpr auto kMultiChildClipSvg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'>
  <defs>
    <clipPath id='CM'>
      <rect id='cm1' x='0' y='0' width='40' height='40'/>
      <rect id='cm2' x='100' y='100' width='60' height='60'/>
    </clipPath>
  </defs>
  <g id='outer' clip-path='url(#CM)'>
    <rect id='a' x='0' y='0' width='200' height='200' fill='#ff0000'/>
    <rect id='b' x='20' y='20' width='160' height='160' fill='#0000ff'/>
  </g>
</svg>)SVG"sv;

enum class UngroupKind { One, All };

void ungroup_keeps_picture(std::string_view svg, UngroupKind kind, std::vector<std::string> const &expected_shapes)
{
    auto doc = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(doc);
    auto const before = render_document(*doc);
    ASSERT_GT(painted_pixels(before), 0u);
    auto const xml_before = sp_repr_save_buf(doc->getReprDoc());
    ASSERT_EQ(visible_shape_ids(doc->getRoot()), expected_shapes);

    ObjectSet set(doc.get());
    set.add(doc->getObjectById("outer"));
    ASSERT_EQ(set.size(), 1);
    if (kind == UngroupKind::One) {
        set.ungroup();
    } else {
        set.ungroup_all();
    }
    EXPECT_EQ(doc->getObjectById("outer"), nullptr) << "the selected group is dissolved";
    EXPECT_EQ(visible_shape_ids(doc->getRoot()), expected_shapes) << "no clip shape became a visible object";
    EXPECT_EQ(render_document(*doc), before) << "the picture must not change";

    // One Undo restores the original document.
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_NE(doc->getObjectById("outer"), nullptr);
    EXPECT_EQ(render_document(*doc), before);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml_before);
    EXPECT_FALSE(DocumentUndo::undo(doc.get())) << "ungroup must be exactly one Undo step";
    // Redo gives the ungrouped result again.
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_EQ(doc->getObjectById("outer"), nullptr);
    EXPECT_EQ(render_document(*doc), before);
}
} // namespace

TEST_F(SPGroupTest, UngroupKeepsAChildsOwnClipUnderTheGroupsClip)
{
    ungroup_keeps_picture(kNestedClipSvg, UngroupKind::One, {"a", "b", "c"});
}

TEST_F(SPGroupTest, UngroupAllKeepsAChildsOwnClipUnderTheGroupsClip)
{
    ungroup_keeps_picture(kNestedClipSvg, UngroupKind::All, {"a", "b", "c"});
}

TEST_F(SPGroupTest, UngroupKeepsAMultiShapeClipPath)
{
    ungroup_keeps_picture(kMultiChildClipSvg, UngroupKind::One, {"a", "b"});
}

TEST_F(SPGroupTest, UngroupAllKeepsAMultiShapeClipPath)
{
    ungroup_keeps_picture(kMultiChildClipSvg, UngroupKind::All, {"a", "b"});
}

TEST_F(SPGroupTest, UngroupAllKeepsAClippedGroupWhenClipsAreNotPreservedOnUngroup)
{
    // /options/maskobject/maskonungroup is the preference that decides whether
    // ungrouping keeps clips; with it off a nested clip would be discarded.
    constexpr auto svg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'>
  <defs><clipPath id='C1'><rect x='0' y='0' width='50' height='50'/></clipPath></defs>
  <g id='outer'>
    <g id='clipped' clip-path='url(#C1)'><rect id='a' x='0' y='0' width='150' height='150' fill='#ff0000'/></g>
    <rect id='b' x='100' y='100' width='60' height='60' fill='#0000ff'/>
  </g>
</svg>)SVG"sv;
    PrefGuard guard("/options/maskobject/maskonungroup", false);
    auto doc = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(doc);
    auto const before = render_document(*doc);
    ObjectSet set(doc.get());
    set.add(doc->getObjectById("outer"));
    set.ungroup_all();
    EXPECT_EQ(doc->getObjectById("outer"), nullptr);
    auto *clipped = doc->getObjectById("clipped");
    ASSERT_NE(clipped, nullptr) << "the clipped group is kept, not silently dissolved";
    EXPECT_STREQ(clipped->getAttribute("clip-path"), "url(#C1)");
    EXPECT_EQ(render_document(*doc), before) << "no nested clip was discarded";
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_NE(doc->getObjectById("outer"), nullptr);
    EXPECT_EQ(render_document(*doc), before);
}

TEST_F(SPGroupTest, MoveToLayerRefusesADestinationInsideTheSelection)
{
    constexpr auto svg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'
     width='200' height='200'>
  <g id='L1' inkscape:groupmode='layer'>
    <rect id='r1' x='0' y='0' width='40' height='40'/>
    <g id='L2' inkscape:groupmode='layer'><rect id='r2' x='50' y='0' width='40' height='40'/></g>
  </g>
  <g id='plain'><rect id='r3' x='0' y='100' width='40' height='40'/></g>
</svg>)SVG"sv;
    auto doc = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const xml_before = sp_repr_save_buf(doc->getReprDoc());
    for (auto destination : {"L1", "L2"}) {
        SCOPED_TRACE(destination);
        ObjectSet set(doc.get());
        set.add(doc->getObjectById("L1"));
        auto *target = doc->getObjectById(destination);
        ASSERT_NE(target, nullptr);
        set.toLayer(target);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml_before) << "document unchanged";
        EXPECT_FALSE(DocumentUndo::undo(doc.get())) << "no Undo step";
        EXPECT_TRUE(set.includes(doc->getObjectById("L1")));
    }
    // A legitimate move still works.
    ObjectSet set(doc.get());
    set.add(doc->getObjectById("r3"));
    set.toLayer(doc->getObjectById("L2"));
    EXPECT_EQ(set.size(), 1);
    EXPECT_EQ(set.items().front()->parent, doc->getObjectById("L2"));
}

// N4: Ungroup All on a deep clipped tree must stay fast and complete.
TEST_F(SPGroupTest, UngroupAllOfADeepClippedTreeFinishesQuicklyAndKeepsEveryObject)
{
    constexpr int levels = 10, per_level = 50;
    for (bool clip_every_level : {true, false}) {
        SCOPED_TRACE(clip_every_level);
        std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'><defs>";
        for (int l = 0; l < levels; ++l) {
            svg += "<clipPath id='C" + std::to_string(l) + "'><rect x='0' y='0' width='200' height='200'/></clipPath>";
        }
        svg += "</defs>";
        for (int l = 0; l < levels; ++l) {
            svg += "<g id='g" + std::to_string(l) + "'";
            if (clip_every_level || l == 0) {
                svg += " clip-path='url(#C" + std::to_string(l) + ")'";
            }
            svg += ">";
            for (int i = 0; i < per_level; ++i) {
                svg += "<rect id='r" + std::to_string(l) + "_" + std::to_string(i) + "' x='" + std::to_string(i) +
                       "' y='" + std::to_string(l * 4) + "' width='3' height='3' fill='#ff0000'/>";
            }
        }
        for (int l = 0; l < levels; ++l) {
            svg += "</g>";
        }
        svg += "</svg>";
        auto doc = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
        ASSERT_TRUE(doc);
        auto const before = render_document(*doc);
        ObjectSet set(doc.get());
        set.add(doc->getObjectById("g0"));
        auto const start = std::chrono::steady_clock::now();
        set.ungroup_all();
        auto const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "[ST-N N4] clip_every_level=" << clip_every_level << " ungroup_all of "
                  << levels * per_level << " objects: " << seconds << " s\n";
        EXPECT_EQ(visible_shape_ids(doc->getRoot()).size(), size_t(levels * per_level));
        EXPECT_EQ(render_document(*doc), before);
        EXPECT_LT(seconds, 30.0);
    }
}
