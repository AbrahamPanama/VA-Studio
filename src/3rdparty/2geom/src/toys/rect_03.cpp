// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * SimpleRect examples
 *
 * Copyright 2009  Evangelos Katsikaros <vkatsikaros at yahoo dot gr>
 */

/*
Simple example of a toy that creates a rectangle on screen.
Now we use the 2 handles to define the rect

I am still very inexperienced with lib2geom
so don't use these examples as a reference :)
*/

#include <toys/path-cairo.h>
#include <toys/toy-framework-2.h>

using std::vector;
using namespace Geom;



class SimpleRect: public Toy {
    PointSetHandle psh;
    void draw( cairo_t *cr, std::ostringstream *notify,
                   int width, int height, bool save, std::ostringstream *timer_stream) override
	{
        Rect r1(psh.pts[0], psh.pts[1]);

        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
        cairo_set_line_width(cr, 0.3);
        cairo_rectangle(cr, r1);

	
        cairo_stroke(cr);

		Toy::draw(cr, notify, width, height, save, timer_stream);
}
    public:
    SimpleRect (unsigned no_of_handles) {
        handles.push_back(&psh);
        for(unsigned i = 0; i < no_of_handles; i++)
            psh.push_back( 200 + ( i * 20 ), 300 + ( i * 20 ) );
    }
};

int main(int argc, char **argv) {   
    unsigned no_of_handles=2;
    if(argc > 1)
        sscanf(argv[1], "%d", &no_of_handles);
    init(argc, argv, new SimpleRect(no_of_handles));

    return 0;
}

