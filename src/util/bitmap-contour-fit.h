// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UTIL_BITMAP_CONTOUR_FIT_H
#define INKSCAPE_UTIL_BITMAP_CONTOUR_FIT_H
#include "util/bitmap-contour.h"

// Explode Bitmap 1.5 contour engine. Contract: internal note EXPLODE_BITMAP_CONTOUR_CONTRACT.
namespace Inkscape::Bitmap {
struct FitSettings { double smoothing = 50; };
struct ContourSegment { bool cubic; ContourPoint c1, c2, end; };
struct FittedRing {
    std::uint32_t begin, end;
    ContourPoint start;
    double area, minX, minY, maxX, maxY;
    std::uint32_t depth, parent;
    bool fallback;
};
struct FittedPiece { std::uint32_t piece, ringBegin, ringEnd; bool noContour; };
enum class FitPhase { simplify, fitting, validation, output, allocation, fitterReservation };
struct FitOptions {
    // Lower-only ceilings; a closing segment ends at start and adds no anchor.
    std::uint32_t maxAnchors = 200000, maxAnchorsPerPiece = 50000;
    std::uint64_t maxSerializedBytes = 16 * MiB;
    AllocationFault *fault = nullptr;
    void (*observe)(FitPhase, void *) noexcept = nullptr;
    void *observerData = nullptr;
    // Caller-local fitter seam for failure/exception oracles. Null uses 2geom.
    // Return 1 for a single cubic (start, c1, c2, end), otherwise ring fallback.
    // Invoked under the same span-points * 64 Budget reservation as 2geom.
    int (*fitCubic)(ContourPoint *, ContourPoint const *, std::uint32_t, double) = nullptr;
};
class FittedContourSet {
public:
    FittedContourSet() noexcept = default;
    FittedContourSet(FittedContourSet const &) = delete;
    FittedContourSet &operator=(FittedContourSet const &) = delete;
    FittedContourSet(FittedContourSet &&o) noexcept { *this = std::move(o); }
    FittedContourSet &operator=(FittedContourSet &&) noexcept;
    std::uint32_t segmentCount = 0, ringCount = 0, pieceCount = 0;
    std::uint64_t anchorCount = 0, serializedBytes = 0;
    ContourSegment const *segments() const noexcept { return reinterpret_cast<ContourSegment const *>(_segments.data()); }
    FittedRing const *rings() const noexcept { return reinterpret_cast<FittedRing const *>(_rings.data()); }
    FittedPiece const *pieces() const noexcept { return reinterpret_cast<FittedPiece const *>(_pieces.data()); }
private:
    PlainBuffer _segments, _rings, _pieces;
    friend Result<FittedContourSet> fitContours(ContourSet const &, FitSettings, Budget &, JobWork &, Stop, FitOptions) noexcept;
};
// Budget must outlive the result. Ring/segment ranges are half-open; parent is
// a global ring index, UINT32_MAX for roots. No partial result on refusal.
Result<FittedContourSet> fitContours(ContourSet const &, FitSettings, Budget &, JobWork &,
                                    Stop = {}, FitOptions = {}) noexcept;
} // namespace Inkscape::Bitmap
#endif
