// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BUG-010 (internal note BUGS_TO_INVESTIGATE): the paint-order query read a
 * NULL text for `paint-order: inherit` under ancestors that set no paint
 * order, and crashed after Ungroup All (the Stroke Style panel queries the
 * new selection).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "desktop-style.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "style.h"
#include "xml/repr.h"

namespace {

class PaintOrderQueryTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    }

    static std::unique_ptr<SPDocument> parse(std::string const &body)
    {
        auto document = SPDocument::createNewDocFromMem(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">)" + body + "</svg>");
        if (document) document->ensureUpToDate();
        return document;
    }
};

TEST_F(PaintOrderQueryTest, ExplicitUnstrokedGroupIsSingle)
{
    auto document = parse(R"(<g id="g" style="paint-order:stroke fill"><path d="M0 0h10v10z"/></g>)");
    ASSERT_TRUE(document);
    auto *item = cast<SPItem>(document->getObjectById("g"));
    ASSERT_TRUE(item);
    ASSERT_TRUE(item->style->stroke.isNone());
    SPStyle result(document.get());
    EXPECT_EQ(sp_desktop_query_style_from_list({item}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_SINGLE);
    EXPECT_STREQ(result.paint_order.value, "stroke fill");
}

TEST_F(PaintOrderQueryTest, UnstrokedObjectWithoutExplicitOrderIsIgnored)
{
    auto document = parse(R"(<path id="a" d="M0 0h10v10z"/>)");
    ASSERT_TRUE(document);
    auto *item = cast<SPItem>(document->getObjectById("a"));
    ASSERT_TRUE(item);
    ASSERT_FALSE(item->style->paint_order.set);
    ASSERT_TRUE(item->style->stroke.isNone());
    SPStyle result(document.get());
    EXPECT_EQ(sp_desktop_query_style_from_list({item}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_NOTHING);
}

TEST_F(PaintOrderQueryTest, StrokedExplicitAndUnsetObjectsKeepCounting)
{
    auto document = parse(R"(<path id="a" d="M0 0h10v10z" style="stroke:black;paint-order:stroke fill"/>)"
                          R"(<path id="b" d="M20 0h10v10z" style="stroke:black"/>)");
    ASSERT_TRUE(document);
    auto *a = cast<SPItem>(document->getObjectById("a"));
    auto *b = cast<SPItem>(document->getObjectById("b"));
    ASSERT_TRUE(a && b);
    ASSERT_FALSE(b->style->paint_order.set);
    SPStyle result(document.get());
    EXPECT_EQ(sp_desktop_query_style_from_list({a}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_SINGLE);
    EXPECT_STREQ(result.paint_order.value, "stroke fill");
    EXPECT_EQ(sp_desktop_query_style_from_list({b}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_SINGLE);
    EXPECT_STREQ(result.paint_order.value, "");
    EXPECT_EQ(sp_desktop_query_style_from_list({a, b}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_MULTIPLE_SAME);
    EXPECT_STREQ(result.paint_order.value, "stroke fill");
}

TEST_F(PaintOrderQueryTest, ExplicitUnstrokedOrdersGiveMultipleDifferent)
{
    auto document = parse(R"(<path id="a" d="M0 0h10v10z" style="paint-order:stroke fill"/>)"
                          R"(<path id="b" d="M20 0h10v10z" style="paint-order:fill stroke"/>)");
    ASSERT_TRUE(document);
    auto *a = cast<SPItem>(document->getObjectById("a"));
    auto *b = cast<SPItem>(document->getObjectById("b"));
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(a->style->stroke.isNone() && b->style->stroke.isNone());
    SPStyle result(document.get());
    EXPECT_EQ(sp_desktop_query_style_from_list({a, b}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_MULTIPLE_DIFFERENT);
}

TEST_F(PaintOrderQueryTest, ExplicitUnstrokedOrdersGiveMultipleSame)
{
    auto document = parse(R"(<path id="a" d="M0 0h10v10z" style="paint-order:stroke fill"/>)"
                          R"(<path id="b" d="M20 0h10v10z" style="paint-order:stroke fill"/>)");
    ASSERT_TRUE(document);
    auto *a = cast<SPItem>(document->getObjectById("a"));
    auto *b = cast<SPItem>(document->getObjectById("b"));
    ASSERT_TRUE(a && b);
    SPStyle result(document.get());
    EXPECT_EQ(sp_desktop_query_style_from_list({a, b}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_MULTIPLE_SAME);
    EXPECT_STREQ(result.paint_order.value, "stroke fill");
}

TEST_F(PaintOrderQueryTest, TextAndPathGroupReportsNewRecursiveOrder)
{
    auto document = parse(R"(<g id="g"><text id="text" x="0" y="20"><tspan id="span" style="fill:red">Corel text</tspan></text>)"
                          R"(<path d="M0 30h10v10z" style="fill:blue"/></g>)");
    ASSERT_TRUE(document);
    auto *group = cast<SPItem>(document->getObjectById("g"));
    auto *text = cast<SPItem>(document->getObjectById("text"));
    ASSERT_TRUE(group && text);
    auto *css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "paint-order", "stroke fill");
    sp_desktop_apply_css_recursive(group, css, true);
    sp_repr_css_attr_unref(css);
    document->ensureUpToDate();
    SPStyle result(document.get());
    for (auto *item : {group, text}) {
        ASSERT_TRUE(item->style->stroke.isNone());
        EXPECT_EQ(sp_desktop_query_style_from_list({item}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_SINGLE);
        EXPECT_STREQ(result.paint_order.value, "stroke fill");
    }
}

TEST_F(PaintOrderQueryTest, InheritWithoutAnAncestorOrderQueriesAsNormal)
{
    auto document = parse(R"svg(<g id="g"><path id="a" d="M0 0h10v10z" style="paint-order:inherit;stroke:#000;fill:#f00"/></g>)svg");
    ASSERT_TRUE(document);
    auto *item = cast<SPItem>(document->getObjectById("a"));
    ASSERT_TRUE(item);
    ASSERT_TRUE(item->style->paint_order.set);
    ASSERT_EQ(item->style->paint_order.value, nullptr) << "the fixture must reach the state that crashed";

    SPStyle result(document.get());
    EXPECT_EQ(sp_desktop_query_style_from_list({item}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_SINGLE);
    ASSERT_TRUE(result.paint_order.value);
    EXPECT_STREQ(result.paint_order.value, "normal");
}

TEST_F(PaintOrderQueryTest, InheritKeepsTheAncestorOrder)
{
    auto document = parse(R"svg(<g id="g" style="paint-order:stroke fill"><path id="a" d="M0 0h10v10z" style="paint-order:inherit;stroke:#000"/></g>)svg");
    ASSERT_TRUE(document);
    auto *item = cast<SPItem>(document->getObjectById("a"));
    ASSERT_TRUE(item);
    SPStyle result(document.get());
    EXPECT_EQ(sp_desktop_query_style_from_list({item}, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_SINGLE);
    ASSERT_TRUE(result.paint_order.value);
    EXPECT_STREQ(result.paint_order.value, "stroke fill");
}

TEST_F(PaintOrderQueryTest, UngroupAllThenTheStrokeQueryDoesNotCrash)
{
    // The owner's sequence: nested groups, Ungroup All, then the query the
    // Stroke Style panel runs on the new selection.
    auto document = parse(R"svg(<g id="outer"><g id="inner">)svg"
                          R"svg(<path id="a" d="M0 0h10v10z" style="paint-order:inherit;stroke:#000;fill:#f00"/>)svg"
                          R"svg(<path id="b" d="M20 0h10v10z" style="stroke:#000;fill:#0f0"/></g>)svg"
                          R"svg(<path id="c" d="M40 0h10v10z" style="paint-order:inherit;stroke:#00f"/></g>)svg");
    ASSERT_TRUE(document);
    Inkscape::ObjectSet set(document.get());
    set.set(cast<SPItem>(document->getObjectById("outer")));
    set.ungroup_all(true);
    document->ensureUpToDate();
    auto const items = set.items_vector();
    ASSERT_EQ(items.size(), 3u);
    for (auto *item : items) {
        EXPECT_EQ(item->parent, document->getRoot()) << "ungrouped to the root";
    }
    SPStyle result(document.get());
    EXPECT_NE(sp_desktop_query_style_from_list(items, &result, QUERY_STYLE_PROPERTY_PAINTORDER), QUERY_STYLE_NOTHING);
    ASSERT_TRUE(result.paint_order.value);
    EXPECT_STREQ(result.paint_order.value, "normal");
}

} // namespace
