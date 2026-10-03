// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later
/*
 * Authors:
 *   Vinícius dos Santos Oliveira <vini.ipsmaker@gmail.com>
 *
 * Copyright (C) 2013 Authors
 */

#ifndef LIBDEPIXELIZE_INTEGRAL_H
#define LIBDEPIXELIZE_INTEGRAL_H

#include <2geom/coord.h>

namespace Depixelize {

/**
 * Compute the integral numerically using Gaussian Quadrature rule with
 * \p samples number of samples.
 */
template<class T, class F>
Geom::Coord integral(F f, T begin, T end, unsigned samples)
{
    T ret = 0;
    const T width = (end - begin) / samples;

    for ( unsigned i = 0 ; i != samples ; ++i )
        ret += width * f(begin + width * (i + .5));

    return ret;
}

} // namespace Depixelize

#endif // LIBDEPIXELIZE_INTEGRAL_H

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
