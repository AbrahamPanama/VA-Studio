// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UTIL_BITMAP_CONTOUR_OFFSET_H
#define INKSCAPE_UTIL_BITMAP_CONTOUR_OFFSET_H
#include "util/bitmap-contour.h"
namespace Inkscape::Bitmap {
struct OffsetSettings { double offsetMm = 0, gapToleranceMm = 0.5, dpiX = 96, dpiY = 96; };
// Sample centres are origin + (x+.5,y+.5), matching B1. Budget outlives storage.
struct DistanceField {
    PlainBuffer storage;
    FieldView field;
    std::uint64_t peakBudgetBytes = 0;
    FieldView view() const noexcept { return field; }
};
Result<DistanceField> signedDistance(RingSet const &, double originX, double originY,
    std::uint32_t width, std::uint32_t height, double mmPerPxX, double mmPerPxY,
    Budget &, JobWork &, Stop = {}) noexcept;
Result<ContourSet> offsetContours(RgbaView, Partition const &, OffsetSettings,
    Budget &, JobWork &, Stop = {}, ContourOptions = {}) noexcept;
} // namespace Inkscape::Bitmap
#endif
