// SPDX-License-Identifier: GPL-2.0-or-later
#include "corner-rounding-context.h"
#include <algorithm>
#include <limits>
#include <glibmm/i18n.h>
#include "helper/geom.h"
#include "live_effects/lpe-fillet-chamfer.h"
#include "object/sp-path.h"
#include "object/sp-polygon.h"
#include "object/sp-polyline.h"
#include "ui/tool/control-point-selection.h"
#include "ui/tool/node.h"
#include "ui/tool/path-manipulator.h"

namespace Inkscape::UI::Tools {
namespace CE = LivePathEffect::CornerEdit;

namespace {
// The document-level corner capture: every check that does not need the Node
// tool, plus the effect-input derivation those checks validate. `ok` means every
// check passed and `geometry`/`satellites` are the effect input the Node tool or
// the command line will target. This is the one implementation of the shared
// checks; capture_corner_rounding_document and capture_corner_rounding both
// build on it so the reason strings and their order cannot drift apart.
struct DocumentCornerCapture {
    Geom::PathVector geometry;
    NodeSatellites satellites;
    std::optional<double> input_to_document_scale;
    std::string reason;
    bool ok = false;
    bool non_similarity = false;
};

DocumentCornerCapture capture_document_corner_edit(SPShape &path, std::size_t node_limit_hint)
{
    DocumentCornerCapture result;
    if (!path.document || !path.isVisibleAndUnlocked()) {
        result.reason = _("The target path is unavailable, hidden or locked.");
        return result;
    }
    if (path.path_effect_list->size() > 32 || node_limit_hint > CE::max_nodes) {
        result.reason = _("The current node/effect context exceeds the bounded corner-edit limit.");
        return result;
    }
    auto effects = path.getPathEffects();
    auto effect = effects.empty() ? nullptr : dynamic_cast<LivePathEffect::LPEFilletChamfer *>(effects.front());
    if ((!effects.empty() && (!effect || effects.size() != 1)) || path.hasBrokenPathEffect()) {
        result.reason = _("Corner controls require one Corners effect first in the stack; existing effects were not changed.");
        return result;
    }
    if (effect && (effect->getLPEObj()->hrefList.size() != 1 || !effect->is_visible)) {
        result.reason = _("Shared or disabled Corners effects must be edited through Path Effects for now.");
        return result;
    }
    for (auto parent = path.parent; parent; parent = parent->parent) {
        if (auto lpe = cast<SPLPEItem>(parent); lpe && lpe->hasPathEffect()) {
            result.reason = _("Ancestor effects require the existing Path Effects controls.");
            return result;
        }
    }
    auto curve = path.curveForEdit();
    if (!curve) { result.reason = _("The path has no editable geometry."); return result; }
    // Loaded documents need not have run the LPE yet; its transient
    // pathvector_before_effect can be empty. curveForEdit is the persisted
    // original geometry and is also what the Node tool edits.
    auto &geometry = result.geometry = *curve;
    if (std::any_of(geometry.begin(), geometry.end(), [](auto const &p) { return p.empty(); })) {
        result.reason = _("Empty subpaths cannot be serialized as native corner records.");
        return result;
    }
    auto &satellites = result.satellites;
    if (effect) {
        satellites = effect->nodesatellites_param.data();
    } else {
        std::size_t total = 0;
        for (auto const &subpath : geometry) {
            auto n = subpath.empty() ? 0 : count_path_nodes(subpath);
            if (n > CE::max_nodes - total) {
                result.reason = _("The path exceeds the 4096-node corner-edit limit."); return result;
            }
            total += n;
            NodeSatellite zero(FILLET);
            zero.setAmount(0); zero.setIsTime(false); zero.setSelected(false);
            zero.setHasMirror(true); zero.setHidden(false); zero.setAngle(100); zero.setSteps(1);
            satellites.emplace_back(n, zero);
        }
    }
    if (!CE::bounded_shape(geometry, satellites)) {
        result.reason = _("Corner data is unavailable, percentage-based, malformed, or exceeds the 4096-node limit.");
        return result;
    }
    result.input_to_document_scale = CE::document_scale(path.i2doc_affine());
    for (auto const &sub : satellites) for (auto const &sat : sub) {
        if (!sat.steps || sat.steps > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            result.reason = _("The existing corner step count is outside the native persistence range.");
            return result;
        }
    }
    if (!result.input_to_document_scale) {
        result.reason = _("Nonuniform scale or skew makes a circular document radius ambiguous; use the existing LPE controls.");
        result.non_similarity = true;
        return result;
    }
    result.ok = true;
    return result;
}
} // namespace

CE::Address corner_rounding_address(Node const &node)
{
    auto const &lists = node.nodeList().subpathList().pm().subpathList();
    std::size_t p = 0;
    for (auto const &list : lists) {
        std::size_t n = 0;
        for (auto const &candidate : *list) {
            if (&candidate == &node) return {p, n};
            ++n;
        }
        ++p;
    }
    return {CE::max_nodes, CE::max_nodes}; // Invalid address, never another corner.
}

CornerRoundingContext capture_corner_rounding_document(SPShape &shape, std::size_t node_limit_hint)
{
    auto capture = capture_document_corner_edit(shape, node_limit_hint);
    CornerRoundingContext result;
    result.input_to_document_scale = capture.input_to_document_scale;
    if (!capture.ok) {
        result.reason = std::move(capture.reason);
        result.non_similarity = capture.non_similarity;
        return result;
    }
    if (is<SPPath>(&shape)) {
        // A path's corner identity is its Node selection, which only the Node
        // tool can supply; the command line refuses it with this same reason.
        result.reason = _("Paths need their node types; use the Node tool or a later version.");
        return result;
    }
    result.snapshot = CE::Snapshot{std::move(capture.geometry), std::move(capture.satellites), {}, {}};
    return result;
}

CornerRoundingContext capture_corner_rounding(SPShape &path, ControlPointSelection const &nodes)
{
    // The shared document checks, then the Node-tool identity exactly as before
    // the command line existed (including a native shape that has a manipulator).
    CornerRoundingContext result;
    auto const capture = capture_document_corner_edit(path, nodes.allPoints().size());
    result.input_to_document_scale = capture.input_to_document_scale;
    if (!capture.ok) {
        result.reason = capture.reason;
        return result;
    }
    auto const &geometry = capture.geometry;
    auto const &satellites = capture.satellites;
    // Native SVG vertices are all cusp identities; open endpoints are excluded
    // by CornerEdit itself. AC-8b path-node classification does not apply.
    if (is<SPPolygon>(&path) || is<SPPolyLine>(&path)) {
        result.snapshot = CE::Snapshot{geometry, satellites, {}, {}};
        return result;
    }
    PathManipulator *manipulator = nullptr;
    for (auto point : nodes.allPoints()) {
        if (auto node = dynamic_cast<Node *>(point)) {
            auto &pm = node->nodeList().subpathList().pm();
            if (pm.item() != &path) continue; // Never follow clip/mask/LPE-parameter editors.
            if (manipulator && manipulator != &pm) {
                result.reason = _("More than one node editor represents this path.");
                return result;
            }
            manipulator = &pm;
        }
    }
    if (!manipulator) {
        if (!is<SPPath>(&path)) {
            // Other native shapes retain their object type. Their
            // shape handles are not Node selections; All or an explicit corner
            // click can target their curve without destructive conversion.
            result.snapshot = CE::Snapshot{geometry, satellites, {}, {}};
            return result;
        }
        result.reason = _("Use the Node tool to edit this path's original nodes.");
        return result;
    }
    auto const &subpaths = manipulator->subpathList();
    if (subpaths.size() != geometry.size()) {
        result.reason = _("Node and effect input topology differ; no corners were changed.");
        return result;
    }
    CE::Snapshot snapshot{geometry, satellites, {}, {}};
    std::size_t p = 0;
    for (auto const &subpath : subpaths) {
        if (subpath->size() != satellites[p].size() || subpath->closed() != geometry[p].closed()) {
            result.reason = _("Node and effect input topology differ; no corners were changed.");
            return result;
        }
        std::size_t j = 0;
        for (auto const &node : *subpath) {
            // Index selects identity; position is only a consistency assertion.
            // Coincident nodes are not merged through the LPE's proximity lookup.
            auto position = j < count_path_curves(geometry[p])
                          ? geometry[p][j].initialPoint() : geometry[p].finalPoint();
            if (!Geom::are_near(position * path.i2dt_affine(), node.position(), 1e-7)) {
                result.reason = _("Node positions no longer match the effect input.");
                return result;
            }
            if (node.selected()) snapshot.selected.push_back({p, j});
            if (node.type() != NODE_CUSP) snapshot.smooth.push_back({p, j});
            ++j;
        }
        ++p;
    }
    result.snapshot = std::move(snapshot);
    return result;
}
} // namespace Inkscape::UI::Tools
