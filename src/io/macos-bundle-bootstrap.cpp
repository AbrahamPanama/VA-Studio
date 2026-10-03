// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * macos-bundle-bootstrap.cpp - native resource bootstrap for internal-test
 * macOS application bundles.
 *//*
 * Copyright (C) 2026 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 * Internal-test bundles are entered directly through CFBundleExecutable
 * (inkscape-bin). They must therefore reproduce the environment that the
 * internal shell launcher used to establish, before GTK/GLib cache any XDG,
 * font, schema or loader lookup. The variable set and the cache regeneration
 * deliberately match packaging/macos/vacards/internal-inkscape-launcher.sh.
 */

#include "macos-bundle-bootstrap.h"

#ifdef __APPLE__

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <unistd.h>

#include <string>

#include "path-prefix.h"

namespace Inkscape::IO {
namespace {

char const *const kMarkerFile         = "VACARDS-INTERNAL-TEST.json";
char const *const kDefaultProfileRel  = "Library/Application Support/Inkscape by VACards Test";
char const *const kSvgLoaderFile      = "libpixbufloader_svg.so";
char const *const kTiffLoaderFile     = "libpixbufloader-tiff.so";
char const *const kSvgMime            = "image/svg+xml";
char const *const kTiffMime           = "image/tiff";
char const *const kFileChooserSchema  = "org.gtk.gtk4.Settings.FileChooser";

std::string join(char const *base, char const *leaf)
{
    char *joined = g_build_filename(base, leaf, nullptr);
    std::string result(joined);
    g_free(joined);
    return result;
}

/// Print a concrete diagnostic and return false so the caller fails closed.
bool fail(std::string const &resource, std::string const &detail)
{
    g_printerr("VA Studio internal bundle: %s: %s\n", resource.c_str(), detail.c_str());
    return false;
}

bool require_readable_file(std::string const &path, char const *what)
{
    if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR) || g_access(path.c_str(), R_OK) != 0) {
        return fail(what, path + " is missing or unreadable");
    }
    return true;
}

bool require_readable_dir(std::string const &path, char const *what)
{
    if (!g_file_test(path.c_str(), G_FILE_TEST_IS_DIR) || g_access(path.c_str(), R_OK) != 0) {
        return fail(what, path + " is missing or unreadable");
    }
    return true;
}

} // namespace

bool init_macos_bundle_resources(char const *macosdir)
{
    std::string macos;
    if (macosdir) {
        macos = macosdir;
    } else if (char const *program_dir = get_program_dir()) {
        macos = program_dir;
    }
    if (macos.empty()) {
        // Cannot determine the bundle location; leave the legacy bootstrap alone.
        return true;
    }

    // Only the real bundle layout <...>/Contents/MacOS/<executable> may activate
    // this bootstrap. Enforce the canonical component names before the marker,
    // so a stray VACARDS-INTERNAL-TEST.json under some other layout cannot take
    // over the environment. Non-bundle layouts stay a no-op.
    char *macos_canon_raw = g_canonicalize_filename(macos.c_str(), nullptr);
    char *macos_leaf = g_path_get_basename(macos_canon_raw);
    char *macos_parent = g_path_get_dirname(macos_canon_raw);
    char *macos_parent_leaf = g_path_get_basename(macos_parent);
    bool const is_bundle_layout = g_strcmp0(macos_leaf, "MacOS") == 0 &&
                                  g_strcmp0(macos_parent_leaf, "Contents") == 0;
    g_free(macos_leaf);
    g_free(macos_parent_leaf);
    g_free(macos_parent);
    g_free(macos_canon_raw);
    if (!is_bundle_layout) {
        return true;
    }

    char *resources_raw = g_build_filename(macos.c_str(), "..", "Resources", nullptr);
    char *resources_canon = g_canonicalize_filename(resources_raw, nullptr);
    g_free(resources_raw);
    std::string resources(resources_canon);
    g_free(resources_canon);

    // Activation marker: only real internal-test bundles are configured here.
    // Any other layout (development, upstream, non-bundle) stays a no-op so
    // set_xdg_env() keeps its existing behavior.
    std::string marker = join(resources.c_str(), kMarkerFile);
    if (!g_file_test(marker.c_str(), G_FILE_TEST_IS_REGULAR)) {
        return true;
    }

    // 1. Profile: preserve an explicit INKSCAPE_PROFILE_DIR, otherwise use the
    //    same per-test directory as the internal shell launcher.
    std::string profile;
    if (char const *explicit_profile = g_getenv("INKSCAPE_PROFILE_DIR"); explicit_profile && *explicit_profile) {
        profile = explicit_profile;
    } else {
        char const *home = g_getenv("HOME");
        if (!home || !*home) {
            return fail("profile directory", "HOME is not set");
        }
        profile = join(home, kDefaultProfileRel);
    }
    if (g_mkdir_with_parents(profile.c_str(), 0755) != 0) {
        return fail("profile directory", profile + " cannot be created");
    }

    std::string moduledir  = join(resources.c_str(), "lib/gdk-pixbuf-2.0/2.10.0/loaders");
    std::string cache      = join(profile.c_str(), "gdk-pixbuf-loaders.cache");
    std::string gio_dir    = join(resources.c_str(), "lib/gio/modules");
    std::string schemas    = join(resources.c_str(), "share/glib-2.0/schemas");
    std::string fonts_file = join(resources.c_str(), "etc/fonts/fonts.conf");
    std::string fonts_path = join(resources.c_str(), "etc/fonts");
    std::string share      = join(resources.c_str(), "share");

    // 2. Install the complete environment before any GTK/GLib cached lookup.
    g_setenv("INKSCAPE_PROFILE_DIR", profile.c_str(), TRUE);
    g_setenv("INKSCAPE_APP_ID_TAG", "vacards_test", TRUE);
    g_setenv("GDK_PIXBUF_MODULEDIR", moduledir.c_str(), TRUE);
    g_setenv("GDK_PIXBUF_MODULE_FILE", cache.c_str(), TRUE);
    g_setenv("GIO_MODULE_DIR", gio_dir.c_str(), TRUE);
    g_setenv("GSETTINGS_SCHEMA_DIR", schemas.c_str(), TRUE);
    g_setenv("XDG_DATA_DIRS", share.c_str(), TRUE);
    g_setenv("FONTCONFIG_FILE", fonts_file.c_str(), TRUE);
    g_setenv("FONTCONFIG_PATH", fonts_path.c_str(), TRUE);
    g_setenv("PATH", (macos + ":/usr/bin:/bin:/usr/sbin:/sbin").c_str(), TRUE);

    // 3. Validate the required bundled resources. Fail closed; never fall back
    //    to host loaders/fonts/schemas.
    if (!require_readable_file(join(schemas.c_str(), "gschemas.compiled"), "GLib schemas")) {
        return false;
    }
    GError *schema_error = nullptr;
    GSettingsSchemaSource *source =
        g_settings_schema_source_new_from_directory(schemas.c_str(), nullptr, TRUE, &schema_error);
    if (!source) {
        std::string detail = schema_error ? schema_error->message : std::string("unknown error");
        g_clear_error(&schema_error);
        return fail("GLib schemas", detail);
    }
    GSettingsSchema *schema = g_settings_schema_source_lookup(source, kFileChooserSchema, TRUE);
    if (!schema) {
        g_settings_schema_source_unref(source);
        return fail("GLib schemas", std::string(kFileChooserSchema) + " is not registered");
    }
    g_settings_schema_unref(schema);
    g_settings_schema_source_unref(source);

    if (!require_readable_file(fonts_file, "fontconfig")) {
        return false;
    }
    if (!require_readable_dir(fonts_path, "fontconfig directory")) {
        return false;
    }
    std::string svg_loader  = join(moduledir.c_str(), kSvgLoaderFile);
    std::string tiff_loader = join(moduledir.c_str(), kTiffLoaderFile);
    if (!require_readable_file(svg_loader, "GdkPixbuf SVG loader")) {
        return false;
    }
    if (!require_readable_file(tiff_loader, "GdkPixbuf TIFF loader")) {
        return false;
    }

    // 4. Regenerate the cache from the bundled modules with an explicit argv and
    //    no shell. The bundled tool must register both SVG and TIFF.
    std::string query = join(macos.c_str(), "gdk-pixbuf-query-loaders");
    if (!g_file_test(query.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
        return fail("GdkPixbuf query tool", query + " is missing or not executable");
    }
    gchar *argv[] = {const_cast<gchar *>(query.c_str()),
                     const_cast<gchar *>(svg_loader.c_str()),
                     const_cast<gchar *>(tiff_loader.c_str()),
                     nullptr};
    gchar *output = nullptr;
    gint wait_status = 0;
    GError *spawn_error = nullptr;
    if (!g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                      &output, nullptr, &wait_status, &spawn_error)) {
        std::string detail = spawn_error ? spawn_error->message : std::string("unknown error");
        g_clear_error(&spawn_error);
        return fail("GdkPixbuf query tool", detail);
    }
    std::string cache_text = output ? output : "";
    g_free(output);
    if (wait_status != 0) {
        GError *status_error = nullptr;
        g_spawn_check_wait_status(wait_status, &status_error);
        std::string detail = status_error ? status_error->message : std::string("unknown error");
        g_clear_error(&status_error);
        return fail("GdkPixbuf query tool", detail);
    }
    if (cache_text.find(kSvgLoaderFile) == std::string::npos ||
        cache_text.find(kSvgMime) == std::string::npos) {
        return fail("GdkPixbuf cache", "the bundled SVG loader was not registered");
    }
    if (cache_text.find(kTiffLoaderFile) == std::string::npos ||
        cache_text.find(kTiffMime) == std::string::npos) {
        return fail("GdkPixbuf cache", "the bundled TIFF loader was not registered");
    }

    // 5. Write the complete validated cache with a checked file helper.
    GError *write_error = nullptr;
    if (!g_file_set_contents(cache.c_str(), cache_text.data(), (gssize)cache_text.size(), &write_error)) {
        std::string detail = write_error ? write_error->message : std::string("unknown error");
        g_clear_error(&write_error);
        return fail("GdkPixbuf cache", cache + ": " + detail);
    }

    return true;
}

} // namespace Inkscape::IO

#endif // __APPLE__
