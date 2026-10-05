// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "vacards-cli-production.h"
namespace Inkscape::VACardsCli {
enum class SettlementOwner { CliTransaction, NativePublisher, NativeHistory, None };
// Request-local policy, independent of mutable GUI preferences.
struct FrozenTransformPolicy {
    bool stroke = true, rectcorners = true, pattern = true, gradient = true;
    bool preserve_transform = false, dash_scale = true;
};
struct HistorySnapshot {
    bool can_undo = false, can_redo = false;
    std::optional<std::string> next_undo_label, next_redo_label;
};
struct HistorySnapshotResult {
    std::optional<HistorySnapshot> value;
    std::optional<ParseError> error;
};
// Deliberate EventLog inspection: one coalesced row is one step.
// Busy is a refusal; no undo/redo calls during query or computed preflight.
HistorySnapshotResult history_snapshot(EditServices &);
struct EditTargets {
    std::vector<std::string> ordered_roots, covered_ids;
    std::vector<Exclusion> exclusions;
    std::uint64_t generation = 0;
};
// Borrowed main-context references. Explicit roots are never installed into live
// selection as scratch state. Native publishers own their own Undo settlement.
struct EditServices {
    SPDocument &document;
    Selection &selection;
    DocumentStamp stamp;
    EditTargets targets;
    std::shared_ptr<void> operation_lease;
    SettlementOwner settlement = SettlementOwner::None;
};
Record execute_selection(Request const &, DispatchContext &, EditServices &);
Record execute_history(Request const &, DispatchContext &, EditServices &);
Record execute_geometry(Request const &, DispatchContext &, EditServices &);
}
