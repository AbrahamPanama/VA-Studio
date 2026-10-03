// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_DISPLAY_BITMAP_TONE_H
#define INKSCAPE_DISPLAY_BITMAP_TONE_H

#include <array>
#include <cstddef>
#include <optional>

namespace Inkscape::Filters {

inline constexpr std::size_t BITMAP_TONE_TABLE_SIZE = 256;

/** Editable parameters for the non-destructive bitmap tone adjustment. */
struct BitmapToneSettings {
    double brightness = 0.0;
    double contrast = 0.0;
    double intensity = 0.0;
    double highlights = 0.0;
    double shadows = 0.0;
    double midtones = 0.0;

    [[nodiscard]] BitmapToneSettings clamped() const;
    [[nodiscard]] bool is_neutral(double tolerance = 1e-9) const;
};

enum class BitmapToneProperty : std::size_t {
    Brightness,
    Contrast,
    Intensity,
    Highlights,
    Shadows,
    Midtones,
    Count
};

inline constexpr std::size_t BITMAP_TONE_PROPERTY_COUNT =
    static_cast<std::size_t>(BitmapToneProperty::Count);

/** A sparse edit. Unset members preserve each target's canonical value. */
struct BitmapTonePatch {
    std::optional<double> brightness;
    std::optional<double> contrast;
    std::optional<double> intensity;
    std::optional<double> highlights;
    std::optional<double> shadows;
    std::optional<double> midtones;

    [[nodiscard]] bool empty() const;
    [[nodiscard]] static BitmapTonePatch all(BitmapToneSettings const &settings);
};

struct BitmapToneAggregateValue {
    double value = 0.0;
    bool mixed = false;
};

[[nodiscard]] double get_bitmap_tone_property(BitmapToneSettings const &settings,
                                              BitmapToneProperty property);
void set_bitmap_tone_property(BitmapToneSettings &settings, BitmapToneProperty property,
                              double value);
void set_bitmap_tone_patch_property(BitmapTonePatch &patch, BitmapToneProperty property,
                                    double value);
[[nodiscard]] BitmapToneSettings apply_bitmap_tone_patch(BitmapToneSettings const &baseline,
                                                         BitmapTonePatch const &patch);
[[nodiscard]] bool bitmap_tone_settings_equal(BitmapToneSettings const &a,
                                              BitmapToneSettings const &b,
                                              double tolerance = 1e-9);

using BitmapToneTable = std::array<double, BITMAP_TONE_TABLE_SIZE>;

/**
 * Build the standards-compatible transfer table used by both canvas preview
 * and the persisted SVG feComponentTransfer primitive.
 *
 * The curve is deliberately monotone: extreme combinations can create flat
 * clipped regions, but never reverse tonal order or produce solarisation.
 */
[[nodiscard]] BitmapToneTable build_bitmap_tone_table(BitmapToneSettings const &settings);

} // namespace Inkscape::Filters

#endif // INKSCAPE_DISPLAY_BITMAP_TONE_H
