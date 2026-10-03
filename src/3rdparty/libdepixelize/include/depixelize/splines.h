// SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later
/*
 * Authors:
 *   Vinícius dos Santos Oliveira <vini.ipsmaker@gmail.com>
 *
 * Copyright (C) 2013 Authors
 */

#ifndef LIBDEPIXELIZE_SPLINES_H
#define LIBDEPIXELIZE_SPLINES_H

#include <cstdint>
#include <2geom/pathvector.h>

namespace Depixelize {

template<typename T, bool adjust_splines = true>
class SimplifiedVoronoi;

template<typename T>
class HomogeneousSplines;

class Splines
{
public:
    struct Path
    {
        Geom::PathVector pathVector;
        uint8_t rgba[4];
    };

    using iterator = std::vector<Path>::iterator;
    using const_iterator = std::vector<Path>::const_iterator;

    Splines() = default;

    template<typename T, bool adjust_splines>
    Splines(const SimplifiedVoronoi<T, adjust_splines> &simplifiedVoronoi);

    /**
     * There are two levels of optimization. The first level only removes
     * redundant points of colinear points. The second level uses the
     * Kopf-Lischinski optimization. The first level is always enabled.
     * The second level is enabled using \p optimize.
     */
    template<typename T>
    Splines(const HomogeneousSplines<T> &homogeneousSplines, bool optimize);

    // Iterators
    iterator begin()
    {
        return _paths.begin();
    }

    const_iterator begin() const
    {
        return _paths.begin();
    }

    iterator end()
    {
        return _paths.end();
    }

    const_iterator end() const
    {
        return _paths.end();
    }

    int width() const
    {
        return _width;
    }

    int height() const
    {
        return _height;
    }

private:
    std::vector<Path> _paths;
    int _width;
    int _height;
};

} // namespace Depixelize

#endif // LIBDEPIXELIZE_SPLINES_H

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
