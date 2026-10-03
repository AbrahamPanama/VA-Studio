// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * macos-bundle-bootstrap-test.cpp - outcome tests for the internal-test
 * macOS bundle resource bootstrap.
 *//*
 * Copyright (C) 2026 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 * These cases invoke the real Inkscape::IO::init_macos_bundle_resources() with
 * an explicit Contents/MacOS directory inside a throwaway app-shaped layout.
 * A synthetic gdk-pixbuf-query-loaders exercises the actual spawn and the
 * cache the helper writes. No installed app, prefix or user profile is touched.
 */

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "io/macos-bundle-bootstrap.h"

namespace {

char const *const kEnvNames[] = {
    "HOME",
    "INKSCAPE_PROFILE_DIR",
    "INKSCAPE_APP_ID_TAG",
    "GDK_PIXBUF_MODULEDIR",
    "GDK_PIXBUF_MODULE_FILE",
    "GIO_MODULE_DIR",
    "GSETTINGS_SCHEMA_DIR",
    "XDG_DATA_DIRS",
    "FONTCONFIG_FILE",
    "FONTCONFIG_PATH",
    "PATH",
};

/// Save and restore the environment the helper mutates, so cases stay isolated.
class ScopedEnv
{
public:
    ScopedEnv()
    {
        for (char const *name : kEnvNames) {
            char const *value = g_getenv(name);
            entries_.push_back({name, value ? std::string(value) : std::string(), value != nullptr});
        }
    }

    ~ScopedEnv()
    {
        for (auto const &entry : entries_) {
            if (entry.had) {
                g_setenv(entry.name, entry.value.c_str(), TRUE);
            } else {
                g_unsetenv(entry.name);
            }
        }
    }

    ScopedEnv(ScopedEnv const &) = delete;
    ScopedEnv &operator=(ScopedEnv const &) = delete;

private:
    struct Entry
    {
        char const *name;
        std::string value;
        bool had;
    };
    std::vector<Entry> entries_;
};

char const *const kSchemaXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<schemalist>\n"
    "  <schema id=\"org.gtk.gtk4.Settings.FileChooser\" path=\"/org/gtk/gtk4/settings/file-chooser/\">\n"
    "    <key name=\"test-enabled\" type=\"b\"><default>false</default></key>\n"
    "  </schema>\n"
    "</schemalist>\n";

// Synthetic query tool: registers SVG+TIFF from the explicit argv and quotes the
// module path exactly like the real tool, so spaces/non-ASCII paths are visible.
char const *const kQueryOk =
    "#!/bin/sh\n"
    "for path in \"$@\"; do\n"
    "  case \"$path\" in\n"
    "    *libpixbufloader_svg.so)\n"
    "      printf '\"%s\"\\n\"svg\" 5 \"gdk-pixbuf\" \"SVG\" \"LGPL\"\\n\"image/svg+xml\"\\n\"svg\" \"svgz\"\\n' \"$path\" ;;\n"
    "    *libpixbufloader-tiff.so)\n"
    "      printf '\"%s\"\\n\"tiff\" 5 \"gdk-pixbuf\" \"TIFF\" \"LGPL\"\\n\"image/tiff\"\\n\"tiff\" \"tif\"\\n' \"$path\" ;;\n"
    "  esac\n"
    "done\n";

char const *const kQueryFail = "#!/bin/sh\nexit 3\n";

std::string join2(std::string const &base, std::string const &leaf)
{
    char *joined = g_build_filename(base.c_str(), leaf.c_str(), nullptr);
    std::string result(joined);
    g_free(joined);
    return result;
}

void write_text(std::string const &path, std::string const &text, bool executable = false)
{
    char *dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_file_set_contents(path.c_str(), text.data(), (gssize)text.size(), nullptr);
    if (executable) {
        g_chmod(path.c_str(), 0755);
    }
}

void remove_tree(std::string const &path)
{
    if (path.empty()) {
        return;
    }
    GDir *dir = g_dir_open(path.c_str(), 0, nullptr);
    if (dir) {
        while (gchar const *name = g_dir_read_name(dir)) {
            std::string child = join2(path, name);
            if (g_file_test(child.c_str(), G_FILE_TEST_IS_DIR)) {
                remove_tree(child);
            } else {
                g_remove(child.c_str());
            }
        }
        g_dir_close(dir);
    }
    g_rmdir(path.c_str());
}

bool run_schema_compiler(std::string const &compiler, std::string const &directory)
{
    gchar *argv[] = {const_cast<gchar *>(compiler.c_str()), const_cast<gchar *>(directory.c_str()), nullptr};
    gint wait_status = 0;
    GError *error = nullptr;
    if (!g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                      nullptr, nullptr, &wait_status, &error)) {
        g_clear_error(&error);
        return false;
    }
    return wait_status == 0;
}

std::string read_file(std::string const &path)
{
    gchar *contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr)) {
        return {};
    }
    std::string result(contents, length);
    g_free(contents);
    return result;
}

struct Bundle
{
    std::string app;
    std::string macos;
    std::string resources;
    std::string moduledir;
    std::string fonts;
    std::string schemas;

    std::string cache_for(std::string const &profile) const
    {
        return join2(profile, "gdk-pixbuf-loaders.cache");
    }
};

class MacosBundleBootstrap : public ::testing::Test
{
protected:
    void SetUp() override
    {
        GError *error = nullptr;
        gchar *tmp = g_dir_make_tmp("vacards-bundle-bootstrap-XXXXXX", &error);
        ASSERT_NE(tmp, nullptr) << (error ? error->message : "g_dir_make_tmp failed");
        root_ = tmp;
        g_free(tmp);

        gchar *compiler = g_find_program_in_path("glib-compile-schemas");
        ASSERT_NE(compiler, nullptr) << "glib-compile-schemas is required for the schema fixture";
        schema_compiler_ = compiler;
        g_free(compiler);

        schema_template_ = join2(root_, "schema-template");
        write_text(join2(schema_template_, "vacards-test.gschema.xml"), kSchemaXml);
        ASSERT_TRUE(run_schema_compiler(schema_compiler_, schema_template_))
            << "glib-compile-schemas failed";
        ASSERT_TRUE(g_file_test(join2(schema_template_, "gschemas.compiled").c_str(), G_FILE_TEST_IS_REGULAR));
    }

    void TearDown() override { remove_tree(root_); }

    /// Build a fully valid internal-test app-shaped layout (marker included).
    Bundle make_bundle(std::string const &suffix)
    {
        Bundle bundle;
        bundle.app = join2(root_, "VA Studio Test" + suffix + ".app");
        std::string contents = join2(bundle.app, "Contents");
        bundle.macos = join2(contents, "MacOS");
        bundle.resources = join2(contents, "Resources");
        g_mkdir_with_parents(bundle.macos.c_str(), 0755);
        g_mkdir_with_parents(bundle.resources.c_str(), 0755);
        write_text(join2(bundle.resources, "VACARDS-INTERNAL-TEST.json"), "{}\n");
        bundle.moduledir = join2(bundle.resources, "lib/gdk-pixbuf-2.0/2.10.0/loaders");
        write_text(join2(bundle.moduledir, "libpixbufloader_svg.so"), "synthetic svg\n");
        write_text(join2(bundle.moduledir, "libpixbufloader-tiff.so"), "synthetic tiff\n");
        bundle.fonts = join2(bundle.resources, "etc/fonts");
        write_text(join2(bundle.fonts, "fonts.conf"), "<fontconfig/>\n");
        bundle.schemas = join2(bundle.resources, "share/glib-2.0/schemas");
        write_text(join2(bundle.schemas, "gschemas.compiled"),
                   read_file(join2(schema_template_, "gschemas.compiled")));
        write_text(join2(bundle.macos, "gdk-pixbuf-query-loaders"), kQueryOk, true);
        return bundle;
    }

    std::string make_home()
    {
        std::string home = join2(root_, "home");
        g_mkdir_with_parents(home.c_str(), 0755);
        return home;
    }

    ScopedEnv env_guard_;
    std::string root_;
    std::string schema_template_;
    std::string schema_compiler_;
};

TEST_F(MacosBundleBootstrap, NonBundleLayoutIsNoOp)
{
    std::string macos = join2(root_, "plain/Contents/MacOS");
    g_mkdir_with_parents(macos.c_str(), 0755);
    g_setenv("GDK_PIXBUF_MODULEDIR", "/sentinel/modules", TRUE);
    g_setenv("INKSCAPE_PROFILE_DIR", "/sentinel/profile", TRUE);

    EXPECT_TRUE(Inkscape::IO::init_macos_bundle_resources(macos.c_str()));
    EXPECT_STREQ(g_getenv("GDK_PIXBUF_MODULEDIR"), "/sentinel/modules");
    EXPECT_STREQ(g_getenv("INKSCAPE_PROFILE_DIR"), "/sentinel/profile");
    EXPECT_FALSE(g_file_test("/sentinel/profile", G_FILE_TEST_EXISTS));
}

TEST_F(MacosBundleBootstrap, MarkedWrongLayoutIsNoOp)
{
    // A marker alone is not enough: the canonical layout must be
    // <...>/Contents/MacOS. A marked directory whose leaf is not MacOS, or whose
    // parent is not Contents, must stay a no-op and must not touch the env.
    std::string wrong_leaf = join2(root_, "wrong-layout/Contents/NotMacOS");
    std::string wrong_leaf_resources = join2(root_, "wrong-layout/Contents/Resources");
    g_mkdir_with_parents(wrong_leaf.c_str(), 0755);
    g_mkdir_with_parents(wrong_leaf_resources.c_str(), 0755);
    write_text(join2(wrong_leaf_resources, "VACARDS-INTERNAL-TEST.json"), "{}\n");
    g_setenv("GDK_PIXBUF_MODULEDIR", "/sentinel/wrong-leaf", TRUE);
    g_setenv("INKSCAPE_PROFILE_DIR", "/sentinel/wrong-leaf-profile", TRUE);

    EXPECT_TRUE(Inkscape::IO::init_macos_bundle_resources(wrong_leaf.c_str()));
    EXPECT_STREQ(g_getenv("GDK_PIXBUF_MODULEDIR"), "/sentinel/wrong-leaf");
    EXPECT_STREQ(g_getenv("INKSCAPE_PROFILE_DIR"), "/sentinel/wrong-leaf-profile");
    EXPECT_FALSE(g_file_test("/sentinel/wrong-leaf-profile", G_FILE_TEST_EXISTS));

    std::string wrong_parent = join2(root_, "wrong-parent/NotContents/MacOS");
    std::string wrong_parent_resources = join2(root_, "wrong-parent/NotContents/Resources");
    g_mkdir_with_parents(wrong_parent.c_str(), 0755);
    g_mkdir_with_parents(wrong_parent_resources.c_str(), 0755);
    write_text(join2(wrong_parent_resources, "VACARDS-INTERNAL-TEST.json"), "{}\n");
    g_setenv("INKSCAPE_PROFILE_DIR", "/sentinel/wrong-parent-profile", TRUE);

    EXPECT_TRUE(Inkscape::IO::init_macos_bundle_resources(wrong_parent.c_str()));
    EXPECT_STREQ(g_getenv("INKSCAPE_PROFILE_DIR"), "/sentinel/wrong-parent-profile");
    EXPECT_FALSE(g_file_test("/sentinel/wrong-parent-profile", G_FILE_TEST_EXISTS));
}

TEST_F(MacosBundleBootstrap, MarkerAloneIsNotEnoughWhenResourcesMissing)
{
    std::string macos = join2(root_, "marker-only/Contents/MacOS");
    std::string resources = join2(root_, "marker-only/Contents/Resources");
    g_mkdir_with_parents(macos.c_str(), 0755);
    g_mkdir_with_parents(resources.c_str(), 0755);
    write_text(join2(resources, "VACARDS-INTERNAL-TEST.json"), "{}\n");
    g_setenv("HOME", make_home().c_str(), TRUE);
    g_unsetenv("INKSCAPE_PROFILE_DIR");

    EXPECT_FALSE(Inkscape::IO::init_macos_bundle_resources(macos.c_str()));
}

TEST_F(MacosBundleBootstrap, ValidBundleConfiguresEnvironmentAndCache)
{
    Bundle bundle = make_bundle("-valid");
    std::string home = make_home();
    g_setenv("HOME", home.c_str(), TRUE);
    g_setenv("INKSCAPE_PROFILE_DIR", "", TRUE);

    ASSERT_TRUE(Inkscape::IO::init_macos_bundle_resources(bundle.macos.c_str()));

    std::string profile = join2(join2(home, "Library"), "Application Support/Inkscape by VACards Test");
    EXPECT_STREQ(g_getenv("INKSCAPE_PROFILE_DIR"), profile.c_str());
    EXPECT_STREQ(g_getenv("INKSCAPE_APP_ID_TAG"), "vacards_test");
    EXPECT_STREQ(g_getenv("GDK_PIXBUF_MODULEDIR"), bundle.moduledir.c_str());
    EXPECT_STREQ(g_getenv("GDK_PIXBUF_MODULE_FILE"), bundle.cache_for(profile).c_str());
    EXPECT_STREQ(g_getenv("GSETTINGS_SCHEMA_DIR"), bundle.schemas.c_str());
    EXPECT_STREQ(g_getenv("FONTCONFIG_FILE"), join2(bundle.fonts, "fonts.conf").c_str());
    EXPECT_STREQ(g_getenv("FONTCONFIG_PATH"), bundle.fonts.c_str());
    EXPECT_STREQ(g_getenv("XDG_DATA_DIRS"), join2(bundle.resources, "share").c_str());
    EXPECT_EQ(std::string(g_getenv("PATH")).find(bundle.macos + ":"), 0u);

    std::string cache = read_file(bundle.cache_for(profile));
    EXPECT_NE(cache.find("libpixbufloader_svg.so"), std::string::npos);
    EXPECT_NE(cache.find("image/svg+xml"), std::string::npos);
    EXPECT_NE(cache.find("libpixbufloader-tiff.so"), std::string::npos);
    EXPECT_NE(cache.find("image/tiff"), std::string::npos);
}

TEST_F(MacosBundleBootstrap, ExplicitProfileIsPreserved)
{
    Bundle bundle = make_bundle("-explicit");
    std::string profile = join2(root_, "custom-profile");
    g_mkdir_with_parents(profile.c_str(), 0755);
    g_setenv("INKSCAPE_PROFILE_DIR", profile.c_str(), TRUE);
    g_setenv("HOME", make_home().c_str(), TRUE);

    ASSERT_TRUE(Inkscape::IO::init_macos_bundle_resources(bundle.macos.c_str()));

    EXPECT_STREQ(g_getenv("INKSCAPE_PROFILE_DIR"), profile.c_str());
    EXPECT_NE(read_file(bundle.cache_for(profile)).find("image/tiff"), std::string::npos);
}

TEST_F(MacosBundleBootstrap, ConfiguresSpacesAndNonAsciiPath)
{
    Bundle bundle = make_bundle(" With Space \xc3\xa9");
    std::string home = make_home();
    g_setenv("HOME", home.c_str(), TRUE);
    g_unsetenv("INKSCAPE_PROFILE_DIR");

    ASSERT_TRUE(Inkscape::IO::init_macos_bundle_resources(bundle.macos.c_str()));

    EXPECT_NE(std::string(g_getenv("GDK_PIXBUF_MODULEDIR")).find("Space"), std::string::npos);
    std::string cache = read_file(bundle.cache_for(g_getenv("INKSCAPE_PROFILE_DIR")));
    EXPECT_NE(cache.find(bundle.moduledir), std::string::npos);
    EXPECT_NE(cache.find("image/tiff"), std::string::npos);
}

TEST_F(MacosBundleBootstrap, MissingTiffLoaderFailsClosed)
{
    Bundle bundle = make_bundle("-no-tiff");
    g_remove(join2(bundle.moduledir, "libpixbufloader-tiff.so").c_str());
    std::string profile = join2(root_, "no-tiff-profile");
    g_setenv("INKSCAPE_PROFILE_DIR", profile.c_str(), TRUE);

    EXPECT_FALSE(Inkscape::IO::init_macos_bundle_resources(bundle.macos.c_str()));
    EXPECT_FALSE(g_file_test(bundle.cache_for(profile).c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(MacosBundleBootstrap, MissingSvgLoaderFailsClosed)
{
    Bundle bundle = make_bundle("-no-svg");
    g_remove(join2(bundle.moduledir, "libpixbufloader_svg.so").c_str());
    g_setenv("INKSCAPE_PROFILE_DIR", join2(root_, "no-svg-profile").c_str(), TRUE);

    EXPECT_FALSE(Inkscape::IO::init_macos_bundle_resources(bundle.macos.c_str()));
}

TEST_F(MacosBundleBootstrap, QueryToolFailureFailsClosed)
{
    Bundle bundle = make_bundle("-query-fail");
    std::string profile = join2(root_, "query-fail-profile");
    g_setenv("INKSCAPE_PROFILE_DIR", profile.c_str(), TRUE);
    write_text(join2(bundle.macos, "gdk-pixbuf-query-loaders"), kQueryFail, true);

    EXPECT_FALSE(Inkscape::IO::init_macos_bundle_resources(bundle.macos.c_str()));
    EXPECT_FALSE(g_file_test(bundle.cache_for(profile).c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(MacosBundleBootstrap, CacheWriteFailureFailsClosed)
{
    Bundle bundle = make_bundle("-cache-fail");
    std::string profile = join2(root_, "cache-fail-profile");
    g_mkdir_with_parents(profile.c_str(), 0755);
    g_setenv("INKSCAPE_PROFILE_DIR", profile.c_str(), TRUE);
    // A directory at the cache path makes g_file_set_contents fail deterministically.
    g_mkdir_with_parents(bundle.cache_for(profile).c_str(), 0755);

    EXPECT_FALSE(Inkscape::IO::init_macos_bundle_resources(bundle.macos.c_str()));
}

TEST_F(MacosBundleBootstrap, MissingCompiledSchemasFailsClosed)
{
    Bundle bundle = make_bundle("-no-schemas");
    g_remove(join2(bundle.schemas, "gschemas.compiled").c_str());
    g_setenv("INKSCAPE_PROFILE_DIR", join2(root_, "no-schemas-profile").c_str(), TRUE);

    EXPECT_FALSE(Inkscape::IO::init_macos_bundle_resources(bundle.macos.c_str()));
}

} // namespace
