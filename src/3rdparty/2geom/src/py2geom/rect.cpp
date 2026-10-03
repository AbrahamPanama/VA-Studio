// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2008 Aaron Spike <aaron@ekips.org>
 */

#include <boost/python.hpp>
#include <boost/python/implicit.hpp>

#include "py2geom.h"
#include "helpers.h"

#include "2geom/affine.h"
#include "2geom/d2.h"
#include "2geom/interval.h"

using namespace boost::python;

static bool wrap_contains_coord(Geom::Rect const &x, Geom::Point val) {
    return x.contains(val);
}

static bool wrap_contains_ivl(Geom::Rect const &x, Geom::Rect val) {
    return x.contains(val);
}

static bool wrap_interiorContains_coord(Geom::Rect const &x, Geom::Point val) {
    return x.interiorContains(val);
}

static bool wrap_interiorContains_ivl(Geom::Rect const &x, Geom::Rect val) {
    return x.interiorContains(val);
}

static void wrap_expandBy_pt(Geom::Rect &x, Geom::Point val) {
    x.expandBy(val);
}

static void wrap_expandBy(Geom::Rect &x, double val) {
    x.expandBy(val);
}

static void wrap_unionWith(Geom::Rect &x, Geom::Rect const &y) {
    x.unionWith(y);
}
static bool wrap_intersects(Geom::Rect const &x, Geom::Rect const &y) {
    return x.intersects(y);
}

void wrap_rect() {
    //TODO: fix overloads
    //def("unify", Geom::unify);
    def("union_list", Geom::union_list);
    //def("intersect", Geom::intersect);
    def("distanceSq", (double (*)( Geom::Point const&, Geom::Rect const&  ))Geom::distanceSq);
    def("distance", (double (*)( Geom::Point const&, Geom::Rect const&  ))Geom::distance);

    class_<Geom::Rect>("Rect", init<Geom::Interval, Geom::Interval>())
        .def(init<Geom::Point,Geom::Point>())
        .def(init<>())
        .def(init<Geom::Rect const &>())
        
        .def("__getitem__", python_getitem<Geom::Rect,Geom::Interval,2>)
    
        .def("min", &Geom::Rect::min)
        .def("max", &Geom::Rect::max)
        .def("corner", &Geom::Rect::corner)
        .def("top", &Geom::Rect::top)
        .def("bottom", &Geom::Rect::bottom)
        .def("left", &Geom::Rect::left)
        .def("right", &Geom::Rect::right)
        .def("width", &Geom::Rect::width)
        .def("height", &Geom::Rect::height)
        .def("dimensions", &Geom::Rect::dimensions)
        .def("midpoint", &Geom::Rect::midpoint)
        .def("area", &Geom::Rect::area)
        .def("maxExtent", &Geom::Rect::maxExtent)
        .def("contains", wrap_contains_coord)
        .def("contains", wrap_contains_ivl)
        .def("interiorContains", wrap_interiorContains_coord)
        .def("interiorContains", wrap_interiorContains_ivl)
        .def("intersects", wrap_intersects)
        .def("expandTo", &Geom::Rect::expandTo)
        .def("unionWith", &wrap_unionWith)
        // TODO: overloaded
        .def("expandBy", wrap_expandBy)
        .def("expandBy", wrap_expandBy_pt)
        
        .def(self * Geom::Affine())
    ;

};

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
