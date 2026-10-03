// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Show off crossings between two Bezier curves.
 * The intersection points are found by using Bezier clipping.
 *
 * Authors:
 *      Marco Cecchetti <mrcekets at gmail.com>
 *
 * Copyright 2008  authors
 */


#include <toys/path-cairo.h>
#include <toys/toy-framework-2.h>

#include <2geom/d2.h>
#include <2geom/basic-intersection.h>
#include <2geom/sbasis-to-bezier.h>




using namespace Geom;


class CurveIntersect : public Toy
{

    void draw( cairo_t *cr, std::ostringstream *notify,
               int width, int height, bool save, std::ostringstream *timer_stream) override
    {
        cairo_set_line_width (cr, 0.3);
        cairo_set_source_rgba (cr, 0.8, 0., 0, 1);
        //pshA.pts.back() = pshB.pts[0];
        D2<SBasis> A = pshA.asBezier();
        cairo_d2_sb(cr, A);
        cairo_stroke(cr);
        cairo_set_source_rgba (cr, 0.0, 0., 0, 1);
        D2<SBasis> B = pshB.asBezier();
        cairo_d2_sb(cr, B);
        cairo_stroke(cr);

        find_intersections_bezier_clipping(xs, pshA.pts, pshB.pts, m_precision);
        cairo_set_line_width (cr, 0.3);
        cairo_set_source_rgba (cr, 0.0, 0.0, 0.7, 1);
        for (auto & x : xs)
        {
            draw_handle(cr, A(x.first));
            draw_handle(cr, B(x.second));
        }
        cairo_stroke(cr);

        Toy::draw(cr, notify, width, height, save,timer_stream);
    }


public:
    CurveIntersect(unsigned int _A_bez_ord, unsigned int _B_bez_ord)
        : A_bez_ord(_A_bez_ord), B_bez_ord(_B_bez_ord)
    {
        handles.push_back(&pshA);
        for (unsigned int i = 0; i <= A_bez_ord; ++i)
            pshA.push_back(Geom::Point(uniform()*400, uniform()*400)+Point(200,200));
        handles.push_back(&pshB);
        for (unsigned int i = 0; i <= B_bez_ord; ++i)
            pshB.push_back(Geom::Point(uniform()*400, uniform()*400)+Point(200,200));

        m_precision = 1e-6;
    }

private:
    unsigned int A_bez_ord, B_bez_ord;
    PointSetHandle pshA, pshB, pshC;
    std::vector< std::pair<double, double> > xs;
    double m_precision;
};


int main(int argc, char **argv)
{
    unsigned int A_bez_ord = 6;
    unsigned int B_bez_ord = 8;
    if(argc > 1)
        sscanf(argv[1], "%d", &A_bez_ord);
    if(argc > 2)
        sscanf(argv[2], "%d", &B_bez_ord);


    init( argc, argv, new CurveIntersect(A_bez_ord, B_bez_ord), 800, 800);
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


