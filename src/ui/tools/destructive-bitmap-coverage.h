// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_DESTRUCTIVE_BITMAP_COVERAGE_H
#define INKSCAPE_UI_TOOLS_DESTRUCTIVE_BITMAP_COVERAGE_H

#include <optional>
#include <stop_token>
#include <vector>

#include "ui/tools/destructive-bitmap-clip.h"

#include <2geom/affine.h>
#include <cstdint>

class SPClipPath;
class SPImage;
class SPItem;

namespace Inkscape::UI::Tools::DestructiveBitmapCoverage {

/**
 * Why a coverage evaluation did not produce detached samples.
 *
 * Every failure is explicit: the helper never silently drops a mask/clip, and
 * never substitutes full coverage for an unevaluatable context.
 */
enum class Status {
    Completed,
    Cancelled,
    InvalidInput,
    MissingImage,
    UnsupportedSharedAncestor,
    UnsupportedEffect,
    SingularMapping,
    RasterizationFailed
};

/**
 * Detached combined mask/clip/opacity coverage on the source pixel grid.
 *
 * `pixels` is a contiguous 8-bit plane with stride == width and one sample per
 * intrinsic source pixel; sample (x, y) is `pixels[y * width + x]`. The buffer
 * is owned by this result and is safe to hand to a worker thread. On failure
 * `pixels` is empty and no partial plane is exposed.
 */
struct Result {
    Status status = Status::InvalidInput;
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool completed() const noexcept
    {
        return status == Status::Completed && width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) * height;
    }

    /**
     * A B01 `Coverage` view over the detached plane, or `std::nullopt` when the
     * evaluation did not complete.
     *
     * The result is status-gated on purpose: B01's `Coverage{}` (null pointer)
     * means "full coverage", so it must be impossible to forward a failed
     * evaluation as full coverage. A caller has to observe `completed()` (or
     * switch on `status`) before it can obtain the coverage; `view()` yields
     * no value for every non-`Completed` status and for a malformed plane.
     */
    [[nodiscard]] std::optional<DestructiveBitmapClip::Coverage> view() const noexcept
    {
        if (!completed()) {
            return std::nullopt;
        }
        return DestructiveBitmapClip::Coverage{pixels.data(), width, width, height};
    }
};

/**
 * Evaluate the existing SVG mask/clip/opacity coverage that applies to
 * @p source along the branch `branch -> ... -> source`.
 *
 * @p branch must be @p source or one of its ancestors. The returned samples are
 * the product, on the intrinsic source W x H grid, of every `opacity`, clip
 * path and luminance/alpha mask carried by the items on that chain, using the
 * same native `DrawingItem::render()` compositing the canvas uses. Source RGB
 * is never sampled or resampled: the coverage view renders opaque white through
 * the native clip/mask/opacity pipeline only. Live `SPDocument`/`SPDrawing`
 * access stays on the calling (application main) thread; only the returned
 * buffer leaves it.
 *
 * Each unsupported context yields a typed Status instead of removing or
 * ignoring a mask: a compositing context on an ancestor above @p branch
 * (`UnsupportedSharedAncestor`), a colour-changing filter or non-normal blend
 * on the chain or non-exclusive branch content (`UnsupportedEffect`), missing
 * image data (`MissingImage`), a nonfinite/singular pixel mapping
 * (`SingularMapping`), cancellation (`Cancelled`) or allocation/cairo failure
 * (`RasterizationFailed`).
 *
 * Hidden handling matches native `sp-item.cpp::invoke_show`, which sets
 * visibility from `SPItem::isHidden()`: a hidden chain item (including a hidden
 * @p branch or @p source) yields a completed, all-zero coverage plane rather
 * than full coverage. A hidden compositing ancestor above @p branch is rejected
 * as `UnsupportedSharedAncestor`.
 *
 * Mask `x`/`y`/`width`/`height` region attributes are deliberately not applied:
 * native Inkscape live rendering (and `SPMask`) ignores the region rectangle, so
 * this helper matches the canvas rather than implementing SVG-spec clamping.
 *
 * Cancellation is observed on entry, after the private view is built (before
 * `Drawing::update`/`render`), and between output scanlines. The native
 * `Drawing::update`/`render` call itself is not interruptible: once it starts it
 * runs to completion before a later stop request can be observed. On any
 * cancellation the returned `pixels` plane is empty.
 */
[[nodiscard]] Result evaluate(SPItem &branch, SPImage &source,
                              std::stop_token cancellation = {});

// Additive EB2 plain geometry; DC's existing API/support policy is unchanged.
struct GridGeometry {
    std::uint32_t width = 0, height = 0;
    Geom::Affine pixelToDocument;
    bool straightened = false;
};
// EB2 main admission: only measured plain shape/group trees; resource expansion refuses.
struct ClipComplexity {
    std::uint64_t objects = 0, pathNodes = 0, styleEntries = 0, shapes = 0, groups = 0;
    std::uint64_t bytes = 0, units = 0;
};
// Mac M2 crossing/even-odd calibration: worst 6572 ns/unit; round up
// generously before the additional 2x headroom. EB4 calibrates other platforms.
inline constexpr std::uint64_t ClipUnitNanoseconds = 20000;
inline constexpr std::uint64_t ClipMainUnitLimit = 100000000 / (2*ClipUnitNanoseconds); // 2x headroom
char const *estimateGridClip(SPClipPath *, unsigned, unsigned, ClipComplexity &);
bool gridMappingQualified(Geom::Affine const &);
// DC-S1 density, transformed extent and whole-pixel span rounding. Exact document
// axis-aligned grids (including flips/quarter turns) keep all intrinsic samples.
std::optional<GridGeometry> gridGeometry(Geom::Affine const &, unsigned, unsigned);

/// The cheap, non-rendering refusals of evaluate() (missing image, singular mapping, invalid chain, shared
/// compositing ancestor, unsupported effect). Completed when evaluate() would get past them.
[[nodiscard]] Status preflight(SPItem &branch, SPImage &source);

} // namespace Inkscape::UI::Tools::DestructiveBitmapCoverage

#endif // INKSCAPE_UI_TOOLS_DESTRUCTIVE_BITMAP_COVERAGE_H
