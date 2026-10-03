// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file
 * @brief Conversion between SBasis and Poly.  Not recommended for general use due to instability.
 *//*
 * Authors:
 *      ? <?@?.?>
 * 
 * Copyright ?-?  authors
 */

#ifndef LIB2GEOM_SEEN_SBASIS_POLY_H
#define LIB2GEOM_SEEN_SBASIS_POLY_H

#include <2geom/polynomial.h>
#include <2geom/sbasis.h>

namespace Geom{

SBasis poly_to_sbasis(Poly const & p);
Poly sbasis_to_poly(SBasis const & s);

};

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
