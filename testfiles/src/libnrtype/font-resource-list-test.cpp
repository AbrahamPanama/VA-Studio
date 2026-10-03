// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/convert.h>
#include <glibmm/init.h>
#include <glibmm/miscutils.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "io/resource.h"

namespace {

class FontResourceListTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Glib::init();
        GError *error = nullptr;
        auto *created = g_dir_make_tmp("inkscape-font-resource-list-XXXXXX", &error);
        auto const message = error ? std::string(error->message) : std::string{};
        g_clear_error(&error);
        ASSERT_NE(created, nullptr) << message;
        root = created;
        g_free(created);

        nested = Glib::build_filename(root, Glib::filename_from_utf8("Fuentes ñ 测试"));
        font_named_directory = Glib::build_filename(root, "directory.ttf");
        ASSERT_EQ(g_mkdir(nested.c_str(), 0700), 0);
        ASSERT_EQ(g_mkdir(font_named_directory.c_str(), 0700), 0);
        paths = {
            Glib::build_filename(root, "regular.ttf"),
            Glib::build_filename(root, "other.otf"),
            Glib::build_filename(root, "readme.txt"),
            Glib::build_filename(root, "skip-regular.ttf"),
            Glib::build_filename(nested, Glib::filename_from_utf8("café-ñ-测试.otf")),
            Glib::build_filename(nested, "skip-nested.otf"),
        };
        for (auto const &path : paths) {
            ASSERT_TRUE(g_file_set_contents(path.c_str(), "enumeration fixture", -1, nullptr)) << path;
        }
    }

    void TearDown() override
    {
        // Remove only the exact files and directories created by this fixture.
        for (auto const &path : paths) {
            g_remove(path.c_str());
        }
        if (!font_named_directory.empty()) g_rmdir(font_named_directory.c_str());
        if (!nested.empty()) g_rmdir(nested.c_str());
        if (!root.empty()) g_rmdir(root.c_str());
    }

    std::string root;
    std::string nested;
    std::string font_named_directory;
    std::vector<std::string> paths;
};

TEST_F(FontResourceListTest, RecursesAndAppendsOnlyRegularFilesWithoutFilters)
{
    std::vector<std::string> actual = {"existing entry"};
    Inkscape::IO::Resource::get_filenames_from_path(actual, root);
    std::vector<std::string> expected = {"existing entry"};
    for (auto const &path : paths) {
        expected.push_back(Glib::filename_to_utf8(path));
    }
    EXPECT_THAT(actual, testing::UnorderedElementsAreArray(expected));
}

TEST_F(FontResourceListTest, FiltersExtensionsAndExclusionPrefixesRecursively)
{
    std::vector<std::string> actual;
    Inkscape::IO::Resource::get_filenames_from_path(actual, root, {"ttf", "otf"}, {"skip-"});
    EXPECT_THAT(actual, testing::UnorderedElementsAre(
        Glib::filename_to_utf8(paths[0]),
        Glib::filename_to_utf8(paths[1]),
        Glib::filename_to_utf8(paths[4])));
}

TEST_F(FontResourceListTest, MissingAndRegularFilePathsLeaveExistingEntriesUnchanged)
{
    std::vector<std::string> actual = {"existing entry"};
    Inkscape::IO::Resource::get_filenames_from_path(actual, Glib::build_filename(root, "missing"));
    Inkscape::IO::Resource::get_filenames_from_path(actual, paths[0]);
    EXPECT_THAT(actual, testing::ElementsAre("existing entry"));
}

TEST_F(FontResourceListTest, ReturnsUnicodeDirectoryAndFilenameInUtf8)
{
    std::vector<std::string> actual;
    Inkscape::IO::Resource::get_filenames_from_path(actual, nested, {"otf"}, {"skip-"});
    ASSERT_EQ(actual.size(), 1);
    EXPECT_EQ(actual[0], Glib::filename_to_utf8(paths[4]));
    EXPECT_TRUE(g_utf8_validate(actual[0].c_str(), -1, nullptr));
    EXPECT_NE(actual[0].find("Fuentes ñ 测试"), std::string::npos);
    EXPECT_NE(actual[0].find("café-ñ-测试.otf"), std::string::npos);
}

} // namespace
