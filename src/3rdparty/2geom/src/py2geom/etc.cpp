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
#include "2geom/sbasis.h"
#include "2geom/exception.h"

using namespace boost::python;

void wrap_etc() {
    // needed for roots
    class_<DoubleVec >("DoubleVec")
        .def(vector_indexing_suite<std::vector<double> >())
    ;
    class_<PointVec>("PointVec")
        .def(vector_indexing_suite<std::vector<Geom::Point> >())
    ;
    // sbasis is a subclass of
    class_<LinearVec >("LinearVec")
        .def(vector_indexing_suite<std::vector<Geom::Linear> >())
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
