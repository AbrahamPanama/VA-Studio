// SPDX-License-Identifier: GPL-2.0-or-later
#include "corner-rounding-controller.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <glibmm/i18n.h>
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "live_effects/lpe-fillet-chamfer.h"
#include "live_effects/lpeobject.h"
#include "live_effects/parameter/array.h"
#include "object/sp-defs.h"
#include "object/sp-path.h"
#include "object/sp-polygon.h"
#include "object/sp-polyline.h"
#include "message-stack.h"
#include "object/weakptr.h"
#include "selection.h"
#include "svg/svg.h"
#include "ui/tools/node-tool.h"
#include "xml/document.h"
#include "xml/attribute-record.h"
#include "xml/repr.h"

namespace Inkscape::UI::Tools {
namespace CE = LivePathEffect::CornerEdit;
std::string corner_conversion_from(SPShape const &target)
{
    if (is<SPPolygon>(&target)) return "polygon";
    if (is<SPPolyLine>(&target)) return "polyline";
    return {};
}
namespace {
using Attributes = std::vector<std::pair<std::string, std::string>>;
Attributes attributes(XML::Node const *repr)
{
    Attributes result;
    for (auto const &a : repr->attributeList()) {
        result.emplace_back(g_quark_to_string(a.key), static_cast<char const *>(a.value));
    }
    return result;
}
bool finite(Geom::PathVector const *path)
{
    if (!path || path->empty()) return false;
    for (auto const &sub : *path) for (auto const &curve : sub) {
        auto b = curve.boundsFast();
        for (int d = 0; d < 2; ++d) {
            if (!std::isfinite(b.min()[d]) || !std::isfinite(b.max()[d])) return false;
        }
    }
    return true;
}
std::string serialize(NodeSatellites const &satellites)
{
    // Existing array serializer, never a second persistence format.
    LivePathEffect::ArrayParam<std::vector<NodeSatellite>> parameter("", "", "", nullptr, nullptr);
    parameter.param_setValue(satellites);
    return parameter.param_getSVGValue().raw();
}
}
struct CornerRoundingController::State {
    SPDesktop *desktop;
    bool alive = true, applying = false;
    std::uint64_t generation = 1;
    SPWeakPtr<SPShape> target;
    std::optional<CE::Address> clicked_corner;
    SPWeakPtr<LivePathEffectObject> effect;
    std::optional<CE::Snapshot> baseline;
    Attributes path_attributes, effect_attributes;
    Geom::Affine transform;
    sigc::signal<void()> invalidated;
    std::vector<sigc::scoped_connection> connections;
    explicit State(SPDesktop &d) : desktop(&d) {}
    void invalidate() {
        if (applying) return; // Own native updates may emit selection notifications.
        baseline.reset(); target.reset(); effect.reset();
        if (generation != std::numeric_limits<std::uint64_t>::max()) ++generation;
        auto signal = invalidated; signal.emit();
    }
    bool current() const {
        return alive && desktop && dynamic_cast<NodeTool *>(desktop->getTool()) &&
               desktop->getSelection()->singleItem() == target.get() && target &&
               target->document == desktop->getDocument() &&
               !DocumentUndo::interactionCloseRequested(target->document);
    }
};

CornerRoundingController::CornerRoundingController(SPDesktop &desktop)
    : _state(std::make_shared<State>(desktop))
{
    auto weak = std::weak_ptr(_state);
    auto invalidate = [weak](auto...) { if (auto s = weak.lock()) s->invalidate(); };
    _state->connections.emplace_back(desktop.getSelection()->connectChanged(invalidate));
    _state->connections.emplace_back(desktop.getSelection()->connectModified(invalidate));
    _state->connections.emplace_back(desktop.connect_control_point_selected(invalidate));
    _state->connections.emplace_back(desktop.connectEventContextChanged(invalidate));
    _state->connections.emplace_back(desktop.connectDocumentReplaced(invalidate));
    _state->connections.emplace_back(desktop.connectDestroy([weak](SPDesktop *) {
        if (auto s = weak.lock()) { s->desktop = nullptr; s->invalidate(); }
    }));
}
CornerRoundingController::~CornerRoundingController()
{
    _state->alive = false;
    _state->connections.clear();
}
sigc::connection CornerRoundingController::connectInvalidated(sigc::slot<void()> slot)
{
    return _state->invalidated.connect(std::move(slot));
}
void CornerRoundingController::invalidate() { auto s = _state; s->invalidate(); }

CornerRoundingController::View CornerRoundingController::inspect(CE::Scope scope, std::optional<CE::Address> clicked_corner)
{
    auto s = _state;
    View view;
    if (s->applying || !s->alive || !s->desktop) return view;
    s->baseline.reset();
    if (s->generation == std::numeric_limits<std::uint64_t>::max()) return view;
    view.generation = ++s->generation;
    auto tool = dynamic_cast<NodeTool *>(s->desktop->getTool());
    auto path = cast<SPShape>(s->desktop->getSelection()->singleItem());
    if (!tool || !path) {
        view.context.reason = _("Select one vector shape. Ungroup artwork first; convert text to paths explicitly.");
        return view;
    }
    view.context = capture_corner_rounding(*path, *tool->_selected_nodes);
    if (!view.context.snapshot) return view;
    s->clicked_corner = clicked_corner;
    if (clicked_corner) view.context.snapshot->selected = {*clicked_corner};
    s->target = path;
    s->effect = path->getFirstPathEffectOfType(LivePathEffect::FILLET_CHAMFER)
        ? path->getFirstPathEffectOfType(LivePathEffect::FILLET_CHAMFER)->getLPEObj() : nullptr;
    s->baseline = view.context.snapshot;
    s->path_attributes = attributes(path->getRepr());
    s->effect_attributes = s->effect ? attributes(s->effect->getRepr()) : Attributes{};
    s->transform = path->i2doc_affine();
    view.summary = CE::inspect(*s->baseline, scope);
    return view;
}

CornerRoundingController::Result CornerRoundingController::apply(Request const &request, std::uint64_t generation)
{
    auto s = _state; // Pin before any callback; never use this after publication.
    auto reject = [](std::string reason) { return Result{Outcome::Rejected, std::move(reason)}; };
    if (s->applying || generation != s->generation || !s->baseline || !s->current())
        return reject(_("The corner selection changed. Reopen the controls."));
    // Existing native knot reloads use the active desktop. Do not allow those
    // to reach another window; do not change the application's active desktop.
    if (s->desktop != SP_ACTIVE_DESKTOP)
        return reject(_("Activate this document before applying corner changes."));
    auto tool = dynamic_cast<NodeTool *>(s->desktop->getTool());
    auto fresh = capture_corner_rounding(*s->target, *tool->_selected_nodes);
    if (fresh.snapshot && s->clicked_corner) fresh.snapshot->selected = {*s->clicked_corner};
    if (!fresh.snapshot || fresh.snapshot->path != s->baseline->path ||
        fresh.snapshot->selected != s->baseline->selected || fresh.snapshot->smooth != s->baseline->smooth ||
        !CE::equal(fresh.snapshot->satellites, s->baseline->satellites) ||
        attributes(s->target->getRepr()) != s->path_attributes || s->target->i2doc_affine() != s->transform ||
        (s->effect && attributes(s->effect->getRepr()) != s->effect_attributes))
        return reject(_("The path or effect changed. Reopen the controls."));

    // The commit is shared with the command line. This controller keeps the
    // live-tool bookkeeping: `Applying` is armed inside before_mutation, right
    // before apply_corner_plan performs its first document mutation, and it is
    // cleared after apply_corner_plan returns -- the same lifetime the pre-shared
    // implementation gave its local guard.
    struct Applying {
        std::shared_ptr<State> s;
        ~Applying() {
            s->applying = false;
            s->baseline.reset();
            if (s->generation != std::numeric_limits<std::uint64_t>::max()) ++s->generation;
        }
    };
    std::optional<Applying> applying;
    auto arm = [&] {
        s->applying = true;
        applying.emplace();
        applying->s = s;
    };
    auto result = apply_corner_plan(*s->target, s->effect.get(), *fresh.snapshot, request,
                                    CornerCommitProtocol::Interaction,
                                    [&] { return s->current() && s->desktop == SP_ACTIVE_DESKTOP; }, arm,
                                    [&](SPShape &replacement) {
                                        if (!s->alive || !s->desktop || s->desktop->getDocument() != replacement.document)
                                            throw std::runtime_error("invalidated");
                                        s->target = &replacement;
                                        s->desktop->getSelection()->set(&replacement);
                                    });
    // Only a recorded edit changes the effect; refusal/Superseded keep the
    // weak reference as it is (the object may have gone meanwhile).
    if (result.outcome == Outcome::Applied) {
        s->effect = result.effect;
        if (!result.converted_from.empty() && s->alive && s->desktop) {
            s->desktop->messageStack()->flash(Inkscape::INFORMATION_MESSAGE,
                result.converted_from == "polygon"
                ? _("Converted polygon to path to round its corners")
                : _("Converted polyline to path to round its corners"));
        }
    }
    return {result.outcome, std::move(result.reason)};
}

std::optional<std::string> command_line_corner_refusal(SPDocument &document)
{
    if (DocumentUndo::interactionCloseRequested(&document)) return std::string(_("The document is closing; no change was made."));
    if (DocumentUndo::interactionActive(&document))
        return std::string(_("Finish the current document operation before editing corners."));
    if (!DocumentUndo::getUndoSensitive(&document))
        return std::string(_("Undo recording is off for this document; no change was made."));
    return std::nullopt;
}

CornerPlanCheck check_corner_plan(bool has_effect, CE::Snapshot const &fresh, CE::Request const &request)
{
    using Outcome = CornerRoundingController::Outcome;
    CornerPlanCheck check;
    auto finish = [&](Outcome outcome, std::string reason) {
        check.outcome = outcome;
        check.reason = std::move(reason);
        return check;
    };
    try { check.plan = CE::prepare(fresh, request); }
    catch (...) { return finish(Outcome::Rejected, _("The native corner calculation failed; no change was made.")); }
    auto &plan = check.plan;
    if (plan.status == CE::Status::NoChange) return finish(Outcome::NoChange, {});
    if (plan.status != CE::Status::Ready)
        return finish(Outcome::Rejected, plan.status == CE::Status::NoCorners
            ? _("Select eligible corners or explicitly choose All corners.")
            : _("The radius is invalid or exceeds the native corner limit. No change was made."));
    // A zero-radius/mode-only request must not install an invisible fresh LPE.
    if (!has_effect && (!request.radius || *request.radius == 0)) return finish(Outcome::NoChange, {});
    // Native SVG precision is the persistence boundary. Compare against its
    // round trip, not an unpersistable extra floating-point bit from radToLen.
    LivePathEffect::ArrayParam<std::vector<NodeSatellite>> normalized("", "", "", nullptr, nullptr);
    auto encoded = serialize(plan.satellites);
    normalized.param_readSVGValue(encoded.c_str());
    plan.satellites = normalized.data();
    if (CE::equal(plan.satellites, fresh.satellites)) return finish(Outcome::NoChange, {});
    check.ready = true;
    return check;
}

CornerApplyResult apply_corner_plan(SPShape &target, LivePathEffectObject *effect, CE::Snapshot const &fresh,
                                    CE::Request const &request, CornerCommitProtocol protocol,
                                    std::function<bool()> const &still_current,
                                    std::function<void()> const &before_mutation,
                                    std::function<void(SPShape &)> const &target_replaced)
{
    using Outcome = CornerRoundingController::Outcome;
    CornerApplyResult result;
    result.effect = effect; // The existing effect; replaced on a successful fresh install.
    auto finish = [&](Outcome outcome, std::string reason) {
        result.outcome = outcome;
        result.reason = std::move(reason);
        return result;
    };
    auto check = check_corner_plan(effect != nullptr, fresh, request);
    result.count = check.plan.count;
    result.approximate = check.plan.approximate;
    if (!check.ready) return finish(check.outcome, std::move(check.reason));
    auto const &plan = check.plan;

    auto document = target.document;
    auto converted_from = corner_conversion_from(target);
    SPShape *working = &target;
    std::optional<DocumentUndo::RollbackableInteraction> interaction;
    std::shared_ptr<void> operation;
    if (protocol == CornerCommitProtocol::Interaction) {
        // begin must precede the operation lease: admission refuses operation_depth.
        interaction = DocumentUndo::beginRollbackableInteraction(document);
        if (!interaction) return finish(Outcome::Rejected, _("Finish the current document operation before editing corners."));
        operation = DocumentUndo::holdInteractionOperation(document);
    } else if (protocol == CornerCommitProtocol::CommandLine) {
        if (auto refusal = command_line_corner_refusal(*document)) {
            result.refused_by_document = true;
            return finish(Outcome::Rejected, std::move(*refusal));
        }
    }
    if (protocol == CornerCommitProtocol::CallerOwnedAtomic &&
        (!DocumentUndo::interactionActive(document) || DocumentUndo::interactionCloseRequested(document))) {
        result.refused_by_document = true;
        return finish(Outcome::Rejected, "The caller must own an active atomic document interaction.");
    }
    if (before_mutation) before_mutation();
    result.mutation_started = true;
    auto current = [&] { return (!interaction || interaction->active()) && still_current(); };
    LivePathEffectObject *applied_effect = effect;
    try {
        {
            XML::Document::MutationScope mutation(*document->getReprDoc());
            if (!converted_from.empty()) {
                if (!current()) throw std::runtime_error("invalidated");
                // Use the admitted shape's own straight-segment input. The generic
                // Object to Path writer normalizes style and copies only selected
                // properties; this narrow replacement must retain every attribute
                // and child (including title/desc), parent and sibling position.
                auto old = working->getRepr();
                auto parent = old->parent();
                auto position = old->position();
                auto repr = document->getReprDoc()->createElement("svg:path");
                struct ReleasePath { XML::Node *n; ~ReleasePath() { GC::release(n); } } release{repr};
                for (auto const &[name, value] : attributes(old)) repr->setAttribute(name, value);
                for (auto child = old->firstChild(); child; child = child->next()) {
                    auto copy = child->duplicate(document->getReprDoc());
                    repr->appendChild(copy);
                    GC::release(copy);
                }
                repr->setAttribute("d", sp_svg_write_path(fresh.path));
                // Resurrect the same id without deleting clones/references.
                sp_repr_unparent(old);
                parent->addChildAtPos(repr, position);
                working = cast<SPPath>(document->getObjectByRepr(repr));
                if (!working) throw std::runtime_error("missing converted path");
                if (target_replaced) target_replaced(*working);
            }
            if (!applied_effect) {
                // Attach an explicit SVG LPE definition, just as a saved SVG
                // loads it. Never run resetDefaults/doOnApply.
                auto repr = document->getReprDoc()->createElement("inkscape:path-effect");
                struct Release { XML::Node *n; ~Release() { GC::release(n); } } release{repr};
                for (auto [name, value] : {
                    std::pair{"effect", "fillet_chamfer"}, {"lpeversion", "1"}, {"is_visible", "true"},
                    {"method", "auto"}, {"flexible", "false"}, {"radius", "0"}, {"unit", "px"},
                    {"mode", "F"}, {"use_knot_distance", "false"}, {"only_selected", "false"},
                    {"hide_knots", "false"}, {"chamfer_steps", "1"}, {"chamfer_scale", "100"}})
                    repr->setAttribute(name, value);
                repr->setAttribute("nodesatellites_param", serialize(plan.satellites));
                document->getDefs()->getRepr()->appendChild(repr);
                if (!current()) throw std::runtime_error("invalidated");
                applied_effect = cast<LivePathEffectObject>(document->getObjectByRepr(repr));
                if (!applied_effect || !repr->attribute("id")) throw std::runtime_error("missing effect");
                auto target_repr = working->getRepr();
                auto reference = std::string("#") + repr->attribute("id");
                if (is<SPPath>(working)) {
                    auto original = target_repr->attribute("d");
                    if (!original) throw std::runtime_error("missing input");
                    target_repr->setAttributesAtomically({
                        {"inkscape:original-d", std::string(original)}, {"inkscape:path-effect", reference}});
                } else {
                    target_repr->setAttribute("inkscape:path-effect", reference);
                }
            } else {
                auto lpe = dynamic_cast<LivePathEffect::LPEFilletChamfer *>(applied_effect->get_lpe());
                if (!lpe) throw std::runtime_error("missing effect");
                lpe->nodesatellites_param.param_set_and_write_new_value(plan.satellites);
            }
            if (!current() || !applied_effect) throw std::runtime_error("invalidated");
            // Previously ineffective polygon LPEs can already exist in files.
            if (!converted_from.empty())
                working->getRepr()->setAttribute("inkscape:original-d", sp_svg_write_path(fresh.path));
            sp_lpe_item_update_patheffect(working, false, true);
        }
        if (!current() || !finite(working->curve())) throw std::runtime_error("invalid output");
        auto lpe = dynamic_cast<LivePathEffect::LPEFilletChamfer *>(applied_effect->get_lpe());
        if (!lpe || lpe->has_exception || !CE::equal(lpe->nodesatellites_param.data(), plan.satellites))
            throw std::runtime_error("native parameters changed");
        if (interaction) {
            interaction->commit(Util::Internal::ContextString{_("Round corners")}, "fillet-chamfer");
        } else if (protocol == CornerCommitProtocol::CommandLine) {
            DocumentUndo::done(document, Util::Internal::ContextString{_("Round corners")}, "fillet-chamfer");
            document->ensureUpToDate();
        }
        // Publish the (possibly freshly created) effect only after the edit is
        // recorded, so a rolled-back creation never escapes as the result effect
        // (a destroyed object would leave the controller's weak pointer dangling).
        result.effect = applied_effect;
        result.converted_from = std::move(converted_from);
        return finish(Outcome::Applied, {});
    } catch (...) {
        if (interaction) {
            if (DocumentUndo::interactionCloseRequested(document))
                return finish(Outcome::Superseded, _("Document closure superseded the edit; no later rollback was attempted."));
            if (!interaction->active())
                return finish(Outcome::Superseded, _("Another document action ended the edit; no further changes were made."));
            interaction->rollback();
            return finish(Outcome::Rejected, _("The corner edit could not finish and was cancelled."));
        }
        // No interaction token: the CommandLine protocol owns the single-step
        // Undo, so a failure after the first mutation reverts it here.
        if (protocol == CornerCommitProtocol::CommandLine) DocumentUndo::cancel(document);
        return finish(Outcome::Rejected, _("The corner edit could not finish and was cancelled."));
    }
}
} // namespace Inkscape::UI::Tools
