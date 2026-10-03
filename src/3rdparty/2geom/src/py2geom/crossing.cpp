// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2009 Nathan Hurst <njh@njhurst.com>
 */

#include <boost/python.hpp>
#include <boost/python/implicit.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>

#include "py2geom.h"
#include "helpers.h"

#include "2geom/crossing.h"
#include "2geom/point.h"

using namespace boost::python;

void wrap_crossing() {
    //line.h

    class_<Geom::Crossing>("Crossing", init<>())
      .def(init<double, double, bool>())
      .def(init<double, double, unsigned, unsigned, bool>())
      .def_readonly("ta", &Geom::Crossing::ta)
      .def_readonly("tb", &Geom::Crossing::tb)
      .def_readonly("a", &Geom::Crossing::a)
      .def_readonly("b", &Geom::Crossing::b)
      .def_readonly("dir", &Geom::Crossing::dir)
      //.def(self_ns::str(self))
      .def("getOther", &Geom::Crossing::getOther)
      .def("getTime", &Geom::Crossing::getTime)
      .def("getOtherTime", &Geom::Crossing::getOtherTime)
      .def("onIx", &Geom::Crossing::onIx)
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
