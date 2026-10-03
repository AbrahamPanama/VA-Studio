// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2006, 2007 Aaron Spike <aaron@ekips.org>
 */

#ifndef SEEN_PY2GEOM_H
#define SEEN_PY2GEOM_H

void wrap_point();
void wrap_etc();
void wrap_interval();
void wrap_transforms();
void wrap_rect();
void wrap_circle();
void wrap_ellipse();
void wrap_sbasis();
void wrap_bezier();
void wrap_linear();
void wrap_pw();
void wrap_d2();
void wrap_path();
void wrap_parser();
void wrap_ray();
// void wrap_shape();
void wrap_line();
void wrap_conic();
void wrap_crossing();
// void wrap_convex_cover();
namespace Geom{
class Point;
class Linear;
};
#include <vector>
typedef std::vector<Geom::Point > PointVec;
typedef std::vector<double > DoubleVec;
typedef std::vector<Geom::Linear> LinearVec;

#endif
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
