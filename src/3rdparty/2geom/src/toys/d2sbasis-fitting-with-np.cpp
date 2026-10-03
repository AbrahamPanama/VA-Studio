// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * D2<SBasis> Fitting Example
 *
 * Authors:
 *      Marco Cecchetti <mrcekets at gmail.com>
 * 
 * Copyright 2008  authors
 */


#include <2geom/numeric/fitting-tool.h>
#include <2geom/numeric/fitting-model.h>

#include <2geom/d2.h>
#include <2geom/sbasis.h>

#include <toys/path-cairo.h>
#include <toys/toy-framework-2.h>


using namespace Geom;


class D2SBasisFitting : public Toy
{
  private:
    void draw( cairo_t *cr, std::ostringstream *notify, 
               int width, int height, bool save, std::ostringstream *timer_stream) override
    {
        bool changed = false;
        for (size_t i = 0; i < total_handles; ++i)
        {
            if (psh.pts[i] !=  prev_pts[i])
            {
                changed = true;
                break;
            }
        }
        if (changed)
        {
            for (size_t k = 0; k < 200; ++k)
            {
                lsf_2dsb.clear();
                lsf_2dsb.append(0);
                lsf_2dsb.append(0.33);
                double t = 0;
                for (size_t i = 2; i < total_handles-2; ++i)
                {
                    t = nearest_time(psh.pts[i], sb_curve);
                    lsf_2dsb.append(t);
                }
                lsf_2dsb.append(0.66);
                lsf_2dsb.append(1);
                lsf_2dsb.update();
                fmd2sb.instance(sb_curve, lsf_2dsb.result(psh.pts));
            }
            prev_pts = psh.pts;
        }
        
        cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 1.0);
        cairo_set_line_width (cr, 0.3);
        cairo_d2_sb(cr, sb_curve);
        cairo_stroke(cr);
        
        Toy::draw(cr, notify, width, height, save,timer_stream);
    }
    
  public:
    D2SBasisFitting()
        : sb_curve(),
          total_handles(8),
          order(total_handles / 2 - 1),
          fmd2sb(order),
          lsf_2dsb(fmd2sb, total_handles),
          prev_pts()
    {
        step = 1.0 / (total_handles - 1);
        for (size_t i = 0; i < total_handles; ++i)
        {
            psh.push_back(400*uniform() + 50, 300*uniform() + 50);
        }
        handles.push_back(&psh);
        
        double t = 0;
        for (size_t i = 0; i < total_handles; ++i)
        {
            lsf_2dsb.append(t);
            t += step;
        }
        prev_pts = psh.pts;
        lsf_2dsb.update();
        fmd2sb.instance(sb_curve, lsf_2dsb.result(prev_pts));        
    }
    
  private:
    D2<SBasis> sb_curve;
    unsigned int total_handles;
    unsigned int order;
    NL::LFMD2SBasis fmd2sb;
    NL::least_squeares_fitter<NL::LFMD2SBasis> lsf_2dsb;
    std::vector<Point> prev_pts;
    double step;
    PointSetHandle psh;
};



int main(int argc, char **argv) 
{   
    init( argc, argv, new D2SBasisFitting(), 600, 600 );
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
