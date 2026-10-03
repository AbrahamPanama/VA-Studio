// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EXP-1: which formats the Export dialog offers, and in what order (per user).
 *
 * Every non-deactivated output extension is a format. Formats are grouped
 * (Raster, Vector, Text, Animation, Other) and keep the user's order inside
 * their group. A new install shows a short list (PNG, TIFF, JPEG, PDF,
 * VA Studio SVG, plain SVG); every other format, including formats added
 * later by an extension or update, stays reachable under "More formats...".
 * Save As and Save a Copy are not affected.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_EXPORT_FORMATS_H
#define INKSCAPE_UI_EXPORT_FORMATS_H

#include <glibmm/ustring.h>
#include <string>
#include <vector>

namespace Inkscape::Extension {
class Output;
}

namespace Inkscape::UI::ExportFormats {

enum class Group { Raster, Vector, Text, Animation, Other };

struct Format {
    std::string id;
    Glib::ustring name;
    Glib::ustring extension;
    Group group = Group::Other;
    bool shown = false;
};

/// Preference node; its "order" and "shown" entries are comma-separated extension ids.
inline constexpr char const *prefs_path = "/dialogs/export/formats";
inline constexpr char const *order_pref = "/dialogs/export/formats/order";
inline constexpr char const *shown_pref = "/dialogs/export/formats/shown";

/// The short list a new install shows.
std::vector<std::string> const &default_shown();

Group group_of(Inkscape::Extension::Output const &output);
Glib::ustring group_label(Group group);

/// Every available format: grouped, in the user's order inside each group,
/// with the user's shown flags (the short list when nothing was configured).
std::vector<Format> formats();

/// Store \a formats' order and shown flags as the user's configuration.
void save(std::vector<Format> const &formats);

/// Forget the user's configuration (back to the short list).
void reset();

/// Store the short list explicitly (default order and shown formats), so
/// open dialogs are notified even on a profile that never changed the lists,
/// and a default format unavailable right now keeps its place.
void restore_short_list();

} // namespace Inkscape::UI::ExportFormats

#endif // INKSCAPE_UI_EXPORT_FORMATS_H
