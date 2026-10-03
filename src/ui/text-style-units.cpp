// SPDX-License-Identifier: GPL-2.0-or-later

#include "text-style-units.h"

#include <algorithm>

#include "style.h"
#include "style-internal.h"
#include "svg/css-ostringstream.h"
#include "util/units.h"

namespace Inkscape::UI {
namespace {

char const *absoluteUnit(TextLineHeightUnit unit)
{
    switch (unit) {
        case TextLineHeightUnit::Px: return "px";
        case TextLineHeightUnit::Pt: return "pt";
        case TextLineHeightUnit::Mm: return "mm";
        case TextLineHeightUnit::Cm: return "cm";
        case TextLineHeightUnit::In: return "in";
        default: return "px";
    }
}

bool isRelative(TextLineHeightUnit unit)
{
    return unit == TextLineHeightUnit::Lines || unit == TextLineHeightUnit::Percent;
}

double asPixels(TextLineHeightValue value, double font_size_px)
{
    if (value.unit == TextLineHeightUnit::Lines) return value.value * font_size_px;
    if (value.unit == TextLineHeightUnit::Percent) return value.value * font_size_px / 100.0;
    return Util::Quantity::convert(value.value, absoluteUnit(value.unit), "px");
}

} // namespace

TextLineHeightValue lineHeightFromStyle(SPStyle const &style)
{
    auto const &height = style.line_height;
    if (height.normal) return {1.25, TextLineHeightUnit::Lines};

    switch (height.unit) {
        case SP_CSS_UNIT_NONE: return {height.value, TextLineHeightUnit::Lines};
        case SP_CSS_UNIT_PERCENT: return {height.value * 100.0, TextLineHeightUnit::Percent};
        case SP_CSS_UNIT_PT: return {height.value, TextLineHeightUnit::Pt};
        case SP_CSS_UNIT_MM: return {height.value, TextLineHeightUnit::Mm};
        case SP_CSS_UNIT_CM: return {height.value, TextLineHeightUnit::Cm};
        case SP_CSS_UNIT_IN: return {height.value, TextLineHeightUnit::In};
        case SP_CSS_UNIT_PX: return {height.value, TextLineHeightUnit::Px};
        default:
            return {height.computed, TextLineHeightUnit::Px};
    }
}

TextLineHeightValue convertLineHeight(TextLineHeightValue value,
                                      TextLineHeightUnit destination,
                                      double font_size_px)
{
    font_size_px = std::max(font_size_px, 0.001);
    if (value.unit == destination) return value;
    auto const px = asPixels(value, font_size_px);
    if (destination == TextLineHeightUnit::Lines) return {px / font_size_px, destination};
    if (destination == TextLineHeightUnit::Percent) return {px / font_size_px * 100.0, destination};
    return {Util::Quantity::convert(px, "px", absoluteUnit(destination)), destination};
}

Glib::ustring serializeLineHeight(TextLineHeightValue value)
{
    CSSOStringStream out;
    if (value.unit == TextLineHeightUnit::Lines) {
        out << value.value;
    } else if (value.unit == TextLineHeightUnit::Percent) {
        out << value.value << '%';
    } else {
        out << Util::Quantity::convert(value.value, absoluteUnit(value.unit), "px") << "px";
    }
    return out.str();
}

char const *lineHeightUnitLabel(TextLineHeightUnit unit)
{
    switch (unit) {
        case TextLineHeightUnit::Lines: return "lines";
        case TextLineHeightUnit::Percent: return "%";
        case TextLineHeightUnit::Px: return "px";
        case TextLineHeightUnit::Pt: return "pt";
        case TextLineHeightUnit::Mm: return "mm";
        case TextLineHeightUnit::Cm: return "cm";
        case TextLineHeightUnit::In: return "in";
    }
    return "lines";
}

} // namespace Inkscape::UI
