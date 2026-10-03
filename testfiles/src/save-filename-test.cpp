// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Regression tests for the Save / Save As / Save Copy filename normalization
 * (BUG-002: a suffixless native-chooser return must become editable Inkscape
 * SVG; explicit known and unknown suffixes must be preserved).
 *
 * These exercise the production helper declared in src/io/save-filename.h; the
 * helper logic is not copied or re-implemented here.
 */

#include <gtest/gtest.h>

#include <string>

#include "io/save-filename.h"

namespace {

using Inkscape::IO::append_save_extension_if_missing;

struct Normalized {
    bool appended;
    std::string path;
};

Normalized normalize(std::string path, std::string const &ext = ".svg")
{
    bool const appended = append_save_extension_if_missing(path, ext);
    return {appended, path};
}

} // namespace

TEST(SaveFilename, AppendsDefaultExtensionToSuffixlessBasename)
{
    auto const r = normalize("drawing");
    EXPECT_TRUE(r.appended);
    EXPECT_EQ(r.path, "drawing.svg");
}

TEST(SaveFilename, NeverDuplicatesAnExistingSuffix)
{
    for (auto const *name : {"drawing.svg", "drawing.SVG", "drawing.svgz"}) {
        auto const r = normalize(name);
        EXPECT_FALSE(r.appended) << name;
        EXPECT_EQ(r.path, name) << name;
        EXPECT_EQ(r.path.find(".svg.svg"), std::string::npos) << name;
    }
}

TEST(SaveFilename, PreservesExplicitKnownAndUnknownSuffixes)
{
    for (auto const *name : {"my.drawing", "archive.tar.gz", "name."}) {
        auto const r = normalize(name);
        EXPECT_FALSE(r.appended) << name;
        EXPECT_EQ(r.path, name) << name;
    }
}

TEST(SaveFilename, IsBasenameAwareForDottedDirectories)
{
    auto const r = normalize("/tmp/x.y/name");
    EXPECT_TRUE(r.appended);
    EXPECT_EQ(r.path, "/tmp/x.y/name.svg");

    auto const already = normalize("/tmp/x.y/name.svg");
    EXPECT_FALSE(already.appended);
    EXPECT_EQ(already.path, "/tmp/x.y/name.svg");
}

TEST(SaveFilename, TreatsLeadingDotAsHiddenFileNotExtension)
{
    auto const r = normalize(".hidden");
    EXPECT_TRUE(r.appended);
    EXPECT_EQ(r.path, ".hidden.svg");
}

TEST(SaveFilename, HandlesUnicodeBasenames)
{
    auto const r = normalize("caf\xC3\xA9");
    EXPECT_TRUE(r.appended);
    EXPECT_EQ(r.path, "caf\xC3\xA9.svg");

    auto const already = normalize("caf\xC3\xA9.svg");
    EXPECT_FALSE(already.appended);
    EXPECT_EQ(already.path, "caf\xC3\xA9.svg");
}

TEST(SaveFilename, NeverInventsAPathForEmptyInputs)
{
    // A non-local/unmounted URI yields an empty get_path(); it must stay empty
    // instead of becoming a relative ".svg" in the current directory.
    auto const empty_path = normalize("");
    EXPECT_FALSE(empty_path.appended);
    EXPECT_EQ(empty_path.path, "");

    auto const empty_ext = normalize("drawing", "");
    EXPECT_FALSE(empty_ext.appended);
    EXPECT_EQ(empty_ext.path, "drawing");
}

TEST(SaveFilename, IsIdempotentAndNeverProducesSvgSvg)
{
    auto first = normalize("no-extension");
    ASSERT_TRUE(first.appended);
    EXPECT_EQ(first.path, "no-extension.svg");

    auto second = normalize(first.path);
    EXPECT_FALSE(second.appended);
    EXPECT_EQ(second.path, "no-extension.svg");
    EXPECT_EQ(second.path.find(".svg.svg"), std::string::npos);
}

TEST(SaveFilename, NeverInventsATargetForDirectoryOrDotPaths)
{
    // A trailing directory separator means the chooser returned a directory,
    // not a file; appending would invent a hidden "/tmp/dir/.svg" or "/.svg".
    for (auto const *dir : {"/tmp/dir/", "dir/", "/", "///"}) {
        auto const r = normalize(dir);
        EXPECT_FALSE(r.appended) << dir;
        EXPECT_EQ(r.path, dir) << dir;
    }

    // "." and ".." are directory references, not suffixless filenames.
    for (auto const *name : {".", "..", "dir/.", "dir/.."}) {
        auto const r = normalize(name);
        EXPECT_FALSE(r.appended) << name;
        EXPECT_EQ(r.path, name) << name;
    }
}

TEST(SaveFilename, AllowsSuffixlessHiddenFilenameInDottedDirectory)
{
    // A leading dot inside a dotted parent is still a hidden filename, so it
    // receives the suffix; only a bare "."/".." basename is a directory.
    auto const r = normalize("/tmp/x.y/.hidden");
    EXPECT_TRUE(r.appended);
    EXPECT_EQ(r.path, "/tmp/x.y/.hidden.svg");
}
