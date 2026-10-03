// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later
/*
 * Authors:
 *   Vinícius dos Santos Oliveira <vini.ipsmaker@gmail.com>
 *
 * Copyright (C) 2013 Authors
 */

#ifndef LIBDEPIXELIZE_DEPIXELIZE_H
#define LIBDEPIXELIZE_DEPIXELIZE_H

#include "image.h"
#include "splines.h"

namespace Depixelize {

struct Options
{
    // Heuristics
    double curves_multiplier = 1;
    int islands_weight = 5;
    double sparse_pixels_multiplier = 1;
    int sparse_pixels_radius = 4;

    // Other options
    bool optimize = true;
};

/**
 * \p options.optimize will be ignored
 */
Splines to_voronoi(const Image &buf,
                   const Options &options = {});

/**
 * \p options.optimize will be ignored
 */
Splines to_grouped_voronoi(const Image &buf,
                           const Options &options = {});

Splines to_splines(const Image &buf,
                   const Options &options = {});

} // namespace Depixelize

#endif // LIBDEPIXELIZE_DEPIXELIZE_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
