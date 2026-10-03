// SPDX-License-Identifier: GPL-2.0-or-later

#include "selection-dimensional-detents.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/units.h"

namespace Inkscape {
namespace {

constexpr double ROUNDING_ULPS = 64.0;

bool near_integer(double value) noexcept
{
    if (!std::isfinite(value)) {
        return false;
    }
    auto const rounded = std::round(value);
    auto const tolerance = ROUNDING_ULPS * std::numeric_limits<double>::epsilon() *
                           std::max(1.0, std::abs(value));
    return std::abs(value - rounded) <= tolerance;
}

bool same_candidate(double lhs, double rhs) noexcept
{
    auto const tolerance = ROUNDING_ULPS * std::numeric_limits<double>::epsilon() *
                           std::max({1.0, std::abs(lhs), std::abs(rhs)});
    return std::abs(lhs - rhs) <= tolerance;
}

bool append_candidate(DimensionalCandidates &out, double display_value,
                      DimensionalCadence const &cadence, Util::Unit const &unit) noexcept
{
    if (!std::isfinite(display_value) || display_value <= 0.0 || out.size >= out.values.size()) {
        return false;
    }

    auto const document_value = Util::Quantity::convert(display_value, &unit, "px");
    if (!std::isfinite(document_value) || document_value <= DIMENSIONAL_DETENT_SINGULAR_EPSILON) {
        return false;
    }

    if (out.size && same_candidate(out.values[out.size - 1].document_value, document_value)) {
        return false;
    }

    out.values[out.size++] = {
        display_value,
        document_value,
        near_integer(display_value / cadence.strong)
    };
    return true;
}

double capture_radius(DimensionalCandidate const &candidate) noexcept
{
    return candidate.strong ? DIMENSIONAL_DETENT_STRONG_CAPTURE_PX
                            : DIMENSIONAL_DETENT_REGULAR_CAPTURE_PX;
}

double release_radius(DimensionalCandidate const &candidate) noexcept
{
    return candidate.strong ? DIMENSIONAL_DETENT_STRONG_RELEASE_PX
                            : DIMENSIONAL_DETENT_REGULAR_RELEASE_PX;
}

} // namespace

std::optional<DimensionalCadence> dimensional_cadence(Util::Unit const &unit) noexcept
{
    auto const &abbr = unit.abbr;
    if (abbr == "in") return DimensionalCadence{0.5, 1.0};
    if (abbr == "ft") return DimensionalCadence{1.0 / 24.0, 1.0};
    if (abbr == "mm") return DimensionalCadence{1.0, 10.0};
    if (abbr == "cm") return DimensionalCadence{0.1, 1.0};
    if (abbr == "m") return DimensionalCadence{0.001, 0.01};
    if (abbr == "px") return DimensionalCadence{1.0, 10.0};
    if (abbr == "pt") return DimensionalCadence{1.0, 12.0};
    if (abbr == "pc") return DimensionalCadence{1.0 / 12.0, 1.0};
    return std::nullopt;
}

DimensionalCandidates nearest_dimensional_candidates(double dimension,
                                                      Util::Unit const &unit) noexcept
{
    DimensionalCandidates result;
    auto const cadence = dimensional_cadence(unit);
    if (!cadence || !std::isfinite(dimension) ||
        dimension <= DIMENSIONAL_DETENT_SINGULAR_EPSILON) {
        return result;
    }

    auto const display_value = Util::Quantity::convert(dimension, "px", &unit);
    if (!std::isfinite(display_value) || display_value <= 0.0) {
        return result;
    }

    auto quotient = display_value / cadence->regular;
    if (!std::isfinite(quotient)) {
        return result;
    }

    if (near_integer(quotient)) {
        quotient = std::round(quotient);
    }
    auto const lower_multiple = std::floor(quotient);
    if (!std::isfinite(lower_multiple)) {
        return result;
    }

    auto const lower = lower_multiple * cadence->regular;
    append_candidate(result, lower, *cadence, unit);

    // At an exact boundary there is only one nearest candidate. Otherwise include the upper
    // neighbor; candidate work therefore remains constant even at extreme zoom.
    if (!same_candidate(display_value, lower)) {
        auto const upper = (lower_multiple + 1.0) * cadence->regular;
        append_candidate(result, upper, *cadence, unit);
    }

    return result;
}

DimensionalDetentResult evaluate_dimensional_detent(
    double dimension, Util::Unit const &unit, double screen_pixels_per_document_px,
    bool enabled, bool higher_priority_snap, DimensionalDetentLatch &latch) noexcept
{
    DimensionalDetentResult result{dimension, 0.0, false, false};
    if (!enabled || higher_priority_snap || !std::isfinite(dimension) ||
        dimension <= DIMENSIONAL_DETENT_SINGULAR_EPSILON ||
        !std::isfinite(screen_pixels_per_document_px) ||
        screen_pixels_per_document_px <= 0.0 || !dimensional_cadence(unit)) {
        latch.reset();
        return result;
    }

    if (latch.candidate) {
        auto const distance = std::abs(dimension - latch.candidate->document_value) *
                              screen_pixels_per_document_px;
        if (std::isfinite(distance) && distance <= release_radius(*latch.candidate)) {
            result.dimension = latch.candidate->document_value;
            result.screen_distance = distance;
            result.engaged = true;
            result.strong = latch.candidate->strong;
            return result;
        }
        latch.reset();
    }

    auto const candidates = nearest_dimensional_candidates(dimension, unit);
    std::optional<DimensionalCandidate> best;
    double best_distance = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < candidates.size; ++i) {
        auto const &candidate = candidates.values[i];
        auto const distance = std::abs(dimension - candidate.document_value) *
                              screen_pixels_per_document_px;
        if (!std::isfinite(distance) || distance > capture_radius(candidate)) {
            continue;
        }
        if (!best || distance < best_distance ||
            (same_candidate(distance, best_distance) && candidate.strong && !best->strong)) {
            best = candidate;
            best_distance = distance;
        }
    }

    if (!best) {
        return result;
    }

    latch.candidate = best;
    result.dimension = best->document_value;
    result.screen_distance = best_distance;
    result.engaged = true;
    result.strong = best->strong;
    return result;
}

} // namespace Inkscape
