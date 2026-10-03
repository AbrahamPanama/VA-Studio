// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file
 * \brief  Contains forward declarations of 2geom types
 *//*
 * Authors:
 *  Johan Engelen <goejendaagh@zonnet.nl>
 *  Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2008-2010 Authors
 */

#ifndef LIB2GEOM_SEEN_FORWARD_H
#define LIB2GEOM_SEEN_FORWARD_H

namespace Geom {

// primitives
typedef double Coord;
typedef int IntCoord;
class Point;
class IntPoint;
class Line;
class Ray;
template <typename> class GenericInterval;
template <typename> class GenericOptInterval;
class Interval;
class OptInterval;
typedef GenericInterval<IntCoord> IntInterval;
typedef GenericOptInterval<IntCoord> OptIntInterval;
template <typename> class GenericRect;
template <typename> class GenericOptRect;
class Rect;
class OptRect;
typedef GenericRect<IntCoord> IntRect;
typedef GenericOptRect<IntCoord> OptIntRect;

// fragments
class Linear;
class Bezier;
class SBasis;
class Poly;

// shapes
class Circle;
class Ellipse;
class ConvexHull;

// curves
class Curve;
class SBasisCurve;
class BezierCurve;
template <unsigned degree> class BezierCurveN;
typedef BezierCurveN<1> LineSegment;
typedef BezierCurveN<2> QuadraticBezier;
typedef BezierCurveN<3> CubicBezier;
class EllipticalArc;

// paths and path sequences
class Path;
class PathVector;
struct PathTime;
class PathInterval;
struct PathVectorTime;

// errors
class Exception;
class LogicalError;
class RangeError;
class NotImplemented;
class InvariantsViolation;
class NotInvertible;
class ContinuityError;

// transforms
class Affine;
class Translate;
class Rotate;
class Scale;
class HShear;
class VShear;
class Zoom;

// templates
template <typename> class D2;
template <typename> class Piecewise;

// misc
class SVGPathSink;
template <typename> class SVGPathGenerator;

}

#endif // SEEN_GEOM_FORWARD_H

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
