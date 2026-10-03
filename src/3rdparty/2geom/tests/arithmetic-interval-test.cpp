// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * @file
 * @brief  Unit tests for the ArithmeticInterval - Wrapper for boost::numeric::interval
 * Uses the Google Testing Framework
 *//*
 * Copyright 2025 Muhammad Rafay Irfan <rafay119muhammad@gmail.com>
 */

#include <gtest/gtest.h>
#include <2geom/arithmetic-interval-utils.h>
#include <2geom/arithmetic-interval.h>
#include <cmath>

namespace Geom {

TEST(ArithmeticIntervalTest, DefaultConstructor) {
    ArithmeticInterval interval;
    EXPECT_EQ(interval.min(), 0.0);
    EXPECT_EQ(interval.max(), 0.0);
    EXPECT_TRUE(interval.isSingular());
}

TEST(ArithmeticIntervalTest, SingleValueConstructor) {
    ArithmeticInterval interval(5.0);
    EXPECT_EQ(interval.min(), 5.0);
    EXPECT_EQ(interval.max(), 5.0);
    EXPECT_TRUE(interval.isSingular());
}

TEST(ArithmeticIntervalTest, RangeConstructor) {
    ArithmeticInterval interval(1.0, 5.0);
    EXPECT_EQ(interval.min(), 1.0);
    EXPECT_EQ(interval.max(), 5.0);
    EXPECT_EQ(interval.extent(), 4.0);
    EXPECT_EQ(interval.middle(), 3.0);
}

TEST(ArithmeticIntervalTest, ContainsValue) {
    ArithmeticInterval interval(1.0, 5.0);
    EXPECT_TRUE(interval.contains(3.0));
    EXPECT_FALSE(interval.contains(0.0));
    EXPECT_FALSE(interval.contains(6.0));
}

TEST(ArithmeticIntervalTest, ContainsInterval) {
    ArithmeticInterval a(1.0, 5.0);
    ArithmeticInterval b(2.0, 4.0);
    ArithmeticInterval c(0.0, 6.0);
    EXPECT_TRUE(a.contains(b));
    EXPECT_FALSE(a.contains(c));
}

TEST(ArithmeticIntervalTest, Intersects) {
    ArithmeticInterval a(1.0, 5.0);
    ArithmeticInterval b(4.0, 6.0);
    ArithmeticInterval c(6.0, 8.0);
    EXPECT_TRUE(a.intersects(b));
    EXPECT_FALSE(a.intersects(c));
}

TEST(ArithmeticIntervalTest, Addition) {
    ArithmeticInterval a(1.0, 2.0);
    ArithmeticInterval b(3.0, 4.0);
    ArithmeticInterval result = a + b;
    EXPECT_EQ(result.min(), 4.0);
    EXPECT_EQ(result.max(), 6.0);

    result = 1.0 + b;
    EXPECT_EQ(result.min(), 4.0);
    EXPECT_EQ(result.max(), 5.0);
}

TEST(ArithmeticIntervalTest, Subtraction) {
    ArithmeticInterval a(1.0, 2.0);
    ArithmeticInterval b(3.0, 4.0);
    ArithmeticInterval result = a - b;
    EXPECT_EQ(result.min(), -3.0);
    EXPECT_EQ(result.max(), -1.0);

    result = 1.0 - b;
    EXPECT_EQ(result.min(), -3.0);
    EXPECT_EQ(result.max(), -2.0);
}

TEST(ArithmeticIntervalTest, ScalarMultiplication) {
    ArithmeticInterval a(1.0, 2.0);
    ArithmeticInterval result = a * 2.0;
    EXPECT_EQ(result.min(), 2.0);
    EXPECT_EQ(result.max(), 4.0);
}

TEST(ArithmeticIntervalTest, ScalarDivision) {
    ArithmeticInterval a(2.0, 4.0);
    ArithmeticInterval result = a / 2.0;
    EXPECT_EQ(result.min(), 1.0);
    EXPECT_EQ(result.max(), 2.0);
}

TEST(ArithmeticIntervalTest, ComparisonOperators) {
    ArithmeticInterval a(5.0);
    ArithmeticInterval b(5.0);
    ArithmeticInterval c(5.0, 5.0);
    ArithmeticInterval d(1.0, 2.0);
    ArithmeticInterval e(1.0, 2.0);

    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
    EXPECT_EQ(d, e);

    EXPECT_NE(a, d);

    EXPECT_LT(e, a);
    EXPECT_GT(a, e);

    EXPECT_LE(a, b);
    EXPECT_GE(a, b);
    EXPECT_GE(a, d);
    EXPECT_LE(d, a);
}

TEST(ArithmeticIntervalTest, AssignmentOperators) {
    ArithmeticInterval a(1.0, 2.0);
    ArithmeticInterval b(3.0, 4.0);

    a += b;
    EXPECT_EQ(a.min(), 4.0);
    EXPECT_EQ(a.max(), 6.0);

    a = ArithmeticInterval(1.0, 2.0);
    a -= b;
    EXPECT_EQ(a.min(), -3.0);
    EXPECT_EQ(a.max(), -1.0);

    a = ArithmeticInterval(1.0, 2.0);
    a *= 2.0;
    EXPECT_EQ(a.min(), 2.0);
    EXPECT_EQ(a.max(), 4.0);

    a = ArithmeticInterval(1.0, 2.0);
    a /= 2.0;
    EXPECT_EQ(a.min(), 0.5);
    EXPECT_EQ(a.max(), 1.0);
}

TEST(ArithmeticIntervalTest, Intersection) {
    ArithmeticInterval a(1.0, 5.0);
    ArithmeticInterval b(3.0, 7.0);
    ArithmeticInterval result = ArithmeticInterval::intersection(a, b);
    EXPECT_EQ(result.min(), 3.0);
    EXPECT_EQ(result.max(), 5.0);
}

} // namespace Geom
