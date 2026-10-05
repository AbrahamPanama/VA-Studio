// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: atomic Explode and alpha-only publication (EB5-publish).
 * Plan: internal note EXPLODE_BITMAP_IMPLEMENTATION_PLAN, internal note EXPLODE_BITMAP_PLAN v3.2.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_BITMAP_EXPLODE_CHEMISTRY_H
#define INKSCAPE_BITMAP_EXPLODE_CHEMISTRY_H

#include "ui/explode-bitmap-dependencies.h"
#include "ui/explode-bitmap-session.h"
#include "util/bitmap-memory-admission.h"
#include "util/bitmap-piece-encoder.h"
#include "util/bitmap-contour-fit.h"

namespace Inkscape::Bitmap {

std::string serializeContours(FittedContourSet const &, unsigned begin, unsigned end);

enum class PublishStage {
    Admission, Href, Node, Script, Guard, Delete, Resources, Insert, Binding, Native,
    Selection, Bake, Readiness, Settlement, Rollback
};
// Main-thread, caller-local fault seam. Throwing here models failures while the
// atomic token is still rollbackable; Rollback runs before native recovery.
struct PublishHooks {
    void (*checkpoint)(PublishStage, unsigned, void *) = nullptr;
    void *data = nullptr;
};
// Immutable worker results and their ledger must outlive the synchronous call.
// The activation owner is shared across results from ONE panel/job controller;
// never recreate it to retry an already consumed ticket or prepared result.
struct ContourStyle {
    std::string stroke = "#ff00ff";
    double strokeWidthMm = 0.1;
};
struct Prepared {
    TargetSnapshot target;
    // Immutable request recipe; required by document contexts. Legacy desktop
    // callers may omit it and retain the session-query behavior.
    std::optional<SessionRecipe> requestRecipe;
    DependencyToken dependencies;
    SessionJobIdentity session;
    FinalGrid const *grid = nullptr;
    EncodedPieces const *pieces = nullptr;
    Budget *budget = nullptr;
    ResourcePlan resources;
    Memory const *start = nullptr;
    MemoryProbe const *probe = nullptr; // null uses the native probe
    AdmissionLimits limits;
    std::shared_ptr<DependencyRequest> activation;
    PublishHooks hooks;
    FittedContourSet const *contours = nullptr;
    ContourStyle contourStyle;
};
// Worker-produced alpha-only source grid: no tone, clip, opacity or geometry bake.
// Profile bytes are passed unchanged through EB2-png. Budget outlives this result.
// Worker fills only image; the main-thread caller attaches publication at admission.
struct PreparedAlpha {
    Prepared publication;
    EncodedPieces image; // exactly one full image, without gutters or bleed
};
Result<PreparedAlpha> prepareAlpha(FinalGrid const &, Budget &, Stop = {}, EncodeOptions = {}) noexcept;
// Uses publication.grid for the analyzed pixel-to-parent mapping; no PNG split.
struct PreparedContourOnly { Prepared publication; };
Outcome publishContourOnly(SPDesktop &, PreparedContourOnly const &, Ticket);
Outcome publishExplode(SPDesktop &, Prepared const &, Ticket);
Outcome publishAlpha(SPDesktop &, PreparedAlpha const &, Ticket);

} // namespace Inkscape::Bitmap

#endif // INKSCAPE_BITMAP_EXPLODE_CHEMISTRY_H
