// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Diffeomorphism-based intersector: given two curves
 *  M(t)=(x(t),y(t)) and N(u)=(X(u),Y(u))
 * and supposing M is a graph over the x-axis, we compute y(x) and the
 * transformation:
 *  X <- X
 *  Y <- Y - y(X)
 * smashes M on the x axis. The intersections are then given by the roots of:
 *  Y(u) - y(X(u)) = 0
 *//*
 * Authors:
 * 		J.-F. Barraud    <jfbarraud at gmail.com>
 * Copyright 2010  authors
 */

#ifndef SEEN_LIB2GEOM_INTERSECTION_BY_SMASHING_H
#define SEEN_LIB2GEOM_INTERSECTION_BY_SMASHING_H

#include <2geom/d2.h>
#include <2geom/interval.h>
#include <2geom/sbasis.h>
#include <2geom/sbasis-geometric.h>
#include <cstdlib>
#include <vector>
#include <algorithm>


namespace Geom{

struct SmashIntersection {
	Rect times;
	Rect bbox;
};

std::vector<SmashIntersection> smash_intersect( D2<SBasis> const &a, D2<SBasis> const &b, double tol);
std::vector<SmashIntersection> monotonic_smash_intersect( D2<SBasis> const &a, D2<SBasis> const &b, double tol);
//std::vector<Intersection> monotonic_smash_intersect( Curve const &a, double a_from, double a_to,
//			                                         Curve const &b, double b_from, double b_to, double tol);

std::vector<Interval> monotonicSplit(D2<SBasis> const &p);

} // end namespace Geom

#endif // !SEEN_LIB2GEOM_INTERSECTION_BY_SMASHING_H

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
