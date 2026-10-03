// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Shared stroke-width command (internal note STROKE_WIDTH_CONTROLS_PLAN §3).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/stroke-width-command.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <glibmm/i18n.h>
#include <sigc++/scoped_connection.h>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "libnrtype/Layout-TNG.h"
#include "message-stack.h"
#include "object/sp-item.h"
#include "object/weakptr.h"
#include "preferences.h"
#include "selection.h"
#include "text-editing.h"
#include "ui/tools/text-tool.h"
#include "util/units.h"

namespace Inkscape::UI {

namespace {

/// Concise status text for a rejected controller plan.
Glib::ustring rejection_message(StrokeWidthMemberReason reason)
{
    switch (reason) {
    case StrokeWidthMemberReason::UnsupportedIntent:
        return _("Stroke width: this selection or text run is not supported; nothing was applied");
    case StrokeWidthMemberReason::InvalidIntent:
        return _("Stroke width: the entered value cannot be applied");
    case StrokeWidthMemberReason::StalePlan:
        return _("Stroke width: the selection or style changed; nothing was applied");
    case StrokeWidthMemberReason::InvalidTransaction:
        return _("Stroke width: another edit is in progress; nothing was applied");
    case StrokeWidthMemberReason::DependentCloneConflict:
    case StrokeWidthMemberReason::DependentCloneUncertain:
        return _("Stroke width: the selection contains a linked clone that cannot be changed independently");
    case StrokeWidthMemberReason::PostconditionMismatch:
        return _("Stroke width: the change could not be verified; nothing was applied");
    case StrokeWidthMemberReason::HairlinePending:
    case StrokeWidthMemberReason::TextAdapterPending:
    case StrokeWidthMemberReason::CloneAdapterPending:
    case StrokeWidthMemberReason::InvalidDash:
    case StrokeWidthMemberReason::StylePriority:
        return _("Stroke width: this stroke style is not supported by the writer; nothing was applied");
    case StrokeWidthMemberReason::None:
    default:
        return _("Stroke width: change not applied");
    }
}

/// The text tool's live text state as plain numbers: the owner and the raw caret or
/// range indices in the tool's own direction (a collapsed caret included). Read
/// from the tool's iterators, but compared as numbers, never as iterators.
struct TextState
{
    SPItem *owner = nullptr;
    bool has_layout = false;
    int first = 0;
    int last = 0;
    bool operator==(TextState const &) const = default;
};

/// The scope a command acts on. Compared as a whole: every field is identity.
struct Scope
{
    SPDocument *document = nullptr;
    std::uint64_t generation = 0;
    std::vector<SPWeakPtr<SPItem>> roots;
    Util::Unit const *unit = nullptr;
    Tools::ToolBase *tool = nullptr;
    TextState text_state;
    /// The explicit non-collapsed range the plan is prepared for (nullopt: none).
    std::optional<StrokeWidthTextRange> text;

    bool roots_alive() const
    {
        return std::all_of(roots.begin(), roots.end(), [](auto const &root) { return root.get() != nullptr; });
    }
};

std::uint64_t live(StrokeWidthCommandRequest const &request)
{
    return request.live_generation ? *request.live_generation : 0;
}

TextState capture_text_state(SPDesktop &desktop)
{
    TextState state;
    auto *text_tool = dynamic_cast<Tools::TextTool *>(desktop.getTool());
    if (!text_tool) return state;
    state.owner = text_tool->textItem();
    auto const *layout = state.owner ? te_get_layout(state.owner) : nullptr;
    if (layout) {
        state.has_layout = true;
        state.first = layout->iteratorToCharIndex(text_tool->text_sel_start);
        state.last = layout->iteratorToCharIndex(text_tool->text_sel_end);
    }
    return state;
}

/// Everything but the roots and the range: cheap enough to run before every write.
bool same_live_state(Scope const &scope, SPDesktop &desktop, StrokeWidthCommandRequest const &request)
{
    return desktop.getDocument() == scope.document && live(request) == scope.generation &&
           request.unit == scope.unit && desktop.getTool() == scope.tool &&
           capture_text_state(desktop) == scope.text_state;
}

bool composing(SPDesktop &desktop)
{
    auto *text_tool = dynamic_cast<Tools::TextTool *>(desktop.getTool());
    return text_tool && text_tool->isComposing();
}

Scope capture_scope(SPDesktop &desktop, StrokeWidthCommandRequest const &request)
{
    Scope scope;
    scope.document = desktop.getDocument();
    scope.generation = live(request);
    scope.unit = request.unit;
    scope.tool = desktop.getTool();
    if (auto *selection = desktop.getSelection()) {
        for (auto *item : selection->items_vector()) scope.roots.emplace_back(item);
    }
    scope.text_state = capture_text_state(desktop);
    scope.text = stroke_width_current_text_range(&desktop);
    return scope;
}

bool same_scope(Scope const &a, Scope const &b)
{
    if (a.document != b.document || a.generation != b.generation || a.unit != b.unit || a.tool != b.tool ||
        !(a.text_state == b.text_state) || a.roots.size() != b.roots.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.roots.size(); ++i) {
        if (!a.roots[i].get() || a.roots[i].get() != b.roots[i].get()) return false;
    }
    if (a.text.has_value() != b.text.has_value()) return false;
    if (!a.text) return true;
    return a.text->owner.get() == b.text->owner.get() && a.text->caret == b.text->caret &&
           a.text->first_char == b.text->first_char && a.text->last_char == b.text->last_char;
}

StrokeWidthPlan prepare(SPDocument &document, Scope const &scope, StrokeWidthIntent const &intent)
{
    std::vector<SPItem *> roots;
    for (auto const &root : scope.roots) roots.push_back(root.get());
    return scope.text ? prepare_stroke_widths_combined(document, *scope.text, roots, intent, scope.generation)
                      : prepare_stroke_widths(document, roots, intent, scope.generation);
}

/// The predicate `begin_stroke_width_interaction` uses to decide whether a plan changes anything.
bool has_change(StrokeWidthPlan const &plan)
{
    return plan.planned_changes > 0 ||
           std::any_of(plan.members.begin(), plan.members.end(), [](auto const &member) {
               return std::any_of(member.text_runs.begin(), member.text_runs.end(), [](auto const &run) {
                   return run.outcome == StrokeWidthMemberOutcome::Change;
               });
           });
}

} // namespace

std::optional<StrokeWidthTextRange> stroke_width_current_text_range(SPDesktop *desktop)
{
    auto *text_tool = desktop ? dynamic_cast<Tools::TextTool *>(desktop->getTool()) : nullptr;
    if (!text_tool) return std::nullopt;
    auto *owner = text_tool->textItem();
    auto const *layout = owner ? te_get_layout(owner) : nullptr;
    if (!owner || !layout || text_tool->text_sel_start == text_tool->text_sel_end) return std::nullopt;
    StrokeWidthTextRange range;
    range.owner = owner;
    range.caret = false;
    range.first_char = static_cast<unsigned>(layout->iteratorToCharIndex(text_tool->text_sel_start));
    range.last_char = static_cast<unsigned>(layout->iteratorToCharIndex(text_tool->text_sel_end));
    return range;
}

StrokeWidthCommandOutcome apply_stroke_widths_command(SPDesktop &desktop, StrokeWidthCommandRequest const &request)
{
    StrokeWidthCommandOutcome out;

    auto *document = desktop.getDocument();
    if (!document || !desktop.getSelection()) {
        out.state = StrokeWidthCommandState::Refused;
        return out;
    }

    // The desktop can go away and the document can be replaced or destroyed while a
    // callback-bearing call (a refresh, a settlement, a write observer) runs. Every
    // such call is followed by a check of `scope_dead` BEFORE anything is
    // dereferenced or reported.
    bool scope_dead = false;
    sigc::scoped_connection const on_destroy = desktop.connectDestroy([&](auto &&...) { scope_dead = true; });
    sigc::scoped_connection const on_replace = desktop.connectDocumentReplaced([&](auto &&...) { scope_dead = true; });
    sigc::scoped_connection const on_document_gone = document->connectDestroy([&] { scope_dead = true; });
    bool selection_changed = false;
    sigc::scoped_connection const on_selection =
        desktop.getSelection()->connectChanged([&](Selection *) { selection_changed = true; });

    // Only reports while the desktop is provably alive.
    auto const finish = [&](StrokeWidthCommandState state, std::string const &note = {}) {
        out.state = state;
        if (scope_dead) return out; // quiet: nothing may be dereferenced
        out.note = note;
        if (!note.empty() && desktop.messageStack()) desktop.messageStack()->flash(Inkscape::WARNING_MESSAGE, note);
        return out;
    };
    auto const stale = [&] {
        out.reason = StrokeWidthMemberReason::StalePlan;
        return finish(StrokeWidthCommandState::Refused, rejection_message(StrokeWidthMemberReason::StalePlan).raw());
    };
    auto const composing_refusal = [&] {
        return finish(StrokeWidthCommandState::Refused,
                      _("Stroke width: finish the text composition first; nothing was applied"));
    };

    // A text-tool composition owns the caret until it commits: never write under it.
    if (composing(desktop)) return composing_refusal();

    StrokeWidthIntent intent = request.intent;
    intent.scale_dashes = Preferences::get()->getBool("/options/dash/scale", true);

    // 1. Capture FIRST, then refresh: a selection change during the refresh is a
    //    refusal, never the new target.
    Scope const scope0 = capture_scope(desktop, request);
    document->ensureUpToDate();
    if (scope_dead) return finish(StrokeWidthCommandState::Refused);
    if (composing(desktop)) return composing_refusal();
    Scope const scope1 = capture_scope(desktop, request);
    if (selection_changed || !scope1.roots_alive() || !same_scope(scope0, scope1)) return stale();
    StrokeWidthPlan const plan1 = prepare(*document, scope1, intent);

    if (plan1.state != StrokeWidthPlanState::Prepared) {
        out.reason = plan1.rejection;
        return finish(StrokeWidthCommandState::Rejected,
                      (plan1.query.range_rejected
                           ? _("Stroke width: the text range is stale or out of bounds; nothing was applied")
                           : rejection_message(plan1.rejection)).raw());
    }
    if (plan1.query.eligible == 0) {
        return finish(StrokeWidthCommandState::Unchanged,
                      _("Stroke width: no eligible shape or text in the selection"));
    }

    // 2. Nothing to change: no settlement, no token, no write. Redo survives.
    if (!has_change(plan1)) {
        StrokeWidthApplyResult none;
        none.state = StrokeWidthApplyState::Unchanged;
        none.excluded = plan1.excluded;
        none.skipped_runs = plan1.skipped_runs;
        out.excluded = plan1.excluded;
        return finish(StrokeWidthCommandState::Unchanged, stroke_width_applied_note(plan1, none));
    }

    // 3. A change is planned: settle unrelated pending automatic updates as their
    //    own step, then capture and prepare again and refuse on any difference.
    if (auto probe = begin_stroke_width_interaction(*document, plan1, [&] { return !scope_dead; })) probe->rollback();
    if (scope_dead) return finish(StrokeWidthCommandState::Refused);
    document->ensureUpToDate();
    if (scope_dead) return finish(StrokeWidthCommandState::Refused);
    if (composing(desktop)) return composing_refusal();
    Scope const scope2 = capture_scope(desktop, request);
    if (selection_changed || !scope2.roots_alive() || !same_scope(scope1, scope2)) return stale();
    StrokeWidthPlan const plan2 = prepare(*document, scope2, intent);
    // The controller's own strict comparison: every write patch, not only outcomes.
    if (!stroke_width_plans_match(plan1, plan2)) return stale();

    // 4. Begin the atomic interaction.
    auto token = begin_stroke_width_interaction(*document, plan2, [&] { return !scope_dead; });
    if (scope_dead) {
        if (token) token->rollback();
        return finish(StrokeWidthCommandState::Refused);
    }
    if (!token) {
        out.reason = StrokeWidthMemberReason::InvalidTransaction;
        return finish(StrokeWidthCommandState::Refused,
                      rejection_message(StrokeWidthMemberReason::InvalidTransaction).raw());
    }

    // 5. Apply. The live-scope predicate is checked by the controller before the
    //    first write and around every later one, so a write observer that changes
    //    the selection, unit, tool or text range stops the remaining writes.
    auto const scope_live = [&]() -> bool {
        return !scope_dead && !selection_changed && !composing(desktop) && same_live_state(scope2, desktop, request);
    };
    bool const text_path = plan2.text_scope.has_value() && !plan2.combined_text_scope;
    StrokeWidthApplyResult const result = text_path
        ? apply_stroke_widths_text(*document, plan2, live(request), *token, scope_live)
        : apply_stroke_widths_compatible(*document, plan2, live(request), *token, scope_live);
    if (scope_dead) {
        token->rollback();
        return finish(StrokeWidthCommandState::Refused);
    }
    out.excluded = result.excluded;
    out.following_clones = plan2.following_clones;

    // 6. Commit with a readiness check over the FULL captured scope.
    switch (result.state) {
    case StrokeWidthApplyState::Applied: {
        bool const committed = token->commitAtomically(request.undo_label, request.icon, [&] {
            if (scope_dead || selection_changed || composing(desktop)) return false;
            if (!same_scope(scope2, capture_scope(desktop, request))) return false;
            return text_path ? stroke_widths_text_output_ready(*document, plan2, live(request), *token)
                             : stroke_widths_compatible_output_ready(*document, plan2, live(request), *token);
        });
        if (scope_dead) {
            if (!committed) token->rollback();
            return finish(committed ? StrokeWidthCommandState::Applied : StrokeWidthCommandState::Refused);
        }
        if (!committed) {
            token->rollback();
            return finish(StrokeWidthCommandState::Failed,
                          _("Stroke width: the change could not be confirmed and was rolled back"));
        }
        out.committed = true;
        out.changed = result.changed;
        // A success with nothing to report is silent.
        return finish(StrokeWidthCommandState::Applied, stroke_width_applied_note(plan2, result));
    }
    case StrokeWidthApplyState::Unchanged:
        // Legitimate no-op found late: no history, no default, no write.
        token->rollback();
        return finish(StrokeWidthCommandState::Unchanged, stroke_width_applied_note(plan2, result));
    case StrokeWidthApplyState::Rejected:
        token->rollback();
        out.reason = result.reason;
        return finish(StrokeWidthCommandState::Rejected, rejection_message(result.reason).raw());
    case StrokeWidthApplyState::Failed:
    default:
        token->rollback();
        out.reason = result.reason;
        return finish(StrokeWidthCommandState::Failed,
                      _("Stroke width: the change failed and was rolled back"));
    }
}

StrokeWidthRowState stroke_width_row_state(StrokeWidthResult const &result)
{
    StrokeWidthRowState row;
    row.state = result.state;
    row.uniform_px = result.uniform_px;
    row.eligible = result.eligible;
    row.hairline = result.hairline;
    row.paint_none = result.paint_none;
    auto const consider = [&](StrokeWidthStyle const &style) {
        if (style.convention != StrokeWidthConvention::Hairline && style.effective_px) {
            row.max_px = std::max(row.max_px, *style.effective_px);
        }
    };
    for (auto const &target : result.targets) {
        if (target.eligibility != StrokeWidthEligibility::Eligible || target.empty_text) continue;
        if (target.kind == StrokeWidthTargetKind::Shape) {
            ++row.writable;
            if (!target.runs.empty()) consider(target.runs[0].style);
        } else if (target.kind == StrokeWidthTargetKind::TextOwner) {
            bool eligible_run = false;
            for (auto const &run : target.runs) {
                if (run.eligibility != StrokeWidthEligibility::Eligible) continue;
                eligible_run = true;
                consider(run.style);
            }
            if (eligible_run) ++row.writable;
        }
    }
    return row;
}

} // namespace Inkscape::UI
