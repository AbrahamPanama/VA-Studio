// SPDX-License-Identifier: GPL-2.0-or-later

#include "display/nr-filter-clipping-warning.h"

#include "display/cairo-templates.h"
#include "display/cairo-utils.h"
#include "display/nr-filter-slot.h"

namespace Inkscape::Filters {
namespace {

struct PaintClippingWarning
{
    guint32 operator()(guint32 pixel) const
    {
        return apply_clipping_warning_to_pixel(pixel);
    }
};

} // namespace

uint32_t apply_clipping_warning_to_pixel(uint32_t pixel)
{
    EXTRACT_ARGB32(pixel, alpha, red, green, blue);
    if (!alpha) return pixel;
    red = unpremul_alpha(red, alpha);
    green = unpremul_alpha(green, alpha);
    blue = unpremul_alpha(blue, alpha);
    auto const luminance = (54 * red + 183 * green + 19 * blue + 128) >> 8;
    if (luminance <= 2) {
        red = 0;
        green = 112;
        blue = 255;
    } else if (luminance >= 253) {
        red = 255;
        green = 0;
        blue = 176;
    } else {
        return pixel;
    }
    red = premul_alpha(red, alpha);
    green = premul_alpha(green, alpha);
    blue = premul_alpha(blue, alpha);
    ASSEMBLE_ARGB32(result, alpha, red, green, blue);
    return result;
}

void FilterClippingWarning::render_cairo(FilterSlot &slot) const
{
    auto input = slot.getcairo(_input);
    auto output = ink_cairo_surface_create_same_size(input, CAIRO_CONTENT_COLOR_ALPHA);
    set_cairo_surface_ci(output, SP_CSS_COLOR_INTERPOLATION_SRGB);
    ink_cairo_surface_blit(input, output);
    ink_cairo_surface_filter(output, output, PaintClippingWarning{});
    slot.set(_output, output);
    cairo_surface_destroy(output);
}

} // namespace Inkscape::Filters
