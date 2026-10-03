// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file
 * \brief Sweepline intersection of groups of rectangles
 *//*
 * Authors:
 *      ? <?@?.?>
 * 
 * Copyright ?-?  authors
 */

#ifndef LIB2GEOM_SEEN_SWEEP_H
#define LIB2GEOM_SEEN_SWEEP_H

#include <vector>
#include <2geom/d2.h>

namespace Geom {

std::vector<std::vector<unsigned> >
sweep_bounds(std::vector<Rect>, Dim2 dim = X);

std::vector<std::vector<unsigned> >
sweep_bounds(std::vector<Rect>, std::vector<Rect>, Dim2 dim = X);

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
