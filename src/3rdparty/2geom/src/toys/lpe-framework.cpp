// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * A framework for writing an Inkscape Live Path Effect (LPE) toy.
 *
 * Copyright 2009  Johan Engelen <goejendaagh@zonnet.nl>
 */
 
#include <string.h>
#include <stdint.h>
#include <toys/lpe-framework.h>

#include <2geom/sbasis-to-bezier.h>
#include <2geom/affine.h>
#include <2geom/pathvector.h>

#define  LPE_CONVERSION_TOLERANCE 0.01

/**
 * When the input path handles are moved, this method is called to re-execute the LPE and draw the result.
 */
void LPEToy::draw(cairo_t *cr, std::ostringstream *notify, int width, int height, bool save, std::ostringstream *timer_stream)
{
    using namespace Geom;

    Piecewise<D2<SBasis> > pwd2(curve_handle.asBezier());
    PathVector A = Geom::path_from_piecewise( pwd2, LPE_CONVERSION_TOLERANCE);
    cairo_set_line_width (cr, 2);
    cairo_set_source_rgba (cr, 1., 0.0, 0., 1);
    cairo_path(cr, A);
    cairo_stroke(cr);

    // perform the effect:
    PathVector B = doEffect_path(A);

    cairo_set_line_width (cr, 1);
    cairo_set_source_rgba (cr, 0., 0.0, 0., 1);
    cairo_path(cr, B);
    cairo_stroke(cr);

    Toy::draw(cr, notify, width, height, save,timer_stream);
}

/**
 * Initializes the LPE toy, and sets a simple default input path.
 */
LPEToy::LPEToy(){
    if(handles.empty()) {
        handles.push_back(&curve_handle);
        for(unsigned i = 0; i < 4; i++) {
            curve_handle.push_back(150+uniform()*300,150+uniform()*300);
        }
    }
}

/*
 *  Here be the doEffect function chain:  (this is copied code from Inkscape)
 */
Geom::PathVector
LPEToy::doEffect_path (Geom::PathVector const &path_in)
{
    Geom::PathVector path_out;

    if ( !concatenate_before_pwd2 ) {
        // default behavior
        for (const auto & i : path_in) {
            Geom::Piecewise<Geom::D2<Geom::SBasis> > pwd2_in = i.toPwSb();
            Geom::Piecewise<Geom::D2<Geom::SBasis> > pwd2_out = doEffect_pwd2(pwd2_in);
            Geom::PathVector path = Geom::path_from_piecewise( pwd2_out, LPE_CONVERSION_TOLERANCE);
            // add the output path vector to the already accumulated vector:
            for (const auto & j : path) {
                path_out.push_back(j);
            }
        }
    } else {
      // concatenate the path into possibly discontinuous pwd2
        Geom::Piecewise<Geom::D2<Geom::SBasis> > pwd2_in;
        for (const auto & i : path_in) {
            pwd2_in.concat( i.toPwSb() );
        }
        Geom::Piecewise<Geom::D2<Geom::SBasis> > pwd2_out = doEffect_pwd2(pwd2_in);
        path_out = Geom::path_from_piecewise( pwd2_out, LPE_CONVERSION_TOLERANCE);
    }

    return path_out;
}

Geom::Piecewise<Geom::D2<Geom::SBasis> >
LPEToy::doEffect_pwd2 (Geom::Piecewise<Geom::D2<Geom::SBasis> > const & pwd2_in)
{
    // default effect does nothing
    return pwd2_in;
}



/*
	Local Variables:
	mode:c++
	c-file-style:"stroustrup"
	c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
	indent-tabs-mode:nil
	fill-column:99
	End:
      */
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=4:softtabstop=4:fileencoding=utf-8:textwidth=99 :
