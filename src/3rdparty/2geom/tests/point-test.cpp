// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file
 * @brief Unit tests for Point, IntPoint and related functions.
 * Uses the Google Testing Framework
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 * 
 * Copyright 2014-2015 Authors
 */

#include <cassert>
#include <unordered_map>
#include <gtest/gtest.h>
#include <2geom/point.h>

namespace Geom {

TEST(PointTest, Normalize) {
    Point a(1e-18, 0);
    Point b = a;
    a.normalize();

    EXPECT_EQ(a, Point(1, 0));
    EXPECT_EQ(b.normalized(), a);
    EXPECT_NE(b, a);
}

TEST(PointTest, ScalarOps) {
    Point a(1,2);
    EXPECT_EQ(a * 2, Point(2, 4));
    EXPECT_EQ(2 * a, Point(2, 4));
    EXPECT_EQ(a / 2, Point(0.5, 1));

    Point b = a;
    a *= 2;
    a /= 2;
    EXPECT_EQ(a, b);
}

TEST(PointTest, Rounding) {
    Point a(-0.7, 0.7);
    IntPoint aceil(0, 1), afloor(-1, 0), around(-1, 1);
    EXPECT_TRUE(a.ceil() == aceil);
    EXPECT_TRUE(a.floor() == afloor);
    EXPECT_TRUE(a.round() == around);
}

TEST(PointTest, Near) {
    EXPECT_TRUE(are_near(Point(), Point(0, 1e-6)));
    EXPECT_FALSE(are_near(Point(), Point(0, 1e-4)));

    EXPECT_TRUE(are_near_rel(Point(100, 0), Point(100, 1e-4)));
    EXPECT_FALSE(are_near_rel(Point(100, 0), Point(100, 1e-2)));
}

TEST(PointTest, Multiplicative) {
    EXPECT_EQ(Point(2, 3) * Point(4, 5), Point(8, 15));
    EXPECT_EQ(IntPoint(2, 3) * IntPoint(4, 5), IntPoint(8, 15));
    EXPECT_EQ(Point(10, 11) / Point(2, 3), Point(5, 11.0 / 3.0));
    EXPECT_EQ(IntPoint(10, 11) / IntPoint(2, 3), IntPoint(5, 11 / 3));
}

TEST(PointTest, PointCtors) {
    Point a(2, 3);
    EXPECT_EQ(a[X], 2);
    EXPECT_EQ(a[Y], 3);

    a.~Point();
    new (&a) Point;
    EXPECT_EQ(a, Point(0, 0));

    a = Point(IntPoint(4, 5));
    EXPECT_EQ(a[X], 4);
    EXPECT_EQ(a[Y], 5);
}

TEST(PointTest, IntPointCtors) {
    IntPoint a(2, 3);
    EXPECT_EQ(a[X], 2);
    EXPECT_EQ(a[Y], 3);

    a.~IntPoint();
    new (&a) IntPoint;
    EXPECT_EQ(a, IntPoint(0, 0));
}

TEST(PointTest, IntPointScalarOps) {
    EXPECT_EQ(IntPoint(2, 4) * 0.25, Point(0.5, 1.0));
    EXPECT_EQ(0.25 * IntPoint(2, 4), Point(0.5, 1.0));
    EXPECT_EQ(IntPoint(2, 4) / 4.0, Point(0.5, 1.0));
}

template <typename PointType>
constexpr bool structured_binding_test()
{
    auto p = PointType(1, 2);

    // Check unpacking the coordinates works.
    {
        auto [x, y] = p;
        assert(p[X] == x);
        assert(p[Y] == y);
    }

    // Ensure point is writeable.
    {
        auto &[x, y] = p;
        assert(p[X] == x);
        assert(p[Y] == y);
        x = 3;
        y = 4;
        assert(p == PointType(3, 4));
    }

    return true;
}

TEST(IntervalTest, StructuredBindingTest)
{
    constexpr bool results[] = { structured_binding_test<Point>(),
                                 structured_binding_test<IntPoint>() };
}

TEST(PointTest, Hash)
{
    auto test = [] <typename PointType> {
        std::unordered_map<PointType, int> map;
        map[PointType(1, 1)] = 1;
        map[PointType(1, 2)] = 2;
        map[PointType(2, 1)] = 3;
        EXPECT_EQ(map[PointType(1, 1)], 1);
        EXPECT_EQ(map[PointType(1, 2)], 2);
        EXPECT_EQ(map[PointType(2, 1)], 3);
    };

    test.template operator()<Point>();
    test.template operator()<IntPoint>();
}

} // namespace Geom

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
