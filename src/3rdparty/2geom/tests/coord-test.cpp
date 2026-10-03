// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file
 * @brief Unit tests for functions related to Coord.
 * Uses the Google Testing Framework
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 * 
 * Copyright 2014 Authors
 */

#include <gtest/gtest.h>
#include <2geom/coord.h>
#include <climits>
#include <stdint.h>
#include <glib.h>
#include <iostream>

namespace Geom {

TEST(CoordTest, StringRoundtripShortest) {
    union {
        uint64_t u;
        double d;
    };
    for (unsigned i = 0; i < 100000; ++i) {
        u = uint64_t(g_random_int()) | (uint64_t(g_random_int()) << 32);
        if (!std::isfinite(d)) continue;

        std::string str = format_coord_shortest(d);
        double x = parse_coord(str);
        if (x != d) {
            std::cout << std::endl << d << " -> " << str << " -> " << x << std::endl;
        }
        EXPECT_EQ(d, x);
    }
}

TEST(CoordTest, StringRoundtripNice) {
    union {
        uint64_t u;
        double d;
    };
    for (unsigned i = 0; i < 100000; ++i) {
        u = uint64_t(g_random_int()) | (uint64_t(g_random_int()) << 32);
        if (!std::isfinite(d)) continue;

        std::string str = format_coord_nice(d);
        double x = parse_coord(str);
        if (x != d) {
            std::cout << std::endl << d << " -> " << str << " -> " << x << std::endl;
        }
        EXPECT_EQ(d, x);
    }
}

} // end namespace Geom

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
