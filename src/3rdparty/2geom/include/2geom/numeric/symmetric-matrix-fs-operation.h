// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * SymmetricMatrix basic operation
 *
 * Authors:
 *      Marco Cecchetti <mrcekets at gmail.com>
 *
 * Copyright 2009  authors
 */

#ifndef _NL_SYMMETRIC_MATRIX_FS_OPERATION_H_
#define _NL_SYMMETRIC_MATRIX_FS_OPERATION_H_


#include <2geom/numeric/symmetric-matrix-fs.h>
#include <2geom/numeric/symmetric-matrix-fs-trace.h>




namespace Geom { namespace NL {

template <size_t N>
SymmetricMatrix<N> adj(const ConstBaseSymmetricMatrix<N> & S);

template <>
inline
SymmetricMatrix<2> adj(const ConstBaseSymmetricMatrix<2> & S)
{
    SymmetricMatrix<2> result;
    result.get<0,0>() = S.get<1,1>();
    result.get<1,0>() = -S.get<1,0>();
    result.get<1,1>() = S.get<0,0>();
    return result;
}

template <>
inline
SymmetricMatrix<3> adj(const ConstBaseSymmetricMatrix<3> & S)
{
    SymmetricMatrix<3> result;

    result.get<0,0>() = S.get<1,1>() * S.get<2,2>() - S.get<1,2>() * S.get<2,1>();
    result.get<1,0>() = S.get<0,2>() * S.get<2,1>() - S.get<0,1>() * S.get<2,2>();
    result.get<1,1>() = S.get<0,0>() * S.get<2,2>() - S.get<0,2>() * S.get<2,0>();
    result.get<2,0>() = S.get<0,1>() * S.get<1,2>() - S.get<0,2>() * S.get<1,1>();
    result.get<2,1>() = S.get<0,2>() * S.get<1,0>() - S.get<0,0>() * S.get<1,2>();
    result.get<2,2>() = S.get<0,0>() * S.get<1,1>() - S.get<0,1>() * S.get<1,0>();
    return result;
}

template <size_t N>
inline
SymmetricMatrix<N> inverse(const ConstBaseSymmetricMatrix<N> & S)
{
    SymmetricMatrix<N> result = adj(S);
    double d = det(S);
    assert (d != 0);
    result.scale (1/d);
    return result;
}

} /* end namespace NL*/ } /* end namespace Geom*/


#endif // _NL_SYMMETRIC_MATRIX_FS_OPERATION_H_




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
