// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cstdint>
#include <cmath>
#include <memory>

#include <cairo.h>

#include "display/bitmap-histogram.h"
#include "display/cairo-utils.h"
#include "display/nr-filter-clipping-warning.h"

using namespace Inkscape;
using namespace Inkscape::Filters;

namespace {

std::unique_ptr<Pixbuf> make_test_pixbuf()
{
    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 1);
    auto pixels = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(surface));
    pixels[0] = 0xff000000; // black
    pixels[1] = 0xff808080; // middle gray
    pixels[2] = 0xffffffff; // white
    pixels[3] = 0x00000000; // transparent and ignored
    cairo_surface_mark_dirty(surface);
    return std::make_unique<Pixbuf>(surface); // Pixbuf owns the surface reference.
}

} // namespace

TEST(BitmapHistogramTest, SamplesLuminanceAndIgnoresTransparentPixels)
{
    auto pixbuf = make_test_pixbuf();
    auto histogram = build_bitmap_histogram(*pixbuf);
    EXPECT_EQ(histogram.sampled_pixels, 3);
    EXPECT_EQ(histogram.transparent_pixels, 1);
    EXPECT_EQ(histogram.luminance[0], 1);
    EXPECT_EQ(histogram.luminance[128], 1);
    EXPECT_EQ(histogram.luminance[255], 1);
    EXPECT_EQ(histogram.shadow_clipped, 1);
    EXPECT_EQ(histogram.highlight_clipped, 1);
}

TEST(BitmapHistogramTest, SamplingBudgetIsBounded)
{
    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100);
    auto const stride = cairo_image_surface_get_stride(surface) / sizeof(uint32_t);
    auto pixels = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(surface));
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) pixels[y * stride + x] = 0xff808080;
    }
    cairo_surface_mark_dirty(surface);
    Pixbuf pixbuf(surface);
    auto histogram = build_bitmap_histogram(pixbuf, 100);
    EXPECT_LE(histogram.sampled_pixels, 100);
    EXPECT_GT(histogram.sampled_pixels, 0);
}

TEST(BitmapHistogramTest, RemapUsesTheSameToneCurveAsRendering)
{
    BitmapHistogram source;
    source.luminance[64] = 2;
    source.luminance[128] = 3;
    source.luminance[192] = 5;
    source.sampled_pixels = 10;

    BitmapToneSettings settings;
    settings.brightness = 25;
    auto const table = build_bitmap_tone_table(settings);
    auto remapped = remap_bitmap_histogram(source, settings);

    for (auto bin : {64u, 128u, 192u}) {
        auto destination = static_cast<unsigned>(std::lround(table[bin] * 255.0));
        EXPECT_GE(remapped.luminance[destination], source.luminance[bin]);
    }
    EXPECT_EQ(remapped.sampled_pixels, source.sampled_pixels);
}

TEST(BitmapClippingWarningTest, RecolorsOnlyClippedOpaquePixels)
{
    EXPECT_EQ(apply_clipping_warning_to_pixel(0xff000000), 0xff0070ff);
    EXPECT_EQ(apply_clipping_warning_to_pixel(0xffffffff), 0xffff00b0);
    EXPECT_EQ(apply_clipping_warning_to_pixel(0xff808080), 0xff808080);
    EXPECT_EQ(apply_clipping_warning_to_pixel(0x00000000), 0x00000000);
}
