// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <locale>
#include <numbers>

#include "selection-rotation-feedback.h"

using namespace Inkscape;

namespace {
constexpr double radians(double degrees) { return degrees * std::numbers::pi / 180.0; }

class CommaDecimal final : public std::numpunct<char>
{
    char do_decimal_point() const override { return ','; }
    std::string do_grouping() const override { return {}; }
};
} // namespace

TEST(SelectionRotationFeedbackTest, AccumulatesBothDirectionsAcrossWrapAndMultipleTurns)
{
    for (double direction : {-1.0, 1.0}) {
        ContinuousRotationAngle angle;
        for (unsigned i = 0; i <= 1080; ++i) {
            double const expected = i * direction;
            auto actual = angle.update(std::remainder(radians(expected), 2 * std::numbers::pi), 100);
            ASSERT_TRUE(actual);
            EXPECT_NEAR(*actual, expected, 1e-9);
        }
        for (int i = 1079; i >= 0; --i) {
            auto actual = angle.update(radians(i * direction), 100);
            ASSERT_TRUE(actual);
            EXPECT_NEAR(*actual, i * direction, 1e-9);
        }
    }
}

TEST(SelectionRotationFeedbackTest, AcceptedGeometryUsesRawRevolutionNotSnappedHistory)
{
    EXPECT_NEAR(equivalent_rotation_degrees(radians(-179), 179), 181, 1e-10);
    EXPECT_NEAR(equivalent_rotation_degrees(radians(179), -179), -181, 1e-10);
    EXPECT_NEAR(equivalent_rotation_degrees(radians(15), 374), 375, 1e-10);
    EXPECT_NEAR(equivalent_rotation_degrees(radians(-15), -374), -375, 1e-10);
    EXPECT_NEAR(equivalent_rotation_degrees(radians(0), 719), 720, 1e-10);
    // A geometric snap at 17 degrees must be reported as accepted, not as
    // the nearest soft-detent angle of 15 degrees.
    EXPECT_NEAR(equivalent_rotation_degrees(radians(17), 734), 737, 1e-10);
}

TEST(SelectionRotationFeedbackTest, PivotNoiseDoesNotInventRevolutions)
{
    ContinuousRotationAngle angle;
    ASSERT_TRUE(angle.update(radians(170), 100));
    EXPECT_FALSE(angle.update(radians(-30), 1));
    EXPECT_FALSE(angle.update(radians(150), 2));
    EXPECT_FALSE(angle.update(radians(-150), 0));
    EXPECT_DOUBLE_EQ(angle.degrees(), 170);
    auto resumed = angle.update(radians(-170), 100);
    ASSERT_TRUE(resumed);
    EXPECT_NEAR(*resumed, 190, 1e-10);
    angle.reset();
    EXPECT_DOUBLE_EQ(angle.degrees(), 0);
    EXPECT_NEAR(*angle.update(radians(-15), 100), -15, 1e-10);
}

TEST(SelectionRotationFeedbackTest, InvalidPointerSamplesAreRejectedWithoutChangingLastAngle)
{
    ContinuousRotationAngle angle;
    ASSERT_TRUE(angle.update(radians(15), 100));
    auto nan = std::numeric_limits<double>::quiet_NaN();
    auto inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(angle.update(nan, 100));
    EXPECT_FALSE(angle.update(inf, 100));
    EXPECT_FALSE(angle.update(0, nan));
    EXPECT_FALSE(angle.update(0, inf));
    EXPECT_FALSE(angle.update(0, -1));
    EXPECT_NEAR(angle.degrees(), 15, 1e-10);
    EXPECT_TRUE(std::isnan(equivalent_rotation_degrees(nan, 15)));
    EXPECT_TRUE(std::isnan(equivalent_rotation_degrees(0, inf)));
}

TEST(SelectionRotationFeedbackTest, CapturesFourteenFifteenSixteenInBothDirections)
{
    for (double direction : {-1.0, 1.0}) {
        for (double input : {14.0, 15.0, 16.0}) {
            RotationDetentLatch latch;
            auto result = evaluate_rotation_detent(direction * input, 100, true, false, latch);
            EXPECT_TRUE(result.engaged);
            EXPECT_TRUE(latch.engaged());
            EXPECT_DOUBLE_EQ(result.degrees, direction * 15);
        }
    }
}

TEST(SelectionRotationFeedbackTest, HysteresisRetainsUntilReleaseWithoutOscillation)
{
    for (double direction : {-1.0, 1.0}) {
        RotationDetentLatch latch;
        ASSERT_TRUE(evaluate_rotation_detent(direction * 14, 100, true, false, latch).engaged);
        for (double input : {16.0, 17.0, 18.0, 17.9, 19.0, 18.9}) {
            auto result = evaluate_rotation_detent(direction * input, 100, true, false, latch);
            EXPECT_TRUE(result.engaged);
            EXPECT_DOUBLE_EQ(result.degrees, direction * 15);
        }
        auto released = evaluate_rotation_detent(direction * 20, 100, true, false, latch);
        EXPECT_FALSE(released.engaged);
        EXPECT_FALSE(latch.engaged());
        EXPECT_DOUBLE_EQ(released.degrees, direction * 20);
        // In the hysteresis gap, an unlatched gesture must stay free.
        EXPECT_FALSE(evaluate_rotation_detent(direction * 18, 100, true, false, latch).engaged);
        EXPECT_TRUE(evaluate_rotation_detent(direction * 16, 100, true, false, latch).engaged);
    }
}

TEST(SelectionRotationFeedbackTest, CadenceExtendsThroughZeroAndManyRevolutions)
{
    for (double center : {-720.0, -375.0, -360.0, -270.0, -180.0, -90.0, -45.0, -30.0,
                           0.0, 30.0, 45.0, 90.0, 180.0, 270.0, 360.0, 375.0, 720.0}) {
        for (double delta : {-1.0, 0.0, 1.0}) {
            RotationDetentLatch latch;
            auto result = evaluate_rotation_detent(center + delta, 100, true, false, latch);
            EXPECT_TRUE(result.engaged);
            EXPECT_DOUBLE_EQ(result.degrees, center);
        }
    }
}

TEST(SelectionRotationFeedbackTest, CaptureAndReleaseUsePointerScreenRadius)
{
    for (double radius : {100.0, 200.0, 1000.0}) {
        RotationDetentLatch latch;
        auto degrees_for_arc = [radius](double pixels) { return pixels / radius * 180 / std::numbers::pi; };
        EXPECT_FALSE(evaluate_rotation_detent(15 + degrees_for_arc(4.1), radius, true, false, latch).engaged);
        EXPECT_TRUE(evaluate_rotation_detent(15 + degrees_for_arc(3.9), radius, true, false, latch).engaged);
        EXPECT_TRUE(evaluate_rotation_detent(15 + degrees_for_arc(7.9), radius, true, false, latch).engaged);
        EXPECT_FALSE(evaluate_rotation_detent(15 + degrees_for_arc(8.1), radius, true, false, latch).engaged);
    }
}

TEST(SelectionRotationFeedbackTest, NearPivotSuppressesCaptureAndAngularCapsAvoidHardGrid)
{
    RotationDetentLatch latch;
    ASSERT_TRUE(evaluate_rotation_detent(15, 100, true, false, latch).engaged);
    EXPECT_FALSE(evaluate_rotation_detent(15, 23.9, true, false, latch).engaged);
    EXPECT_FALSE(latch.engaged());
    EXPECT_FALSE(evaluate_rotation_detent(11.9, 24, true, false, latch).engaged);
    EXPECT_TRUE(evaluate_rotation_detent(12, 24, true, false, latch).engaged);
    EXPECT_TRUE(evaluate_rotation_detent(9, 24, true, false, latch).engaged);
    EXPECT_FALSE(evaluate_rotation_detent(8.9, 24, true, false, latch).engaged);
}

TEST(SelectionRotationFeedbackTest, DisabledBypassedOrHigherPriorityClearsLatch)
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        RotationDetentLatch latch;
        ASSERT_TRUE(evaluate_rotation_detent(14, 100, true, false, latch).engaged);
        auto result = evaluate_rotation_detent(17, mode == 2 ? 0 : mode == 3
            ? std::numeric_limits<double>::infinity() : 100, mode != 0, mode == 1, latch);
        EXPECT_FALSE(result.engaged);
        EXPECT_FALSE(latch.engaged());
        EXPECT_DOUBLE_EQ(result.degrees, 17);
    }
}

TEST(SelectionRotationFeedbackTest, FormattingPreservesSignTurnsLocaleAndReadableZero)
{
    auto locale = std::locale::classic();
    EXPECT_EQ(format_selection_rotation(15, locale), "+15.00\u00b0");
    EXPECT_EQ(format_selection_rotation(-15, locale), "-15.00\u00b0");
    EXPECT_EQ(format_selection_rotation(735, locale), "+735.00\u00b0");
    EXPECT_EQ(format_selection_rotation(-0.0001, locale), "0.00\u00b0");
    EXPECT_EQ(format_selection_rotation(0, locale), "0.00\u00b0");
    EXPECT_EQ(format_selection_rotation(std::numeric_limits<double>::infinity(), locale), "\u2014\u00b0");
    EXPECT_EQ(format_selection_rotation(15.25, std::locale(locale, new CommaDecimal)), "+15,25\u00b0");
}
