// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * A simple toy to test the path along path
 *
 * Copyright 2007 Johan Engelen
 */

#include <iostream>
#include <2geom/path-sink.h>
#include <2geom/svg-path-parser.h>
#include <2geom/sbasis.h>
#include <2geom/sbasis-geometric.h>
#include <2geom/bezier-to-sbasis.h>
#include <2geom/sbasis-to-bezier.h>
#include <2geom/d2.h>
#include <2geom/piecewise.h>

Geom::Piecewise<Geom::D2<Geom::SBasis> >
doEffect_pwd2 (Geom::Piecewise<Geom::D2<Geom::SBasis> > & pwd2_in, Geom::Piecewise<Geom::D2<Geom::SBasis> > & pattern)
{
    using namespace Geom;

    Piecewise<D2<SBasis> > uskeleton = arc_length_parametrization(pwd2_in, 2, .1);
    uskeleton = remove_short_cuts(uskeleton,.01);
    Piecewise<D2<SBasis> > n = rot90(derivative(uskeleton));
    n = force_continuity(remove_short_cuts(n,.1));

    D2<Piecewise<SBasis> > patternd2 = make_cuts_independent(pattern);
    Piecewise<SBasis> x = Piecewise<SBasis>(patternd2[0]);
    Piecewise<SBasis> y = Piecewise<SBasis>(patternd2[1]);
    Interval pattBnds = *bounds_exact(x);
    x -= pattBnds.min();
    Interval pattBndsY = *bounds_exact(y);
    y -= (pattBndsY.max()+pattBndsY.min())/2;

    int nbCopies = int(uskeleton.cuts.back()/pattBnds.extent());
    double scaling = 1;

    double pattWidth = pattBnds.extent() * scaling;

    if (scaling != 1.0) {
        x*=scaling;
    }

    double offs = 0;
    Piecewise<D2<SBasis> > output;
    for (int i=0; i<nbCopies; i++){
        output.concat(compose(uskeleton,x+offs)+y*compose(n,x+offs));
        offs+=pattWidth;
    }

    return output;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        Geom::PathVector originald = Geom::parse_svg_path(&*argv[1]);
        Geom::Piecewise<Geom::D2<Geom::SBasis> > originaldpwd2;
        for (const auto & i : originald) {
            originaldpwd2.concat( i.toPwSb() );
        }

        Geom::PathVector pattern = Geom::parse_svg_path(&*argv[2]);
        Geom::Piecewise<Geom::D2<Geom::SBasis> > patternpwd2;
        for (const auto & i : pattern) {
            patternpwd2.concat( i.toPwSb() );
        }
        
        doEffect_pwd2(originaldpwd2, patternpwd2);
    }
    return 0;
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
