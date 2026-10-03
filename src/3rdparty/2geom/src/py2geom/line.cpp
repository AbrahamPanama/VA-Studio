// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2009 Nathan Hurst <njh@njhurst.com>
 */

#include <boost/python.hpp>
#include <boost/python/implicit.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>

#include "py2geom.h"
#include "helpers.h"

#include "2geom/line.h"
//#include "2geom/bezier-curve.h"
#include "2geom/point.h"

using namespace boost::python;

template <typename S, typename T>
object wrap_intersection(S const& a, T const& b) {
    Geom::OptCrossing oc = intersection(a, b);
    return oc?object(*oc):object();
}

std::vector<Geom::Coord> (Geom::Line::*coefficients_vec)() const = &Geom::Line::coefficients;

void wrap_line() {
    //line.h

    def("intersection", wrap_intersection<Geom::Line, Geom::Line>);
    def("intersection", wrap_intersection<Geom::Line, Geom::Ray>);
    //def("intersection", wrap_intersection<Geom::Line, Geom::LineSegment>);
    def("intersection", wrap_intersection<Geom::Ray, Geom::Line>);
    def("intersection", wrap_intersection<Geom::Ray, Geom::Ray>);
    //def("intersection", wrap_intersection<Geom::Ray, Geom::LineSegment>);
    //def("intersection", wrap_intersection<Geom::LineSegement, Geom::Line>);
    //def("intersection", wrap_intersection<Geom::LineSegement, Geom::Ray>);
    //def("intersection", wrap_intersection<Geom::LineSegement, Geom::LineSegment>);
    class_<Geom::Line>("Line", init<>())
        .def(init<Geom::Point const&, Geom::Coord>())
        .def(init<Geom::Point const&, Geom::Point const&>())
        .def(init<double, double, double>())
        //.def(self_ns::str(self))
        .def("valueAt", &Geom::Line::valueAt)

        .def("coefficients", coefficients_vec)
        .def("isDegenerate", &Geom::Line::isDegenerate)
        .def("pointAt", &Geom::Line::pointAt)
        .def("roots", &Geom::Line::roots)
        .def("nearestTime", &Geom::Line::nearestTime)
        .def("reverse", &Geom::Line::reverse)
        //.def("portion", &Geom::Line::portion)
        //.def("segment", &Geom::Line::segment)
        .def("derivative", &Geom::Line::derivative)
        .def("transformed", &Geom::Line::transformed)
        .def("normal", &Geom::Line::normal)
        .def("normalAndDist", &Geom::Line::normalAndDist)
        .def("setPoints", &Geom::Line::setPoints)
        .def("setCoefficients", &Geom::Line::setCoefficients)
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
