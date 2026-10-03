// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_SELECTION_DIMENSIONAL_DETENTS_H
#define INKSCAPE_SELECTION_DIMENSIONAL_DETENTS_H

#include <array>
#include <cstddef>
#include <optional>

namespace Inkscape {

namespace Util {
class Unit;
}

constexpr double DIMENSIONAL_DETENT_REGULAR_CAPTURE_PX = 4.0;
constexpr double DIMENSIONAL_DETENT_STRONG_CAPTURE_PX = 6.0;
constexpr double DIMENSIONAL_DETENT_REGULAR_RELEASE_PX = 8.0;
constexpr double DIMENSIONAL_DETENT_STRONG_RELEASE_PX = 10.0;
constexpr double DIMENSIONAL_DETENT_SINGULAR_EPSILON = 1e-6;

struct DimensionalCadence {
    double regular = 0.0;
    double strong = 0.0;
};

struct DimensionalCandidate {
    double display_value = 0.0;
    double document_value = 0.0;
    bool strong = false;
};

struct DimensionalCandidates {
    std::array<DimensionalCandidate, 2> values;
    std::size_t size = 0;
};

struct DimensionalDetentLatch {
    std::optional<DimensionalCandidate> candidate;

    bool engaged() const noexcept { return candidate.has_value(); }
    void reset() noexcept { candidate.reset(); }
};

struct DimensionalDetentResult {
    double dimension = 0.0;
    double screen_distance = 0.0;
    bool engaged = false;
    bool strong = false;
};

/** Return the SEL-003 cadence for a supported absolute display unit. */
std::optional<DimensionalCadence> dimensional_cadence(Util::Unit const &unit) noexcept;

/** Return at most the nearest lower and upper non-singular candidates. */
DimensionalCandidates nearest_dimensional_candidates(double dimension,
                                                      Util::Unit const &unit) noexcept;

/**
 * Apply capture/release hysteresis to one active dimension.
 *
 * @param dimension Current free dimension in document px (strictly positive).
 * @param screen_pixels_per_document_px Screen displacement caused by one document px of
 *                                      change in this dimension.
 * @param enabled False when global/preference/modifier state bypasses dimensional snapping.
 * @param higher_priority_snap True when existing geometric snapping resolved this axis.
 */
DimensionalDetentResult evaluate_dimensional_detent(
    double dimension, Util::Unit const &unit, double screen_pixels_per_document_px,
    bool enabled, bool higher_priority_snap, DimensionalDetentLatch &latch) noexcept;

} // namespace Inkscape

#endif // INKSCAPE_SELECTION_DIMENSIONAL_DETENTS_H
