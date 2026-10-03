// SPDX-License-Identifier: GPL-2.0-or-later

#include "bitmap-histogram.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "display/cairo-utils.h"

namespace Inkscape::Filters {
namespace {

constexpr unsigned clipping_threshold = 2;

unsigned luminance(unsigned red, unsigned green, unsigned blue)
{
    return std::clamp(static_cast<unsigned>(std::lround(
        0.2126 * red + 0.7152 * green + 0.0722 * blue)), 0u, 255u);
}

void count_pixel(BitmapHistogram &histogram, unsigned alpha,
                 unsigned red, unsigned green, unsigned blue)
{
    if (!alpha) {
        ++histogram.transparent_pixels;
        return;
    }
    auto const value = luminance(red, green, blue);
    ++histogram.luminance[value];
    ++histogram.sampled_pixels;
    if (value <= clipping_threshold) ++histogram.shadow_clipped;
    if (value >= 255 - clipping_threshold) ++histogram.highlight_clipped;
}

} // namespace

uint64_t BitmapHistogram::peak() const
{
    return *std::max_element(luminance.begin(), luminance.end());
}

BitmapHistogram build_bitmap_histogram(Pixbuf const &pixbuf, std::size_t max_samples)
{
    BitmapHistogram result;
    auto const width = pixbuf.width();
    auto const height = pixbuf.height();
    auto const pixels = pixbuf.pixels();
    if (width <= 0 || height <= 0 || !pixels || max_samples == 0) return result;

    auto const total = static_cast<std::size_t>(width) * height;
    auto const stride = std::max<std::size_t>(1, static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(total) / max_samples))));
    auto const rowstride = pixbuf.rowstride();

    for (int y = 0; y < height; y += stride) {
        auto const row = pixels + static_cast<std::size_t>(y) * rowstride;
        for (int x = 0; x < width; x += stride) {
            if (pixbuf.pixelFormat() == Pixbuf::PF_GDK) {
                auto const pixel = row + static_cast<std::size_t>(x) * 4;
                count_pixel(result, pixel[3], pixel[0], pixel[1], pixel[2]);
            } else {
                uint32_t pixel = 0;
                std::memcpy(&pixel, row + static_cast<std::size_t>(x) * 4, sizeof(pixel));
                auto const alpha = (pixel >> 24) & 0xff;
                auto red = (pixel >> 16) & 0xff;
                auto green = (pixel >> 8) & 0xff;
                auto blue = pixel & 0xff;
                if (alpha && alpha != 255) {
                    red = unpremul_alpha(red, alpha);
                    green = unpremul_alpha(green, alpha);
                    blue = unpremul_alpha(blue, alpha);
                }
                count_pixel(result, alpha, red, green, blue);
            }
        }
    }
    return result;
}

BitmapHistogram remap_bitmap_histogram(BitmapHistogram const &source,
                                       BitmapToneSettings const &settings)
{
    BitmapHistogram result;
    result.transparent_pixels = source.transparent_pixels;
    auto const table = build_bitmap_tone_table(settings);
    for (unsigned source_bin = 0; source_bin < source.luminance.size(); ++source_bin) {
        auto const count = source.luminance[source_bin];
        auto const destination = std::clamp(static_cast<unsigned>(
            std::lround(table[source_bin] * 255.0)), 0u, 255u);
        result.luminance[destination] += count;
        result.sampled_pixels += count;
        if (destination <= clipping_threshold) result.shadow_clipped += count;
        if (destination >= 255 - clipping_threshold) result.highlight_clipped += count;
    }
    return result;
}

} // namespace Inkscape::Filters
