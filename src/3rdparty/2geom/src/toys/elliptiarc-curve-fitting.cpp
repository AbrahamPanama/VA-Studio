// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Elliptical Arc Fitting Toy
 *
 * Authors:
 * 		Marco Cecchetti <mrcekets at gmail.com>
 *
 * Copyright 2008  authors
 */

#include <gsl/gsl_linalg.h>

#include <2geom/d2.h>
#include <2geom/sbasis.h>
#include <2geom/path.h>
#include <2geom/bezier-to-sbasis.h>
#include <2geom/elliptical-arc.h>

#include <toys/path-cairo.h>
#include <toys/toy-framework-2.h>



using namespace Geom;



class EAFittingToy : public Toy
{
  private:
    void draw( cairo_t *cr,	std::ostringstream *notify,
  	      	   int width, int height, bool save, std::ostringstream *timer_stream) override
    {
    	cairo_set_line_width (cr, 0.2);
    	cairo_set_source_rgb(cr, 0.0, 0.0, 0.);
    	//D2<SBasis> SB = handles_to_sbasis(handles.begin(), total_handles - 1);
    	D2<SBasis> SB = psh.asBezier();
    	cairo_d2_sb(cr, SB);
    	cairo_stroke(cr);

    	cairo_set_line_width (cr, 0.4);
    	cairo_set_source_rgba(cr, 0.0, 0.0, 0.7, 1.0);
    	try
    	{
    		EllipticalArc EA;
		if (!arc_from_sbasis(EA, SB, tolerance, 10)) {
//    			*notify << "distance error: " << convert.get_error()
//    			        << " ( " << convert.get_bound() << " )" << std::endl
//    			        << "angle error: " << convert.get_angle_error()
//    			        << " ( " << convert.get_angle_tolerance() << " )";
    			Toy::draw(cr, notify, width, height, save,timer_stream);
    			return;
    		}
    		D2<SBasis> easb = EA.toSBasis();
        	cairo_d2_sb(cr, easb);
        	cairo_stroke(cr);
    	}
        catch (RangeError const &e)
        {
        	std::cerr << e.what() << std::endl;
        	Toy::draw(cr, notify, width, height, save,timer_stream);
        	return;
        }

    	Toy::draw(cr, notify, width, height, save,timer_stream);
    }

  public:
	EAFittingToy( double _tolerance )
		: tolerance(_tolerance)
	{
	    handles.push_back(&psh);
		total_handles = 6;
		for ( unsigned int i = 0; i < total_handles; ++i )
		{
			psh.push_back(uniform()*400, uniform()*400);
		}
	}

	PointSetHandle psh;
	unsigned int total_handles;
	double tolerance;
};



int main(int argc, char **argv)
{
	double tolerance = 8;
	if(argc > 1)
	        sscanf(argv[1], "%lf", &tolerance);
    init( argc, argv, new EAFittingToy(tolerance) );
    return 0;
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
