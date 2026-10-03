// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Tests for classes like Pixbuf from cairo-utils
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2020 Authors
 *
 * Released under GNU GPL version 2 or later, read the file 'COPYING' for more information
 */

#include <gtest/gtest.h>
#include <new>
#include <src/display/cairo-templates.h>
#include <src/display/cairo-utils.h>
#include <src/inkscape.h>


class PixbufTest : public ::testing::Test {
  public:
    static std::string base64of(const std::string &s)
    {
        gchar *encoded = g_base64_encode(reinterpret_cast<guchar const *>(s.c_str()), s.size());
        std::string r(encoded);
        g_free(encoded);
        return r;
    }

  protected:
    void SetUp() override
    {
        // setup hidden dependency
        Inkscape::Application::create(false);
    }
};

TEST_F(PixbufTest, creatingFromSvgBufferWithoutViewboxOrWidthAndHeightReturnsNull)
{
    std::string svg_buffer(
        "<svg><path d=\"M 71.527648,186.14229 A 740.48715,740.48715 0 0 0 696.31258,625.8041 Z\"/></svg>");
    double default_dpi = 96.0;
    std::string filename_with_svg_extension("malformed.svg");

    ASSERT_EQ(Inkscape::Pixbuf::create_from_buffer(svg_buffer, default_dpi, filename_with_svg_extension), nullptr);
}

TEST_F(PixbufTest, creatingFromSvgUriWithoutViewboxOrWidthAndHeightReturnsNull)
{
    std::string uri_data = "image/svg+xml;base64," + base64of("<svg><path d=\"M 71.527648,186.14229 A 740.48715,740.48715 0 0 0 696.31258,625.8041 Z\"/></svg>");
    double default_dpi = 96.0;

    ASSERT_EQ(Inkscape::Pixbuf::create_from_data_uri(uri_data.c_str(), default_dpi), nullptr);
}

// ---------------------------------------------------------------------------
// Out-of-memory safety (ST-S R3/R4)
// ---------------------------------------------------------------------------

namespace {
/// An image surface Cairo refuses to allocate (wider than 32767 px): it is a failed surface with no
/// pixel storage, like the one a real out-of-memory leaves behind, and costs no memory to create.
cairo_surface_t *make_failed_surface()
{
    return cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 40000, 4);
}
} // namespace

TEST(CairoSurfaceSafety, ByteSizeDoesNotOverflowInt)
{
    // 60000 x 60000 ARGB32 is 14.4 GB: far past INT_MAX, which is what stride * height wrapped at.
    size_t const size = ink_cairo_surface_byte_size(240000, 60000);
    EXPECT_EQ(size, size_t(240000) * size_t(60000));
    EXPECT_GT(size, size_t(2147483647));
    EXPECT_EQ(ink_cairo_surface_byte_size(0, 10), 0u);
    EXPECT_EQ(ink_cairo_surface_byte_size(-4, 10), 0u);
    EXPECT_EQ(ink_cairo_surface_byte_size(4, -10), 0u);
    // The int expression this replaces: it is negative or wrong for the same inputs.
    int const wrapped = static_cast<int>(static_cast<unsigned>(240000) * static_cast<unsigned>(60000));
    EXPECT_NE(static_cast<size_t>(wrapped), size);
}

TEST(CairoSurfaceSafety, RequirePixelsAcceptsRealSurfacesAndRejectsFailedOnes)
{
    cairo_surface_t *good = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    EXPECT_NO_THROW(ink_cairo_surface_require_pixels(good));
    cairo_surface_destroy(good);

    cairo_surface_t *empty = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 0, 0);
    EXPECT_NO_THROW(ink_cairo_surface_require_pixels(empty)) << "an empty surface is valid, not an error";
    cairo_surface_destroy(empty);

    cairo_surface_t *failed = make_failed_surface();
    ASSERT_NE(cairo_surface_status(failed), CAIRO_STATUS_SUCCESS);
    EXPECT_THROW(ink_cairo_surface_require_pixels(failed), std::bad_alloc);
    cairo_surface_destroy(failed);
    EXPECT_THROW(ink_cairo_surface_require_pixels(nullptr), std::bad_alloc);
}

TEST(CairoSurfaceSafety, CopyOfFailedSurfaceThrowsInsteadOfWritingThroughNull)
{
    cairo_surface_t *failed = make_failed_surface();
    EXPECT_THROW(ink_cairo_surface_copy(failed), std::bad_alloc);
    cairo_surface_destroy(failed);

    // A good surface still copies exactly.
    cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 3, 2);
    auto *px = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(src));
    for (int i = 0; i < 6; ++i) px[i] = 0xff000000u + i;
    cairo_surface_mark_dirty(src);
    cairo_surface_t *copy = ink_cairo_surface_copy(src);
    cairo_surface_flush(copy);
    auto *cp = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(copy));
    for (int i = 0; i < 6; ++i) EXPECT_EQ(cp[i], 0xff000000u + i);
    cairo_surface_destroy(copy);
    cairo_surface_destroy(src);
}

TEST(CairoSurfaceSafety, FilterTemplatesThrowOnFailedOutputInsteadOfWritingThroughNull)
{
    cairo_surface_t *in = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    cairo_surface_t *failed = make_failed_surface();
    auto invert = [](guint32 p) { return ~p; };
    EXPECT_THROW(ink_cairo_surface_filter(in, failed, invert), std::bad_alloc);
    EXPECT_THROW(ink_cairo_surface_blend(in, in, failed, [](guint32 a, guint32) { return a; }), std::bad_alloc);
    EXPECT_THROW(ink_cairo_surface_synthesize(failed, [](int, int) -> guint32 { return 0; }), std::bad_alloc);
    EXPECT_THROW(SurfaceSynth{failed}, std::bad_alloc);

    // Healthy surfaces are untouched by the check: the same call still writes the result.
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    ink_cairo_surface_filter(in, out, invert);
    cairo_surface_flush(out);
    EXPECT_EQ(reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(out))[5], 0xffffffffu);
    cairo_surface_destroy(out);
    cairo_surface_destroy(failed);
    cairo_surface_destroy(in);
}

TEST(CairoSurfaceSafety, SameSizeSurfaceOfFailedSourceThrows)
{
    cairo_surface_t *failed = make_failed_surface();
    EXPECT_THROW(ink_cairo_surface_create_same_size(failed, CAIRO_CONTENT_COLOR_ALPHA), std::bad_alloc);
    EXPECT_THROW(ink_cairo_surface_create_identical(failed), std::bad_alloc);
    cairo_surface_destroy(failed);
}
