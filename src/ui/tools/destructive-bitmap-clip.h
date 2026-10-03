// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_DESTRUCTIVE_BITMAP_CLIP_H
#define INKSCAPE_UI_TOOLS_DESTRUCTIVE_BITMAP_CLIP_H

#include <memory>
#include <stop_token>
#include <2geom/pathvector.h>

namespace Inkscape {
class Pixbuf;
}

namespace Inkscape::UI::Tools::DestructiveBitmapClip {

/** Which side of the cutter remains visible in the returned bitmap. */
enum class Mode {
    KeepInside,
    KeepOutside
};

/** Fill semantics captured from the detached vector cutter. */
enum class FillRule {
    NonZero,
    EvenOdd
};

enum class Status {
    Completed,
    Cancelled,
    InvalidInput,
    RasterizationFailed
};

/**
 * Reviewed combined mask/clip/opacity coverage on the intrinsic source grid.
 *
 * One 8-bit sample per source pixel; sample (x, y) is read from
 * `pixels[y * stride + x]`. A null `pixels` pointer requests full coverage
 * (255) and the dimension fields are then ignored. The caller owns the buffer;
 * the helper only reads it. This is the detached coverage interface B02
 * produces; it is intentionally independent of any live document object.
 */
struct Coverage {
    unsigned char const *pixels = nullptr;
    int stride = 0;
    int width = 0;
    int height = 0;
};

/**
 * Result of the detached pixel operation.
 *
 * A completed result always owns a new Pixbuf, including for a no-op. Nonempty
 * results are cropped to the smallest rectangle containing alpha > 0; offsets
 * retain their source-grid mapping. Failed and cancelled operations never
 * expose a partially modified buffer.
 */
struct Result {
    Status status = Status::InvalidInput;
    std::shared_ptr<Inkscape::Pixbuf> pixels;
    bool changed = false;
    bool all_transparent = false;
    // Pixels are cropped to nonzero alpha; these locate them in the source
    // grid. An empty result retains the source extent and zero offsets.
    int original_width = 0;
    int original_height = 0;
    int left = 0;
    int top = 0;

    [[nodiscard]] bool trimmed() const noexcept;

    [[nodiscard]] bool completed() const noexcept
    {
        return status == Status::Completed && static_cast<bool>(pixels);
    }
};

/**
 * Apply vector cutter coverage to a detached straight-RGBA copy of @p source.
 *
 * The path is expressed in intrinsic image-pixel coordinates and supplies the
 * cutter coverage C. The source alpha A is combined with the cutter as
 * `inside = mul(A, C)` and, for KeepOutside, `inverse = A - inside` with
 * `mul(a, b) = floor((a * b + 127) / 255)`. Source straight RGB is preserved
 * byte-for-byte wherever the final alpha is nonzero; pixels whose final alpha
 * is zero become transparent black (0, 0, 0, 0). This overload is the
 * full-coverage (M = 255) case.
 *
 * The caller-owned source is never changed. The returned Pixbuf uses the
 * detached straight-RGBA (`PF_GDK`) representation. Work is tiled to bound mask
 * memory and observes @p cancellation between scanlines and tiles, including
 * the alpha-bounds scan and exact row-copy crop. No resampling is performed.
 */
[[nodiscard]] Result apply(Inkscape::Pixbuf const &source,
                           Geom::PathVector const &cutter_pixels,
                           FillRule fill_rule,
                           Mode mode,
                           std::stop_token cancellation = {});

/**
 * Apply combined mask/clip/opacity coverage @p combined_coverage (M) together
 * with vector cutter coverage C to a detached straight-RGBA copy of @p source.
 *
 * For source alpha A, mask coverage M and cutter coverage C:
 * `B = mul(A, M)`, `KeepInside = mul(B, C)` and `KeepOutside = B - mul(B, C)`.
 * Source straight RGB is preserved wherever the final alpha is nonzero and
 * transparent output pixels are black. `Coverage{}` (a null buffer) selects
 * full coverage and is equivalent to the overload above.
 */
[[nodiscard]] Result apply(Inkscape::Pixbuf const &source,
                           Coverage const &combined_coverage,
                           Geom::PathVector const &cutter_pixels,
                           FillRule fill_rule,
                           Mode mode,
                           std::stop_token cancellation = {});

} // namespace Inkscape::UI::Tools::DestructiveBitmapClip

#endif // INKSCAPE_UI_TOOLS_DESTRUCTIVE_BITMAP_CLIP_H
