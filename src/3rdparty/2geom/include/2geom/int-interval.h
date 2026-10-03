// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 *  \file
 *  \brief Closed interval of integer values
 *//*
 * Copyright 2011 Krzysztof Kosiński <tweenk.pl@gmail.com>
 */

#ifndef LIB2GEOM_SEEN_INT_INTERVAL_H
#define LIB2GEOM_SEEN_INT_INTERVAL_H

#include <2geom/coord.h>
#include <2geom/generic-interval.h>

namespace Geom {

/**
 * @brief Range of integers that is never empty.
 * @ingroup Primitives
 */
using IntInterval = GenericInterval<IntCoord>;

/**
 * @brief Range of integers that can be empty.
 * @ingroup Primitives
 */
using OptIntInterval = GenericOptInterval<IntCoord>;

} // namespace Geom

#endif // LIB2GEOM_SEEN_INT_INTERVAL_H

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
