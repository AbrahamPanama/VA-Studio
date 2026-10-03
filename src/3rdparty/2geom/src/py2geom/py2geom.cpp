// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Python bindings for lib2geom
 *
 * Copyright 2006, 2007 Aaron Spike <aaron@ekips.org>
 * Copyright 2007 Alex Mac <ajm@cs.nott.ac.uk>
 */
#include <boost/python.hpp>
#include <boost/python/implicit.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>

#include <2geom/geom.h>

#include "py2geom.h"

using namespace boost::python;

BOOST_PYTHON_MODULE(_py2geom)
{
    
    /*enum_<IntersectorKind>("IntersectorKind")
        .value("intersects", intersects)
        .value("parallel", parallel)
        .value("coincident", coincident)
        .value("no_intersection", no_intersection)
    ;
    def("segment_intersect", segment_intersect);*/
    
    wrap_point();
    wrap_etc();
    wrap_interval();
    wrap_transforms();
    wrap_rect();
    wrap_circle();
    wrap_ellipse();
    wrap_sbasis();
    wrap_bezier();
    wrap_linear();
    wrap_line();
    wrap_conic();
    wrap_pw();
    wrap_d2();
    wrap_parser();
    wrap_path();
    wrap_ray();
    // wrap_shape();
    wrap_crossing();
    // wrap_convex_cover();

}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(substatement-open . 0))
  indent-tabs-mode:nil
  c-brace-offset:0
  fill-column:99
  End:
  vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
*/
