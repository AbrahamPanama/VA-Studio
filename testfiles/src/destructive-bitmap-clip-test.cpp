// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui/tools/destructive-bitmap-clip.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/ustring.h>
#include <gtest/gtest.h>
#include <cairomm/surface.h>
#include <2geom/curves.h>
#include <2geom/rect.h>

#include "desktop.h"
#include "display/cairo-utils.h"
#include "display/drawing.h"
#include "display/drawing-item.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "selection.h"
#include "svg/svg.h"
#include "ui/tools/destructive-bitmap-clip-chemistry.h"
#include "xml/attribute-record.h"
#include "xml/href-attribute-helper.h"
#include "xml/document.h"
#include "xml/repr.h"
#include "xml/node-observer.h"

using namespace Inkscape;
namespace BitmapClip = Inkscape::UI::Tools::DestructiveBitmapClip;

namespace {

std::unique_ptr<Pixbuf> solid_pixbuf(int width, int height, guint32 rgba)
{
    auto *raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!raw) {
        return {};
    }
    gdk_pixbuf_fill(raw, rgba);
    return std::make_unique<Pixbuf>(raw);
}

Geom::PathVector rect_path(double left, double top, double right, double bottom)
{
    return {Geom::Path(Geom::Rect(left, top, right, bottom))};
}

Geom::PathVector triangle_path(Geom::Point a, Geom::Point b, Geom::Point c)
{
    Geom::Path triangle(a);
    triangle.appendNew<Geom::LineSegment>(b);
    triangle.appendNew<Geom::LineSegment>(c);
    triangle.close();
    return {std::move(triangle)};
}

unsigned char const *pixel(Pixbuf const &pixbuf, int x, int y)
{
    return pixbuf.pixels() + static_cast<std::size_t>(y) * pixbuf.rowstride() +
           static_cast<std::size_t>(x) * 4;
}

unsigned char *pixel(Pixbuf &pixbuf, int x, int y)
{
    return pixbuf.pixels() + static_cast<std::size_t>(y) * pixbuf.rowstride() +
           static_cast<std::size_t>(x) * 4;
}

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
constexpr std::size_t cairo_alpha_offset = 3;
#else
constexpr std::size_t cairo_alpha_offset = 0;
#endif

unsigned alpha_at(Pixbuf const &pixbuf, int x, int y)
{
    auto const offset = pixbuf.pixelFormat() == Pixbuf::PF_CAIRO
                            ? cairo_alpha_offset
                            : std::size_t{3};
    return pixel(pixbuf, x, y)[offset];
}

void convert_to_gdk(BitmapClip::Result &result)
{
    ASSERT_TRUE(result.pixels);
    result.pixels->ensurePixelFormat(Pixbuf::PF_GDK);
}

std::vector<unsigned char> snapshot(Pixbuf const &pixbuf)
{
    std::vector<unsigned char> result(static_cast<std::size_t>(pixbuf.width()) * pixbuf.height() * 4);
    for (int y = 0; y < pixbuf.height(); ++y) {
        std::copy_n(pixel(pixbuf, 0, y), static_cast<std::size_t>(pixbuf.width()) * 4,
                    result.data() + static_cast<std::size_t>(y) * pixbuf.width() * 4);
    }
    return result;
}

std::string png_uri(Pixbuf const &pixbuf)
{
    auto encoded = sp_image_encode_png_data_uri(pixbuf);
    return encoded ? *encoded : std::string{};
}

// Independent oracle: explicitly erase the known integer source rectangle,
// keeping the original canvas. Never call apply(), its alpha scanner, its crop
// result, or the geometry/resource planner to manufacture expected pixels.
std::unique_ptr<Pixbuf> untrimmed_rectangle_oracle(int left, int top, int right, int bottom,
                                                 BitmapClip::Mode mode)
{
    auto result = solid_pixbuf(8, 8, 0x336699ff);
    if (!result) return {};
    result->ensurePixelFormat(Pixbuf::PF_CAIRO);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            bool const inside = x >= left && x < right && y >= top && y < bottom;
            if (inside != (mode == BitmapClip::Mode::KeepInside)) std::memset(pixel(*result, x, y), 0, 4);
        }
    }
    return result;
}

Cairo::RefPtr<Cairo::ImageSurface> render_oracle_grid(SPDocument &document, double zoom)
{
    document.ensureUpToDate();
    Drawing drawing;
    auto const key = SPItem::display_key_new(1);
    auto root = document.getRoot();
    auto shown = root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
    drawing.setRoot(shown);
    shown->setTransform(Geom::Scale(zoom) * Geom::Translate(12.25, 9.375));
    // The cutter must remain in XML/selection; exclude it from both rendering
    // views so it cannot conceal an image-edge mismatch.
    if (auto cutter = cast<SPItem>(document.getObjectById("cutter"))) {
        if (auto view = cutter->get_arenaitem(key)) view->setVisible(false);
    }
    drawing.update();
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 128, 96);
    DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
    DrawingContext context(target);
    drawing.render(context, Geom::IntRect::from_xywh(0, 0, 128, 96));
    surface->flush();
    root->invoke_hide(key);
    return surface;
}

::testing::AssertionResult same_render(Cairo::RefPtr<Cairo::ImageSurface> const &expected,
                                      Cairo::RefPtr<Cairo::ImageSurface> const &actual)
{
    std::size_t different = 0;
    int first_x = -1, first_y = -1;
    guint32 first_expected = 0, first_actual = 0;
    for (int y = 0; y < expected->get_height(); ++y) {
        for (int x = 0; x < expected->get_width(); ++x) {
            guint32 a, b;
            std::memcpy(&a, expected->get_data() + y * expected->get_stride() + 4 * x, 4);
            std::memcpy(&b, actual->get_data() + y * actual->get_stride() + 4 * x, 4);
            if (a != b) {
                if (!different) { first_x = x; first_y = y; first_expected = a; first_actual = b; }
                ++different;
            }
        }
    }
    if (!different) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << different << " differing premultiplied pixels; first at "
        << first_x << ',' << first_y << ": expected ARGB=" << first_expected << ", actual=" << first_actual;
}

void drain_main_context()
{
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
}

std::string xml_for_bitmap_undo(XML::Document const *document)
{
    auto release = [](XML::Document *copy) { GC::release(copy); };
    std::unique_ptr<XML::Document, decltype(release)> copy(document->duplicate(nullptr), release);
    // Undo restores a removed attribute with setAttribute(), which appends it.
    // The readable Undo diagnostic confirmed only bitmap's absref moves. Move
    // that one attribute in a DETACHED snapshot; preserve its exact value, every
    // other attribute's order/value, all nodes, content and child order. Keep
    // byte-exact snapshots for failure/no-op/idle checks on the live document.
    sp_repr_visit_descendants(copy.get(), [](XML::Node *node) {
        if (g_strcmp0(node->name(), "svg:image") == 0 &&
            g_strcmp0(node->attribute("id"), "bitmap") == 0) {
            if (auto const *absref = node->attribute("sodipodi:absref")) {
                std::string const value(absref);
                node->removeAttribute("sodipodi:absref");
                node->setAttribute("sodipodi:absref", value.c_str());
            }
        }
        return true;
    });
    return sp_repr_save_buf(copy.get()).raw();
}

InkscapeApplication *initialize_application()
{
    static auto *application = [] {
        g_setenv("INKSCAPE_APP_ID_TAG", "destructivebitmapcliptest", TRUE);
        auto *result = new InkscapeApplication(); // Process-lifetime test fixture.
        result->gio_app()->register_application();
        return result;
    }();
    return application;
}

void expect_full_coverage_rgb_or_transparent_black(Pixbuf const &expected, Pixbuf const &actual)
{
    ASSERT_EQ(expected.width(), actual.width());
    ASSERT_EQ(expected.height(), actual.height());
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            if (alpha_at(actual, x, y) == 0) {
                for (int channel = 0; channel < 4; ++channel) {
                    EXPECT_EQ(pixel(actual, x, y)[channel], 0u)
                        << "discarded pixel at (" << x << ", " << y << ") channel " << channel;
                }
            } else if (alpha_at(actual, x, y) == alpha_at(expected, x, y)) {
                // Full-coverage pixels make an exact lossless round trip. At a
                // partially covered edge, straight-alpha RGB can legitimately
                // move by several units when a tiny premultiplied value is
                // unpremultiplied, so those pixels are checked in Cairo form.
                for (int channel = 0; channel < 3; ++channel) {
                    EXPECT_EQ(pixel(expected, x, y)[channel], pixel(actual, x, y)[channel])
                        << "retained pixel at (" << x << ", " << y << ") channel " << channel;
                }
            }
        }
    }
}

// Contract alpha multiply from plan 4.1, re-derived here so the oracle does not
// call the implementation's helper.
unsigned char alpha_mul(unsigned char a, unsigned char b)
{
    return static_cast<unsigned char>((static_cast<unsigned>(a) * b + 127u) / 255u);
}

// Coverage/complement tests compare both results on the original source grid.
// Reconstruct using the public crop offsets so these tests still exercise the
// production crop. The engine returns detached straight RGBA, so the canvas is
// a straight (PF_GDK) buffer; converting a Cairo-format result first keeps the
// helper independent of the engine's storage format.
BitmapClip::Result reconstruct_in_source_grid(BitmapClip::Result result, int width, int height)
{
    if (!result.completed() || !result.trimmed()) {
        return result;
    }
    result.pixels->ensurePixelFormat(Pixbuf::PF_GDK);
    auto canvas = solid_pixbuf(width, height, 0);
    if (!canvas) {
        ADD_FAILURE() << "Could not allocate reconstructed fixture";
        return {};
    }
    for (int y = 0; y < result.pixels->height(); ++y) {
        std::memcpy(pixel(*canvas, result.left, y + result.top), pixel(*result.pixels, 0, y),
                    static_cast<std::size_t>(result.pixels->width()) * 4);
    }
    result.pixels = std::move(canvas);
    result.left = result.top = 0;
    return result;
}

BitmapClip::Result apply_in_source_grid(Pixbuf const &source, BitmapClip::Coverage const &coverage,
                                        Geom::PathVector const &path, BitmapClip::FillRule rule,
                                        BitmapClip::Mode mode, std::stop_token cancellation = {})
{
    return reconstruct_in_source_grid(
        BitmapClip::apply(source, coverage, path, rule, mode, cancellation),
        source.width(), source.height());
}

BitmapClip::Result apply_in_source_grid(Pixbuf const &source, Geom::PathVector const &path,
                                        BitmapClip::FillRule rule, BitmapClip::Mode mode,
                                        std::stop_token cancellation = {})
{
    return apply_in_source_grid(source, BitmapClip::Coverage{}, path, rule, mode, cancellation);
}

} // namespace

TEST(DestructiveBitmapClipPixelsTest, KeepInsideAndOutsideUseComplementaryCoverage)
{
    auto source = solid_pixbuf(12, 10, 0x33669980);
    ASSERT_TRUE(source);
    auto const source_bytes = snapshot(*source);
    auto const cutter = triangle_path({1.25, 1.25}, {10.25, 2.75}, {4.25, 8.75});

    auto inside = apply_in_source_grid(*source, cutter, BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepInside);
    auto outside = apply_in_source_grid(*source, cutter, BitmapClip::FillRule::NonZero,
                                     BitmapClip::Mode::KeepOutside);

    ASSERT_TRUE(inside.completed());
    ASSERT_TRUE(outside.completed());
    EXPECT_TRUE(inside.changed);
    EXPECT_TRUE(outside.changed);
    EXPECT_EQ(snapshot(*source), source_bytes) << "the caller-owned source must remain untouched";

    bool saw_partial_coverage = false;
    for (int y = 0; y < source->height(); ++y) {
        for (int x = 0; x < source->width(); ++x) {
            auto const *original = pixel(*source, x, y);
            auto const *in = pixel(*inside.pixels, x, y);
            auto const *out = pixel(*outside.pixels, x, y);
            // The cutter complement is exact in alpha: in + out == source alpha.
            EXPECT_EQ(static_cast<unsigned>(in[3]) + out[3], original[3])
                << "alpha at (" << x << ", " << y << ')';
            // Straight RGB survives byte-for-byte on every covered pixel; a
            // discarded pixel is transparent black.
            for (int channel = 0; channel < 3; ++channel) {
                EXPECT_EQ(in[channel], in[3] ? original[channel] : 0u)
                    << "inside channel " << channel << " at (" << x << ", " << y << ')';
                EXPECT_EQ(out[channel], out[3] ? original[channel] : 0u)
                    << "outside channel " << channel << " at (" << x << ", " << y << ')';
            }
            saw_partial_coverage |= in[3] > 0 && in[3] < original[3];
        }
    }
    EXPECT_TRUE(saw_partial_coverage);

    convert_to_gdk(inside);
    convert_to_gdk(outside);
    expect_full_coverage_rgb_or_transparent_black(*source, *inside.pixels);
    expect_full_coverage_rgb_or_transparent_black(*source, *outside.pixels);
}

TEST(DestructiveBitmapClipPixelsTest, DiscardedPixelsBecomeTransparentBlack)
{
    auto source = solid_pixbuf(6, 4, 0x102030ff);
    ASSERT_TRUE(source);
    auto *already_transparent = pixel(*source, 0, 0);
    already_transparent[0] = 211;
    already_transparent[1] = 37;
    already_transparent[2] = 149;
    already_transparent[3] = 0;
    auto const bytes_before = snapshot(*source);

    auto result = apply_in_source_grid(*source, rect_path(2, 1, 5, 3),
                                    BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(result.completed());
    EXPECT_EQ(snapshot(*source), bytes_before);
    convert_to_gdk(result);
    expect_full_coverage_rgb_or_transparent_black(*source, *result.pixels);
    EXPECT_EQ(alpha_at(*result.pixels, 0, 0), 0u);
    EXPECT_EQ(pixel(*result.pixels, 0, 0)[0], 0u);
    EXPECT_EQ(pixel(*result.pixels, 0, 0)[1], 0u);
    EXPECT_EQ(pixel(*result.pixels, 0, 0)[2], 0u);
    EXPECT_EQ(alpha_at(*result.pixels, 3, 2), 255u);
}

TEST(DestructiveBitmapClipPixelsTest, SaturatedAntialiasedEdgeHasNoColoredOrBlackHalo)
{
    auto source = solid_pixbuf(5, 3, 0xff0000ff);
    ASSERT_TRUE(source);

    auto result = apply_in_source_grid(*source, rect_path(1.5, 0, 5, 3),
                                    BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(result.completed());
    convert_to_gdk(result);

    auto const *discarded = pixel(*result.pixels, 0, 1);
    EXPECT_EQ(discarded[0], 0u);
    EXPECT_EQ(discarded[1], 0u);
    EXPECT_EQ(discarded[2], 0u);
    EXPECT_EQ(discarded[3], 0u);

    auto const *edge = pixel(*result.pixels, 1, 1);
    ASSERT_GT(edge[3], 0u);
    ASSERT_LT(edge[3], 255u);
    EXPECT_EQ(edge[0], 255u);
    EXPECT_EQ(edge[1], 0u);
    EXPECT_EQ(edge[2], 0u);

    // Straight-alpha composition over white must remain saturated red at the
    // antialiased edge, not darken toward black or reveal hidden edge color.
    auto const composite_red = (edge[0] * edge[3] + 255u * (255u - edge[3]) + 127u) / 255u;
    auto const composite_green = (edge[1] * edge[3] + 255u * (255u - edge[3]) + 127u) / 255u;
    EXPECT_EQ(composite_red, 255u);
    EXPECT_EQ(composite_green, 255u - edge[3]);
}

TEST(DestructiveBitmapClipPixelsTest, EvenOddRuleRetainsHoles)
{
    auto source = solid_pixbuf(12, 12, 0x8899aaff);
    ASSERT_TRUE(source);
    auto cutter = rect_path(1, 1, 11, 11);
    cutter.push_back(Geom::Path(Geom::Rect(4, 4, 8, 8)));

    auto even_odd = apply_in_source_grid(*source, cutter, BitmapClip::FillRule::EvenOdd,
                                      BitmapClip::Mode::KeepInside);
    auto non_zero = apply_in_source_grid(*source, cutter, BitmapClip::FillRule::NonZero,
                                      BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(even_odd.completed());
    ASSERT_TRUE(non_zero.completed());
    convert_to_gdk(even_odd);
    convert_to_gdk(non_zero);
    EXPECT_EQ(alpha_at(*even_odd.pixels, 6, 6), 0u);
    EXPECT_EQ(alpha_at(*non_zero.pixels, 6, 6), 255u);
    EXPECT_EQ(alpha_at(*even_odd.pixels, 2, 2), 255u);
}

TEST(DestructiveBitmapClipPixelsTest, ExistingAlphaIsMultipliedRatherThanReplaced)
{
    auto source = solid_pixbuf(8, 8, 0xaabbcc60);
    ASSERT_TRUE(source);

    auto inside = apply_in_source_grid(*source, rect_path(2, 2, 6, 6),
                                    BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepInside);
    auto outside = apply_in_source_grid(*source, rect_path(2, 2, 6, 6),
                                     BitmapClip::FillRule::NonZero,
                                     BitmapClip::Mode::KeepOutside);
    ASSERT_TRUE(inside.completed());
    ASSERT_TRUE(outside.completed());
    convert_to_gdk(inside);
    convert_to_gdk(outside);
    EXPECT_EQ(alpha_at(*inside.pixels, 4, 4), 0x60u);
    EXPECT_EQ(alpha_at(*inside.pixels, 0, 0), 0u);
    EXPECT_EQ(alpha_at(*outside.pixels, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*outside.pixels, 0, 0), 0x60u);
}

TEST(DestructiveBitmapClipPixelsTest, StraightRgbSurvivesExactAlphaCasesAndInverseRecompose)
{
    constexpr std::array<unsigned char, 6> source_alpha = {0, 1, 127, 128, 254, 255};
    auto source = solid_pixbuf(1, static_cast<int>(source_alpha.size()), 0xffffffff);
    ASSERT_TRUE(source);
    for (std::size_t y = 0; y < source_alpha.size(); ++y) {
        auto *rgba = pixel(*source, 0, static_cast<int>(y));
        rgba[0] = rgba[1] = rgba[2] = 255;
        rgba[3] = source_alpha[y];
    }
    auto const source_before = snapshot(*source);
    auto const half_pixel = rect_path(0.5, 0, 1, static_cast<double>(source_alpha.size()));

    auto inside = apply_in_source_grid(*source, half_pixel, BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepInside);
    auto outside = apply_in_source_grid(*source, half_pixel, BitmapClip::FillRule::NonZero,
                                     BitmapClip::Mode::KeepOutside);
    ASSERT_TRUE(inside.completed());
    ASSERT_TRUE(outside.completed());
    EXPECT_EQ(snapshot(*source), source_before);

    auto const coverage = pixel(*inside.pixels, 0, 5)[3];
    ASSERT_GT(coverage, 0u);
    ASSERT_LT(coverage, 255u);

    for (std::size_t y = 0; y < source_alpha.size(); ++y) {
        auto const *kept = pixel(*inside.pixels, 0, static_cast<int>(y));
        auto const *removed = pixel(*outside.pixels, 0, static_cast<int>(y));
        auto const expected_alpha = alpha_mul(source_alpha[y], coverage);
        EXPECT_EQ(kept[3], expected_alpha) << "source alpha " << +source_alpha[y];
        EXPECT_EQ(static_cast<unsigned>(kept[3]) + removed[3], source_alpha[y])
            << "source alpha " << +source_alpha[y];
        // White input keeps straight RGB == 255 whenever alpha survives; at
        // alpha 0 the output is transparent black, never hidden white.
        for (int channel = 0; channel < 3; ++channel) {
            EXPECT_EQ(kept[channel], expected_alpha ? 255u : 0u)
                << "kept source alpha " << +source_alpha[y] << ", channel " << channel;
            EXPECT_EQ(removed[channel], removed[3] ? 255u : 0u)
                << "removed source alpha " << +source_alpha[y] << ", channel " << channel;
        }
    }
}

TEST(DestructiveBitmapClipPixelsTest, DisjointCutterHasDefinedSemantics)
{
    auto source = solid_pixbuf(8, 8, 0x123456ff);
    ASSERT_TRUE(source);
    auto const cutter = rect_path(20, 20, 24, 24);

    auto inside = apply_in_source_grid(*source, cutter, BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepInside);
    auto outside = apply_in_source_grid(*source, cutter, BitmapClip::FillRule::NonZero,
                                     BitmapClip::Mode::KeepOutside);
    ASSERT_TRUE(inside.completed());
    ASSERT_TRUE(outside.completed());
    EXPECT_TRUE(inside.changed);
    EXPECT_TRUE(inside.all_transparent);
    EXPECT_FALSE(outside.changed);
    EXPECT_FALSE(outside.all_transparent);
    convert_to_gdk(outside);
    EXPECT_EQ(snapshot(*outside.pixels), snapshot(*source));
}

TEST(DestructiveBitmapClipPixelsTest, FullImageInsideAndTransparentInputsAreNoOps)
{
    auto opaque = solid_pixbuf(8, 8, 0x123456ff);
    ASSERT_TRUE(opaque);
    auto full = apply_in_source_grid(*opaque, rect_path(0, 0, 8, 8),
                                  BitmapClip::FillRule::NonZero,
                                  BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(full.completed());
    EXPECT_FALSE(full.changed);
    convert_to_gdk(full);
    EXPECT_EQ(snapshot(*full.pixels), snapshot(*opaque));

    auto transparent = solid_pixbuf(8, 8, 0x00000000);
    ASSERT_TRUE(transparent);
    auto empty = apply_in_source_grid(*transparent, rect_path(2, 2, 6, 6),
                                   BitmapClip::FillRule::NonZero,
                                   BitmapClip::Mode::KeepOutside);
    ASSERT_TRUE(empty.completed());
    EXPECT_FALSE(empty.changed);
    EXPECT_TRUE(empty.all_transparent);
    convert_to_gdk(empty);
    expect_full_coverage_rgb_or_transparent_black(*transparent, *empty.pixels);
}

TEST(DestructiveBitmapClipPixelsTest, RasterizesAcrossTileBoundaries)
{
    auto source = solid_pixbuf(520, 5, 0x556677ff);
    ASSERT_TRUE(source);
    auto result = apply_in_source_grid(*source, rect_path(250.25, 0.25, 262.75, 4.75),
                                    BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(result.completed());
    convert_to_gdk(result);
    EXPECT_EQ(alpha_at(*result.pixels, 254, 2), 255u);
    EXPECT_EQ(alpha_at(*result.pixels, 258, 2), 255u);
    EXPECT_EQ(alpha_at(*result.pixels, 100, 2), 0u);
    EXPECT_EQ(alpha_at(*result.pixels, 400, 2), 0u);
}

TEST(DestructiveBitmapClipPixelsTest, CancellationAndInvalidGeometryExposeNoPartialResult)
{
    auto source = solid_pixbuf(32, 32, 0x102030ff);
    ASSERT_TRUE(source);
    auto const before = snapshot(*source);

    std::stop_source stop;
    stop.request_stop();
    auto cancelled = apply_in_source_grid(*source, rect_path(2, 2, 30, 30),
                                       BitmapClip::FillRule::NonZero,
                                       BitmapClip::Mode::KeepInside, stop.get_token());
    EXPECT_EQ(cancelled.status, BitmapClip::Status::Cancelled);
    EXPECT_FALSE(cancelled.pixels);
    EXPECT_FALSE(cancelled.changed);

    auto invalid = apply_in_source_grid(*source, {}, BitmapClip::FillRule::NonZero,
                                     BitmapClip::Mode::KeepInside);
    EXPECT_EQ(invalid.status, BitmapClip::Status::InvalidInput);
    EXPECT_FALSE(invalid.pixels);
    EXPECT_EQ(snapshot(*source), before);
}

TEST(DestructiveBitmapClipPixelsTest, DetachedRasterKernelCompletesOnARealWorkerThread)
{
    auto source = solid_pixbuf(64, 64, 0x336699ff);
    ASSERT_TRUE(source);
    auto cutter = rect_path(8.25, 7.75, 56.5, 55.25);
    auto const caller_thread = std::this_thread::get_id();

    auto future = std::async(std::launch::async, [source = std::move(source), cutter = std::move(cutter)]() mutable {
        auto const worker_thread = std::this_thread::get_id();
        auto result = apply_in_source_grid(*source, cutter, BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepInside);
        return std::make_pair(worker_thread, std::move(result));
    });
    auto [worker_thread, result] = future.get();

    EXPECT_NE(worker_thread, caller_thread);
    ASSERT_TRUE(result.completed());
    ASSERT_TRUE(result.changed);
    convert_to_gdk(result);
    EXPECT_EQ(alpha_at(*result.pixels, 0, 0), 0u);
    EXPECT_GT(alpha_at(*result.pixels, 32, 32), 0u);
}

TEST(DestructiveBitmapClipPixelsTest, TrimCopiesOddStrideAndLowAlphaIslandsExactly)
{
    // Explicit padding detects accidental assumptions about tight source rows.
    std::array<unsigned char, 7 * 40> bytes{};
    auto *raw = gdk_pixbuf_new_from_data(bytes.data(), GDK_COLORSPACE_RGB, TRUE, 8, 9, 7, 40,
                                        nullptr, nullptr);
    ASSERT_TRUE(raw);
    Pixbuf source(raw);
    for (auto const sample : {std::array<int, 3>{2, 1, 1}, {6, 5, 127}, {3, 4, 254}, {5, 2, 255}}) {
        auto *p = pixel(source, sample[0], sample[1]);
        p[0] = 255; p[1] = 0; p[2] = 255; p[3] = sample[2];
    }
    // The source stays straight RGBA: the contract output is detached straight
    // RGBA, so the trimmed rows must match the source bytes directly. The
    // previous premise converted the source to PF_CAIRO and compared Cairo
    // bytes; that cannot hold for a straight-RGBA result.
    auto const original = snapshot(source);
    auto result = BitmapClip::apply(source, rect_path(-1, -1, 10, 8), BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(result.completed());
    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(result.trimmed());
    EXPECT_EQ(result.left, 2);
    EXPECT_EQ(result.top, 1);
    EXPECT_EQ(result.original_width, 9);
    EXPECT_EQ(result.original_height, 7);
    ASSERT_EQ(result.pixels->width(), 5);
    ASSERT_EQ(result.pixels->height(), 5);
    EXPECT_EQ(result.pixels->pixelFormat(), Pixbuf::PF_GDK);
    for (int y = 0; y < 5; ++y) {
        EXPECT_EQ(std::memcmp(pixel(*result.pixels, 0, y), pixel(source, 2, y + 1), 5 * 4), 0);
    }
    EXPECT_EQ(alpha_at(*result.pixels, 0, 0), 1u);
    EXPECT_EQ(alpha_at(*result.pixels, 1, 1), 0u); // Keep transparent interior gaps.
    EXPECT_EQ(snapshot(source), original);
}

TEST(DestructiveBitmapClipPixelsTest, InverseStripsAndInteriorHoleChooseAlphaBounds)
{
    auto source = solid_pixbuf(10, 15, 0x336699ff);
    ASSERT_TRUE(source);
    struct Strip { double x, y, w, h; int left, top, width, height; };
    for (auto const &strip : {Strip{-1, 13, 12, 3, 0, 0, 10, 13},
                              Strip{-1, -1, 12, 3, 0, 2, 10, 13},
                              Strip{-1, -1, 3, 17, 2, 0, 8, 15},
                              Strip{8, -1, 3, 17, 0, 0, 8, 15},
                              Strip{3, 4, 2, 3, 0, 0, 10, 15},
                              Strip{-1, -1, 3, 3, 0, 0, 10, 15}}) {
        auto result = BitmapClip::apply(*source,
            rect_path(strip.x, strip.y, strip.x + strip.w, strip.y + strip.h),
            BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepOutside);
        ASSERT_TRUE(result.completed());
        EXPECT_EQ(result.left, strip.left);
        EXPECT_EQ(result.top, strip.top);
        EXPECT_EQ(result.pixels->width(), strip.width);
        EXPECT_EQ(result.pixels->height(), strip.height);
    }
    auto fractional = BitmapClip::apply(*source, rect_path(-1, 12.5, 11, 16),
        BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepOutside);
    ASSERT_TRUE(fractional.completed());
    EXPECT_EQ(fractional.pixels->height(), 13);
    EXPECT_GT(alpha_at(*fractional.pixels, 5, 12), 0u);
    EXPECT_LT(alpha_at(*fractional.pixels, 5, 12), 255u);
}

TEST(DestructiveBitmapClipPixelsTest, EmptyAndOnePixelResultsNeverInventExtent)
{
    for (auto mode : {BitmapClip::Mode::KeepInside, BitmapClip::Mode::KeepOutside}) {
        auto source = solid_pixbuf(1, 1, 0xff00ffff);
        auto result = BitmapClip::apply(*source, rect_path(0, 0, 1, 1), BitmapClip::FillRule::NonZero, mode);
        ASSERT_TRUE(result.completed());
        EXPECT_FALSE(result.trimmed());
        EXPECT_EQ(result.pixels->width(), 1);
        EXPECT_EQ(result.pixels->height(), 1);
        EXPECT_EQ(result.all_transparent, mode == BitmapClip::Mode::KeepOutside);
    }
    auto source = solid_pixbuf(9, 7, 0);
    auto empty = BitmapClip::apply(*source, rect_path(1, 1, 3, 3), BitmapClip::FillRule::NonZero,
                                   BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(empty.completed());
    EXPECT_FALSE(empty.changed);
    EXPECT_TRUE(empty.all_transparent);
    EXPECT_FALSE(empty.trimmed());
    EXPECT_EQ(empty.pixels->width(), 9);
    EXPECT_EQ(empty.pixels->height(), 7);
}

TEST(DestructiveBitmapClipPixelsTest, PngRoundTripPreservesColorMetadataButNotOrientation)
{
    auto source = solid_pixbuf(2, 2, 0x336699ff);
    ASSERT_TRUE(source);
    auto *raw = source->getPixbufRaw();

    gchar *profile_bytes = nullptr;
    gsize profile_size = 0;
    auto const profile_path = std::string(INKSCAPE_TESTS_DIR) + "/data/colors/display.icc";
    ASSERT_TRUE(g_file_get_contents(profile_path.c_str(), &profile_bytes, &profile_size, nullptr));
    auto *profile_base64 = g_base64_encode(reinterpret_cast<guchar const *>(profile_bytes), profile_size);
    g_free(profile_bytes);
    ASSERT_TRUE(profile_base64);

    ASSERT_TRUE(gdk_pixbuf_set_option(raw, "icc-profile", profile_base64));
    ASSERT_TRUE(gdk_pixbuf_set_option(raw, "x-dpi", "300"));
    ASSERT_TRUE(gdk_pixbuf_set_option(raw, "y-dpi", "301"));
    ASSERT_TRUE(gdk_pixbuf_set_option(raw, "orientation", "6"));
    ASSERT_TRUE(gdk_pixbuf_set_option(raw, "unrelated-private-option", "must-not-propagate"));

    Pixbuf copied(*source);
    auto *copied_raw = copied.getPixbufRaw();
    EXPECT_STREQ(gdk_pixbuf_get_option(copied_raw, "icc-profile"), profile_base64);
    EXPECT_STREQ(gdk_pixbuf_get_option(copied_raw, "x-dpi"), "300");
    EXPECT_STREQ(gdk_pixbuf_get_option(copied_raw, "y-dpi"), "301");
    EXPECT_EQ(gdk_pixbuf_get_option(copied_raw, "orientation"), nullptr);
    EXPECT_EQ(gdk_pixbuf_get_option(copied_raw, "unrelated-private-option"), nullptr);

    auto const uri = png_uri(*source);
    ASSERT_FALSE(uri.empty());
    std::unique_ptr<Pixbuf> decoded(Pixbuf::create_from_data_uri(uri.c_str()));
    ASSERT_TRUE(decoded);
    auto *decoded_raw = decoded->getPixbufRaw();
    auto const *roundtrip_profile = gdk_pixbuf_get_option(decoded_raw, "icc-profile");
    ASSERT_TRUE(roundtrip_profile);
    gsize expected_size = 0;
    gsize roundtrip_size = 0;
    auto *expected_bytes = g_base64_decode(profile_base64, &expected_size);
    auto *roundtrip_bytes = g_base64_decode(roundtrip_profile, &roundtrip_size);
    ASSERT_TRUE(expected_bytes);
    ASSERT_TRUE(roundtrip_bytes);
    ASSERT_EQ(roundtrip_size, expected_size);
    EXPECT_EQ(std::memcmp(roundtrip_bytes, expected_bytes, roundtrip_size), 0);
    g_free(expected_bytes);
    g_free(roundtrip_bytes);
    EXPECT_STREQ(gdk_pixbuf_get_option(decoded_raw, "x-dpi"), "300");
    EXPECT_STREQ(gdk_pixbuf_get_option(decoded_raw, "y-dpi"), "301");
    EXPECT_EQ(gdk_pixbuf_get_option(decoded_raw, "orientation"), nullptr);
    EXPECT_EQ(gdk_pixbuf_get_option(decoded_raw, "unrelated-private-option"), nullptr);

    // Exercise metadata through the real cropped allocation and shared encoder.
    auto cropped = BitmapClip::apply(*source, rect_path(1, 0, 2, 2), BitmapClip::FillRule::NonZero,
                                     BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(cropped.completed());
    ASSERT_TRUE(cropped.trimmed());
    auto const cropped_uri = png_uri(*cropped.pixels);
    std::unique_ptr<Pixbuf> cropped_decoded(Pixbuf::create_from_data_uri(cropped_uri.c_str()));
    ASSERT_TRUE(cropped_decoded);
    ASSERT_EQ(cropped_decoded->width(), 1);
    ASSERT_EQ(cropped_decoded->height(), 2);
    auto *cropped_raw = cropped_decoded->getPixbufRaw();
    EXPECT_STREQ(gdk_pixbuf_get_option(cropped_raw, "icc-profile"), roundtrip_profile);
    EXPECT_STREQ(gdk_pixbuf_get_option(cropped_raw, "x-dpi"), "300");
    EXPECT_STREQ(gdk_pixbuf_get_option(cropped_raw, "y-dpi"), "301");
    EXPECT_EQ(gdk_pixbuf_get_option(cropped_raw, "orientation"), nullptr);
    EXPECT_EQ(gdk_pixbuf_get_option(cropped_raw, "unrelated-private-option"), nullptr);
    g_free(profile_base64);
}

// ---------------------------------------------------------------------------
// B01 straight-RGBA outcome tests. The oracle is either the plan-4.1 integer
// formula (alpha_mul) or a decoded output PNG; never a broad image tolerance.
// ---------------------------------------------------------------------------

std::unique_ptr<Pixbuf> decoded_png(Pixbuf const &pixbuf)
{
    auto const uri = png_uri(pixbuf);
    if (uri.empty()) {
        return {};
    }
    std::unique_ptr<Pixbuf> decoded(Pixbuf::create_from_data_uri(uri.c_str()));
    if (decoded) {
        decoded->ensurePixelFormat(Pixbuf::PF_GDK);
    }
    return decoded;
}

TEST(DestructiveBitmapClipPixelsTest, CombinedMaskAndCutterUseExactIntegerAlpha)
{
    constexpr std::array<unsigned char, 6> source_alpha = {0, 1, 127, 128, 254, 255};
    auto source = solid_pixbuf(6, 1, 0);
    ASSERT_TRUE(source);
    for (int x = 0; x < 6; ++x) {
        auto *rgba = pixel(*source, x, 0);
        rgba[0] = static_cast<unsigned char>(10 + 40 * x);
        rgba[1] = static_cast<unsigned char>(20 + 40 * x);
        rgba[2] = static_cast<unsigned char>(30 + 40 * x);
        rgba[3] = source_alpha[x];
    }

    // M is the reviewed combined mask/clip coverage on the source grid.
    std::array<unsigned char, 6> mask = {255, 255, 128, 200, 255, 128};
    BitmapClip::Coverage coverage{mask.data(), 6, 6, 1};
    // Integer-aligned cutter covers columns 2 and 3 completely.
    auto const cutter = rect_path(2, 0, 4, 1);

    auto inside = apply_in_source_grid(*source, coverage, cutter, BitmapClip::FillRule::NonZero,
                                       BitmapClip::Mode::KeepInside);
    auto outside = apply_in_source_grid(*source, coverage, cutter, BitmapClip::FillRule::NonZero,
                                        BitmapClip::Mode::KeepOutside);
    ASSERT_TRUE(inside.completed());
    ASSERT_TRUE(outside.completed());

    for (int x = 0; x < 6; ++x) {
        auto const *original = pixel(*source, x, 0);
        auto const *in = pixel(*inside.pixels, x, 0);
        auto const *out = pixel(*outside.pixels, x, 0);
        auto const base = alpha_mul(source_alpha[x], mask[x]);
        auto const cutter_coverage = (x >= 2 && x < 4) ? 255u : 0u;
        auto const expected_inside = alpha_mul(base, cutter_coverage);
        auto const expected_outside = static_cast<unsigned>(base) - expected_inside;
        EXPECT_EQ(in[3], expected_inside) << "inside alpha at column " << x;
        EXPECT_EQ(out[3], expected_outside) << "outside alpha at column " << x;
        EXPECT_EQ(static_cast<unsigned>(in[3]) + out[3], base) << "complement at column " << x;
        for (int channel = 0; channel < 3; ++channel) {
            EXPECT_EQ(in[channel], in[3] ? original[channel] : 0u)
                << "inside channel " << channel << " at column " << x;
            EXPECT_EQ(out[channel], out[3] ? original[channel] : 0u)
                << "outside channel " << channel << " at column " << x;
        }
    }
}

TEST(DestructiveBitmapClipPixelsTest, MaskRoundingToZeroAlphaEmitsTransparentBlack)
{
    // A == 1 and M == 1 gives floor((1*1 + 127)/255) == 0. The pixel must become
    // transparent black even though the mask sample itself is nonzero.
    auto source = solid_pixbuf(3, 1, 0);
    ASSERT_TRUE(source);
    auto *first = pixel(*source, 0, 0);
    first[0] = 200; first[1] = 100; first[2] = 50; first[3] = 1;
    auto *second = pixel(*source, 1, 0);
    second[0] = 200; second[1] = 100; second[2] = 50; second[3] = 255;
    auto *third = pixel(*source, 2, 0);
    third[0] = 210; third[1] = 110; third[2] = 60; third[3] = 128;

    std::array<unsigned char, 3> mask = {1, 255, 255};
    BitmapClip::Coverage coverage{mask.data(), 3, 3, 1};
    // The cutter covers the whole image, so C == 255 and the mask result shows.
    auto result = apply_in_source_grid(*source, coverage, rect_path(0, 0, 3, 1),
                                       BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(result.completed());
    auto const *zero = pixel(*result.pixels, 0, 0);
    EXPECT_EQ(zero[3], 0u);
    EXPECT_EQ(zero[0], 0u);
    EXPECT_EQ(zero[1], 0u);
    EXPECT_EQ(zero[2], 0u);
    EXPECT_EQ(alpha_at(*result.pixels, 1, 0), 255u);
    EXPECT_EQ(pixel(*result.pixels, 1, 0)[0], 200u);
}

TEST(DestructiveBitmapClipPixelsTest, StraightRgbAndLowAlphaSurviveDecodedPng)
{
    constexpr std::array<unsigned char, 6> source_alpha = {1, 127, 128, 254, 255, 200};
    auto source = solid_pixbuf(6, 1, 0);
    ASSERT_TRUE(source);
    for (int x = 0; x < 6; ++x) {
        auto *rgba = pixel(*source, x, 0);
        rgba[0] = static_cast<unsigned char>(5 + 40 * x);
        rgba[1] = static_cast<unsigned char>(200 - 30 * x);
        rgba[2] = static_cast<unsigned char>(250 - 10 * x);
        rgba[3] = source_alpha[x];
    }

    auto result = BitmapClip::apply(*source, rect_path(0, 0, 6, 1), BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(result.completed());
    EXPECT_FALSE(result.trimmed());
    EXPECT_EQ(result.pixels->pixelFormat(), Pixbuf::PF_GDK);

    auto decoded = decoded_png(*result.pixels);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded->width(), 6);
    ASSERT_EQ(decoded->height(), 1);
    // Full integer coverage: every surviving straight sample must come back
    // byte-identical, especially the alpha == 1 pixel that premultiplication
    // would quantize to black.
    for (int x = 0; x < 6; ++x) {
        for (int channel = 0; channel < 4; ++channel) {
            EXPECT_EQ(pixel(*decoded, x, 0)[channel], pixel(*source, x, 0)[channel])
                << "channel " << channel << " at x=" << x;
        }
    }
}

TEST(DestructiveBitmapClipPixelsTest, PartialCoverageEdgePreservesLowValuedStraightRgb)
{
    // The old premultiplied path multiplied RGB by the cutter coverage, which
    // quantizes low-valued channels at an antialiased edge. This is the direct
    // oracle for that defect; it must fail if RGB is ever coverage-scaled.
    constexpr std::array<unsigned char, 3> rgb = {50, 120, 200};
    auto source = solid_pixbuf(4, 1, 0);
    ASSERT_TRUE(source);
    for (int x = 0; x < 4; ++x) {
        auto *p = pixel(*source, x, 0);
        p[0] = rgb[0];
        p[1] = rgb[1];
        p[2] = rgb[2];
        p[3] = 255;
    }
    auto const cutter = rect_path(0.5, 0, 4, 1);

    for (auto mode : {BitmapClip::Mode::KeepInside, BitmapClip::Mode::KeepOutside}) {
        auto result = BitmapClip::apply(*source, cutter, BitmapClip::FillRule::NonZero, mode);
        ASSERT_TRUE(result.completed());
        auto decoded = decoded_png(*result.pixels);
        ASSERT_TRUE(decoded) << "mode " << static_cast<int>(mode);
        // The first column carries fractional cutter coverage. Its alpha must be
        // partial and its RGB must still equal the source exactly.
        auto const *edge = pixel(*decoded, 0, 0);
        EXPECT_GT(edge[3], 0u) << "mode " << static_cast<int>(mode);
        EXPECT_LT(edge[3], 255u) << "mode " << static_cast<int>(mode);
        for (int channel = 0; channel < 3; ++channel) {
            EXPECT_EQ(edge[channel], rgb[channel])
                << "partial edge channel " << channel << " mode " << static_cast<int>(mode);
        }
    }
}

TEST(DestructiveBitmapClipPixelsTest, AsymmetricColoredDetailsSurviveCutterAndPngDecode)
{
    auto source = solid_pixbuf(5, 4, 0);
    ASSERT_TRUE(source);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 5; ++x) {
            auto *rgba = pixel(*source, x, y);
            rgba[0] = static_cast<unsigned char>(11 + 37 * x + 3 * y);
            rgba[1] = static_cast<unsigned char>(201 - 29 * x);
            rgba[2] = static_cast<unsigned char>(51 + 43 * y);
            rgba[3] = (x == 2 && y == 2) ? 1 : 255; // one-pixel low-alpha detail
        }
    }

    for (auto mode : {BitmapClip::Mode::KeepInside, BitmapClip::Mode::KeepOutside}) {
        auto result = BitmapClip::apply(*source, rect_path(1, 1, 4, 3),
                                        BitmapClip::FillRule::NonZero, mode);
        ASSERT_TRUE(result.completed());
        auto decoded = decoded_png(*result.pixels);
        ASSERT_TRUE(decoded) << "mode " << static_cast<int>(mode);
        ASSERT_EQ(decoded->width(), result.pixels->width());
        ASSERT_EQ(decoded->height(), result.pixels->height());
        for (int y = 0; y < decoded->height(); ++y) {
            for (int x = 0; x < decoded->width(); ++x) {
                auto const *got = pixel(*decoded, x, y);
                auto const *canonical = pixel(*source, x + result.left, y + result.top);
                if (got[3] != 0) {
                    for (int channel = 0; channel < 3; ++channel) {
                        EXPECT_EQ(got[channel], canonical[channel])
                            << "mode " << static_cast<int>(mode) << " channel " << channel
                            << " at (" << x << ", " << y << ')';
                    }
                } else {
                    EXPECT_EQ(got[0], 0u);
                    EXPECT_EQ(got[1], 0u);
                    EXPECT_EQ(got[2], 0u);
                }
            }
        }
    }
}

TEST(DestructiveBitmapClipPixelsTest, TileSeamKeepsExactStraightRgbaAcrossPng)
{
    constexpr int width = 520;
    constexpr int height = 3;
    auto source = solid_pixbuf(width, height, 0);
    ASSERT_TRUE(source);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto *rgba = pixel(*source, x, y);
            rgba[0] = static_cast<unsigned char>(x * 7 + y);
            rgba[1] = static_cast<unsigned char>(255 - (x & 0xff));
            rgba[2] = static_cast<unsigned char>((x * 13) & 0xff);
            rgba[3] = 255;
        }
    }

    auto result = BitmapClip::apply(*source, rect_path(0, 0, width, height),
                                    BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(result.completed());
    EXPECT_FALSE(result.trimmed());
    auto decoded = decoded_png(*result.pixels);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded->width(), width);
    ASSERT_EQ(decoded->height(), height);
    // Two mask tiles are involved across x == 255/256; every sample must match.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            EXPECT_EQ(std::memcmp(pixel(*decoded, x, y), pixel(*source, x, y), 4), 0)
                << "tile-seam sample at (" << x << ", " << y << ')';
        }
    }
}

TEST(DestructiveBitmapClipPixelsTest, HolesAndInversePreserveExactAlphaAndRgb)
{
    auto source = solid_pixbuf(8, 8, 0x33669964);
    ASSERT_TRUE(source);
    auto cutter = rect_path(1, 1, 7, 7);
    cutter.push_back(Geom::Path(Geom::Rect(3, 3, 5, 5)));

    auto inside = BitmapClip::apply(*source, cutter, BitmapClip::FillRule::EvenOdd,
                                    BitmapClip::Mode::KeepInside);
    auto outside = BitmapClip::apply(*source, cutter, BitmapClip::FillRule::EvenOdd,
                                     BitmapClip::Mode::KeepOutside);
    ASSERT_TRUE(inside.completed());
    ASSERT_TRUE(outside.completed());

    auto inside_decoded = decoded_png(*inside.pixels);
    ASSERT_TRUE(inside_decoded);
    for (int y = 0; y < inside_decoded->height(); ++y) {
        for (int x = 0; x < inside_decoded->width(); ++x) {
            auto const *got = pixel(*inside_decoded, x, y);
            int const sx = x + inside.left;
            int const sy = y + inside.top;
            bool const in_hole = sx >= 3 && sx < 5 && sy >= 3 && sy < 5;
            EXPECT_EQ(got[3], in_hole ? 0u : 100u) << "inside alpha at (" << sx << ", " << sy << ')';
            for (int channel = 0; channel < 3; ++channel) {
                EXPECT_EQ(got[channel], in_hole ? 0u : pixel(*source, sx, sy)[channel])
                    << "inside channel " << channel << " at (" << sx << ", " << sy << ')';
            }
        }
    }

    auto outside_decoded = decoded_png(*outside.pixels);
    ASSERT_TRUE(outside_decoded);
    ASSERT_EQ(outside_decoded->width(), 8);
    ASSERT_EQ(outside_decoded->height(), 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            auto const *got = pixel(*outside_decoded, x, y);
            bool const on_ring = ((x >= 1 && x < 7 && y >= 1 && y < 7) &&
                                  !(x >= 3 && x < 5 && y >= 3 && y < 5));
            EXPECT_EQ(got[3], on_ring ? 0u : 100u) << "outside alpha at (" << x << ", " << y << ')';
            for (int channel = 0; channel < 3; ++channel) {
                EXPECT_EQ(got[channel], on_ring ? 0u : pixel(*source, x, y)[channel])
                    << "outside channel " << channel << " at (" << x << ", " << y << ')';
            }
        }
    }
}

TEST(DestructiveBitmapClipPixelsTest, AllTransparentAndDisjointResultsAreBlackWithSourceExtent)
{
    auto source = solid_pixbuf(4, 3, 0x224466ff);
    ASSERT_TRUE(source);

    auto inside = BitmapClip::apply(*source, rect_path(10, 10, 12, 12),
                                    BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(inside.completed());
    EXPECT_TRUE(inside.all_transparent);
    EXPECT_TRUE(inside.changed);
    EXPECT_FALSE(inside.trimmed());
    EXPECT_EQ(inside.pixels->width(), 4);
    EXPECT_EQ(inside.pixels->height(), 3);
    auto decoded = decoded_png(*inside.pixels);
    ASSERT_TRUE(decoded);
    for (int y = 0; y < decoded->height(); ++y) {
        for (int x = 0; x < decoded->width(); ++x) {
            for (int channel = 0; channel < 4; ++channel) {
                EXPECT_EQ(pixel(*decoded, x, y)[channel], 0u) << "at (" << x << ", " << y << ')';
            }
        }
    }

    auto outside = BitmapClip::apply(*source, rect_path(10, 10, 12, 12),
                                     BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepOutside);
    ASSERT_TRUE(outside.completed());
    EXPECT_FALSE(outside.changed);
    EXPECT_FALSE(outside.all_transparent);
    EXPECT_EQ(snapshot(*outside.pixels), snapshot(*source));

    auto transparent = solid_pixbuf(4, 3, 0);
    ASSERT_TRUE(transparent);
    auto empty = BitmapClip::apply(*transparent, rect_path(1, 1, 3, 3),
                                   BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(empty.completed());
    EXPECT_TRUE(empty.all_transparent);
    EXPECT_FALSE(empty.trimmed());
    EXPECT_EQ(empty.pixels->width(), 4);
    EXPECT_EQ(empty.pixels->height(), 3);
}

TEST(DestructiveBitmapClipPixelsTest, ZeroAlphaSourcePixelOutsideCutterBecomesTransparentBlack)
{
    // R01-B01 FINDING-1 regression, independent of coverage and cutter bounds:
    // a source pixel that is already transparent but still carries hidden RGB,
    // outside a limited cutter, was left byte-for-byte unchanged by the pre-fix
    // candidate in KeepOutside. The plan (4.1) requires every final-alpha-zero
    // pixel to be transparent black.
    auto source = solid_pixbuf(5, 1, 0);
    ASSERT_TRUE(source);
    auto *opaque_left = pixel(*source, 0, 0);
    opaque_left[0] = 10; opaque_left[1] = 20; opaque_left[2] = 30; opaque_left[3] = 255;
    auto *hidden_outside = pixel(*source, 1, 0);
    hidden_outside[0] = 200; hidden_outside[1] = 100; hidden_outside[2] = 50; hidden_outside[3] = 0;
    auto *opaque_middle = pixel(*source, 2, 0);
    opaque_middle[0] = 40; opaque_middle[1] = 50; opaque_middle[2] = 60; opaque_middle[3] = 255;
    auto *hidden_in_bounds = pixel(*source, 3, 0);
    hidden_in_bounds[0] = 70; hidden_in_bounds[1] = 80; hidden_in_bounds[2] = 90; hidden_in_bounds[3] = 0;
    auto *opaque_right = pixel(*source, 4, 0);
    opaque_right[0] = 100; opaque_right[1] = 110; opaque_right[2] = 120; opaque_right[3] = 255;
    auto const source_before = snapshot(*source);

    // A limited cutter covers only the rightmost opaque pixel (plus the integer
    // antialias neighborhood). The transparent hidden-color pixel at x == 1 is
    // entirely outside the rendered cutter bounds.
    auto const cutter = rect_path(4, 0, 5, 1);
    auto result = BitmapClip::apply(*source, cutter, BitmapClip::FillRule::NonZero,
                                    BitmapClip::Mode::KeepOutside);
    ASSERT_TRUE(result.completed());
    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(result.all_transparent);
    EXPECT_EQ(result.left, 0);
    EXPECT_EQ(result.top, 0);
    // Only the two surviving opaque pixels define the trimmed extent.
    ASSERT_EQ(result.pixels->width(), 3);
    ASSERT_EQ(result.pixels->height(), 1);
    EXPECT_EQ(snapshot(*source), source_before) << "the caller-owned source must remain untouched";

    // Raw straight-RGBA assertion: this is the defect the pre-fix candidate
    // fails (200,100,50 at alpha 0).
    auto const *raw_hidden = pixel(*result.pixels, 1, 0);
    for (int channel = 0; channel < 4; ++channel) {
        EXPECT_EQ(raw_hidden[channel], 0u)
            << "raw hidden-color pixel channel " << channel << " must be transparent black";
    }
    auto const *raw_left = pixel(*result.pixels, 0, 0);
    EXPECT_EQ(raw_left[0], 10u); EXPECT_EQ(raw_left[1], 20u); EXPECT_EQ(raw_left[2], 30u);
    EXPECT_EQ(raw_left[3], 255u);
    auto const *raw_middle = pixel(*result.pixels, 2, 0);
    EXPECT_EQ(raw_middle[0], 40u); EXPECT_EQ(raw_middle[1], 50u); EXPECT_EQ(raw_middle[2], 60u);
    EXPECT_EQ(raw_middle[3], 255u);

    // And the same contract must survive the real PNG encoder/decoder.
    auto decoded = decoded_png(*result.pixels);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded->width(), 3);
    ASSERT_EQ(decoded->height(), 1);
    for (int channel = 0; channel < 4; ++channel) {
        EXPECT_EQ(pixel(*decoded, 1, 0)[channel], 0u)
            << "decoded hidden-color pixel channel " << channel << " must be transparent black";
    }
    EXPECT_EQ(pixel(*decoded, 0, 0)[0], 10u);
    EXPECT_EQ(pixel(*decoded, 2, 0)[0], 40u);
}

TEST(DestructiveBitmapClipPixelsTest, FullCoverageBufferMatchesThePathOnlyOverload)
{
    auto source = solid_pixbuf(5, 4, 0x8899aacc);
    ASSERT_TRUE(source);
    auto const cutter = triangle_path({0.5, 0.5}, {4.5, 1.25}, {2.25, 3.75});
    std::vector<unsigned char> full(static_cast<std::size_t>(5) * 4, 255);
    BitmapClip::Coverage coverage{full.data(), 5, 5, 4};

    auto path_only = BitmapClip::apply(*source, cutter, BitmapClip::FillRule::NonZero,
                                       BitmapClip::Mode::KeepInside);
    auto with_full = BitmapClip::apply(*source, coverage, cutter, BitmapClip::FillRule::NonZero,
                                       BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(path_only.completed());
    ASSERT_TRUE(with_full.completed());
    EXPECT_EQ(path_only.changed, with_full.changed);
    EXPECT_EQ(path_only.all_transparent, with_full.all_transparent);
    EXPECT_EQ(path_only.left, with_full.left);
    EXPECT_EQ(path_only.top, with_full.top);
    EXPECT_EQ(snapshot(*path_only.pixels), snapshot(*with_full.pixels));
}

TEST(DestructiveBitmapClipPixelsTest, CairoFormattedSourceIsConvertedOnceToStraightRgba)
{
    // Documents the one unavoidable conversion: if canonical storage is already
    // premultiplied, the engine converts once to straight RGBA and thereafter
    // preserves those straight samples exactly. It never promises original-file
    // identity beyond what the decoder retained.
    auto straight_source = solid_pixbuf(4, 1, 0);
    ASSERT_TRUE(straight_source);
    constexpr std::array<unsigned char, 4> alphas = {1, 128, 255, 64};
    for (int x = 0; x < 4; ++x) {
        auto *rgba = pixel(*straight_source, x, 0);
        rgba[0] = static_cast<unsigned char>(30 + 50 * x);
        rgba[1] = static_cast<unsigned char>(220 - 40 * x);
        rgba[2] = static_cast<unsigned char>(90 + 20 * x);
        rgba[3] = alphas[x];
    }
    Pixbuf cairo_source(*straight_source);
    cairo_source.ensurePixelFormat(Pixbuf::PF_CAIRO);
    Pixbuf canonical_straight(cairo_source);
    canonical_straight.ensurePixelFormat(Pixbuf::PF_GDK);

    auto result = BitmapClip::apply(cairo_source, rect_path(0, 0, 4, 1),
                                    BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(result.completed());
    EXPECT_EQ(result.pixels->pixelFormat(), Pixbuf::PF_GDK);
    EXPECT_EQ(snapshot(*result.pixels), snapshot(canonical_straight));
}

TEST(DestructiveBitmapClipPixelsTest, CoverageValidationRejectsMismatchedGrid)
{
    auto source = solid_pixbuf(6, 1, 0xffffffff);
    ASSERT_TRUE(source);
    std::array<unsigned char, 6> mask{};
    mask.fill(128);

    BitmapClip::Coverage bad_dims{mask.data(), 6, 3, 1};
    auto dims = BitmapClip::apply(*source, bad_dims, rect_path(0, 0, 6, 1),
                                  BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepInside);
    EXPECT_EQ(dims.status, BitmapClip::Status::InvalidInput);
    EXPECT_FALSE(dims.pixels);

    BitmapClip::Coverage bad_stride{mask.data(), 3, 6, 1};
    auto stride = BitmapClip::apply(*source, bad_stride, rect_path(0, 0, 6, 1),
                                    BitmapClip::FillRule::NonZero, BitmapClip::Mode::KeepInside);
    EXPECT_EQ(stride.status, BitmapClip::Status::InvalidInput);
    EXPECT_FALSE(stride.pixels);
}

class DestructiveBitmapClipActionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const *datadir = g_getenv("INKSCAPE_DATADIR");
        ASSERT_TRUE(datadir && *datadir)
            << "Run this integration test through CTest so INKSCAPE_DATADIR is configured";
        auto const units = std::string(datadir) + "/inkscape/ui/units.xml";
        ASSERT_TRUE(g_file_test(units.c_str(), G_FILE_TEST_IS_REGULAR))
            << "INKSCAPE_DATADIR does not contain the test share tree";

        application = initialize_application();
        ASSERT_TRUE(application);
        if (!Application::exists()) {
            Application::create(false);
        }

        auto source = solid_pixbuf(8, 8, 0x336699ff);
        ASSERT_TRUE(source);
        source_uri = png_uri(*source);
        ASSERT_FALSE(source_uri.empty());
        auto const svg = Glib::ustring::compose(
            R"svg(<svg xmlns="http://www.w3.org/2000/svg"
 xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="20" height="12">
 <defs>
  <clipPath id="user-clip" clipPathUnits="userSpaceOnUse"><rect x="0" y="0" width="20" height="20"/></clipPath>
  <clipPath id="box-clip" clipPathUnits="objectBoundingBox"><rect width="1" height="1"/></clipPath>
  <mask id="user-mask" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse"
        x="0" y="0" width="20" height="20"><rect width="20" height="20" fill="white"/></mask>
  <mask id="box-mask"><rect width="20" height="20" fill="white"/></mask>
  <filter id="adjustment"><feColorMatrix type="saturate" values="0.5"/></filter>
  <filter id="user-filter" filterUnits="userSpaceOnUse" x="0" y="0" width="20" height="20">
   <feGaussianBlur stdDeviation="0.1"/>
  </filter>
 </defs>
 <g id="outer"><g id="parent">
 <image id="bitmap" x="1" y="1" width="8" height="8" preserveAspectRatio="none"
        sodipodi:absref="/tmp/original-linked.png" href="%1"/>
 </g></g>
 <rect id="cutter" x="3" y="3" width="4" height="4" fill="#ff00ff"
       transform="translate(0.25 0.25)"/>
</svg>)svg",
            source_uri);
        document = SPDocument::createNewDocFromMem(svg.raw());
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        ASSERT_TRUE(desktop);
        INKSCAPE.add_desktop(desktop.get());
        desktop_registered = true;
        application->set_active_desktop(desktop.get());
        ASSERT_TRUE(image());
        ASSERT_TRUE(cutter());
        desktop->getSelection()->setList(std::vector<SPItem *>{image(), cutter()});

        application->set_active_document(document.get());
        application->set_active_selection(desktop->getSelection());
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Initialize destructive clip fixture"},
                           "document-new");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
    }

    void TearDown() override
    {
        if (application) {
            application->set_active_selection(nullptr);
            application->set_active_document(nullptr);
            application->set_active_desktop(nullptr);
        }
        if (desktop_registered && desktop && Application::exists()) {
            INKSCAPE.remove_desktop(desktop.get());
            desktop_registered = false;
        }
        desktop.reset();
        document.reset();
    }

    SPImage *image() const
    {
        return document ? cast<SPImage>(document->getObjectById("bitmap")) : nullptr;
    }

    SPItem *cutter() const
    {
        return document ? cast<SPItem>(document->getObjectById("cutter")) : nullptr;
    }

    std::string href() const
    {
        auto const value = image() ? getHrefAttribute(*image()->getRepr()).second : nullptr;
        return value ? value : "";
    }

    void activate_normal_clip()
    {
        application->gio_app()->activate_action("object-destructive-clip");
        document->ensureUpToDate();
    }

    void activate_inverse_clip()
    {
        application->gio_app()->activate_action("object-destructive-inverse-clip");
        document->ensureUpToDate();
    }

    // B04 transaction entry point. The focused-link harness binds the gio
    // action dispatch above to the pre-B04 chemistry embedded in
    // libinkscape_base, so transaction semantics are exercised through the
    // directly linked chemistry translation unit.
    BitmapClip::CommitStatus commit_clip(BitmapClip::Mode mode)
    {
        auto status = BitmapClip::commit_selection(*desktop->getSelection(), mode);
        document->ensureUpToDate();
        return status;
    }

    BitmapClip::CommitStatus commit_clip(BitmapClip::Mode mode, std::stop_token cancellation)
    {
        auto status = BitmapClip::commit_selection(*desktop->getSelection(), mode, cancellation);
        document->ensureUpToDate();
        return status;
    }

    // Rebuild the two-role selection after an Undo. Undo/Redo do not restore
    // selection through the existing XML
    // primitives; the contract test asserts that explicitly.
    void reselect_pair()
    {
        std::vector<SPItem *> items;
        if (auto *image_item = image()) {
            items.push_back(image_item);
        }
        if (auto *cutter_item = cutter()) {
            items.push_back(cutter_item);
        }
        select(items);
    }

    bool cutter_is_consumed() const
    {
        return document && document->getObjectById("cutter") == nullptr;
    }

    // Parent, stacking position and every attribute of the cutter (CLIP-1: the
    // clip leaves it exactly where and as it was).
    std::string cutter_state() const
    {
        auto *item = cutter();
        if (!item) {
            return "<absent>";
        }
        auto *repr = item->getRepr();
        std::string state = std::string(repr->parent() && repr->parent()->attribute("id") ? repr->parent()->attribute("id") : "?") +
                            "#" + std::to_string(repr->position());
        for (auto const &attribute : repr->attributeList()) {
            state += std::string(" ") + g_quark_to_string(attribute.key) + "=" + attribute.value.pointer();
        }
        return state;
    }

    bool action_enabled(char const *name) const
    {
        auto action = application->gio_app()->lookup_action(name);
        return action && action->get_enabled();
    }

    void checkpoint()
    {
        document->ensureUpToDate();
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Prepare trim fixture"},
                           "object-properties");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
    }

    void place_pixels(Pixbuf const &source, double x, double y, double width, double height,
                      char const *aspect = "none")
    {
        auto *repr = image()->getRepr();
        setHrefAttribute(*repr, png_uri(source).c_str());
        repr->setAttributeSvgDouble("x", x);
        repr->setAttributeSvgDouble("y", y);
        repr->setAttributeSvgDouble("width", width);
        repr->setAttributeSvgDouble("height", height);
        repr->setAttribute("preserveAspectRatio", aspect);
        document->ensureUpToDate();
    }

    void cutter_in_pixels(double x, double y, double width, double height)
    {
        auto *repr = cutter()->getRepr();
        repr->removeAttribute("transform");
        repr->setAttributeSvgDouble("x", x);
        repr->setAttributeSvgDouble("y", y);
        repr->setAttributeSvgDouble("width", width);
        repr->setAttributeSvgDouble("height", height);
        document->ensureUpToDate();
        auto const mapping = image()->pixelToDocumentAffine();
        ASSERT_TRUE(mapping);
        auto transform = sp_svg_transform_write(*mapping * cutter()->i2doc_affine().inverse());
        repr->setAttribute("transform", transform);
        document->ensureUpToDate();
    }

    void expect_mapping(Geom::Affine const &before, int left, int top)
    {
        auto const after = image()->pixelToDocumentAffine();
        ASSERT_TRUE(after);
        for (auto const point : {Geom::Point(0.5, 0.5), Geom::Point(image()->pixbuf->width() - 0.5, 0.5),
                                 Geom::Point(0.5, image()->pixbuf->height() - 0.5),
                                 Geom::Point(image()->pixbuf->width() - 0.5, image()->pixbuf->height() - 0.5)}) {
            auto const expected = (point + Geom::Point(left, top)) * before;
            auto const actual = point * *after;
            EXPECT_NEAR(actual.x(), expected.x(), 1e-7);
            EXPECT_NEAR(actual.y(), expected.y(), 1e-7);
        }
    }

    SPItem *object(char const *id) const
    {
        return document ? cast<SPItem>(document->getObjectById(id)) : nullptr;
    }

    BitmapClip::ResolvedTargets resolve() const
    {
        return BitmapClip::resolve_targets(*desktop->getSelection());
    }

    void select(std::vector<SPItem *> items)
    {
        desktop->getSelection()->setList(items);
        document->ensureUpToDate();
        drain_main_context();
    }

    // Resolution is read-only: assert the document XML, the selected object
    // pointers and the undo history are unchanged, then return the outcome.
    BitmapClip::ResolvedTargets resolve_and_expect_unchanged()
    {
        auto const before_xml = sp_repr_save_buf(document->getReprDoc()).raw();
        auto const before_selection = desktop->getSelection()->items_vector();
        auto const result = resolve();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before_xml);
        EXPECT_EQ(desktop->getSelection()->items_vector(), before_selection);
        EXPECT_FALSE(DocumentUndo::undo(document.get())) << "resolution must not create undo history";
        return result;
    }

    // Detached data-URI image node, not yet parented.
    XML::Node *make_image_node(char const *id, double x, double y)
    {
        auto *node = document->getReprDoc()->createElement("svg:image");
        node->setAttribute("id", id);
        node->setAttributeSvgDouble("x", x);
        node->setAttributeSvgDouble("y", y);
        node->setAttributeSvgDouble("width", 8);
        node->setAttributeSvgDouble("height", 8);
        node->setAttribute("preserveAspectRatio", "none");
        setHrefAttribute(*node, source_uri.c_str());
        return node;
    }

    XML::Node *make_rect_node(char const *id, double x, double y, double width, double height)
    {
        auto *node = document->getReprDoc()->createElement("svg:rect");
        node->setAttribute("id", id);
        node->setAttributeSvgDouble("x", x);
        node->setAttributeSvgDouble("y", y);
        node->setAttributeSvgDouble("width", width);
        node->setAttributeSvgDouble("height", height);
        node->setAttribute("fill", "#336699");
        return node;
    }

    XML::Node *make_group(char const *id)
    {
        auto *group = document->getReprDoc()->createElement("svg:g");
        group->setAttribute("id", id);
        return group;
    }

    void append_to_root(XML::Node *node)
    {
        document->getReprRoot()->appendChild(node);
        Inkscape::GC::release(node);
        document->ensureUpToDate();
    }

    // Append a child to a detached group and give up the createElement ref.
    void append_child(XML::Node *parent, XML::Node *child)
    {
        parent->appendChild(child);
        Inkscape::GC::release(child);
    }

    InkscapeApplication *application = nullptr;
    bool desktop_registered = false;
    std::string source_uri;
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
};

TEST_F(DestructiveBitmapClipActionTest, TransactionReplacesBitmapKeepsCutterAndCreatesExactlyOneUndo)
{
    EXPECT_TRUE(action_enabled("object-destructive-clip"));
    EXPECT_TRUE(action_enabled("object-destructive-inverse-clip"));
    auto const href_before = href();
    auto const absref_before = std::string(image()->getRepr()->attribute("sodipodi:absref"));
    auto const image_x = std::string(image()->getRepr()->attribute("x"));
    auto const image_width = std::string(image()->getRepr()->attribute("width"));

    auto const cutter_before = cutter_state();
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);

    ASSERT_NE(href(), href_before);
    EXPECT_EQ(href().find("data:image/png;base64,"), 0u);
    EXPECT_EQ(image()->getRepr()->attribute("sodipodi:absref"), nullptr);
    EXPECT_DOUBLE_EQ(image()->x.computed, 3);
    EXPECT_DOUBLE_EQ(image()->width.computed, 5);
    // CLIP-1: the cutter stays exactly where and as it was; the bitmap is the
    // sole selection target.
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_EQ(cutter_state(), cutter_before);
    EXPECT_EQ(desktop->getSelection()->items_vector(),
              std::vector<SPItem *>{static_cast<SPItem *>(image())});
    ASSERT_TRUE(image()->pixbuf);
    EXPECT_GT(alpha_at(*image()->pixbuf, 0, 0), 0u);
    EXPECT_GT(alpha_at(*image()->pixbuf, 4, 4), 0u);
    EXPECT_EQ(image()->pixbuf->width(), 5);
    EXPECT_EQ(image()->pixbuf->height(), 5);
    auto const committed = sp_repr_save_buf(document->getReprDoc()).raw();

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), href_before);
    EXPECT_STREQ(image()->getRepr()->attribute("sodipodi:absref"), absref_before.c_str());
    EXPECT_STREQ(image()->getRepr()->attribute("x"), image_x.c_str());
    EXPECT_STREQ(image()->getRepr()->attribute("width"), image_width.c_str());
    EXPECT_FALSE(cutter_is_consumed());
    // The existing XML-only Undo primitive restores the cutter node but not the
    // pre-command selection; the surviving bitmap stays selected.
    EXPECT_TRUE(desktop->getSelection()->includes(image()));
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "one action must create exactly one Undo entry";

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_NE(href(), href_before);
    EXPECT_GT(alpha_at(*image()->pixbuf, 0, 0), 0u);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), committed);
}

TEST_F(DestructiveBitmapClipActionTest, TransactionCompletesBeforeReturningToActionSequenceOrExport)
{
    auto const href_before = href();

    // No main-context drain or ensureUpToDate before the check: a following CLI
    // action, save or auto-export must already observe the replacement pixels.
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);

    auto const href_observed_by_next_step = href();
    ASSERT_NE(href_observed_by_next_step, href_before);
    EXPECT_EQ(href_observed_by_next_step.find("data:image/png;base64,"), 0u);
    ASSERT_TRUE(image()->pixbuf);
    EXPECT_GT(alpha_at(*image()->pixbuf, 0, 0), 0u);
    EXPECT_EQ(image()->pixbuf->width(), 5);
    EXPECT_DOUBLE_EQ(image()->width.computed, 5);
    EXPECT_GT(alpha_at(*image()->pixbuf, 4, 4), 0u);
    EXPECT_FALSE(cutter_is_consumed());

    // Main-loop delivery must not contain a hidden late mutation.
    drain_main_context();
    EXPECT_EQ(href(), href_observed_by_next_step);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), href_before);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, ImmediateUndoTargetsClipAndPreservesPriorHistory)
{
    auto const href_before = href();
    cutter()->getRepr()->setAttribute("data-vacards-prior-edit", "preserved");
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Prior user edit"}, "object-properties");

    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    ASSERT_NE(href(), href_before);
    EXPECT_FALSE(cutter_is_consumed());

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), href_before);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_STREQ(cutter()->getRepr()->attribute("data-vacards-prior-edit"), "preserved");

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(cutter()->getRepr()->attribute("data-vacards-prior-edit"), nullptr);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, RapidSequentialActivationCannotQueuePixelWork)
{
    for (unsigned iteration = 0; iteration < 12; ++iteration) {
        SCOPED_TRACE(iteration);
        auto const href_before = href();
        auto const mode = iteration % 2 == 0 ? BitmapClip::Mode::KeepInside : BitmapClip::Mode::KeepOutside;

        ASSERT_EQ(commit_clip(mode), BitmapClip::CommitStatus::Committed);
        auto const committed_href = href();
        ASSERT_NE(committed_href, href_before);
        EXPECT_FALSE(cutter_is_consumed());

        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
        EXPECT_EQ(href(), href_before);
        EXPECT_FALSE(cutter_is_consumed());
        EXPECT_FALSE(DocumentUndo::undo(document.get()));

        // Redo must replay the same replacement payload, not a stale one.
        ASSERT_TRUE(DocumentUndo::redo(document.get()));
        document->ensureUpToDate();
        EXPECT_EQ(href(), committed_href);
        EXPECT_FALSE(cutter_is_consumed());
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
        EXPECT_EQ(href(), href_before);

        // Explicit reselection is required: a successful transaction leaves
        // only the bitmap selected.
        reselect_pair();

        // If a worker or queued delivery survived activation, this would
        // reapply stale pixels after Undo. The synchronous model has neither.
        drain_main_context();
        EXPECT_EQ(href(), href_before);
    }
}

TEST_F(DestructiveBitmapClipActionTest, InvalidSelectionAndNoOpDoNotCreateUndo)
{
    auto const href_before = href();
    desktop->getSelection()->set(image());
    EXPECT_FALSE(action_enabled("object-destructive-clip"));
    EXPECT_FALSE(action_enabled("object-destructive-inverse-clip"));
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidSelection);
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::InvalidSelection);
    EXPECT_EQ(href(), href_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    cutter()->getRepr()->setAttribute("x", "-10");
    cutter()->getRepr()->setAttribute("y", "-10");
    cutter()->getRepr()->setAttribute("width", "30");
    cutter()->getRepr()->setAttribute("height", "30");
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Prepare containing cutter"},
                       "object-properties");
    DocumentUndo::clearUndo(document.get());
    DocumentUndo::clearRedo(document.get());
    document->setModifiedSinceSave(false);
    desktop->getSelection()->setList(std::vector<SPItem *>{image(), cutter()});
    EXPECT_TRUE(action_enabled("object-destructive-clip"));
    EXPECT_TRUE(action_enabled("object-destructive-inverse-clip"));

    // CLIP-1: the cutter is kept, so a cutter that fully contains the image
    // changes nothing: a legitimate no-op without an Undo step.
    auto const unchanged = sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::NoChange);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), unchanged);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, ActionSensitivityTracksSelectedObjectModifications)
{
    ASSERT_TRUE(action_enabled("object-destructive-clip"));
    ASSERT_TRUE(action_enabled("object-destructive-inverse-clip"));

    cutter()->getRepr()->setAttribute("width", "0");
    document->ensureUpToDate();
    drain_main_context();
    EXPECT_FALSE(action_enabled("object-destructive-clip"));
    EXPECT_FALSE(action_enabled("object-destructive-inverse-clip"));

    cutter()->getRepr()->setAttribute("width", "4");
    document->ensureUpToDate();
    drain_main_context();
    EXPECT_TRUE(action_enabled("object-destructive-clip"));
    EXPECT_TRUE(action_enabled("object-destructive-inverse-clip"));

    cutter()->setLocked(true);
    document->ensureUpToDate();
    drain_main_context();
    EXPECT_FALSE(action_enabled("object-destructive-clip"));
    EXPECT_FALSE(action_enabled("object-destructive-inverse-clip"));

    cutter()->setLocked(false);
    document->ensureUpToDate();
    drain_main_context();
    EXPECT_TRUE(action_enabled("object-destructive-clip"));
    EXPECT_TRUE(action_enabled("object-destructive-inverse-clip"));

    cutter()->setHidden(true);
    document->ensureUpToDate();
    drain_main_context();
    EXPECT_FALSE(action_enabled("object-destructive-clip"));
    EXPECT_FALSE(action_enabled("object-destructive-inverse-clip"));

    cutter()->setHidden(false);
    document->ensureUpToDate();
    drain_main_context();
    EXPECT_TRUE(action_enabled("object-destructive-clip"));
    EXPECT_TRUE(action_enabled("object-destructive-inverse-clip"));
}

TEST_F(DestructiveBitmapClipActionTest, ClipObserversSeeCompleteGeometryAndPayloadTuple)
{
    cutter_in_pixels(-1, 6, 10, 3);
    checkpoint();
    auto original = xml_for_bitmap_undo(document->getReprDoc());
    struct Observer final : XML::NodeObserver {
        unsigned calls = 0;
        void notifyAttributeChanged(XML::Node &node, GQuark, Util::ptr_shared, Util::ptr_shared) override {
            ++calls;
            EXPECT_STREQ(node.attribute("height"), "6");
            EXPECT_STREQ(node.attribute("preserveAspectRatio"), "none");
            EXPECT_EQ(node.attribute("sodipodi:absref"), nullptr);
            auto href = getHrefAttribute(node).second;
            ASSERT_TRUE(href);
            std::unique_ptr<Pixbuf> pixels(Pixbuf::create_from_data_uri(href));
            ASSERT_TRUE(pixels);
            EXPECT_EQ(pixels->width(), 8);
            EXPECT_EQ(pixels->height(), 6);
        }
    } observer;
    auto node = image()->getRepr();
    node->addObserver(observer);
    auto result = BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepOutside);
    node->removeObserver(observer);
    EXPECT_EQ(result, BitmapClip::CommitStatus::Committed);
    EXPECT_GE(observer.calls, 3u);
    auto cropped = sp_repr_save_buf(document->getReprDoc()).raw();
    ASSERT_TRUE(DocumentUndo::undo(document.get())); document->ensureUpToDate();
    EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), original);
    ASSERT_TRUE(DocumentUndo::redo(document.get())); document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), cropped);
}

TEST_F(DestructiveBitmapClipActionTest, IndependentUntrimmedAlphaRenderOracleAtFractionalZoom)
{
    for (auto const *rendering : {"auto", "pixelated"}) {
        SCOPED_TRACE(rendering);
        for (auto mode : {BitmapClip::Mode::KeepInside, BitmapClip::Mode::KeepOutside}) {
            SCOPED_TRACE(mode == BitmapClip::Mode::KeepInside ? "normal" : "inverse");
            setHrefAttribute(*image()->getRepr(), source_uri.c_str());
            image()->getRepr()->setAttribute("style", (std::string("image-rendering:") + rendering).c_str());
            image()->getRepr()->setAttribute("x", "1");
            image()->getRepr()->setAttribute("y", "1");
            image()->getRepr()->setAttribute("width", "8");
            image()->getRepr()->setAttribute("height", "8");
            auto const normal = mode == BitmapClip::Mode::KeepInside;
            cutter_in_pixels(-1, normal ? -1 : 6, 10, normal ? 7 : 3);
            checkpoint();
            auto oracle = SPDocument::createNewDocFromMem(sp_repr_save_buf(document->getReprDoc()).raw());
            ASSERT_TRUE(oracle);
            auto expected_pixels = untrimmed_rectangle_oracle(-1, normal ? -1 : 6, 9, normal ? 6 : 9, mode);
            ASSERT_TRUE(expected_pixels);
            auto reference = cast<SPImage>(oracle->getObjectById("bitmap"));
            ASSERT_TRUE(reference);
            setHrefAttribute(*reference->getRepr(), png_uri(*expected_pixels).c_str());
            oracle->ensureUpToDate();
            ASSERT_EQ(reference->pixbuf->height(), 8);
            ASSERT_DOUBLE_EQ(reference->height.computed, 8);
            ASSERT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), mode), BitmapClip::CommitStatus::Committed);
            ASSERT_EQ(image()->pixbuf->height(), 6);
            for (double zoom : {0.75, 1.0, 1.375, 2.25}) {
                SCOPED_TRACE(zoom);
                auto expected = render_oracle_grid(*oracle, zoom);
                EXPECT_TRUE(same_render(expected, render_oracle_grid(*document, zoom)));
                // Positive control: the comparison must catch a registration error.
                reference->getRepr()->setAttribute("x", "2");
                EXPECT_FALSE(same_render(expected, render_oracle_grid(*oracle, zoom)));
                reference->getRepr()->setAttribute("x", "1");
            }
            // Undo the transaction before the next setup and explicitly rebuild
            // the two-role selection (only the bitmap stays selected).
            ASSERT_TRUE(DocumentUndo::undo(document.get()));
            document->ensureUpToDate();
            reselect_pair();
        }
    }
}

TEST_F(DestructiveBitmapClipActionTest, UserBottomStripMatchesNormalCropAndRoundTrips)
{
    auto source = solid_pixbuf(1000, 1500, 0x336699ff);
    ASSERT_TRUE(source);
    // Ten by fifteen inches at the SVG 96 user units per inch.
    place_pixels(*source, 17, 23, 960, 1440);
    cutter_in_pixels(-100, 1300, 1200, 300);
    checkpoint();
    auto const original_href = href();
    auto const mapping = *image()->pixelToDocumentAffine();
    auto const cutter_transform = std::string(cutter()->getRepr()->attribute("transform"));
    EXPECT_STREQ(cutter()->getRepr()->attribute("transform"), cutter_transform.c_str());
    EXPECT_DOUBLE_EQ(cutter()->getRepr()->getAttributeDouble("y"), 1300);
    auto const cutter_before = cutter_state();

    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepOutside), BitmapClip::CommitStatus::Committed);
    // CLIP-1: Inverse Clip keeps the cutter too, and selects only the bitmap.
    EXPECT_EQ(cutter_state(), cutter_before);
    EXPECT_EQ(desktop->getSelection()->items_vector(),
              std::vector<SPItem *>{static_cast<SPItem *>(image())});
    auto const inverse_href = href();
    ASSERT_EQ(image()->pixbuf->width(), 1000);
    ASSERT_EQ(image()->pixbuf->height(), 1300);
    EXPECT_DOUBLE_EQ(image()->x.computed, 17);
    EXPECT_DOUBLE_EQ(image()->y.computed, 23);
    EXPECT_NEAR(image()->width.computed, 960, 1e-7);
    EXPECT_NEAR(image()->height.computed, 1248, 1e-7);
    expect_mapping(mapping, 0, 0);
    EXPECT_FALSE(cutter_is_consumed());
    auto const retained = snapshot(*image()->pixbuf);
    Pixbuf canonical(*source);
    canonical.ensurePixelFormat(Pixbuf::PF_CAIRO);
    for (int y = 0; y < 1300; ++y) {
        ASSERT_EQ(std::memcmp(pixel(*image()->pixbuf, 0, y), pixel(canonical, 0, y), 4000), 0);
    }
    desktop->getSelection()->set(image());
    auto const bounds = desktop->getSelection()->geometricBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 960, 1e-7);
    EXPECT_NEAR(bounds->height(), 1248, 1e-7);

    auto saved = sp_repr_save_buf(document->getReprDoc()).raw();
    auto reopened = SPDocument::createNewDocFromMem(saved);
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto *reloaded = cast<SPImage>(reopened->getObjectById("bitmap"));
    ASSERT_TRUE(reloaded);
    ASSERT_TRUE(reloaded->pixbuf);
    EXPECT_EQ(reloaded->pixbuf->height(), 1300);
    EXPECT_EQ(snapshot(*reloaded->pixbuf), retained);
    EXPECT_NEAR(reloaded->height.computed, 1248, 1e-7);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), original_href);
    EXPECT_EQ(image()->pixbuf->height(), 1500);
    EXPECT_DOUBLE_EQ(image()->height.computed, 1440);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), inverse_href);
    expect_mapping(mapping, 0, 0);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    desktop->getSelection()->setList(std::vector<SPItem *>{image(), cutter()});
    cutter_in_pixels(0, 0, 1000, 1300);
    checkpoint();
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    EXPECT_EQ(snapshot(*image()->pixbuf), retained);
    EXPECT_NEAR(image()->height.computed, 1248, 1e-7);
    expect_mapping(mapping, 0, 0);
    EXPECT_FALSE(cutter_is_consumed());
}

TEST_F(DestructiveBitmapClipActionTest, CropMappingSurvivesTransformsAspectModesIdleAndUndoRedo)
{
    auto source = solid_pixbuf(20, 16, 0x55aa33ff);
    ASSERT_TRUE(source);
    auto *parent = document->getObjectById("parent")->getRepr();
    auto *outer = document->getObjectById("outer")->getRepr();
    outer->setAttribute("transform", "matrix(0 1 -1 0 11.25 -7.5)");
    parent->setAttribute("transform", "matrix(-1.25 0 0 0.8 8 12)");
    image()->getRepr()->setAttribute("transform", "matrix(0.75 0 0 1.1 3 -4)");
    for (auto const *aspect : {"none", "xMinYMin meet", "xMidYMid meet", "xMaxYMax meet",
                               "xMinYMin slice", "xMidYMid slice", "xMaxYMax slice"}) {
        SCOPED_TRACE(aspect);
        place_pixels(*source, 1.25, 2.75, 50, 32, aspect);
        // Interior source rectangle lies within every tested meet/slice viewport.
        cutter_in_pixels(5, 4, 8, 6);
        checkpoint();
        auto const before = xml_for_bitmap_undo(document->getReprDoc());
        auto const mapping = *image()->pixelToDocumentAffine();
        {   // Fail loudly if the fixture ever becomes oblique: this test is about the exact path.
            double big = std::max({std::abs(mapping[0]), std::abs(mapping[1]), std::abs(mapping[2]), std::abs(mapping[3])});
            ASSERT_TRUE(std::max(std::abs(mapping[1]), std::abs(mapping[2])) <= 1e-9 * big ||
                        std::max(std::abs(mapping[0]), std::abs(mapping[3])) <= 1e-9 * big);
        }
        auto const original_pixels = image()->pixbuf;
        ASSERT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
                  BitmapClip::CommitStatus::Committed);
        ASSERT_EQ(image()->pixbuf->width(), 8);
        ASSERT_EQ(image()->pixbuf->height(), 6);
        expect_mapping(mapping, 5, 4);
        for (int y = 0; y < 6; ++y) {
            EXPECT_EQ(std::memcmp(pixel(*image()->pixbuf, 0, y), pixel(*original_pixels, 5, y + 4), 32), 0);
        }
        auto const committed = sp_repr_save_buf(document->getReprDoc()).raw();
        drain_main_context();
        document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), committed);
        for (int repeat = 0; repeat < 3; ++repeat) {
            ASSERT_TRUE(DocumentUndo::undo(document.get()));
            document->ensureUpToDate();
            EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), before);
            expect_mapping(mapping, 0, 0);
            ASSERT_TRUE(DocumentUndo::redo(document.get()));
            document->ensureUpToDate();
            EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), committed);
            expect_mapping(mapping, 5, 4);
        }
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
        // The transaction consumed the cutter; Undo restored it but not the
        // selection. Rebuild the two-role target before the next aspect.
        reselect_pair();
    }
}

TEST_F(DestructiveBitmapClipActionTest, PendingTrimAndIneligibleInvocationPreservesRedo)
{
    auto source = solid_pixbuf(8, 8, 0);
    ASSERT_TRUE(source);
    for (int y = 2; y < 6; ++y) {
        for (int x = 1; x < 7; ++x) {
            std::fill_n(pixel(*source, x, y), 4, 255);
        }
    }
    place_pixels(*source, 3, 4, 16, 24);
    cutter_in_pixels(-1, -1, 10, 10);
    checkpoint();
    auto const mapping = *image()->pixelToDocumentAffine();
    auto const old_href = href();
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    ASSERT_NE(href(), old_href);
    EXPECT_EQ(image()->pixbuf->width(), 6);
    EXPECT_EQ(image()->pixbuf->height(), 4);
    expect_mapping(mapping, 1, 2);
    EXPECT_FALSE(cutter_is_consumed());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    reselect_pair();

    // Create an unrelated Redo branch above the successful pending trim.
    cutter()->getRepr()->setAttribute("data-redo", "kept");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Redo sentinel"}, "object-properties");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    reselect_pair();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    document->setModifiedSinceSave(false);
    // A one-image selection is ineligible; the ineligible invocation must not
    // touch XML, the modified flag or the existing Redo branch.
    select({static_cast<SPItem *>(image())});
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::InvalidSelection);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    reselect_pair();
    EXPECT_STREQ(cutter()->getRepr()->attribute("data-redo"), "kept");
}

TEST_F(DestructiveBitmapClipActionTest, HrefOnlyReplacementKeepsLegacyAspectCorrection)
{
    auto replacement = solid_pixbuf(8, 4, 0x112233ff);
    ASSERT_TRUE(replacement);
    // No dimension attributes are set here: replacing an image alone retains
    // the established intrinsic-size heuristic, even with aspect="none".
    setHrefAttribute(*image()->getRepr(), png_uri(*replacement).c_str());
    document->ensureUpToDate();
    EXPECT_DOUBLE_EQ(image()->width.computed, 8);
    EXPECT_DOUBLE_EQ(image()->height.computed, 4);
}

TEST_F(DestructiveBitmapClipActionTest, GeometryAndHrefReplacementDoesNotForceProportions)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto replacement = solid_pixbuf(8, 4, 0x112233ff);
    ASSERT_TRUE(replacement);
    place_pixels(*replacement, 1, 1, 8, 3);
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Explicit image dimensions"}, "object-properties");
    EXPECT_DOUBLE_EQ(image()->width.computed, 8);
    EXPECT_DOUBLE_EQ(image()->height.computed, 3);
    auto const after = sp_repr_save_buf(document->getReprDoc()).raw();
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after);
    EXPECT_DOUBLE_EQ(image()->height.computed, 3);
}

TEST_F(DestructiveBitmapClipActionTest, UserSpaceClipIsBakedDetachedAndSharedResourceKept)
{
    auto *resource = document->getObjectById("user-clip")->getRepr();
    auto *clip_child = resource->firstChild();
    ASSERT_TRUE(clip_child);
    auto *image_repr = image()->getRepr();
    image_repr->setAttribute("clip-path", "url(#user-clip)");
    // A second, unselected user of the same clip proves the shared resource and
    // its reference are preserved while the converted image detaches its own.
    auto *sibling = make_rect_node("clip-sibling", 12, 1, 4, 4);
    sibling->setAttribute("clip-path", "url(#user-clip)");
    append_to_root(sibling);
    cutter_in_pixels(2, 2, 4, 4);
    checkpoint();
    auto const mapping = *image()->pixelToDocumentAffine();
    auto const before = xml_for_bitmap_undo(document->getReprDoc());
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    expect_mapping(mapping, 2, 2);
    // No live converted clip dependency remains on the result...
    EXPECT_EQ(image_repr->attribute("clip-path"), nullptr);
    // ...while the shared definition, its geometry and the other user are kept.
    EXPECT_EQ(document->getObjectById("user-clip")->getRepr(), resource);
    EXPECT_EQ(resource->firstChild(), clip_child);
    EXPECT_STREQ(clip_child->attribute("width"), "20");
    EXPECT_STREQ(clip_child->attribute("height"), "20");
    EXPECT_STREQ(document->getObjectById("clip-sibling")->getRepr()->attribute("clip-path"), "url(#user-clip)");
    EXPECT_FALSE(cutter_is_consumed());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), before);
    EXPECT_STREQ(image()->getRepr()->attribute("clip-path"), "url(#user-clip)");
    EXPECT_FALSE(cutter_is_consumed());
}

// CLIP-1: with the cutter kept, results that change no pixel are no-ops.
TEST_F(DestructiveBitmapClipActionTest, InverseClipWithDisjointCutterIsANoOp)
{
    cutter()->getRepr()->setAttribute("x", "40");
    cutter()->getRepr()->setAttribute("y", "40");
    cutter()->getRepr()->setAttribute("width", "4");
    cutter()->getRepr()->setAttribute("height", "4");
    checkpoint();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const selected_before = desktop->getSelection()->items_vector();
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepOutside), BitmapClip::CommitStatus::NoChange);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    // A no-op changes nothing, selection included.
    EXPECT_EQ(desktop->getSelection()->items_vector(), selected_before);
}

TEST_F(DestructiveBitmapClipActionTest, FullCoverageOwnClipWithContainingCutterIsANoOpAndKeepsTheClip)
{
    // The image's own clip covers it completely, so baking it changes no alpha;
    // with a cutter containing the whole image the result is a no-op and the
    // clip reference stays attached (the image renders exactly as before).
    image()->getRepr()->setAttribute("clip-path", "url(#user-clip)");
    cutter()->getRepr()->setAttribute("x", "-10");
    cutter()->getRepr()->setAttribute("y", "-10");
    cutter()->getRepr()->setAttribute("width", "40");
    cutter()->getRepr()->setAttribute("height", "40");
    checkpoint();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::NoChange);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_STREQ(image()->getRepr()->attribute("clip-path"), "url(#user-clip)");
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, ImageMaskAndClipEffectsAreBakedAndDetached)
{
    struct Effect { char const *attribute; char const *value; };
    for (auto const effect : {Effect{"clip-path", "url(#box-clip)"},
                              Effect{"mask", "url(#user-mask)"}, Effect{"mask", "url(#box-mask)"}}) {
        SCOPED_TRACE(effect.value);
        auto *repr = image()->getRepr();
        repr->setAttribute(effect.attribute, effect.value);
        cutter_in_pixels(2, 2, 4, 4);
        checkpoint();
        auto const mapping = *image()->pixelToDocumentAffine();
        auto const before = xml_for_bitmap_undo(document->getReprDoc());
        ASSERT_TRUE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
        ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
        // The converted mask/clip no longer applies a second time.
        EXPECT_EQ(image()->getRepr()->attribute(effect.attribute), nullptr);
        EXPECT_FALSE(cutter_is_consumed());
        EXPECT_EQ(image()->pixbuf->width(), 4);
        EXPECT_EQ(image()->pixbuf->height(), 4);
        expect_mapping(mapping, 2, 2);
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
        EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), before);
        EXPECT_STREQ(image()->getRepr()->attribute(effect.attribute), effect.value);
        EXPECT_FALSE(cutter_is_consumed());
        reselect_pair();
        image()->getRepr()->removeAttribute(effect.attribute);
        document->ensureUpToDate();
    }
}

TEST_F(DestructiveBitmapClipActionTest, SharedAncestorAndFilterEffectsRejectAtomically)
{
    struct Effect { char const *attribute; char const *value; };
    for (auto const *target : {"bitmap", "parent"}) {
        for (auto const effect : {Effect{"clip-path", "url(#box-clip)"},
                                  Effect{"mask", "url(#user-mask)"},
                                  Effect{"style", "filter:url(#adjustment)"},
                                  Effect{"style", "filter:url(#user-filter)"}}) {
            // A mask/clip on the bitmap itself is now baked and detached; the
            // rejection cases for the bitmap target are the colour-changing
            // filters. Shared-ancestor effects are rejected for either target.
            if (std::string(target) == "bitmap" && std::string(effect.attribute) != "style") {
                continue;
            }
            SCOPED_TRACE(target);
            SCOPED_TRACE(effect.value);
            auto *repr = document->getObjectById(target)->getRepr();
            repr->setAttribute(effect.attribute, effect.value);
            cutter_in_pixels(2, 2, 4, 4);
            checkpoint();
            cutter()->getRepr()->setAttribute("data-redo", "resource-sentinel");
            DocumentUndo::done(document.get(), Util::Internal::ContextString{"Resource redo sentinel"},
                               "object-properties");
            ASSERT_TRUE(DocumentUndo::undo(document.get()));
            document->ensureUpToDate();
            reselect_pair();
            document->setModifiedSinceSave(false);
            auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
            auto const pixels = image()->pixbuf;
            EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::InvalidSelection);
            EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
            EXPECT_EQ(image()->pixbuf, pixels);
            EXPECT_FALSE(document->isModifiedSinceSave());
            EXPECT_TRUE(desktop->getSelection()->includes(image()));
            EXPECT_TRUE(desktop->getSelection()->includes(cutter()));
            ASSERT_TRUE(DocumentUndo::redo(document.get()));
            document->ensureUpToDate();
            EXPECT_STREQ(cutter()->getRepr()->attribute("data-redo"), "resource-sentinel");
            ASSERT_TRUE(DocumentUndo::undo(document.get()));
            document->ensureUpToDate();
            reselect_pair();
            repr->removeAttribute(effect.attribute);
        }
    }
}

TEST_F(DestructiveBitmapClipActionTest, ReferencedImageAndAncestorRejectAtomically)
{
    for (auto const *reference : {"#bitmap", "#parent"}) {
        SCOPED_TRACE(reference);
        auto *clone = document->getReprDoc()->createElement("svg:use");
        clone->setAttribute("href", reference);
        clone->setAttribute("x", "10");
        document->getReprRoot()->appendChild(clone);
        Inkscape::GC::release(clone);
        cutter_in_pixels(2, 2, 4, 4);
        checkpoint();
        auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
        auto const pixels = image()->pixbuf;
        for (auto mode : {BitmapClip::Mode::KeepInside, BitmapClip::Mode::KeepOutside}) {
            SCOPED_TRACE(mode == BitmapClip::Mode::KeepInside ? "inside" : "outside");
            // An untrimmed replacement would still alter the clone, so both
            // modes must reject rather than silently rewrite a shared payload.
            EXPECT_EQ(commit_clip(mode), BitmapClip::CommitStatus::InvalidSelection);
            EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
            EXPECT_EQ(image()->pixbuf, pixels);
            EXPECT_FALSE(document->isModifiedSinceSave());
            EXPECT_FALSE(DocumentUndo::undo(document.get()));
            EXPECT_FALSE(cutter_is_consumed());
        }
        document->getReprRoot()->removeChild(clone);
    }
}

TEST_F(DestructiveBitmapClipActionTest, SliceCannotRevealPreviouslyHiddenPixels)
{
    auto source = solid_pixbuf(8, 8, 0x336699ff);
    ASSERT_TRUE(source);
    place_pixels(*source, 1, 1, 8, 4, "xMidYMid slice");
    // Slice shows only source rows 2..6. Removing the last row would otherwise
    // normalize the viewport to source rows 0..7 and reveal hidden content.
    cutter_in_pixels(-1, 7, 10, 2);
    checkpoint();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepOutside),
              BitmapClip::CommitStatus::UnsupportedTrim);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, AveragedViewportScaleTrimReachingLastRowCommits)
{
    auto source = solid_pixbuf(1200, 1600, 0);
    ASSERT_TRUE(source);
    for (int y = 0; y < 1600; ++y) {
        for (int x = 0; x < 1200; ++x) {
            auto *rgba = pixel(*source, x, y);
            rgba[0] = static_cast<unsigned char>((x * 17 + y * 3) % 256);
            rgba[1] = static_cast<unsigned char>((x * 5 + y * 11) % 256);
            rgba[2] = static_cast<unsigned char>((x * 7 + y * 13) % 256);
            rgba[3] = 255;
        }
    }
    constexpr double x = 1294.227552;
    constexpr double y = 442.3421295;
    constexpr double width = 241.4878;
    constexpr double height = 321.9837;
    place_pixels(*source, x, y, width, height);
    cutter_in_pixels(-1, 510, 1202, 1100);
    checkpoint();
    auto const original_mapping = *image()->pixelToDocumentAffine();
    auto const original_href = href();
    auto const original_xml = xml_for_bitmap_undo(document->getReprDoc());
    auto const original_x = std::string(image()->getRepr()->attribute("x"));
    auto const original_y = std::string(image()->getRepr()->attribute("y"));
    auto const original_width = std::string(image()->getRepr()->attribute("width"));
    auto const original_height = std::string(image()->getRepr()->attribute("height"));
    Pixbuf original_pixels(*image()->pixbuf);
    original_pixels.ensurePixelFormat(Pixbuf::PF_GDK);

    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    ASSERT_FALSE(cutter_is_consumed());
    ASSERT_TRUE(image()->pixbuf);
    EXPECT_EQ(image()->pixbuf->width(), 1200);
    EXPECT_EQ(image()->pixbuf->height(), 1090);
    EXPECT_GE(image()->x.computed, x - 1e-9);
    EXPECT_GE(image()->y.computed, y - 1e-9);
    EXPECT_LE(image()->x.computed + image()->width.computed, x + width + 1e-9);
    EXPECT_LE(image()->y.computed + image()->height.computed, y + height + 1e-9);
    EXPECT_NEAR(image()->y.computed + image()->height.computed, y + height, 1e-9);
    // The image renders at SPViewBox's averaged uniform scale, so the retained
    // rows must stay exactly where they were drawn before the trim.
    constexpr double rendered_scale = (width / 1200 + height / 1600) / 2;
    EXPECT_NEAR(image()->x.computed, x, 1e-6);
    EXPECT_NEAR(image()->y.computed, y + 510 * rendered_scale, 1e-6);
    EXPECT_NEAR(image()->width.computed, 1200 * rendered_scale, 1e-6);
    auto const trimmed_mapping = image()->pixelToDocumentAffine();
    ASSERT_TRUE(trimmed_mapping);
    // Averaged-scale re-rounding can shift the mapping; 1e-4 source pixel is far below visibility.
    auto const mapping_tolerance = 1e-4 * original_mapping[0];
    for (auto const point : {Geom::Point(0, 0), Geom::Point(1200, 1090)}) {
        auto const actual = point * *trimmed_mapping;
        auto const expected = (point + Geom::Point(0, 510)) * original_mapping;
        EXPECT_NEAR(actual.x(), expected.x(), mapping_tolerance);
        EXPECT_NEAR(actual.y(), expected.y(), mapping_tolerance);
    }
    Pixbuf output_pixels(*image()->pixbuf);
    output_pixels.ensurePixelFormat(Pixbuf::PF_GDK);
    for (int row = 0; row < 1090; ++row) {
        for (int col = 0; col < 1200; ++col) {
            auto const *actual = pixel(output_pixels, col, row);
            auto const *expected = pixel(original_pixels, col, row + 510);
            for (int channel = 0; channel < 3; ++channel) {
                ASSERT_EQ(actual[channel], expected[channel]) << col << ',' << row << " channel " << channel;
            }
            ASSERT_EQ(actual[3], 255) << col << ',' << row;
        }
    }
    auto const committed_href = href();
    auto const committed_x = std::string(image()->getRepr()->attribute("x"));
    auto const committed_y = std::string(image()->getRepr()->attribute("y"));
    auto const committed_width = std::string(image()->getRepr()->attribute("width"));
    auto const committed_height = std::string(image()->getRepr()->attribute("height"));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), original_href);
    EXPECT_STREQ(image()->getRepr()->attribute("x"), original_x.c_str());
    EXPECT_STREQ(image()->getRepr()->attribute("y"), original_y.c_str());
    EXPECT_STREQ(image()->getRepr()->attribute("width"), original_width.c_str());
    EXPECT_STREQ(image()->getRepr()->attribute("height"), original_height.c_str());
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), original_xml);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), committed_href);
    EXPECT_STREQ(image()->getRepr()->attribute("x"), committed_x.c_str());
    EXPECT_STREQ(image()->getRepr()->attribute("y"), committed_y.c_str());
    EXPECT_STREQ(image()->getRepr()->attribute("width"), committed_width.c_str());
    EXPECT_STREQ(image()->getRepr()->attribute("height"), committed_height.c_str());
    EXPECT_FALSE(cutter_is_consumed());

    // Re-enter the edge-clamping path on the committed bitmap. Its final row
    // still reaches the previous viewport edge after removing the first column.
    append_to_root(make_rect_node("cutter", 0, 0, 1, 1));
    cutter_in_pixels(1, -1, 1200, 1092);
    reselect_pair();
    auto const second_mapping = *image()->pixelToDocumentAffine();
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    ASSERT_TRUE(image()->pixbuf);
    EXPECT_EQ(image()->pixbuf->width(), 1199);
    EXPECT_EQ(image()->pixbuf->height(), 1090);
    EXPECT_FALSE(cutter_is_consumed());
    auto const after_second = image()->pixelToDocumentAffine();
    ASSERT_TRUE(after_second);
    auto const last_before = Geom::Point(1200, 1090) * second_mapping;
    auto const last_after = Geom::Point(1199, 1090) * *after_second;
    auto const second_mapping_tolerance = 1e-4 * second_mapping[0];
    EXPECT_NEAR(last_after.x(), last_before.x(), second_mapping_tolerance);
    EXPECT_NEAR(last_after.y(), last_before.y(), second_mapping_tolerance);
}

TEST_F(DestructiveBitmapClipActionTest, MinAlignedSliceExtentGuardRejectsBothAxes)
{
    auto source = solid_pixbuf(1200, 1600, 0x336699ff);
    ASSERT_TRUE(source);
    for (double ratio : {1.00001, 1.000002}) {
        for (bool overflow_x : {true, false}) {
            SCOPED_TRACE(::testing::Message() << "ratio=" << ratio << " overflow_x=" << overflow_x);
            place_pixels(*source, 10, 20, 1200 * (overflow_x ? 1 : ratio),
                         1600 * (overflow_x ? ratio : 1), "xMinYMin slice");
            if (overflow_x) {
                cutter_in_pixels(-1, 510, 1202, 1091);
            } else {
                cutter_in_pixels(510, -1, 691, 1602);
            }
            checkpoint();
            auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
            auto const pixels = image()->pixbuf;
            EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::UnsupportedTrim);
            EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
            EXPECT_EQ(image()->pixbuf, pixels);
            EXPECT_FALSE(document->isModifiedSinceSave());
            EXPECT_FALSE(cutter_is_consumed());
            EXPECT_FALSE(DocumentUndo::undo(document.get()));
        }
    }
}

TEST_F(DestructiveBitmapClipActionTest, NearUnitSliceScaleClampsWithoutRevealingPixels)
{
    auto source = solid_pixbuf(1200, 1600, 0x336699ff);
    ASSERT_TRUE(source);
    place_pixels(*source, 10, 20, 1200 - 1e-4, 1600, "xMinYMin slice");
    cutter_in_pixels(-1, 510, 1202, 1091);
    checkpoint();
    auto const before = *image()->pixelToDocumentAffine();
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    ASSERT_TRUE(image()->pixbuf);
    EXPECT_EQ(image()->pixbuf->width(), 1200);
    EXPECT_EQ(image()->pixbuf->height(), 1090);
    auto const after = image()->pixelToDocumentAffine();
    ASSERT_TRUE(after);
    auto const mapping_tolerance = 1e-4 * before[0];
    for (auto const point : {Geom::Point(0, 0), Geom::Point(1200, 1090)}) {
        auto const actual = point * *after;
        auto const expected = (point + Geom::Point(0, 510)) * before;
        EXPECT_NEAR(actual.x(), expected.x(), mapping_tolerance);
        EXPECT_NEAR(actual.y(), expected.y(), mapping_tolerance);
    }
    EXPECT_LE(image()->x.computed + image()->width.computed, 10 + 1200 - 1e-4 + 1e-9);
}

TEST_F(DestructiveBitmapClipActionTest, NearUniformSliceBeyondAveragingStillRejects)
{
    auto source = solid_pixbuf(1200, 1600, 0x336699ff);
    ASSERT_TRUE(source);
    place_pixels(*source, 1294.227552, 442.3421295, 241.4878, 321.9837 * 1.00001,
                 "xMidYMid slice");
    cutter_in_pixels(-1, 510, 1202, 1100);
    checkpoint();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::UnsupportedTrim);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, ExactSliceViewportCropRestoresOriginalAspectOnUndo)
{
    auto source = solid_pixbuf(8, 8, 0x336699ff);
    ASSERT_TRUE(source);
    place_pixels(*source, 1, 1, 8, 4, "xMidYMid slice");
    cutter_in_pixels(0, 2, 8, 4);
    checkpoint();
    auto const before = xml_for_bitmap_undo(document->getReprDoc());
    auto const mapping = *image()->pixelToDocumentAffine();
    ASSERT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::Committed);
    EXPECT_EQ(image()->pixbuf->width(), 8);
    EXPECT_EQ(image()->pixbuf->height(), 4);
    EXPECT_DOUBLE_EQ(image()->width.computed, 8);
    EXPECT_DOUBLE_EQ(image()->height.computed, 4);
    expect_mapping(mapping, 0, 2);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), before);
    EXPECT_DOUBLE_EQ(image()->height.computed, 4);
    EXPECT_STREQ(image()->getRepr()->attribute("preserveAspectRatio"), "xMidYMid slice");
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    expect_mapping(mapping, 0, 2);
}

TEST_F(DestructiveBitmapClipActionTest, AllTransparentKeepsNonuniformGeometryAndCutter)
{
    auto source = solid_pixbuf(8, 8, 0x336699ff);
    ASSERT_TRUE(source);
    place_pixels(*source, 1, 2, 16, 24);
    cutter_in_pixels(-1, -1, 10, 10);
    checkpoint();
    auto const before = xml_for_bitmap_undo(document->getReprDoc());
    auto const mapping = *image()->pixelToDocumentAffine();
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepOutside),
              BitmapClip::CommitStatus::CommittedAllTransparent);
    // A fully transparent result is still a successful bitmap at the original
    // extent and nonuniform registration.
    EXPECT_EQ(image()->pixbuf->width(), 8);
    EXPECT_EQ(image()->pixbuf->height(), 8);
    EXPECT_DOUBLE_EQ(image()->width.computed, 16);
    EXPECT_DOUBLE_EQ(image()->height.computed, 24);
    expect_mapping(mapping, 0, 0);
    EXPECT_EQ(alpha_at(*image()->pixbuf, 4, 4), 0u);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_EQ(desktop->getSelection()->items_vector(), std::vector<SPItem *>{static_cast<SPItem *>(image())});
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), before);
    EXPECT_FALSE(cutter_is_consumed());
}

TEST_F(DestructiveBitmapClipActionTest, LinkedPhysicalCropPreservesFileAndRestoresLinkOnUndo)
{
    struct TemporaryFile {
        std::string directory;
        std::string path;
        ~TemporaryFile()
        {
            if (!path.empty()) g_unlink(path.c_str());
            if (!directory.empty()) g_rmdir(directory.c_str());
        }
    } file;
    auto *directory = g_dir_make_tmp("vacards-bitmap-trim-XXXXXX", nullptr);
    ASSERT_TRUE(directory);
    file.directory = directory;
    g_free(directory);
    file.path = file.directory + "/linked.png";
    auto source = solid_pixbuf(1000, 800, 0x336699ff);
    ASSERT_TRUE(source);
    ASSERT_TRUE(gdk_pixbuf_save(source->getPixbufRaw(), file.path.c_str(), "png", nullptr, nullptr));
    auto *uri = g_filename_to_uri(file.path.c_str(), nullptr, nullptr);
    ASSERT_TRUE(uri);
    std::string const link = uri;
    g_free(uri);
    setHrefAttribute(*image()->getRepr(), link.c_str());
    image()->getRepr()->setAttribute("sodipodi:absref", file.path.c_str());
    image()->getRepr()->setAttribute("width", "960");
    image()->getRepr()->setAttribute("height", "768");
    document->ensureUpToDate();
    ASSERT_FALSE(image()->missing);
    cutter_in_pixels(200, 150, 400, 300);
    checkpoint();
    auto const mapping = *image()->pixelToDocumentAffine();
    auto const before = xml_for_bitmap_undo(document->getReprDoc());
    GStatBuf stat_before{};
    ASSERT_EQ(g_stat(file.path.c_str(), &stat_before), 0);
    gchar *bytes_before = nullptr;
    gsize size_before = 0;
    ASSERT_TRUE(g_file_get_contents(file.path.c_str(), &bytes_before, &size_before, nullptr));
    std::string const contents(bytes_before, size_before);
    g_free(bytes_before);
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    EXPECT_FALSE(cutter_is_consumed());
    ASSERT_EQ(image()->pixbuf->width(), 400);
    ASSERT_EQ(image()->pixbuf->height(), 300);
    EXPECT_NEAR(image()->x.computed, 193, 1e-7);
    EXPECT_NEAR(image()->y.computed, 145, 1e-7);
    EXPECT_NEAR(image()->width.computed, 384, 1e-7);
    EXPECT_NEAR(image()->height.computed, 288, 1e-7);
    expect_mapping(mapping, 200, 150);
    EXPECT_EQ(image()->getRepr()->attribute("sodipodi:absref"), nullptr);
    EXPECT_EQ(href().find("data:image/png;base64,"), 0u);
    GStatBuf stat_after{};
    ASSERT_EQ(g_stat(file.path.c_str(), &stat_after), 0);
    EXPECT_EQ(stat_after.st_size, stat_before.st_size);
    EXPECT_EQ(stat_after.st_mtime, stat_before.st_mtime);
    EXPECT_EQ(stat_after.st_mode, stat_before.st_mode);
    gchar *bytes_after = nullptr;
    gsize size_after = 0;
    ASSERT_TRUE(g_file_get_contents(file.path.c_str(), &bytes_after, &size_after, nullptr));
    EXPECT_EQ(std::string(bytes_after, size_after), contents);
    g_free(bytes_after);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), link);
    EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), before);
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    expect_mapping(mapping, 200, 150);
}

TEST_F(DestructiveBitmapClipActionTest, UndoXmlSnapshotPreservesEverythingExceptAbsrefOrder)
{
    auto const live_before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const baseline = xml_for_bitmap_undo(document->getReprDoc());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), live_before);
    auto release = [](XML::Document *copy) { GC::release(copy); };
    std::unique_ptr<XML::Document, decltype(release)> copy(document->getReprDoc()->duplicate(nullptr), release);
    XML::Node *bitmap = nullptr;
    XML::Node *resource = nullptr;
    sp_repr_visit_descendants(copy.get(), [&](XML::Node *node) {
        if (g_strcmp0(node->attribute("id"), "bitmap") == 0) bitmap = node;
        if (g_strcmp0(node->attribute("id"), "user-clip") == 0) resource = node;
        return true;
    });
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(resource);
    std::string const absref(bitmap->attribute("sodipodi:absref"));
    bitmap->removeAttribute("sodipodi:absref");
    bitmap->setAttribute("sodipodi:absref", absref.c_str());
    EXPECT_NE(sp_repr_save_buf(copy.get()).raw(), live_before);
    EXPECT_EQ(xml_for_bitmap_undo(copy.get()), baseline);

    for (auto const *attribute : {"x", "y", "width", "height", "preserveAspectRatio", "href", "sodipodi:absref"}) {
        SCOPED_TRACE(attribute);
        auto const *value = bitmap->attribute(attribute);
        ASSERT_TRUE(value);
        std::string const original(value);
        bitmap->setAttribute(attribute, "changed");
        EXPECT_NE(xml_for_bitmap_undo(copy.get()), baseline);
        bitmap->setAttribute(attribute, original.c_str());
        EXPECT_EQ(xml_for_bitmap_undo(copy.get()), baseline);
    }
    bitmap->removeAttribute("sodipodi:absref");
    EXPECT_NE(xml_for_bitmap_undo(copy.get()), baseline);
    bitmap->setAttribute("sodipodi:absref", absref.c_str());
    bitmap->setAttribute("data-extra", "present");
    EXPECT_NE(xml_for_bitmap_undo(copy.get()), baseline);
    bitmap->removeAttribute("data-extra");
    resource->setAttribute("clipPathUnits", "objectBoundingBox");
    EXPECT_NE(xml_for_bitmap_undo(copy.get()), baseline);
    resource->setAttribute("clipPathUnits", "userSpaceOnUse");
    EXPECT_EQ(xml_for_bitmap_undo(copy.get()), baseline);

    auto *comment = copy->createComment("resource comment");
    resource->appendChild(comment);
    EXPECT_NE(xml_for_bitmap_undo(copy.get()), baseline);
    auto const with_comment = xml_for_bitmap_undo(copy.get());
    comment->setContent("different comment");
    EXPECT_NE(xml_for_bitmap_undo(copy.get()), with_comment);
    comment->setContent("resource comment");
    resource->changeOrder(comment, nullptr);
    EXPECT_NE(xml_for_bitmap_undo(copy.get()), with_comment);
    resource->removeChild(comment);
    GC::release(comment);
    auto *text = copy->createTextNode("resource text");
    resource->appendChild(text);
    EXPECT_NE(xml_for_bitmap_undo(copy.get()), baseline);
    auto const with_text = xml_for_bitmap_undo(copy.get());
    text->setContent("changed resource text");
    EXPECT_NE(xml_for_bitmap_undo(copy.get()), with_text);
    resource->removeChild(text);
    GC::release(text);
    EXPECT_EQ(xml_for_bitmap_undo(copy.get()), baseline);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), live_before);
}

TEST_F(DestructiveBitmapClipActionTest, MissingLinkedImageCannotEmbedItsPlaceholder)
{
    struct TemporaryDirectory {
        gchar *path = g_dir_make_tmp("vacards-missing-bitmap-XXXXXX", nullptr);
        ~TemporaryDirectory()
        {
            if (path) {
                g_rmdir(path);
                g_free(path);
            }
        }
    } directory;
    ASSERT_TRUE(directory.path);
    auto const missing_path = std::string(directory.path) + "/not-created.png";
    ASSERT_FALSE(g_file_test(missing_path.c_str(), G_FILE_TEST_EXISTS));
    auto *uri = g_filename_to_uri(missing_path.c_str(), nullptr, nullptr);
    ASSERT_TRUE(uri);
    setHrefAttribute(*image()->getRepr(), uri);
    g_free(uri);
    image()->getRepr()->setAttribute("sodipodi:absref", missing_path.c_str());
    checkpoint();
    ASSERT_TRUE(image()->missing);
    ASSERT_TRUE(image()->pixbuf) << "The broken-image placeholder is not usable source artwork";

    cutter()->getRepr()->setAttribute("data-redo", "missing-image-sentinel");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Missing image redo sentinel"},
                       "object-properties");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    document->setModifiedSinceSave(false);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const placeholder = image()->pixbuf;
    auto const placeholder_bytes = snapshot(*placeholder);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    // Object updates queue Selection::_schedule_modified's idle notification;
    // ensureUpToDate() alone does not run the cached action-sensitivity callback.
    // Exercise that real callback, without reselecting or directly refreshing it.
    drain_main_context();
    EXPECT_FALSE(action_enabled("object-destructive-clip"));
    EXPECT_FALSE(action_enabled("object-destructive-inverse-clip"));
    for (auto mode : {BitmapClip::Mode::KeepInside, BitmapClip::Mode::KeepOutside}) {
        EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), mode),
                  BitmapClip::CommitStatus::InvalidSelection);
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
        EXPECT_EQ(image()->pixbuf, placeholder);
        EXPECT_EQ(snapshot(*placeholder), placeholder_bytes);
        EXPECT_TRUE(image()->missing);
        EXPECT_TRUE(desktop->getSelection()->includes(image()));
        EXPECT_TRUE(desktop->getSelection()->includes(cutter()));
        EXPECT_FALSE(document->isModifiedSinceSave());
    }
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    EXPECT_STREQ(cutter()->getRepr()->attribute("data-redo"), "missing-image-sentinel");

    // Relinking to genuine pixels restores normal eligibility.
    setHrefAttribute(*image()->getRepr(), source_uri.c_str());
    document->ensureUpToDate();
    EXPECT_FALSE(image()->missing);
    EXPECT_TRUE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    drain_main_context();
    EXPECT_TRUE(action_enabled("object-destructive-clip"));
    EXPECT_TRUE(action_enabled("object-destructive-inverse-clip"));
}

TEST_F(DestructiveBitmapClipActionTest, SingularMappingRejectsWithoutMutation)
{
    image()->getRepr()->setAttribute("transform", "matrix(0 0 0 1 0 0)");
    checkpoint();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    // A singular/nonfinite image pixel-to-document mapping stays the typed
    // InvalidGeometry reason and keeps the existing geometry failure outcome.
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::InvalidGeometry);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidGeometry);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, InvalidProfileEncodingCannotPublishPixelsOrGeometry)
{
    auto poisoned = std::make_shared<Pixbuf>(*image()->pixbuf);
    // Inject encoder failure into detached canonical pixels without corrupting
    // source SVG. A truncated ICC header is rejected by the PNG encoder.
    ASSERT_TRUE(gdk_pixbuf_set_option(poisoned->getPixbufRaw(false), "icc-profile", "AAAA"));
    image()->pixbuf = poisoned;
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const source_bytes = snapshot(*poisoned);
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::EncodingFailed);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_EQ(image()->pixbuf, poisoned);
    EXPECT_EQ(snapshot(*poisoned), source_bytes);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, InverseTransactionUsesComplementAndCreatesExactlyOneUndo)
{
    auto const href_before = href();
    auto const absref_before = std::string(image()->getRepr()->attribute("sodipodi:absref"));

    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepOutside), BitmapClip::CommitStatus::Committed);

    ASSERT_NE(href(), href_before);
    ASSERT_TRUE(image()->pixbuf);
    EXPECT_EQ(alpha_at(*image()->pixbuf, 4, 4), 0u);
    EXPECT_GT(alpha_at(*image()->pixbuf, 0, 0), 0u);
    for (int channel = 0; channel < 4; ++channel) {
        EXPECT_EQ(pixel(*image()->pixbuf, 4, 4)[channel], 0u);
    }
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_EQ(desktop->getSelection()->items_vector(),
              std::vector<SPItem *>{static_cast<SPItem *>(image())});

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), href_before);
    EXPECT_STREQ(image()->getRepr()->attribute("sodipodi:absref"), absref_before.c_str());
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_TRUE(desktop->getSelection()->includes(image()));
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "inverse action must create exactly one Undo entry";

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_NE(href(), href_before);
    EXPECT_EQ(alpha_at(*image()->pixbuf, 4, 4), 0u);
    EXPECT_GT(alpha_at(*image()->pixbuf, 0, 0), 0u);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_TRUE(desktop->getSelection()->includes(image()));
}

// ---------------------------------------------------------------------------
// B04 transaction: coverage baking, cutter consumption and rollback.
// ---------------------------------------------------------------------------

TEST_F(DestructiveBitmapClipActionTest, MaskIsBakedIntoAlphaAndDetachedFromTheImage)
{
    // Independent oracle: a 4x4 opaque image under a grayscale mask that is
    // black over the left half and white over the right half. The baked alpha
    // must be exactly alpha_mul(255, coverage) per source pixel.
    auto source = solid_pixbuf(4, 4, 0x336699ff);
    ASSERT_TRUE(source);
    place_pixels(*source, 0, 0, 4, 4);
    auto *mask = document->getReprDoc()->createElement("svg:mask");
    mask->setAttribute("id", "half-mask");
    mask->setAttribute("maskUnits", "userSpaceOnUse");
    mask->setAttribute("maskContentUnits", "userSpaceOnUse");
    auto *left = document->getReprDoc()->createElement("svg:rect");
    left->setAttribute("x", "0");
    left->setAttribute("y", "0");
    left->setAttribute("width", "2");
    left->setAttribute("height", "4");
    // Mid-gray keeps a nonzero analytic alpha so the result is not trimmed;
    // native luminance-to-alpha maps 128,128,128 to coverage 128.
    left->setAttribute("fill", "rgb(50%,50%,50%)");
    auto *right = document->getReprDoc()->createElement("svg:rect");
    right->setAttribute("x", "2");
    right->setAttribute("y", "0");
    right->setAttribute("width", "2");
    right->setAttribute("height", "4");
    right->setAttribute("fill", "white");
    mask->appendChild(left);
    mask->appendChild(right);
    GC::release(left);
    GC::release(right);
    // Reuse the fixture's existing defs container.
    auto *defs = document->getObjectById("user-mask")->getRepr()->parent();
    ASSERT_TRUE(defs);
    defs->appendChild(mask);
    GC::release(mask);
    image()->getRepr()->setAttribute("mask", "url(#half-mask)");
    // Cutter covers the whole image, so alpha comes only from the mask.
    cutter_in_pixels(-1, -1, 8, 8);
    document->ensureUpToDate();
    checkpoint();
    auto const before = xml_for_bitmap_undo(document->getReprDoc());
    auto const mapping = *image()->pixelToDocumentAffine();

    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    ASSERT_TRUE(image()->pixbuf);
    EXPECT_EQ(image()->pixbuf->width(), 4);
    EXPECT_EQ(image()->pixbuf->height(), 4);
    expect_mapping(mapping, 0, 0);
    // Gray left half and white right half; keep-inside with a full cutter
    // leaves the analytic mask alphas (128 and 255) untouched.
    for (int y = 0; y < 4; ++y) {
        EXPECT_EQ(alpha_at(*image()->pixbuf, 0, y), 128u);
        EXPECT_EQ(alpha_at(*image()->pixbuf, 3, y), 255u);
    }
    EXPECT_EQ(image()->getRepr()->attribute("mask"), nullptr);
    EXPECT_FALSE(cutter_is_consumed());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), before);
    EXPECT_STREQ(image()->getRepr()->attribute("mask"), "url(#half-mask)");
}

TEST_F(DestructiveBitmapClipActionTest, ImageOpacityIsBakedAndRemovedFromStyle)
{
    image()->getRepr()->setAttribute("style", "opacity:0.5");
    document->ensureUpToDate();
    cutter_in_pixels(-2, -2, 12, 12);
    checkpoint();
    auto const before = xml_for_bitmap_undo(document->getReprDoc());
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    ASSERT_TRUE(image()->pixbuf);
    EXPECT_EQ(image()->pixbuf->width(), 8);
    EXPECT_EQ(image()->pixbuf->height(), 8);
    // Native half opacity rounds to 127 or 128 per the drawing backend; the
    // important contract is that it is baked exactly once and then detached.
    for (int y = 0; y < 4; ++y) {
        auto const alpha = alpha_at(*image()->pixbuf, 4, y);
        EXPECT_GE(alpha, 126u);
        EXPECT_LE(alpha, 129u);
    }
    EXPECT_FALSE(image()->style && image()->style->opacity.as_double() < 0.995);
    EXPECT_FALSE(cutter_is_consumed());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), before);
    ASSERT_TRUE(image()->style);
    EXPECT_DOUBLE_EQ(image()->style->opacity.as_double(), 0.5);
}

TEST_F(DestructiveBitmapClipActionTest, CutterReferencedElsewhereIsKeptAndStillReferenced)
{
    // CLIP-1: the cutter is kept unchanged, so a use of it elsewhere is
    // unaffected and no longer a reason to refuse.
    auto *cutter_use = document->getReprDoc()->createElement("svg:use");
    cutter_use->setAttribute("id", "cutter-use");
    cutter_use->setAttribute("href", "#cutter");
    cutter_use->setAttribute("x", "12");
    document->getReprRoot()->appendChild(cutter_use);
    GC::release(cutter_use);
    cutter_in_pixels(2, 2, 4, 4);
    document->ensureUpToDate();
    checkpoint();
    auto const href_before = href();
    auto const cutter_before = cutter_state();
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    EXPECT_NE(href(), href_before);
    EXPECT_EQ(cutter_state(), cutter_before);
    auto *use = document->getObjectById("cutter-use");
    ASSERT_TRUE(use);
    EXPECT_STREQ(use->getRepr()->attribute("href"), "#cutter");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(), href_before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, PublicationRollbackRestoresTheWholeTuple)
{
    // Force a throw from the first attribute observer; the transaction must roll
    // the payload back and leave the original cutter, selection and Undo intact.
    struct ThrowingObserver final : XML::NodeObserver {
        bool armed = true;
        void notifyAttributeChanged(XML::Node &, GQuark, Util::ptr_shared, Util::ptr_shared) override {
            if (armed) {
                armed = false;
                throw std::runtime_error("injected publication failure");
            }
        }
    } observer;
    auto *repr = image()->getRepr();
    auto const href_before = href();
    auto const before = xml_for_bitmap_undo(document->getReprDoc());
    repr->addObserver(observer);
    auto const status = commit_clip(BitmapClip::Mode::KeepInside);
    repr->removeObserver(observer);
    EXPECT_EQ(status, BitmapClip::CommitStatus::RasterizationFailed);
    document->ensureUpToDate();
    EXPECT_EQ(href(), href_before);
    EXPECT_EQ(xml_for_bitmap_undo(document->getReprDoc()), before);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    // The rollback also rebuilds the pre-command selection (cutter by id).
    EXPECT_TRUE(desktop->getSelection()->includes(image()));
    EXPECT_TRUE(desktop->getSelection()->includes(cutter()));
}

TEST_F(DestructiveBitmapClipActionTest, ReentrantObserverDuringPublicationFailsClosed)
{
    struct ReentrantObserver final : XML::NodeObserver {
        SPDesktop *desktop = nullptr;
        BitmapClip::CommitStatus inner = BitmapClip::CommitStatus::NoChange;
        unsigned calls = 0;
        void notifyAttributeChanged(XML::Node &, GQuark, Util::ptr_shared, Util::ptr_shared) override {
            if (calls++ == 0 && desktop && desktop->getSelection()) {
                inner = BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside);
            }
        }
    } observer;
    observer.desktop = desktop.get();
    auto *repr = image()->getRepr();
    auto const href_before = href();
    repr->addObserver(observer);
    ASSERT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::Committed);
    repr->removeObserver(observer);
    EXPECT_GE(observer.calls, 1u);
    // A nested dispatch from the publication notification must not begin a
    // second transaction; the outer commit remains the only one.
    EXPECT_TRUE(observer.inner == BitmapClip::CommitStatus::RasterizationFailed ||
                observer.inner == BitmapClip::CommitStatus::InvalidSelection)
        << "inner status " << static_cast<int>(observer.inner);
    EXPECT_NE(href(), href_before);
    EXPECT_FALSE(cutter_is_consumed());
    // Exactly one Undo entry belongs to the outer transaction.
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "reentrancy must not add an Undo entry";
}

TEST_F(DestructiveBitmapClipActionTest, StopRequestBeforePublicationLeavesTheDocumentUnchanged)
{
    auto const href_before = href();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside, stop.get_token()),
              BitmapClip::CommitStatus::Cancelled);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_EQ(href(), href_before);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, PlainImageCutterMemberIsClassifiedNotDropped)
{
    // A `use` of a bitmap inside the cutter group is invisible to the branch
    // scan, so the group is a cutter candidate; OffsetShapes would silently
    // drop the referenced bitmap member, so the resolver must classify it.
    auto *group = make_group("image-cutter-group");
    append_child(group, make_rect_node("image-cutter-rect", 1, 1, 4, 8));
    auto *image_use = document->getReprDoc()->createElement("svg:use");
    image_use->setAttribute("id", "image-cutter-use");
    image_use->setAttribute("href", "#bitmap");
    image_use->setAttribute("x", "5");
    append_child(group, image_use);
    append_to_root(group);
    select({static_cast<SPItem *>(image()), object("image-cutter-group")});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::UnsupportedCutter);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_EQ(resolved.cutter, nullptr);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::InvalidGeometry);
    EXPECT_FALSE(cutter_is_consumed());
}

TEST_F(DestructiveBitmapClipActionTest, WrapperBranchCommitIsRejectedAtomically)
{
    // The resolver classifies a single-image wrapper, but the transaction does
    // not yet collapse/reparent it. It must reject without touching anything.
    select({object("parent"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::Resolved);
    EXPECT_EQ(resolved.branch, object("parent"));
    EXPECT_EQ(resolved.image, image());
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const selection_before = desktop->getSelection()->items_vector();
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::InvalidSelection);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_EQ(desktop->getSelection()->items_vector(), selection_before);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipActionTest, ExclusivelyOwnedAncestorEffectIsClassifiedNotSilentlyCrossed)
{
    // A mask above the selected image is a shared compositing boundary; even
    // when the ancestor has no siblings it is rejected explicitly rather than
    // silently double-applied or dropped.
    document->getObjectById("parent")->getRepr()->setAttribute("mask", "url(#user-mask)");
    document->ensureUpToDate();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(commit_clip(BitmapClip::Mode::KeepInside), BitmapClip::CommitStatus::InvalidSelection);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(cutter_is_consumed());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

// ---------------------------------------------------------------------------
// B03 target resolution: exact roles, typed reasons and read-only behavior.
// ---------------------------------------------------------------------------

TEST_F(DestructiveBitmapClipActionTest, DirectPairResolvesBranchAndCutter)
{
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::Resolved);
    EXPECT_EQ(resolved.image, image());
    EXPECT_EQ(resolved.branch, static_cast<SPItem *>(image()));
    EXPECT_EQ(resolved.cutter, cutter());
    EXPECT_TRUE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
}

TEST_F(DestructiveBitmapClipActionTest, SingleImageWrapperGroupResolvesToLeaf)
{
    select({object("parent"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::Resolved);
    EXPECT_EQ(resolved.image, image());
    EXPECT_EQ(resolved.branch, object("parent"));
    EXPECT_EQ(resolved.cutter, cutter());
    EXPECT_TRUE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
}

TEST_F(DestructiveBitmapClipActionTest, NestedSingleImageWrapperResolvesToLeaf)
{
    select({object("outer"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::Resolved);
    EXPECT_EQ(resolved.image, image());
    EXPECT_EQ(resolved.branch, object("outer"));
    EXPECT_EQ(resolved.cutter, cutter());
}

TEST_F(DestructiveBitmapClipActionTest, ParentAndChildRootsAreRejectedAtomically)
{
    // The live Selection collapses a parent/child pair to the ancestor before
    // the adapter runs, so the operation sees one root and rejects with NotPair.
    select({static_cast<SPItem *>(image()), object("parent")});
    auto const live = resolve_and_expect_unchanged();
    EXPECT_EQ(live.status, BitmapClip::TargetStatus::NotPair);
    EXPECT_EQ(live.image, nullptr);

    // An explicit unnormalized root list gets the dedicated ancestor reason.
    auto const explicit_pair = BitmapClip::resolve_targets(
        std::vector<SPItem *>{static_cast<SPItem *>(image()), object("parent")}, *document);
    EXPECT_EQ(explicit_pair.status, BitmapClip::TargetStatus::NestedOrDuplicateRoots);
    EXPECT_EQ(explicit_pair.image, nullptr);
    EXPECT_EQ(explicit_pair.branch, nullptr);
    EXPECT_EQ(explicit_pair.cutter, nullptr);

    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidSelection);
}

TEST_F(DestructiveBitmapClipActionTest, AmbiguousMultiImageGroupIsRejected)
{
    auto *group = make_group("two");
    append_child(group, make_image_node("two-a", 1, 1));
    append_child(group, make_image_node("two-b", 10, 1));
    append_to_root(group);

    select({object("two"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::MultipleImages);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_EQ(resolved.branch, nullptr);
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepOutside),
              BitmapClip::CommitStatus::InvalidSelection);
}

TEST_F(DestructiveBitmapClipActionTest, MixedImageAndVectorBranchIsRejected)
{
    auto *group = make_group("mixed");
    append_child(group, make_image_node("mixed-image", 1, 1));
    append_child(group, make_rect_node("mixed-rect", 12, 1, 4, 4));
    append_to_root(group);

    select({object("mixed"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::UnsupportedBranch);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidSelection);
}

TEST_F(DestructiveBitmapClipActionTest, SharedMaskAncestorWithSiblingsIsRejected)
{
    auto *group = make_group("shared");
    group->setAttribute("mask", "url(#user-mask)");
    append_child(group, make_image_node("shared-image", 1, 1));
    append_child(group, make_rect_node("shared-sibling", 12, 1, 4, 4));
    append_to_root(group);

    select({object("shared"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::UnsupportedBranch);
    EXPECT_EQ(resolved.branch, nullptr);
}

TEST_F(DestructiveBitmapClipActionTest, CloneOfWrapperBranchIsRejected)
{
    auto *clone = document->getReprDoc()->createElement("svg:use");
    clone->setAttribute("href", "#parent");
    clone->setAttribute("x", "12");
    append_to_root(clone);

    select({object("parent"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::UnsupportedBranch);
    EXPECT_EQ(resolved.branch, nullptr);

    // A directly selected bitmap keeps the existing trim-conditional clone
    // policy: the clone is not itself a resolution rejection there.
    select({static_cast<SPItem *>(image()), cutter()});
    auto const direct = resolve_and_expect_unchanged();
    EXPECT_EQ(direct.status, BitmapClip::TargetStatus::Resolved);
    EXPECT_EQ(direct.image, image());
    EXPECT_EQ(direct.branch, static_cast<SPItem *>(image()));
}

TEST_F(DestructiveBitmapClipActionTest, HiddenWrapperBranchIsRejected)
{
    object("parent")->getRepr()->setAttribute("style", "display:none");
    document->ensureUpToDate();

    select({object("parent"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::ProtectedObject);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
}

TEST_F(DestructiveBitmapClipActionTest, LockedWrapperBranchIsRejected)
{
    object("parent")->getRepr()->setAttribute("sodipodi:insensitive", "1");
    document->ensureUpToDate();

    select({object("parent"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::ProtectedObject);
    EXPECT_EQ(resolved.image, nullptr);
}

TEST_F(DestructiveBitmapClipActionTest, MissingImageInWrapperIsRejected)
{
    setHrefAttribute(*image()->getRepr(), "file:///nonexistent/vacards-b03-missing.png");
    image()->getRepr()->removeAttribute("sodipodi:absref");
    document->ensureUpToDate();
    ASSERT_TRUE(image()->missing);

    select({object("parent"), cutter()});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::MissingImage);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
}

TEST_F(DestructiveBitmapClipActionTest, UnsupportedOpenCutterIsRejected)
{
    auto *open = document->getReprDoc()->createElement("svg:path");
    open->setAttribute("id", "open-cutter");
    open->setAttribute("d", "M 1 1 L 8 8");
    open->setAttribute("fill", "none");
    open->setAttribute("stroke", "#000000");
    append_to_root(open);

    select({static_cast<SPItem *>(image()), object("open-cutter")});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::UnsupportedCutter);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_EQ(resolved.cutter, nullptr);
    // D1: the typed reason stays UnsupportedCutter, but the user-facing
    // outcome is the pre-B03 cutter-specific InvalidGeometry, not the generic
    // InvalidSelection message.
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidGeometry);
}

// D2: a cutter group must not silently drop or include a member that
// `OffsetShapes::prepare` cannot faithfully turn into the closed region. A
// locked member is protected; a hidden/masked/filtered member would be dropped
// by `item_geometry` without incrementing its skipped counter.
TEST_F(DestructiveBitmapClipActionTest, LockedCutterDescendantIsRejected)
{
    auto *group = make_group("cutter-with-locked");
    auto *locked = make_rect_node("cutter-locked", 1, 1, 4, 8);
    locked->setAttribute("sodipodi:insensitive", "1");
    append_child(group, locked);
    append_child(group, make_rect_node("cutter-visible", 5, 1, 2, 8));
    append_to_root(group);

    select({static_cast<SPItem *>(image()), object("cutter-with-locked")});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::ProtectedObject);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_EQ(resolved.branch, nullptr);
    EXPECT_EQ(resolved.cutter, nullptr);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidSelection);
}

TEST_F(DestructiveBitmapClipActionTest, HiddenCutterDescendantIsRejected)
{
    auto *group = make_group("cutter-with-hidden");
    auto *hidden = make_rect_node("cutter-hidden", 1, 1, 4, 8);
    hidden->setAttribute("style", "display:none");
    append_child(group, hidden);
    append_child(group, make_rect_node("cutter-visible", 5, 1, 2, 8));
    append_to_root(group);

    select({static_cast<SPItem *>(image()), object("cutter-with-hidden")});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::ProtectedObject);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_EQ(resolved.cutter, nullptr);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidSelection);
}

TEST_F(DestructiveBitmapClipActionTest, MaskedCutterDescendantIsRejected)
{
    auto *group = make_group("cutter-with-mask");
    auto *masked = make_rect_node("cutter-masked", 1, 1, 4, 8);
    masked->setAttribute("mask", "url(#user-mask)");
    append_child(group, masked);
    append_child(group, make_rect_node("cutter-plain", 5, 1, 2, 8));
    append_to_root(group);

    select({static_cast<SPItem *>(image()), object("cutter-with-mask")});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::UnsupportedCutter);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_EQ(resolved.cutter, nullptr);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidGeometry);
}

TEST_F(DestructiveBitmapClipActionTest, FilteredCutterDescendantIsRejected)
{
    auto *group = make_group("cutter-with-filter");
    auto *filtered = make_rect_node("cutter-filtered", 1, 1, 4, 8);
    filtered->setAttribute("style", "filter:url(#adjustment)");
    append_child(group, filtered);
    append_child(group, make_rect_node("cutter-plain", 5, 1, 2, 8));
    append_to_root(group);

    select({static_cast<SPItem *>(image()), object("cutter-with-filter")});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::UnsupportedCutter);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_EQ(resolved.cutter, nullptr);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidGeometry);
}

TEST_F(DestructiveBitmapClipActionTest, GroupedClosedCutterResolvesAndBakesAsOneRegion)
{
    auto *group = make_group("cutter-group");
    append_child(group, make_rect_node("cutter-left", 1, 1, 4, 8));
    append_child(group, make_rect_node("cutter-right", 5, 1, 2, 8));
    append_to_root(group);

    select({static_cast<SPItem *>(image()), object("cutter-group")});
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::Resolved);
    EXPECT_EQ(resolved.image, image());
    EXPECT_EQ(resolved.branch, static_cast<SPItem *>(image()));
    EXPECT_EQ(resolved.cutter, object("cutter-group"));
    EXPECT_TRUE(BitmapClip::selection_is_eligible(*desktop->getSelection()));

    // The two rectangles are one collective closed region: the retained crop
    // spans the union (source pixels 0..5 of 8), proving both members applied.
    auto const href_before = href();
    ASSERT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::Committed);
    EXPECT_NE(href(), href_before);
    ASSERT_TRUE(image()->pixbuf);
    EXPECT_EQ(image()->pixbuf->width(), 6);
    EXPECT_EQ(image()->pixbuf->height(), 8);
}

// D3: a compositing context above the selected wrapper is outside the
// conversion boundary and stays shared with the rest of the document. The
// sibling lives outside the wrapper, so the branch subtree scan cannot see it;
// only ancestor-level classification can reject the pair.
TEST_F(DestructiveBitmapClipActionTest, SharedCompositingAncestorAboveWrapperIsRejected)
{
    auto *outer = object("outer");
    ASSERT_TRUE(outer);
    outer->getRepr()->setAttribute("mask", "url(#user-mask)");
    append_child(outer->getRepr(), make_rect_node("ancestor-sibling", 12, 1, 4, 4));
    document->ensureUpToDate();

    // Control: the same unrelated sibling without the shared mask resolves,
    // proving the mask above the wrapper -- not the sibling -- is the reason.
    outer->getRepr()->removeAttribute("mask");
    document->ensureUpToDate();
    select({object("parent"), cutter()});
    auto const control = resolve_and_expect_unchanged();
    EXPECT_EQ(control.status, BitmapClip::TargetStatus::Resolved);
    EXPECT_EQ(control.branch, object("parent"));

    outer->getRepr()->setAttribute("mask", "url(#user-mask)");
    document->ensureUpToDate();
    auto const resolved = resolve_and_expect_unchanged();
    EXPECT_EQ(resolved.status, BitmapClip::TargetStatus::UnsupportedBranch);
    EXPECT_EQ(resolved.image, nullptr);
    EXPECT_EQ(resolved.branch, nullptr);
    EXPECT_EQ(resolved.cutter, nullptr);
    EXPECT_FALSE(BitmapClip::selection_is_eligible(*desktop->getSelection()));
    EXPECT_EQ(BitmapClip::commit_selection(*desktop->getSelection(), BitmapClip::Mode::KeepInside),
              BitmapClip::CommitStatus::InvalidSelection);
}

// DC-S1 uses a headless selection: these document/render/Undo tests do not
// require SPDesktop or a physical display and can run inside the macOS sandbox.
class DestructiveBitmapClipStraightenTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        initialize_application();
        if (!Application::exists()) Application::create(false);
        auto source = solid_pixbuf(128, 96, 0x336699ff);
        ASSERT_TRUE(source);
        // Smooth, asymmetric colours expose incorrect transform/density mapping.
        for (int y = 0; y < 96; ++y) for (int x = 0; x < 128; ++x) {
            auto *p = pixel(*source, x, y);
            p[0] = 40 + x; p[1] = 60 + y; p[2] = 150;
        }
        source->markDirty();
        auto svg = Glib::ustring::compose(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="24">
<defs/>
<g id="parent"><image id="bitmap" x="8" y="6" width="16" height="12" preserveAspectRatio="none"
transform="rotate(30 16 12)" href="%1"/></g>
<rect id="cutter" x="11" y="7" width="10" height="10" fill="#ff00ff"/>
</svg>)svg", png_uri(*source));
        document = SPDocument::createNewDocFromMem(svg.raw());
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        selection = std::make_unique<Selection>(document.get());
        selection->setList(std::vector<SPItem *>{image(), cutter()});
        checkpoint();
    }
    void TearDown() override { selection.reset(); document.reset(); }
    SPImage *image() const { return cast<SPImage>(document->getObjectById("bitmap")); }
    SPItem *cutter() const { return cast<SPItem>(document->getObjectById("cutter")); }
    void checkpoint()
    {
        document->ensureUpToDate();
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Straighten fixture"}, "object-properties");
        DocumentUndo::clearUndo(document.get()); DocumentUndo::clearRedo(document.get());
    }
    BitmapClip::CommitStatus commit(BitmapClip::Mode mode = BitmapClip::Mode::KeepInside)
    {
        auto status = BitmapClip::commit_selection(*selection, mode);
        document->ensureUpToDate();
        return status;
    }
    Geom::Rect frame() const
    {
        auto box = Geom::Rect::from_xywh(0, 0, image()->pixbuf->width(), image()->pixbuf->height());
        box *= *image()->pixelToDocumentAffine();
        return box;
    }
    void expect_straight(double density)
    {
        auto const l = *image()->pixelToDocumentAffine();
        EXPECT_NEAR(l[1], 0, 1e-8); EXPECT_NEAR(l[2], 0, 1e-8);
        EXPECT_GT(l[0], 0); EXPECT_GT(l[3], 0);
        EXPECT_GE(1 / l[0], density - 1e-5); EXPECT_GE(1 / l[3], density - 1e-5);
        EXPECT_NEAR(image()->x.computed, 0, 1e-9); EXPECT_NEAR(image()->y.computed, 0, 1e-9);
        EXPECT_STREQ(image()->getRepr()->attribute("preserveAspectRatio"), "none");
        EXPECT_EQ(selection->items_vector(), std::vector<SPItem *>{image()});
    }
    void expect_inside(Geom::Rect const &box, double density)
    {
        auto const actual = frame(); double const tolerance = 1 / density + 1e-5;
        EXPECT_GE(actual.left(), box.left() - tolerance); EXPECT_GE(actual.top(), box.top() - tolerance);
        EXPECT_LE(actual.right(), box.right() + tolerance); EXPECT_LE(actual.bottom(), box.bottom() + tolerance);
    }
    void expect_frame(Geom::Rect const &box, double density, double tolerance = -1)
    {
        auto const actual = frame();
        if (tolerance < 0) tolerance = 1 / density + 1e-5;
        EXPECT_NEAR(actual.left(), box.left(), tolerance); EXPECT_NEAR(actual.top(), box.top(), tolerance);
        EXPECT_NEAR(actual.right(), box.right(), tolerance); EXPECT_NEAR(actual.bottom(), box.bottom(), tolerance);
        EXPECT_NEAR(1 / (*image()->pixelToDocumentAffine())[0], density, 1e-3);
    }
    // Independent document-render oracle: clip the original document with a
    // normal vector clip, rather than constructing expected pixels via apply().
    std::unique_ptr<SPDocument> oracle(bool clip_to_cutter)
    {
        auto copy = SPDocument::createNewDocFromMem(sp_repr_save_buf(document->getReprDoc()).raw());
        if (clip_to_cutter) {
            auto *clip = copy->getReprDoc()->createElement("svg:clipPath");
            clip->setAttribute("id", "straighten-oracle"); clip->setAttribute("clipPathUnits", "userSpaceOnUse");
            auto *rect = cutter()->getRepr()->duplicate(copy->getReprDoc());
            rect->removeAttribute("id");
            auto const transform = cutter()->i2doc_affine() * image()->i2doc_affine().inverse();
            rect->setAttribute("transform", sp_svg_transform_write(transform));
            clip->appendChild(rect); GC::release(rect);
            auto *defs = copy->getReprRoot()->firstChild();
            defs->appendChild(clip); GC::release(clip);
            copy->getObjectById("bitmap")->getRepr()->setAttribute("clip-path", "url(#straighten-oracle)");
        }
        copy->ensureUpToDate(); return copy;
    }
    void expect_render(SPDocument &expected, Geom::Rect const &region, bool cutter_edges, bool bitmap_edges = false)
    {
        constexpr double zoom = 3;
        auto a = render_oracle_grid(expected, zoom), b = render_oracle_grid(*document, zoom);
        double sum = 0; std::size_t samples = 0; unsigned max_interior = 0;
        unsigned maximum = 0;
        for (int y = 0; y < a->get_height(); ++y) for (int x = 0; x < a->get_width(); ++x) {
            double const dx = (x + 0.5 - 12.25) / zoom, dy = (y + 0.5 - 9.375) / zoom;
            if (!region.contains(Geom::Point(dx, dy))) continue;
            bool edge = cutter_edges &&
                std::min({dx - region.left(), region.right() - dx, dy - region.top(), region.bottom() - dy}) * zoom <= 2;
            if (bitmap_edges) {
                auto const alpha = a->get_data()[y*a->get_stride()+4*x+cairo_alpha_offset];
                for (int ey=std::max(0,y-2);ey<=std::min(a->get_height()-1,y+2);++ey)
                    for (int ex=std::max(0,x-2);ex<=std::min(a->get_width()-1,x+2);++ex)
                        edge |= a->get_data()[ey*a->get_stride()+4*ex+cairo_alpha_offset] != alpha;
            }
            for (int c = 0; c < 4; ++c) {
                unsigned const diff = std::abs(int(a->get_data()[y * a->get_stride() + 4*x+c]) -
                                               int(b->get_data()[y * b->get_stride() + 4*x+c]));
                sum += diff; ++samples; maximum = std::max(maximum, diff);
                if (!edge) max_interior = std::max(max_interior, diff);
            }
        }
        ASSERT_GT(samples, 0u);
        RecordProperty("mean_diff_bytes", sum / samples);
        RecordProperty("max_interior_diff_bytes", max_interior);
        EXPECT_LE(sum / samples, 3.0) << "mean byte difference";
        EXPECT_LE(max_interior, 3u) << "large differences must be within 2 render pixels of cutter edges; max=" << maximum;
    }
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<Selection> selection;
};

TEST_F(DestructiveBitmapClipStraightenTest, T1RotatedCrossingCutterDensityFrameAndRendering)
{
    auto expected = oracle(true);
    auto const cutter_xml = sp_repr_save_buf(document->getReprDoc()).raw();
    ASSERT_EQ(commit(), BitmapClip::CommitStatus::CommittedStraightened);
    expect_straight(8); expect_inside(Geom::Rect(11, 7, 21, 17), 8);
    expect_frame(Geom::Rect(11,7,21,17),8);
    expect_render(*expected, Geom::Rect(11, 7, 21, 17), true);
    EXPECT_EQ(image()->parent, document->getObjectById("parent"));
    EXPECT_EQ(image()->getRepr()->position(), 0);
    EXPECT_NE(sp_repr_save_buf(document->getReprDoc()).raw(), cutter_xml);
    auto original_cutter = expected->getObjectById("cutter")->getRepr();
    for (auto const &attribute : original_cutter->attributeList()) {
        EXPECT_STREQ(cutter()->getRepr()->attribute(g_quark_to_string(attribute.key)), attribute.value.pointer());
    }
    EXPECT_EQ(cutter()->getRepr()->position(), original_cutter->position());
}

TEST_F(DestructiveBitmapClipStraightenTest, T2VisiblePixelsInsideCutterStillTrimsFrame)
{
    auto source = solid_pixbuf(128, 96, 0x00000000);
    for (int y = 0; y < 96; ++y) for (int x = 0; x < 128; ++x) {
        double const dx=(x+0.5-64)/64, dy=(y+0.5-48)/48;
        if (dx*dx+dy*dy>1) continue;
        auto *p = pixel(*source, x, y); p[0] = 51; p[1] = 102; p[2] = 153; p[3] = 255;
    }
    source->markDirty(); setHrefAttribute(*image()->getRepr(), png_uri(*source).c_str());
    auto *repr=cutter()->getRepr();
    repr->setAttributeSvgDouble("x",8); repr->setAttributeSvgDouble("y",5);
    repr->setAttributeSvgDouble("width",16); repr->setAttributeSvgDouble("height",14); checkpoint();
    // The ellipse touches every intrinsic-grid side, yet fits the cutter.
    // Establish the owner's old NoChange trigger, rather than merely testing
    // an alpha trim which the previous intrinsic-grid path already performed.
    auto const old_result=BitmapClip::apply(*source,
        rect_path(8,5,24,19)*image()->pixelToDocumentAffine()->inverse(),
        BitmapClip::FillRule::NonZero,BitmapClip::Mode::KeepInside);
    ASSERT_TRUE(old_result.completed()); ASSERT_FALSE(old_result.changed); ASSERT_FALSE(old_result.trimmed());
    auto expected = oracle(false); auto const old_frame = frame();
    ASSERT_EQ(commit(), BitmapClip::CommitStatus::CommittedStraightened);
    expect_straight(8); expect_inside(Geom::Rect(8, 5, 24, 19), 8);
    EXPECT_LT(frame().width(), old_frame.width()); EXPECT_LT(frame().height(), old_frame.height());
    // Include the bitmap edges in the 2 px edge band by using its visible box.
    auto a = render_oracle_grid(*expected, 3), b = render_oracle_grid(*document, 3);
    double sum = 0; unsigned interior_max = 0; std::size_t count = 0;
    for (int y = 0; y < 96; ++y) for (int x = 0; x < 128; ++x) {
        auto const point = Geom::Point((x + 0.5 - 12.25) / 3, (y + 0.5 - 9.375) / 3);
        if (!Geom::Rect(8,5,24,19).contains(point)) continue;
        auto *pa = a->get_data() + y*a->get_stride()+4*x;
        auto *pb = b->get_data() + y*b->get_stride()+4*x;
        bool edge = false;
        for (int ey = std::max(0,y-2); ey <= std::min(95,y+2); ++ey)
            for (int ex = std::max(0,x-2); ex <= std::min(127,x+2); ++ex) {
                unsigned alpha = a->get_data()[ey*a->get_stride()+4*ex+cairo_alpha_offset];
                edge |= alpha != pa[cairo_alpha_offset];
            }
        for (int c = 0; c < 4; ++c) {
            unsigned diff = std::abs(int(pa[c])-int(pb[c])); sum += diff; ++count;
            if (!edge) interior_max = std::max(interior_max, diff);
        }
    }
    ASSERT_GT(count,0u);
    RecordProperty("mean_diff_bytes", sum/count);
    RecordProperty("max_interior_diff_bytes",interior_max);
    EXPECT_LE(sum/count, 3); EXPECT_LE(interior_max, 3u);
}

TEST_F(DestructiveBitmapClipStraightenTest, T3OrthogonalRotationsAndFlipsKeepExactPixels)
{
    for (auto const *transform : {"rotate(90 16 12)", "rotate(180 16 12)",
                                  "rotate(270 16 12)", "translate(32 0) scale(-1 1)",
                                  "rotate(90 16 12) translate(32 0) scale(-1 1)"}) {
        SCOPED_TRACE(transform);
        image()->getRepr()->setAttribute("transform", transform); document->ensureUpToDate();
        auto const mapping = *image()->pixelToDocumentAffine();
        auto *rect = cutter()->getRepr();
        rect->setAttributeSvgDouble("x", 32); rect->setAttributeSvgDouble("y", 24);
        rect->setAttributeSvgDouble("width", 64); rect->setAttributeSvgDouble("height", 48);
        rect->setAttribute("transform", sp_svg_transform_write(mapping)); checkpoint();
        Pixbuf original(*image()->pixbuf); original.ensurePixelFormat(Pixbuf::PF_GDK);
        ASSERT_EQ(commit(), BitmapClip::CommitStatus::Committed);
        ASSERT_EQ(image()->pixbuf->width(), 64); ASSERT_EQ(image()->pixbuf->height(), 48);
        Pixbuf actual(*image()->pixbuf); actual.ensurePixelFormat(Pixbuf::PF_GDK);
        for (int y = 0; y < 48; ++y) EXPECT_EQ(std::memcmp(pixel(original,32,y+24), pixel(actual,0,y),64*4),0);
        EXPECT_STREQ(image()->getRepr()->attribute("transform"), transform);
        ASSERT_TRUE(DocumentUndo::undo(document.get())); document->ensureUpToDate();
        selection->setList(std::vector<SPItem *>{image(),cutter()});
    }
}

TEST_F(DestructiveBitmapClipStraightenTest, T4RotatedParentIsCancelledInDocumentFrame)
{
    image()->getRepr()->removeAttribute("transform");
    document->getObjectById("parent")->getRepr()->setAttribute("transform", "rotate(30 16 12)"); checkpoint();
    auto expected = oracle(true);
    ASSERT_EQ(commit(), BitmapClip::CommitStatus::CommittedStraightened);
    expect_straight(8); expect_inside(Geom::Rect(11,7,21,17),8);
    expect_frame(Geom::Rect(11,7,21,17),8);
    auto const parent = cast<SPItem>(image()->parent)->i2doc_affine();
    auto const composed = image()->transform * parent;
    EXPECT_NEAR(composed[1],0,1e-7); EXPECT_NEAR(composed[2],0,1e-7);
    EXPECT_NEAR(composed[0],1,1e-7); EXPECT_NEAR(composed[3],1,1e-7);
    expect_render(*expected, Geom::Rect(11,7,21,17),true);
}

TEST_F(DestructiveBitmapClipStraightenTest, T5InverseHoleInStraightGrid)
{
    auto const old_frame = frame();
    auto const original_inverse = image()->pixelToDocumentAffine()->inverse();
    ASSERT_EQ(commit(BitmapClip::Mode::KeepOutside), BitmapClip::CommitStatus::CommittedStraightened);
    expect_straight(8); expect_inside(old_frame,8);
    auto const inverse = image()->pixelToDocumentAffine()->inverse();
    auto const center = Geom::Point(16,12) * inverse;
    EXPECT_EQ(alpha_at(*image()->pixbuf, int(center.x()), int(center.y())),0u);
    // Source centre above the hole remains opaque, so a blank output cannot pass.
    auto const retained = Geom::Point(16,6) * inverse;
    EXPECT_GT(alpha_at(*image()->pixbuf,int(retained.x()),int(retained.y())),200u);
    auto const mapping = *image()->pixelToDocumentAffine();
    std::size_t hole_samples=0, retained_samples=0;
    for (int y=0;y<image()->pixbuf->height();++y) for (int x=0;x<image()->pixbuf->width();++x) {
        auto const doc = Geom::Point(x+0.5,y+0.5)*mapping;
        auto const src = doc*original_inverse;
        if (src.x()<2 || src.y()<2 || src.x()>126 || src.y()>94) continue;
        bool const hole = doc.x()>11.25 && doc.x()<20.75 && doc.y()>7.25 && doc.y()<16.75;
        bool const outside = doc.x()<10.75 || doc.x()>21.25 || doc.y()<6.75 || doc.y()>17.25;
        if (hole) { EXPECT_EQ(alpha_at(*image()->pixbuf,x,y),0u); ++hole_samples; }
        if (outside) { EXPECT_EQ(alpha_at(*image()->pixbuf,x,y),255u); ++retained_samples; }
    }
    EXPECT_GT(hole_samples,0u); EXPECT_GT(retained_samples,0u);

}

TEST_F(DestructiveBitmapClipStraightenTest, T6PixelatedCheckerUsesNearestOnly)
{
    auto source = solid_pixbuf(128,96,0x000000ff);
    for (int y=0;y<96;++y) for (int x=0;x<128;++x) {
        auto *p = pixel(*source,x,y); bool white = ((x/4+y/4)%2)==0;
        p[0]=p[1]=p[2]=white?255:0;
    }
    source->markDirty(); setHrefAttribute(*image()->getRepr(),png_uri(*source).c_str());
    image()->getRepr()->setAttribute("style","image-rendering:pixelated"); checkpoint();
    ASSERT_EQ(commit(),BitmapClip::CommitStatus::CommittedStraightened);
    Pixbuf actual(*image()->pixbuf); actual.ensurePixelFormat(Pixbuf::PF_GDK);
    bool black=false,white=false,transparent=false;
    for (int y=0;y<image()->pixbuf->height();++y) for (int x=0;x<image()->pixbuf->width();++x) {
        auto *p=pixel(actual,x,y);
        if (!p[3]) { transparent=true; continue; }
        EXPECT_EQ(p[0],p[1]); EXPECT_EQ(p[0],p[2]); EXPECT_TRUE(p[0]==0 || p[0]==255);
        black |= p[0]==0; white |= p[0]==255;
    }
    EXPECT_TRUE(black); EXPECT_TRUE(white); EXPECT_TRUE(transparent);
}

TEST_F(DestructiveBitmapClipStraightenTest, T7UndoRestoresXmlAndRedoReappliesOneStep)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    ASSERT_EQ(commit(),BitmapClip::CommitStatus::CommittedStraightened);
    auto const after = sp_repr_save_buf(document->getReprDoc()).raw();
    ASSERT_TRUE(DocumentUndo::undo(document.get())); document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get())); document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),after);
}

TEST_F(DestructiveBitmapClipStraightenTest, T8OversizedGridRefusesAtomically)
{
    // Strong anisotropy raises D while keeping the decoded source tiny.
    image()->getRepr()->setAttributeSvgDouble("width",10000);
    image()->getRepr()->setAttributeSvgDouble("height",0.01); checkpoint();
    auto const before=sp_repr_save_buf(document->getReprDoc()).raw();
    auto const selected=selection->items_vector();
    EXPECT_EQ(commit(),BitmapClip::CommitStatus::StraighteningTooLarge);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before);
    EXPECT_EQ(selection->items_vector(),selected); EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipStraightenTest, ContainingCutterNoFrameReductionPreservesRedo)
{
    auto *repr = cutter()->getRepr();
    repr->setAttributeSvgDouble("x",0); repr->setAttributeSvgDouble("y",0);
    repr->setAttributeSvgDouble("width",32); repr->setAttributeSvgDouble("height",24); checkpoint();
    document->getReprRoot()->setAttribute("data-redo","sentinel");
    DocumentUndo::done(document.get(),Util::Internal::ContextString{"Redo sentinel"},"object-properties");
    ASSERT_TRUE(DocumentUndo::undo(document.get())); document->ensureUpToDate();
    auto const before=sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(commit(),BitmapClip::CommitStatus::NoChange);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before);
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    EXPECT_STREQ(document->getReprRoot()->attribute("data-redo"),"sentinel");
}

TEST_F(DestructiveBitmapClipStraightenTest, EmptyRegionRefusesAndInverseFullyTransparentCommits)
{
    auto *repr=cutter()->getRepr();
    repr->setAttributeSvgDouble("x",100); checkpoint();
    auto const before=sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(commit(),BitmapClip::CommitStatus::StraighteningEmptyRegion);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    repr->setAttributeSvgDouble("x",0); repr->setAttributeSvgDouble("y",0);
    repr->setAttributeSvgDouble("width",32); repr->setAttributeSvgDouble("height",24); checkpoint();
    ASSERT_EQ(commit(BitmapClip::Mode::KeepOutside),BitmapClip::CommitStatus::CommittedStraightenedAllTransparent);
    expect_straight(8);
    for (int y=0;y<image()->pixbuf->height();++y) for (int x=0;x<image()->pixbuf->width();++x)
        EXPECT_EQ(alpha_at(*image()->pixbuf,x,y),0u);
}

TEST_F(DestructiveBitmapClipStraightenTest, SkewBakesOpacityAndRetainsProfileWithUniformDpi)
{
    image()->getRepr()->setAttribute("transform","matrix(1 0 0.25 1 -3 0)");
    image()->getRepr()->setAttribute("style","opacity:0.5;image-rendering:optimizeSpeed");
    auto source=solid_pixbuf(128,96,0x336699ff);
    gchar *profile=nullptr; gsize length=0;
    ASSERT_TRUE(g_file_get_contents((std::string(INKSCAPE_TESTS_DIR)+"/data/colors/display.icc").c_str(),&profile,&length,nullptr));
    auto encoded=g_base64_encode(reinterpret_cast<guchar const *>(profile),length); g_free(profile);
    std::string const profile_option(encoded); g_free(encoded);
    ASSERT_TRUE(gdk_pixbuf_set_option(source->getPixbufRaw(),"icc-profile",profile_option.c_str()));
    ASSERT_TRUE(gdk_pixbuf_set_option(source->getPixbufRaw(),"x-dpi","300"));
    ASSERT_TRUE(gdk_pixbuf_set_option(source->getPixbufRaw(),"y-dpi","301"));
    setHrefAttribute(*image()->getRepr(),png_uri(*source).c_str()); checkpoint();
    ASSERT_EQ(commit(),BitmapClip::CommitStatus::CommittedStraightened);
    expect_straight(8);
    Pixbuf actual(*image()->pixbuf); actual.ensurePixelFormat(Pixbuf::PF_GDK);
    auto *raw=actual.getPixbufRaw(false);
    EXPECT_STREQ(gdk_pixbuf_get_option(raw,"icc-profile"),profile_option.c_str());
    EXPECT_STREQ(gdk_pixbuf_get_option(raw,"x-dpi"),"768");
    EXPECT_STREQ(gdk_pixbuf_get_option(raw,"y-dpi"),"768");
    EXPECT_EQ(std::string(image()->getRepr()->attribute("style")).find("opacity"),std::string::npos);
    auto const center=Geom::Point(16,12)*image()->pixelToDocumentAffine()->inverse();
    EXPECT_NEAR(alpha_at(actual,int(center.x()),int(center.y())),128,1);
}

TEST_F(DestructiveBitmapClipStraightenTest, MegapixelLimitRefusesBelowSideLimit)
{
    image()->getRepr()->setAttribute("transform","rotate(45 16 12)");
    image()->getRepr()->setAttributeSvgDouble("width",2.2);
    image()->getRepr()->setAttributeSvgDouble("height",0.01); checkpoint();
    auto const before=sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(commit(BitmapClip::Mode::KeepOutside),BitmapClip::CommitStatus::StraighteningTooLarge);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipStraightenTest, ObliqueCommitsInEveryAspectModeWithoutRevealingHiddenPixels)
{
    Geom::Rect const cutter_box(11, 7, 21, 17);
    for (auto const *aspect : {"none", "xMinYMin meet", "xMidYMid meet", "xMaxYMax meet",
                               "xMinYMin slice", "xMidYMid slice", "xMaxYMax slice"}) {
        SCOPED_TRACE(aspect);
        auto *repr = image()->getRepr();
        repr->setAttribute("preserveAspectRatio", aspect);
        repr->setAttributeSvgDouble("height", 8);
        checkpoint();
        auto expected = oracle(true);
        auto const original_inverse = image()->pixelToDocumentAffine()->inverse();
        auto const viewport_pixels = Geom::Rect(8,6,24,14) *
            (image()->i2doc_affine() * original_inverse);
        bool const slice = std::string(aspect).find("slice") != std::string::npos;
        double const density = slice ? 8 : 12;
        ASSERT_EQ(commit(), BitmapClip::CommitStatus::CommittedStraightened);
        expect_straight(density); expect_inside(cutter_box, density);
        Geom::Rect const viewport_box = Geom::Rect(8, 6, 24, 14) *
            (Geom::Translate(-16, -12) * Geom::Rotate::from_degrees(30) * Geom::Translate(16, 12));
        expect_inside(viewport_box, density);
        expect_render(*expected, cutter_box, true, true);
        // Explicitly inspect hidden source rows, away from the viewport AA edge.
        auto const mapping = *image()->pixelToDocumentAffine();
        unsigned hidden=0;
        for (int y=0;y<image()->pixbuf->height();++y) for (int x=0;x<image()->pixbuf->width();++x) {
            auto const src=Geom::Point(x+0.5,y+0.5)*mapping*original_inverse;
            if (src.x()<viewport_pixels.left()-1 || src.x()>viewport_pixels.right()+1 ||
                src.y()<viewport_pixels.top()-1 || src.y()>viewport_pixels.bottom()+1) {
                EXPECT_EQ(alpha_at(*image()->pixbuf,x,y),0u); ++hidden;
            }
        }
        if (slice) EXPECT_GT(hidden,0u);
        ASSERT_TRUE(DocumentUndo::undo(document.get())); document->ensureUpToDate();
        selection->setList(std::vector<SPItem *>{image(), cutter()});
    }
}

TEST_F(DestructiveBitmapClipStraightenTest, ObliqueAlphaMarginAndFiducialRegisterExactly)
{
    auto source = solid_pixbuf(128,96,0x0000ffff);
    for (int y=0; y<96; ++y) for (int x=0; x<128; ++x) {
        auto *p = pixel(*source,x,y);
        if (x<16 || y<12) std::fill_n(p,4,0);
        else if (x>=80 && x<83 && y>=60 && y<63) { p[0]=255; p[1]=p[2]=0; }
    }
    source->markDirty(); setHrefAttribute(*image()->getRepr(),png_uri(*source).c_str());
    auto *repr=cutter()->getRepr();
    repr->setAttributeSvgDouble("x",0); repr->setAttributeSvgDouble("y",0);
    repr->setAttributeSvgDouble("width",32); repr->setAttributeSvgDouble("height",24); checkpoint();
    auto const original_inverse = image()->pixelToDocumentAffine()->inverse();
    auto const dot_doc = Geom::Point(81.5,61.5) * original_inverse.inverse();
    auto const old_frame = frame();
    ASSERT_EQ(commit(),BitmapClip::CommitStatus::CommittedStraightened);
    expect_straight(8);
    EXPECT_GT(frame().left(),old_frame.left()+1/8.0);
    EXPECT_GT(frame().top(),old_frame.top()+1/8.0); EXPECT_LT(frame().width(),old_frame.width());
    Pixbuf actual(*image()->pixbuf); actual.ensurePixelFormat(Pixbuf::PF_GDK);
    double sx=0,sy=0; unsigned count=0;
    for (int y=0;y<actual.height();++y) for (int x=0;x<actual.width();++x) {
        auto *p=pixel(actual,x,y);
        if (p[0]>200 && p[2]<80 && p[3]) { sx+=x+0.5; sy+=y+0.5; ++count; }
    }
    ASSERT_GT(count,0u);
    auto const centroid=Geom::Point(sx/count,sy/count)* *image()->pixelToDocumentAffine();
    RecordProperty("fiducial_dx_doc",centroid.x()-dot_doc.x());
    RecordProperty("fiducial_dy_doc",centroid.y()-dot_doc.y());
    EXPECT_NEAR(centroid.x(),dot_doc.x(),0.3/8); EXPECT_NEAR(centroid.y(),dot_doc.y(),0.3/8);
}

TEST_F(DestructiveBitmapClipStraightenTest, ObliqueImageOwnClipAndMaskAreBakedThenStraightened)
{
    auto source=solid_pixbuf(128,96,0x336699ff);
    setHrefAttribute(*image()->getRepr(),png_uri(*source).c_str());
    auto *defs=document->getObjectById("defs1")->getRepr();
    auto *clip=document->getReprDoc()->createElement("svg:clipPath");
    clip->setAttribute("id","c"); clip->setAttribute("clipPathUnits","userSpaceOnUse");
    auto *rect=document->getReprDoc()->createElement("svg:rect");
    rect->setAttribute("x","8"); rect->setAttribute("y","6");
    rect->setAttribute("width","9.6"); rect->setAttribute("height","12");
    clip->appendChild(rect); GC::release(rect); defs->appendChild(clip); GC::release(clip);
    auto *mask=document->getReprDoc()->createElement("svg:mask");
    mask->setAttribute("id","m"); mask->setAttribute("maskUnits","userSpaceOnUse");
    mask->setAttribute("x","8"); mask->setAttribute("y","6");
    mask->setAttribute("width","16"); mask->setAttribute("height","12");
    for (int band=0;band<2;++band) {
        auto *r=document->getReprDoc()->createElement("svg:rect");
        r->setAttribute("x",band?"16":"8"); r->setAttribute("y","6");
        r->setAttribute("width","8"); r->setAttribute("height","12");
        r->setAttribute("fill","white"); r->setAttribute("fill-opacity",band?"0.5":"1");
        mask->appendChild(r); GC::release(r);
    }
    defs->appendChild(mask); GC::release(mask);
    image()->getRepr()->setAttribute("mask","url(#m)");
    image()->getRepr()->setAttribute("clip-path","url(#c)");
    auto *repr=cutter()->getRepr();
    repr->setAttributeSvgDouble("x",0); repr->setAttributeSvgDouble("y",0);
    repr->setAttributeSvgDouble("width",32); repr->setAttributeSvgDouble("height",24); checkpoint();
    auto const before=sp_repr_save_buf(document->getReprDoc()).raw();
    auto const original_inverse=image()->pixelToDocumentAffine()->inverse();
    ASSERT_EQ(commit(),BitmapClip::CommitStatus::CommittedStraightened);
    expect_straight(8);
    EXPECT_EQ(image()->getRepr()->attribute("clip-path"),nullptr);
    EXPECT_EQ(image()->getRepr()->attribute("mask"),nullptr);
    auto const mapping=*image()->pixelToDocumentAffine();
    unsigned outside=0,masked=0,opaque=0;
    for (int y=0;y<image()->pixbuf->height();++y) for (int x=0;x<image()->pixbuf->width();++x) {
        auto const src=Geom::Point(x+0.5,y+0.5)*mapping*original_inverse;
        if (src.y()<2 || src.y()>94 || src.x()<2) continue;
        auto const alpha=alpha_at(*image()->pixbuf,x,y);
        if (src.x()>79) { EXPECT_EQ(alpha,0u); ++outside; }
        else if (src.x()>66 && src.x()<74) { EXPECT_NEAR(alpha,128,1); ++masked; }
        else if (src.x()<62) { EXPECT_EQ(alpha,255u); ++opaque; }
    }
    EXPECT_GT(outside,0u); EXPECT_GT(masked,0u); EXPECT_GT(opaque,0u);
    ASSERT_TRUE(DocumentUndo::undo(document.get())); document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before);
}

TEST_F(DestructiveBitmapClipStraightenTest, NearNinetyDegreeMatrixKeepsByteIdenticalPixels)
{
    auto const transform=Geom::Translate(-16,-12)*Geom::Rotate::from_degrees(90.000001)*Geom::Translate(16,12);
    image()->getRepr()->setAttribute("transform",sp_svg_transform_write(transform)); document->ensureUpToDate();
    auto const mapping=*image()->pixelToDocumentAffine();
    auto *rect=cutter()->getRepr();
    rect->setAttributeSvgDouble("x",32); rect->setAttributeSvgDouble("y",24);
    rect->setAttributeSvgDouble("width",64); rect->setAttributeSvgDouble("height",48);
    rect->setAttribute("transform",sp_svg_transform_write(mapping)); checkpoint();
    auto const written=std::string(image()->getRepr()->attribute("transform"));
    Pixbuf original(*image()->pixbuf); original.ensurePixelFormat(Pixbuf::PF_GDK);
    ASSERT_EQ(commit(),BitmapClip::CommitStatus::Committed);
    ASSERT_EQ(image()->pixbuf->width(),64); ASSERT_EQ(image()->pixbuf->height(),48);
    Pixbuf actual(*image()->pixbuf); actual.ensurePixelFormat(Pixbuf::PF_GDK);
    for (int y=0;y<48;++y) EXPECT_EQ(std::memcmp(pixel(original,32,y+24),pixel(actual,0,y),64*4),0);
    EXPECT_STREQ(image()->getRepr()->attribute("transform"),written.c_str());
}

TEST_F(DestructiveBitmapClipStraightenTest, InteriorRectangularCutterGivesExactFrame)
{
    ASSERT_EQ(commit(),BitmapClip::CommitStatus::CommittedStraightened);
    expect_frame(Geom::Rect(11,7,21,17),8,1e-6);
}

TEST_F(DestructiveBitmapClipStraightenTest, PreRequestedCancelPreservesXmlSelectionAndHistory)
{
    auto const before=sp_repr_save_buf(document->getReprDoc()).raw();
    auto const selected=selection->items_vector();
    std::stop_source stop; stop.request_stop();
    EXPECT_EQ(BitmapClip::commit_selection(*selection,BitmapClip::Mode::KeepInside,stop.get_token()),
              BitmapClip::CommitStatus::Cancelled);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before);
    EXPECT_EQ(selection->items_vector(),selected); EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(DestructiveBitmapClipStraightenTest, SkewFlipParentIsCancelledInDocumentFrame)
{
    image()->getRepr()->removeAttribute("transform");
    document->getObjectById("parent")->getRepr()->setAttribute("transform","matrix(-1.25 0.15 0.3 0.8 8 12)");
    auto *repr=cutter()->getRepr();
    repr->setAttributeSvgDouble("x",-14); repr->setAttributeSvgDouble("y",21);
    repr->setAttributeSvgDouble("width",14); repr->setAttributeSvgDouble("height",8); checkpoint();
    ASSERT_EQ(commit(),BitmapClip::CommitStatus::CommittedStraightened);
    auto const composed=image()->transform*cast<SPItem>(image()->parent)->i2doc_affine();
    EXPECT_NEAR(composed[0],1,1e-6); EXPECT_NEAR(composed[1],0,1e-6);
    EXPECT_NEAR(composed[2],0,1e-6); EXPECT_NEAR(composed[3],1,1e-6);
    EXPECT_NEAR(composed[4],frame().left(),1e-6); EXPECT_NEAR(composed[5],frame().top(),1e-6);
}

TEST_F(DestructiveBitmapClipStraightenTest, NonIntegralRectangularCutterUsesPerAxisDensityAndExactFrame)
{
    Geom::Rect const box=Geom::Rect::from_xywh(11.031,7.123,9.731,9.643);
    auto *repr=cutter()->getRepr();
    repr->setAttributeSvgDouble("x",box.left()); repr->setAttributeSvgDouble("y",box.top());
    repr->setAttributeSvgDouble("width",box.width()); repr->setAttributeSvgDouble("height",box.height()); checkpoint();
    double const dx=std::ceil(box.width()*8)/box.width(), dy=std::ceil(box.height()*8)/box.height();
    ASSERT_EQ(commit(),BitmapClip::CommitStatus::CommittedStraightened);
    expect_straight(8); expect_frame(box,dx,1e-6);
    EXPECT_NEAR(1/(*image()->pixelToDocumentAffine())[3],dy,1e-6);
    auto const dpi=std::to_string(int(std::round(dx*96)));
    EXPECT_STREQ(gdk_pixbuf_get_option(image()->pixbuf->getPixbufRaw(),"x-dpi"),dpi.c_str());
    EXPECT_STREQ(gdk_pixbuf_get_option(image()->pixbuf->getPixbufRaw(),"y-dpi"),dpi.c_str());
}

TEST_F(DestructiveBitmapClipStraightenTest, HundredMegapixelCapRefusesBelowOldAreaAndSideCaps)
{
    image()->getRepr()->setAttribute("transform","rotate(45 16 12)");
    image()->getRepr()->setAttributeSvgDouble("width",1.6);
    image()->getRepr()->setAttributeSvgDouble("height",0.01); checkpoint();
    auto const l=*image()->pixelToDocumentAffine();
    double const density=std::max(1/std::hypot(l[0],l[1]),1/std::hypot(l[2],l[3]));
    auto const box=frame();
    double const w=std::ceil(box.width()*density), h=std::ceil(box.height()*density);
    ASSERT_LT(w,16384); ASSERT_LT(h,16384);
    ASSERT_GT(w*h,100000000); ASSERT_LT(w*h,150000000);
    auto const before=sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(commit(BitmapClip::Mode::KeepOutside),BitmapClip::CommitStatus::StraighteningTooLarge);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}
