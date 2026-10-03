// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * feOffset filter primitive renderer
 *
 * Authors:
 *   Niko Kiirala <niko@kiirala.com>
 *
 * Copyright (C) 2007 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <algorithm>
#include <cmath>

#include "display/cairo-utils.h"
#include "display/nr-filter-offset.h"
#include "display/preview-render-budget.h"
#include "display/nr-filter-slot.h"
#include "display/nr-filter-units.h"

namespace Inkscape {
namespace Filters {

using Geom::X;
using Geom::Y;

FilterOffset::FilterOffset()
    : dx(0)
    , dy(0) {}

FilterOffset::~FilterOffset() = default;

namespace {

/// An offset beyond this many pixels moves everything out of any renderable area anyway.
constexpr double max_offset_px = 1 << 24;

double bound_offset(double v)
{
    if (std::isnan(v)) return 0.0;
    return std::clamp(v, -max_offset_px, max_offset_px);
}

} // namespace

void FilterOffset::render_cairo(FilterSlot &slot) const
{
    cairo_surface_t *in = slot.getcairo(_input);
    cairo_surface_t *out = ink_cairo_surface_create_identical(in);
    // color_interpolation_filters for out same as in. See spec (DisplacementMap).
    copy_cairo_surface_ci(in, out);
    cairo_t *ct = cairo_create(out);

    Geom::Rect vp = filter_primitive_area(slot.get_units());
    slot.set_primitive_area(_output, vp); // Needed for tiling

    Geom::Affine p2pb = slot.get_units().get_matrix_primitiveunits2pb();
    // Bounded so a huge or non-finite offset cannot reach cairo as NaN/inf.
    double x = bound_offset(dx * p2pb.expansionX());
    double y = bound_offset(dy * p2pb.expansionY());

    cairo_set_source_surface(ct, in, x, y);
    cairo_paint(ct);
    cairo_destroy(ct);

    slot.set(_output, out);
    cairo_surface_destroy(out);
}

bool FilterOffset::can_handle_affine(Geom::Affine const &) const
{
    return true;
}

void FilterOffset::set_dx(double amount)
{
    dx = std::isnan(amount) ? 0.0 : std::clamp(amount, -1e9, 1e9);
}

void FilterOffset::set_dy(double amount)
{
    dy = std::isnan(amount) ? 0.0 : std::clamp(amount, -1e9, 1e9);
}

void FilterOffset::area_enlarge(Geom::IntRect &area, Geom::Affine const &trans) const
{
    Geom::Point offset(dx, dy);
    offset *= trans;
    offset[X] = bound_offset(offset[X] - trans[4]);
    offset[Y] = bound_offset(offset[Y] - trans[5]);
    double x0, y0, x1, y1;
    x0 = area.left();
    y0 = area.top();
    x1 = area.right();
    y1 = area.bottom();

    if (offset[X] > 0) {
        x0 -= std::ceil(offset[X]);
    } else {
        x1 -= std::floor(offset[X]);
    }

    if (offset[Y] > 0) {
        y0 -= std::ceil(offset[Y]);
    } else {
        y1 -= std::floor(offset[Y]);
    }

    PreviewRenderBudget::point(x0, y0); PreviewRenderBudget::point(x1, y1);
    area = Geom::IntRect(x0, y0, x1, y1);
}

double FilterOffset::complexity(Geom::Affine const &) const
{
    return 1.02;
}

} // namespace Filters
} // namespace Inkscape

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
