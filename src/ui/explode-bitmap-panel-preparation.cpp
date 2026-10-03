// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: the panel's off-thread preparation (EB6-panel part 2).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/explode-bitmap-panel-preparation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glib.h>

namespace Inkscape::Bitmap::PanelPreparation {
namespace {
// B1/B2/B3 allocate as topology. An independent ledger gives them the engine's
// 256 MiB ceiling without spending the retained analysis topology allowance.
// Reserve that entire peak (fields, raw rings, fit scratch AND fitted output)
// in the parent before calling any engine code, as for exact outlines below.
constexpr std::uint64_t contourBytes = 256 * MiB;
constexpr std::uint64_t contourEnvelope = sizeof(ContourProduct) + sizeof(Budget) + 256;

}

Result<std::shared_ptr<OutlineReservation>> reserveOutlines(std::shared_ptr<Budget> const &budget,
                                                            std::uint64_t bytes) noexcept
{
    try {
        constexpr auto overhead = sizeof(OutlineReservation) + sizeof(OutlineStorage) + sizeof(Budget) + 256;
        bytes = std::min(bytes, outlineByteLimit);
        if (bytes <= overhead) return {{Status::unavailable, "Piece outlines are unavailable. The piece count is exact."}};
        Budget::Token token;
        auto o = budget->acquire(Stage::prepared, bytes, token); if (!o.ok()) return {o};
        auto owner = std::make_shared<OutlineReservation>(); owner->parent = budget;
        owner->reservation = std::move(token);
        owner->ledger = std::make_shared<Budget>(bytes-overhead);
        auto memory = sampleMemory(); if (!memory.ok()) return {memory.outcome};
        o = owner->ledger->recheck(memory.value); if (!o.ok()) return {o};
        return {{}, std::move(owner)};
    } catch (...) { return {{Status::failed, "Piece outlines are unavailable. The piece count is exact."}}; }
}

AnalysisIdentity analysisIdentity(FinalGrid const &grid, TargetSnapshot const &target) noexcept
{
    return {grid.recipeHash, grid.candidateIdentity,
            {target.document, target.desktop, target.bitmap, target.destinationParent,
             target.documentSerial, target.incarnation, target.generation}};
}

ProxySize proxySize(unsigned width, unsigned height) noexcept
{
    if (!width || !height || width > 16384 || height > 16384 || std::uint64_t(width)*height > 100000000) return {};
    // Reserve both immutable RGBA and the toolkit's possible texture copy.
    auto scale = std::min({1.0, 1024.0 / std::max(width, height), std::sqrt(double(4*MiB) / (8.0*width*height))});
    unsigned w = std::max(1u, unsigned(width*scale)), h = std::max(1u, unsigned(height*scale));
    return {w, h, std::uint64_t(w)*h*4};
}
ResourcePlan resources(unsigned width, unsigned height, std::uint64_t encoded, std::uint64_t retained, ContourRecipe contour, bool display) noexcept
{
    ResourcePlan plan;
    if (!proxySize(width, height).bytes || encoded > 256*MiB) return plan;
    plan.phases = 3;
    auto p = std::uint64_t(width)*height;
    plan.add(Term::decode, p*8 + MiB, 0, 0); // decoder output + native codec pixels
    plan.add(Term::canonical, p*8, 1, 1); // adjusted source + exact grid; expansion rechecks at allocation
    plan.add(Term::canonical, p*8, 2, 2); // source retained through count; encode Apply only after product admission
    plan.add(Term::preview, proxySize(width, height).bytes*2, 1, 2);
    if (display) plan.add(Term::cache, p*4, 0, 2); // includes pre-dispatch reservation; retired readers are queued
    plan.add(Term::prepared, sizeof(Output) + sizeof(PreparedAlpha) + sizeof(Budget) + 256, 0, 2);
    // Bootstrap for row indexes and small run/region tables, not a dense
    // 128-bytes-per-pixel worst case. label counts runs before allocating;
    // label/enclose/attach reserve every actual buffer (including overlap)
    // before allocation through the same Budget. Growth remains bounded by
    // refreshed J and the combined 256 MiB topology ceiling (§13.2).
    plan.add(Term::topology, 4*MiB + (std::uint64_t(height)+1)*sizeof(std::uint32_t), 2, 2);
    plan.add(Term::recovery, encoded*4, 0, 2);
    plan.add(Term::history, 0, 0, 2); plan.add(Term::redo, 0, 0, 2);
    plan.add(Term::queued, retained, 0, 2);
    if (contour.enabled) {
        // Analysis topology is retained during contours/encoding. Its existing
        // 256 MiB engine ceiling includes live Regions and Partition overlap.
        plan.add(Term::topology, 256*MiB - (4*MiB + (std::uint64_t(height)+1)*4), 2, 2);
        plan.add(Term::canonical, p*4, 2, 2); // immutable FinalGrid copy
        plan.add(Term::prepared, contourBytes + contourEnvelope + sizeof(AnalysisState) + 64, 2, 2);
    }
    return plan;
}
ResourcePlan contourResources(std::uint64_t retained) noexcept
{
    ResourcePlan plan;
    plan.add(Term::prepared, contourBytes + contourEnvelope + sizeof(ContourResult) + 64, 0, 0);
    plan.add(Term::queued, retained, 0, 0);
    plan.add(Term::recovery, 0, 0, 0);
    plan.add(Term::history, 0, 0, 0);
    plan.add(Term::redo, 0, 0, 0);
    return plan;
}
Outcome makeProxy(RgbaView source, AlphaLut const &lut, bool bypass, Output &out, Stop stop, AllocationFault *fault)
{
    auto check = source.validate(); if (!check.ok()) return check;
    auto size = proxySize(source.width, source.height);
    if (!size.bytes) return {Status::failed, "Invalid proxy dimensions."};
    check = out.proxy.allocate(*out.budget, Stage::preview, size.bytes, 1, fault, stop); if (!check.ok()) return check;
    check = out.budget->acquire(Stage::preview, size.bytes, out.proxyView); if (!check.ok()) { out.proxy.reset(); return check; }
    out.proxyWidth = size.width; out.proxyHeight = size.height;
    for (unsigned y = 0; y < size.height; ++y) {
        if (stop.requested()) return {Status::canceled, "Canceled"};
        for (unsigned x = 0; x < size.width; ++x) {
            auto src = source.data + (std::uint64_t(y)*source.height/size.height)*source.stride + (std::uint64_t(x)*source.width/size.width)*4;
            auto dst = reinterpret_cast<unsigned char *>(out.proxy.data()) + (std::uint64_t(y)*size.width+x)*4;
            std::memcpy(dst, src, 3); dst[3] = bypass ? src[3] : lut[src[3]];
        }
    }
    return {};
}
Outcome recheck(Budget &b) { auto m = sampleMemory(); return m.ok() ? b.recheck(m.value) : m.outcome; }
namespace {
ContourResult contours(AnalysisState const &state, ContourRecipe recipe,
                       std::shared_ptr<Budget> budget, Stop stop, JobReporter &reporter, Observer observer,
                       AllocationFault *fault)
{
    ContourResult result;
    if (!recipe.enabled) return result;
    auto start = JobClock::now();
    JobWork work(std::uint64_t(state.grid.width)*state.grid.height, 100);
    PhaseTimer timer;
    auto run = [&]() -> Outcome {
        if (stop.requested()) return {Status::canceled, "Canceled"};
        if (!std::isfinite(recipe.offsetMm) || !std::isfinite(recipe.gapToleranceMm) ||
            recipe.gapToleranceMm < 0 || !std::isfinite(recipe.smoothing) ||
            recipe.smoothing < 0 || recipe.smoothing > 100)
            return {Status::incompatible, "Invalid contour recipe."};
        auto check = recheck(*budget); if (!check.ok()) return check;
        Budget::Token reservation;
        check = budget->acquire(Stage::prepared, contourBytes + contourEnvelope, reservation);
        if (!check.ok()) return check;
        result.peakBudget = budget->reserved();
        auto product = std::make_shared<ContourProduct>();
        product->budget = budget; product->reservation = std::move(reservation);
        product->ledger = std::make_shared<Budget>(contourBytes);
        check = recheck(*product->ledger); if (!check.ok()) return check;
        struct Progress {
            JobReporter &reporter;
            JobWork &work;
            static void trace(ContourPhase, void *data) noexcept {
                auto &p = *static_cast<Progress *>(data);
                p.reporter.progress({Stage::prepared, std::min<std::uint64_t>(79, 80*p.work.visits()/p.work.limit()),
                                     100, JobPhase::contourTrace});
            }
            static void fit(FitPhase, void *data) noexcept {
                auto &p = *static_cast<Progress *>(data);
                p.reporter.progress({Stage::prepared, 80, 100, JobPhase::contourFit});
            }
        } progress{reporter, work};
        observer(PreparationPhase::contourOffset);
        // A boundary observer may request Stop synchronously. Honor it here,
        // before dispatching the engine or allowing an optional refusal to win.
        if (stop.requested()) return {Status::canceled, "Canceled"};
        reporter.progress({Stage::prepared, 0, 100, JobPhase::contourTrace});
        ContourOptions options; options.observe = Progress::trace; options.observerData = &progress; options.fault = fault;
        auto raw = offsetContours(state.grid, state.partition,
            {recipe.offsetMm, recipe.gapToleranceMm, state.dpiX, state.dpiY},
            *product->ledger, work, stop, options);
        if (!raw.ok()) return raw.outcome;
        check = timer.check(stop); if (!check.ok()) return check;
        observer(PreparationPhase::contourFit);
        if (stop.requested()) return {Status::canceled, "Canceled"};
        FitOptions fit; fit.observe = Progress::fit; fit.observerData = &progress; fit.fault = fault;
        auto fitted = fitContours(raw.value, {recipe.smoothing}, *product->ledger, work, stop, fit);
        if (!fitted.ok()) return fitted.outcome;
        check = timer.check(stop); if (!check.ok()) return check;
        product->fitted = std::move(fitted.value);
        // Scratch is gone before shrinking the parent reservation to live output.
        raw.value = {};
        check = product->reservation.resize(Stage::prepared, product->ledger->reserved() + contourEnvelope);
        if (!check.ok()) return check;
        result.product = std::move(product);
        reporter.progress({Stage::prepared, 100, 100, JobPhase::contourFit});
        return {Status::changed, ""};
    };
    result.outcome = run();
    result.seconds = std::chrono::duration<double>(JobClock::now()-start).count();
    result.visits = work.visits();
    return result;
}
}
JobResult calculateContours(JobInput const &job, Stop stop, JobWork &, JobReporter &reporter)
{
    auto const &in = static_cast<ContourInput const &>(*job.storage.payload);
    if (!in.analysis) return {{Status::incompatible, "Missing retained analysis."}, {}, 0};
    ContourResult value;
    if (in.expectedIdentity != in.analysis->identity) {
        value.outcome = {{Status::unavailable, "Stale retained analysis."}, ContourRefusal::staleAnalysis};
    } else {
        value = contours(*in.analysis, in.contour, job.storage.budget, stop, reporter, in.observer, in.contourFault);
    }
    if (value.outcome.status == Status::canceled) return {value.outcome, {}, 0};
    // The result object itself is charged; pin the ledger with an aliasing owner.
    struct Owner {
        std::shared_ptr<Budget> budget;
        Budget::Token envelope;
        ContourResult value;
    };
    Budget::Token envelope;
    auto check = job.storage.budget->acquire(Stage::prepared, sizeof(Owner) + 64, envelope);
    if (!check.ok()) return {check, {}, 0};
    auto owner = std::make_shared<Owner>(); owner->budget = job.storage.budget;
    owner->envelope = std::move(envelope); owner->value = std::move(value);
    JobResult result; result.value.budget = job.storage.budget;
    result.value.payload = std::shared_ptr<ContourResult const>(owner, &owner->value);
    return result;
}
JobResult calculate(JobInput const &job, Stop stop, JobWork &work, JobReporter &reporter)
{
    auto const &in = static_cast<Input const &>(*job.storage.payload);
    if (in.recipe.threshold > 255 || in.recipe.softness > 127 || in.recipe.faintFloor > 25)
        return {{Status::incompatible, "Invalid alpha recipe."}, {}, 0};
    auto budget = job.storage.budget;
    // Optional geometry yields to required grid/topology buffers. A refused
    // stage releases its partial result before one retry without outlines.
    auto required = [&](auto run) {
        auto result = run();
        if (!result.ok() && std::string_view(result.outcome.diagnostic) ==
                "Reservation exceeds current operation/stage budget" &&
            in.outlineReservation && in.outlineReservation->reservation) {
            result = {};
            in.outlineReservation->reservation.release();
            result = run();
        }
        return result;
    };
    auto fail = [](Outcome o) { return JobResult{o, {}, 0}; };
    auto check = recheck(*budget); if (!check.ok()) return fail(check);
    Budget::Token envelope;
    check = budget->acquire(Stage::prepared, sizeof(Output) + sizeof(PreparedAlpha) + sizeof(Budget) + 256, envelope); if (!check.ok()) return fail(check);
    auto out = std::make_shared<Output>(); out->budget = budget; out->envelope = std::move(envelope);
    // Progress counts completed preparation phases, never elapsed time or an
    // invented pixel estimate. Each phase may take a different amount of time.
    auto analyzing = [&](std::uint64_t done) { reporter.progress({Stage::topology, done, 8}); };
    auto preparing = [&](std::uint64_t done) { reporter.progress({Stage::prepared, done, 3}); };
    analyzing(0);
    if (!in.candidate.identity) {
        PlainBuffer encoded;
        check = encoded.allocate(*budget, Stage::input, in.decodedBytes, 1, nullptr, stop); if (!check.ok()) return fail(check);
        gint state = 0; guint save = 0; std::size_t size = 0;
        for (std::size_t at = 0; at < job.storage.bytes.size(); at += 65536) {
            if (stop.requested()) return fail({Status::canceled, "Canceled"});
            size += g_base64_decode_step(reinterpret_cast<char const *>(job.storage.bytes.data() + at),
                std::min<std::size_t>(65536, job.storage.bytes.size() - at),
                reinterpret_cast<guchar *>(encoded.data()) + size, &state, &save);
        }
        analyzing(1);
        auto decoded = decode({{reinterpret_cast<std::uint8_t const *>(encoded.data()), size, {}}, in.limits}, *budget, stop);
        if (!decoded.ok()) return fail(decoded.outcome);
        out->alpha = std::move(decoded.value); out->alpha.original = {};
        // Decoder storage includes base64 scratch padding; grid/PNG own exact ICC bytes.
        if (out->alpha.profile.size() != out->alpha.profileBytes) {
            if (out->alpha.profileBytes > out->alpha.profile.size()) return fail({Status::failed, "Invalid profile extent"});
            PlainBuffer profile;
            check = profile.allocate(*budget, Stage::canonical, out->alpha.profileBytes, 1, nullptr, stop);
            if (!check.ok()) return fail(check);
            for (std::size_t at = 0; at < profile.size(); at += 65536) {
                if (stop.requested()) return fail({Status::canceled, "Canceled"});
                std::memcpy(profile.data() + at, out->alpha.profile.data() + at, std::min<std::size_t>(65536, profile.size() - at));
            }
            out->alpha.profile = std::move(profile);
        }
    }
    analyzing(2);
    auto recipe = in.recipe; recipe.coverage = std::any_of(in.target.contexts.begin(), in.target.contexts.end(), [&](auto const &c) { return c.identity == in.target.bitmap && c.clip; }) ? &in.coverage : nullptr; recipe.work = &work;
    auto const &raster = in.candidate.identity ? in.candidate.raster : out->alpha;
    auto bytes = reinterpret_cast<std::uint8_t const *>(raster.pixels.data());
    auto lut = recipeAlphaLut(recipe); out->opaque = true;
    auto pixels = std::uint64_t(raster.width) * raster.height;
    for (std::uint64_t i = 0; i < pixels; ++i) {
        if ((i & 16383) == 0 && stop.requested()) return fail({Status::canceled, "Canceled"});
        auto a = bytes[4 * i + 3]; out->opaque &= a == 255; out->visible += a != 0;
        auto adjusted = lut[a]; out->lost += a && !adjusted;
        if (!in.candidate.identity) out->alpha.pixels.data()[4*i+3] = std::byte(adjusted);
    }
    if (in.display && !in.candidate.identity) {
        out->displayOutcome = prepareAlphaDisplay(
            {bytes, raster.pixels.size(), std::uint64_t(raster.width)*4, raster.width, raster.height}, *in.display, stop);
        if (out->displayOutcome.status == Status::canceled) return fail(out->displayOutcome);
        if (out->displayOutcome.ok()) out->display = in.display;
    } else out->displayOutcome = {Status::unavailable, "Detailed display allocation unavailable."};
    analyzing(3);
    check = makeProxy({bytes, raster.pixels.size(), std::uint64_t(raster.width)*4, raster.width, raster.height},
                      lut, !in.candidate.identity, *out, stop);
    if (!check.ok()) return fail(check);
    analyzing(4);
    auto prepareAdjustment = [&]() -> Outcome {
        // With T/S bypassed, only removing nonzero alpha changes pixels.
        // A nonzero floor alone must not prepare an unchanged Apply payload.
        if (in.recipe.alphaPrepared || (in.recipe.bypassAlpha && !out->lost) || in.candidate.identity || out->pngStarted) return {};
        // Move the exact alpha-only source storage temporarily; never encode the
        // Explode grid, which already includes tone/clip/opacity/straightening.
        FinalGrid source; source.width = out->alpha.width; source.height = out->alpha.height;
        // Source-axis density, independent of Explode's straightened grid.
        auto own = std::find_if(in.target.contexts.begin(), in.target.contexts.end(),
            [&](auto const &c) { return c.identity == in.target.bitmap; });
        if (own == in.target.contexts.end()) return {Status::failed, "Missing source geometry"};
        auto const &p = own->pixelToItem; auto const &m = own->itemToDocument;
        source.dpiX = 96 / std::hypot(p[0]*m[0] + p[1]*m[2], p[0]*m[1] + p[1]*m[3]);
        source.dpiY = 96 / std::hypot(p[2]*m[0] + p[3]*m[2], p[2]*m[1] + p[3]*m[3]);
        source.pixels = std::move(out->alpha.pixels); source.profile = std::move(out->alpha.profile);
        out->pngStarted = true;
        check = recheck(*budget); if (!check.ok()) return check;
        EncodeOptions options; options.work = &work; options.maxCropPixels = in.adjustmentPixelLimit;
        in.observer(PreparationPhase::encode);
        auto adjustment = prepareAlpha(source, *budget, stop, options);
        out->alpha.pixels = std::move(source.pixels); out->alpha.profile = std::move(source.profile);
        out->adjustmentOutcome = adjustment.outcome;
        if (adjustment.outcome.status == Status::canceled) return adjustment.outcome;
        if (adjustment.ok()) out->adjustment = std::make_shared<PreparedAlpha>(std::move(adjustment.value));
        return {};
    };
    AlphaLut identity; for (unsigned i = 0; i < 256; ++i) identity[i] = i;
    auto explodeFail = [&](Outcome o) {
        if (o.status == Status::canceled) return fail(o);
        // Preserve Apply for geometric refusals, but never encode an over-cap partition.
        if (std::string_view(o.diagnostic) != "Final piece cap exceeded") {
            auto adjustment = prepareAdjustment(); if (!adjustment.ok()) return fail(adjustment);
        }
        out->alpha.pixels.reset();
        out->omitOutlines();
        out->explodeOutcome = o;
        JobResult result; result.value.budget = budget; result.value.payload = std::move(out); return result;
    };
    check = recheck(*budget); if (!check.ok()) return explodeFail(check);
    // Source alpha is already refined; compose/straighten it exactly once for Explode.
    if (!in.candidate.identity) recipe.alphaPrepared = true;
    auto grid = required([&] { return in.candidate.identity ? prepareGrid(in.candidate, recipe, *budget, stop) : prepareGrid(in.target, out->alpha, recipe, *budget, stop); });
    if (!grid.ok()) return explodeFail(grid.outcome);
    out->grid = std::move(grid.value);
    if (!in.candidate.identity && out->grid.sampling == GridSampling::Exact &&
        out->grid.width == raster.width && out->grid.height == raster.height) {
        auto own = std::find_if(in.target.contexts.begin(), in.target.contexts.end(),
            [&](auto const &c) { return c.identity == in.target.bitmap; });
        if (own != in.target.contexts.end()) {
            auto const &p = own->pixelToItem; auto const &m = own->itemToDocument;
            GridTransform mapping{p[0]*m[0]+p[1]*m[2], p[0]*m[1]+p[1]*m[3],
                p[2]*m[0]+p[3]*m[2], p[2]*m[1]+p[3]*m[3],
                p[4]*m[0]+p[5]*m[2]+m[4], p[4]*m[1]+p[5]*m[3]+m[5]};
            out->exactSourceMapping = true;
            for (unsigned i = 0; i < 6; ++i)
                out->exactSourceMapping &= std::isfinite(mapping[i]) &&
                    std::abs(mapping[i]-out->grid.pixelToDocument[i]) <= 1e-10 * std::max(1.0, std::abs(mapping[i]));
        }
    }
    analyzing(5);
    check = recheck(*budget); if (!check.ok()) return explodeFail(check);
    std::shared_ptr<AnalysisState> retained;
    if (in.contour.enabled) {
        Budget::Token token;
        check = budget->acquire(Stage::prepared, sizeof(AnalysisState) + 64, token);
        if (!check.ok()) return explodeFail(check);
        retained = std::make_shared<AnalysisState>(analysisIdentity(out->grid, in.target)); retained->budget = budget;
        retained->envelope = std::move(token);
    }
    in.observer(PreparationPhase::label);
    auto regions = required([&] { return label(out->grid.view(), identity, *budget, work, stop); }); if (!regions.ok()) return explodeFail(regions.outcome);
    // Move BEFORE creating any borrowing partition, never afterward.
    if (retained) retained->regions = std::move(regions.value);
    auto const &regionData = retained ? retained->regions : regions.value;
    analyzing(6);
    EnclosureOptions eo; eo.speckDpiX = out->grid.dpiX; eo.speckDpiY = out->grid.dpiY;
    in.observer(PreparationPhase::enclose);
    auto enclosed = required([&] { return enclose(regionData, *budget, work, stop, eo); }); if (!enclosed.ok()) return explodeFail(enclosed.outcome);
    analyzing(7);
    auto metric = OrthogonalMetric::fromDpi(out->grid.dpiX, out->grid.dpiY); if (!metric.ok()) return explodeFail(metric.outcome);
    in.observer(PreparationPhase::attach);
    auto partition = required([&] { return attach(enclosed.value, metric.value, *budget, work, stop); }); if (!partition.ok()) return explodeFail(partition.outcome);
    analyzing(8);
    if (retained) retained->partition = std::move(partition.value);
    auto const &p = retained ? retained->partition : partition.value;
    out->topologyPeak = p.topologyPeak; out->topologyRuns = p.runCount;
    out->count = p.pieceCount; out->initial = p.foregroundCount; out->enclosed = p.enclosureMerges;
    out->joined = p.speckJoins; out->isolated = p.retainedIsolatedSpecks;
    if (out->count > MaxExplodePieces) {
        out->alpha.pixels.reset();
        JobResult result; result.value.budget = budget; result.value.payload = std::move(out); return result;
    }
    if (retained) {
        check = retained->pixels.allocate(*budget, Stage::canonical, out->grid.pixels.size(), 1, in.retainedGridFault, stop);
        if (check.status == Status::canceled) return fail(check);
        if (!check.ok()) {
            // Partition still borrows retained->regions for encoding below.
            // Refuse only contours/reuse; the exact count and bitmap products survive.
            out->contours.outcome = check;
        } else {
            for (std::size_t at = 0; at < out->grid.pixels.size(); at += 65536) {
                if (stop.requested()) return fail({Status::canceled, "Canceled"});
                std::memcpy(retained->pixels.data()+at, out->grid.pixels.data()+at,
                            std::min<std::size_t>(65536, out->grid.pixels.size()-at));
            }
            retained->grid = out->grid.view();
            retained->grid.data = reinterpret_cast<std::uint8_t const *>(retained->pixels.data());
            retained->dpiX = out->grid.dpiX; retained->dpiY = out->grid.dpiY;
            out->analysis = retained;
            out->contours = contours(*retained, in.contour, budget, stop, reporter, in.observer, in.contourFault);
            if (out->contours.outcome.status == Status::canceled) return fail(out->contours.outcome);
        }
    }
    preparing(0);
    check = prepareAdjustment(); if (!check.ok()) return fail(check);
    preparing(1);
    out->alpha.pixels.reset();
    auto mm = metric.value.pixelMmX() * metric.value.pixelMmY();
    auto const &c = (in.candidate.identity ? in.candidate.geometry : in.target).contexts.front();
    auto det = [](auto const &m) { return m[0] * m[3] - m[1] * m[2]; };
    out->lostArea = out->lost * std::abs(det(c.pixelToItem) * det(c.itemToDocument)) * (25.4 / 96) * (25.4 / 96);
    for (unsigned i = 0; i < p.pieceCount; ++i) if (!i || p.pieces()[i].area * mm < out->smallestArea) {
        out->smallest = i; out->smallestArea = p.pieces()[i].area * mm;
    }
    if (!out->opaque && out->visible) {
        // Adapter for EB4's read-only API: retain a bounded subledger under the
        // operation's outline reservation, never compete with the alpha proxy.
        auto outline = [&]() -> Outcome {
            auto owner = in.outlineReservation;
            if (!owner || !owner->reservation) return {Status::unavailable, "Piece outlines are unavailable. The piece count is exact."};
            // Optional work must not exhaust the exact count/encoder work budget.
            JobWork outlineWork(std::uint64_t(p.width)*p.height);
            auto outlines = prepareOutlines(p, out->grid, *owner->ledger, outlineWork, stop);
            if (outlines.ok()) {
                owner->storage = std::move(outlines.value.storage);
                outlines.value.storage = std::shared_ptr<OutlineStorage const>(owner, owner->storage.get());
                out->outlines = std::move(outlines.value);
            }
            if (!outlines.ok()) owner->reservation.release();
            else owner->reservation.resize(Stage::prepared, owner->ledger->reserved() +
                sizeof(OutlineReservation) + sizeof(OutlineStorage) + sizeof(Budget) + 256);
            return outlines.outcome;
        };
        out->outlineOutcome = outline();
        if (out->outlineOutcome.status == Status::canceled) return fail(out->outlineOutcome);
        if (!out->outlineOutcome.ok()) {
            out->omitOutlines();
        }
        preparing(2);
        if (out->count >= 1) {
            out->pngStarted = true;
            check = recheck(*budget); if (!check.ok()) return explodeFail(check);
            EncodeOptions options; options.work = &work;
            in.observer(PreparationPhase::encode);
            auto png = encode(out->grid, p, *budget, stop, options); if (!png.ok()) return explodeFail(png.outcome);
            out->pieces = std::move(png.value);
        }
    }
    preparing(3);
    JobResult result; result.value.budget = budget; result.value.payload = std::move(out); return result;
}


} // namespace Inkscape::Bitmap
