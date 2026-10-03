// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * SBasis Fitting Example
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


class SBasisFitting : public Toy
{
  private:
    void draw( cairo_t *cr, std::ostringstream *notify, 
               int width, int height, bool save, std::ostringstream *timer_stream) override
    {
        sliders[0].geometry(Point(50, 30), 100);
       
        size_t value = (size_t)(sliders[0].value());
        
        if (degree != value)
        {
            degree = value;
            total_handles = degree + 1;
            order = total_handles/2 - 1;
            dstep = interval_length/degree;
            step = 1.0/degree;
            
            double x = sx;
            psh.pts.clear();
            for (size_t i = 0; i < total_handles; ++i)
            {
                psh.push_back(x, 300*uniform()+50);
                x += dstep;
            }
            
            handles[0] = &psh;
            
            if (fmsb != NULL) delete fmsb;
            fmsb = new NL::LFMSBasis(order);
            assert(fmsb != NULL);
            if (lsf_sb != NULL) delete lsf_sb;
            lsf_sb = new NL::least_squeares_fitter<NL::LFMSBasis>(*fmsb, 25);
            assert(lsf_sb != NULL);
            
            double t = 0;
            for (size_t i = 0; i < total_handles; ++i)
            {
                lsf_sb->append(t);
                t += step;
            }
            lsf_sb->update();

            curr_ys.clear();
            curr_ys.resize(total_handles);
            for (size_t i = 0; i < total_handles; ++i)
            {
                curr_ys[i] = psh.pts[i][Y];
            }
            prev_ys = curr_ys;

            fmsb->instance(sb_curve, lsf_sb->result(curr_ys));
        }
        else
        {
            double x = sx;
            for (size_t i = 0; i < total_handles; ++i)
            {
                psh.pts[i][X] = x;
                curr_ys[i] = psh.pts[i][Y];
                x += dstep;
            }
            fmsb->instance(sb_curve, lsf_sb->result(prev_ys, curr_ys));
            prev_ys = curr_ys;
        }
        
        
        D2<SBasis> curve;
        curve[X] = SBasis(Linear(sx,sx) + interval_length * Linear(0, 1));
        curve[Y] = sb_curve;

        cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 1.0);
        cairo_set_line_width (cr, 0.3);
        cairo_d2_sb(cr, curve);
        cairo_stroke(cr);
        Toy::draw(cr, notify, width, height, save,timer_stream);
    }
    
  public:
    SBasisFitting()
        : degree(3),
          total_handles(degree+1),
          order(total_handles/2 - 1),
          interval_length(400),
          dstep(interval_length/degree),
          step(1.0/degree)
    {
        sx = 50;
        double x = sx;
        for (size_t i = 0; i < total_handles; ++i)
        {
            psh.push_back(x, 300*uniform()+50);
            x += dstep;
        }
        
        handles.push_back(&psh);
        
        fmsb = new NL::LFMSBasis(order);
        assert(fmsb != NULL);
        lsf_sb = new NL::least_squeares_fitter<NL::LFMSBasis>(*fmsb, 25);
        assert(lsf_sb != NULL);

        double t = 0;
        for (size_t i = 0; i < total_handles; ++i)
        {
            lsf_sb->append(t);
            t += step;
        }
        lsf_sb->update();
        
        curr_ys.clear();
        curr_ys.resize(total_handles);
        for (size_t i = 0; i < total_handles; ++i)
        {
            curr_ys[i] = psh.pts[i][Y];
        }
        prev_ys = curr_ys;
        
        fmsb->instance(sb_curve, lsf_sb->result(curr_ys));
        
        
        sliders.emplace_back(1, 11, 2, 3, "degree");
        handles.push_back(&(sliders[0]));
    }
    
    ~SBasisFitting() override
    {
        if (fmsb != NULL) delete fmsb;
        if (lsf_sb != NULL) delete lsf_sb;
    }
    
  private:
    size_t degree;
    size_t total_handles;
    size_t order;
    double interval_length;
    double dstep;
    double step;
    double sx;
    std::vector<double> curr_ys, prev_ys;
    SBasis sb_curve;
    NL::LFMSBasis* fmsb;
    NL::least_squeares_fitter<NL::LFMSBasis>* lsf_sb;    
    PointSetHandle psh;
    std::vector<Slider> sliders;
};




int main(int argc, char **argv) 
{   
    init( argc, argv, new SBasisFitting(), 600, 600 );
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

