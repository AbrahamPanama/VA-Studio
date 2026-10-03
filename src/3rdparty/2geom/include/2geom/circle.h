// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file
 * @brief Circle shape
 *//*
 * Authors:
 *   Marco Cecchetti <mrcekets at gmail.com>
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright 2008-2014 Authors
 */

#ifndef LIB2GEOM_SEEN_CIRCLE_H
#define LIB2GEOM_SEEN_CIRCLE_H

#include <2geom/forward.h>
#include <2geom/intersection.h>
#include <2geom/point.h>
#include <2geom/rect.h>
#include <2geom/transforms.h>

namespace Geom {

class EllipticalArc;

/** @brief Set of all points at a fixed distance from the center
 * @ingroup Shapes */
class Circle
    : boost::equality_comparable1< Circle
    , MultipliableNoncommutative< Circle, Translate
    , MultipliableNoncommutative< Circle, Rotate
    , MultipliableNoncommutative< Circle, Zoom
      > > > >
{
    Point _center;
    Coord _radius;

public:
    Circle() {}
    Circle(Coord cx, Coord cy, Coord r)
        : _center(cx, cy), _radius(r)
    {}
    Circle(Point const &center, Coord r)
        : _center(center), _radius(r)
    {}

    Circle(Coord A, Coord B, Coord C, Coord D) {
        setCoefficients(A, B, C, D);
    }

    // Construct the unique circle passing through three points.
    //Circle(Point const &a, Point const &b, Point const &c);

    Point center() const { return _center; }
    Coord center(Dim2 d) const { return _center[d]; }
    Coord radius() const { return _radius; }
    Coord area() const { return M_PI * _radius * _radius; }
    bool isDegenerate() const { return _radius == 0; }

    void setCenter(Point const &p) { _center = p; }
    void setRadius(Coord c) { _radius = c; }

    Rect boundsFast() const;
    Rect boundsExact() const { return boundsFast(); }

    Point initialPoint() const;
    Point finalPoint() const { return initialPoint(); }
    Point pointAt(Coord t) const;
    Coord valueAt(Coord t, Dim2 d) const;
    Coord timeAt(Point const &p) const;
    Coord nearestTime(Point const &p) const;

    bool contains(Point const &p) const { return distance(p, _center) <= _radius; }
    bool contains(Rect const &other) const;
    bool contains(Circle const &other) const;

    bool intersects(Line const &l) const;
    bool intersects(LineSegment const &l) const;
    bool intersects(Circle const &other) const;

    std::vector<ShapeIntersection> intersect(Line const &other) const;
    std::vector<ShapeIntersection> intersect(LineSegment const &other) const;
    std::vector<ShapeIntersection> intersect(Circle const &other) const;

    // build a circle by its implicit equation:
    // Ax^2 + Ay^2 + Bx + Cy + D = 0
    void setCoefficients(Coord A, Coord B, Coord C, Coord D);
    void coefficients(Coord &A, Coord &B, Coord &C, Coord &D) const;
    std::vector<Coord> coefficients() const;

    Zoom unitCircleTransform() const;
    Zoom inverseUnitCircleTransform() const;

    EllipticalArc *
    arc(Point const& initial, Point const& inner, Point const& final) const;

    D2<SBasis> toSBasis() const;

    Circle &operator*=(Translate const &t) {
        _center *= t;
        return *this;
    }
    Circle &operator*=(Rotate const &) {
        return *this;
    }
    Circle &operator*=(Zoom const &z) {
        _center *= z;
        _radius *= z.scale();
        return *this;
    }

    bool operator==(Circle const &other) const;

    /** @brief Fit the circle to the passed points using the least squares method.
     * @param points Samples at the perimeter of the circle */
    void fit(std::vector<Point> const &points);
};

bool are_near(Circle const &a, Circle const &b, Coord eps=EPSILON);

std::ostream &operator<<(std::ostream &out, Circle const &c);

template <>
struct ShapeTraits<Circle> {
    typedef Coord TimeType;
    typedef Interval IntervalType;
    typedef Ellipse AffineClosureType;
    typedef Intersection<> IntersectionType;
};

} // end namespace Geom

#endif // LIB2GEOM_SEEN_CIRCLE_H

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
