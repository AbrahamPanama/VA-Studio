// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: feature gate and registration (EB6-register).
 * Plan: internal note EXPLODE_BITMAP_IMPLEMENTATION_PLAN, internal note EXPLODE_BITMAP_PLAN v3.2.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_EXPLODE_BITMAP_FEATURE_H
#define INKSCAPE_UI_EXPLODE_BITMAP_FEATURE_H

#include "preferences.h"

namespace Inkscape::Bitmap {

inline constexpr char explodeBitmapPreference[] = "/options/explodebitmap/enabled";

// Read at every entry point: a stale menu/search result must never bypass opt-in.
inline bool explodeBitmapEnabled()
{
    return Preferences::get()->getBool(explodeBitmapPreference, false);
}

} // namespace Inkscape::Bitmap

#endif // INKSCAPE_UI_EXPLODE_BITMAP_FEATURE_H
