// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit test for the font factory
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glibmm/init.h>
#include "libnrtype/font-instance.h"
#include "libnrtype/font-factory.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace {

class FontFactoryTest : public ::testing::Test
{
protected:
    FontFactoryTest()
    {
        Glib::init();
    }

    Inkscape::Util::Statics statics;
};

TEST_F(FontFactoryTest, getSingleton)
{
    auto &factory = FontFactory::get();
    ASSERT_EQ(factory.fontSize, 512);
}

TEST_F(FontFactoryTest, GetUIFamilies)
{
    std::vector<std::string> families;
    for (auto &x : FontFactory::get().GetUIFamilies()) {
        families.emplace_back(x.first);
    }
    // A sample of test fonts shipped in the data fonts directory
    EXPECT_THAT(families, testing::Contains("Sans"));
    EXPECT_THAT(families, testing::Contains("ComicSpice"));
    EXPECT_THAT(families, testing::Contains("Monospace"));
    EXPECT_THAT(families, testing::Contains("Noto Sans"));
    EXPECT_THAT(families, testing::Contains("WenQuanYi Micro Hei"));
}

TEST_F(FontFactoryTest, GetUIStyles)
{
    auto families = FontFactory::get().GetUIFamilies();
    {
        auto styles = FontFactory::get().GetUIStyles(families["ComicSpice"]);
        ASSERT_EQ(styles.size(), 1);
        EXPECT_EQ(styles[0].css_name, "Normal");
        EXPECT_EQ(styles[0].display_name, "Regular");
    }
    {
        auto styles = FontFactory::get().GetUIStyles(families["Serif"]);
        ASSERT_EQ(styles.size(), 4);
        EXPECT_EQ(styles[0].display_name, "Regular") << styles[0].display_name;
        EXPECT_EQ(styles[1].display_name, "Italic") << styles[1].display_name;
        EXPECT_EQ(styles[2].display_name, "Bold") << styles[2].display_name;
        EXPECT_EQ(styles[3].display_name, "Bold Italic") << styles[3].display_name;
    }
}

TEST_F(FontFactoryTest, DefaultFaceHasNoSyntheticNormalStyleLabel)
{
    auto *description = pango_font_description_from_string("Sans");
    ASSERT_TRUE(description);
    EXPECT_TRUE(FontFactory::get().GetUIStyleString(description).empty());
    pango_font_description_free(description);
}

TEST_F(FontFactoryTest, FaceFromDesc)
{
    auto face = FontFactory::get().FaceFromDescr("Serif", "Bold");
    ASSERT_TRUE(face);
}

TEST_F(FontFactoryTest, FontConfigurationManifestIsStableAndContentAddressed)
{
    auto const first = FontFactory::get().font_config_manifest();
    auto const second = FontFactory::get().font_config_manifest();
    EXPECT_EQ(first.size(), 64);
    EXPECT_EQ(first, second);
    EXPECT_THAT(first, testing::Each(testing::AnyOf(
        testing::AllOf(testing::Ge('0'), testing::Le('9')),
        testing::AllOf(testing::Ge('a'), testing::Le('f')))));
}

} // namespace

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
