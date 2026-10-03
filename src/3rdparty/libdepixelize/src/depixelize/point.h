// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later
/*
 * Authors:
 *   Vinícius dos Santos Oliveira <vini.ipsmaker@gmail.com>
 *
 * Copyright (C) 2013 Authors
 */

#ifndef LIBDEPIXELIZE_POINT_H
#define LIBDEPIXELIZE_POINT_H

#include <2geom/point.h>

namespace Depixelize {

template <typename T>
struct Point
{
    T x, y;

    bool smooth = false;

    /**
     * By default, all points are visible, but the poor amount of information
     * that B-Splines (libdepixelize-specific) allows us to represent forces us
     * to create additional points. But... these additional points don't need to
     * be visible.
     */
    bool visible = true;

    Point operator+(const Point &rhs) const
    {
        return Point(x + rhs.x, y + rhs.y);
    }

    Point operator/(T foo) const
    {
        return Point(x / foo, y / foo);
    }

    Point invisible() const
    {
        Point p = *this;
        p.visible = false;
        return p;
    }
};

template <typename T>
Point<T> midpoint(const Point<T> &a, const Point<T> &b)
{
    return Point<T>((a.x + b.x) / 2, (a.y + b.y) / 2);
}

template <typename T>
bool operator==(const Point<T> &lhs, const Point<T> &rhs)
{
    return
        /*
         * Will make a better job identifying which points can be eliminated by
         * cells union.
         */
#ifndef LIBDEPIXELIZE_IS_VERY_WELL_TESTED
        lhs.smooth == rhs.smooth &&
#endif // LIBDEPIXELIZE_IS_VERY_WELL_TESTED
        lhs.x == rhs.x && lhs.y == rhs.y;
}

template <typename T>
bool weakly_equal(const Point<T> &a, const Point<T> &b)
{
    return a.x == b.x && a.y == b.y;
}

template <typename T>
Geom::Point to_geom_point(Point<T> p)
{
    return Geom::Point(p.x, p.y);
}

} // namespace Depixelize

#endif // LIBDEPIXELIZE_POINT_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
