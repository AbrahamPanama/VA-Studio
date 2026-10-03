// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 *  \file
 *  \brief Various geometrical calculations
 *
 *  Authors:
 *   Nathan Hurst <njh@mail.csse.monash.edu.au>
 *
 * Copyright (C) 1999-2002 authors
 */
#ifndef LIB2GEOM_SEEN_GEOM_H
#define LIB2GEOM_SEEN_GEOM_H

//TODO: move somewhere else

#include <vector>
#include <2geom/forward.h>
#include <optional>
#include <2geom/bezier-curve.h>
#include <2geom/line.h>

namespace Geom {

std::optional<Geom::LineSegment>
rect_line_intersect(Geom::Rect &r,
                    Geom::LineSegment ls);

int centroid(std::vector<Geom::Point> const &p, Geom::Point& centroid, double &area);

}

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
