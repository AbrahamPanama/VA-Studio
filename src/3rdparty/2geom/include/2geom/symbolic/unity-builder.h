// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Routines to make up "zero" and "one" elements of a ring
 *
 * Authors:
 *      Marco Cecchetti <mrcekets at gmail.com>
 *
 * Copyright 2008  authors
 */

#ifndef _GEOM_SL_UNITY_BUILDER_H_
#define _GEOM_SL_UNITY_BUILDER_H_


#include <type_traits>



namespace Geom { namespace SL {


/*
 *  zero builder function class type
 *
 *  made up a zero element, in the algebraic ring theory meaning,
 *  for the type T
 */

template< typename T, bool numeric = std::is_arithmetic<T>::value >
struct zero
{};

// specialization for basic numeric type
template< typename T >
struct zero<T, true>
{
    T operator() () const
    {
        return 0;
    }
};


/*
 *  one builder function class type
 *
 *  made up a one element, in the algebraic ring theory meaning,
 *  for the type T
 */

template< typename T, bool numeric = std::is_arithmetic<T>::value >
struct one
{};

// specialization for basic numeric type
template< typename T >
struct one<T, true>
{
    T operator() ()
    {
        return 1;
    }
};

} /*end namespace Geom*/  } /*end namespace SL*/


#endif // _GEOM_SL_UNITY_BUILDER_H_


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
