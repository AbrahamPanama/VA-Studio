// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Combination style and object testing for cascading and flags.
 *//*
 *
 * Authors:
 *   Martin Owens
 *
 * Copyright (C) 2018 Authors
 *
 * Released under GNU GPL version 2 or later, read the file 'COPYING' for more information
 */

#include <vector>

#include <gtest/gtest.h>
#include <doc-per-case-test.h>

#include <src/style.h>
#include <src/object/sp-root.h>
#include <src/object/sp-rect.h>
#include <src/xml/repr.h>

using namespace Inkscape;
using namespace Inkscape::XML;
using namespace std::literals;

class ObjectTest: public DocPerCaseTest {
public:
    ObjectTest() {
        constexpr auto docString = R"A(
<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>
<style>
rect { fill: #808080; opacity:0.5; }
.extra { opacity:1.0; }
.overload { fill: #d0d0d0 !important; stroke: #c0c0c0 !important; }
.font { font: italic bold 12px/30px Georgia, serif; }
.exsize { stroke-width: 1ex; }
.fosize { font-size: 15px; }
</style>
<g style='fill:blue; stroke-width:2px;font-size: 14px;'>
  <rect id='one' style='fill:red; stroke:green;'/>
  <rect id='two' style='stroke:green; stroke-width:4px;'/>
  <rect id='three' class='extra' style='fill: #cccccc;'/>
  <rect id='four' class='overload' style='fill:green;stroke:red !important;'/>
  <rect id='five' class='font' style='font: 15px arial, sans-serif;'/>/
  <rect id='six' style='stroke-width:1em;'/>
  <rect id='seven' class='exsize'/>
  <rect id='eight' class='fosize' style='stroke-width: 50%;'/>
</g>
</svg>)A"sv;
        doc = SPDocument::createNewDocFromMem(docString);
        doc->ensureUpToDate();
    }

    std::unique_ptr<SPDocument> doc;
};

/*
 * Test basic cascade values, that they are set correctly as we'd want to see them.
 */
TEST_F(ObjectTest, Styles) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    SPRoot *root = doc->getRoot();
    ASSERT_TRUE(root->getRepr() != nullptr);
    ASSERT_TRUE(root->hasChildren());

    auto one = cast<SPRect>(doc->getObjectById("one"));
    ASSERT_TRUE(one != nullptr);

    EXPECT_EQ(one->style->fill.get_value(), Glib::ustring("red"));
    EXPECT_EQ(one->style->stroke.get_value(), Glib::ustring("green"));
    EXPECT_EQ(one->style->opacity.get_value(), Glib::ustring("0.5"));
    EXPECT_EQ(one->style->stroke_width.get_value(), Glib::ustring("2px"));

    auto two = cast<SPRect>(doc->getObjectById("two"));
    ASSERT_TRUE(two != nullptr);

    EXPECT_EQ(two->style->fill.get_value(), Glib::ustring("#808080"));
    EXPECT_EQ(two->style->stroke.get_value(), Glib::ustring("green"));
    EXPECT_EQ(two->style->opacity.get_value(), Glib::ustring("0.5"));
    EXPECT_EQ(two->style->stroke_width.get_value(), Glib::ustring("4px"));

    auto three = cast<SPRect>(doc->getObjectById("three"));
    ASSERT_TRUE(three != nullptr);

    EXPECT_EQ(three->style->fill.get_value(), Glib::ustring("#cccccc"));
    EXPECT_EQ(three->style->stroke.get_value(), Glib::ustring(""));
    EXPECT_EQ(three->style->opacity.get_value(), Glib::ustring("1"));
    EXPECT_EQ(three->style->stroke_width.get_value(), Glib::ustring("2px"));

    auto four = cast<SPRect>(doc->getObjectById("four"));
    ASSERT_TRUE(four != nullptr);

    EXPECT_EQ(four->style->fill.get_value(), Glib::ustring("#d0d0d0"));
    EXPECT_EQ(four->style->stroke.get_value(), Glib::ustring("red"));
    EXPECT_EQ(four->style->opacity.get_value(), Glib::ustring("0.5"));
    EXPECT_EQ(four->style->stroke_width.get_value(), Glib::ustring("2px"));
}

/*
 * Test the origin flag for each of the values, should indicate where it came from.
 */
TEST_F(ObjectTest, StyleSource) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    SPRoot *root = doc->getRoot();
    ASSERT_TRUE(root->getRepr() != nullptr);
    ASSERT_TRUE(root->hasChildren());

    auto one = cast<SPRect>(doc->getObjectById("one"));
    ASSERT_TRUE(one != nullptr);

    EXPECT_EQ(one->style->fill.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(one->style->stroke.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(one->style->opacity.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(one->style->stroke_width.style_src, SPStyleSrc::STYLE_PROP);

    auto two = cast<SPRect>(doc->getObjectById("two"));
    ASSERT_TRUE(two != nullptr);

    EXPECT_EQ(two->style->fill.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(two->style->stroke.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(two->style->opacity.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(two->style->stroke_width.style_src, SPStyleSrc::STYLE_PROP);

    auto three = cast<SPRect>(doc->getObjectById("three"));
    ASSERT_TRUE(three != nullptr);

    EXPECT_EQ(three->style->fill.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(three->style->stroke.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(three->style->opacity.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(three->style->stroke_width.style_src, SPStyleSrc::STYLE_PROP);

    auto four = cast<SPRect>(doc->getObjectById("four"));
    ASSERT_TRUE(four != nullptr);

    EXPECT_EQ(four->style->fill.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(four->style->stroke.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(four->style->opacity.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(four->style->stroke_width.style_src, SPStyleSrc::STYLE_PROP);
}

/*
 * Test the breaking up of the font property and recreation into separate properties.
 */
TEST_F(ObjectTest, StyleFont) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    SPRoot *root = doc->getRoot();
    ASSERT_TRUE(root->getRepr() != nullptr);
    ASSERT_TRUE(root->hasChildren());

    auto five = cast<SPRect>(doc->getObjectById("five"));
    ASSERT_TRUE(five != nullptr);

    // Font property is ALWAYS unset as it's converted into specific font css properties
    EXPECT_EQ(five->style->font.get_value(), Glib::ustring(""));
    EXPECT_EQ(five->style->font_size.get_value(), Glib::ustring("12px"));
    EXPECT_EQ(five->style->font_weight.get_value(), Glib::ustring("bold"));
    EXPECT_EQ(five->style->font_style.get_value(), Glib::ustring("italic"));
    EXPECT_EQ(five->style->font_family.get_value(), Glib::ustring("arial, sans-serif"));
}

/*
 * Test the consumption of font dependent lengths in SPILength, e.g. EM, EX and % units
 */
TEST_F(ObjectTest, StyleFontSizes) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    SPRoot *root = doc->getRoot();
    ASSERT_TRUE(root->getRepr() != nullptr);
    ASSERT_TRUE(root->hasChildren());

    auto six = cast<SPRect>(doc->getObjectById("six"));
    ASSERT_TRUE(six != nullptr);

    EXPECT_EQ(six->style->stroke_width.get_value(), Glib::ustring("1em"));
    EXPECT_EQ(six->style->stroke_width.computed, 14);

    auto seven = cast<SPRect>(doc->getObjectById("seven"));
    ASSERT_TRUE(seven != nullptr);

    EXPECT_EQ(seven->style->stroke_width.get_value(), Glib::ustring("1ex"));
    EXPECT_EQ(seven->style->stroke_width.computed, 7);

    auto eight = cast<SPRect>(doc->getObjectById("eight"));
    ASSERT_TRUE(eight != nullptr);

    EXPECT_EQ(eight->style->stroke_width.get_value(), Glib::ustring("50%"));

    // stroke-width in percent is relative to viewport size, which is 300x150 in this example.
    // 50% is 118.59 == ((300^2 + 150^2) / 2)^0.5 * 0.5
    EXPECT_FLOAT_EQ(eight->style->stroke_width.computed, 118.58541);
}

/*
 * Regression: merging a width-only native CSS attribute must not strip the
 * priority/source of an unrelated !important property on the same object.
 *
 * Fixture 'four' (class='overload', style='fill:green;stroke:red !important;')
 * initially resolves stroke to red from the inline STYLE_PROP with !important,
 * beating the stylesheet .overload { stroke: #c0c0c0 !important; }. Applying a
 * native SPCSSAttr containing ONLY stroke-width must change the width while
 * preserving the unrelated red stroke winner/priority/source and fill/opacity.
 *
 * Diagnostic only: this inspects the computed SPStyle merge result. No renderer,
 * raster batch or native CLI is invoked.
 */
TEST_F(ObjectTest, WidthOnlyCssMergePreservesUnrelatedImportant) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    auto four = cast<SPRect>(doc->getObjectById("four"));
    ASSERT_TRUE(four != nullptr);
    ASSERT_TRUE(four->style != nullptr);
    ASSERT_TRUE(four->getRepr() != nullptr);

    // Preconditions: the fixture must start from the expected non-vacuous state.
    ASSERT_DOUBLE_EQ(four->style->stroke_width.computed, 2.0);
    ASSERT_EQ(four->style->stroke.get_value(), Glib::ustring("red"));
    ASSERT_TRUE(four->style->stroke.important);
    ASSERT_EQ(four->style->stroke.style_src, SPStyleSrc::STYLE_PROP);
    ASSERT_EQ(four->style->fill.get_value(), Glib::ustring("#d0d0d0"));
    ASSERT_EQ(four->style->opacity.get_value(), Glib::ustring("0.5"));

    // Native merge path only: a width-only SPCSSAttr, no stroke-controller.
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_set_property_double(css, "stroke-width", 10.0);
    four->changeCSS(css, "style");
    sp_repr_css_attr_unref(css);
    doc->ensureUpToDate();

    // The requested width change must apply.
    EXPECT_DOUBLE_EQ(four->style->stroke_width.computed, 10.0);

    // The unrelated !important stroke must retain its winner/priority/source.
    EXPECT_EQ(four->style->stroke.get_value(), Glib::ustring("red"));
    EXPECT_TRUE(four->style->stroke.important);
    EXPECT_EQ(four->style->stroke.style_src, SPStyleSrc::STYLE_PROP);

    // Unrelated fill/opacity must be untouched.
    EXPECT_EQ(four->style->fill.get_value(), Glib::ustring("#d0d0d0"));
    EXPECT_EQ(four->style->opacity.get_value(), Glib::ustring("0.5"));
}

// ---------------------------------------------------------------------------
// Native CSS priority repair coverage.
//
// These helpers exercise the real native SPCSSAttr path
// (sp_repr_css_attr_add_from_string + sp_repr_css_merge), not
// SPStyle::mergeString alone, so a dropped CRDeclaration::important flag is
// observable. Expectations are hand-derived from CSS2 cascade order point 4
// and the documented patch contract.
// ---------------------------------------------------------------------------
namespace {

Glib::ustring css_attr_merge_serialize(char const *dst_block, char const *src_block)
{
    SPCSSAttr *dst = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(dst, dst_block);
    SPCSSAttr *src = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(src, src_block);
    sp_repr_css_merge(dst, src);
    Glib::ustring out;
    sp_repr_css_write_string(dst, out);
    sp_repr_css_attr_unref(src);
    sp_repr_css_attr_unref(dst);
    return out;
}

} // namespace

/*
 * Real document fixture for unrelated !important preservation. The inline
 * declarations and the stylesheet declarations are all !important, so the
 * inline values must win by specificity and keep STYLE_PROP / important. Valid
 * marker defs are present so no resource is missing.
 */
class ObjectStylePriorityTest : public DocPerCaseTest {
public:
    ObjectStylePriorityTest() {
        constexpr auto docString = R"A(
<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>
<defs>
<marker id='localmarker' markerWidth='4' markerHeight='4' refX='2' refY='2' orient='auto'><path d='M0,0 L4,2 L0,4 z'/></marker>
<marker id='othermarker' markerWidth='4' markerHeight='4' refX='2' refY='2' orient='auto'><path d='M0,0 L4,2 L0,4 z'/></marker>
</defs>
<style>
.prio { fill: blue !important; opacity: 0.25 !important; font-size: 12px !important; stroke-dasharray: 9, 9 !important; stroke-dashoffset: 7 !important; marker-start: url(#othermarker) !important; }
</style>
<rect id='prio-one' class='prio' style='fill:red !important;opacity:0.75 !important;font-size:24px !important;stroke-dasharray:2, 5 !important;stroke-dashoffset:3 !important;marker-start:url(#localmarker) !important;stroke-width:2px;'/>
</svg>)A"sv;
        doc = SPDocument::createNewDocFromMem(docString);
        doc->ensureUpToDate();
    }

    std::unique_ptr<SPDocument> doc;
};

/*
 * Outcome A: a width-only native changeCSS must change the width while
 * preserving every unrelated !important value, priority and source.
 */
TEST_F(ObjectStylePriorityTest, WidthOnlyMergePreservesUnrelatedImportantValues) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    auto rect = cast<SPRect>(doc->getObjectById("prio-one"));
    ASSERT_TRUE(rect != nullptr);
    ASSERT_TRUE(rect->style != nullptr);
    ASSERT_TRUE(rect->getRepr() != nullptr);

    // Fixture must start from the expected non-vacuous state (fatal).
    ASSERT_DOUBLE_EQ(rect->style->stroke_width.computed, 2.0);
    ASSERT_EQ(rect->style->fill.get_value(), Glib::ustring("red"));
    ASSERT_TRUE(rect->style->fill.important);
    ASSERT_EQ(rect->style->fill.style_src, SPStyleSrc::STYLE_PROP);
    ASSERT_DOUBLE_EQ(rect->style->opacity.as_double(), 0.75);
    ASSERT_TRUE(rect->style->opacity.important);
    ASSERT_EQ(rect->style->opacity.style_src, SPStyleSrc::STYLE_PROP);
    ASSERT_DOUBLE_EQ(rect->style->font_size.computed, 24.0);
    ASSERT_TRUE(rect->style->font_size.important);
    ASSERT_EQ(rect->style->font_size.style_src, SPStyleSrc::STYLE_PROP);
    ASSERT_EQ(rect->style->stroke_dasharray.get_computed(), (std::vector<double>{2.0, 5.0}));
    ASSERT_TRUE(rect->style->stroke_dasharray.important);
    ASSERT_EQ(rect->style->stroke_dasharray.style_src, SPStyleSrc::STYLE_PROP);
    ASSERT_DOUBLE_EQ(rect->style->stroke_dashoffset.computed, 3.0);
    ASSERT_TRUE(rect->style->stroke_dashoffset.important);
    ASSERT_EQ(rect->style->stroke_dashoffset.style_src, SPStyleSrc::STYLE_PROP);
    ASSERT_EQ(rect->style->marker_start.get_value(), Glib::ustring("url(#localmarker)"));
    ASSERT_TRUE(rect->style->marker_start.important);
    ASSERT_EQ(rect->style->marker_start.style_src, SPStyleSrc::STYLE_PROP);

    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_set_property_double(css, "stroke-width", 10.0);
    rect->changeCSS(css, "style");
    sp_repr_css_attr_unref(css);
    doc->ensureUpToDate();

    EXPECT_DOUBLE_EQ(rect->style->stroke_width.computed, 10.0);
    EXPECT_EQ(rect->style->fill.get_value(), Glib::ustring("red"));
    EXPECT_TRUE(rect->style->fill.important);
    EXPECT_EQ(rect->style->fill.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_DOUBLE_EQ(rect->style->opacity.as_double(), 0.75);
    EXPECT_TRUE(rect->style->opacity.important);
    EXPECT_EQ(rect->style->opacity.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_DOUBLE_EQ(rect->style->font_size.computed, 24.0);
    EXPECT_TRUE(rect->style->font_size.important);
    EXPECT_EQ(rect->style->font_size.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(rect->style->stroke_dasharray.get_computed(), (std::vector<double>{2.0, 5.0}));
    EXPECT_TRUE(rect->style->stroke_dasharray.important);
    EXPECT_EQ(rect->style->stroke_dasharray.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_DOUBLE_EQ(rect->style->stroke_dashoffset.computed, 3.0);
    EXPECT_TRUE(rect->style->stroke_dashoffset.important);
    EXPECT_EQ(rect->style->stroke_dashoffset.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(rect->style->marker_start.get_value(), Glib::ustring("url(#localmarker)"));
    EXPECT_TRUE(rect->style->marker_start.important);
    EXPECT_EQ(rect->style->marker_start.style_src, SPStyleSrc::STYLE_PROP);
}

/*
 * Outcome B: within one parsed declaration block all four !important orders
 * must resolve as CSS2 specifies, through the real native SPCSSAttr parse and
 * sp_repr_css_merge patch path.
 */
TEST_F(ObjectStylePriorityTest, CssAttrBlockImportantPrecedenceAllOrders) {
    // normal -> normal: later normal wins, not important.
    {
        SPStyle style;
        auto const out = css_attr_merge_serialize("fill:green", "fill:red;fill:blue");
        style.mergeString(out.c_str());
        EXPECT_EQ(style.fill.get_value(), Glib::ustring("blue"));
        EXPECT_FALSE(style.fill.important);
    }
    // important -> normal: the first (!important) declaration wins.
    {
        SPStyle style;
        auto const out = css_attr_merge_serialize("fill:green", "fill:red !important;fill:blue");
        style.mergeString(out.c_str());
        EXPECT_EQ(style.fill.get_value(), Glib::ustring("red"));
        EXPECT_TRUE(style.fill.important);
    }
    // normal -> important: the !important declaration wins.
    {
        SPStyle style;
        auto const out = css_attr_merge_serialize("fill:green", "fill:red;fill:blue !important");
        style.mergeString(out.c_str());
        EXPECT_EQ(style.fill.get_value(), Glib::ustring("blue"));
        EXPECT_TRUE(style.fill.important);
    }
    // important -> important: the last !important declaration wins.
    {
        SPStyle style;
        auto const out = css_attr_merge_serialize("fill:green", "fill:red !important;fill:blue !important");
        style.mergeString(out.c_str());
        EXPECT_EQ(style.fill.get_value(), Glib::ustring("blue"));
        EXPECT_TRUE(style.fill.important);
    }
}

/*
 * Outcome B (value preservation): commas, URI text, quoted family names and a
 * normal non-important control must survive native parse/serialize unchanged.
 */
TEST_F(ObjectStylePriorityTest, CssAttrPreservesCommaUriQuotedAndNormalValues) {
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css,
        "font-family:Georgia, 'Minion Web';marker-start:url(#m);stroke-dasharray:2, 5;fill:none");

    EXPECT_STREQ(sp_repr_css_property(css, "font-family", nullptr), "Georgia, 'Minion Web'");
    EXPECT_STREQ(sp_repr_css_property(css, "marker-start", nullptr), "url(#m)");
    EXPECT_STREQ(sp_repr_css_property(css, "stroke-dasharray", nullptr), "2, 5");
    EXPECT_STREQ(sp_repr_css_property(css, "fill", nullptr), "none");
    sp_repr_css_attr_unref(css);

    // !important values must survive an actual serialize + reparse round trip.
    SPCSSAttr *imp = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(imp,
        "font-family:Georgia, 'Minion Web' !important;marker-start:url(#m) !important;"
        "stroke-dasharray:2, 5 !important;fill:none");
    Glib::ustring imp_out;
    sp_repr_css_write_string(imp, imp_out);
    sp_repr_css_attr_unref(imp);

    SPCSSAttr *imp2 = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(imp2, imp_out.c_str());
    EXPECT_STREQ(sp_repr_css_property(imp2, "font-family", nullptr), "Georgia, 'Minion Web' !important");
    EXPECT_STREQ(sp_repr_css_property(imp2, "marker-start", nullptr), "url(#m) !important");
    EXPECT_STREQ(sp_repr_css_property(imp2, "stroke-dasharray", nullptr), "2, 5 !important");
    EXPECT_STREQ(sp_repr_css_property(imp2, "fill", nullptr), "none");
    sp_repr_css_attr_unref(imp2);

    // Normal non-important control.
    SPStyle style;
    style.mergeString("fill:none");
    EXPECT_EQ(style.fill.get_value(), Glib::ustring("none"));
    EXPECT_FALSE(style.fill.important);
}

/*
 * Real document fixture for deliberate width-patch behavior and the
 * parent-important / child-normal inheritance contract.
 */
class ObjectStyleWidthPatchTest : public DocPerCaseTest {
public:
    ObjectStyleWidthPatchTest() {
        constexpr auto docString = R"A(
<svg xmlns='http://www.w3.org/2000/svg'>
<style>
.sheetwidth { stroke-width: 2 !important; fill: blue !important; }
</style>
<rect id='w-inline' style='stroke-width:2px !important;fill:red !important;'/>
<rect id='w-sheet-blocked' class='sheetwidth' style='fill:red !important;'/>
<rect id='w-patch-important' class='sheetwidth' style='fill:red !important;'/>
<g id='parent-important' style='stroke-width:2px !important;'>
  <rect id='child-normal' style='stroke-width:5px;'/>
</g>
</svg>)A"sv;
        doc = SPDocument::createNewDocFromMem(docString);
        doc->ensureUpToDate();
    }

    std::unique_ptr<SPDocument> doc;
};

/*
 * Outcome C(i): an ordinary (non-important) patch may replace an inline
 * !important width when no !important stylesheet competes for that property.
 */
TEST_F(ObjectStyleWidthPatchTest, OrdinaryPatchReplacesInlineImportantWidthWithoutSheetCompetition) {
    auto rect = cast<SPRect>(doc->getObjectById("w-inline"));
    ASSERT_TRUE(rect != nullptr);
    ASSERT_TRUE(rect->style != nullptr);

    ASSERT_DOUBLE_EQ(rect->style->stroke_width.computed, 2.0);
    ASSERT_TRUE(rect->style->stroke_width.important);
    ASSERT_EQ(rect->style->fill.get_value(), Glib::ustring("red"));
    ASSERT_TRUE(rect->style->fill.important);

    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, "stroke-width:10");
    rect->changeCSS(css, "style");
    sp_repr_css_attr_unref(css);
    doc->ensureUpToDate();

    EXPECT_DOUBLE_EQ(rect->style->stroke_width.computed, 10.0);
    EXPECT_FALSE(rect->style->stroke_width.important);
    EXPECT_EQ(rect->style->fill.get_value(), Glib::ustring("red"));
    EXPECT_TRUE(rect->style->fill.important);
}

/*
 * Outcome C(ii): an explicit !important patch beats an !important stylesheet.
 */
TEST_F(ObjectStyleWidthPatchTest, ImportantPatchBeatsImportantStylesheetWidth) {
    auto rect = cast<SPRect>(doc->getObjectById("w-patch-important"));
    ASSERT_TRUE(rect != nullptr);
    ASSERT_TRUE(rect->style != nullptr);

    ASSERT_DOUBLE_EQ(rect->style->stroke_width.computed, 2.0);
    ASSERT_TRUE(rect->style->stroke_width.important);
    ASSERT_EQ(rect->style->fill.get_value(), Glib::ustring("red"));
    ASSERT_TRUE(rect->style->fill.important);

    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, "stroke-width:10 !important");
    rect->changeCSS(css, "style");
    sp_repr_css_attr_unref(css);
    doc->ensureUpToDate();

    EXPECT_DOUBLE_EQ(rect->style->stroke_width.computed, 10.0);
    EXPECT_TRUE(rect->style->stroke_width.important);
    EXPECT_EQ(rect->style->fill.get_value(), Glib::ustring("red"));
    EXPECT_TRUE(rect->style->fill.important);
}

/*
 * Outcome C(iii): an ordinary patch cannot beat an !important stylesheet. The
 * correct native effective outcome is the stylesheet's 2 with important true,
 * not 10; the native parser must not invent !important.
 */
TEST_F(ObjectStyleWidthPatchTest, OrdinaryPatchBlockedByImportantStylesheetWidth) {
    auto rect = cast<SPRect>(doc->getObjectById("w-sheet-blocked"));
    ASSERT_TRUE(rect != nullptr);
    ASSERT_TRUE(rect->style != nullptr);

    ASSERT_DOUBLE_EQ(rect->style->stroke_width.computed, 2.0);
    ASSERT_TRUE(rect->style->stroke_width.important);
    ASSERT_EQ(rect->style->fill.get_value(), Glib::ustring("red"));
    ASSERT_TRUE(rect->style->fill.important);

    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, "stroke-width:10");
    rect->changeCSS(css, "style");
    sp_repr_css_attr_unref(css);
    doc->ensureUpToDate();

    EXPECT_DOUBLE_EQ(rect->style->stroke_width.computed, 2.0);
    EXPECT_TRUE(rect->style->stroke_width.important);
    EXPECT_EQ(rect->style->fill.get_value(), Glib::ustring("red"));
    EXPECT_TRUE(rect->style->fill.important);
}

/*
 * Outcome D (overlay): separate sp_repr_css_attr_add_from_string calls keep the
 * documented patch overlay. A later incoming normal value replaces an earlier
 * value that carried !important.
 */
TEST_F(ObjectStyleWidthPatchTest, SeparateAddFromStringStillOverlaysImportantValue) {
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, "stroke:red !important");
    sp_repr_css_attr_add_from_string(css, "stroke:blue");

    EXPECT_STREQ(sp_repr_css_property(css, "stroke", nullptr), "blue");

    Glib::ustring out;
    sp_repr_css_write_string(css, out);
    sp_repr_css_attr_unref(css);

    SPStyle style;
    style.mergeString(out.c_str());
    EXPECT_EQ(style.stroke.get_value(), Glib::ustring("blue"));
    EXPECT_FALSE(style.stroke.important);
}

/*
 * Outcome D (inheritance): a parent's !important width must not be treated as a
 * global local-cascade priority that blocks the child's explicit normal value.
 */
TEST_F(ObjectStyleWidthPatchTest, ChildExplicitNormalWidthSurvivesParentImportant) {
    auto parent = doc->getObjectById("parent-important");
    auto child = doc->getObjectById("child-normal");
    ASSERT_TRUE(parent != nullptr);
    ASSERT_TRUE(child != nullptr);
    ASSERT_TRUE(parent->style != nullptr);
    ASSERT_TRUE(child->style != nullptr);
    ASSERT_TRUE(child->getRepr() != nullptr);

    ASSERT_DOUBLE_EQ(parent->style->stroke_width.computed, 2.0);
    ASSERT_TRUE(parent->style->stroke_width.important);
    ASSERT_DOUBLE_EQ(child->style->stroke_width.computed, 5.0);
    ASSERT_FALSE(child->style->stroke_width.important);

    // Aggregating parent(!important) then child(normal) must yield the child's
    // explicit value, not the inherited priority.
    SPCSSAttr *inherited = sp_repr_css_attr_inherited(child->getRepr(), "style");
    EXPECT_STREQ(sp_repr_css_property(inherited, "stroke-width", nullptr), "5px");
    sp_repr_css_attr_unref(inherited);

    // Width-only patch on the child.
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, "stroke-width:10");
    child->changeCSS(css, "style");
    sp_repr_css_attr_unref(css);
    doc->ensureUpToDate();

    EXPECT_DOUBLE_EQ(child->style->stroke_width.computed, 10.0);
    EXPECT_FALSE(child->style->stroke_width.important);
    EXPECT_DOUBLE_EQ(parent->style->stroke_width.computed, 2.0);
    EXPECT_TRUE(parent->style->stroke_width.important);

    inherited = sp_repr_css_attr_inherited(child->getRepr(), "style");
    EXPECT_STREQ(sp_repr_css_property(inherited, "stroke-width", nullptr), "10");
    sp_repr_css_attr_unref(inherited);
}
