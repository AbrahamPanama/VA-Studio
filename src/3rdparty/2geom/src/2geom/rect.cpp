// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/* Axis-aligned rectangle
 *
 * Authors:
 *   Michael Sloan <mgsloan@gmail.com>
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 * Copyright 2007-2011 Authors
 */

#include <2geom/rect.h>
#include <2geom/transforms.h>

namespace Geom {

Point align_factors(Align g) {
    Point p;
    switch (g) {
    case ALIGN_XMIN_YMIN:
        p[X] = 0.0;
        p[Y] = 0.0;
        break;
    case ALIGN_XMID_YMIN:
        p[X] = 0.5;
        p[Y] = 0.0;
        break;
    case ALIGN_XMAX_YMIN:
        p[X] = 1.0;
        p[Y] = 0.0;
        break;
    case ALIGN_XMIN_YMID:
        p[X] = 0.0;
        p[Y] = 0.5;
        break;
    case ALIGN_XMID_YMID:
        p[X] = 0.5;
        p[Y] = 0.5;
        break;
    case ALIGN_XMAX_YMID:
        p[X] = 1.0;
        p[Y] = 0.5;
        break;
    case ALIGN_XMIN_YMAX:
        p[X] = 0.0;
        p[Y] = 1.0;
        break;
    case ALIGN_XMID_YMAX:
        p[X] = 0.5;
        p[Y] = 1.0;
        break;
    case ALIGN_XMAX_YMAX:
        p[X] = 1.0;
        p[Y] = 1.0;
        break;
    default:
        break;
    }
    return p;
}


/** @brief Transform the rectangle by an affine.
 * The result of the transformation might not be axis-aligned. The return value
 * of this operation will be the smallest axis-aligned rectangle containing
 * all points of the true result. */
Rect &Rect::operator*=(Affine const &m) {
    Point pts[4];
    for (unsigned i=0; i<4; ++i) pts[i] = corner(i) * m;
    Coord minx = std::min(std::min(pts[0][X], pts[1][X]), std::min(pts[2][X], pts[3][X]));
    Coord miny = std::min(std::min(pts[0][Y], pts[1][Y]), std::min(pts[2][Y], pts[3][Y]));
    Coord maxx = std::max(std::max(pts[0][X], pts[1][X]), std::max(pts[2][X], pts[3][X]));
    Coord maxy = std::max(std::max(pts[0][Y], pts[1][Y]), std::max(pts[2][Y], pts[3][Y]));
    f[X].setMin(minx); f[X].setMax(maxx);
    f[Y].setMin(miny); f[Y].setMax(maxy);
    return *this;
}

Affine Rect::transformTo(Rect const &viewport, Aspect const &aspect) const
{
    // 1. translate viewbox to origin
    Geom::Affine total = Translate(-min());

    // 2. compute scale
    Geom::Point vdims = viewport.dimensions();
    Geom::Point dims = dimensions();
    Geom::Scale scale(vdims[X] / dims[X], vdims[Y] / dims[Y]);

    if (aspect.align == ALIGN_NONE) {
        // apply non-uniform scale
        total *= scale * Translate(viewport.min());
    } else {
        double uscale = 0;
        if (aspect.expansion == EXPANSION_MEET) {
            uscale = std::min(scale[X], scale[Y]);
        } else {
            uscale = std::max(scale[X], scale[Y]);
        }
        scale = Scale(uscale);

        // compute offset for align
        Geom::Point offset = vdims - dims * scale;
        offset *= Scale(align_factors(aspect.align));
        total *= scale * Translate(viewport.min() + offset);
    }

    return total;
}

Coord distanceSq(Point const &p, Rect const &rect)
{
    double dx = 0, dy = 0;
    if ( p[X] < rect.left() ) {
        dx = p[X] - rect.left();
    } else if ( p[X] > rect.right() ) {
        dx = rect.right() - p[X];
    }
    if (p[Y] < rect.top() ) {
        dy = rect.top() - p[Y];
    } else if (  p[Y] > rect.bottom() ) {
        dy = p[Y] - rect.bottom();
    }
    return dx*dx+dy*dy;
}

/** @brief Returns the smallest distance between p and rect.
 * @relates Rect */
Coord distance(Point const &p, Rect const &rect)
{
    // copy of distanceSq, because we need to use hypot()
    double dx = 0, dy = 0;
    if ( p[X] < rect.left() ) {
        dx = p[X] - rect.left();
    } else if ( p[X] > rect.right() ) {
        dx = rect.right() - p[X];
    }
    if (p[Y] < rect.top() ) {
        dy = rect.top() - p[Y];
    } else if (  p[Y] > rect.bottom() ) {
        dy = p[Y] - rect.bottom();
    }
    return hypot(dx, dy);
}

Coord distanceSq(Point const &p, OptRect const &rect)
{
    if (!rect) return std::numeric_limits<Coord>::max();
    return distanceSq(p, *rect);
}
Coord distance(Point const &p, OptRect const &rect)
{
    if (!rect) return std::numeric_limits<Coord>::max();
    return distance(p, *rect);
}

} // namespace Geom

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
