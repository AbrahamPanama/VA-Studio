// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file
 * \brief Intersection utilities
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 * 
 * Copyright 2015 Authors
 */

#ifndef SEEN_LIB2GEOM_INTERSECTION_H
#define SEEN_LIB2GEOM_INTERSECTION_H

#include <2geom/coord.h>
#include <2geom/point.h>

namespace Geom {


/** @brief Intersection between two shapes.
 */
template <typename TimeA = Coord, typename TimeB = TimeA>
class Intersection
    : boost::totally_ordered< Intersection<TimeA, TimeB> >
{
public:
    /** @brief Construct from shape references and time values.
     * By default, the intersection point will be halfway between the evaluated
     * points on the two shapes. */
    template <typename TA, typename TB>
    Intersection(TA const &sa, TB const &sb, TimeA const &ta, TimeB const &tb)
        : first(ta)
        , second(tb)
        , _point(lerp(0.5, sa.pointAt(ta), sb.pointAt(tb)))
    {}

    /// Additionally report the intersection point.
    Intersection(TimeA const &ta, TimeB const &tb, Point const &p)
        : first(ta)
        , second(tb)
        , _point(p)
    {}

    /// Intersection point, as calculated by the intersection algorithm.
    Point point() const {
        return _point;
    }
    /// Implicit conversion to Point.
    operator Point() const {
        return _point;
    }

    friend inline void swap(Intersection &a, Intersection &b) {
        using std::swap;
        swap(a.first, b.first);
        swap(a.second, b.second);
        swap(a._point, b._point);
    }

    bool operator==(Intersection const &other) const {
        if (first != other.first) return false;
        if (second != other.second) return false;
        return true;
    }
    bool operator<(Intersection const &other) const {
        if (first < other.first) return true;
        if (first == other.first && second < other.second) return true;
        return false;
    }

public:
    /// First shape and time value.
    TimeA first;
    /// Second shape and time value.
    TimeB second;
private:
    // Recalculation of the intersection point from the time values is in many cases
    // less precise than the value obtained directly from the intersection algorithm,
    // so we need to store it.
    Point _point;
};


// TODO: move into new header?
template <typename T>
struct ShapeTraits {
    typedef Coord TimeType;
    typedef Interval IntervalType;
    typedef T AffineClosureType;
    typedef Intersection<> IntersectionType;
};

template <typename A, typename B> inline
std::vector< Intersection<A, B> > transpose(std::vector< Intersection<B, A> > const &in) {
    std::vector< Intersection<A, B> > result;
    for (std::size_t i = 0; i < in.size(); ++i) {
        result.push_back(Intersection<A, B>(in[i].second, in[i].first, in[i].point()));
    }
    return result;
}

template <typename T> inline
void transpose_in_place(std::vector< Intersection<T, T> > &xs) {
    for (std::size_t i = 0; i < xs.size(); ++i) {
        std::swap(xs[i].first, xs[i].second);
    }
}

typedef Intersection<> ShapeIntersection;


} // namespace Geom

#endif // SEEN_LIB2GEOM_INTERSECTION_H
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
