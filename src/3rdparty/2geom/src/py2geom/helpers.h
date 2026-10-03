// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2006, 2007 Aaron Spike <aaron@ekips.org>
 */

#ifndef SEEN_PY2GEOM_HELPERS_H
#define SEEN_PY2GEOM_HELPERS_H

#include <boost/python.hpp>

template <typename T, typename R, unsigned D>
R python_getitem(T const& p, int index)
{
    unsigned i = index;
    if (index < 0)
    {
        i = index = D + index;
    }
    if ((index < 0) || (i > (D - 1))) {
        PyErr_SetString(PyExc_IndexError, "index out of range");
        boost::python::throw_error_already_set();
    }
    return p[i];
}

#endif
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
