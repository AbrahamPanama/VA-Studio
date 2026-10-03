// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file
 * \brief Performance test for Boolops
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 * 
 * Copyright 2015 Authors
 */

#include <2geom/intersection-graph.h>
#include <2geom/svg-path-parser.h>
#include <iostream>
#include <glib.h>

using namespace Geom;

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cout << "boolops: wrong number of arguments; no tests run!" << std::endl;
        exit(0); // TODO: add suitable arguments in CMake target / actually do some tests here
    }

    PathVector a = read_svgd(argv[2]);
    PathVector b = read_svgd(argv[3]);
    unsigned const ops = atoi(argv[1]);

    OptRect abox = a.boundsExact();
    OptRect bbox = a.boundsExact();
    if (!abox) {
        std::cout << argv[1] << " contains an empty path" << std::endl;
        std::exit(1);
    }
    if (!bbox) {
        std::cout << argv[2] << " contains an empty path" << std::endl;
        std::exit(1);
    }

    a *= Translate(-abox->corner(0));
    b *= Translate(-bbox->corner(0));

    long num_intersections = 0;
    long num_outcv = 0;

    // for reproducibility.
    g_random_set_seed(1234);

    for (unsigned i = 0; i < ops; ++i) {
        Point delta;
        delta[X] = g_random_double_range(-bbox->width(), abox->width());
        delta[Y] = g_random_double_range(-bbox->height(), abox->height());

        PathVector bt = b * Translate(delta);

        PathIntersectionGraph pig(a, bt);
        PathVector x = pig.getIntersection();
        num_intersections += pig.intersectionPoints().size();
        num_outcv += x.curveCount();
    }

    std::cout << "Completed " << ops << " operations.\n"
              << "Total intersections: " << num_intersections << "\n"
              << "Total output curves: " << num_outcv << std::endl;

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
