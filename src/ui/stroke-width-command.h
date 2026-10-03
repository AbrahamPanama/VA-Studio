// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Shared stroke-width command for the Fill and Stroke width row and the status
 * bar (internal note STROKE_WIDTH_CONTROLS_PLAN §3).
 *
 * Operation mode: compatible-member editing of the whole selection (or the
 * explicit text range) as ONE user action with one Undo step. Targets, exclusions
 * and clone rules are the stroke-width controller's; this file owns only the
 * order of the steps around it:
 *   1. capture the scope (desktop, document, generation, roots, unit, text range);
 *   2. prepare read-only; a plan that changes nothing returns Unchanged here, with
 *      nothing settled, so an existing Redo stack survives;
 *   3. only for a change: settle pending automatic updates, capture and prepare
 *      again, refuse if the scope or the planned outcome differs;
 *   4. begin the atomic interaction, 5. apply, 6. commit with an output-ready check
 *      that revalidates the full captured scope.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_STROKE_WIDTH_COMMAND_H
#define INKSCAPE_UI_STROKE_WIDTH_COMMAND_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "ui/stroke-width-controller.h"
#include "util-string/context-string.h"

class SPDesktop;
namespace Inkscape::Util { class Unit; }

namespace Inkscape::UI {

enum class StrokeWidthCommandState {
    Applied,   ///< committed: one Undo step
    Unchanged, ///< nothing to write; no history, no settlement, no default touched
    Refused,   ///< scope changed, IME composition, or another edit in progress; nothing written
    Rejected,  ///< the controller refused the plan or the apply; nothing written
    Failed,    ///< the change could not be confirmed and was rolled back
};

struct StrokeWidthCommandRequest {
    /// kind and value; scale_dashes is set by the command from /options/dash/scale.
    StrokeWidthIntent intent;
    /// The caller's live scope identity (read at capture and again in the commit
    /// readiness check, so a synchronous selection or document change fails it).
    /// Null: a constant 0.
    std::uint64_t const *live_generation;
    /// Display unit at click time; part of the captured scope.
    Util::Unit const *unit;
    Util::Internal::ContextString undo_label;
    std::string icon;
};

struct StrokeWidthCommandOutcome {
    StrokeWidthCommandState state = StrokeWidthCommandState::Unchanged;
    bool committed = false;
    std::size_t changed = 0;
    std::size_t excluded = 0;
    std::size_t following_clones = 0;
    StrokeWidthMemberReason reason = StrokeWidthMemberReason::None;
    /// The status text that was flashed; empty when the command was silent.
    std::string note;
};

/// Apply \a request to the live selection of \a desktop. Never reads the active
/// desktop, and never touches selection, tool state or default styles.
StrokeWidthCommandOutcome apply_stroke_widths_command(SPDesktop &desktop, StrokeWidthCommandRequest const &request);

/// The non-collapsed text-tool range of \a desktop's tool, or nullopt. Raw
/// logical indices, never collapsed or reordered. One copy for every caller.
std::optional<StrokeWidthTextRange> stroke_width_current_text_range(SPDesktop *desktop);

/// Width-row policy computed from one query. No GTK and no document access.
struct StrokeWidthRowState {
    StrokeWidthQuery state = StrokeWidthQuery::Empty;
    std::optional<double> uniform_px;
    std::size_t eligible = 0;
    /// Eligible members the controller can write: shapes and text owners with an
    /// eligible run. A clone instance alone is not writable; a clone whose source
    /// is also selected is covered by that source.
    std::size_t writable = 0;
    std::size_t hairline = 0;
    std::size_t paint_none = 0;
    /// Largest eligible non-hairline effective width in CSS px (0 if none).
    /// `stroke_width_can_decrease` is monotone in the width, so "some member can
    /// decrease" is `can_decrease(max_px, step_px)`.
    double max_px = 0.0;
};

StrokeWidthRowState stroke_width_row_state(StrokeWidthResult const &result);

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_STROKE_WIDTH_COMMAND_H
