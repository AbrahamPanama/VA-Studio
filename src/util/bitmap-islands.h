// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UTIL_BITMAP_ISLANDS_H
#define INKSCAPE_UTIL_BITMAP_ISLANDS_H
#include "util/bitmap-island-budget.h"
#include <utility>
namespace Inkscape::Bitmap {
using AlphaLut = std::array<std::uint8_t, 256>;
AlphaLut alphaLut(unsigned T, unsigned S) noexcept;
// Half-open coordinates. Runs cover BOTH classes in row-major order.
struct Run { std::uint32_t x, end, y, region; };
struct Island {
    std::uint32_t id, x, y, endX, endY;
    std::uint64_t area;
    bool foreground, exterior;
};
// Move-only storage. Budget must outlive this result. Returned pointers are read-only
// and valid until the result is moved or destroyed. No partial Regions on refusal.
// Foreground records precede background records; each class sorts by bbox(y,x,id).
// ids start at 1 independently in each class, in first-pixel discovery order.
// Run.region is a zero-based index into the sorted records, NOT a discovery id.
// Background uses four-connectivity. Border-touching components have exterior=true;
// enclosure must treat ALL such records and the virtual image border as exterior.
// Foreground uses eight-connectivity and only samples with adjusted alpha > 0.
// Region areas count pixels, not alpha mass. Coordinates remain on the input grid.
class Regions {
public:
    std::uint32_t width = 0, height = 0, runCount = 0, islandCount = 0, foregroundCount = 0;
    Run const *runs() const noexcept { return reinterpret_cast<Run const *>(_runs.data()); }
    Island const *islands() const noexcept { return reinterpret_cast<Island const *>(_islands.data()); }
private:
    PlainBuffer _runs, _islands;
    friend Result<Regions> label(RgbaView, AlphaLut const &, Budget &, JobWork &, Stop, struct LabelOptions) noexcept;
};
// Lower-only ceilings permit bounded fault/cap tests and more conservative hosts.
// islands limits foreground initial islands; background count is bounded by runs/bytes.
// Shared topology clients must serialize stage growth checks on a common Budget.
enum class LabelPhase { unionFind, sorting };
struct LabelOptions {
    std::uint32_t runs = 16000000, islands = 2000000;
    std::uint64_t topologyBytes = 256 * MiB;
    AllocationFault *fault = nullptr;
    // Optional allocation-free observer, called once inside each traversal phase.
    void (*observe)(LabelPhase, void *) noexcept = nullptr;
    void *observerData = nullptr;
};
Result<Regions> label(RgbaView, AlphaLut const &, Budget &, JobWork &, Stop = {}, LabelOptions = {}) noexcept;
} // namespace Inkscape::Bitmap
#endif
