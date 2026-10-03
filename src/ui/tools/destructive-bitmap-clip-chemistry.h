// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_DESTRUCTIVE_BITMAP_CLIP_CHEMISTRY_H
#define INKSCAPE_UI_TOOLS_DESTRUCTIVE_BITMAP_CLIP_CHEMISTRY_H

#include "destructive-bitmap-clip.h"
#include "destructive-bitmap-coverage.h"

#include <stop_token>
#include <vector>

class SPImage;
class SPItem;
class SPDocument;

namespace Inkscape {
class Selection;
}

namespace Inkscape::UI::Tools::DestructiveBitmapClip {

enum class CommitStatus {
    Committed,
    CommittedAllTransparent,
    CommittedStraightened,
    CommittedStraightenedAllTransparent,
    /// An oblique bitmap has no target region (all-transparent/disjoint extent).
    StraighteningEmptyRegion,
    /// The straight grid exceeds 16384 pixels per side or 100 megapixels.
    StraighteningTooLarge,
    NoChange,
    InvalidSelection,
    InvalidGeometry,
    UnsupportedTrim,
    RasterizationFailed,
    EncodingFailed,
    /// The operation observed a stop request before publication. No document,
    /// selection or undo state changed.
    Cancelled
};

/**
 * The single shared mapping from a coverage-evaluation status to the commit
 * outcome that `commit_selection` reports for it.
 *
 * `commit_selection` and the `vacards-destructive-clip` dry-run preflight both
 * derive their refusal from this one function, so a predicted refusal cannot
 * drift from the real transaction. It mirrors the historical switch: a failed
 * coverage evaluation becomes the same status the commit would return for it,
 * and `Completed` (which the commit never reaches through this path) falls
 * through to `RasterizationFailed`.
 */
[[nodiscard]] CommitStatus commit_status_for(DestructiveBitmapCoverage::Status status) noexcept;

/** Validate the exact one-bitmap/one-vector V1 selection contract. */
[[nodiscard]] bool selection_is_eligible(Inkscape::Selection &selection) noexcept;

/**
 * Why the deterministic bitmap-branch/cutter pair was or was not resolved.
 *
 * `Resolved` means the two selected roots form exactly one directly selected
 * bitmap or one exclusively owned single-image wrapper plus one supported
 * closed cutter. Every other status is an atomic, read-only rejection: the
 * document, selection and undo history are not touched. `UnsupportedBranch`
 * is also the fail-closed result when resolution itself throws.
 */
enum class TargetStatus {
    Resolved,
    /// The selection is not exactly two distinct item roots.
    NotPair,
    /// One selected root is an ancestor of the other (parent plus child).
    NestedOrDuplicateRoots,
    /// Both roots look like image branches, or neither does.
    AmbiguousImageBranch,
    /// A single selected branch contains more than one image.
    MultipleImages,
    /// The image has no usable decoded source pixels.
    MissingImage,
    /// A hidden/locked image, wrapper chain member, cutter or cutter descendant.
    ProtectedObject,
    /// A wrapper chain crosses a shared/mixed/unsupported context, or a clone
    /// or outside reference would be altered by collapsing it. Also used when a
    /// compositing context (mask/clip/filter/opacity/blend) sits above the
    /// selected wrapper and is therefore shared with the rest of the document.
    UnsupportedBranch,
    /// The cutter supplies no single supported closed region, including a
    /// hidden/masked/filtered member that `OffsetShapes::prepare` would drop.
    UnsupportedCutter,
    /// The image's pixel-to-document mapping is singular or nonfinite.
    InvalidGeometry
};

/**
 * Read-only result of resolving a bitmap-branch/cutter selection.
 *
 * `image` is the intrinsic bitmap to bake; `branch` is the selected root whose
 * pixels are converted (the image itself or its exclusive wrapper); `cutter` is
 * the selected root supplying the closed region. Pointers are null unless the
 * corresponding role resolved. No object or document state is modified.
 */
struct ResolvedTargets {
    SPImage *image = nullptr;
    SPItem *branch = nullptr;
    SPItem *cutter = nullptr;
    TargetStatus status = TargetStatus::NotPair;
};

/**
 * Deterministically resolve one bitmap branch and one supported closed cutter
 * from a two-root selection without choosing first descendants or nearest
 * objects. Resolution is read-only and safe to call from the action-sensitivity
 * path; cutter geometry preparation is cheap and no coverage is rasterized.
 *
 * This is the narrow two-role pair exception documented in
 * `doc/vacards/SELECTION_CONTRACT.md`; `Util::resolve_composite_targets` does
 * not fit because the two roles are not independent composited roots.
 */
[[nodiscard]] ResolvedTargets resolve_targets(Inkscape::Selection &selection) noexcept;

/**
 * Same resolution over an explicit, unnormalized root list.
 *
 * A live `Selection` already collapses parent/child roots before the adapter
 * sees them; this overload lets callers and tests supply the acyclic root list
 * directly, so duplicate and ancestor/descendant roots get the explicit
 * `NestedOrDuplicateRoots` reason. It performs the same read-only checks.
 */
[[nodiscard]] ResolvedTargets resolve_targets(std::vector<SPItem *> const &selected,
                                              SPDocument &document) noexcept;

/// Transaction preconditions checked after a pair resolves: the image must be selected directly (a
/// single-image wrapper group is not collapsed) and neither the image nor its branch may be referenced
/// or cloned by unselected objects. The cutter is kept unchanged (CLIP-1), so references to it are
/// allowed. commit_selection() refuses with InvalidSelection otherwise; dry runs use this to predict
/// that refusal.
[[nodiscard]] bool commit_preconditions_hold(SPImage const *image, SPItem const *branch,
                                             SPItem const *cutter) noexcept;

/**
 * Synchronous BITMAP-001/002/003 document adapter.
 *
 * Pixels, crop geometry and PNG encoding are staged before publication. A
 * successful mutation replaces the directly selected bitmap payload, bakes any
 * private mask/clip/opacity coverage into its alpha and removes those now-baked
 * references, then selects the bitmap and records exactly one Undo entry. The
 * cutter is kept unchanged (CLIP-1). Oblique pixel axes are resampled into a
 * document-aligned grid at source density after baking coverage; orthogonal
 * axes retain the exact source-grid crop. A result with unchanged pixels and no trim
 * returns NoChange. Only the four Committed statuses mutate the document.
 * Every other status mutates nothing, selection included. UnsupportedTrim requires resource/viewport rebasing that has not
 * been qualified; it does not silently fall back to an untrimmed edit.
 *
 * A selected single-image wrapper group resolves (see resolve_targets) but is
 * rejected atomically by the transaction: collapsing/reparenting the wrapper is
 * not yet proven safe. A stop request is observed before publication.
 *
 * The two-argument form is retained as the action entry point; the three-
 * argument form exposes a stop token for cancellation-aware callers and tests.
 */
[[nodiscard]] CommitStatus commit_selection(Inkscape::Selection &selection, Mode mode);

[[nodiscard]] CommitStatus commit_selection(Inkscape::Selection &selection, Mode mode,
                                            std::stop_token cancellation);

} // namespace Inkscape::UI::Tools::DestructiveBitmapClip

#endif // INKSCAPE_UI_TOOLS_DESTRUCTIVE_BITMAP_CLIP_CHEMISTRY_H
