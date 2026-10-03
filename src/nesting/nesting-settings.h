// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_NESTING_SETTINGS_H
#define INKSCAPE_NESTING_SETTINGS_H

#include <cstdint>

namespace Inkscape { class Preferences; }

namespace Inkscape::Nesting {

inline constexpr char PART_SPACING_PREF_PATH[] = "/tools/nesting/part_spacing";
inline constexpr char CONTAINER_MARGIN_PREF_PATH[] = "/tools/nesting/container_margin";
inline constexpr char TIME_PRESET_PREF_PATH[] = "/tools/nesting/optimization_time_preset";
inline constexpr char CUSTOM_TIME_PREF_PATH[] = "/tools/nesting/custom_time_seconds";
inline constexpr char TIME_LIMIT_PREF_PATH[] = "/tools/nesting/time_limit_ms";
inline constexpr char ROTATION_MODE_PREF_PATH[] = "/tools/nesting/rotation_mode";
inline constexpr char ROTATION_STEP_PREF_PATH[] = "/tools/nesting/rotation_step";
inline constexpr char SHOW_LABELS_PREF_PATH[] = "/tools/nesting/show_labels";

enum class OptimizationTimePreset : int
{
    Quick = 0,
    Balanced = 1,
    Refined = 2,
    Maximum = 3,
    Custom = 4,
    Unlimited = 5,
};

constexpr std::uint64_t MIN_CUSTOM_TIME_MS = 100;
constexpr std::uint64_t MAX_CUSTOM_TIME_MS = 600'000;
constexpr std::uint64_t DEFAULT_CUSTOM_TIME_MS = 30'000;
constexpr std::uint64_t DEFAULT_OPTIMIZATION_TIME_MS = 5'000;

[[nodiscard]] OptimizationTimePreset optimizationTimePresetFromInt(int value) noexcept;
[[nodiscard]] OptimizationTimePreset optimizationTimePresetForMilliseconds(std::uint64_t value) noexcept;
[[nodiscard]] std::uint64_t optimizationTimeMilliseconds(OptimizationTimePreset preset,
                                                         std::uint64_t custom_time_ms) noexcept;

/**
 * Read a persisted physical nesting distance in SVG user units (CSS px).
 *
 * Preferences written before the unit-aware controls existed are unitless and
 * retain their historical meaning as px. Invalid or negative values are
 * clamped to zero before they reach the Rust geometry boundary.
 */
[[nodiscard]] double readLengthPreferencePx(Preferences &preferences, char const *path);

/**
 * Recompute the time limit the solver reads (TIME_LIMIT_PREF_PATH) from the
 * preset and custom seconds, and return it. The Preferences page and the
 * controls bar both call this, so they cannot disagree.
 */
std::uint64_t syncOptimizationTimeLimit(Preferences &preferences);

/**
 * Settings written before the presets existed have only a time limit: infer
 * the preset (and custom seconds) from it once, so the controls bar and the
 * Preferences page show what the tool uses.
 */
void migrateOptimizationTimePreset(Preferences &preferences);

} // namespace Inkscape::Nesting

#endif // INKSCAPE_NESTING_SETTINGS_H
