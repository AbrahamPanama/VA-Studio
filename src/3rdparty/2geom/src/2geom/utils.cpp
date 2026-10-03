// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** Various utility functions.
 *
 * Copyright 2008 Marco Cecchetti <mrcekets at gmail.com>
 * Copyright 2007 Johan Engelen <goejendaagh@zonnet.nl>
 * Copyright 2006 Michael G. Sloan <mgsloan@gmail.com>
 */


#include <2geom/utils.h>


namespace Geom 
{

// return a vector that contains all the binomial coefficients of degree n 
void binomial_coefficients(std::vector<size_t>& bc, std::size_t n)
{
    size_t s = n+1;
    bc.clear();
    bc.resize(s);
    bc[0] = 1;
    for (size_t i = 1; i < n; ++i)
    {
        size_t k = i >> 1;
        if (i & 1u)
        {
            bc[k+1] = bc[k] << 1;
        }
        for (size_t j = k; j > 0; --j)
        {
            bc[j] += bc[j-1];
        }
    }
    s >>= 1;
    for (size_t i = 0; i < s; ++i)
    {
        bc[n-i] = bc[i];
    }
}

} // end namespace Geom











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
