// SPDX-License-Identifier: GPL-2.0-or-later

#include "nesting-settings.h"

#include <algorithm>
#include <cmath>

#include "preferences.h"

namespace Inkscape::Nesting {

OptimizationTimePreset optimizationTimePresetFromInt(int value) noexcept
{
    if (value < static_cast<int>(OptimizationTimePreset::Quick) ||
        value > static_cast<int>(OptimizationTimePreset::Unlimited)) {
        return OptimizationTimePreset::Balanced;
    }
    return static_cast<OptimizationTimePreset>(value);
}

OptimizationTimePreset optimizationTimePresetForMilliseconds(std::uint64_t value) noexcept
{
    switch (value) {
        case 1'000: return OptimizationTimePreset::Quick;
        case 5'000: return OptimizationTimePreset::Balanced;
        case 15'000: return OptimizationTimePreset::Refined;
        case 60'000: return OptimizationTimePreset::Maximum;
        case 0: return OptimizationTimePreset::Unlimited;
        default: return OptimizationTimePreset::Custom;
    }
}

std::uint64_t optimizationTimeMilliseconds(OptimizationTimePreset preset,
                                           std::uint64_t custom_time_ms) noexcept
{
    switch (preset) {
        case OptimizationTimePreset::Quick: return 1'000;
        case OptimizationTimePreset::Balanced: return 5'000;
        case OptimizationTimePreset::Refined: return 15'000;
        case OptimizationTimePreset::Maximum: return 60'000;
        case OptimizationTimePreset::Custom:
            return std::clamp(custom_time_ms, MIN_CUSTOM_TIME_MS, MAX_CUSTOM_TIME_MS);
        case OptimizationTimePreset::Unlimited: return 0;
    }
    return DEFAULT_OPTIMIZATION_TIME_MS;
}

double readLengthPreferencePx(Preferences &preferences, char const *path)
{
    auto const entry = preferences.getEntry(path);
    if (!entry.isSet()) {
        return 0.0;
    }

    // Unitless values predate the unit-aware preferences UI and were consumed
    // directly as SVG user units. Preserve that meaning during migration.
    auto const value = entry.getUnit().empty() ? entry.getDouble(0.0) : entry.getDouble(0.0, "px");
    return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

std::uint64_t syncOptimizationTimeLimit(Preferences &preferences)
{
    auto const preset = optimizationTimePresetFromInt(
        preferences.getInt(TIME_PRESET_PREF_PATH, static_cast<int>(OptimizationTimePreset::Balanced)));
    auto const custom_seconds = std::clamp(preferences.getDouble(CUSTOM_TIME_PREF_PATH, 30.0), 0.1, 600.0);
    auto const custom_ms = static_cast<std::uint64_t>(std::llround(custom_seconds * 1000.0));
    auto const limit = optimizationTimeMilliseconds(preset, custom_ms);
    preferences.setInt(TIME_LIMIT_PREF_PATH, static_cast<int>(limit));
    return limit;
}

void migrateOptimizationTimePreset(Preferences &preferences)
{
    auto const current_ms = static_cast<std::uint64_t>(std::max(
        0, preferences.getInt(TIME_LIMIT_PREF_PATH, static_cast<int>(DEFAULT_OPTIMIZATION_TIME_MS))));
    auto const inferred = optimizationTimePresetForMilliseconds(current_ms);
    if (!preferences.hasPref(TIME_PRESET_PREF_PATH)) {
        preferences.setInt(TIME_PRESET_PREF_PATH, static_cast<int>(inferred));
    }
    if (!preferences.hasPref(CUSTOM_TIME_PREF_PATH) && inferred == OptimizationTimePreset::Custom) {
        preferences.setDouble(CUSTOM_TIME_PREF_PATH, current_ms / 1000.0);
    }
}

} // namespace Inkscape::Nesting
