// SPDX-License-Identifier: GPL-2.0-or-later

#include "bitmap-tone.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Inkscape::Filters {
namespace {

constexpr double lower_limit = -100.0;
constexpr double upper_limit = 100.0;

double clamp_parameter(double value)
{
    return std::clamp(std::isfinite(value) ? value : 0.0, lower_limit, upper_limit);
}

double smooth_bump(double x, double center, double radius)
{
    auto const distance = std::abs(x - center) / radius;
    if (distance >= 1.0) return 0.0;
    auto const t = 1.0 - distance;
    return t * t * (3.0 - 2.0 * t);
}

} // namespace

BitmapToneSettings BitmapToneSettings::clamped() const
{
    return {
        clamp_parameter(brightness),
        clamp_parameter(contrast),
        clamp_parameter(intensity),
        clamp_parameter(highlights),
        clamp_parameter(shadows),
        clamp_parameter(midtones)
    };
}

bool BitmapToneSettings::is_neutral(double tolerance) const
{
    auto const normalized = clamped();
    return std::abs(normalized.brightness) <= tolerance &&
           std::abs(normalized.contrast) <= tolerance &&
           std::abs(normalized.intensity) <= tolerance &&
           std::abs(normalized.highlights) <= tolerance &&
           std::abs(normalized.shadows) <= tolerance &&
           std::abs(normalized.midtones) <= tolerance;
}

bool BitmapTonePatch::empty() const
{
    return !brightness && !contrast && !intensity && !highlights && !shadows && !midtones;
}

BitmapTonePatch BitmapTonePatch::all(BitmapToneSettings const &raw)
{
    auto const settings = raw.clamped();
    return {settings.brightness, settings.contrast, settings.intensity,
            settings.highlights, settings.shadows, settings.midtones};
}

double get_bitmap_tone_property(BitmapToneSettings const &settings,
                                BitmapToneProperty property)
{
    switch (property) {
        case BitmapToneProperty::Brightness: return settings.brightness;
        case BitmapToneProperty::Contrast: return settings.contrast;
        case BitmapToneProperty::Intensity: return settings.intensity;
        case BitmapToneProperty::Highlights: return settings.highlights;
        case BitmapToneProperty::Shadows: return settings.shadows;
        case BitmapToneProperty::Midtones: return settings.midtones;
        case BitmapToneProperty::Count: break;
    }
    throw std::logic_error("invalid bitmap tone property");
}

void set_bitmap_tone_property(BitmapToneSettings &settings, BitmapToneProperty property,
                              double value)
{
    switch (property) {
        case BitmapToneProperty::Brightness: settings.brightness = value; return;
        case BitmapToneProperty::Contrast: settings.contrast = value; return;
        case BitmapToneProperty::Intensity: settings.intensity = value; return;
        case BitmapToneProperty::Highlights: settings.highlights = value; return;
        case BitmapToneProperty::Shadows: settings.shadows = value; return;
        case BitmapToneProperty::Midtones: settings.midtones = value; return;
        case BitmapToneProperty::Count: break;
    }
    throw std::logic_error("invalid bitmap tone property");
}

void set_bitmap_tone_patch_property(BitmapTonePatch &patch, BitmapToneProperty property,
                                    double value)
{
    switch (property) {
        case BitmapToneProperty::Brightness: patch.brightness = value; return;
        case BitmapToneProperty::Contrast: patch.contrast = value; return;
        case BitmapToneProperty::Intensity: patch.intensity = value; return;
        case BitmapToneProperty::Highlights: patch.highlights = value; return;
        case BitmapToneProperty::Shadows: patch.shadows = value; return;
        case BitmapToneProperty::Midtones: patch.midtones = value; return;
        case BitmapToneProperty::Count: break;
    }
    throw std::logic_error("invalid bitmap tone property");
}

BitmapToneSettings apply_bitmap_tone_patch(BitmapToneSettings const &baseline,
                                           BitmapTonePatch const &patch)
{
    auto result = baseline.clamped();
    if (patch.brightness) result.brightness = *patch.brightness;
    if (patch.contrast) result.contrast = *patch.contrast;
    if (patch.intensity) result.intensity = *patch.intensity;
    if (patch.highlights) result.highlights = *patch.highlights;
    if (patch.shadows) result.shadows = *patch.shadows;
    if (patch.midtones) result.midtones = *patch.midtones;
    return result.clamped();
}

bool bitmap_tone_settings_equal(BitmapToneSettings const &a, BitmapToneSettings const &b,
                                double tolerance)
{
    auto const ca = a.clamped();
    auto const cb = b.clamped();
    auto close = [tolerance](double left, double right) {
        return std::abs(left - right) <=
               tolerance * std::max({1.0, std::abs(left), std::abs(right)});
    };
    return close(ca.brightness, cb.brightness) && close(ca.contrast, cb.contrast) &&
           close(ca.intensity, cb.intensity) && close(ca.highlights, cb.highlights) &&
           close(ca.shadows, cb.shadows) && close(ca.midtones, cb.midtones);
}

BitmapToneTable build_bitmap_tone_table(BitmapToneSettings const &raw)
{
    auto const settings = raw.clamped();
    BitmapToneTable result{};

    if (settings.is_neutral()) {
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = static_cast<double>(i) / static_cast<double>(result.size() - 1);
        }
        return result;
    }

    // Intensity is exposure-like gain (up to +/-2 stops). Brightness is a
    // perceptual offset, while contrast changes slope around middle grey.
    auto const gain = std::exp2(settings.intensity / 50.0);
    auto const brightness = settings.brightness / 100.0 * 0.25;
    auto const contrast = std::exp2(settings.contrast / 50.0);
    auto const shadow_amount = settings.shadows / 100.0 * 0.22;
    auto const midtone_amount = settings.midtones / 100.0 * 0.20;
    auto const highlight_amount = settings.highlights / 100.0 * 0.22;

    for (std::size_t i = 0; i < result.size(); ++i) {
        auto const x = static_cast<double>(i) / static_cast<double>(result.size() - 1);
        auto y = x * gain;
        y += brightness;
        y = 0.5 + (y - 0.5) * contrast;

        y += shadow_amount * smooth_bump(x, 0.20, 0.34);
        y += midtone_amount * smooth_bump(x, 0.50, 0.38);
        y += highlight_amount * smooth_bump(x, 0.80, 0.34);
        result[i] = std::clamp(y, 0.0, 1.0);
    }

    // Enforce monotonicity after combining local controls. This intentionally
    // turns impossible/extreme combinations into clipping instead of inversions.
    for (std::size_t i = 1; i < result.size(); ++i) {
        result[i] = std::max(result[i], result[i - 1]);
    }

    return result;
}

} // namespace Inkscape::Filters
