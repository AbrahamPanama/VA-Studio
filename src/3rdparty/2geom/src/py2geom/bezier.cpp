// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2006, 2007 Aaron Spike <aaron@ekips.org>
 */

#include <boost/python.hpp>
#include <boost/python/implicit.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>

#include "py2geom.h"
#include "helpers.h"

#include "2geom/bezier.h"
#include "2geom/point.h"

using namespace boost::python;

double bezier_getitem(Geom::Bezier const& p, int index)
{
    int D = p.size();
    if (index < 0)
    {
        index = D + index;
    }
    if ((index < 0) || (index > (D - 1))) {
        PyErr_SetString(PyExc_IndexError, "index out of range");
        boost::python::throw_error_already_set();
    }
    return p[index];
}

void wrap_bezier() {
    //bezier.h

    class_<Geom::Bezier>("Bezier", init<double>())
        .def(init<double, double>())
        .def(init<double, double, double>())
        .def(init<double, double, double, double>())
        .def(self_ns::str(self))
        //TODO: add important vector funcs
        .def("__getitem__", &bezier_getitem)

        .def("isZero", &Geom::Bezier::isZero)
        .def("isFinite", &Geom::Bezier::isFinite)
        .def("at0", (double (Geom::Bezier::*)() const) &Geom::Bezier::at0)
        .def("at1", (double (Geom::Bezier::*)() const) &Geom::Bezier::at1)
        .def("valueAt", &Geom::Bezier::valueAt)
        .def("toSBasis", &Geom::Bezier::toSBasis)

        .def(self + float())
        .def(self - float())

        .def(self * float())
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
