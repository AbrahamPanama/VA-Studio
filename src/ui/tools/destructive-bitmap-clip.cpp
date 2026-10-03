// SPDX-License-Identifier: GPL-2.0-or-later

#include "destructive-bitmap-clip.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <new>
#include <limits>
#include <vector>
#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

#include "display/cairo-utils.h"

namespace Inkscape::UI::Tools::DestructiveBitmapClip {
namespace {

constexpr int MASK_TILE_SIZE = 256;
constexpr std::size_t RGBA_CHANNELS = 4;
// Straight RGBA (Pixbuf::PF_GDK) is the byte sequence R, G, B, A on every
// platform, unlike Cairo's premultiplied native-endian ARGB32 surface.
constexpr std::size_t STRAIGHT_ALPHA_OFFSET = 3;

struct PixelBounds {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] bool empty() const noexcept { return left >= right || top >= bottom; }
};

struct CutterBounds {
    bool valid = false;
    PixelBounds pixels;
};

CutterBounds cutter_bounds(Geom::PathVector const &path, int width, int height)
{
    auto const bounds = Geom::bounds_exact(path);
    if (!bounds || !bounds->isFinite()) {
        return {};
    }

    // Cairo's antialias filter can cover one neighboring pixel.
    PixelBounds pixels;
    pixels.left = static_cast<int>(std::clamp(std::floor(bounds->left()) - 1.0,
                                              0.0, static_cast<double>(width)));
    pixels.top = static_cast<int>(std::clamp(std::floor(bounds->top()) - 1.0,
                                             0.0, static_cast<double>(height)));
    pixels.right = static_cast<int>(std::clamp(std::ceil(bounds->right()) + 1.0,
                                               0.0, static_cast<double>(width)));
    pixels.bottom = static_cast<int>(std::clamp(std::ceil(bounds->bottom()) + 1.0,
                                                0.0, static_cast<double>(height)));
    return {true, pixels};
}

bool clear_pixels_outside(Inkscape::Pixbuf &pixels, PixelBounds const &keep,
                          std::stop_token cancellation, bool &changed)
{
    auto *data = pixels.pixels();
    auto const stride = pixels.rowstride();
    auto const width = pixels.width();
    auto const height = pixels.height();

    for (int y = 0; y < height; ++y) {
        if (cancellation.stop_requested()) {
            return false;
        }

        auto *row = data + static_cast<std::size_t>(y) * stride;
        auto const row_inside = y >= keep.top && y < keep.bottom;
        for (int x = 0; x < width; ++x) {
            if (row_inside && x >= keep.left && x < keep.right) {
                continue;
            }
            auto *pixel = row + static_cast<std::size_t>(x) * RGBA_CHANNELS;
            for (int channel = 0; channel < 4; ++channel) {
                changed |= pixel[channel] != 0;
                pixel[channel] = 0;
            }
        }
    }
    return true;
}

bool render_mask_tile(std::vector<unsigned char> &mask, int stride,
                      int left, int top, int width, int height,
                      Geom::PathVector const &path, FillRule fill_rule)
{
    std::fill(mask.begin(), mask.end(), 0);
    auto *surface = cairo_image_surface_create_for_data(mask.data(), CAIRO_FORMAT_A8,
                                                        width, height, stride);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) {
            cairo_surface_destroy(surface);
        }
        return false;
    }

    auto *context = cairo_create(surface);
    if (!context || cairo_status(context) != CAIRO_STATUS_SUCCESS) {
        if (context) {
            cairo_destroy(context);
        }
        cairo_surface_destroy(surface);
        return false;
    }

    cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
    cairo_set_antialias(context, CAIRO_ANTIALIAS_GOOD);
    cairo_set_fill_rule(context, fill_rule == FillRule::EvenOdd
                                     ? CAIRO_FILL_RULE_EVEN_ODD
                                     : CAIRO_FILL_RULE_WINDING);
    cairo_translate(context, -left, -top);
    cairo_new_path(context);
    feed_pathvector_to_cairo(context, path);
    cairo_fill(context);

    auto const succeeded = cairo_status(context) == CAIRO_STATUS_SUCCESS &&
                           cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(context);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    return succeeded;
}

/** Contract alpha multiply: floor((a * b + 127) / 255). */
unsigned char multiply_alpha(unsigned char a, unsigned char b)
{
    return static_cast<unsigned char>((static_cast<unsigned>(a) * b + 127u) / 255u);
}

/**
 * Apply the reviewed combined mask/clip/opacity coverage M over the whole
 * intrinsic grid. Only alpha is touched: B = mul(A, M). A pixel whose combined
 * alpha becomes zero is emitted as transparent black.
 */
bool apply_combined_coverage(Inkscape::Pixbuf &pixels, Coverage const &coverage,
                             std::stop_token cancellation, bool &changed)
{
    if (!coverage.pixels) {
        return true; // Full coverage: B == A, nothing to do.
    }

    auto *data = pixels.pixels();
    auto const pixel_stride = pixels.rowstride();
    auto const width = pixels.width();
    auto const height = pixels.height();

    for (int y = 0; y < height; ++y) {
        if (cancellation.stop_requested()) {
            return false;
        }
        auto *row = data + static_cast<std::size_t>(y) * pixel_stride;
        auto const *coverage_row = coverage.pixels + static_cast<std::size_t>(y) * coverage.stride;
        for (int x = 0; x < width; ++x) {
            auto const mask = coverage_row[x];
            if (mask == 255) {
                continue;
            }
            auto *pixel = row + static_cast<std::size_t>(x) * RGBA_CHANNELS;
            auto const base = multiply_alpha(pixel[STRAIGHT_ALPHA_OFFSET], mask);
            changed |= base != pixel[STRAIGHT_ALPHA_OFFSET];
            if (base == 0) {
                // A small source alpha and a small mask can round to zero; a
                // transparent result is black, never hidden source color.
                for (int channel = 0; channel < 4; ++channel) {
                    changed |= pixel[channel] != 0;
                    pixel[channel] = 0;
                }
            } else {
                // The mask can only lower alpha; the sample's straight RGB stays.
                pixel[STRAIGHT_ALPHA_OFFSET] = base;
            }
        }
    }
    return true;
}

/**
 * Apply cutter coverage C within the rendered bounds. With the base alpha B
 * already carrying the combined mask, inside = mul(B, C) and the inverse is the
 * exact integer remainder B - inside, so the two results recompose exactly.
 */
bool apply_cutter_tiles(Inkscape::Pixbuf &pixels, PixelBounds const &bounds,
                        Geom::PathVector const &path, FillRule fill_rule, Mode mode,
                        std::stop_token cancellation, bool &changed)
{
    if (bounds.empty()) {
        return true;
    }

    auto *data = pixels.pixels();
    auto const pixel_stride = pixels.rowstride();
    std::vector<unsigned char> mask;

    for (int top = bounds.top; top < bounds.bottom; top += MASK_TILE_SIZE) {
        if (cancellation.stop_requested()) {
            return false;
        }

        auto const tile_height = std::min(MASK_TILE_SIZE, bounds.bottom - top);
        for (int left = bounds.left; left < bounds.right; left += MASK_TILE_SIZE) {
            if (cancellation.stop_requested()) {
                return false;
            }

            auto const tile_width = std::min(MASK_TILE_SIZE, bounds.right - left);
            auto const mask_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, tile_width);
            if (mask_stride <= 0) {
                return false;
            }

            try {
                mask.resize(static_cast<std::size_t>(mask_stride) * tile_height);
            } catch (std::bad_alloc const &) {
                return false;
            }
            if (!render_mask_tile(mask, mask_stride, left, top, tile_width, tile_height,
                                  path, fill_rule)) {
                return false;
            }

            for (int local_y = 0; local_y < tile_height; ++local_y) {
                if (cancellation.stop_requested()) {
                    return false;
                }

                auto *row = data + static_cast<std::size_t>(top + local_y) * pixel_stride +
                            static_cast<std::size_t>(left) * RGBA_CHANNELS;
                auto const *coverage_row = mask.data() + static_cast<std::size_t>(local_y) * mask_stride;
                for (int local_x = 0; local_x < tile_width; ++local_x) {
                    auto const cutter = coverage_row[local_x];
                    auto *pixel = row + static_cast<std::size_t>(local_x) * RGBA_CHANNELS;
                    auto const base = pixel[STRAIGHT_ALPHA_OFFSET];
                    auto const inside = multiply_alpha(base, cutter);
                    auto const result = mode == Mode::KeepInside
                                            ? inside
                                            : static_cast<unsigned char>(base - inside);
                    changed |= result != base;
                    if (result == 0) {
                        // Transparent output is black; never keep hidden color.
                        for (int channel = 0; channel < 4; ++channel) {
                            changed |= pixel[channel] != 0;
                            pixel[channel] = 0;
                        }
                    } else {
                        pixel[STRAIGHT_ALPHA_OFFSET] = result;
                    }
                }
            }
        }
    }
    return true;
}

/**
 * Contract normalization: every pixel whose final alpha is zero is transparent
 * black, independent of mode, coverage and whether the tiled cutter pass
 * touched it. This covers source-transparent pixels (A == 0) and KeepOutside
 * pixels that lie outside the cutter bounds, neither of which is visited by the
 * mode-specific passes. Nonzero-alpha pixels are left byte-for-byte untouched,
 * so retained straight RGB is never modified. Returns false on cancellation,
 * before any partial result can be published.
 */
bool normalize_transparent_pixels(Inkscape::Pixbuf &pixels, std::stop_token cancellation,
                                  bool &changed)
{
    auto *data = pixels.pixels();
    auto const stride = pixels.rowstride();
    auto const width = pixels.width();
    auto const height = pixels.height();

    for (int y = 0; y < height; ++y) {
        if (cancellation.stop_requested()) {
            return false;
        }
        auto *row = data + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            auto *pixel = row + static_cast<std::size_t>(x) * RGBA_CHANNELS;
            if (pixel[STRAIGHT_ALPHA_OFFSET] != 0) {
                continue;
            }
            for (int channel = 0; channel < 4; ++channel) {
                changed |= pixel[channel] != 0;
                pixel[channel] = 0;
            }
        }
    }
    return true;
}

PixelBounds alpha_bounds(Inkscape::Pixbuf const &pixels, std::stop_token cancellation,
                         bool &cancelled)
{
    auto const *data = pixels.pixels();
    auto const stride = pixels.rowstride();
    PixelBounds bounds{pixels.width(), pixels.height(), 0, 0};
    for (int y = 0; y < pixels.height(); ++y) {
        if (cancellation.stop_requested()) {
            cancelled = true;
            return {};
        }
        auto const *row = data + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < pixels.width(); ++x) {
            if (row[static_cast<std::size_t>(x) * RGBA_CHANNELS + STRAIGHT_ALPHA_OFFSET] != 0) {
                bounds.left = std::min(bounds.left, x);
                bounds.top = std::min(bounds.top, y);
                bounds.right = std::max(bounds.right, x + 1);
                bounds.bottom = std::max(bounds.bottom, y + 1);
            }
        }
    }
    return bounds;
}

// The contract returns a detached straight-RGBA (PF_GDK) buffer. Allocate only
// the crop-sized GdkPixbuf, copy exact rows from the straight working buffer and
// reuse the metadata whitelist. A Cairo ARGB32 surface here would reintroduce
// the premultiply/unpremultiply round trip the engine exists to avoid.
std::shared_ptr<Inkscape::Pixbuf> crop_pixels(Inkscape::Pixbuf &source,
                                           PixelBounds const &bounds,
                                           std::stop_token cancellation)
{
    auto *raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8,
                               bounds.right - bounds.left, bounds.bottom - bounds.top);
    if (!raw) {
        return {};
    }
    std::shared_ptr<Inkscape::Pixbuf> cropped;
    try {
        cropped = std::make_shared<Inkscape::Pixbuf>(raw); // takes ownership
    } catch (...) {
        g_object_unref(raw);
        throw;
    }
    for (int y = 0; y < cropped->height(); ++y) {
        if (cancellation.stop_requested()) {
            return {};
        }
        std::memcpy(cropped->pixels() + static_cast<std::size_t>(y) * cropped->rowstride(),
                    source.pixels() + static_cast<std::size_t>(y + bounds.top) * source.rowstride() +
                        static_cast<std::size_t>(bounds.left) * RGBA_CHANNELS,
                    static_cast<std::size_t>(cropped->width()) * RGBA_CHANNELS);
    }
    auto *source_raw = source.getPixbufRaw(false);
    auto *cropped_raw = cropped->getPixbufRaw(false);
    Inkscape::copy_supported_pixbuf_metadata(source_raw, cropped_raw);
    for (auto const *key : {"icc-profile", "x-dpi", "y-dpi"}) {
        if (g_strcmp0(gdk_pixbuf_get_option(source_raw, key), gdk_pixbuf_get_option(cropped_raw, key)) != 0) {
            return {}; // Never publish a crop that lost available color metadata.
        }
    }
    cropped->markDirty();
    return cropped;
}

Result apply_impl(Inkscape::Pixbuf const &source, Coverage const &combined_coverage,
                  Geom::PathVector const &cutter_pixels, FillRule fill_rule, Mode mode,
                  std::stop_token cancellation)
{
    if (source.width() <= 0 || source.height() <= 0 || cutter_pixels.empty() ||
        static_cast<std::size_t>(source.width()) > static_cast<std::size_t>(source.rowstride()) / 4 ||
        source.rowstride() <= 0 || static_cast<std::size_t>(source.height()) >
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(source.rowstride())) {
        return {.status = Status::InvalidInput};
    }
    if (combined_coverage.pixels &&
        (combined_coverage.width != source.width() || combined_coverage.height != source.height() ||
         combined_coverage.stride < source.width())) {
        return {.status = Status::InvalidInput};
    }
    if (cancellation.stop_requested()) {
        return {.status = Status::Cancelled};
    }

    auto const bounds = cutter_bounds(cutter_pixels, source.width(), source.height());
    if (!bounds.valid) {
        return {.status = Status::InvalidInput};
    }

    std::shared_ptr<Inkscape::Pixbuf> output;
    try {
        output = std::make_shared<Inkscape::Pixbuf>(source);
    } catch (std::bad_alloc const &) {
        return {.status = Status::RasterizationFailed};
    }
    // Detached straight RGBA. If the source is already PF_GDK this is a no-op;
    // if a decoder handed us Cairo's premultiplied bytes this is the single
    // documented conversion and no further quantization happens in the engine.
    output->ensurePixelFormat(Inkscape::Pixbuf::PF_GDK);

    bool changed = false;
    if (!apply_combined_coverage(*output, combined_coverage, cancellation, changed)) {
        return {.status = Status::Cancelled};
    }
    if (mode == Mode::KeepInside &&
        !clear_pixels_outside(*output, bounds.pixels, cancellation, changed)) {
        return {.status = Status::Cancelled};
    }
    if (!apply_cutter_tiles(*output, bounds.pixels, cutter_pixels, fill_rule, mode,
                            cancellation, changed)) {
        return {.status = cancellation.stop_requested()
                              ? Status::Cancelled
                              : Status::RasterizationFailed};
    }
    // FINDING-1 normalization: the whole-grid pass above is intentionally last
    // so every zero-alpha pixel is transparent black regardless of mode,
    // coverage or cutter bounds.
    if (!normalize_transparent_pixels(*output, cancellation, changed)) {
        return {.status = Status::Cancelled};
    }

    bool cancelled = false;
    auto crop = alpha_bounds(*output, cancellation, cancelled);
    auto const all_transparent = crop.empty();
    if (cancelled) {
        return {.status = Status::Cancelled};
    }
    if (changed) {
        output->markDirty();
    }
    if (all_transparent) {
        crop = {0, 0, source.width(), source.height()};
    } else if (crop.left != 0 || crop.top != 0 || crop.right != source.width() || crop.bottom != source.height()) {
        try {
            output = crop_pixels(*output, crop, cancellation);
        } catch (std::bad_alloc const &) {
            return {.status = Status::RasterizationFailed};
        }
        if (!output) {
            return {.status = cancellation.stop_requested() ? Status::Cancelled : Status::RasterizationFailed};
        }
        changed = true; // A pending trim is a change even when alpha was unchanged.
    }
    return {
        .status = Status::Completed,
        .pixels = std::move(output),
        .changed = changed,
        .all_transparent = all_transparent,
        .original_width = source.width(),
        .original_height = source.height(),
        .left = crop.left,
        .top = crop.top
    };
}

} // namespace

bool Result::trimmed() const noexcept
{
    return pixels && (pixels->width() != original_width || pixels->height() != original_height);
}

Result apply(Inkscape::Pixbuf const &source, Geom::PathVector const &cutter_pixels,
             FillRule fill_rule, Mode mode, std::stop_token cancellation)
{
    return apply_impl(source, Coverage{}, cutter_pixels, fill_rule, mode, cancellation);
}

Result apply(Inkscape::Pixbuf const &source, Coverage const &combined_coverage,
             Geom::PathVector const &cutter_pixels, FillRule fill_rule, Mode mode,
             std::stop_token cancellation)
{
    return apply_impl(source, combined_coverage, cutter_pixels, fill_rule, mode, cancellation);
}

} // namespace Inkscape::UI::Tools::DestructiveBitmapClip
