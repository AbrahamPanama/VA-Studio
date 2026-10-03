// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_DISPLAY_BITMAP_HISTOGRAM_H
#define INKSCAPE_DISPLAY_BITMAP_HISTOGRAM_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "display/bitmap-tone.h"

namespace Inkscape {
class Pixbuf;
}

namespace Inkscape::Filters {

struct BitmapHistogram {
    std::array<uint64_t, 256> luminance{};
    uint64_t sampled_pixels = 0;
    uint64_t transparent_pixels = 0;
    uint64_t shadow_clipped = 0;
    uint64_t highlight_clipped = 0;

    bool empty() const { return sampled_pixels == 0; }
    uint64_t peak() const;
};

BitmapHistogram build_bitmap_histogram(Pixbuf const &pixbuf,
                                       std::size_t max_samples = 262144);
BitmapHistogram remap_bitmap_histogram(BitmapHistogram const &source,
                                       BitmapToneSettings const &settings);

} // namespace Inkscape::Filters

#endif // INKSCAPE_DISPLAY_BITMAP_HISTOGRAM_H
