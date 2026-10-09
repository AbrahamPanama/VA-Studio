// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * feTile filter primitive renderer
 *
 * Authors:
 *   Felipe Corrêa da Silva Sanches <juca@members.fsf.org>
 *
 * Copyright (C) 2007 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <algorithm>
#include <cmath>
#include <glib.h>

#include "display/cairo-utils.h"
#include "display/nr-filter-tile.h"
#include "display/nr-filter-slot.h"
#include "display/nr-filter-units.h"

namespace Inkscape {
namespace Filters {

FilterTile::FilterTile() = default;

FilterTile::~FilterTile() = default;

void FilterTile::render_cairo(FilterSlot &slot) const
{
    // This input source contains only the "rendering" tile.
    cairo_surface_t *in = slot.getcairo(_input);

    // For debugging
    // static int i = 0;
    // ++i;
    // std::stringstream filename;
    // filename << "dump." << i << ".png";
    // cairo_surface_write_to_png( in, filename.str().c_str() );

    // This is the feTile source area as determined by the input primitive area (see SVG spec).
    Geom::Rect tile_area = slot.get_primitive_area(_input);

    if (tile_area.width() == 0.0 || tile_area.height() == 0.0) {
        auto *out = ink_cairo_surface_create_identical(in);
        copy_cairo_surface_ci(in, out);
        slot.set(_output, out);
        cairo_surface_destroy(out);
        return;
    } else {

        cairo_surface_t *out = ink_cairo_surface_create_identical(in);
        // color_interpolation_filters for out same as in.
        copy_cairo_surface_ci(in, out);
        cairo_t *ct = cairo_create(out);

        // The rectangle of the "rendering" tile.
        Geom::Rect sa = slot.get_slot_area();

        Geom::Affine trans = slot.get_units().get_matrix_user2pb();

        // Create feTile tile ----------------

        // Get tile area in pixbuf units (tile transformed).
        Geom::Rect tt = tile_area * trans;
        
        // Shift between "rendering" tile and feTile tile
        Geom::Point shift = sa.min() - tt.min(); 

        // A non-finite tile cannot be tiled; do not hand NaN/inf to cairo or to the loops below.
        if (!std::isfinite(tt.width()) || !std::isfinite(tt.height()) || !std::isfinite(shift[Geom::X]) ||
            !std::isfinite(shift[Geom::Y])) {
            g_warning("FilterTile::render_cairo: tile geometry is not finite; tiling skipped");
            slot.set(_output, out);
            cairo_destroy(ct);
            cairo_surface_destroy(out);
            return;
        }

        // With an upright, positively scaled transform every copy lands at a non-negative offset,
        // so only the part of the tile inside the slot can ever be visible: clip the tile to the
        // slot instead of allocating a tile surface larger than the output.
        bool const upright = trans[0] > 0 && trans[3] > 0 && trans[1] == 0 && trans[2] == 0;
        double tile_w = tt.width();
        double tile_h = tt.height();
        if (upright) {
            tile_w = std::min(tile_w, sa.width());
            tile_h = std::min(tile_h, sa.height());
        }

        // Cairo surfaces have integer device-pixel dimensions. A sub-pixel tile therefore has
        // no drawable area, and no copy of it can contribute to the output slot.
        int const tile_pixel_w = static_cast<int>(tile_w);
        int const tile_pixel_h = static_cast<int>(tile_h);
        if (tile_pixel_w <= 0 || tile_pixel_h <= 0) {
            slot.set(_output, out);
            cairo_destroy(ct);
            cairo_surface_destroy(out);
            return;
        }

        // Determine the tile copies whose device-pixel bounds can intersect the output slot.
        // The primitive-area limits remain in place, while the slot bounds keep very large
        // primitive regions from producing copies that Cairo will only clip away.
        Geom::Rect pr = filter_primitive_area(slot.get_units());
        constexpr double max_count = 1 << 24;
        int tile_cols = static_cast<int>(std::clamp(std::ceil(pr.width() / tile_area.width()), 0.0, max_count));
        int tile_rows = static_cast<int>(std::clamp(std::ceil(pr.height() / tile_area.height()), 0.0, max_count));
        int col0 = 0;
        int row0 = 0;

        Geom::Affine const linear(trans[0], trans[1], trans[2], trans[3], 0, 0);
        if (linear.isSingular() || tile_cols == 0 || tile_rows == 0) {
            slot.set(_output, out);
            cairo_destroy(ct);
            cairo_surface_destroy(out);
            return;
        }

        // A copy at (col, row) is placed at L * (col * w, row * h) - translation.
        // Expand the slot by one tile in device pixels, map it back to tile coordinates, and
        // retain only the copies that can reach the slot. The final slot-sized bound also caps
        // the number of copies independently of the filter primitive's potentially huge area.
        Geom::Rect const reach(Geom::Point(-tile_pixel_w - trans[4], -tile_pixel_h - trans[5]),
                               Geom::Point(sa.width() - trans[4], sa.height() - trans[5]));
        Geom::Rect const back = reach * linear.inverse();
        int const c0 = static_cast<int>(std::clamp(std::floor(back.left() / tile_area.width()), 0.0, max_count));
        int const c1 = static_cast<int>(std::clamp(std::ceil(back.right() / tile_area.width()), 0.0, max_count));
        int const r0 = static_cast<int>(std::clamp(std::floor(back.top() / tile_area.height()), 0.0, max_count));
        int const r1 = static_cast<int>(std::clamp(std::ceil(back.bottom() / tile_area.height()), 0.0, max_count));
        col0 = std::min(std::max(c0 - 1, 0), tile_cols);
        row0 = std::min(std::max(r0 - 1, 0), tile_rows);
        tile_cols = std::min(tile_cols, c1 + 1);
        tile_rows = std::min(tile_rows, r1 + 1);

        if (upright) {
            int const slot_cols = static_cast<int>(std::clamp(std::ceil(sa.width() / tile_pixel_w) + 1.0, 0.0, max_count));
            int const slot_rows = static_cast<int>(std::clamp(std::ceil(sa.height() / tile_pixel_h) + 1.0, 0.0, max_count));
            tile_cols = std::min(tile_cols, col0 + slot_cols);
            tile_rows = std::min(tile_rows, row0 + slot_rows);
        }

        if (tile_cols <= col0 || tile_rows <= row0) {
            slot.set(_output, out);
            cairo_destroy(ct);
            cairo_surface_destroy(out);
            return;
        }

        // Create feTile tile surface
        cairo_surface_t *tile = cairo_surface_create_similar(in, cairo_surface_get_content(in), tile_w, tile_h);
        cairo_t *ct_tile = cairo_create(tile);
        cairo_set_source_surface(ct_tile, in, shift[Geom::X], shift[Geom::Y]);
        cairo_paint(ct_tile);

        // Paint tiles ------------------
        
        // For debugging
        // std::stringstream filename;
        // filename << "tile." << i << ".png";
        // cairo_surface_write_to_png( tile, filename.str().c_str() );
        
        // Hard bound on painted copies for sub-pixel tiles over a large slot.
        constexpr long max_copies = 1L << 22;
        if (static_cast<long>(tile_cols - col0) * static_cast<long>(tile_rows - row0) > max_copies) {
            g_warning("FilterTile::render_cairo: too many tiles (%d x %d); drawing a bounded number",
                      tile_cols - col0, tile_rows - row0);
            tile_cols = col0 + static_cast<int>(std::min<long>(tile_cols - col0, max_copies));
            tile_rows = row0 + static_cast<int>(std::max<long>(1, max_copies / (tile_cols - col0)));
        }

        for (int col = col0; col < tile_cols; ++col) {
            for (int row = row0; row < tile_rows; ++row) {
                Geom::Point offset(col * tile_area.width(), row * tile_area.height());
                offset *= trans;
                offset[Geom::X] -= trans[4];
                offset[Geom::Y] -= trans[5];
        
                cairo_set_source_surface(ct, tile, offset[Geom::X], offset[Geom::Y]);
                cairo_paint(ct);
            }
        }
        slot.set(_output, out);

        // Clean up
        cairo_destroy(ct);
        cairo_surface_destroy(out);
        cairo_destroy(ct_tile);
        cairo_surface_destroy(tile);
    }
}

void FilterTile::area_enlarge(Geom::IntRect &area, Geom::Affine const &trans) const
{
    // Set to very large rectangle so we get tile source. It will be clipped later.

    // Note, setting to infinite using Geom::IntRect::infinite() causes overflow/underflow problems.
    Geom::IntCoord max = std::numeric_limits<Geom::IntCoord>::max() / 4;
    area = Geom::IntRect(-max, -max, max, max);
}

double FilterTile::complexity(Geom::Affine const &) const
{
    return 1.0;
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
