// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_NESTING_CAPTURE_H
#define INKSCAPE_NESTING_CAPTURE_H

/*
 * Plain data captured from the document for off-thread geometry preparation
 * (consolidated nesting work order 8.7, R1). Nothing here points into the
 * document: no SPObject, SPWeakPtr, XML node or Inkscape::Pixbuf. Paths are
 * built as `path * affine` (which unshares lib2geom's copy-on-write data) and
 * bitmap alpha is copied, so the GTK thread may edit or re-render the document
 * while a worker reads these records.
 *
 * Stroke-to-path (item_find_paths) and the fill/stroke union stay in the
 * capture: livarot's Path::Outline keeps mutable function-local static state
 * (TurnInside/PrevPos in PathOutline.cpp), which is neither thread-safe nor
 * history-free, so it must run on one thread in the original order.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <2geom/affine.h>
#include <2geom/pathvector.h>
#include <2geom/rect.h>

#include "nesting-types.h"

namespace Inkscape::Nesting {

/// A bitmap's alpha channel, row by row (width x height bytes).
struct AlphaPlane
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha;
    std::uint64_t content_hash = 0;
};

/// A traced alpha boundary, shared by every bitmap with the same pixels.
struct AlphaTrace;

/**
 * One captured object and its visible item children: everything the part
 * decision tree (explicit contour, clip, bitmap, group children, vector
 * outline, conservative bounds) reads from the document.
 */
struct CapturedLeaf
{
    std::string label;
    bool group = false;
    bool image = false;
    bool explicit_contour = false; // inkscape:nesting-contour="true"
    bool clip_or_mask = false;
    bool clip = false;             // a clip object exists
    Geom::PathVector clip_path;    // document coordinates; empty when unusable
    Geom::PathVector path;         // the vector outline (fill with its stroke), document coordinates
    int wind_rule = 0;             // SPWindRule of the fill
    Geom::OptRect visual_bounds;   // document coordinates
    bool vector_content = false;   // renders non-bitmap content
    // Bitmaps.
    bool pixels = false;
    std::optional<Geom::Affine> pixel_to_document;
    /// Copied unless `trace` was already known; one plane per distinct bitmap
    /// within a capture (copies of the same sticker share it).
    std::shared_ptr<AlphaPlane const> alpha;
    std::shared_ptr<AlphaTrace const> trace;
    std::vector<CapturedLeaf> children;   // visible item children
};

/// A selected part or an obstacle.
struct CapturedItem
{
    std::string label;
    CapturedLeaf root;
    bool ignore_bitmaps = false;          // a group with vector artwork (S10)
    Geom::OptRect ignored_bitmap_bounds;  // for the sparse-vector warning
};

/// One entry of the selection, in selection order.
struct CapturedCandidate
{
    bool captured = false;                // false: skipped during capture
    int skipped_reason = 0;               // SkippedPartReason when not captured
    std::string skipped_detail;
    CapturedItem item;
};

/// One obstacle, in the order given.
struct CapturedObstacle
{
    bool ignored = false;                 // null, the sheet, a part, foreign or hidden
    std::optional<std::string> error;     // captured failure (non-finite transform)
    CapturedItem item;
};

struct CapturedInput
{
    Geom::PathVector container_path;      // document coordinates
    int container_wind_rule = 0;
    std::vector<CapturedCandidate> candidates;
    std::vector<CapturedObstacle> obstacles;
    double flatten_tolerance = 0.05;
    bool reject_conservative = false; // request-local; GUI keeps existing recovery
};

} // namespace Inkscape::Nesting

#endif // INKSCAPE_NESTING_CAPTURE_H
