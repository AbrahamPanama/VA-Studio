// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_LIBNRTYPE_FONT_FEATURE_UTILS_H
#define INKSCAPE_LIBNRTYPE_FONT_FEATURE_UTILS_H

#include <optional>
#include <string_view>

#include <glibmm/ustring.h>

namespace Inkscape {

/** Return a canonical feature string after changing one OpenType tag only. */
Glib::ustring merge_font_feature(Glib::ustring const &settings, std::string_view tag,
                                 std::optional<bool> enabled);

/** Read a four-character OpenType tag from a CSS/Pango feature string. */
std::optional<bool> query_font_feature(Glib::ustring const &settings, std::string_view tag);

} // namespace Inkscape

#endif // INKSCAPE_LIBNRTYPE_FONT_FEATURE_UTILS_H
