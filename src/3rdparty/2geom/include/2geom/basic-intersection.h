// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file
 * @brief Basic intersection routines
 *//*
 * Authors:
 *   Nathan Hurst <njh@njhurst.com>
 *   Marco Cecchetti <mrcekets at gmail.com>
 *   Jean-François Barraud <jf.barraud@gmail.com>
 * 
 * Copyright 2008-2009 Authors
 */

#ifndef LIB2GEOM_SEEN_BASIC_INTERSECTION_H
#define LIB2GEOM_SEEN_BASIC_INTERSECTION_H

#include <2geom/point.h>
#include <2geom/bezier.h>
#include <2geom/sbasis.h>
#include <2geom/d2.h>

#include <vector>
#include <utility>

#define USE_RECURSIVE_INTERSECTOR 0


namespace Geom {

void find_intersections(std::vector<std::pair<double, double> > &xs,
                        D2<Bezier> const &A,
                        D2<Bezier> const &B,
                        double precision = EPSILON);

void find_intersections(std::vector<std::pair<double, double> > &xs,
                        D2<SBasis> const &A,
                        D2<SBasis> const &B,
                        double precision = EPSILON);

void find_intersections(std::vector< std::pair<double, double> > &xs,
                        std::vector<Point> const &A,
                        std::vector<Point> const &B,
                        double precision = EPSILON);

void find_self_intersections(std::vector<std::pair<double, double> > &xs,
                             D2<SBasis> const &A,
                             double precision = EPSILON);

void find_self_intersections(std::vector<std::pair<double, double> > &xs,
                             D2<Bezier> const &A,
                             double precision = EPSILON);

/*
 * find_intersection
 *
 *  input: A, B       - set of control points of two Bezier curve
 *  input: precision  - required precision of computation
 *  output: xs        - set of pairs of parameter values
 *                      at which crossing happens
 *
 *  If A and B are identical, or identical up to reversal,
 *  then no intersections are returned.
 *
 *  This routine is based on the Bezier Clipping Algorithm,
 *  see: Sederberg, Nishita, 1990 - Curve intersection using Bezier clipping
 */
void find_intersections_bezier_clipping (std::vector< std::pair<double, double> > & xs,
                         std::vector<Point> const& A,
                         std::vector<Point> const& B,
                         double precision = EPSILON);
//#endif

void subdivide(D2<Bezier> const &a,
               D2<Bezier> const &b,
               std::vector< std::pair<double, double> > const &xs,
               std::vector< D2<Bezier> > &av,
               std::vector< D2<Bezier> > &bv);

/*
 * find_collinear_normal
 *
 *  input: A, B       - set of control points of two Bezier curve
 *  input: precision  - required precision of computation
 *  output: xs        - set of pairs of parameter values
 *                      at which there are collinear normals
 *
 *  This routine is based on the Bezier Clipping Algorithm,
 *  see: Sederberg, Nishita, 1990 - Curve intersection using Bezier clipping
 */
void find_collinear_normal (std::vector< std::pair<double, double> >& xs,
                            std::vector<Point> const& A,
                            std::vector<Point> const& B,
                            double precision = EPSILON);

void polish_intersections(std::vector<std::pair<double, double> > &xs, 
                          D2<SBasis> const &A,
                          D2<SBasis> const &B);


/**
 * Compute the Hausdorf distance from A to B only.
 */
double hausdorfl(D2<SBasis>& A, D2<SBasis> const &B,
                 double m_precision,
                 double *a_t=NULL, double *b_t=NULL);

/** 
 * Compute the symmetric Hausdorf distance.
 */
double hausdorf(D2<SBasis> &A, D2<SBasis> const &B,
                double m_precision,
                double *a_t=NULL, double *b_t=NULL);

/**
 * Check if two line segments intersect. If they are collinear, the result is undefined.
 * @return True if line segments AB and CD intersect
 */
bool non_collinear_segments_intersect(const Point &A, const Point &B, const Point &C, const Point &D);
}

#endif // !LIB2GEOM_SEEN_BASIC_INTERSECTION_H

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
