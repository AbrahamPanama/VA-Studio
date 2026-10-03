// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "display/bitmap-tone.h"

using Inkscape::Filters::BitmapToneSettings;
using Inkscape::Filters::BitmapTonePatch;
using Inkscape::Filters::BitmapToneProperty;
using Inkscape::Filters::apply_bitmap_tone_patch;
using Inkscape::Filters::build_bitmap_tone_table;
using Inkscape::Filters::set_bitmap_tone_patch_property;

namespace {

double at(Inkscape::Filters::BitmapToneTable const &table, double position)
{
    auto const index = static_cast<std::size_t>(
        std::round(position * static_cast<double>(table.size() - 1)));
    return table[std::min(index, table.size() - 1)];
}

void expect_valid(Inkscape::Filters::BitmapToneTable const &table)
{
    EXPECT_TRUE(std::all_of(table.begin(), table.end(), [](double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }));
    EXPECT_TRUE(std::is_sorted(table.begin(), table.end()));
}

} // namespace

TEST(BitmapToneTest, NeutralIsAnExactIdentity)
{
    auto const table = build_bitmap_tone_table({});
    for (std::size_t i = 0; i < table.size(); ++i) {
        EXPECT_DOUBLE_EQ(table[i], static_cast<double>(i) / (table.size() - 1));
    }
}

TEST(BitmapToneTest, ClampsParametersAndRejectsNonFiniteValues)
{
    BitmapToneSettings settings;
    settings.brightness = 1000.0;
    settings.contrast = -1000.0;
    settings.intensity = std::numeric_limits<double>::infinity();
    settings.highlights = std::numeric_limits<double>::quiet_NaN();

    auto const clamped = settings.clamped();
    EXPECT_DOUBLE_EQ(clamped.brightness, 100.0);
    EXPECT_DOUBLE_EQ(clamped.contrast, -100.0);
    EXPECT_DOUBLE_EQ(clamped.intensity, 0.0);
    EXPECT_DOUBLE_EQ(clamped.highlights, 0.0);
    expect_valid(build_bitmap_tone_table(settings));
}

TEST(BitmapToneTest, GlobalControlsHaveDistinctPredictableEffects)
{
    auto const neutral = build_bitmap_tone_table({});

    BitmapToneSettings brightness;
    brightness.brightness = 40.0;
    auto const bright = build_bitmap_tone_table(brightness);
    EXPECT_GT(at(bright, 0.5), at(neutral, 0.5));

    BitmapToneSettings intensity;
    intensity.intensity = 40.0;
    auto const intense = build_bitmap_tone_table(intensity);
    EXPECT_GT(at(intense, 0.25), at(neutral, 0.25));

    BitmapToneSettings contrast;
    contrast.contrast = 40.0;
    auto const contrasted = build_bitmap_tone_table(contrast);
    EXPECT_LT(at(contrasted, 0.25), at(neutral, 0.25));
    EXPECT_GT(at(contrasted, 0.75), at(neutral, 0.75));
}

TEST(BitmapToneTest, LocalControlsConcentrateTheirEffectInTheExpectedBand)
{
    BitmapToneSettings shadows;
    shadows.shadows = 50.0;
    auto const shadow_curve = build_bitmap_tone_table(shadows);
    EXPECT_GT(at(shadow_curve, 0.2) - 0.2, at(shadow_curve, 0.8) - 0.8);

    BitmapToneSettings highlights;
    highlights.highlights = 50.0;
    auto const highlight_curve = build_bitmap_tone_table(highlights);
    EXPECT_GT(at(highlight_curve, 0.8) - 0.8, at(highlight_curve, 0.2) - 0.2);

    BitmapToneSettings midtones;
    midtones.midtones = 50.0;
    auto const midtone_curve = build_bitmap_tone_table(midtones);
    EXPECT_GT(at(midtone_curve, 0.5) - 0.5, at(midtone_curve, 0.05) - 0.05);
}

TEST(BitmapToneTest, EveryControlRespondsPredictablyInBothDirections)
{
    auto const neutral = build_bitmap_tone_table({});

    auto expect_global_direction = [&](auto member, double sample) {
        BitmapToneSettings positive;
        positive.*member = 40.0;
        BitmapToneSettings negative;
        negative.*member = -40.0;
        EXPECT_GT(at(build_bitmap_tone_table(positive), sample), at(neutral, sample));
        EXPECT_LT(at(build_bitmap_tone_table(negative), sample), at(neutral, sample));
    };
    expect_global_direction(&BitmapToneSettings::brightness, 0.5);
    expect_global_direction(&BitmapToneSettings::intensity, 0.25);
    expect_global_direction(&BitmapToneSettings::shadows, 0.2);
    expect_global_direction(&BitmapToneSettings::midtones, 0.5);
    expect_global_direction(&BitmapToneSettings::highlights, 0.8);

    BitmapToneSettings positive_contrast;
    positive_contrast.contrast = 40.0;
    auto const positive_curve = build_bitmap_tone_table(positive_contrast);
    EXPECT_LT(at(positive_curve, 0.25), at(neutral, 0.25));
    EXPECT_GT(at(positive_curve, 0.75), at(neutral, 0.75));

    BitmapToneSettings negative_contrast;
    negative_contrast.contrast = -40.0;
    auto const negative_curve = build_bitmap_tone_table(negative_contrast);
    EXPECT_GT(at(negative_curve, 0.25), at(neutral, 0.25));
    EXPECT_LT(at(negative_curve, 0.75), at(neutral, 0.75));
}

TEST(BitmapToneTest, ExtremeCombinationsRemainMonotoneAndBounded)
{
    for (double sign : {-1.0, 1.0}) {
        BitmapToneSettings settings;
        settings.brightness = 100.0 * sign;
        settings.contrast = -100.0 * sign;
        settings.intensity = 100.0 * sign;
        settings.highlights = -100.0 * sign;
        settings.shadows = 100.0 * sign;
        settings.midtones = -100.0 * sign;
        expect_valid(build_bitmap_tone_table(settings));
    }
}

TEST(BitmapToneTest, EmptyPatchPreservesEveryProperty)
{
    BitmapToneSettings baseline{12, -8, 4, -20, 15, 6};
    EXPECT_TRUE(Inkscape::Filters::bitmap_tone_settings_equal(
        apply_bitmap_tone_patch(baseline, {}), baseline));
}

TEST(BitmapToneTest, SparsePatchChangesOnlyItsProperty)
{
    BitmapToneSettings baseline{12, -8, 4, -20, 15, 6};
    BitmapTonePatch patch;
    set_bitmap_tone_patch_property(patch, BitmapToneProperty::Brightness, 90);
    auto const result = apply_bitmap_tone_patch(baseline, patch);

    EXPECT_DOUBLE_EQ(result.brightness, 90);
    EXPECT_DOUBLE_EQ(result.contrast, baseline.contrast);
    EXPECT_DOUBLE_EQ(result.intensity, baseline.intensity);
    EXPECT_DOUBLE_EQ(result.highlights, baseline.highlights);
    EXPECT_DOUBLE_EQ(result.shadows, baseline.shadows);
    EXPECT_DOUBLE_EQ(result.midtones, baseline.midtones);
}

TEST(BitmapToneTest, SparsePatchClampsAndSanitizesInputs)
{
    BitmapToneSettings baseline{12, -8, 4, -20, 15, 6};
    BitmapTonePatch patch;
    patch.brightness = 1000;
    patch.contrast = std::numeric_limits<double>::quiet_NaN();
    auto const result = apply_bitmap_tone_patch(baseline, patch);

    EXPECT_DOUBLE_EQ(result.brightness, 100);
    EXPECT_DOUBLE_EQ(result.contrast, 0);
    EXPECT_DOUBLE_EQ(result.intensity, baseline.intensity);
}

TEST(BitmapToneTest, FullPatchExplicitlyResetsAllProperties)
{
    BitmapToneSettings baseline{12, -8, 4, -20, 15, 6};
    auto const reset = apply_bitmap_tone_patch(baseline, BitmapTonePatch::all({}));
    EXPECT_TRUE(reset.is_neutral());
}
