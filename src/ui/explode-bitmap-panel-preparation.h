// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: the panel's off-thread preparation (immutable input, worker, output) (EB6-panel part 2).
 * Plan: internal note EXPLODE_BITMAP_IMPLEMENTATION_PLAN, internal note EXPLODE_BITMAP_PLAN v3.2.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_EXPLODE_BITMAP_PANEL_PREPARATION_H
#define INKSCAPE_UI_EXPLODE_BITMAP_PANEL_PREPARATION_H

#include "bitmap-explode-chemistry.h"
#include "ui/explode-bitmap-overlay.h"
#include "util/bitmap-contour-offset.h"
#include "util/bitmap-contour-fit.h"

namespace Inkscape::Bitmap::PanelPreparation {
struct ContourRecipe {
    bool enabled = false;
    double offsetMm = 0, gapToleranceMm = 0.5, smoothing = 50;
};
enum class PreparationPhase { label, enclose, attach, encode, contourOffset, contourFit };
// Caller-local observation seam; no global counters or UI callbacks.
struct Observer {
    void (*observe)(PreparationPhase, void *) noexcept = nullptr;
    void *data = nullptr;
    void operator()(PreparationPhase phase) const noexcept { if (observe) observe(phase, data); }
};
// Opaque target tokens only; workers never dereference these identities.
struct AnalysisTargetIdentity {
    std::uintptr_t document = 0, desktop = 0, bitmap = 0, destinationParent = 0;
    unsigned long documentSerial = 0;
    std::uint64_t incarnation = 0, generation = 0;
    bool operator==(AnalysisTargetIdentity const &) const = default;
};
struct AnalysisIdentity {
    std::uint64_t recipeHash = 0, candidateIdentity = 0;
    AnalysisTargetIdentity target;
    bool operator==(AnalysisIdentity const &) const = default;
};
AnalysisIdentity analysisIdentity(FinalGrid const &, TargetSnapshot const &) noexcept;
// Never move this object: Partition borrows Regions. The copied final-grid view
// permits retiring the original Output while a contour-only job is in flight.
struct AnalysisState {
    explicit AnalysisState(AnalysisIdentity identity) : identity(identity) {}
    AnalysisState(AnalysisState const &) = delete;
    AnalysisState &operator=(AnalysisState const &) = delete;
    AnalysisIdentity const identity;
    std::shared_ptr<Budget> budget;
    Budget::Token envelope;
    PlainBuffer pixels;
    RgbaView grid;
    double dpiX = 0, dpiY = 0;
    Regions regions;
    Partition partition; // destroyed before regions
};
struct ContourProduct {
    std::shared_ptr<Budget> budget, ledger;
    Budget::Token reservation;
    FittedContourSet fitted; // destroyed before reservation and ledgers
};
enum class ContourRefusal { none, staleAnalysis };
struct ContourOutcome : Outcome {
    std::optional<CliBitmapFailure> failure;
    ContourRefusal refusal = ContourRefusal::none;
    ContourOutcome() = default;
    ContourOutcome(Outcome outcome, ContourRefusal refusal = ContourRefusal::none)
        : Outcome(outcome), failure(bitmapFailure(outcome, CliBitmapStage::Contour,
              refusal == ContourRefusal::staleAnalysis ? CliBitmapReason::StaleCapture : CliBitmapReason::ContourFailed)), refusal(refusal) {}
};
struct ContourResult final : JobPayload {
    std::shared_ptr<ContourProduct const> product;
    ContourOutcome outcome;
    double seconds = 0;
    std::uint64_t visits = 0, peakBudget = 0;
};
struct ContourInput final : JobPayload {
    std::shared_ptr<AnalysisState const> analysis;
    // Must describe the currently requested grid/target, not blindly copy an old state.
    AnalysisIdentity expectedIdentity;
    ContourRecipe contour;
    Observer observer;
    AllocationFault *contourFault = nullptr; // caller-local contour allocation seam
};
struct OutlineReservation {
    std::shared_ptr<Budget> parent, ledger;
    Budget::Token reservation;
    std::shared_ptr<OutlineStorage const> storage;
};
// Optional, main-thread reservation before dispatch. The aliasing output owner
// retains both ledgers through the last deferred canvas reader.
Result<std::shared_ptr<OutlineReservation>> reserveOutlines(std::shared_ptr<Budget> const &,
                                                           std::uint64_t bytes = outlineByteLimit) noexcept;
struct Input final : JobPayload {
    std::shared_ptr<OutlineReservation> outlineReservation;
    ContourRecipe contour;
    Observer observer;
    bool retainAnalysis = false; // CLI analyze retains topology without running contours.
    AllocationFault *retainedGridFault = nullptr; // only the optional retained-grid copy
    AllocationFault *contourFault = nullptr; // only B2/B3 allocations
    std::shared_ptr<AlphaDisplayBacking> display; // reserved on main before dispatch; filled only by worker
    TargetSnapshot target;
    Recipe recipe;
    GridCoverage coverage;
    HeaderLimits limits;
    std::uint64_t decodedBytes = 0;
    std::uint64_t adjustmentPixelLimit = maxCropPixels; // Lower-only encoder limit; injectable for bounded tests.
    CandidateGridInput candidate;
};
struct Output final : JobPayload {
    // Own the ledger even when a Pixbuf outlives the result/mailbox; no ownership cycle.
    std::shared_ptr<Budget> budget;
    Budget::Token envelope;
    Budget::Token proxyView;
    PlainBuffer proxy;
    unsigned proxyWidth = 0, proxyHeight = 0;
    mutable std::shared_ptr<AlphaDisplayBacking const> display; // optional display may retire before publication
    Outcome displayOutcome; // optional; never gates publication
    bool exactSourceMapping = false;
    DecodedRaster alpha;
    std::shared_ptr<PreparedAlpha> adjustment; // filled by worker; publication attached on main only
    FinalGrid grid;
    std::shared_ptr<AnalysisState const> analysis;
    ContourResult contours;
    EncodedPieces pieces;
    // Main-thread optional view state; immutable storage independently owns its ledgers.
    mutable ExactOutlines outlines;
    void omitOutlines() const noexcept { outlines = {}; }
    Outcome outlineOutcome; // Optional visual aid; never gates exact count/publication.
    std::uint64_t topologyPeak = 0;
    unsigned topologyRuns = 0;
    std::optional<CliBitmapFailure> explodeFailure, adjustmentFailure;
    Outcome explodeOutcome; // A grid/partition refusal must not discard source alpha.
    Outcome adjustmentOutcome; // Apply's whole-image encoder must not reject an admissible sparse Explode.
    unsigned count = 0, initial = 0, enclosed = 0, joined = 0, isolated = 0, smallest = 0;
    std::uint64_t visible = 0, lost = 0;
    double smallestArea = 0, lostArea = 0;
    bool opaque = false;
    bool pngStarted = false; // Records entry into either encoder, including a refused/failed encoding.
};
Outcome recheck(Budget &, MemoryProbe const * = nullptr);
struct ProxySize { unsigned width = 0, height = 0; std::uint64_t bytes = 0; };
ProxySize proxySize(unsigned width, unsigned height) noexcept;
ResourcePlan resources(unsigned width, unsigned height, std::uint64_t encoded, std::uint64_t retained,
                       ContourRecipe contour = {}, bool display = true) noexcept;
// Includes the retained input/old result supplied by the caller; use before dispatch.
ResourcePlan contourResources(std::uint64_t retained) noexcept;
JobResult calculateContours(JobInput const &, Stop, JobWork &, JobReporter &);
Outcome makeProxy(RgbaView, AlphaLut const &, bool bypass, Output &, Stop = {}, AllocationFault * = nullptr);
JobResult calculate(JobInput const &, Stop, JobWork &, JobReporter &);


} // namespace Inkscape::Bitmap

#endif // INKSCAPE_UI_EXPLODE_BITMAP_PANEL_PREPARATION_H
