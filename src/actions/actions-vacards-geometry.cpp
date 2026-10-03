// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: boolean, offset, corners and resize actions.
 *
 * Four selection-geometry actions:
 *   vacards-boolean           -- combine the selected paths exactly like the
 *     Boolean Assist commit (pathUnion/pathIntersect/pathSymDiff/pathDiffMany)
 *     in one Undo step, reported through the document content revision;
 *   vacards-transform-resize  -- resize the selection as one unit around a
 *     reference point with the same affine the select toolbar applies
 *     (shared via selection_resize_affine);
 *   vacards-corners           -- round or inverse-round the corners of one
 *     selected native shape (rect/ellipse/polygon/star) with the same corner
 *     engine the Node tool's corner controls use (shared via
 *     capture_corner_rounding_document/apply_corner_plan);
 *   vacards-offset            -- create offset paths around the selection using
 *     the Offset Shapes engine (prepare/build/commit) shared with the tool.
 * vacards-boolean and vacards-transform-resize are collective-geometry: one
 * document-space transform or boolean over the whole selection, one Undo step,
 * atomic no-op on rejection. vacards-offset is per-selected-root: each selected
 * root is one source (a selected group becomes one combined silhouette, not one
 * offset per member), and every such source gets its own offset, in one Undo
 * step. vacards-corners is single-object: exactly one selected native shape,
 * matching the Node tool's single-item corner controls. None writes preferences.
 */

#define BOOST_JSON_NO_LIB

#include "actions-vacards-geometry.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <boost/json.hpp>

#include <glibmm/i18n.h>

#include "actions-vacards-cli.h"
#include "vacards-cli-result.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "live_effects/lpeobject.h"
#include "live_effects/lpe-fillet-chamfer.h"
#include "object/object-set.h"
#include "object/weakptr.h"
#include "object/sp-item-transform.h"
#include "object/sp-item.h"
#include "object/sp-item-group.h"
#include "object/sp-path.h"
#include "object/sp-shape.h"
#include "path/offset-shapes.h"
#include "preferences.h"
#include "selection.h"
#include "ui/icon-names.h"
#include "ui/toolbar/boolean-assist.h"
#include "ui/toolbar/transform-reference.h"
#include "ui/tools/corner-rounding-context.h"
#include "ui/tools/corner-rounding-controller.h"
#include "xml/document.h"

namespace Inkscape::VACardsCli {
namespace {

namespace CE = LivePathEffect::CornerEdit;
namespace Tools = UI::Tools;

std::string item_id(SPItem const *item)
{
    if (item) {
        if (char const *id = item->getId()) {
            return id;
        }
    }
    return "";
}

// ---- vacards-boolean ------------------------------------------------------------------------

constexpr std::string_view boolean_choices[] = {
    "union", "intersection", "exclusion", "bottom-minus-rest", "top-minus-rest"};

constexpr ParamSpec boolean_params[] = {
    {.key = "op", .type = ParamType::Choice, .required = true, .choices = boolean_choices,
     .help = "union, intersection, exclusion, or the Boolean Assist differences by stacking order."}};

constexpr ActionSpec boolean_spec{.name = "vacards-boolean", .mode = "collective-geometry",
    .summary = "Combine the selected paths like Boolean Assist.", .params = boolean_params};

void boolean(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(boolean_spec, value, app, [](ActionContext &c) {
        Record &r = c.record;

        if (!c.selection || c.selection->size() < 2) {
            r.selected = c.selection ? c.selection->size() : 0;
            r.status = Status::Rejected;
            r.reason = "needs-two-operands";
            r.message = "Select at least two objects.";
            return;
        }

        auto const operands = c.selection->items_vector();
        r.selected = static_cast<int>(operands.size());

        // Eligibility preflight, shared with the Boolean Assistant GUI: a boolean over a
        // selection containing an object the GUI would refuse would silently change the
        // meaning of the operation, so refuse the whole action instead. A selected group is
        // accepted when every one of its leaves is an eligible, available operand: the group
        // then acts as one combined shape.
        std::vector<Exclusion> exclusions;
        std::vector<std::string> before_ids;
        auto const availability = [](SPItem *item) { return Inkscape::VACardsCli::document_available(item); };
        for (auto *item : operands) {
            if (!document_available(item)) {
                exclusions.push_back({item_id(item), "unavailable"});
                continue;
            }
            // One shared reason path for every operand: a plain shape/text and a group both go through
            // boolean_assist_leaves, so an empty outline is reported as "empty-geometry" for either.
            std::vector<SPItem *> leaves;
            SPItem *offending = nullptr;
            std::string reason;
            if (UI::Toolbar::boolean_assist_leaves(item, availability, leaves, &offending, &reason)) {
                before_ids.push_back(item_id(item));
                for (auto *leaf : leaves) {
                    if (leaf != item) {
                        before_ids.push_back(item_id(leaf));
                    }
                }
                continue;
            }
            auto *const culprit = offending ? offending : item;
            exclusions.push_back({item_id(culprit), reason.empty() ? "not-a-shape" : reason});
        }
        if (!exclusions.empty()) {
            r.status = Status::Rejected;
            r.reason = "incompatible-operands";
            r.message = "Boolean operations need visible, unlocked paths, shapes or text; see excluded.";
            r.excluded = exclusions;
            return;
        }

        auto const before_rev = c.document->getReprDoc()->contentRevision();

        // Exactly the Boolean Assist mapping, shared with the GUI; skip_undo/silent so this body owns the
        // single Undo step. Selected groups are first combined into one path each by the shared helper.
        std::string const op = c.params.at("op").text;
        UI::Toolbar::BooleanAssistOp assist_op = UI::Toolbar::BooleanAssistOp::Union;
        if (op == "union") {
            assist_op = UI::Toolbar::BooleanAssistOp::Union;
        } else if (op == "intersection") {
            assist_op = UI::Toolbar::BooleanAssistOp::Intersection;
        } else if (op == "exclusion") {
            assist_op = UI::Toolbar::BooleanAssistOp::Exclusion;
        } else if (op == "bottom-minus-rest") {
            assist_op = UI::Toolbar::BooleanAssistOp::BottomMinusRest;
        } else { // top-minus-rest
            assist_op = UI::Toolbar::BooleanAssistOp::TopMinusRest;
        }
        std::vector<std::string> removed_group_ids;
        auto *const boolean_result = UI::Toolbar::apply_boolean_assist(*c.selection, assist_op, &removed_group_ids);

        auto const after_rev = c.document->getReprDoc()->contentRevision();
        bool const changed = after_rev != before_rev;

        std::vector<std::string> result_ids;
        for (auto *item : c.selection->items_vector()) {
            result_ids.push_back(item_id(item));
        }

        if (!changed) {
            r.status = Status::Rejected;
            r.reason = "boolean-failed";
            r.message = "The boolean operation produced no result; nothing was changed.";
            return;
        }
        // A committed boolean must leave exactly one path; anything else is a partial
        // success (or a degenerate result) and must never be committed. The shared helper also returns null when
        // the only remaining path is an untouched operand (a path-effect bake with an early exit), which must
        // fail rather than commit that original.
        SPItem *const single = c.selection->singleItem();
        if (!boolean_result || c.selection->size() != 1 || !is<SPPath>(single) || single != boolean_result) {
            Inkscape::DocumentUndo::cancel(c.document);
            r.status = Status::Failed;
            r.reason = "boolean-failed";
            r.message = "The boolean operation did not produce one path; the document was restored.";
            return;
        }

        auto const label = UI::Toolbar::boolean_assist_undo_label(assist_op, operands.size());
        DocumentUndo::done(c.document, label.first, label.second);

        r.status = Status::Changed;
        r.reason = "success";
        r.message = "Boolean " + op + " produced " + std::to_string(result_ids.size()) + " object(s).";
        r.one_undo_step = true;
        r.eligible = r.selected;

        for (auto const &id : result_ids) {
            bool const existed_before = std::find(before_ids.begin(), before_ids.end(), id) != before_ids.end();
            if (existed_before) {
                r.modified.push_back(id);
            } else {
                r.created.push_back(id);
            }
        }
        for (auto const &id : before_ids) {
            if (!id.empty() && !c.document->getObjectById(id)) {
                r.deleted.push_back(id);
            }
        }
        // The shared helper also reports the groups it removed; nested groups are not part of before_ids, so
        // add any id not already recorded (a selected group root appears in both lists).
        for (auto const &id : removed_group_ids) {
            if (id.empty()) {
                continue;
            }
            if (std::find(r.deleted.begin(), r.deleted.end(), id) == r.deleted.end()) {
                r.deleted.push_back(id);
            }
        }
    });
}

// ---- vacards-transform-resize ---------------------------------------------------------------

constexpr std::string_view anchor_choices[] = {"nw", "n", "ne", "w", "center", "e", "sw", "s", "se"};
constexpr std::string_view bbox_choices[] = {"visual", "geometric"};

constexpr ParamSpec resize_params[] = {
    {.key = "width", .type = ParamType::Length, .min = 0.000001,
     .help = "Target width."},
    {.key = "height", .type = ParamType::Length, .min = 0.000001,
     .help = "Target height."},
    {.key = "anchor", .type = ParamType::Choice, .default_value = "nw", .choices = anchor_choices,
     .help = "Reference point the resize keeps fixed."},
    {.key = "keep-ratio", .type = ParamType::Boolean, .default_value = "false",
     .help = "Scale the missing dimension by the same factor."},
    {.key = "bbox", .type = ParamType::Choice, .default_value = "visual", .choices = bbox_choices,
     .help = "Which selection bounds to resize."}};

constexpr ActionSpec resize_spec{.name = "vacards-transform-resize", .mode = "collective-geometry",
    .summary = "Resize the selection as one unit around a reference point, like the select toolbar.",
    .params = resize_params};

UI::Toolbar::TransformReferencePoint anchor_from_name(std::string const &name)
{
    using UI::Toolbar::TransformReferencePoint;
    if (name == "n") return TransformReferencePoint::TopCenter;
    if (name == "ne") return TransformReferencePoint::TopRight;
    if (name == "w") return TransformReferencePoint::CenterLeft;
    if (name == "center") return TransformReferencePoint::Center;
    if (name == "e") return TransformReferencePoint::CenterRight;
    if (name == "sw") return TransformReferencePoint::BottomLeft;
    if (name == "s") return TransformReferencePoint::BottomCenter;
    if (name == "se") return TransformReferencePoint::BottomRight;
    return TransformReferencePoint::TopLeft; // nw (default)
}

void transform_resize(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(resize_spec, value, app, [](ActionContext &c) {
        Record &r = c.record;

        if (!c.selection || c.selection->isEmpty()) {
            r.status = Status::Rejected;
            r.reason = "empty-selection";
            r.message = "Select at least one object.";
            return;
        }

        // An unavailable member cannot be transformed; resizing the rest would silently
        // change the collective result, so refuse the whole action.
        std::vector<Exclusion> unavailable;
        for (auto *item : c.selection->items_vector()) {
            if (!document_available(item)) {
                unavailable.push_back({item_id(item), "unavailable"});
            }
        }
        if (!unavailable.empty()) {
            r.status = Status::Rejected;
            r.reason = "unavailable";
            r.message = "Resize needs visible, unlocked objects; see excluded.";
            r.excluded = unavailable;
            return;
        }

        bool const has_width = c.params.has("width");
        bool const has_height = c.params.has("height");
        bool const visual_bbox = c.params.at("bbox").text == "visual";

        if (!has_width && !has_height) {
            constexpr char const *message = "Give width, height or both.";
            r.status = Status::Rejected;
            r.reason = "invalid-argument";
            r.message = message;
            r.error = ParseError{"missing-required", "width", message};
            return;
        }

        auto const bounds = visual_bbox ? c.selection->visualBounds() : c.selection->geometricBounds();
        if (!bounds) {
            r.status = Status::Rejected;
            r.reason = "no-bounds";
            r.message = "The selection has no size.";
            return;
        }

        // Resizing an axis the selection does not have cannot produce a size; it would just
        // move the object, so it is refused before the transform is built.
        if ((has_width && bounds->width() <= 1e-12) || (has_height && bounds->height() <= 1e-12)) {
            r.status = Status::Rejected;
            r.reason = "zero-dimension";
            r.message = "The selection has no size along the requested axis.";
            return;
        }

        double w = has_width ? c.params.at("width").number : bounds->width();
        double h = has_height ? c.params.at("height").number : bounds->height();

        bool const keep_ratio = c.params.at("keep-ratio").boolean;
        if (keep_ratio && has_width && has_height) {
            r.warnings.push_back("keep-ratio ignored because both width and height were given");
        }
        if (keep_ratio && (has_width != has_height)) {
            if (has_width) {
                double const old_w = bounds->width();
                if (old_w == 0.0) {
                    r.status = Status::Rejected;
                    r.reason = "zero-dimension";
                    r.message = "The selection has no size.";
                    return;
                }
                h = bounds->height() * (w / old_w);
            } else {
                double const old_h = bounds->height();
                if (old_h == 0.0) {
                    r.status = Status::Rejected;
                    r.reason = "zero-dimension";
                    r.message = "The selection has no size.";
                    return;
                }
                w = bounds->width() * (h / old_h);
            }
        }

        auto const point = anchor_from_name(c.params.at("anchor").text);
        auto const target = UI::Toolbar::resize_around_reference(*bounds, w, h, point);

        auto prefs = Preferences::get();
        bool const transform_stroke = prefs->getBool("/options/transform/stroke", true);
        bool const preserve = prefs->getBool("/options/preservetransform/value", false);

        auto const scaler = selection_resize_affine(*c.selection, visual_bbox, transform_stroke, preserve,
                                                    target.x0, target.y0, target.x1, target.y1);
        if (!scaler) {
            r.status = Status::Rejected;
            r.reason = "no-bounds";
            r.message = "The selection has no size.";
            return;
        }
        if (scaler->isIdentity(1e-12)) {
            r.status = Status::Unchanged;
            r.reason = "unchanged";
            r.message = "The selection already has that size.";
            return;
        }

        double const old_width = bounds->width();
        double const old_height = bounds->height();

        c.selection->applyAffine(*scaler);
        DocumentUndo::done(c.document, RC_("Undo", "VACards resize"), INKSCAPE_ICON("tool-pointer"));

        auto const new_bounds = visual_bbox ? c.selection->visualBounds() : c.selection->geometricBounds();
        double const new_width = new_bounds ? new_bounds->width() : w;
        double const new_height = new_bounds ? new_bounds->height() : h;

        r.status = Status::Changed;
        r.reason = "success";
        r.message = "Resized " + std::to_string(c.selection->size()) + " object(s).";
        r.one_undo_step = true;
        r.eligible = c.selection->size();
        for (auto *item : c.selection->items_vector()) {
            r.modified.push_back(item_id(item));
        }
        r.metrics["old_width"] = old_width;
        r.metrics["old_height"] = old_height;
        r.metrics["new_width"] = new_width;
        r.metrics["new_height"] = new_height;
        r.data["transform_stroke"] = transform_stroke;
        r.data["preserve_transform"] = preserve;
    });
}

// ---- vacards-offset --------------------------------------------------------------------------

constexpr std::string_view offset_direction_choices[] = {"outward", "inward", "both"};
constexpr std::string_view offset_corner_choices[] = {"round", "bevel", "miter"};

constexpr ParamSpec offset_params[] = {
    {.key = "distance", .type = ParamType::Length, .required = true, .min = 0.000001,
     .help = "Offset distance."},
    {.key = "direction", .type = ParamType::Choice, .default_value = "outward",
     .choices = offset_direction_choices,
     .help = "Offset away from (outward) or into (inward) each shape, or both."},
    {.key = "corner", .type = ParamType::Choice, .default_value = "miter",
     .choices = offset_corner_choices, .help = "Corner join of the generated outline."},
    {.key = "miter-limit", .type = ParamType::Number, .default_value = "4", .min = 1, .max = 100,
     .help = "Miter join limit."},
    {.key = "outer-only", .type = ParamType::Boolean, .default_value = "false",
     .help = "Offset the combined outer silhouette only, dropping interior holes."},
    {.key = "delete-originals", .type = ParamType::Boolean, .default_value = "false",
     .help = "Delete each source that produced an offset (with outer-only: every source)."},
    {.key = "simplify", .type = ParamType::Boolean, .default_value = "false",
     .help = "Simplify the generated outlines within simplify-tolerance."},
    {.key = "simplify-tolerance", .type = ParamType::Length, .default_value = "0.05px", .min = 0.000001,
     .help = "Maximum deviation accepted when simplifying."},
    {.key = "select-results", .type = ParamType::Boolean, .default_value = "true",
     .help = "Select the created paths instead of keeping the original selection."},
    {.key = "dry-run", .type = ParamType::Boolean, .default_value = "false",
     .help = "Report what would be created without changing the document."}};

constexpr ActionSpec offset_spec{.name = "vacards-offset", .mode = "per-selected-root",
    .summary = "Create offset paths around the selected shapes like the Offset Shapes tool.",
    .params = offset_params};

OffsetShapes::Options offset_options(ParseResult const &params)
{
    OffsetShapes::Options options;
    options.distance_px = params.at("distance").number;
    auto const &direction = params.at("direction").text;
    options.direction = direction == "inward"  ? OffsetShapes::Direction::Inward
                        : direction == "both" ? OffsetShapes::Direction::Both
                                              : OffsetShapes::Direction::Outward;
    auto const &corner = params.at("corner").text;
    options.corner = corner == "round"  ? OffsetShapes::Corner::Round
                     : corner == "bevel" ? OffsetShapes::Corner::Bevel
                                         : OffsetShapes::Corner::Miter;
    options.miter_limit = params.at("miter-limit").number;
    options.outer_shapes_only = params.at("outer-only").boolean;
    options.delete_originals = params.at("delete-originals").boolean;
    options.simplify_results = params.at("simplify").boolean;
    // Tiny tolerances explode livarot's pre-sampling (hang/OOM): clamp.
    options.simplify_tolerance_px = std::max(params.at("simplify-tolerance").number, 0.001);
    options.select_results = params.at("select-results").boolean;
    return options;
}

void offset(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(offset_spec, value, app, [](ActionContext &c) {
        Record &r = c.record;

        if (!c.selection || c.selection->isEmpty()) {
            r.status = Status::Rejected;
            r.reason = "empty-selection";
            r.message = "Select at least one object.";
            return;
        }

        auto const items = c.selection->items_vector();
        auto const prepared = OffsetShapes::prepare(items);
        r.selected = static_cast<int>(items.size());
        r.eligible = static_cast<int>(prepared.sources.size());
        r.metrics["skipped"] = static_cast<std::int64_t>(prepared.skipped_count);
        r.metrics["open_subpaths_skipped"] = static_cast<std::int64_t>(prepared.open_subpaths_skipped);

        // Capture identities before commit. With delete-originals, commit deletes the selected
        // items like Edit > Delete (selected clones first; unselected clones of a deleted original
        // are unlinked per the orphaned-clone preference), so the raw selection pointers must not
        // be read afterwards. Weak pointers null themselves when the
        // object is destroyed; the ids are what the caller compares against the document.
        std::vector<SPWeakPtr<SPItem>> selected_weak;
        std::vector<std::string> selected_ids;
        selected_weak.reserve(items.size());
        selected_ids.reserve(items.size());
        for (auto *item : items) {
            selected_weak.emplace_back(item);
            selected_ids.emplace_back(item_id(item));
        }

        if (!prepared) {
            r.status = Status::Rejected;
            r.reason = "no-closed-shapes";
            r.message = !prepared.error.empty() ? prepared.error
                                                : "No selected object has a closed shape to offset.";
            return;
        }

        auto const options = offset_options(c.params);
        auto const built = OffsetShapes::build(prepared.sources, options);
        if (!built) {
            r.status = Status::Rejected;
            r.reason = "offset-failed";
            r.message = built.error;
            return;
        }

        r.metrics["results"] = static_cast<std::int64_t>(built.results.size());
        if (options.simplify_results) {
            r.metrics["simplify_skipped"] = static_cast<std::int64_t>(built.simplify_skipped);
            if (built.simplify_skipped) {
                r.warnings.push_back(std::to_string(built.simplify_skipped) +
                                     " outline(s) could not be simplified within the work budget or tolerance and "
                                     "were kept as generated.");
            }
        }
        if (r.dry_run) {
            r.status = Status::Ok;
            r.reason = "success";
            r.message = "Dry run: " + std::to_string(built.results.size()) + " offset path(s) would be created.";
            return;
        }

        auto const committed = OffsetShapes::commit(c.document, prepared, built, options,
                                                    OffsetShapes::CommitProtocol::CommandLine);
        if (!committed) {
            r.status = Status::Failed;
            r.reason = "offset-failed";
            r.message = committed.error;
            return;
        }

        r.status = Status::Changed;
        r.reason = "success";
        r.message = "Created " + std::to_string(committed.created.size()) + " offset path(s).";
        r.one_undo_step = true;

        bool missing_id = false;
        for (auto *item : committed.created) {
            auto const id = item_id(item);
            if (id.empty()) missing_id = true;
            r.created.push_back(id);
        }
        if (missing_id) {
            r.warnings.push_back("A created offset path has no id; the document did not assign one (app defect).");
        }
        // A selected original counts as deleted when it no longer exists in the document (an
        // unlinked unselected clone keeps its id and is therefore not reported as deleted). Report the selected
        // items in selection order first, then any further result ids, without duplicates.
        std::vector<std::string> deleted_ids;
        for (auto const &id : selected_ids) {
            if (!id.empty() && !c.document->getObjectById(id) &&
                std::find(deleted_ids.begin(), deleted_ids.end(), id) == deleted_ids.end()) {
                deleted_ids.push_back(id);
            }
        }
        for (auto const &id : committed.deleted_ids) {
            if (!id.empty() && std::find(deleted_ids.begin(), deleted_ids.end(), id) == deleted_ids.end()) {
                deleted_ids.push_back(id);
            }
        }
        r.deleted = deleted_ids;

        if (options.select_results) {
            c.selection->setList(committed.created);
        } else if (options.delete_originals) {
            // Same policy as the tool: keep the surviving original selection, built only from weak
            // pointers that are still alive. The raw selection pointers dangle after the deletions.
            std::unordered_set<SPItem *> const deleted(committed.deleted.begin(), committed.deleted.end());
            std::vector<SPItem *> surviving;
            for (auto const &weak : selected_weak) {
                auto *item = weak.get();
                if (item && !deleted.contains(item)) surviving.push_back(item);
            }
            c.selection->setList(surviving);
        }
    });
}

// ---- vacards-corners -------------------------------------------------------------------------

namespace {

// A "P:N" corner address: two non-negative decimal integers joined by ':'. Only
// plain digits are accepted (no sign, whitespace, or extra separator), so a
// malformed pair is reported rather than guessed.
bool parse_node_address(std::string_view text, CE::Address &out)
{
    auto const colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) return false;
    auto const path_text = text.substr(0, colon);
    auto const node_text = text.substr(colon + 1);
    unsigned long path = 0, node = 0;
    auto const path_result = std::from_chars(path_text.data(), path_text.data() + path_text.size(), path);
    if (path_result.ec != std::errc{} || path_result.ptr != path_text.data() + path_text.size()) return false;
    auto const node_result = std::from_chars(node_text.data(), node_text.data() + node_text.size(), node);
    if (node_result.ec != std::errc{} || node_result.ptr != node_text.data() + node_text.size()) return false;
    out = {static_cast<std::size_t>(path), static_cast<std::size_t>(node)};
    return true;
}

} // namespace

constexpr std::string_view corner_mode_choices[] = {"round", "inverse-round"};
constexpr std::string_view corner_scope_choices[] = {"all", "nodes"};

constexpr ParamSpec corner_params[] = {
    {.key = "radius", .type = ParamType::Length, .required = true, .min = 0.0,
     .help = "Corner radius; zero removes rounding."},
    {.key = "mode", .type = ParamType::Choice, .default_value = "round", .choices = corner_mode_choices,
     .help = "round (convex) or inverse-round (concave cutout)."},
    {.key = "scope", .type = ParamType::Choice, .default_value = "all", .choices = corner_scope_choices,
     .help = "All corners, or only the corner addresses listed in nodes."},
    {.key = "nodes", .type = ParamType::List,
     .help = "Corner addresses (P:N) for scope=nodes; each must exist in the shape."},
    {.key = "dry-run", .type = ParamType::Boolean, .default_value = "false",
     .help = "Report the corner plan without changing the document."}};

constexpr ActionSpec corner_spec{.name = "vacards-corners", .mode = "single-object",
    .summary = "Round or inverse-round the corners of one selected native shape like the Node tool's corner controls.",
    .params = corner_params};

void corners(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(corner_spec, value, app, [](ActionContext &c) {
        Record &r = c.record;

        // Single-object, like the Node tool's singleItem(): exactly one visible,
        // unlocked native shape. Groups, text and multi-selections are refused.
        auto *item = c.selection ? c.selection->singleItem() : nullptr;
        if (!c.selection || c.selection->size() != 1 || !is<SPShape>(item) || !document_available(item)) {
            r.selected = c.selection ? static_cast<int>(c.selection->size()) : 0;
            r.status = Status::Rejected;
            r.reason = "requires-single-shape";
            r.message = "Select exactly one visible, unlocked native shape.";
            return;
        }
        r.selected = 1;
        auto &shape = *cast<SPShape>(item);

        // The document-level capture, shared with the Node tool: an SPPath is not
        // in scope here (AC-8b), a native shape with a corner-capable input is.
        auto context = Tools::capture_corner_rounding_document(shape, 0);
        if (!context.snapshot) {
            r.status = Status::Rejected;
            r.reason = context.non_similarity ? "non-similarity-transform"
                     : is<SPPath>(&shape)     ? "path-not-supported"
                                              : "unsupported-shape";
            r.message = context.reason; // Paths need their node types; ... / hidden, locked, effects, ...
            return;
        }

        auto snapshot = *context.snapshot;
        CE::Request request;
        request.mode = c.params.at("mode").text == "inverse-round" ? CE::Mode::InverseRound : CE::Mode::Round;
        if (c.params.at("scope").text == "nodes") {
            request.scope = CE::Scope::Selected;
            if (!c.params.has("nodes")) {
                constexpr char const *message = "scope=nodes needs nodes=P:N corner addresses.";
                r.status = Status::Rejected;
                r.reason = "invalid-argument";
                r.message = message;
                r.error = ParseError{"missing-required", "nodes", message};
                return;
            }
            for (auto const &text : c.params.at("nodes").items) {
                CE::Address address{};
                if (!parse_node_address(text, address)) {
                    constexpr char const *message = "Each node must be a P:N pair of non-negative integers.";
                    r.status = Status::Rejected;
                    r.reason = "invalid-argument";
                    r.message = message;
                    r.error = ParseError{"malformed-value", "nodes", message};
                    return;
                }
                if (address.path >= snapshot.satellites.size() ||
                    address.node >= snapshot.satellites[address.path].size()) {
                    r.status = Status::Rejected;
                    r.reason = "no-such-node";
                    r.message = "The shape has no corner at " + text + ".";
                    return;
                }
                snapshot.selected.push_back(address);
            }
        } else {
            if (c.params.has("nodes")) {
                // Never widen an explicit corner list to every corner.
                constexpr char const *message = "nodes needs scope=nodes; scope=all rounds every corner.";
                r.status = Status::Rejected;
                r.reason = "invalid-argument";
                r.message = message;
                r.error = ParseError{"missing-required", "scope", message};
                return;
            }
            request.scope = CE::Scope::All;
        }
        // The popover multiplies a display-unit value by the same conversion and
        // sends input units; a document-px radius therefore divides by the scale.
        request.radius = c.params.at("radius").number / *context.input_to_document_scale;

        LivePathEffectObject *effect = nullptr;
        if (auto *lpe = shape.getFirstPathEffectOfType(LivePathEffect::FILLET_CHAMFER)) {
            effect = lpe->getLPEObj();
        }
        // The same plan and refusal/no-change checks the commit performs, so a
        // dry run reports exactly what the real run would do.
        auto check = Tools::check_corner_plan(effect != nullptr, snapshot, request);
        auto const &plan = check.plan;
        auto rejection_reason = [&] {
            switch (plan.status) {
            case CE::Status::NoCorners: return "no-corners";
            case CE::Status::InvalidInput: return "invalid-input";
            case CE::Status::EngineLimit: return "engine-limit";
            default: return "corner-edit-failed";
            }
        };

        if (r.dry_run) {
            // Read-only; the document is never touched. The same admission the
            // commit applies, so a dry run never promises a change it would refuse.
            r.metrics["count"] = static_cast<std::int64_t>(plan.count);
            r.metrics["approximate"] = plan.approximate;
            auto const busy = check.ready ? Tools::command_line_corner_refusal(*shape.document) : std::nullopt;
            if (busy) {
                r.status = Status::Rejected;
                r.reason = "document-busy";
                r.message = *busy;
            } else if (check.ready) {
                r.status = Status::Ok;
                r.reason = "success";
                r.message = "Dry run: " + std::to_string(plan.count) + " corner(s) would change.";
                r.data["status"] = "would-change";
            } else if (check.outcome == Tools::CornerRoundingController::Outcome::NoChange) {
                r.status = Status::Unchanged;
                r.reason = "unchanged";
                r.message = "The corners already have that radius and mode.";
            } else {
                r.status = Status::Rejected;
                r.reason = rejection_reason();
                r.message = check.reason;
            }
            return;
        }
        auto result = Tools::apply_corner_plan(shape, effect, snapshot, request,
                                               Tools::CornerCommitProtocol::CommandLine, [] { return true; });
        r.metrics["count"] = static_cast<std::int64_t>(result.count);
        r.metrics["approximate"] = result.approximate;
        switch (result.outcome) {
        case Tools::CornerRoundingController::Outcome::Applied:
            r.status = Status::Changed;
            r.reason = "success";
            r.message = "Rounded corners on " + item_id(item) + ".";
            r.one_undo_step = true;
            r.eligible = 1;
            r.modified.push_back(item_id(item));
            break;
        case Tools::CornerRoundingController::Outcome::NoChange:
            r.status = Status::Unchanged;
            r.reason = "unchanged";
            r.message = "The corners already have that radius and mode.";
            break;
        default:
            if (result.mutation_started) {
                r.status = Status::Failed;
                r.reason = "corner-edit-failed";
            } else if (result.refused_by_document) {
                r.status = Status::Rejected;
                r.reason = "document-busy";
            } else {
                r.status = Status::Rejected;
                r.reason = rejection_reason();
            }
            r.message = result.reason;
            break;
        }
    });
}

} // namespace
} // namespace Inkscape::VACardsCli

std::vector<std::vector<Glib::ustring>> raw_data_vacards_geometry = {
    {"app.vacards-boolean", N_("VACards Boolean"), N_("VACards"),
     N_("Combine the selected paths like Boolean Assist")},
    {"app.vacards-transform-resize", N_("VACards Transform Resize"), N_("VACards"),
     N_("Resize the selection as one unit around a reference point")},
    {"app.vacards-corners", N_("VACards Corners"), N_("VACards"),
     N_("Round or inverse-round one native shape's corners")},
    {"app.vacards-offset", N_("VACards Offset"), N_("VACards"),
     N_("Create offset paths around the selected shapes")}};

void add_actions_vacards_geometry(InkscapeApplication *app)
{
    auto *gapp = app->gio_app();
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);
    gapp->add_action_with_parameter("vacards-boolean", String,
        sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::boolean), app));
    gapp->add_action_with_parameter("vacards-transform-resize", String,
        sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::transform_resize), app));
    gapp->add_action_with_parameter("vacards-corners", String,
        sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::corners), app));
    gapp->add_action_with_parameter("vacards-offset", String,
        sigc::bind(sigc::ptr_fun(&Inkscape::VACardsCli::offset), app));

    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::boolean_spec);
    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::resize_spec);
    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::corner_spec);
    Inkscape::VACardsCli::register_action_spec(Inkscape::VACardsCli::offset_spec);

    app->get_action_extra_data().add_data(raw_data_vacards_geometry);
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
