// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TEXT_STYLE_UNITS_H
#define INKSCAPE_UI_TEXT_STYLE_UNITS_H

#include <glibmm/ustring.h>

class SPStyle;

namespace Inkscape::UI {

enum class TextLineHeightUnit { Lines, Percent, Px, Pt, Mm, Cm, In };

struct TextLineHeightValue {
    double value = 1.25;
    TextLineHeightUnit unit = TextLineHeightUnit::Lines;

    bool operator==(TextLineHeightValue const &) const = default;
};

TextLineHeightValue lineHeightFromStyle(SPStyle const &style);
TextLineHeightValue convertLineHeight(TextLineHeightValue value,
                                      TextLineHeightUnit destination,
                                      double font_size_px);
Glib::ustring serializeLineHeight(TextLineHeightValue value);
char const *lineHeightUnitLabel(TextLineHeightUnit unit);

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_TEXT_STYLE_UNITS_H
