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
#include "2geom/circle.h"
#include "2geom/exception.h"

// i can't get these to work
//Geom::Point  (Geom::Circle::*center_point)() = (Geom::Point (*)() const)&Geom::Circle::center;
//Geom::Coord  (Geom::Circle::*center_coord)(Geom::Dim2 const& d) = &Geom::Circle::center;

using namespace boost::python;

void wrap_circle() {

    class_<Geom::Circle>("Circle", init<double, double, double>())
        .def(init<double, double, double, double>())
            
        .def("setCoefficients", &Geom::Circle::setCoefficients)
        .def("fit", &Geom::Circle::fit)
        .add_property("radius", &Geom::Circle::radius)
        
        .add_property("center", (Geom::Point (Geom::Circle::*)() const )&Geom::Circle::center)
        //.def("center", center)        
        // requires EllipticalArc
        //.def("arc", &Geom::Circle::arc)
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
