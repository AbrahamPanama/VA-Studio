// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <glibmm/miscutils.h>
#include <gio/gio.h>
#include <map>
#include "io/export-destination.h"

namespace ED = Inkscape::IO::ExportDestination;
namespace {
std::string path(std::string const &name) { return Glib::build_filename(Glib::get_tmp_dir(), name); }
bool await(std::function<bool()> const &complete)
{
    auto deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (!complete() && g_get_monotonic_time() < deadline) {
        for (unsigned i = 0; i < 16; ++i) g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    return complete();
}
std::string missing_directory()
{
    auto uuid = g_uuid_string_random();
    auto result = path(std::string("vacards-export-missing-") + uuid);
    g_free(uuid);
    return result;
}
}

TEST(ExportDestination, SourceNamesAndImportedProvenance)
{
    EXPECT_EQ(ED::basename(path("Invitacion.svg"), "bitmap"), "Invitacion");
    EXPECT_EQ(ED::basename(path("Cliente.revision.2.cdr"), "bitmap"), "Cliente.revision.2");
    EXPECT_EQ(ED::basename(path("Véronica 测试.PDF"), "bitmap"), "Véronica 测试");
    EXPECT_EQ(ED::basename("", "bitmap"), "bitmap");
    EXPECT_EQ(ED::basename(path(".hidden"), "bitmap"), ".hidden");
}

TEST(ExportDestination, RegistryFormatsAndAliases)
{
    EXPECT_EQ(ED::format_key("image/tiff", ".tif"), ED::format_key("image/tiff", ".TIFF"));
    EXPECT_EQ(ED::format_key("image/svg+xml", ".svg"), ED::format_key("image/svg+xml", ".SVGZ"));
    EXPECT_NE(ED::format_key("image/png", ".png"), ED::format_key("application/pdf", ".pdf"));
    EXPECT_NE(ED::format_key("application/postscript", ".ps"), ED::format_key("application/postscript", ".eps"));
    EXPECT_EQ(ED::format_key("image/png", ".PNG"), ED::format_key("image/png", ".png"));
    EXPECT_EQ(ED::format_key("", "../evil").find('/'), std::string::npos);
}

TEST(ExportDestination, IndependentManualFolderAndBasename)
{
    ED::Session folder;
    folder.folder = path("original");
    folder.refresh(path("Cliente.revision.2.cdr"), "bitmap", ".tiff");
    folder.edit(Glib::build_filename(path("chosen"), "Cliente.revision.2.tiff"), ".tiff");
    EXPECT_TRUE(folder.user_folder);
    EXPECT_FALSE(folder.user_name);
    folder.refresh(path("Saved.As.svg"), "bitmap", ".pdf");
    EXPECT_EQ(folder.path(), Glib::build_filename(path("chosen"), "Saved.As.pdf"));

    ED::Session name;
    name.folder = path("original");
    name.refresh(path("Original.svg"), "bitmap", ".tiff");
    name.edit(Glib::build_filename(name.folder, "Manual.name.tif"), ".tiff");
    EXPECT_FALSE(name.user_folder);
    EXPECT_TRUE(name.user_name);
    name.refresh(path("Saved.As.svg"), "bitmap", ".png");
    name.folder = path("png-history"); // Async folder selection remains independent.
    EXPECT_EQ(name.path(), Glib::build_filename(path("png-history"), "Manual.name.png"));
}

TEST(ExportDestination, ChooserPinsBothComponentsAndSaveAsRefreshesOnlyAutomaticNames)
{
    ED::Session session;
    session.folder = path("outputs");
    session.refresh(path("Original.svg"), "bitmap", ".svg");
    auto chosen = session.path();
    session.edit(chosen, ".svg", true); // Even selecting the same suggested path is explicit.
    session.refresh(path("New.svg"), "bitmap", ".pdf");
    EXPECT_EQ(session.path(), Glib::build_filename(path("outputs"), "Original.pdf"));
    ED::Session automatic;
    automatic.refresh("", "bitmap", ".png");
    EXPECT_EQ(automatic.name, "bitmap");
    automatic.refresh(path("Now.Saved.svg"), "bitmap", ".png");
    EXPECT_EQ(automatic.name, "Now.Saved");
}

TEST(ExportDestination, DirectoryOrderIsLexicalAndDoesNotProbeOrCreate)
{
    auto source = Glib::build_filename(path("source"), "Drawing.svg");
    EXPECT_EQ(ED::directories(path("history"), source, path("fallback")),
              (std::vector<std::string>{path("history"), path("source"), path("fallback")}));
    EXPECT_EQ(ED::directories(path("source"), source, path("source")),
              (std::vector<std::string>{path("source")}));
    EXPECT_EQ(ED::directories("relative-history", "", path("fallback")),
              (std::vector<std::string>{path("fallback")}));
}

TEST(ExportDestination, OnlySuccessfulCompleteOutputsRememberDirectories)
{
    std::map<std::string, std::string> profile;
    auto remember = [&](auto const &key, auto const &directory) { profile[key] = directory; };
    auto tiff = ED::format_key("image/tiff", ".tif");
    auto svg = ED::format_key("image/svg+xml", ".svg");
    auto output = Glib::build_filename(path("TIFF"), "First.tiff");
    ED::record_result(tiff, output, false, false, remember);
    ED::record_result(tiff, output, true, true, remember);
    ED::record_result(tiff, "relative.tiff", true, false, remember);
    EXPECT_TRUE(profile.empty());
    ED::record_result(tiff, output, true, false, remember);
    ED::record_result(svg, Glib::build_filename(path("SVG"), "First.svg"), true, false, remember);
    EXPECT_EQ(profile.at(ED::preference_key(tiff)), path("TIFF"));
    EXPECT_EQ(profile.at(ED::preference_key(svg)), path("SVG"));
    ED::Session next_document;
    next_document.folder = profile.at(ED::preference_key(ED::format_key("image/tiff", ".tiff")));
    next_document.refresh(path("Otro.svg"), "bitmap", ".tiff");
    EXPECT_EQ(next_document.path(), Glib::build_filename(path("TIFF"), "Otro.tiff"));
    ED::record_result(tiff, path("Failed.tiff"), false, false, remember);
    EXPECT_EQ(profile.at(ED::preference_key(tiff)), path("TIFF"));
}

TEST(ExportDestination, SeparateSessionsDoNotRewriteEachOther)
{
    ED::Session first, second;
    first.folder = path("A"); second.folder = path("B");
    first.refresh(path("First.svg"), "bitmap", ".png");
    second.refresh(path("Second.svg"), "bitmap", ".png");
    first.edit(Glib::build_filename(path("manual"), "Chosen.png"), ".png");
    EXPECT_EQ(second.path(), Glib::build_filename(path("B"), "Second.png"));
}

TEST(ExportDestination, DestroyedDirectoryRequestDoesNotDeliverToUi)
{
    bool delivered = false;
    {
        ED::DirectoryRequest request({Glib::get_tmp_dir()}, [&](auto) { delivered = true; });
    }
    // Cancelled completion is allowed to arrive later; it owns only pure data.
    for (int i = 0; i < 20; ++i) g_main_context_iteration(nullptr, false);
    EXPECT_FALSE(delivered);
}

TEST(ExportDestination, ValidDirectoryCompletesExactlyOnce)
{
    unsigned count = 0;
    std::string resolved;
    ED::DirectoryRequest request({Glib::get_tmp_dir()}, [&](auto folder) {
        ++count;
        resolved = std::move(folder);
    });
    ASSERT_TRUE(await([&] { return count != 0; }));
    EXPECT_EQ(count, 1);
    EXPECT_EQ(resolved, Glib::get_tmp_dir());
    for (unsigned i = 0; i < 32; ++i) g_main_context_iteration(nullptr, false);
    EXPECT_EQ(count, 1);
}

TEST(ExportDestination, MissingDirectoryFallsBackWithoutCreatingIt)
{
    auto missing = missing_directory();
    ASSERT_FALSE(g_file_test(missing.c_str(), G_FILE_TEST_EXISTS));
    unsigned count = 0;
    std::string resolved;
    ED::DirectoryRequest request({missing, Glib::get_tmp_dir()}, [&](auto folder) {
        ++count;
        resolved = std::move(folder);
    });
    ASSERT_TRUE(await([&] { return count != 0; }));
    EXPECT_EQ(count, 1);
    EXPECT_EQ(resolved, Glib::get_tmp_dir());
    EXPECT_FALSE(g_file_test(missing.c_str(), G_FILE_TEST_EXISTS));
}

TEST(ExportDestination, UnavailableCandidatesCompleteWithoutFabricatingFolder)
{
    auto missing = missing_directory();
    unsigned count = 0;
    std::string resolved = "not completed";
    ED::DirectoryRequest request({missing, Glib::build_filename(missing, "child")}, [&](auto folder) {
        ++count;
        resolved = std::move(folder);
    });
    ASSERT_TRUE(await([&] { return count != 0; }));
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(resolved.empty());
    EXPECT_FALSE(g_file_test(missing.c_str(), G_FILE_TEST_EXISTS));
}

TEST(ExportDestination, SupersededRequestCannotPublishOverCurrentResult)
{
    unsigned old_count = 0, current_count = 0;
    auto stale = std::make_unique<ED::DirectoryRequest>(
        std::vector<std::string>{Glib::get_tmp_dir()}, [&](auto) { ++old_count; });
    stale.reset();
    ED::DirectoryRequest current({Glib::get_tmp_dir()}, [&](auto) { ++current_count; });
    ASSERT_TRUE(await([&] { return current_count != 0; }));
    EXPECT_EQ(current_count, 1);
    EXPECT_EQ(old_count, 0);
}

TEST(ExportDestination, EmptyCandidatesCompleteOnceWithoutSchedulingIo)
{
    unsigned count = 0;
    ED::DirectoryRequest request({}, [&](auto folder) {
        ++count;
        EXPECT_TRUE(folder.empty());
    });
    EXPECT_EQ(count, 1);
    for (unsigned i = 0; i < 32; ++i) g_main_context_iteration(nullptr, false);
    EXPECT_EQ(count, 1);
}
