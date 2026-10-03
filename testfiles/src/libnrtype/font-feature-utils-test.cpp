// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "libnrtype/font-feature-utils.h"

namespace {

using Inkscape::merge_font_feature;
using Inkscape::query_font_feature;

TEST(FontFeatureUtils, QueriesCommonCssAndPangoForms)
{
    EXPECT_EQ(query_font_feature("liga", "liga"), true);
    EXPECT_EQ(query_font_feature("'liga' 0", "liga"), false);
    EXPECT_EQ(query_font_feature("\"liga\"=off", "liga"), false);
    EXPECT_EQ(query_font_feature("kern on, c2sc=1", "c2sc"), true);
    EXPECT_EQ(query_font_feature("kern 1, liga 0", "smcp"), std::nullopt);
}

TEST(FontFeatureUtils, ChangesOnlyTheRequestedTag)
{
    EXPECT_EQ(merge_font_feature("kern 1, liga 0, ss01 1", "liga", true),
              "kern 1, ss01 1, liga 1");
    EXPECT_EQ(merge_font_feature("'c2sc' 0, smcp 1, c2sc 1", "c2sc", false),
              "smcp 1, c2sc 0");
    EXPECT_EQ(merge_font_feature("kern 1, liga 0", "liga", std::nullopt), "kern 1");
}

TEST(FontFeatureUtils, UsesNormalForAnEmptyFeatureSet)
{
    EXPECT_EQ(merge_font_feature("normal", "liga", std::nullopt), "normal");
    EXPECT_EQ(merge_font_feature("liga 1", "liga", std::nullopt), "normal");
    EXPECT_EQ(merge_font_feature("", "c2sc", true), "c2sc 1");
}

TEST(FontFeatureUtils, LeavesUnknownValuesQueryableAsUnknown)
{
    EXPECT_EQ(query_font_feature("liga 2", "liga"), std::nullopt);
    EXPECT_EQ(query_font_feature("'liga", "liga"), std::nullopt);
}

} // namespace
