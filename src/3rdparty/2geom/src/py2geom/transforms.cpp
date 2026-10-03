// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2006, 2007 Aaron Spike <aaron@ekips.org>
 */

#include <boost/python.hpp>

#include "py2geom.h"

#include "2geom/transforms.h"

using namespace boost::python;

//TODO: properly wrap other transforms

void wrap_transforms() {
    class_<Geom::Affine>("Affine", init<double, double, double, double, double, double>())
        .def(init<>())
        .def(init<Geom::Rotate>())
        .def(init<Geom::Scale>())
        .def(init<Geom::Translate>())
        .def(self_ns::str(self))
        .add_property("xAxis",&Geom::Affine::xAxis,&Geom::Affine::setXAxis)
        .add_property("yAxis",&Geom::Affine::yAxis,&Geom::Affine::setYAxis)
        .add_property("translation",&Geom::Affine::translation,&Geom::Affine::setTranslation)
        .def("isTranslation", &Geom::Affine::isTranslation)
        .def("isRotation", &Geom::Affine::isRotation)
        .def("isScale", &Geom::Affine::isScale)
        .def("isUniformScale", &Geom::Affine::isUniformScale)
        .def("setIdentity", &Geom::Affine::setIdentity)
        .def("inverse", &Geom::Affine::inverse)
        .def("det", &Geom::Affine::det)
        .def("descrim2", &Geom::Affine::descrim2)
        .def("descrim", &Geom::Affine::descrim)
        .def("expansionX", &Geom::Affine::expansionX)
        .def("expansionY", &Geom::Affine::expansionY)
        .def(self * self)
        .def(self * other<Geom::Translate>())
        .def(self * other<Geom::Scale>())
        .def(self * other<Geom::Rotate>())
    ;

    class_<Geom::Scale>("Scale", init<double, double>())
        .def(self == self)
        .def(self != self)
        .def("inverse", &Geom::Scale::inverse)
        .def(Geom::Point() * self)
        .def(self * self)
        .def(self * Geom::Affine())
    ;

    class_<Geom::Translate>("Translate", init<double, double>())
        .def(init<Geom::Point>())
        .def(self == self)
        .def(self != self)
        .def("inverse", &Geom::Translate::inverse)
        .def(Geom::Point() * self)
        .def(self * self)
        .def(self * other<Geom::Rotate>())
        .def(self * other<Geom::Scale>())
    ;

    class_<Geom::Rotate>("Rotate", init<double>())
        .def(self == self)
        .def(self != self)
        .def("inverse", &Geom::Rotate::inverse)
        .def("from_degrees", &Geom::Rotate::from_degrees)
        .staticmethod("from_degrees")
        .def(Geom::Point() * self)
        .def(self * self)
        .def(Geom::Affine() * self)
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
