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
#include "2geom/ray.h"
// #include "2geom/bezier_curve.h"
#include "2geom/exception.h"


using namespace boost::python;

bool (*are_near_ray)(Geom::Point const& _point, Geom::Ray const& _ray, double eps) = &Geom::are_near;
double (*angle_between_ray)(Geom::Ray const& r1, Geom::Ray const& r2, bool cw) = &Geom::angle_between;


double angle_between_ray_def(Geom::Ray const& r1, Geom::Ray const& r2) {
    return Geom::angle_between(r1, r2);
}
double (*distance_ray)(Geom::Point const& _point, Geom::Ray const& _ray) = &Geom::distance;

// why don't these compile?
//Geom::Point (*get_ray_origin)(Geom::Ray const) = &Geom::Ray::origin;
//void (*set_ray_origin)(Geom::Ray const, Geom::Point const& _point) = &Geom::Ray::origin;

void wrap_ray() {
    def("distance", distance_ray);
    def("are_near", are_near_ray);
    def("are_same", Geom::are_same);
    def("angle_between", angle_between_ray);
    def("angle_between", angle_between_ray_def);
    def("make_angle_bisector_ray", Geom::make_angle_bisector_ray);

    class_<Geom::Ray>("Ray", init<Geom::Point, Geom::Coord>())
        .def(init<Geom::Point,Geom::Point>())
        .def(init<>())
            
        // TODO: overloaded
        //.add_property("origin", get_ray_origin, set_ray_origin) 
        // .add_property("versor", &Geom::Ray::versor, &Geom::Ray::versor)
        // .add_property("angle", &Geom::Ray::angle, &Geom::Ray::angle)

        .def("isDegenerate", &Geom::Ray::isDegenerate)
        .def("nearestTime", &Geom::Ray::nearestTime) 
        .def("setBy2Points", &Geom::Ray::setPoints)
        .def("valueAt", &Geom::Ray::valueAt)
        .def("pointAt", &Geom::Ray::pointAt)
        .def("nearestTime", &Geom::Ray::nearestTime)
        .def("reverse", &Geom::Ray::reverse) 
        .def("roots", &Geom::Ray::roots) 
        .def("transformed", &Geom::Ray::transformed) 
        // requires Curve
        // .def("portion", &Geom::Ray::portion) 
        .def("segment", &Geom::Ray::segment) 
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
