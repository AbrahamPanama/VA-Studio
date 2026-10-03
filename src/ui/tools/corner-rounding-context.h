// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_CORNER_ROUNDING_CONTEXT_H
#define INKSCAPE_UI_TOOLS_CORNER_ROUNDING_CONTEXT_H
#include <cstddef>
#include <optional>
#include <string>
#include "live_effects/fillet-chamfer-edit.h"
class SPShape;
namespace Inkscape::UI { class ControlPointSelection; class Node; }
namespace Inkscape::UI::Tools {
struct CornerRoundingContext {
    std::optional<LivePathEffect::CornerEdit::Snapshot> snapshot;
    std::optional<double> input_to_document_scale;
    std::string reason;
    bool non_similarity = false; ///< refused only because of nonuniform scale or skew
};
// Read-only, synchronous owning-thread capture. Borrowed path/selection and
// their private/registered document owner must survive the call; no callbacks,
// ensureUpToDate, selection edits, default changes or XML writes occur here.
// Native shape/path with no effect, or one private Corners effect.
CornerRoundingContext capture_corner_rounding(SPShape &path, ControlPointSelection const &nodes);
// Document-level part of capture_corner_rounding: the same checks, in the same
// order and with the same reason strings, up to (and not including) the
// ControlPointSelection/PathManipulator loop. A native non-SPPath shape yields a
// ready snapshot{geometry, satellites, {}, {}}; an SPPath yields no snapshot and
// the "paths need their node types" reason, because its node identity comes from
// the Node tool, not this document capture. @a node_limit_hint replaces the Node
// selection size (pass the selection's allPoints().size(), or 0 without a tool).
CornerRoundingContext capture_corner_rounding_document(SPShape &shape, std::size_t node_limit_hint);
LivePathEffect::CornerEdit::Address corner_rounding_address(Node const &node);
} // namespace Inkscape::UI::Tools
#endif
