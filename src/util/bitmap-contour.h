// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UTIL_BITMAP_CONTOUR_H
#define INKSCAPE_UTIL_BITMAP_CONTOUR_H
#include "util/bitmap-island-enclosure.h"

namespace Inkscape::Bitmap {
struct ContourPoint { double x, y; };
struct ContourRing {
    std::uint32_t begin, end;
    double area, minX, minY, maxX, maxY;
    std::uint32_t depth, parent;
};
struct PieceContour { std::uint32_t piece, ringBegin, ringEnd; bool noContour; };
struct FieldView {
    float const *values = nullptr;
    std::uint32_t width = 0, height = 0;
    double originX = 0, originY = 0;
};
enum class ContourPhase { field, trace, stitch, hierarchy, output, allocation };
struct ContourOptions {
    // Lower-only: traceLevel is one piece (2,000,000 raw points); traceContours
    // also enforces the 8,000,000-raw-point job ceiling. No simplification or truncation.
    std::uint32_t maxPoints = 8000000, maxRings = 50000;
    AllocationFault *fault = nullptr;
    // allocation observes every successful reservation, including live overlap.
    void (*observe)(ContourPhase, void *) noexcept = nullptr;
    void *observerData = nullptr;
};
// Budget must outlive these move-only results. Rings have no repeated closing
// vertex. Parent indexes address this set's rings, UINT32_MAX denotes a root.
class RingSet {
public:
    RingSet() noexcept = default;
    RingSet(RingSet &&o) noexcept { *this = std::move(o); }
    RingSet &operator=(RingSet &&) noexcept;
    std::uint32_t pointCount = 0, ringCount = 0;
    std::uint64_t peakBudgetBytes = 0; // includes input reservations and allocation overlap
    ContourPoint const *points() const noexcept { return reinterpret_cast<ContourPoint const *>(_points.data()); }
    ContourRing const *rings() const noexcept { return reinterpret_cast<ContourRing const *>(_rings.data()); }
private:
    PlainBuffer _points, _rings;
    friend Result<RingSet> traceLevel(FieldView, Budget &, JobWork &, Stop, ContourOptions) noexcept;
};
struct OffsetSettings;
class ContourSet {
public:
    ContourSet() noexcept = default;
    ContourSet(ContourSet &&o) noexcept { *this = std::move(o); }
    ContourSet &operator=(ContourSet &&) noexcept;
    std::uint32_t pointCount = 0, ringCount = 0, pieceCount = 0;
    std::uint64_t peakBudgetBytes = 0; // whole Budget high-water mark during tracing
    ContourPoint const *points() const noexcept { return reinterpret_cast<ContourPoint const *>(_points.data()); }
    ContourRing const *rings() const noexcept { return reinterpret_cast<ContourRing const *>(_rings.data()); }
    PieceContour const *pieces() const noexcept { return reinterpret_cast<PieceContour const *>(_pieces.data()); }
private:
    PlainBuffer _points, _rings, _pieces;
    friend Result<ContourSet> offsetContours(RgbaView, Partition const &, OffsetSettings, Budget &, JobWork &, Stop, ContourOptions) noexcept;
    friend Result<ContourSet> traceContours(RgbaView, Partition const &, Budget &, JobWork &, Stop, ContourOptions) noexcept;
};
// B1 support samples, with the existing two-sample gutter.
struct SupportField {
    PlainBuffer storage;
    FieldView field;
    std::uint64_t peakBudgetBytes = 0;
};
Result<SupportField> pieceSupportField(RgbaView, Partition const &, std::uint32_t piece,
                                     Budget &, JobWork &, Stop = {}, ContourOptions = {}) noexcept;
Result<RingSet> traceLevel(FieldView, Budget &, JobWork &, Stop = {}, ContourOptions = {}) noexcept;
Result<ContourSet> traceContours(RgbaView, Partition const &, Budget &, JobWork &, Stop = {}, ContourOptions = {}) noexcept;
} // namespace Inkscape::Bitmap
#endif
