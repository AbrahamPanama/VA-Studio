// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <glib.h>
#include <glib/gstdio.h>

#include "util/font-discovery.h"
#include "libnrtype/font-utils.h"

using namespace Inkscape;

namespace {

TEST(FontStartupTest, SkipsRemotePathsBeforeProbingThem)
{
    for (auto path : {"\\\\offline-server\\fonts", "//offline-server/fonts"}) {
        g_test_expect_message(nullptr, G_LOG_LEVEL_WARNING, "*Remote fonts dir*skipped at startup*Copy the fonts*");
        auto const started = std::chrono::steady_clock::now();
        EXPECT_FALSE(Inkscape::font_directory_allowed_at_startup(path));
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
        g_test_assert_expected_messages();
    }
}

TEST(FontStartupTest, KeepsLocalFoldersAndLogsMissingFoldersPromptly)
{
    auto directory = g_dir_make_tmp("st-q-fonts-XXXXXX", nullptr);
    ASSERT_NE(directory, nullptr);
    EXPECT_TRUE(Inkscape::font_directory_allowed_at_startup(directory));
    auto missing = std::string(directory) + "/missing";
    g_test_expect_message(nullptr, G_LOG_LEVEL_INFO, "*does not exist and will be ignored*");
    auto const started = std::chrono::steady_clock::now();
    EXPECT_FALSE(Inkscape::font_directory_allowed_at_startup(missing.c_str()));
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
    g_test_assert_expected_messages();
    EXPECT_FALSE(Inkscape::font_directory_allowed_at_startup(nullptr));
    EXPECT_FALSE(Inkscape::font_directory_allowed_at_startup(""));
    g_rmdir(directory);
    g_free(directory);
}


class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        GError *error = nullptr;
        auto created = g_dir_make_tmp("inkscape-font-catalog-test-XXXXXX", &error);
        if (!created) {
            auto message = error ? error->message : "unknown error";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
        path = std::filesystem::u8path(created);
        g_free(created);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path); }

    std::filesystem::path path;
};

FontInfo face(char const *family, char const *style, char const *description,
              char const *fontspec, char const *id)
{
    FontInfo result;
    result.family_name = family;
    result.face_name = style;
    result.description = description;
    result.fontspec = fontspec;
    result.id = id;
    result.source_path = "/fonts/test.otf";
    result.source_size = 1234;
    result.source_mtime = 5678;
    result.weight = style == std::string{"Bold"} ? 0.8 : 0.4;
    result.width = 0.5;
    result.family_kind = 8;
    result.monospaced = false;
    result.oblique = false;
    result.variable_font = false;
    result.synthetic = false;
    result.available = true;
    return result;
}

TEST(FontCatalogTest, RoundTripsCompleteSerializableRecords)
{
    TemporaryDirectory temporary;
    auto filename = (temporary.path / "catalog.ini").string();
    FontFamilies expected{
        {face("Example Sans", "Bold", "Example Sans Bold", "Example Sans Bold", "bold"),
         face("Example Sans", "Regular", "Example Sans Regular", "Example Sans", "regular")},
        {face("وسيم", "فاتح", "وسيم Light", "وسيم, Light", "unicode")}
    };

    ASSERT_TRUE(FontCatalog::save(filename, expected, "manifest-a"));
    auto actual = FontCatalog::load(filename, "manifest-a");
    ASSERT_TRUE(actual);
    ASSERT_EQ(actual->size(), 2);

    std::size_t count = 0;
    for (auto const &family : *actual) {
        for (auto const &font : family) {
            ++count;
            EXPECT_TRUE(font.available);
            EXPECT_FALSE(font.family_name.empty());
            EXPECT_FALSE(font.face_name.empty());
            EXPECT_FALSE(font.description.empty());
            EXPECT_FALSE(font.fontspec.empty());
            EXPECT_EQ(font.source_size, 1234);
            EXPECT_EQ(font.source_mtime, 5678);
            EXPECT_FALSE(font.ff);
            EXPECT_FALSE(font.face);
        }
    }
    EXPECT_EQ(count, 3);
}

TEST(FontCatalogTest, RejectsDifferentFontConfigurationManifest)
{
    TemporaryDirectory temporary;
    auto filename = (temporary.path / "catalog.ini").string();
    FontFamilies fonts{{face("Example", "Regular", "Example Regular", "Example", "regular")}};

    ASSERT_TRUE(FontCatalog::save(filename, fonts, "manifest-a"));
    EXPECT_FALSE(FontCatalog::load(filename, "manifest-b"));
}

TEST(FontCatalogTest, SavesAndWarmLoadsUtf8ProfilePath)
{
    TemporaryDirectory temporary;
    auto unicode_directory = temporary.path / std::filesystem::u8path("Véronica测试 profile");
    ASSERT_TRUE(std::filesystem::create_directory(unicode_directory));
    auto encoded = (unicode_directory / "catalog.ini").u8string();
    std::string filename(reinterpret_cast<char const *>(encoded.data()), encoded.size());
    auto variable = face("Example Variable", "Regular", "Example Variable Regular", "Example Variable", "variable");
    variable.variable_font = true;
    variable.variations = "wght=650";
    FontFamilies fonts{{variable}};
    ASSERT_TRUE(FontCatalog::save(filename, fonts, "unicode-manifest"));
    auto loaded = FontCatalog::load(filename, "unicode-manifest");
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->size(), 1);
    EXPECT_EQ(loaded->front().front().variations, "wght=650");
    EXPECT_TRUE(loaded->front().front().variable_font);
    EXPECT_FALSE(FontCatalog::load(filename, "stale-manifest"));
}

TEST(FontCatalogTest, RejectsTruncatedRecordInsteadOfPublishingPartialList)
{
    TemporaryDirectory temporary;
    auto filename = (temporary.path / "catalog.ini").string();
    FontFamilies fonts{{face("Example", "Regular", "Example Regular", "Example", "regular")}};
    ASSERT_TRUE(FontCatalog::save(filename, fonts, "manifest"));

    std::ifstream input(filename);
    std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto const start = data.find("fontspec=");
    ASSERT_NE(start, std::string::npos);
    auto const end = data.find('\n', start);
    data.erase(start, end - start + 1);
    std::ofstream(filename, std::ios::trunc) << data;

    EXPECT_FALSE(FontCatalog::load(filename, "manifest"));
}

TEST(FontCatalogTest, RejectsHeaderCountMismatch)
{
    TemporaryDirectory temporary;
    auto filename = (temporary.path / "catalog.ini").string();
    FontFamilies fonts{{face("Example", "Regular", "Example Regular", "Example", "regular")}};
    ASSERT_TRUE(FontCatalog::save(filename, fonts, "manifest"));

    std::ifstream input(filename);
    std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto const start = data.find("faces=1");
    ASSERT_NE(start, std::string::npos);
    data.replace(start, 7, "faces=2");
    std::ofstream(filename, std::ios::trunc) << data;

    EXPECT_FALSE(FontCatalog::load(filename, "manifest"));
}

TEST(FontCatalogTest, DeduplicatesIdenticalStableFaceIds)
{
    TemporaryDirectory temporary;
    auto filename = (temporary.path / "catalog.ini").string();
    auto duplicate = face("Example", "Regular", "Example Regular", "Example", "same-id");
    FontFamilies fonts{{duplicate, duplicate}};

    ASSERT_TRUE(FontCatalog::save(filename, fonts, "manifest"));
    auto loaded = FontCatalog::load(filename, "manifest");
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->size(), 1);
    EXPECT_EQ(loaded->front().size(), 1);
}

TEST(FontCatalogTest, AppliesVariableAxesToCachedFontSpecification)
{
    auto variable = face("Example Variable", "Regular", "Example Variable Regular",
                         "Example Variable", "variable");
    variable.variations = "wght=650";

    auto const specification = font_specification(variable);
    EXPECT_NE(specification.raw().find("wght=650"), std::string::npos);
}

} // namespace
