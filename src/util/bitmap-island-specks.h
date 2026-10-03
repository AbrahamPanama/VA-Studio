// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UTIL_BITMAP_ISLAND_SPECKS_H
#define INKSCAPE_UTIL_BITMAP_ISLAND_SPECKS_H
#include "util/bitmap-island-enclosure.h"
namespace Inkscape::Bitmap {
// Orthogonal pixel axes, including rotation/flips. Skew must be baked first.
// pixelsPerMm preserves the simulator's evaluation order (300/25.4)*(dpi/300).
// The distance calculation uses x-pixel units, avoiding reciprocal round trips.
struct OrthogonalMetric {
public:
    static Result<OrthogonalMetric> fromDpi(double x, double y) noexcept;
    double pixelMmX() const noexcept { return 1 / _x; }
    double pixelMmY() const noexcept { return 1 / _y; }
    double reachSquared() const noexcept { return _x * _x; }
    double areaLimit() const noexcept { return 2 * _x * _y; }
    double yScale() const noexcept { return _x / _y; }
    bool valid() const noexcept;
private:
    double _x = 0, _y = 0;
};
// Frozen enclosure areas and nearest candidates; targets include other specks,
// as in the pinned engine. Equal distances use ORIGINAL foreground discovery id
// at the nearest cell, which may differ from an enclosure aggregate's minimum.
// Output borrows the same Regions runs as input. Regions/Budget must outlive it.
// All allocations count against the combined 256 MiB topology cap; all visits
// use the caller's shared meter. Failure returns no partial output or input edits.
// Checked preflight allowance; the shared visit/time caps remain authoritative.
Result<std::uint64_t> speckWorkReserve(Partition const &, OrthogonalMetric const &,
                                      JobWork &, Stop = {}) noexcept;
Result<Partition> attach(Partition const &, OrthogonalMetric const &, Budget &,
                         JobWork &, Stop = {}) noexcept;
} // namespace Inkscape::Bitmap
#endif
