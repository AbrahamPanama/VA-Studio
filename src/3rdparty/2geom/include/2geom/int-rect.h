// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 *  \file
 *  \brief Axis-aligned rectangle with integer coordinates
 *//*
 * Copyright 2011 Krzysztof Kosiński <tweenk.pl@gmail.com>
 */

#ifndef LIB2GEOM_SEEN_INT_RECT_H
#define LIB2GEOM_SEEN_INT_RECT_H

#include <2geom/coord.h>
#include <2geom/int-interval.h>
#include <2geom/generic-rect.h>

namespace Geom {

typedef GenericRect<IntCoord> IntRect;
typedef GenericOptRect<IntCoord> OptIntRect;

// the functions below do not work when defined generically
inline OptIntRect operator&(IntRect const &a, IntRect const &b) {
    OptIntRect ret(a);
    ret.intersectWith(b);
    return ret;
}
inline OptIntRect intersect(IntRect const &a, IntRect const &b) {
    return a & b;
}
inline OptIntRect intersect(OptIntRect const &a, OptIntRect const &b) {
    return a & b;
}
inline IntRect unify(IntRect const &a, IntRect const &b) {
    return a | b;
}
inline OptIntRect unify(OptIntRect const &a, OptIntRect const &b) {
    return a | b;
}

} // end namespace Geom

#endif // !LIB2GEOM_SEEN_INT_RECT_H

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
