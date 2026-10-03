// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Tests for Style internal classes
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2020 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#include <gtest/gtest.h>
#include <src/style-internal.h>

#include "colors/cms/system.h"
#include "colors/document-cms.h"
#include "colors/spaces/cms.h"
#include "document.h"
#include "object/sp-object.h"
#include "object/sp-shape-reference.h"
#include "xml/repr.h"
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include "inkscape.h"
#include "style.h"

TEST(StyleInternalTest, PaintOrderInheritLeavesNoFreedText)
{
    // BUG-010: "inherit" assigns no text; the previous one must not stay
    // behind freed (a later read or the destructor would free it again).
    SPIPaintOrder order;
    order.read("stroke fill");
    ASSERT_NE(order.value, nullptr);
    order.read("inherit");
    EXPECT_TRUE(order.set);
    EXPECT_TRUE(order.inherit);
    EXPECT_EQ(order.value, nullptr);
    order.read("markers");
    ASSERT_NE(order.value, nullptr);
    EXPECT_STREQ(order.value, "markers");
    EXPECT_FALSE(order.inherit);
}

TEST(StyleInternalTest, testSPIDashArrayInequality)
{
	SPIDashArray array;
	array.read("0 1 2 3");
	SPIDashArray subsetArray;
	subsetArray.read("0 1");
	
	ASSERT_FALSE(array == subsetArray);
	ASSERT_FALSE(subsetArray == array);
}

TEST(StyleInternalTest, testSPIDashArrayEquality)
{
	SPIDashArray anArray;
	anArray.read("0 1 2 3");
	SPIDashArray sameArray;
	sameArray.read("0 1 2 3");
	
	ASSERT_TRUE(anArray == sameArray);
	ASSERT_TRUE(sameArray == anArray);
}

TEST(StyleInternalTest, testSPIDashArrayValidity)
{
    // valid dash arrays
	SPIDashArray array10;
	array10.read("");

	SPIDashArray array11;
	array11.read("0");

	SPIDashArray array12;
	array12.read("0 1e3");

    // invalid dash arrayas
	SPIDashArray array20;
	array20.read("1-1");

	SPIDashArray array21;
	array21.read("10 10 -10");

	SPIDashArray array22;
	array22.read("-1");

	SPIDashArray array23;
	array23.read("0 -5e3");


    EXPECT_TRUE(array10.is_valid());
    EXPECT_TRUE(array11.is_valid());
    EXPECT_TRUE(array12.is_valid());

    // SPIDashArray::read is geared towards happy path, so it may reject negative entries:

    // EXPECT_FALSE(array20.is_valid()); // cannot read "1-1" as numbers, so 0
    EXPECT_FALSE(array21.is_valid());
    // EXPECT_FALSE(array22.is_valid()); // lone negative number is deemed invalid and removed by 'read'
    // EXPECT_FALSE(array23.is_valid()); // negative total: invalid and removed by 'read'
}

TEST(StyleInternalTest, testSPIPaint)
{
    SPIPaint paint;
    EXPECT_EQ(paint.get_value(), "");
    paint.read("red");
    EXPECT_EQ(paint.get_value(), "red");
    paint.clear();
    EXPECT_EQ(paint.get_value(), "");
}

class SPIColorInterpolationTest : public ::testing::Test {
protected:
    void SetUp() override {
        Inkscape::Application::create(false);

        auto &cms = Inkscape::Colors::CMS::System::get();
        cms.clearDirectoryPaths();
        std::string icc_dir = INKSCAPE_TESTS_DIR "/data/colors/";
        cms.addDirectoryPath(icc_dir, false);
        cms.refreshProfiles();

        std::string svg_objs_file = INKSCAPE_TESTS_DIR "/data/colors/cms-in-objs.svg";
        doc = SPDocument::createNewDoc(svg_objs_file.c_str());

        style = new SPStyle(doc.get());
    }

    void TearDown() override {
        delete style;
    }

    std::unique_ptr<SPDocument> doc;
    SPStyle* style;
};


TEST_F(SPIColorInterpolationTest, ReadsStandardGlobalProfile) {
    /* Global standard profiles */
    style->color_interpolation.read("sRGB");
    auto space_srgb = style->color_interpolation.getInterpolationSpace();

    ASSERT_NE(space_srgb, nullptr) << "Failed to load standard global profile: sRGB";
    EXPECT_EQ(space_srgb->getSvgName(), "sRGB");

    style->color_interpolation.read("linearRGB");
    auto space_linear = style->color_interpolation.getInterpolationSpace();

    ASSERT_NE(space_linear, nullptr) << "Failed to load standard global profile: linearRGB";
    EXPECT_EQ(space_linear->getSvgName(), "linearRGB");
}

TEST_F(SPIColorInterpolationTest, ReadsCustomDocumentProfile) {
    /* Document ICC Profiles */
    ASSERT_TRUE(doc->getDocumentCMS().getSpace("grb"))
        << "Test SVG failed to load the custom profile.";

    style->color_interpolation.read("grb");
    auto space_grb = style->color_interpolation.getInterpolationSpace();

    ASSERT_NE(space_grb, nullptr) << "Interpolation space is null!";
    EXPECT_EQ(space_grb->getSvgName(), "grb");

    ASSERT_TRUE(doc->getDocumentCMS().getSpace("cmyk-rcm"))
        << "Test SVG failed to load the custom profile.";

    style->color_interpolation.read("cmyk-rcm");
    auto space_rcm = style->color_interpolation.getInterpolationSpace();

    ASSERT_NE(space_rcm, nullptr) << "Interpolation space is null!";
    EXPECT_EQ(space_rcm->getSvgName(), "cmyk-rcm");
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :


namespace {
std::unique_ptr<SPDocument> shape_none_document(std::string const &inline_style,
                                               std::string const &rule = "")
{
    auto xml = std::string("<svg xmlns='http://www.w3.org/2000/svg' width='96' height='96'>") +
        "<defs><rect id='frame' width='80' height='60'/>"
        "<rect id='cut' x='20' width='10' height='60'/></defs>"
        "<style>" + rule + "</style><g id='probe' class='probe' style='" + inline_style + "'/></svg>";
    auto doc = SPDocument::createNewDocFromMem(std::string_view(xml));
    if (!doc) throw std::runtime_error("Shape none native document fixture");
    doc->ensureUpToDate();
    return doc;
}

// SPIShapes reports URL diagnostics through std::cerr, not GLib logging.
// Restore the stream buffer on every exit, including fixture exceptions.
std::string shape_none_stderr(std::function<void()> const &operation)
{
    std::ostringstream captured;
    struct Restore {
        std::streambuf *previous;
        ~Restore() { std::cerr.rdbuf(previous); }
    } restore{std::cerr.rdbuf(captured.rdbuf())};
    operation();
    return captured.str();
}
class SPIShapesNoneTest : public ::testing::Test {
    void SetUp() override {
        if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    }
};
}

TEST_F(SPIShapesNoneTest, NeutralValueNeedsNoObjectAndRetainsImportantState)
{
    auto doc = shape_none_document("");
    SPStyle style(doc.get()); // No SPObject: a neutral value needs no reference owner.
    auto diagnostics = shape_none_stderr([&] {
        style.shape_inside.readIfUnset("none !important");
        style.shape_subtract.readIfUnset("NONE !important");
    });
    EXPECT_TRUE(diagnostics.empty()) << diagnostics;
    EXPECT_TRUE(style.shape_inside.set); EXPECT_TRUE(style.shape_inside.important);
    EXPECT_TRUE(style.shape_subtract.set); EXPECT_TRUE(style.shape_subtract.important);
    EXPECT_FALSE(style.shape_inside.inherit); EXPECT_FALSE(style.shape_subtract.inherit);
    EXPECT_TRUE(style.shape_inside.hrefs.empty()); EXPECT_TRUE(style.shape_subtract.hrefs.empty());
    EXPECT_EQ(style.shape_inside.write(SP_STYLE_FLAG_IFSET), "shape-inside:none !important;");
    EXPECT_EQ(style.shape_subtract.write(SP_STYLE_FLAG_IFSET), "shape-subtract:NONE !important;");
}

TEST_F(SPIShapesNoneTest, NativeNoneClearsPriorReferencesAndRetainsPriority)
{
    auto doc = shape_none_document("shape-inside:url(#frame);shape-subtract:url(#cut)");
    auto probe = doc->getObjectById("probe"); ASSERT_TRUE(probe);
    ASSERT_EQ(probe->style->shape_inside.hrefs.size(), 1u);
    ASSERT_EQ(probe->style->shape_subtract.hrefs.size(), 1u);
    EXPECT_EQ(probe->style->shape_inside.hrefs[0]->getObject(), doc->getObjectById("frame"));
    EXPECT_EQ(probe->style->shape_subtract.hrefs[0]->getObject(), doc->getObjectById("cut"));
    auto diagnostics = shape_none_stderr([&] {
        probe->getRepr()->setAttribute("style", "shape-inside:none !important;shape-subtract:none !important");
        doc->ensureUpToDate();
    });
    EXPECT_TRUE(diagnostics.empty()) << diagnostics;
    EXPECT_TRUE(probe->style->shape_inside.set); EXPECT_TRUE(probe->style->shape_inside.important);
    EXPECT_TRUE(probe->style->shape_subtract.set); EXPECT_TRUE(probe->style->shape_subtract.important);
    EXPECT_TRUE(probe->style->shape_inside.hrefs.empty()); EXPECT_TRUE(probe->style->shape_subtract.hrefs.empty());
    EXPECT_EQ(probe->style->shape_inside.write(SP_STYLE_FLAG_IFSET), "shape-inside:none !important;");
    EXPECT_EQ(probe->style->shape_subtract.write(SP_STYLE_FLAG_IFSET), "shape-subtract:none !important;");
}

TEST_F(SPIShapesNoneTest, ImportantNoneShieldsStylesheetUrlsThroughNativeRebuild)
{
    auto const rule = ".probe{shape-inside:url(#frame)!important;shape-subtract:url(#cut)!important}";
    auto control = shape_none_document("", rule);
    auto unshielded = control->getObjectById("probe"); ASSERT_TRUE(unshielded);
    // Positive control: the destination rule really attaches both dependencies.
    ASSERT_EQ(unshielded->style->shape_inside.hrefs.size(), 1u);
    ASSERT_EQ(unshielded->style->shape_subtract.hrefs.size(), 1u);
    EXPECT_EQ(unshielded->style->shape_inside.hrefs[0]->getObject(), control->getObjectById("frame"));
    EXPECT_EQ(unshielded->style->shape_subtract.hrefs[0]->getObject(), control->getObjectById("cut"));
    for (auto keyword : {"none", "NONE", "nOnE"}) {
        SCOPED_TRACE(keyword);
        std::unique_ptr<SPDocument> doc;
        auto diagnostics = shape_none_stderr([&] {
            doc = shape_none_document(std::string("shape-inside:") + keyword +
                " !important;shape-subtract:" + keyword + " !important", rule);
            auto probe = doc->getObjectById("probe");
            probe->style->readFromObject(probe);
            doc->ensureUpToDate();
        });
        EXPECT_TRUE(diagnostics.empty()) << diagnostics;
        auto probe = doc->getObjectById("probe"); ASSERT_TRUE(probe);
        SPIShapes *properties[] = {&probe->style->shape_inside, &probe->style->shape_subtract};
        for (auto *property : properties) {
            EXPECT_TRUE(property->set); EXPECT_TRUE(property->important);
            EXPECT_FALSE(property->inherit); EXPECT_TRUE(property->hrefs.empty());
            EXPECT_EQ(g_ascii_strcasecmp(property->get_value().c_str(), "none"), 0);
        }
    }
}

TEST_F(SPIShapesNoneTest, SerializedNeutralPrioritySurvivesNativeReopen)
{
    auto const rule = ".probe{shape-inside:url(#frame)!important;shape-subtract:url(#cut)!important}";
    std::unique_ptr<SPDocument> source, reopened;
    std::string shield;
    auto diagnostics = shape_none_stderr([&] {
        source = shape_none_document("shape-inside:none !important;shape-subtract:none !important", rule);
        auto probe = source->getObjectById("probe");
        shield = (probe->style->shape_inside.write(SP_STYLE_FLAG_IFSET) +
                  probe->style->shape_subtract.write(SP_STYLE_FLAG_IFSET)).raw();
        reopened = shape_none_document(shield, rule);
    });
    EXPECT_TRUE(diagnostics.empty()) << diagnostics;
    EXPECT_EQ(shield, "shape-inside:none !important;shape-subtract:none !important;");
    auto probe = reopened->getObjectById("probe"); ASSERT_TRUE(probe);
    EXPECT_TRUE(probe->style->shape_inside.important); EXPECT_TRUE(probe->style->shape_inside.hrefs.empty());
    EXPECT_TRUE(probe->style->shape_subtract.important); EXPECT_TRUE(probe->style->shape_subtract.hrefs.empty());
}

TEST_F(SPIShapesNoneTest, NativeUrlListsStillAttachAfterNeutralValue)
{
    std::unique_ptr<SPDocument> doc;
    auto diagnostics = shape_none_stderr([&] {
        doc = shape_none_document("shape-inside:none !important;shape-subtract:none !important");
        doc->getObjectById("probe")->getRepr()->setAttribute("style",
            "shape-inside:url(#frame) url(#cut)!important;shape-subtract:url(#cut)!important");
        doc->ensureUpToDate();
    });
    EXPECT_TRUE(diagnostics.empty()) << diagnostics;
    auto probe = doc->getObjectById("probe"); ASSERT_TRUE(probe);
    ASSERT_EQ(probe->style->shape_inside.hrefs.size(), 2u);
    ASSERT_EQ(probe->style->shape_subtract.hrefs.size(), 1u);
    EXPECT_EQ(probe->style->shape_inside.hrefs[0]->getObject(), doc->getObjectById("frame"));
    EXPECT_EQ(probe->style->shape_inside.hrefs[1]->getObject(), doc->getObjectById("cut"));
    EXPECT_EQ(probe->style->shape_subtract.hrefs[0]->getObject(), doc->getObjectById("cut"));
    EXPECT_TRUE(probe->style->shape_inside.important); EXPECT_TRUE(probe->style->shape_subtract.important);
}
