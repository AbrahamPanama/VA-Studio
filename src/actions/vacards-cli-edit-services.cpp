// SPDX-License-Identifier: GPL-2.0-or-later
#include "vacards-cli-edit-services.h"
#include "vacards-cli-fault.h"
#include "document.h"
#include "document-undo.h"
#include "event-log.h"
#include <functional>
#include <charconv>
#include <vector>
#include <algorithm>
#include <boost/json.hpp>
#include "actions-vacards-cli.h"
#include "selection.h"
#include "object/sp-item.h"
#include "util/operation-targets.h"
#include "path/offset-shapes.h"
#include "svg/svg.h"
#include "object/sp-path.h"
#include <2geom/pathvector.h>
#include <cmath>
#include <stdexcept>
#include "ui/toolbar/boolean-assist.h"
#include "object/transform-policy-scope.h"
#include "object/sp-item-transform.h"
#include "object/sp-shape.h"
#include "ui/toolbar/transform-reference.h"
#include "ui/tools/corner-rounding-controller.h"
#include "live_effects/effect.h"
namespace Inkscape::VACardsCli {
HistorySnapshotResult history_snapshot(EditServices &services)
{
    auto *document = &services.document;
    // Deliberately read only: do not acquire a lease, settle pending XML, or
    // instantiate history by performing an Undo/Redo probe. A caller already
    // holding its one operation lease uses the corresponding readiness probe.
    bool const ready = services.operation_lease
        ? DocumentUndo::fileOperationOutputReady(document)
        : DocumentUndo::fileOperationFreshReady(document);
    if (!ready || DocumentUndo::interactionCloseRequested(document))
        return {{}, ParseError{"document-busy", {}, "History is closing or an interaction is busy."}};
    auto *log = document->get_event_log();
    if (!log)
        return {{}, ParseError{"internal-error", {}, "Document EventLog is unavailable."}};

    // The first row is a sentinel, never an Undo step. Subsequent rows are
    // coalesced native events. Children group consecutive similar events for
    // presentation, but each child is still a separate native step; flatten in
    // preorder instead of treating a visual group as one history operation.
    // Keyed maybeDone updates the existing row rather than adding a step.
    auto model = log->getEventListStore();
    auto current = log->getCurrEvent();
    auto const &columns = EventLog::getColumns();
    std::vector<Gtk::TreeModel::iterator> rows;
    std::function<void(Gtk::TreeModel::Children)> visit = [&](Gtk::TreeModel::Children children) {
        for (auto row = children.begin(); row != children.end(); ++row) {
            rows.emplace_back(row);
            visit(row->children());
        }
    };
    visit(model->children());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i] != current) continue;
        HistorySnapshot snapshot;
        auto label = [&](std::size_t index) {
            Glib::ustring text = (*rows[index])[columns.description];
            return text.raw();
        };
        snapshot.can_undo = i != 0;
        snapshot.can_redo = i + 1 < rows.size();
        if (snapshot.can_undo) snapshot.next_undo_label = label(i);
        if (snapshot.can_redo) snapshot.next_redo_label = label(i + 1);
        return {std::move(snapshot), {}};
    }
    return {{}, ParseError{"internal-error", {}, "Current EventLog row is missing."}};
}
namespace {
boost::json::array ids_json(std::vector<std::string> const &ids)
{
    boost::json::array result;
    for (auto const &id : ids) result.emplace_back(id);
    return result;
}
std::vector<std::string> selected_ids(Selection &selection)
{
    std::vector<std::string> result;
    for (auto *item : selection.items()) if (item->getId()) result.emplace_back(item->getId());
    return result;
}
Record initial_record(Request const &request, DispatchContext const &context, EditServices &services)
{
    Record result;
    result.action = request.command;
    result.dry_run = request.dry_run;
    result.preferred_unit = context.preferred_unit;
    result.normalized_params = request.params;
    auto stamp = document_stamp(&services.document);
    result.document_id = stamp.id;
    result.revision_before = result.revision_after = stamp.revision;
    result.selection_after = selected_ids(services.selection);
    return result;
}
Record refuse(Record result, std::string code, std::string detail)
{
    result.status = code == "cancelled" ? Status::Cancelled : Status::Rejected;
    result.reason = code;
    result.message = detail;
    result.error_details = {{"reason", detail}, {"mutation_state", "none"}};
    result.error_retryable = code == "document-busy" || code == "transaction-unavailable";
    result.data.clear();
    result.error = ParseError{std::move(code), {}, std::move(detail)};
    return result;
}
boost::json::object history_data(HistorySnapshot const &snapshot)
{
    return {{"can-undo", snapshot.can_undo}, {"can-redo", snapshot.can_redo},
            {"next-undo-label", snapshot.next_undo_label ? boost::json::value(*snapshot.next_undo_label) : boost::json::value(nullptr)},
            {"next-redo-label", snapshot.next_redo_label ? boost::json::value(*snapshot.next_redo_label) : boost::json::value(nullptr)}};
}
} // namespace

Record execute_selection(Request const &request, DispatchContext &context, EditServices &services)
{
    // Integration owns schema, identity/session guard and read-only admission. The
    // service owns installation; integration advances the session revision after comparing IDs.
    auto result = initial_record(request, context, services);
    result.mode = "explicit-root-selection";
    auto const before = result.selection_after;
    std::vector<SPItem *> requested;
    std::vector<std::string> covered;
    if (request.command == "selection.set") {
        for (auto const &value : request.params.at("ids").as_array()) {
            auto id = std::string(value.as_string());
            auto *object = services.document.getObjectById(id);
            auto *item = cast<SPItem>(object);
            if (!item || !item->parent)
                return refuse(std::move(result), "unknown-id", "resolve_composite_targets: unknown root");
            if (!document_available(item))
                return refuse(std::move(result), "unavailable", "resolve_composite_targets: protected root");
            requested.push_back(item);
        }
    } else if (request.command != "selection.clear") {
        return production_unavailable(request);
    }
    // Selection normalization only covers ancestry, not clone/source pairs:
    // both explicitly requested rendering objects remain independently selected.
    auto resolved = Util::resolve_composite_targets(requested,
        [](SPItem *) { return Util::TargetAvailability::Eligible; },
        [](SPItem *item) { return cast<SPItem>(item->parent); },
        [](SPItem *) -> SPItem * { return nullptr; });
    std::vector<std::string> after;
    for (auto *item : resolved.items) after.emplace_back(item->getId());
    for (auto *item : requested)
        if (std::find(resolved.items.begin(), resolved.items.end(), item) == resolved.items.end())
            covered.emplace_back(item->getId());
    if (context.cancelled && context.cancelled())
        return refuse(std::move(result), "cancelled", "cancellation observed before commit");
    bool const changed = before != after;
    result.selected = requested.size();
    result.eligible = resolved.items.size();
    result.covered = covered.size();
    if (!request.dry_run && changed) {
        services.selection.setList(resolved.items);
        result.selection_after = selected_ids(services.selection);
    }
    result.status = request.dry_run ? Status::Ok : changed ? Status::Changed : Status::Unchanged;
    result.data = {{"variant", request.dry_run ? "computed-dry-run" : changed ? "success" : "unchanged"},
                   {"selection-before", ids_json(before)}, {"selection-after", ids_json(after)},
                   {"session-revision", context.session_revision + (changed ? 1 : 0)}};
    if (request.command == "selection.clear") {
        result.data["cleared-count"] = before.size();
    } else {
        result.data["normalized-ids"] = ids_json(after);
        result.data["covered-ids"] = ids_json(covered);
        result.data["exclusions"] = boost::json::array{};
    }
    return result;
}

Record execute_history(Request const &request, DispatchContext &context, EditServices &services)
{
    // Native history owns settlement. In particular never wrap Undo/Redo in an
    // EditTransaction and never exercise them just to compute a preview.
    auto result = initial_record(request, context, services);
    result.mode = "document-history";
    auto before = history_snapshot(services);
    if (before.error) {
        auto detail = before.error->code == "document-busy"
            ? (request.command == "history.query" ? "EventLog snapshot: closing or interaction busy" : "DocumentUndo admission busy")
            : before.error->message;
        return refuse(std::move(result), before.error->code, detail);
    }
    if (context.cancelled && context.cancelled())
        return refuse(std::move(result), "cancelled", "cancellation observed before commit");
    bool const query = request.command == "history.query";
    bool const redo = request.command == "history.redo";
    if (!query && !redo && request.command != "history.undo") return production_unavailable(request);
    if (!query && !(redo ? before.value->can_redo : before.value->can_undo))
        return refuse(std::move(result), "history-empty", redo ? "EventLog: no redo row" : "EventLog: no undo row");
    result.data = history_data(*before.value);
    result.data["variant"] = request.dry_run ? "computed-dry-run" : "success";
    if (query) return result;
    result.data["selection-after"] = ids_json(result.selection_after);
    if (request.dry_run) {
        result.data[redo ? "would-redo" : "would-undo"] = true;
        return result;
    }
    auto label = redo ? before.value->next_redo_label : before.value->next_undo_label;
    bool const done = redo ? DocumentUndo::redo(&services.document) : DocumentUndo::undo(&services.document);
    if (!done) {
        result.data.clear();
        return refuse(std::move(result), "document-busy", "DocumentUndo admission busy");
    }
    services.document.ensureUpToDate();
    auto after = history_snapshot(services);
    if (after.error) {
        // A successful native history operation is already settled. Never
        // misreport this as a no-mutation refusal or attempt a second operation.
        result.status = Status::Failed;
        result.reason = "internal-error";
        result.error = after.error;
        result.data.clear();
        result.revision_after = document_stamp(&services.document).revision;
        return result;
    }
    result.status = Status::Changed;
    result.undo_effect = redo ? "redo" : "undo";
    result.revision_after = document_stamp(&services.document).revision;
    result.selection_after = selected_ids(services.selection);
    result.data = history_data(*after.value);
    result.data["variant"] = "success";
    result.data["consumed-label"] = *label;
    result.data["selection-after"] = ids_json(result.selection_after);
    return result;
}
namespace {
double css_px(boost::json::value const &value)
{
    if (value.is_number()) return value.to_number<double>();
    auto const &length = value.as_object();
    auto const &unit = length.at("unit").as_string();
    double scale = unit == "mm" ? 96. / 25.4 : unit == "cm" ? 96. / 2.54 :
                   unit == "in" ? 96. : unit == "pt" ? 96. / 72. : unit == "pc" ? 16. : 1.;
    return length.at("value").to_number<double>() * scale;
}
boost::json::value bounds_json(Geom::OptRect const &bounds)
{
    if (!bounds) return nullptr;
    return boost::json::array{bounds->left(), bounds->top(), bounds->right(), bounds->bottom()};
}
boost::json::array affine_json(Geom::Affine const &a)
{
    return {a[0], a[1], a[2], a[3], a[4], a[5]};
}
bool finite_affine(Geom::Affine const &a)
{
    for (unsigned i = 0; i < 6; ++i) if (!std::isfinite(a[i])) return false;
    return std::isfinite(a.det()) && a.det() != 0;
}
Geom::OptRect collective_bounds(std::vector<SPItem *> const &roots, bool visual)
{
    Geom::OptRect bounds;
    for (auto *item : roots) bounds |= visual ? item->documentVisualBounds() : item->documentGeometricBounds();
    return bounds;
}
UI::Toolbar::TransformReferencePoint reference_point(boost::json::object const &p, char const *fallback)
{
    std::string name = p.if_contains("anchor") ? std::string(p.at("anchor").as_string()) : fallback;
    static std::vector<std::string> const names{"nw", "n", "ne", "w", "center", "e", "sw", "s", "se"};
    auto it = std::find(names.begin(), names.end(), name);
    return UI::Toolbar::transform_reference_from_index(std::distance(names.begin(), it));
}
Record execute_transform(Request const &request, DispatchContext &context, EditServices &services)
{
    auto result = initial_record(request, context, services);
    result.mode = "collective-geometry";
    try {
        auto const &p = request.params;
        std::vector<SPItem *> raw;
        for (auto const &value : p.at("ids").as_array()) {
            auto *item = cast<SPItem>(services.document.getObjectById(std::string(value.as_string())));
            if (!item || !item->parent) return refuse(std::move(result), "unknown-id", "explicit ID cannot resolve");
            raw.push_back(item);
        }
        auto normalized = Util::resolve_composite_targets(raw,
            [](SPItem *) { return Util::TargetAvailability::Eligible; },
            [](SPItem *item) { return cast<SPItem>(item->parent); },
            [](SPItem *) -> SPItem * { return nullptr; });
        std::vector<SPItem *> roots;
        for (auto *item : normalized.items) {
            if (document_available(item)) roots.push_back(item);
            else result.excluded.push_back({item->getId(), "unavailable"});
        }
        result.selected = raw.size(); result.covered = normalized.covered; result.eligible = roots.size();
        if (roots.empty()) return refuse(std::move(result), "no-eligible-targets", "no available transform roots");
        // Reject unsafe parent/item frames before native inversion or any writes.
        for (auto *item : roots)
            if (!finite_affine(item->i2doc_affine()))
                return refuse(std::move(result), "invalid-transform", "applyAffine: nonfinite or singular affine");
        bool const resize = request.command == "geometry.resize";
        bool const matrix = request.command == "geometry.matrix";
        bool const move = request.command == "geometry.move";
        bool const visual = p.if_contains("bbox") ? p.at("bbox").as_string() == "visual" : resize;
        auto before = collective_bounds(roots, visual);
        if (!before && !matrix) return refuse(std::move(result), "no-bounds", "collective bounds absent");
        Geom::Affine affine;
        Geom::Point pivot;
        if (matrix) {
            auto const &m = p.at("matrix").as_array();
            affine = Geom::Affine(m[0].to_number<double>(), m[1].to_number<double>(), m[2].to_number<double>(),
                                  m[3].to_number<double>(), m[4].to_number<double>(), m[5].to_number<double>());
        } else if (move) {
            affine = Geom::Translate(css_px(p.at("dx")), css_px(p.at("dy")));
        } else {
            if (auto value = p.if_contains("pivot")) {
                auto const &xy = value->as_object(); pivot = {css_px(xy.at("x")), css_px(xy.at("y"))};
            } else pivot = UI::Toolbar::reference_position(*before, reference_point(p, resize ? "nw" : "center"));
            if (resize) {
                if (before->width() <= 1e-12 || before->height() <= 1e-12)
                    return refuse(std::move(result), "zero-dimension", "collective dimension zero");
                double w = p.if_contains("width") ? css_px(p.at("width")) : before->width();
                double h = p.if_contains("height") ? css_px(p.at("height")) : before->height();
                if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0)
                    return refuse(std::move(result), "invalid-transform", "applyAffine: nonfinite or singular affine");
                bool keep = p.if_contains("keep-ratio") && p.at("keep-ratio").as_bool();
                if (keep && p.if_contains("width") && p.if_contains("height")) result.warnings.emplace_back("keep-ratio-ignored");
                else if (keep) {
                    if (p.if_contains("width")) h = before->height() * w / before->width();
                    else w = before->width() * h / before->height();
                }
                auto target = UI::Toolbar::resize_around_reference(*before, w, h, reference_point(p, "nw"));
                Geom::Rect desktop_target(Geom::Point(target.x0, target.y0), Geom::Point(target.x1, target.y1));
                desktop_target *= services.document.doc2dt();
                ObjectSet set(&services.document); for (auto *item : roots) set.add(item);
                auto native = selection_resize_affine(set, visual, true, false, desktop_target.left(), desktop_target.top(),
                                                       desktop_target.right(), desktop_target.bottom());
                if (!native) return refuse(std::move(result), "no-bounds", "collective bounds absent");
                affine = services.document.doc2dt() * *native * services.document.dt2doc();
            } else {
                Geom::Affine linear;
                if (request.command == "geometry.rotate") linear = Geom::Rotate::from_degrees(p.at("angle").to_number<double>());
                else if (request.command == "geometry.skew") {
                    if (before->width() <= 1e-12 || before->height() <= 1e-12)
                        return refuse(std::move(result), "zero-dimension", "collective dimension zero");
                    auto t = std::tan(p.at("angle").to_number<double>() * M_PI / 180.);
                    linear = p.at("axis").as_string() == "x" ? Geom::Affine(1, 0, t, 1, 0, 0) : Geom::Affine(1, t, 0, 1, 0, 0);
                } else linear = p.at("axis").as_string() == "horizontal" ? Geom::Scale(-1, 1) : Geom::Scale(1, -1);
                affine = Geom::Translate(-pivot) * linear * Geom::Translate(pivot);
            }
        }
        if (!finite_affine(affine)) return refuse(std::move(result), "invalid-transform", "applyAffine: nonfinite or singular affine");
        if (context.cancelled && context.cancelled()) return refuse(std::move(result), "cancelled", "cancellation observed before commit");
        bool const unchanged = affine.isIdentity(1e-12);
        boost::json::array transforms;
        for (auto *item : roots) transforms.emplace_back(boost::json::object{{"id", item->getId()},
            {"before", affine_json(item->i2doc_affine())}, {"after", affine_json(item->i2doc_affine() * affine)}});
        result.data = {{"variant", request.dry_run ? "computed-dry-run" : unchanged ? "unchanged" : "success"},
            {"coordinate-space", "document-css-px"}, {"transforms", std::move(transforms)},
            {"applied-affine", affine_json(affine)}, {"bounds-before", bounds_json(before)}, {"bounds-after", bounds_json(before)}};
        if (!move && !matrix) result.data["pivot"] = boost::json::array{pivot.x(), pivot.y()};
        if (unchanged) { result.status = request.dry_run ? Status::Ok : Status::Unchanged; return result; }
        bool const ready = services.operation_lease ? DocumentUndo::fileOperationOutputReady(&services.document)
                                                    : DocumentUndo::fileOperationFreshReady(&services.document);
        if (!ready) return refuse(std::move(result), request.command == "geometry.corners" ? "document-busy" : "transaction-unavailable", "document operation admission busy");
        auto scratch = request.dry_run ? services.document.copy() : nullptr;
        auto *document = scratch ? scratch.get() : &services.document;
        if (scratch) { document->ensureUpToDate(); DocumentUndo::setUndoSensitive(document, true); }
        std::vector<SPItem *> targets;
        for (auto *item : roots) targets.push_back(cast<SPItem>(document->getObjectById(item->getId())));
        EditTransaction transaction(document, scratch ? document->getSelection() : &services.selection,
                                    scratch ? std::shared_ptr<void>{} : services.operation_lease);
        if (!transaction.active()) return refuse(std::move(result), "transaction-unavailable", "EditTransaction inactive");
        {
            ScopedTransformPolicy policy;
            ObjectSet set(document); for (auto *item : targets) set.add(item);
            // Native ObjectSet operates in desktop coordinates. Conjugate the
            // single document-space affine, retaining all native relation rules.
            set.applyAffine(document->dt2doc() * affine * document->doc2dt());
            document->ensureUpToDate();
        }
        result.data["bounds-after"] = bounds_json(collective_bounds(targets, visual));
        if (request.dry_run) return result;
        if (context.cancelled && context.cancelled()) {
            transaction.rollback(); return refuse(initial_record(request, context, services), "cancelled", "cancellation observed before commit");
        }
        for (auto *item : targets) result.modified.emplace_back(item->getId());
        transaction.commit(Util::Internal::ContextString("Transform objects"), "tool-pointer");
        result.status = Status::Changed; result.one_undo_step = true; result.undo_effect = "one-step";
        result.revision_after = document_stamp(document).revision;
        return result;
    } catch (...) {
        auto failed = refuse(initial_record(request, context, services), "internal-error", "unexpected service exception; rollback before return");
        failed.status = Status::Failed; failed.error_details["mutation_state"] = request.dry_run ? "none" : "rolled-back";
        return failed;
    }
}
Record execute_corners(Request const &request, DispatchContext &context, EditServices &services)
{
    namespace Tools = UI::Tools;
    namespace CE = LivePathEffect::CornerEdit;
    using Outcome = Tools::CornerRoundingController::Outcome;
    auto result = initial_record(request, context, services); result.mode = "single-object";
    try {
        auto const &ids = request.params.at("ids").as_array(); result.selected = ids.size();
        if (ids.size() != 1) return refuse(std::move(result), "requires-single-shape", "requires-single-shape");
        auto id = std::string(ids[0].as_string());
        auto *original = services.document.getObjectById(id);
        if (!original) return refuse(std::move(result), "unknown-id", "explicit ID cannot resolve");
        auto *shape = cast<SPShape>(original);
        if (!shape || !document_available(shape)) return refuse(std::move(result), "requires-single-shape", "requires-single-shape");
        auto capture = Tools::capture_corner_rounding_document(*shape, 0);
        if (!capture.snapshot) return refuse(std::move(result), capture.non_similarity ? "non-similarity-transform" :
            is<SPPath>(shape) ? "path-not-supported" : "unsupported-shape", capture.reason);
        auto snapshot = *capture.snapshot;
        CE::Request native_request;
        native_request.radius = css_px(request.params.at("radius")) / *capture.input_to_document_scale;
        native_request.mode = request.params.at("mode").as_string() == "inverse-round" ? CE::Mode::InverseRound : CE::Mode::Round;
        native_request.scope = request.params.at("scope").as_string() == "nodes" ? CE::Scope::Selected : CE::Scope::All;
        if (native_request.scope == CE::Scope::Selected) {
            for (auto const &value : request.params.at("nodes").as_array()) {
                auto text = std::string(value.as_string()); auto colon = text.find(':');
                CE::Address address{};
                if (colon == std::string::npos) return refuse(std::move(result), "invalid-input", "invalid corner address");
                auto a = std::from_chars(text.data(), text.data() + colon, address.path);
                auto b = std::from_chars(text.data() + colon + 1, text.data() + text.size(), address.node);
                if (a.ec != std::errc{} || b.ec != std::errc{})
                    return refuse(std::move(result), "no-such-node", "no-such-node");
                if (address.path >= snapshot.satellites.size() || address.node >= snapshot.satellites[address.path].size())
                    return refuse(std::move(result), "no-such-node", "no-such-node");
                snapshot.selected.push_back(address);
            }
        }
        auto *lpe = shape->getFirstPathEffectOfType(LivePathEffect::FILLET_CHAMFER);
        auto *effect = lpe ? lpe->getLPEObj() : nullptr;
        auto check = Tools::check_corner_plan(effect != nullptr, snapshot, native_request);
        if (!check.ready && check.outcome != Outcome::NoChange) {
            auto code = check.plan.status == CE::Status::NoCorners ? "no-corners" :
                        check.plan.status == CE::Status::EngineLimit ? "engine-limit" : "invalid-input";
            return refuse(std::move(result), code, check.reason);
        }
        bool unchanged = !check.ready;
        if (context.cancelled && context.cancelled()) return refuse(std::move(result), "cancelled", "cancellation observed before commit");
        auto converted_from = unchanged ? std::string{} : Tools::corner_conversion_from(*shape);
        bool const ready = services.operation_lease ? DocumentUndo::fileOperationOutputReady(&services.document)
                                                    : DocumentUndo::fileOperationFreshReady(&services.document);
        if (!ready) return refuse(std::move(result), request.command == "geometry.corners" ? "document-busy" : "transaction-unavailable", "document operation admission busy");
        auto scratch = request.dry_run ? services.document.copy() : nullptr;
        auto *document = scratch ? scratch.get() : &services.document;
        if (scratch) { document->ensureUpToDate(); DocumentUndo::setUndoSensitive(document, true); }
        auto *working = cast<SPShape>(document->getObjectById(id));
        auto write_data = [&] {
            document->ensureUpToDate();
            working = cast<SPShape>(document->getObjectById(id));
            auto geometry = *working->curve() * working->i2doc_affine();
            boost::json::array converted;
            if (!converted_from.empty()) converted.emplace_back(boost::json::object{{"id", id}, {"from", converted_from}, {"to", "path"}});
            result.data = {{"variant", request.dry_run ? "computed-dry-run" : unchanged ? "unchanged" : "success"},
                {"paths", boost::json::array{boost::json::object{{"id", id}, {"d", sp_svg_write_path(geometry)}, {"bounds", bounds_json(geometry.boundsExact())}}}},
                {"consumed-ids", boost::json::array{}}, {"source-output", boost::json::array{boost::json::object{{"source", id}, {"outputs", boost::json::array{id}}}}},
                {"converted", std::move(converted)}};
            result.eligible = 1;
        };
        if (unchanged) { write_data(); result.status = request.dry_run ? Status::Ok : Status::Unchanged; return result; }
        EditTransaction transaction(document, scratch ? document->getSelection() : &services.selection,
                                    scratch ? std::shared_ptr<void>{} : services.operation_lease);
        if (!transaction.active()) return refuse(std::move(result), "document-busy", "document-busy");
        auto *working_lpe = working->getFirstPathEffectOfType(LivePathEffect::FILLET_CHAMFER);
        // Test-only native-plan invalidation after normal admission/preparation.
        // The real helper rechecks the plan and returns its typed pre-write refusal.
        if (cli_fault("geometry.corners.before-native-apply", CliFaultKind::CommitRefusal))
            native_request.radius = -1;
        auto native = Tools::apply_corner_plan(*working, working_lpe ? working_lpe->getLPEObj() : nullptr, snapshot, native_request,
            Tools::CornerCommitProtocol::CallerOwnedAtomic, [&] { return !context.cancelled || !context.cancelled(); });
        if (native.outcome != Outcome::Applied) {
            transaction.rollback(); auto failed = refuse(std::move(result), "corner-edit-failed", native.reason);
            failed.status = native.mutation_started ? Status::Failed : Status::Rejected;
            failed.error_details["mutation_state"] = native.mutation_started ? "rolled-back" : "none"; return failed;
        }
        write_data();
        if (request.dry_run) return result;
        result.modified.push_back(id);
        transaction.commit(Util::Internal::ContextString("Round corners"), "fillet-chamfer");
        result.status = Status::Changed; result.one_undo_step = true; result.undo_effect = "one-step";
        result.revision_after = document_stamp(document).revision; return result;
    } catch (...) {
        auto failed = refuse(initial_record(request, context, services), "internal-error", "unexpected service exception; rollback before return");
        failed.status = Status::Failed; failed.error_details["mutation_state"] = request.dry_run ? "none" : "rolled-back"; return failed;
    }
}
Record execute_boolean(Request const &request, DispatchContext &context, EditServices &services)
{
    namespace Bool = UI::Toolbar;
    auto result = initial_record(request, context, services);
    result.mode = "collective-geometry";
    std::vector<std::string> ids;
    for (auto const &value : request.params.at("ids").as_array()) ids.emplace_back(value.as_string());
    result.selected = ids.size();
    auto const &name = request.params.at("op").as_string();
    Bool::BooleanOperandOp op;
    char const *label;
    if (name == "union") { op = Bool::BooleanOperandOp::Union; label = "Union"; }
    else if (name == "intersection") { op = Bool::BooleanOperandOp::Intersection; label = "Intersection"; }
    else if (name == "difference") { op = Bool::BooleanOperandOp::Difference; label = "Difference"; }
    else if (name == "xor") { op = Bool::BooleanOperandOp::Exclusion; label = "Exclusion"; }
    else if (name == "division") { op = Bool::BooleanOperandOp::Division; label = "Division"; }
    else return refuse(std::move(result), "invalid-argument", "BooleanOperandReason::InvalidOperation");
    auto const &policy_name = request.params.at("empty-result").as_string();
    if (policy_name != "allow" && policy_name != "reject")
        return refuse(std::move(result), "invalid-argument", "BooleanOperandReason::InvalidOperation");
    auto const policy = policy_name == "allow" ? Bool::BooleanEmptyPolicy::Allow : Bool::BooleanEmptyPolicy::Refuse;
    if (context.cancelled && context.cancelled())
        return refuse(std::move(result), "cancelled", "cancellation observed before commit");
    auto const ready = services.operation_lease ? DocumentUndo::fileOperationOutputReady(&services.document)
                                               : DocumentUndo::fileOperationFreshReady(&services.document);
    if (!ready) return refuse(std::move(result), "transaction-unavailable", "EditTransaction inactive");
    try {
        // The ordered service has no separate preview API. Execute it on an
        // independent document for computed preview, never on live XML followed
        // by Undo. This also exercises exactly the real eligibility/empty policy.
        auto scratch = request.dry_run ? services.document.copy() : nullptr;
        auto *document = scratch ? scratch.get() : &services.document;
        if (scratch) { document->ensureUpToDate(); DocumentUndo::setUndoSensitive(document, true); }
        std::vector<SPItem *> roots;
        for (auto const &id : ids) roots.push_back(cast<SPItem>(document->getObjectById(id)));
        EditTransaction transaction(document, scratch ? document->getSelection() : &services.selection,
                                    scratch ? std::shared_ptr<void>{} : services.operation_lease);
        if (!transaction.active()) return refuse(std::move(result), "transaction-unavailable", "EditTransaction inactive");
        // Preserve raw order and cardinality. The seam rejects ancestor overlap
        // rather than silently removing a requested subject or cutter.
        auto native = Bool::apply_boolean_assist(document, roots, op, policy);
        if (native.status == Bool::BooleanOperandStatus::Refused) {
            char const *code = "internal-error", *reason = "BooleanOperandReason::None";
            using Reason = Bool::BooleanOperandReason;
            switch (native.reason) {
                case Reason::MissingDocument: code = "no-document"; reason = "BooleanOperandReason::MissingDocument"; break;
                case Reason::InvalidOperation: code = "invalid-argument"; reason = "BooleanOperandReason::InvalidOperation"; break;
                case Reason::InvalidCount: code = "invalid-argument"; reason = "BooleanOperandReason::InvalidCount"; break;
                case Reason::InvalidOperand: code = "unknown-id"; reason = "BooleanOperandReason::InvalidOperand"; break;
                case Reason::OverlappingOperands: code = "incompatible-operands"; reason = "BooleanOperandReason::OverlappingOperands"; break;
                case Reason::Unavailable: code = "unavailable"; reason = "BooleanOperandReason::Unavailable"; break;
                case Reason::GroupEffect: code = "incompatible-operands"; reason = "BooleanOperandReason::GroupEffect"; break;
                case Reason::NotAShape: code = "incompatible-operands"; reason = "BooleanOperandReason::NotAShape"; break;
                case Reason::EmptyGeometry: code = "incompatible-operands"; reason = "BooleanOperandReason::EmptyGeometry"; break;
                case Reason::EmptyGroup: code = "incompatible-operands"; reason = "BooleanOperandReason::EmptyGroup"; break;
                case Reason::UnsafeTransform: code = "invalid-transform"; reason = "BooleanOperandReason::UnsafeTransform"; break;
                case Reason::EmptyResult: code = "empty-result"; reason = "BooleanOperandReason::EmptyResult"; break;
                case Reason::None: break;
            }
            transaction.rollback();
            auto refused = refuse(std::move(result), code, reason);
            refused.error_details["offending-id"] = native.offending_id;
            refused.message = native.detail;
            return refused;
        }
        document->ensureUpToDate();
        boost::json::array paths, mapping;
        for (auto const &id : native.output_ids) {
            auto *item = cast<SPItem>(document->getObjectById(id));
            auto path = Bool::boolean_operand_path(item);
            if (!path) throw std::runtime_error("Native Boolean output is not a path.");
            auto geometry = *path * item->i2doc_affine();
            paths.emplace_back(boost::json::object{{"id", request.dry_run ? boost::json::value(nullptr) : boost::json::value(id)},
                {"d", sp_svg_write_path(geometry)}, {"bounds", bounds_json(geometry.boundsExact())}});
        }
        for (auto const &id : native.consumed_ids)
            mapping.emplace_back(boost::json::object{{"source", id},
                {"outputs", request.dry_run ? boost::json::array{} : ids_json(native.output_ids)}});
        result.data = {{"variant", request.dry_run ? "computed-dry-run" : "success"}, {"paths", std::move(paths)},
                       {"consumed-ids", ids_json(native.consumed_ids)}, {"source-output", std::move(mapping)}};
        result.eligible = ids.size();
        if (request.dry_run) return result; // scratch transaction rolls back; live state never changed.
        if (context.cancelled && context.cancelled()) {
            transaction.rollback();
            return refuse(initial_record(request, context, services), "cancelled", "cancellation observed before commit");
        }
        for (auto const &id : native.output_ids) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) result.created.push_back(id);
            else result.modified.push_back(id);
        }
        for (auto const &id : native.consumed_ids)
            if (!document->getObjectById(id)) result.deleted.push_back(id);
        result.selection_after = native.output_ids;
        transaction.commit(Util::Internal::ContextString(label), "path-union");
        result.status = Status::Changed; result.one_undo_step = true; result.undo_effect = "one-step";
        result.revision_after = document_stamp(document).revision;
        return result;
    } catch (...) {
        auto failed = initial_record(request, context, services);
        failed.status = Status::Failed; failed.reason = "internal-error";
        failed.error = ParseError{"internal-error", {}, "unexpected service exception; rollback before return"};
        failed.error_details = {{"reason", "unexpected service exception; rollback before return"},
                                {"mutation_state", request.dry_run ? "none" : "rolled-back"}};
        failed.error_retryable = false;
        return failed;
    }
}
Record execute_offset(Request const &request, DispatchContext &context, EditServices &services)
{
    auto result = initial_record(request, context, services);
    result.mode = "per-selected-root-geometry";
    std::vector<SPItem *> raw;
    for (auto const &value : request.params.at("ids").as_array()) {
        auto id = std::string(value.as_string());
        auto *item = cast<SPItem>(services.document.getObjectById(id));
        if (!item || !item->parent) return refuse(std::move(result), "unknown-id", "explicit ID cannot resolve");
        raw.push_back(item);
    }
    auto normalized = Util::resolve_composite_targets(raw,
        [](SPItem *) { return Util::TargetAvailability::Eligible; },
        [](SPItem *item) { return cast<SPItem>(item->parent); },
        [](SPItem *) -> SPItem * { return nullptr; });
    std::vector<SPItem *> available;
    for (auto *item : normalized.items) {
        if (document_available(item)) available.push_back(item);
        else result.excluded.push_back({item->getId(), "unavailable"});
    }
    auto prepared = OffsetShapes::prepare(available);
    std::vector<std::string> sources;
    for (auto const &source : prepared.sources) sources.emplace_back(source.item->getId());
    for (auto *item : available)
        if (std::find(sources.begin(), sources.end(), item->getId()) == sources.end())
            result.excluded.push_back({item->getId(), "no-closed-shapes"});
    result.selected = raw.size(); result.eligible = sources.size(); result.covered = normalized.covered;
    if (!result.excluded.empty()) result.warnings.emplace_back("exclusions");
    if (!prepared) return refuse(std::move(result), "no-closed-shapes", "prepare: no closed compatible sources");
    OffsetShapes::Options options;
    auto const &params = request.params;
    options.distance_px = css_px(params.at("distance"));
    auto direction = params.at("direction").as_string();
    options.direction = direction == "both" ? OffsetShapes::Direction::Both : direction == "inward"
        ? OffsetShapes::Direction::Inward : OffsetShapes::Direction::Outward;
    auto corner = params.at("corner").as_string();
    options.corner = corner == "round" ? OffsetShapes::Corner::Round : corner == "bevel"
        ? OffsetShapes::Corner::Bevel : OffsetShapes::Corner::Miter;
    options.miter_limit = params.at("miter-limit").to_number<double>();
    options.outer_shapes_only = params.at("outer-only").as_bool();
    options.delete_originals = params.at("delete-originals").as_bool();
    options.select_results = params.at("select-results").as_bool();
    options.simplify_results = params.at("simplify").as_bool();
    options.simplify_tolerance_px = css_px(params.at("simplify-tolerance"));
    // Do not use the legacy CLI tolerance clamp. Native build reports its
    // bounded simplification fallback, preserving the generated geometry.
    auto built = OffsetShapes::build(prepared.sources, options);
    if (!built) return refuse(std::move(result), "offset-failed", "build: failed geometry");
    if (built.simplify_skipped) result.warnings.emplace_back("simplify-skipped");
    // Match commit's read-only parent-domain admission in computed previews.
    SPObject *common_parent = nullptr;
    for (auto const &source : prepared.sources) {
        auto *item = source.item.get();
        auto *parent = item ? item->parent : nullptr;
        auto *parent_item = cast<SPItem>(parent);
        if (!parent || (parent_item && std::abs(parent_item->i2doc_affine().det()) < 1e-12))
            return refuse(std::move(result), "offset-failed", "commit: invalid parent transform");
        if (options.outer_shapes_only && common_parent && parent != common_parent)
            return refuse(std::move(result), "offset-failed", "commit: outer-only needs a shared parent");
        common_parent = parent;
    }
    if (context.cancelled && context.cancelled())
        return refuse(std::move(result), "cancelled", "cancellation observed before commit");
    if (cli_fault("geometry.offset.before-dependency-check", CliFaultKind::StaleDependency) ||
        !OffsetShapes::geometry_still_matches(prepared.sources))
        return refuse(std::move(result), "stale-dependency", "commit: geometry_still_matches false");
    boost::json::array paths, mapping;
    auto data = [&](std::vector<std::string> const &outputs) {
        paths.clear(); mapping.clear();
        for (std::size_t i = 0; i < built.results.size(); ++i) {
            auto const &geometry = built.results[i].geometry_document;
            paths.emplace_back(boost::json::object{
                {"id", request.dry_run ? boost::json::value(nullptr) : boost::json::value(outputs.at(i))},
                {"d", std::string(sp_svg_write_path(geometry))}, {"bounds", bounds_json(geometry.boundsExact())}});
        }
        for (std::size_t i = 0; i < sources.size(); ++i) {
            boost::json::array output_ids;
            if (!request.dry_run)
                for (std::size_t j = 0; j < built.results.size(); ++j)
                    if (options.outer_shapes_only || built.results[j].source_index == i) output_ids.emplace_back(outputs.at(j));
            mapping.emplace_back(boost::json::object{{"source", sources[i]}, {"outputs", std::move(output_ids)}});
        }
        result.data = {{"variant", request.dry_run ? "computed-dry-run" : "success"}, {"paths", paths},
                       {"consumed-ids", ids_json(result.deleted)}, {"source-output", mapping}};
    };
    if (request.dry_run) { data({}); return result; }
    try {
        EditTransaction transaction(&services.document, &services.selection, services.operation_lease);
        if (!transaction.active()) return refuse(std::move(result), "transaction-unavailable", "EditTransaction inactive");
        auto committed = OffsetShapes::commit(&services.document, prepared, built, options,
                                              OffsetShapes::CommitProtocol::CallerOwnedAtomic);
        // Native publication has run inside the caller transaction. A test fault
        // changes only its failure signal; the existing caller path rolls back
        // the real writes before returning a Record.
        if (committed && cli_fault("geometry.offset.after-native-commit", CliFaultKind::CommitRefusal))
            committed.error = "injected native commit refusal after publication";
        if (!committed) {
            transaction.rollback();
            auto failed = refuse(std::move(result), "offset-failed", "commit failure; caller rollback");
            failed.status = committed.mutation_started ? Status::Failed : Status::Rejected;
            failed.error_details["mutation_state"] = committed.mutation_started ? "rolled-back" : "none";
            return failed;
        }
        for (auto *item : committed.created) result.created.emplace_back(item->getId());
        result.deleted = committed.deleted_ids;
        data(result.created); // Allocate the required result before settlement.
        if (context.cancelled && context.cancelled()) {
            transaction.rollback();
            auto cancelled = initial_record(request, context, services);
            return refuse(std::move(cancelled), "cancelled", "cancellation observed before commit");
        }
        if (options.select_results) result.selection_after = result.created;
        else result.selection_after = selected_ids(services.selection);
        transaction.commit(Util::Internal::ContextString("Offset shapes"), "path-offset-dynamic");
        result.status = Status::Changed; result.one_undo_step = true; result.undo_effect = "one-step";
        result.revision_after = document_stamp(&services.document).revision;
        // Integration installs result.selection_after only after successful settlement.
        return result;
    } catch (...) {
        auto failed = initial_record(request, context, services);
        failed.status = Status::Failed; failed.reason = "internal-error";
        failed.error = ParseError{"internal-error", {}, "unexpected service exception; rollback before return"};
        failed.error_details = {{"reason", "unexpected service exception; rollback before return"}, {"mutation_state", "rolled-back"}};
        failed.error_retryable = false;
        return failed;
    }
}
} // namespace
Record execute_geometry(Request const &r, DispatchContext &c, EditServices &s)
{
    if (r.command == "geometry.boolean") return execute_boolean(r, c, s);
    if (r.command == "geometry.offset") return execute_offset(r, c, s);
    if (r.command == "geometry.corners") return execute_corners(r, c, s);
    if (r.command == "geometry.move" || r.command == "geometry.resize" || r.command == "geometry.rotate" ||
        r.command == "geometry.skew" || r.command == "geometry.flip" || r.command == "geometry.matrix")
        return execute_transform(r, c, s);
    return production_unavailable(r);
}
}
