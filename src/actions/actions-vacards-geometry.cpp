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
#include "vacards-cli-transaction.h"
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

void boolean_body(ActionContext &c)
{
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

        EditTransaction transaction(c.document, c.selection, c.operation_lease);
        if (!transaction.active()) {
            r.status = Status::Rejected;
            r.reason = "transaction-unavailable";
            r.message = "The document cannot start an isolated edit.";
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
            transaction.rollback();
            r.status = Status::Failed;
            r.reason = "boolean-failed";
            r.message = "The boolean operation did not produce one path; the document was restored.";
            return;
        }

        auto const label = UI::Toolbar::boolean_assist_undo_label(assist_op, operands.size());
        transaction.commit(label.first, label.second);

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
}

void boolean(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(boolean_spec, value, app, boolean_body);
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

void transform_resize_body(ActionContext &c)
{
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
}

void transform_resize(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(resize_spec, value, app, transform_resize_body);
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

void offset_body(ActionContext &c)
{
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

        EditTransaction transaction(c.document, c.selection, c.operation_lease);
        if (!transaction.active()) {
            r.status = Status::Rejected;
            r.reason = "transaction-unavailable";
            r.message = "The document cannot start an isolated edit.";
            return;
        }
        auto const committed = OffsetShapes::commit(c.document, prepared, built, options,
                                                    OffsetShapes::CommitProtocol::CallerOwnedAtomic);
        if (!committed) {
            r.status = Status::Failed;
            r.reason = "offset-failed";
            r.message = committed.error;
            return;
        }

        r.status = Status::Changed;
        r.reason = "success";
        r.message = "Created " + std::to_string(committed.created.size()) + " offset path(s).";

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
        transaction.commit(Util::Internal::ContextString("Offset shapes"), "path-offset-dynamic");
        r.one_undo_step = true;
}

void offset(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(offset_spec, value, app, offset_body);
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

void corners_body(ActionContext &c)
{
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
        auto const target_id = item_id(item); // Replacement invalidates item.
        auto const converted_from = Tools::corner_conversion_from(shape);
        auto report_conversion = [&] {
            if (!converted_from.empty())
                r.data["converted"] = boost::json::array{
                    boost::json::object{{"id", target_id}, {"from", converted_from}, {"to", "path"}}};
        };

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
                report_conversion();
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
            r.message = "Rounded corners on " + target_id + ".";
            if (!result.converted_from.empty()) {
                report_conversion();
                c.selection->set(c.document->getObjectById(target_id));
            }
            r.one_undo_step = true;
            r.eligible = 1;
            r.modified.push_back(target_id);
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
}

void corners(Glib::VariantBase const &value, InkscapeApplication *app)
{
    run_action(corner_spec, value, app, corners_body);
}

} // namespace
ActionSpec boolean_command() { auto s = boolean_spec; s.handler = boolean_body; return s; }
ActionSpec transform_resize_command() { auto s = resize_spec; s.handler = transform_resize_body; return s; }
ActionSpec offset_command() { auto s = offset_spec; s.handler = offset_body; return s; }
ActionSpec corners_command() { auto s = corner_spec; s.handler = corners_body; return s; }

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

#include "vacards-cli-production.h"
#include <boost/json.hpp>
namespace Inkscape::VACardsCli {
namespace {
// | `geometry.move` | ids; `dx:L,dy:L` | roots, root affine and geometric/visual bounds before/after | T, no-bounds, invalid-transform | E / one / computed / G |
TypeDescriptor const m3_0{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "dx": {
      "type": "object",
      "properties": {
        "value": {
          "type": "number",
          "minimum": -1000000.0,
          "maximum": 1000000.0
        },
        "unit": {
          "type": "string",
          "enum": [
            "px",
            "mm",
            "cm",
            "in",
            "pt",
            "pc"
          ]
        }
      },
      "required": [
        "value",
        "unit"
      ],
      "additionalProperties": false,
      "x-css-px-absolute-maximum": 1000000,
      "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
      "allOf": [
        {
          "if": {
            "properties": {
              "unit": {
                "const": "px"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -1000000.0,
                "maximum": 1000000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "mm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -264583.3333333333,
                "maximum": 264583.3333333333
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "cm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -26458.333333333336,
                "maximum": 26458.333333333336
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "in"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -10416.666666666666,
                "maximum": 10416.666666666666
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pt"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -750000.0,
                "maximum": 750000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pc"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -62500.0,
                "maximum": 62500.0
              }
            }
          }
        }
      ]
    },
    "dy": {
      "type": "object",
      "properties": {
        "value": {
          "type": "number",
          "minimum": -1000000.0,
          "maximum": 1000000.0
        },
        "unit": {
          "type": "string",
          "enum": [
            "px",
            "mm",
            "cm",
            "in",
            "pt",
            "pc"
          ]
        }
      },
      "required": [
        "value",
        "unit"
      ],
      "additionalProperties": false,
      "x-css-px-absolute-maximum": 1000000,
      "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
      "allOf": [
        {
          "if": {
            "properties": {
              "unit": {
                "const": "px"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -1000000.0,
                "maximum": 1000000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "mm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -264583.3333333333,
                "maximum": 264583.3333333333
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "cm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -26458.333333333336,
                "maximum": 26458.333333333336
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "in"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -10416.666666666666,
                "maximum": 10416.666666666666
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pt"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -750000.0,
                "maximum": 750000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pc"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": -62500.0,
                "maximum": 62500.0
              }
            }
          }
        }
      ]
    }
  },
  "required": [
    "ids",
    "dx",
    "dy"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
    "transform_policy": {
      "space": "document-root-css-px",
      "stroke": 1,
      "rectcorners": 1,
      "pattern": 1,
      "gradient": 1,
      "preservetransform": 0,
      "dash-scale": 1,
      "group-relative-layout": "preserved",
      "matrix-without-bounds": "allowed; null bounds"
    },
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "fresh readiness guard rejects busy document",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document operation admission busy",
        "native_evidence": "`DocumentUndo::fileOperationFreshReady` / `fileOperationOutputReady`, `src/actions/vacards-cli-edit-services.cpp:361-363`",
        "oracle": "P9:geometry.move:identity/admission:transaction-unavailable:fresh readiness guard rejects busy document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "applyAffine: nonfinite or singular affine",
        "code": "invalid-transform",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "applyAffine",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:command-service:invalid-transform:applyAffine: nonfinite or singular affine",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "collective bounds absent",
        "code": "no-bounds",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "collective bounds absent",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:command-service:no-bounds:collective bounds absent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809` (native move forwarding `:2074`)",
        "oracle": "P9:geometry.move:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.move:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:geometry.move:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:geometry.move:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `geometry.resize` | ids; width and/or height positive L; `keep-ratio=false`, `anchor=nw`, `bbox=visual`; anchor nw/n/ne/w/center/e/sw/s/se | dimensions/bounds/affines before/after; keep-ratio-ignored warning when applicable | T, no-bounds, zero-dimension | E / one / computed / G; existing native resize contract |
TypeDescriptor const m3_1{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "width": {
      "type": "object",
      "properties": {
        "value": {
          "type": "number",
          "minimum": 0,
          "maximum": 1000000.0,
          "exclusiveMinimum": 0
        },
        "unit": {
          "type": "string",
          "enum": [
            "px",
            "mm",
            "cm",
            "in",
            "pt",
            "pc"
          ]
        }
      },
      "required": [
        "value",
        "unit"
      ],
      "additionalProperties": false,
      "x-css-px-absolute-maximum": 1000000,
      "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
      "allOf": [
        {
          "if": {
            "properties": {
              "unit": {
                "const": "px"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 1000000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "mm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 264583.3333333333
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "cm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 26458.333333333336
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "in"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 10416.666666666666
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pt"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 750000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pc"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 62500.0
              }
            }
          }
        }
      ]
    },
    "height": {
      "type": "object",
      "properties": {
        "value": {
          "type": "number",
          "minimum": 0,
          "maximum": 1000000.0,
          "exclusiveMinimum": 0
        },
        "unit": {
          "type": "string",
          "enum": [
            "px",
            "mm",
            "cm",
            "in",
            "pt",
            "pc"
          ]
        }
      },
      "required": [
        "value",
        "unit"
      ],
      "additionalProperties": false,
      "x-css-px-absolute-maximum": 1000000,
      "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
      "allOf": [
        {
          "if": {
            "properties": {
              "unit": {
                "const": "px"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 1000000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "mm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 264583.3333333333
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "cm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 26458.333333333336
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "in"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 10416.666666666666
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pt"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 750000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pc"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 62500.0
              }
            }
          }
        }
      ]
    },
    "keep-ratio": {
      "type": "boolean",
      "default": false
    },
    "anchor": {
      "type": "string",
      "enum": [
        "nw",
        "n",
        "ne",
        "w",
        "center",
        "e",
        "sw",
        "s",
        "se"
      ],
      "default": "nw"
    },
    "bbox": {
      "type": "string",
      "enum": [
        "visual",
        "geometric"
      ],
      "default": "visual"
    }
  },
  "required": [
    "ids"
  ],
  "additionalProperties": false,
  "anyOf": [
    {
      "required": [
        "width"
      ]
    },
    {
      "required": [
        "height"
      ]
    }
  ],
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
    "transform_policy": {
      "space": "document-root-css-px",
      "stroke": 1,
      "rectcorners": 1,
      "pattern": 1,
      "gradient": 1,
      "preservetransform": 0,
      "dash-scale": 1,
      "group-relative-layout": "preserved",
      "matrix-without-bounds": "allowed; null bounds"
    },
    "warnings": [
      {
        "code": "keep-ratio-ignored",
        "emit_site": "resize admission: both width and height supplied with keep-ratio=true"
      }
    ],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "fresh readiness guard rejects busy document",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document operation admission busy",
        "native_evidence": "`DocumentUndo::fileOperationFreshReady` / `fileOperationOutputReady`, `src/actions/vacards-cli-edit-services.cpp:361-363`",
        "oracle": "P9:geometry.resize:identity/admission:transaction-unavailable:fresh readiness guard rejects busy document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "applyAffine: nonfinite or singular affine",
        "code": "invalid-transform",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "applyAffine",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:command-service:invalid-transform:applyAffine: nonfinite or singular affine",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "collective bounds absent",
        "code": "no-bounds",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "collective bounds absent",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:command-service:no-bounds:collective bounds absent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "collective dimension zero",
        "code": "zero-dimension",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "collective dimension zero",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:command-service:zero-dimension:collective dimension zero",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`selection_resize_affine`, `src/object/sp-item-transform.cpp:394`, then `ObjectSet::applyAffine`, `selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.resize:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.resize:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:geometry.resize:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:geometry.resize:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `geometry.rotate` | ids; finite `angle`; `pivot:{x:L,y:L}` or `anchor=center`; `bbox=geometric` | collective pivot/affine/bounds before/after | T, no-bounds, invalid-transform | E / one / computed / G; pivot/anchor mutually exclusive |
TypeDescriptor const m3_2{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "pivot": {
      "type": "object",
      "properties": {
        "x": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "y": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        }
      },
      "required": [
        "x",
        "y"
      ],
      "additionalProperties": false
    },
    "anchor": {
      "type": "string",
      "enum": [
        "nw",
        "n",
        "ne",
        "w",
        "center",
        "e",
        "sw",
        "s",
        "se"
      ],
      "default": "center"
    },
    "bbox": {
      "type": "string",
      "enum": [
        "visual",
        "geometric"
      ],
      "default": "geometric"
    },
    "angle": {
      "type": "number"
    }
  },
  "required": [
    "ids",
    "angle"
  ],
  "additionalProperties": false,
  "not": {
    "required": [
      "pivot",
      "anchor"
    ]
  },
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
    "transform_policy": {
      "space": "document-root-css-px",
      "stroke": 1,
      "rectcorners": 1,
      "pattern": 1,
      "gradient": 1,
      "preservetransform": 0,
      "dash-scale": 1,
      "group-relative-layout": "preserved",
      "matrix-without-bounds": "allowed; null bounds"
    },
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "fresh readiness guard rejects busy document",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document operation admission busy",
        "native_evidence": "`DocumentUndo::fileOperationFreshReady` / `fileOperationOutputReady`, `src/actions/vacards-cli-edit-services.cpp:361-363`",
        "oracle": "P9:geometry.rotate:identity/admission:transaction-unavailable:fresh readiness guard rejects busy document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "applyAffine: nonfinite or singular affine",
        "code": "invalid-transform",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "applyAffine",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:command-service:invalid-transform:applyAffine: nonfinite or singular affine",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "collective bounds absent",
        "code": "no-bounds",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "collective bounds absent",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:command-service:no-bounds:collective bounds absent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`ObjectSet::applyAffine`, `src/selection-chemistry.cpp:1809`",
        "oracle": "P9:geometry.rotate:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.rotate:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:geometry.rotate:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:geometry.rotate:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `geometry.skew` | ids; `axis=x|y`, finite `angle` strictly between -90 and 90; same pivot/anchor/bbox | collective affine/bounds before/after | T, no-bounds, zero-dimension, invalid-transform | E / one / computed / G; delta skew; degenerate collective target refuses |
TypeDescriptor const m3_3{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "pivot": {
      "type": "object",
      "properties": {
        "x": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "y": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        }
      },
      "required": [
        "x",
        "y"
      ],
      "additionalProperties": false
    },
    "anchor": {
      "type": "string",
      "enum": [
        "nw",
        "n",
        "ne",
        "w",
        "center",
        "e",
        "sw",
        "s",
        "se"
      ],
      "default": "center"
    },
    "bbox": {
      "type": "string",
      "enum": [
        "visual",
        "geometric"
      ],
      "default": "geometric"
    },
    "axis": {
      "type": "string",
      "enum": [
        "x",
        "y"
      ]
    },
    "angle": {
      "type": "number",
      "exclusiveMinimum": -90,
      "exclusiveMaximum": 90
    }
  },
  "required": [
    "ids",
    "axis",
    "angle"
  ],
  "additionalProperties": false,
  "not": {
    "required": [
      "pivot",
      "anchor"
    ]
  },
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "same `ObjectSet::applyAffine`, `:1809`",
    "transform_policy": {
      "space": "document-root-css-px",
      "stroke": 1,
      "rectcorners": 1,
      "pattern": 1,
      "gradient": 1,
      "preservetransform": 0,
      "dash-scale": 1,
      "group-relative-layout": "preserved",
      "matrix-without-bounds": "allowed; null bounds"
    },
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "fresh readiness guard rejects busy document",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document operation admission busy",
        "native_evidence": "`DocumentUndo::fileOperationFreshReady` / `fileOperationOutputReady`, `src/actions/vacards-cli-edit-services.cpp:361-363`",
        "oracle": "P9:geometry.skew:identity/admission:transaction-unavailable:fresh readiness guard rejects busy document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "applyAffine: nonfinite or singular affine",
        "code": "invalid-transform",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "applyAffine",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:command-service:invalid-transform:applyAffine: nonfinite or singular affine",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "collective bounds absent",
        "code": "no-bounds",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "collective bounds absent",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:command-service:no-bounds:collective bounds absent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "collective dimension zero",
        "code": "zero-dimension",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "collective dimension zero",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:command-service:zero-dimension:collective dimension zero",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.skew:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.skew:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:geometry.skew:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:geometry.skew:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `geometry.flip` | ids; `axis=horizontal|vertical`; same pivot/anchor/bbox | collective affine/bounds before/after | T, no-bounds, invalid-transform | E / one / computed / G |
TypeDescriptor const m3_4{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "pivot": {
      "type": "object",
      "properties": {
        "x": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        },
        "y": {
          "type": "object",
          "properties": {
            "value": {
              "type": "number",
              "minimum": -1000000.0,
              "maximum": 1000000.0
            },
            "unit": {
              "type": "string",
              "enum": [
                "px",
                "mm",
                "cm",
                "in",
                "pt",
                "pc"
              ]
            }
          },
          "required": [
            "value",
            "unit"
          ],
          "additionalProperties": false,
          "x-css-px-absolute-maximum": 1000000,
          "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
          "allOf": [
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "px"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -1000000.0,
                    "maximum": 1000000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "mm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -264583.3333333333,
                    "maximum": 264583.3333333333
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "cm"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -26458.333333333336,
                    "maximum": 26458.333333333336
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "in"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -10416.666666666666,
                    "maximum": 10416.666666666666
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pt"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -750000.0,
                    "maximum": 750000.0
                  }
                }
              }
            },
            {
              "if": {
                "properties": {
                  "unit": {
                    "const": "pc"
                  }
                },
                "required": [
                  "unit"
                ]
              },
              "then": {
                "properties": {
                  "value": {
                    "minimum": -62500.0,
                    "maximum": 62500.0
                  }
                }
              }
            }
          ]
        }
      },
      "required": [
        "x",
        "y"
      ],
      "additionalProperties": false
    },
    "anchor": {
      "type": "string",
      "enum": [
        "nw",
        "n",
        "ne",
        "w",
        "center",
        "e",
        "sw",
        "s",
        "se"
      ],
      "default": "center"
    },
    "bbox": {
      "type": "string",
      "enum": [
        "visual",
        "geometric"
      ],
      "default": "geometric"
    },
    "axis": {
      "type": "string",
      "enum": [
        "horizontal",
        "vertical"
      ]
    }
  },
  "required": [
    "ids",
    "axis"
  ],
  "additionalProperties": false,
  "not": {
    "required": [
      "pivot",
      "anchor"
    ]
  },
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "same `ObjectSet::applyAffine`, `:1809`",
    "transform_policy": {
      "space": "document-root-css-px",
      "stroke": 1,
      "rectcorners": 1,
      "pattern": 1,
      "gradient": 1,
      "preservetransform": 0,
      "dash-scale": 1,
      "group-relative-layout": "preserved",
      "matrix-without-bounds": "allowed; null bounds"
    },
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "fresh readiness guard rejects busy document",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document operation admission busy",
        "native_evidence": "`DocumentUndo::fileOperationFreshReady` / `fileOperationOutputReady`, `src/actions/vacards-cli-edit-services.cpp:361-363`",
        "oracle": "P9:geometry.flip:identity/admission:transaction-unavailable:fresh readiness guard rejects busy document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "applyAffine: nonfinite or singular affine",
        "code": "invalid-transform",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "applyAffine",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:command-service:invalid-transform:applyAffine: nonfinite or singular affine",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "collective bounds absent",
        "code": "no-bounds",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "collective bounds absent",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:command-service:no-bounds:collective bounds absent",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.flip:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.flip:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:geometry.flip:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:geometry.flip:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `geometry.matrix` | ids; `matrix:[a,b,c,d,e,f]` finite, nonsingular | composed root affines/bounds before/after | T, invalid-transform | E / one / computed / G; apply once in root space, convert to each parent's space |
TypeDescriptor const m3_5{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "matrix": {
      "type": "array",
      "items": {
        "type": "number"
      },
      "minItems": 6,
      "maxItems": 6
    }
  },
  "required": [
    "ids",
    "matrix"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "same `ObjectSet::applyAffine`, `:1809`",
    "transform_policy": {
      "space": "document-root-css-px",
      "stroke": 1,
      "rectcorners": 1,
      "pattern": 1,
      "gradient": 1,
      "preservetransform": 0,
      "dash-scale": 1,
      "group-relative-layout": "preserved",
      "matrix-without-bounds": "allowed; null bounds"
    },
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "fresh readiness guard rejects busy document",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document operation admission busy",
        "native_evidence": "`DocumentUndo::fileOperationFreshReady` / `fileOperationOutputReady`, `src/actions/vacards-cli-edit-services.cpp:361-363`",
        "oracle": "P9:geometry.matrix:identity/admission:transaction-unavailable:fresh readiness guard rejects busy document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "applyAffine: nonfinite or singular affine",
        "code": "invalid-transform",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "applyAffine",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:command-service:invalid-transform:applyAffine: nonfinite or singular affine",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "same `ObjectSet::applyAffine`, `:1809`",
        "oracle": "P9:geometry.matrix:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.matrix:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:geometry.matrix:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:geometry.matrix:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `geometry.boolean` | ordered ids; `op=union|intersection|difference|xor|division`; `empty-result=reject|allow` default reject | output IDs, paths/bounds, operands consumed | T, needs-two-operands, incompatible-operands, boolean-failed, empty-result | E / one / computed / G with explicit ordered operand policy; first is subject for difference/division |
TypeDescriptor const m3_6{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "op": {
      "type": "string",
      "enum": [
        "union",
        "intersection",
        "difference",
        "xor",
        "division"
      ]
    },
    "empty-result": {
      "type": "string",
      "enum": [
        "reject",
        "allow"
      ]
    }
  },
  "required": [
    "ids",
    "op",
    "empty-result"
  ],
  "additionalProperties": false,
  "allOf": [
    {
      "if": {
        "properties": {
          "op": {
            "const": "division"
          }
        },
        "required": [
          "op"
        ]
      },
      "then": {
        "properties": {
          "ids": {
            "minItems": 2,
            "maxItems": 2
          }
        }
      }
    },
    {
      "if": {
        "properties": {
          "op": {
            "enum": [
              "intersection",
              "difference",
              "xor"
            ]
          }
        },
        "required": [
          "op"
        ]
      },
      "then": {
        "properties": {
          "ids": {
            "minItems": 2
          }
        }
      }
    }
  ],
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "collective-geometry",
    "target_cardinality": {
      "min": 1,
      "max": 100000,
      "division": {
        "min": 2,
        "max": 2
      },
      "other-than-union": {
        "min": 2,
        "max": 100000
      }
    },
    "normalization": "ordered distinct whole operand roots; reject ancestor/descendant overlap, never silently drop an operand",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input[0] subject; remaining roots ordered operands; division input[1] cutter; stacking ignored; xor maps to native exclusion",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "EditTransaction inactive",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EditTransaction inactive",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:identity/admission:transaction-unavailable:EditTransaction inactive",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "BooleanOperandReason::InvalidOperation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "raw request schema rejects before native call; defensive native status maps identically",
        "detail.reason": "BooleanOperandReason::InvalidOperation",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:transport/schema:invalid-argument:BooleanOperandReason::InvalidOperation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "BooleanOperandReason::InvalidCount",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "raw request schema rejects before native call; defensive native status maps identically",
        "detail.reason": "BooleanOperandReason::InvalidCount",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:transport/schema:invalid-argument:BooleanOperandReason::InvalidCount",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "BooleanOperandReason::InvalidOperand",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "BooleanOperandReason::InvalidOperand",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:unknown-id:BooleanOperandReason::InvalidOperand",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "BooleanOperandReason::OverlappingOperands",
        "code": "incompatible-operands",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "BooleanOperandReason::OverlappingOperands",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:incompatible-operands:BooleanOperandReason::OverlappingOperands",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "BooleanOperandReason::Unavailable",
        "code": "unavailable",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "BooleanOperandReason::Unavailable",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:unavailable:BooleanOperandReason::Unavailable",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "BooleanOperandReason::GroupEffect",
        "code": "incompatible-operands",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "BooleanOperandReason::GroupEffect",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:incompatible-operands:BooleanOperandReason::GroupEffect",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "BooleanOperandReason::NotAShape",
        "code": "incompatible-operands",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "BooleanOperandReason::NotAShape",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:incompatible-operands:BooleanOperandReason::NotAShape",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "BooleanOperandReason::EmptyGeometry",
        "code": "incompatible-operands",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "BooleanOperandReason::EmptyGeometry",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:incompatible-operands:BooleanOperandReason::EmptyGeometry",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "BooleanOperandReason::EmptyGroup",
        "code": "incompatible-operands",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "BooleanOperandReason::EmptyGroup",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:incompatible-operands:BooleanOperandReason::EmptyGroup",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "BooleanOperandReason::UnsafeTransform",
        "code": "invalid-transform",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "BooleanOperandReason::UnsafeTransform",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:invalid-transform:BooleanOperandReason::UnsafeTransform",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "BooleanOperandReason::EmptyResult",
        "code": "empty-result",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "BooleanOperandReason::EmptyResult",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:empty-result:BooleanOperandReason::EmptyResult",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`apply_boolean_assist`, `src/ui/toolbar/boolean-assist.cpp:340`; native `pathUnion/pathIntersect/pathDiff/pathSymDiff/pathCut`, `src/path/path-object-set.cpp:43,48,53,122,127`",
        "oracle": "P9:geometry.boolean:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "BooleanOperandStatus::Applied (outputs nonempty)",
      "BooleanOperandStatus::Applied (empty policy allow; consumed roots)"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "BooleanOperandStatus::Applied (outputs nonempty)",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.boolean:native:BooleanOperandStatus::Applied (outputs nonempty)",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "BooleanOperandStatus::Applied (empty policy allow; consumed roots)",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.boolean:native:BooleanOperandStatus::Applied (empty policy allow; consumed roots)",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `geometry.offset` | ids; positive `distance:L`; `direction=outward|inward|both` default outward, `corner=round|bevel|miter` default miter; `miter-limit=4` in 1..100; `outer-only=false`, `delete-originals=false`, `simplify=false`, `simplify-tolerance=0.05px`, `select-results=true` | paths/bounds, source→output map, exclusions | T, no-closed-shapes, offset-failed | E / one / computed / G refined per-selected-root, existing partial policy; never delete an excluded source |
TypeDescriptor const m3_7{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 100000,
      "uniqueItems": true
    },
    "distance": {
      "type": "object",
      "properties": {
        "value": {
          "type": "number",
          "minimum": 0,
          "maximum": 1000000.0,
          "exclusiveMinimum": 0
        },
        "unit": {
          "type": "string",
          "enum": [
            "px",
            "mm",
            "cm",
            "in",
            "pt",
            "pc"
          ]
        }
      },
      "required": [
        "value",
        "unit"
      ],
      "additionalProperties": false,
      "x-css-px-absolute-maximum": 1000000,
      "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
      "allOf": [
        {
          "if": {
            "properties": {
              "unit": {
                "const": "px"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 1000000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "mm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 264583.3333333333
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "cm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 26458.333333333336
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "in"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 10416.666666666666
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pt"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 750000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pc"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 62500.0
              }
            }
          }
        }
      ]
    },
    "direction": {
      "type": "string",
      "enum": [
        "outward",
        "inward",
        "both"
      ],
      "default": "outward"
    },
    "corner": {
      "type": "string",
      "enum": [
        "round",
        "bevel",
        "miter"
      ],
      "default": "miter"
    },
    "miter-limit": {
      "type": "number",
      "minimum": 1,
      "maximum": 100,
      "default": 4
    },
    "outer-only": {
      "type": "boolean",
      "default": false
    },
    "delete-originals": {
      "type": "boolean",
      "default": false
    },
    "simplify": {
      "type": "boolean",
      "default": false
    },
    "simplify-tolerance": {
      "type": "object",
      "properties": {
        "value": {
          "type": "number",
          "minimum": 0,
          "maximum": 1000000.0
        },
        "unit": {
          "type": "string",
          "enum": [
            "px",
            "mm",
            "cm",
            "in",
            "pt",
            "pc"
          ]
        }
      },
      "required": [
        "value",
        "unit"
      ],
      "additionalProperties": false,
      "x-css-px-absolute-maximum": 1000000,
      "default": {
        "value": 0.05,
        "unit": "px"
      },
      "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
      "allOf": [
        {
          "if": {
            "properties": {
              "unit": {
                "const": "px"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 1000000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "mm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 264583.3333333333
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "cm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 26458.333333333336
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "in"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 10416.666666666666
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pt"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 750000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pc"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 62500.0
              }
            }
          }
        }
      ]
    },
    "select-results": {
      "type": "boolean",
      "default": true
    }
  },
  "required": [
    "ids",
    "distance"
  ],
  "additionalProperties": false,
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "per-selected-root-geometry",
    "target_cardinality": {
      "min": 1,
      "max": 100000
    },
    "normalization": "ordered-composite-roots; ancestor covers descendant, preserving surviving input order",
    "partial_policy": "preserve-and-report-exclusions",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
    "warnings": [
      {
        "code": "exclusions",
        "emit_site": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`; after target resolution, nonempty exclusions"
      },
      {
        "code": "simplify-skipped",
        "emit_site": "OffsetShapes::Build::simplify_skipped > 0; original built geometry retained"
      }
    ],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "EditTransaction inactive",
        "code": "transaction-unavailable",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "EditTransaction inactive",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:identity/admission:transaction-unavailable:EditTransaction inactive",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "prepare: no closed compatible sources",
        "code": "no-closed-shapes",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "prepare",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:command-service:no-closed-shapes:prepare: no closed compatible sources",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "build: failed geometry",
        "code": "offset-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "build",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:command-service:offset-failed:build: failed geometry",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "commit: geometry_still_matches false",
        "code": "stale-dependency",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "commit",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:command-service:stale-dependency:commit: geometry_still_matches false",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "commit failure; caller rollback",
        "code": "offset-failed",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "commit failure; caller rollback",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:command-service:offset-failed:commit failure; caller rollback",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`OffsetShapes::prepare/build/commit`, `src/path/offset-shapes.cpp:317,354,427`",
        "oracle": "P9:geometry.offset:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.offset:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:geometry.offset:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:geometry.offset:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
// | `geometry.corners` | exactly one id; `radius:L>=0`, `mode=round|inverse-round` default round; `scope=all|nodes` default all; nodes list `P:N` only when nodes | output/path/bounds, converted polygon/polyline report | T, requires-single-shape, unsupported-shape, path-not-supported, non-similarity-transform, no-such-node, no-corners, corner-edit-failed | E / one / computed / G single-shape exception; preserve AC-8b/native vertex restrictions |
TypeDescriptor const m3_8{boost::json::parse(R"m3({
  "type": "object",
  "properties": {
    "ids": {
      "type": "array",
      "items": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "minItems": 1,
      "maxItems": 1,
      "uniqueItems": true
    },
    "radius": {
      "type": "object",
      "properties": {
        "value": {
          "type": "number",
          "minimum": 0,
          "maximum": 1000000.0
        },
        "unit": {
          "type": "string",
          "enum": [
            "px",
            "mm",
            "cm",
            "in",
            "pt",
            "pc"
          ]
        }
      },
      "required": [
        "value",
        "unit"
      ],
      "additionalProperties": false,
      "x-css-px-absolute-maximum": 1000000,
      "x-nonnegative-css-px": true,
      "description": "Convert recursively to CSS px at 96 dpi, then enforce absolute <=1000000 and sign bound. No rounding or clamping.",
      "allOf": [
        {
          "if": {
            "properties": {
              "unit": {
                "const": "px"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 1000000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "mm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 264583.3333333333
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "cm"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 26458.333333333336
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "in"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 10416.666666666666
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pt"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 750000.0
              }
            }
          }
        },
        {
          "if": {
            "properties": {
              "unit": {
                "const": "pc"
              }
            },
            "required": [
              "unit"
            ]
          },
          "then": {
            "properties": {
              "value": {
                "minimum": 0,
                "maximum": 62500.0
              }
            }
          }
        }
      ]
    },
    "mode": {
      "type": "string",
      "enum": [
        "round",
        "inverse-round"
      ],
      "default": "round"
    },
    "scope": {
      "type": "string",
      "enum": [
        "all",
        "nodes"
      ],
      "default": "all"
    },
    "nodes": {
      "type": "array",
      "items": {
        "type": "string",
        "pattern": "^[0-9]+:[0-9]+$"
      },
      "minItems": 1,
      "maxItems": 100000
    }
  },
  "required": [
    "ids",
    "radius"
  ],
  "additionalProperties": false,
  "allOf": [
    {
      "if": {
        "properties": {
          "scope": {
            "const": "nodes"
          }
        },
        "required": [
          "scope"
        ]
      },
      "then": {
        "required": [
          "nodes"
        ]
      }
    },
    {
      "if": {
        "required": [
          "nodes"
        ]
      },
      "then": {
        "properties": {
          "scope": {
            "const": "nodes"
          }
        },
        "required": [
          "scope"
        ]
      }
    }
  ],
  "x-m3-contract": {
    "guard_domain": "document",
    "guard_required": true,
    "needs_document": true,
    "selection_mode": "single-object",
    "target_cardinality": {
      "min": 1,
      "max": 1
    },
    "normalization": "one explicit root or retained target; no member expansion",
    "partial_policy": "reject-any-incompatible",
    "role_order": "input-order; each normalized root once",
    "normalization_order": [
      "validate raw schema and exclusive route",
      "select route",
      "fill defaults recursively only in selected branch; pivot suppresses anchor, size suppresses dpi, token suppresses recipe/analyze/solve",
      "parse exact integers",
      "convert all lengths including nested recipes to CSS px and enforce bounds",
      "resolve and normalize targets",
      "canonicalize normalized params for token binding"
    ],
    "native_service": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
    "warnings": [],
    "error_rows": [
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: request-too-large",
        "code": "request-too-large",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: request-too-large",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:transport/schema:request-too-large:parse_request: request-too-large",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: repeated-key",
        "code": "repeated-key",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: repeated-key",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:transport/schema:repeated-key:parse_request: repeated-key",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: invalid-utf8",
        "code": "invalid-utf8",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: invalid-utf8",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:transport/schema:invalid-utf8:parse_request: invalid-utf8",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "parse_request: malformed-json",
        "code": "malformed-json",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "parse_request: malformed-json",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:transport/schema:malformed-json:parse_request: malformed-json",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "transport/schema",
        "native_branch": "raw schema/type/range/route/uniqueItems violation",
        "code": "invalid-argument",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "raw schema/type/range/route/uniqueItems violation",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:transport/schema:invalid-argument:raw schema/type/range/route/uniqueItems violation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "missing current document",
        "code": "no-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "missing current document",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:identity/admission:no-document:missing current document",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "document identity mismatch",
        "code": "stale-document",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document identity mismatch",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:identity/admission:stale-document:document identity mismatch",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "supplied guard differs from document revision",
        "code": "stale-revision",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "supplied guard differs from document revision",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:identity/admission:stale-revision:supplied guard differs from document revision",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "cancellation observed before commit",
        "code": "cancelled",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "cancellation observed before commit",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:identity/admission:cancelled:cancellation observed before commit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "explicit ID cannot resolve",
        "code": "unknown-id",
        "retryable": false,
        "mutation_state": "none",
        "route": "explicit-target route",
        "detail.reason": "explicit ID cannot resolve",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:identity/admission:unknown-id:explicit ID cannot resolve",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "requires-single-shape",
        "code": "requires-single-shape",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "requires-single-shape",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:requires-single-shape:requires-single-shape",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unsupported-shape",
        "code": "unsupported-shape",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "unsupported-shape",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:unsupported-shape:unsupported-shape",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "path-not-supported",
        "code": "path-not-supported",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "path-not-supported",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:path-not-supported:path-not-supported",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "non-similarity-transform",
        "code": "non-similarity-transform",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "non-similarity-transform",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:non-similarity-transform:non-similarity-transform",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "no-such-node",
        "code": "no-such-node",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "no-such-node",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:no-such-node:no-such-node",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "no-corners",
        "code": "no-corners",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "no-corners",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:no-corners:no-corners",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "invalid-input",
        "code": "invalid-input",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "invalid-input",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:invalid-input:invalid-input",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "document-busy",
        "code": "document-busy",
        "retryable": true,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "document-busy",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:document-busy:document-busy",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "apply refused before mutation",
        "code": "corner-edit-failed",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "native pre-mutation refusal reason",
        "native_evidence": "`apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:247-286`; `execute_corners`, `src/actions/vacards-cli-edit-services.cpp:469-471`",
        "oracle": "P9:geometry.corners:command-service:corner-edit-failed:apply refused before mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "apply failed after mutation; settled rollback",
        "code": "corner-edit-failed",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "native post-mutation failure reason",
        "native_evidence": "`apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:288-393`; `execute_corners`, `src/actions/vacards-cli-edit-services.cpp:469-471`",
        "oracle": "P9:geometry.corners:command-service:corner-edit-failed:apply failed after mutation; settled rollback",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "identity/admission",
        "native_branch": "read-only session rejects requested document mutation",
        "code": "document-read-only",
        "retryable": false,
        "mutation_state": "none",
        "route": "document-edit or history mutation only",
        "detail.reason": "read-only session rejects requested document mutation",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:identity/admission:document-read-only:read-only session rejects requested document mutation",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "CornerEdit::Status::EngineLimit",
        "code": "engine-limit",
        "retryable": false,
        "mutation_state": "none",
        "route": "all",
        "detail.reason": "CornerEdit::Status::EngineLimit",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:engine-limit:CornerEdit::Status::EngineLimit",
        "evidence_status": "required-unimplemented"
      },
      {
        "layer": "command-service",
        "native_branch": "unexpected service exception; rollback before return",
        "code": "internal-error",
        "retryable": false,
        "mutation_state": "rolled-back",
        "route": "all",
        "detail.reason": "unexpected service exception; rollback before return",
        "native_evidence": "`capture_corner_rounding_document`, `src/ui/tools/corner-rounding-context.cpp:124`; `check_corner_plan/apply_corner_plan`, `src/ui/tools/corner-rounding-controller.cpp:217,247`",
        "oracle": "P9:geometry.corners:command-service:internal-error:unexpected service exception; rollback before return",
        "evidence_status": "required-unimplemented"
      }
    ],
    "success_native_branches": [
      "native successful outcome",
      "native unchanged outcome when applicable",
      "read-only computed preparation"
    ],
    "variant_applicability": {
      "success": "ok or changed",
      "unchanged": "native no-op only; query/preparation/history empty are not successful unchanged",
      "computed-dry-run": "successful read-only preflight; no retention, publication, Undo or Redo change"
    },
    "success_rows": [
      {
        "native_branch": "native successful outcome",
        "code": null,
        "retryable": false,
        "mutation_state": "committed",
        "data_variant": "success",
        "evidence_route": "P9:geometry.corners:native:native successful outcome",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "native unchanged outcome when applicable",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "unchanged",
        "evidence_route": "P9:geometry.corners:native:native unchanged outcome when applicable",
        "evidence_status": "required-unimplemented"
      },
      {
        "native_branch": "read-only computed preparation",
        "code": null,
        "retryable": false,
        "mutation_state": "none",
        "data_variant": "computed-dry-run",
        "evidence_route": "P9:geometry.corners:native:read-only computed preparation",
        "evidence_status": "required-unimplemented"
      }
    ]
  }
})m3").as_object()};
}
std::vector<PackageCommand> geometry_commands()
{
    std::vector<PackageCommand> out;
    out.push_back({ActionSpec{.name="geometry.move", .mode="G", .summary="Move selected objects by the requested offset.", .canonical_id="geometry.move", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_0}, "geometry.move", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="geometry.resize", .mode="G", .summary="Resize selected objects to the requested dimensions.", .canonical_id="geometry.resize", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_1}, "geometry.resize", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="geometry.rotate", .mode="G", .summary="Rotate selected objects around the requested pivot.", .canonical_id="geometry.rotate", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_2}, "geometry.rotate", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="geometry.skew", .mode="G", .summary="Skew selected objects by the requested angles.", .canonical_id="geometry.skew", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_3}, "geometry.skew", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="geometry.flip", .mode="G", .summary="Flip selected objects across the requested axis.", .canonical_id="geometry.flip", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_4}, "geometry.flip", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="geometry.matrix", .mode="G", .summary="Apply the requested matrix to selected objects.", .canonical_id="geometry.matrix", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_5}, "geometry.matrix", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="geometry.boolean", .mode="G", .summary="Apply the requested Boolean operation to selected shapes.", .canonical_id="geometry.boolean", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_6}, "geometry.boolean", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="geometry.offset", .mode="G", .summary="Offset selected paths by the requested distance.", .canonical_id="geometry.offset", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_7}, "geometry.offset", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out.push_back({ActionSpec{.name="geometry.corners", .mode="G", .summary="Round or chamfer the corners of the selected shape by the requested radius.", .canonical_id="geometry.corners", .handler=production_unavailable_action, .version=2, .effects="document-edit", .target_policy="G", .undo_policy="one-step", .dry_run_grade="computed", .cancellation_boundary="before-native-publication", .input=&m3_8}, "geometry.corners", "document-edit", "G", {}, {{"type","object"},{"properties",boost::json::object{}},{"additionalProperties",false}}, {"slice-unavailable"}, {}});
    out[0].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ],
  "dx": {
    "value": 1,
    "unit": "mm"
  },
  "dy": {
    "value": 1,
    "unit": "mm"
  }
})m3").as_object();
    out[0].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[0].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","invalid-transform","no-bounds","document-read-only","internal-error"};
    out[1].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ],
  "width": {
    "value": 1,
    "unit": "mm"
  }
})m3").as_object();
    out[1].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[1].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","invalid-transform","no-bounds","zero-dimension","document-read-only","internal-error"};
    out[2].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ],
  "angle": 15
})m3").as_object();
    out[2].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[2].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","invalid-transform","no-bounds","document-read-only","internal-error"};
    out[3].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ],
  "axis": "x",
  "angle": 15
})m3").as_object();
    out[3].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[3].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","invalid-transform","no-bounds","zero-dimension","document-read-only","internal-error"};
    out[4].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ],
  "axis": "horizontal"
})m3").as_object();
    out[4].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "pivot": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 2,
          "maxItems": 2
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after",
        "pivot"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[4].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","invalid-transform","no-bounds","document-read-only","internal-error"};
    out[5].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ],
  "matrix": [
    1,
    0,
    0,
    1,
    0,
    0
  ]
})m3").as_object();
    out[5].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "transforms": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "before": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              },
              "after": {
                "type": "array",
                "items": {
                  "type": "number"
                },
                "minItems": 6,
                "maxItems": 6
              }
            },
            "required": [
              "id",
              "before",
              "after"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "coordinate-space": {
          "const": "document-css-px"
        },
        "applied-affine": {
          "type": "array",
          "items": {
            "type": "number"
          },
          "minItems": 6,
          "maxItems": 6
        },
        "bounds-before": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        },
        "bounds-after": {
          "oneOf": [
            {
              "type": "array",
              "items": {
                "type": "number"
              },
              "minItems": 4,
              "maxItems": 4
            },
            {
              "type": "null"
            }
          ]
        }
      },
      "required": [
        "variant",
        "transforms",
        "coordinate-space",
        "applied-affine",
        "bounds-before",
        "bounds-after"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[5].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","invalid-transform","document-read-only","internal-error"};
    out[6].example = boost::json::parse(R"m3({
  "ids": [
    "shape1",
    "shape2"
  ],
  "op": "union",
  "empty-result": "reject"
})m3").as_object();
    out[6].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "paths": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "d": {
                "type": "string"
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              }
            },
            "required": [
              "id",
              "d",
              "bounds"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "consumed-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-output": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": "string"
              },
              "outputs": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              }
            },
            "required": [
              "source",
              "outputs"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "paths",
        "consumed-ids",
        "source-output"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "paths": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "d": {
                "type": "string"
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              }
            },
            "required": [
              "id",
              "d",
              "bounds"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "consumed-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-output": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": "string"
              },
              "outputs": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              }
            },
            "required": [
              "source",
              "outputs"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "paths",
        "consumed-ids",
        "source-output"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "paths": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "d": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              }
            },
            "required": [
              "id",
              "d",
              "bounds"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "consumed-ids": {
          "type": "array",
          "items": {
            "type": [
              "string",
              "null"
            ],
            "description": "null for prospective output; existing source IDs remain strings"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-output": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "outputs": {
                "type": "array",
                "items": {
                  "type": [
                    "string",
                    "null"
                  ],
                  "description": "null for prospective output; existing source IDs remain strings"
                },
                "minItems": 0,
                "maxItems": 100000
              }
            },
            "required": [
              "source",
              "outputs"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "paths",
        "consumed-ids",
        "source-output"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[6].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","document-read-only","incompatible-operands","unavailable","invalid-transform","empty-result","internal-error"};
    out[7].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ],
  "distance": {
    "value": 1,
    "unit": "mm"
  }
})m3").as_object();
    out[7].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "paths": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "d": {
                "type": "string"
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              }
            },
            "required": [
              "id",
              "d",
              "bounds"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "consumed-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-output": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": "string"
              },
              "outputs": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              }
            },
            "required": [
              "source",
              "outputs"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "paths",
        "consumed-ids",
        "source-output"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "paths": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "d": {
                "type": "string"
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              }
            },
            "required": [
              "id",
              "d",
              "bounds"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "consumed-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-output": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": "string"
              },
              "outputs": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              }
            },
            "required": [
              "source",
              "outputs"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "paths",
        "consumed-ids",
        "source-output"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "paths": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "d": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              }
            },
            "required": [
              "id",
              "d",
              "bounds"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "consumed-ids": {
          "type": "array",
          "items": {
            "type": [
              "string",
              "null"
            ],
            "description": "null for prospective output; existing source IDs remain strings"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-output": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "outputs": {
                "type": "array",
                "items": {
                  "type": [
                    "string",
                    "null"
                  ],
                  "description": "null for prospective output; existing source IDs remain strings"
                },
                "minItems": 0,
                "maxItems": 100000
              }
            },
            "required": [
              "source",
              "outputs"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "paths",
        "consumed-ids",
        "source-output"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[7].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","transaction-unavailable","no-closed-shapes","offset-failed","stale-dependency","document-read-only","internal-error"};
    out[8].example = boost::json::parse(R"m3({
  "ids": [
    "shape1"
  ],
  "radius": {
    "value": 1,
    "unit": "mm"
  }
})m3").as_object();
    out[8].result_data = boost::json::parse(R"m3({
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "success"
        },
        "paths": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "d": {
                "type": "string"
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              }
            },
            "required": [
              "id",
              "d",
              "bounds"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "consumed-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-output": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": "string"
              },
              "outputs": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              }
            },
            "required": [
              "source",
              "outputs"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "converted": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "from": {
                "enum": [
                  "polygon",
                  "polyline"
                ]
              },
              "to": {
                "const": "path"
              }
            },
            "required": [
              "id",
              "from",
              "to"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "paths",
        "consumed-ids",
        "source-output",
        "converted"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "unchanged"
        },
        "paths": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "d": {
                "type": "string"
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              }
            },
            "required": [
              "id",
              "d",
              "bounds"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "consumed-ids": {
          "type": "array",
          "items": {
            "type": "string"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-output": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": "string"
              },
              "outputs": {
                "type": "array",
                "items": {
                  "type": "string"
                },
                "minItems": 0,
                "maxItems": 100000
              }
            },
            "required": [
              "source",
              "outputs"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "converted": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": "string"
              },
              "from": {
                "enum": [
                  "polygon",
                  "polyline"
                ]
              },
              "to": {
                "const": "path"
              }
            },
            "required": [
              "id",
              "from",
              "to"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "paths",
        "consumed-ids",
        "source-output",
        "converted"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "variant": {
          "const": "computed-dry-run"
        },
        "paths": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "d": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "bounds": {
                "oneOf": [
                  {
                    "type": "array",
                    "items": {
                      "type": "number"
                    },
                    "minItems": 4,
                    "maxItems": 4
                  },
                  {
                    "type": "null"
                  }
                ]
              }
            },
            "required": [
              "id",
              "d",
              "bounds"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "consumed-ids": {
          "type": "array",
          "items": {
            "type": [
              "string",
              "null"
            ],
            "description": "null for prospective output; existing source IDs remain strings"
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "source-output": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "source": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "outputs": {
                "type": "array",
                "items": {
                  "type": [
                    "string",
                    "null"
                  ],
                  "description": "null for prospective output; existing source IDs remain strings"
                },
                "minItems": 0,
                "maxItems": 100000
              }
            },
            "required": [
              "source",
              "outputs"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        },
        "converted": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id": {
                "type": [
                  "string",
                  "null"
                ],
                "description": "null for prospective output; existing source IDs remain strings"
              },
              "from": {
                "enum": [
                  "polygon",
                  "polyline"
                ]
              },
              "to": {
                "const": "path"
              }
            },
            "required": [
              "id",
              "from",
              "to"
            ],
            "additionalProperties": false
          },
          "minItems": 0,
          "maxItems": 100000
        }
      },
      "required": [
        "variant",
        "paths",
        "consumed-ids",
        "source-output",
        "converted"
      ],
      "additionalProperties": false
    }
  ],
  "description": "Data only; common va-studio.cli-result/1 envelope authoritative. Variant selected by status/dry_run; any duplicated fields must equal envelope. Preview IDs never claim live publication."
})m3").as_object();
    out[8].errors = {"request-too-large","repeated-key","invalid-utf8","malformed-json","invalid-argument","no-document","stale-document","stale-revision","cancelled","unknown-id","requires-single-shape","unsupported-shape","path-not-supported","non-similarity-transform","no-such-node","no-corners","invalid-input","document-busy","corner-edit-failed","document-read-only","engine-limit","internal-error"};
    out[0].policy = "collective-geometry"; out[0].spec.target_policy = "collective-geometry";
    out[1].policy = "collective-geometry"; out[1].spec.target_policy = "collective-geometry";
    out[2].policy = "collective-geometry"; out[2].spec.target_policy = "collective-geometry";
    out[3].policy = "collective-geometry"; out[3].spec.target_policy = "collective-geometry";
    out[4].policy = "collective-geometry"; out[4].spec.target_policy = "collective-geometry";
    out[5].policy = "collective-geometry"; out[5].spec.target_policy = "collective-geometry";
    out[6].policy = "collective-geometry"; out[6].spec.target_policy = "collective-geometry";
    out[7].policy = "per-selected-root-geometry"; out[7].spec.target_policy = "per-selected-root-geometry";
    out[8].policy = "single-object"; out[8].spec.target_policy = "single-object";
    out[0].warnings = {};
    out[1].warnings = {"keep-ratio-ignored"};
    out[2].warnings = {};
    out[3].warnings = {};
    out[4].warnings = {};
    out[5].warnings = {};
    out[6].warnings = {};
    out[7].warnings = {"exclusions","simplify-skipped"};
    out[8].warnings = {};
    return out;
}
}
