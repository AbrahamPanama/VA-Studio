// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file
 * \brief Conversion between Bezier control points and SBasis curves
 *//*
 * Copyright 2006 Nathan Hurst <njh@mail.csse.monash.edu.au>
 */

#ifndef LIB2GEOM_SEEN_BEZIER_TO_SBASIS_H
#define LIB2GEOM_SEEN_BEZIER_TO_SBASIS_H

#include <2geom/coord.h>
#include <2geom/point.h>
#include <2geom/d2.h>
#include <2geom/sbasis-to-bezier.h>

namespace Geom
{

#if 0
inline SBasis bezier_to_sbasis(Coord const *handles, unsigned order) {
    if(order == 0)
        return Linear(handles[0]);
    else if(order == 1)
        return Linear(handles[0], handles[1]);
    else
        return multiply(Linear(1, 0), bezier_to_sbasis(handles, order-1)) +
            multiply(Linear(0, 1), bezier_to_sbasis(handles+1, order-1));
}


template <typename T>
inline D2<SBasis> handles_to_sbasis(T const &handles, unsigned order)
{
    double v[2][order+1];
    for(unsigned i = 0; i <= order; i++)
        for(unsigned j = 0; j < 2; j++)
             v[j][i] = handles[i][j];
    return D2<SBasis>(bezier_to_sbasis(v[0], order),
                      bezier_to_sbasis(v[1], order));
}
#endif


template <typename T>
inline
D2<SBasis> handles_to_sbasis(T const& handles, unsigned order)
{
    D2<SBasis> sbc;
    size_t sz = order + 1;
    std::vector<Point> v;
    v.reserve(sz);
    for (size_t i = 0; i < sz; ++i)
        v.push_back(handles[i]);
    bezier_to_sbasis(sbc, v);
    return sbc;
}

} // end namespace Geom

#endif // LIB2GEOM_SEEN_BEZIER_TO_SBASIS_H
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
