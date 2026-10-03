// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_DISPLAY_NR_FILTER_CLIPPING_WARNING_H
#define INKSCAPE_DISPLAY_NR_FILTER_CLIPPING_WARNING_H

#include <cstdint>

#include "display/nr-filter-primitive.h"

namespace Inkscape::Filters {

uint32_t apply_clipping_warning_to_pixel(uint32_t premultiplied_argb);

/** View-only diagnostic overlay. It is never represented by SVG/XML. */
class FilterClippingWarning final : public FilterPrimitive
{
public:
    void render_cairo(FilterSlot &slot) const override;
    bool can_handle_affine(Geom::Affine const &) const override { return true; }
    double complexity(Geom::Affine const &) const override { return 1.05; }
    Glib::ustring name() const override { return "Bitmap clipping warning"; }
};

} // namespace Inkscape::Filters

#endif // INKSCAPE_DISPLAY_NR_FILTER_CLIPPING_WARNING_H
