// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file
 * @brief some std functions to work with (pw)s-basis
 *//*
 *  Authors:
 *   Jean-Francois Barraud
 *
 * Copyright (C) 2006-2007 authors
 */

//this a first try to define sqrt, cos, sin, etc...
//TODO: define a truncated compose(sb,sb, order) and extend it to pw<sb>.
//TODO: in all these functions, compute 'order' according to 'tol'.
//TODO: use template to define the pw version automatically from the sb version?

#ifndef LIB2GEOM_SEEN_SBASIS_MATH_H
#define LIB2GEOM_SEEN_SBASIS_MATH_H


#include <2geom/sbasis.h>
#include <2geom/piecewise.h>
#include <2geom/d2.h>

namespace Geom{
//-|x|---------------------------------------------------------------
Piecewise<SBasis> abs(          SBasis const &f);
Piecewise<SBasis> abs(Piecewise<SBasis>const &f);

//- max(f,g), min(f,g) ----------------------------------------------
Piecewise<SBasis> max(          SBasis  const &f,           SBasis  const &g);
Piecewise<SBasis> max(Piecewise<SBasis> const &f,           SBasis  const &g);
Piecewise<SBasis> max(          SBasis  const &f, Piecewise<SBasis> const &g);
Piecewise<SBasis> max(Piecewise<SBasis> const &f, Piecewise<SBasis> const &g);
Piecewise<SBasis> min(          SBasis  const &f,           SBasis  const &g);
Piecewise<SBasis> min(Piecewise<SBasis> const &f,           SBasis  const &g);
Piecewise<SBasis> min(          SBasis  const &f, Piecewise<SBasis> const &g);
Piecewise<SBasis> min(Piecewise<SBasis> const &f, Piecewise<SBasis> const &g);

//-sign(x)---------------------------------------------------------------
Piecewise<SBasis> signSb(          SBasis const &f);
Piecewise<SBasis> signSb(Piecewise<SBasis>const &f);

//-Sqrt---------------------------------------------------------------
Piecewise<SBasis> sqrt(          SBasis const &f, double tol=1e-3, int order=3);
Piecewise<SBasis> sqrt(Piecewise<SBasis>const &f, double tol=1e-3, int order=3);

//-sin/cos--------------------------------------------------------------
Piecewise<SBasis> cos(          SBasis  const &f, double tol=1e-3, int order=3);
Piecewise<SBasis> cos(Piecewise<SBasis> const &f, double tol=1e-3, int order=3);
Piecewise<SBasis> sin(          SBasis  const &f, double tol=1e-3, int order=3);
Piecewise<SBasis> sin(Piecewise<SBasis> const &f, double tol=1e-3, int order=3);
//-Log---------------------------------------------------------------
Piecewise<SBasis> log(          SBasis const &f, double tol=1e-3, int order=3);
Piecewise<SBasis> log(Piecewise<SBasis>const &f, double tol=1e-3, int order=3);

//--1/x------------------------------------------------------------
//TODO: change this...
Piecewise<SBasis> reciprocalOnDomain(Interval range, double tol=1e-3);
Piecewise<SBasis> reciprocal(          SBasis const &f, double tol=1e-3, int order=3);
Piecewise<SBasis> reciprocal(Piecewise<SBasis>const &f, double tol=1e-3, int order=3);

//--interpolate------------------------------------------------------------
Piecewise<SBasis> interpolate( std::vector<double> times, std::vector<double> values, unsigned smoothness = 1);
}

#endif //SEEN_GEOM_PW_SB_CALCULUS_H

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
