// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * TODO: insert short description here
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef SEEN_SP_ITEM_TRANSFORM_H
#define SEEN_SP_ITEM_TRANSFORM_H

#include <optional>

#include <2geom/forward.h>

class SPItem;

namespace Inkscape {
class ObjectSet;
} // namespace Inkscape

Geom::Affine get_scale_transform_for_uniform_stroke (Geom::Rect const &bbox_visual, double stroke_x, double stroke_y, bool transform_stroke, bool preserve, double x0, double y0, double x1, double y1);
Geom::Affine get_scale_transform_for_variable_stroke (Geom::Rect const &bbox_visual, Geom::Rect const &bbox_geom, bool transform_stroke, bool preserve, double x0, double y0, double x1, double y1);
Geom::Rect get_visual_bbox (Geom::OptRect const &initial_geom_bbox, Geom::Affine const &abs_affine, double const initial_strokewidth, bool const transform_stroke);

/// The affine the select toolbar applies to resize @a set to the bounds [x0,x1] x [y0,y1].
/// visual_bbox: scale by the visual bounds with stroke handling (get_scale_transform_for_variable_stroke);
/// otherwise the geometric path (get_scale_transform_for_uniform_stroke with zero stroke, as the toolbar does).
/// nullopt when the set has no visual or geometric bounds.
std::optional<Geom::Affine> selection_resize_affine(Inkscape::ObjectSet &set, bool visual_bbox, bool transform_stroke,
                                                    bool preserve, double x0, double y0, double x1, double y1);

#endif // SEEN_SP_ITEM_TRANSFORM_H

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
