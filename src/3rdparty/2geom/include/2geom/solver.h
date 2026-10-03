// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file
 * \brief Finding roots of Bernstein-Bezier polynomials
 *//*
 * Authors:
 *      ? <?@?.?>
 *
 * Copyright ?-?  authors
 */

#ifndef LIB2GEOM_SEEN_SOLVER_H
#define LIB2GEOM_SEEN_SOLVER_H

#include <2geom/point.h>
#include <2geom/sbasis.h>
#include <vector>

namespace Geom {

	class Point;
	class Bezier;

unsigned
crossing_count(Geom::Point const *V,	/*  Control pts of Bezier curve	*/
	       unsigned degree);	/*  Degree of Bezier curve */
void
find_parametric_bezier_roots(
    Geom::Point const *w, /* The control points  */
    unsigned degree,	/* The degree of the polynomial */
    std::vector<double> & solutions,	/* RETURN candidate t-values */
    unsigned depth);	/* The depth of the recursion */

unsigned
crossing_count(double const *V,	/*  Control pts of Bezier curve	*/
	       unsigned degree,	/*  Degree of Bezier curve */
	       double left_t, double right_t);


void
find_bernstein_roots(
    double const *w, /* The control points  */
    unsigned degree,	/* The degree of the polynomial */
    std::vector<double> & solutions,	/* RETURN candidate t-values */
    unsigned depth,	/* The depth of the recursion */
    double left_t=0, double right_t=1, bool use_secant=true);

};

void
find_bernstein_roots(std::vector<double> &solutions, /* RETURN candidate t-values */
                     Geom::Bezier const& bz,
                     double left_t, double right_t);

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
