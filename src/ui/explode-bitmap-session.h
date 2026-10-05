// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 VA Studio authors; released under GNU GPL v2+.
#ifndef INKSCAPE_UI_EXPLODE_BITMAP_SESSION_H
#define INKSCAPE_UI_EXPLODE_BITMAP_SESSION_H
#include <cstdint>
#include <vector>
#include <memory>
#include "ui/explode-bitmap-grid.h"
class SPImage;
namespace Inkscape::Bitmap {
// Plain opaque tokens. Neither SVG ids nor SPObject addresses define identity.
struct LogicalImageIdentity {
    std::uint64_t document = 0, image = 0;
    bool operator==(LogicalImageIdentity const &) const = default;
    explicit operator bool() const { return document && image; }
};
struct SessionRecipe;
struct PreparedBake;
class OperationIdentity {
    std::shared_ptr<PreparedBake> prepared;
    friend OperationIdentity prepareBaked(LogicalImageIdentity, std::vector<LogicalImageIdentity> const &, AllocationFault *);
    friend OperationIdentity prepareBaked(LogicalImageIdentity, std::vector<LogicalImageIdentity> const &, SessionRecipe const &, AllocationFault *);
    friend OperationIdentity prepareSessionTransfer(LogicalImageIdentity, LogicalImageIdentity);
    friend bool markBaked(OperationIdentity const &) noexcept;
public:
    explicit operator bool() const { return bool(prepared); }
};
struct SessionJobIdentity {
    LogicalImageIdentity image;
    std::uint64_t recipeGeneration = 0;
    TargetSnapshot target;
};
// All APIs main-thread only. Document destruction removes memory automatically.
LogicalImageIdentity logicalImageIdentity(SPImage &);
// Refine is user intent; bypass separately recognizes an already baked image.
struct SessionRecipe : Recipe { bool refine = true; };
// Defaults 128/40, faint floor 5% and refine ON, never serialized; no job pointers.
SessionRecipe query(LogicalImageIdentity);
// Explicit field/slider edit only: clears baked bypass, even for equal T/S.
void remember(LogicalImageIdentity, Recipe, bool refine = true);
// Prepare after staging native XML/results, BEFORE DocumentUndo::done. Failure
// returns an empty receipt: caller must cancel the pending transaction, not publish.
// All storage is allocated here; preparation changes no session settings/history.
OperationIdentity prepareBaked(LogicalImageIdentity, std::vector<LogicalImageIdentity> const &, AllocationFault * = nullptr);
OperationIdentity prepareBaked(LogicalImageIdentity, std::vector<LogicalImageIdentity> const &, SessionRecipe const &, AllocationFault * = nullptr);
// Request snapshot: bypass is inherited only for the same baked alpha recipe. No writes.
SessionRecipe requestRecipe(LogicalImageIdentity, SessionRecipe);
// Unchanged image copy: preserve the recipe AND current baked state. Prepare
// before done, then settle with markBaked; the same receipt follows Undo/Redo.
OperationIdentity prepareSessionTransfer(LogicalImageIdentity source, LogicalImageIdentity copy);
// Immediately after successful independent done; no allocation, even on rejection.
// Returns false for stale/no-op/failed publication, duplicate or wrong-thread use.
bool markBaked(OperationIdentity const &) noexcept;
// Capture alongside EB4's ticket. Delivery/admission must validate BOTH identities.
SessionJobIdentity sessionJobIdentity(LogicalImageIdentity, TargetSnapshot const &);
bool valid(SessionJobIdentity const &);
// Panel close/detach cancels EB4 jobs and resets its composer ClientLease separately.
// This invalidates queued session results while retaining T/S, floor and Refine for panel reopen.
void invalidateSessionJobs(LogicalImageIdentity);
} // namespace Inkscape::Bitmap
#endif
