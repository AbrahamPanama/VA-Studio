// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later
/*
 * Authors:
 *   Vinícius dos Santos Oliveira <vini.ipsmaker@gmail.com>
 *
 * Copyright (C) 2013 Authors
 */

#ifndef LIBDEPIXELIZE_SPLINES_IMPL_H
#define LIBDEPIXELIZE_SPLINES_IMPL_H

#include "splines.h"
#include "homogeneoussplines.h"
#include "optimization.h"

namespace Depixelize {

/**
 * Maybe the pass-by-value and then move idiom should be more efficient. But all
 * this is inlinable and we're not even in C++11 yet.
 */
template<class T>
Geom::Path worker_helper(const std::vector< Point<T> > &source1, bool optimize)
{
    using Line = Geom::LineSegment;
    using Quad = Geom::QuadraticBezier;
    using iterator = typename std::vector<Point<T>>::const_iterator;

    std::vector< Point<T> > source;

    if ( optimize )
        source = Depixelize::optimize(source1);
    else
        source = source1;

    iterator it = source.begin();
    Point<T> prev = source.back();
    Geom::Path ret(to_geom_point(midpoint(prev, *it)));

    for ( iterator end = source.end() ; it != end ; ++it ) {
#if LIBDEPIXELIZE_ENABLE_EXPERIMENTAL_FEATURES
        // remove redundant points
        if ( !it->visible ) {
            prev = *it;
            continue;
        }
#endif // LIBDEPIXELIZE_ENABLE_EXPERIMENTAL_FEATURES

        if ( !prev.visible ) {
            Geom::Point middle = to_geom_point(midpoint(prev, *it));
            if ( ret.finalPoint() != middle ) {
                // All generated invisible points are straight lines
                ret.appendNew<Line>(middle);
            }
        }

        Point<T> next = (it + 1 == end) ? source.front() : *(it + 1);
        Point<T> middle = midpoint(*it, next);

        if ( !it->smooth ) {
            ret.appendNew<Line>(to_geom_point(*it));
            ret.appendNew<Line>(to_geom_point(middle));
        } else {
            ret.appendNew<Quad>(to_geom_point(*it), to_geom_point(middle));
        }

        prev = *it;
    }

    return ret;
}

/**
 * It should be used by worker threads. Convert only one object.
 */
template<class T>
void worker(const typename HomogeneousSplines<T>::Polygon &source,
            Splines::Path &dest, bool optimize)
{
    for ( int i = 0 ; i != 4 ; ++i )
        dest.rgba[i] = source.rgba[i];

    dest.pathVector.push_back(worker_helper(source.vertices, optimize));

    for ( typename std::vector< std::vector< Point<T> > >::const_iterator
              it = source.holes.begin(), end = source.holes.end()
              ; it != end ; ++it ) {
        dest.pathVector.push_back(worker_helper(*it, optimize));
    }
}

template<typename T, bool adjust_splines>
Splines::Splines(const SimplifiedVoronoi<T, adjust_splines> &diagram) :
    _width(diagram.width()),
    _height(diagram.height())
{
    _paths.reserve(diagram.size());

    for ( typename SimplifiedVoronoi<T, adjust_splines>::const_iterator
              it = diagram.begin() , end = diagram.end() ; it != end ; ++it ) {
        Path path;

        path.pathVector
            .push_back(Geom::Path(to_geom_point(it->vertices.front())));

        for ( typename std::vector< Point<T> >::const_iterator
                  it2 = ++it->vertices.begin(), end2 = it->vertices.end()
                  ; it2 != end2 ; ++it2 ) {
            path.pathVector.back()
                .appendNew<Geom::LineSegment>(Geom::Point(it2->x, it2->y));
        }

        for ( int i = 0 ; i != 4 ; ++i )
            path.rgba[i] = it->rgba[i];

        _paths.push_back(path);
    }
}

template<class T>
Splines::Splines(const HomogeneousSplines<T> &homogeneousSplines, bool optimize)
    : _paths(homogeneousSplines.size())
    , _width(homogeneousSplines.width())
    , _height(homogeneousSplines.height())
{
    // TODO: It should be threaded
    auto paths_it = begin();
    for (auto const &h : homogeneousSplines) {
        worker<T>(h, *paths_it, optimize);
        ++paths_it;
    }
}

} // namespace Depixelize

#endif // LIBDEPIXELIZE_SPLINES_IMPL_H

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
