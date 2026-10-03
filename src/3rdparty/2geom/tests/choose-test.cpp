// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file
 * @brief Unit tests for the binomial coefficient function.
 * Uses the Google Testing Framework
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 * 
 * Copyright 2015 Authors
 */

#include "testing.h"
#include <2geom/choose.h>
#include <glib.h>

using namespace Geom;

TEST(ChooseTest, PascalsTriangle) {
    // check whether the values match Pascal's triangle
    for (unsigned i = 0; i < 500; ++i) {
        int n = g_random_int_range(3, 100);
        int k = g_random_int_range(1, n-1);

        double a = choose<double>(n, k);
        double b = choose<double>(n-1, k);
        double c = choose<double>(n-1, k-1);

        EXPECT_NEAR((b + c) / a, 1.0, 1e-14);
    }
}

TEST(ChooseTest, Values) {
    // test some well-known values
    EXPECT_EQ(choose<double>(0, 0), 1);
    EXPECT_EQ(choose<double>(1, 0), 1);
    EXPECT_EQ(choose<double>(1, 1), 1);
    EXPECT_EQ(choose<double>(127, 127), 1);
    EXPECT_EQ(choose<double>(92, 0), 1);
    EXPECT_EQ(choose<double>(2, 1), 2);

    // number of possible flops in Texas Hold 'Em Poker
    EXPECT_EQ(choose<double>(50,  3), 19600.);
    EXPECT_EQ(choose<double>(50, 47), 19600.);
    // number of possible hands in bridge
    EXPECT_EQ(choose<double>(52, 13), 635013559600.);
    EXPECT_EQ(choose<double>(52, 39), 635013559600.);
    // number of possible Lotto results
    EXPECT_EQ(choose<double>(49,  6), 13983816.);
    EXPECT_EQ(choose<double>(49, 43), 13983816.);
}

TEST(ChooseTest, Unsigned) {
    auto const BIG = std::numeric_limits<unsigned>::max() - 1;
    EXPECT_EQ(choose<unsigned>(BIG, BIG - 1), BIG);
    EXPECT_EQ(choose<unsigned>(BIG, BIG), 1);
    EXPECT_EQ(choose<unsigned>(BIG, BIG + 1), 0);
}
