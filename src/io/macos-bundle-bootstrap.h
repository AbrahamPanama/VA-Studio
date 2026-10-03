// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * macos-bundle-bootstrap.h - native resource bootstrap for internal-test
 * macOS application bundles.
 *//*
 * Copyright (C) 2026 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_IO_MACOS_BUNDLE_BOOTSTRAP_H
#define INKSCAPE_IO_MACOS_BUNDLE_BOOTSTRAP_H

#ifdef __APPLE__

namespace Inkscape::IO {

/**
 * Configure the process environment and regenerate the GdkPixbuf loader cache
 * for an internal-test .app bundle, before any GLib/GTK cached state exists.
 *
 * Only an actual internal-test bundle (a Contents/MacOS directory with a
 * sibling Contents/Resources/VACARDS-INTERNAL-TEST.json marker) is configured.
 * Development, upstream and non-bundle layouts are a no-op, so the existing
 * set_xdg_env() legacy handling still owns those cases.
 *
 * The caller's explicit INKSCAPE_PROFILE_DIR is preserved. Required bundled
 * resources (schemas, fonts, GdkPixbuf loaders) are validated and the loader
 * cache is regenerated from the bundled query tool. There is no host-loader
 * fallback: a missing or unreadable resource fails closed.
 *
 * @param macosdir Contents/MacOS directory, or nullptr to use get_program_dir().
 * @return true when the layout was a no-op or was configured successfully;
 *         false when a required bundled resource failed, in which case the
 *         caller must stop startup with a non-zero status.
 */
bool init_macos_bundle_resources(char const *macosdir = nullptr);

} // namespace Inkscape::IO

#endif // __APPLE__

#endif // INKSCAPE_IO_MACOS_BUNDLE_BOOTSTRAP_H
