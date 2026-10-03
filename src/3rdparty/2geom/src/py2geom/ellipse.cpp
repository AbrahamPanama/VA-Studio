// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2009 Ricardo Lafuente <r@sollec.org>
 */

#include <boost/python.hpp>
#include <boost/python/implicit.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>

#include "py2geom.h"
#include "helpers.h"

#include "2geom/point.h"
#include "2geom/ellipse.h"
#include "2geom/circle.h"
#include "2geom/exception.h"
#include "2geom/d2.h"


void  (Geom::Ellipse::*ellipse_set1)(Geom::Point const &, Geom::Point const &, double) = &Geom::Ellipse::set;
void  (Geom::Ellipse::*ellipse_set2)(double, double, double, double, double) = &Geom::Ellipse::set;
std::vector<Geom::Coord> (Geom::Ellipse::*ellipse_coefficients)() const = &Geom::Ellipse::coefficients;

// i can't get these to work
//Geom::Point  (Geom::Ellipse::*center_point)() = (Geom::Point (*)() const)&Geom::Ellipse::center;
// Geom::Coord  (Geom::Ellipse::*center_coord)(Geom::Dim2 const& d) = &Geom::Ellipse::center;

using namespace boost::python;

void wrap_ellipse() {
    class_<Geom::Ellipse>("Ellipse", init<double, double, double, double, double>())
        .def(init<double, double, double, double, double, double>())
        // needs to be mapped to PointVec, but i can't figure out how
        .def(init<Geom::Circle>())

        .def("set", ellipse_set1)
        .def("set", ellipse_set2)
        .def("setCoefficients", &Geom::Ellipse::setCoefficients)
        .def("fit", &Geom::Ellipse::fit)
        
        .def("center", (Geom::Point (Geom::Ellipse::*)() const) &Geom::Ellipse::center)
        // .def("center", center_coord)
        
        .def("ray", &Geom::Ellipse::ray)
        .def("rotationAngle", &Geom::Ellipse::rotationAngle)
        .def("coefficients", ellipse_coefficients)
        .def(self * Geom::Affine())
        .def(self *= Geom::Affine())
        // requires EllipticalArc
        //.def("arc", &Geom::Ellipse::arc)
        
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
