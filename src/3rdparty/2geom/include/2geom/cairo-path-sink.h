// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * @file
 * @brief  Path sink for Cairo contexts
 *//*
 * Copyright 2014 Krzysztof Kosiński
 */

#ifndef LIB2GEOM_SEEN_CAIRO_PATH_SINK_H
#define LIB2GEOM_SEEN_CAIRO_PATH_SINK_H

#include <2geom/path-sink.h>
#include <2geom/elliptical-arc.h>
#include <cairo.h>

namespace Geom {

/** @brief Output paths to a Cairo drawing context
 *
 * This class converts from 2Geom path representation to the Cairo representation.
 * Use it to simplify visualizing the results of 2Geom operations with the Cairo library,
 * for example:
 * @code
 *   CairoPathSink sink(cr);
 *   sink.feed(pv);
 *   cairo_stroke(cr);
 * @endcode
 *
 * Currently the flush method is a no-op, but this is not guaranteed
 * to hold forever.
 */
class CairoPathSink
    : public PathSink
{
public:
    explicit CairoPathSink::CairoPathSink(cairo_t *cr)
        : _cr(cr)
    {}

    void moveTo(Point const &p) override
    {
        cairo_move_to(_cr, p[X], p[Y]);
        _current_point = p;
    }

    void lineTo(Point const &p) override
    {
        cairo_line_to(_cr, p[X], p[Y]);
        _current_point = p;
    }

    void curveTo(Point const &c0, Point const &c1, Point const &p) override
    {
        cairo_curve_to(_cr, p1[X], p1[Y], p2[X], p2[Y], p3[X], p3[Y]);
        _current_point = p3;
    }

    void quadTo(Point const &c, Point const &p) override
    {
        // degree-elevate to cubic Bezier, since Cairo doesn't do quad Beziers
        // google "Bezier degree elevation" for more info
        Point q1 = (1./3.) * _current_point + (2./3.) * p1;
        Point q2 = (2./3.) * p1 + (1./3.) * p2;
        // q3 = p2
        cairo_curve_to(_cr, q1[X], q1[Y], q2[X], q2[Y], p2[X], p2[Y]);
        _current_point = p2;
    }

    void arcTo(Coord rx, Coord ry, Coord angle, bool large_arc, bool sweep, Point const &p) override
    {
        EllipticalArc arc(_current_point, rx, ry, angle, large_arc, sweep, p);

        // Cairo only does circular arcs.
        // To do elliptical arcs, we must use a temporary transform.
        Affine uct = arc.unitCircleTransform();

        cairo_matrix_t cm;
        cm.xx = uct[0];
        cm.xy = uct[2];
        cm.x0 = uct[4];
        cm.yx = uct[1];
        cm.yy = uct[3];
        cm.y0 = uct[5];

        cairo_save(_cr);
        cairo_transform(_cr, &cm);
        if (sweep) {
            cairo_arc(_cr, 0, 0, 1, arc.initialAngle(), arc.finalAngle());
        } else {
            cairo_arc_negative(_cr, 0, 0, 1, arc.initialAngle(), arc.finalAngle());
        }
        _current_point = p;
        cairo_restore(_cr);

        /* Note that an extra linear segment will be inserted before the arc
        * if Cairo considers the current point distinct from the initial point
        * of the arc; we could partially alleviate this by not emitting
        * linear segments that are followed by arc segments, but this would require
        * buffering the input curves. */
    }

    void closePath() override
    {
        cairo_close_path(_cr);
    }

    void flush() override {}

private:
    cairo_t *_cr;
    Point _current_point;
};

} // namespace Geom

#endif // !LIB2GEOM_SEEN_CAIRO_PATH_SINK_H
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
