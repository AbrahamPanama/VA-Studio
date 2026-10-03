// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file arc-length.h
 * \brief Arc length computations for paths
 *//*
 * Copyright 2006 Nathan Hurst <njh@mail.csse.monash.edu.au>
 */

#ifndef __2GEOM_ARC_LENGTH_H
#define __2GEOM_ARC_LENGTH_H

#include <2geom/path.h>

/* Routines in this group return a path that looks the same, but
 * include extra knots for certain points of interest. */

/* find_vector_extreme_points
 * extreme points . dir.
 */

double arc_length_subdividing(Geom::Path const & p, double tol);
double arc_length_integrating(Geom::Path const & p, double tol);

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
