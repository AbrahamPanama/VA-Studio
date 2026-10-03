// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Tests for the Explode Bitmap engine (Memory): T02 overlap, T04 caches/post-commit, T05 probes.
 * Every case except NativeSmoke uses only injected probes, so results are deterministic.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include "util/bitmap-memory-admission.h"
#include "ui/explode-bitmap-panel-preparation.h"

using namespace Inkscape::Bitmap;
namespace {
constexpr std::uint64_t GiB = 1024 * MiB;

struct FakeProbe final : MemoryProbe {
    bool ok = true;
    RawMemory raw;
    FakeProbe(std::uint64_t r, std::uint64_t a, std::uint64_t e, bool good = true) : ok(good), raw{r, a, e} {}
    bool read(RawMemory &out) const noexcept override { out = raw; return ok; }
};
MemorySample sample(std::uint64_t r, std::uint64_t a, std::uint64_t e)
{
    auto s = sampleMemory(FakeProbe(r, a, e));
    EXPECT_TRUE(s.ok());
    EXPECT_TRUE(s.value.measured);
    return s.value;
}
MemorySample roomy() { return sample(16 * GiB, 16 * GiB, 100 * MiB); } // J = 1536 MiB
ResourcePlan plan(std::initializer_list<Reservation> list)
{
    ResourcePlan p;
    for (auto &r : list) EXPECT_TRUE(p.add(r.term, r.bytes, r.first, r.last));
    for (Term t : {Term::recovery, Term::history, Term::redo, Term::queued}) { // explicit zero, whole plan
        bool have = false;
        for (unsigned i = 0; i < p.count; ++i) have = have || p.items[i].term == t;
        if (!have) EXPECT_TRUE(p.add(t, 0, 0, maxPhases - 1));
    }
    return p;
}
} // namespace

// ---- T05 probes ----
TEST(ExplodeBitmapMemory, T05UnavailableProbeNeverGuesses)
{
    auto r = sampleMemory(FakeProbe(8 * GiB, 4 * GiB, GiB, false));
    EXPECT_EQ(r.outcome.status, Status::unavailable);
    EXPECT_FALSE(r.value.measured);
    EXPECT_EQ(r.value.physical, 0u);
    // Admission through an unmeasured sample is refused, never optimistic.
    auto a = admit(plan({{Term::decode, MiB, 0, 0}}), r.value);
    EXPECT_EQ(a.outcome.status, Status::unavailable);
    EXPECT_EQ(a.value.limit, 0u);
}

TEST(ExplodeBitmapMemory, T05ZeroAndInconsistentValuesRefused)
{
    for (auto const &p : {FakeProbe(0, GiB, GiB), FakeProbe(8 * GiB, 0, GiB), FakeProbe(8 * GiB, GiB, 0),
                          FakeProbe(2 * GiB, 3 * GiB, GiB),     // available > physical
                          FakeProbe(2 * GiB, GiB, 3 * GiB)}) { // footprint > physical
        auto r = sampleMemory(p);
        EXPECT_EQ(r.outcome.status, Status::unavailable);
        EXPECT_FALSE(r.value.measured);
    }
}

TEST(ExplodeBitmapMemory, T05PressuredProbeRefusesBeforeAllocation)
{
    // 8 GiB machine, only 200 MiB available: below the 256 MiB recovery margin.
    auto s = sample(8 * GiB, 200 * MiB, GiB);
    auto a = admit(plan({{Term::decode, MiB, 0, 0}}), s);
    EXPECT_EQ(a.outcome.status, Status::failed);
    EXPECT_STREQ(a.outcome.diagnostic, "No recovery headroom");
    // High baseline: E >= 3072-256 MiB leaves no process headroom at all.
    a = admit(plan({{Term::decode, MiB, 0, 0}}), sample(16 * GiB, 16 * GiB, 2816 * MiB));
    EXPECT_EQ(a.outcome.status, Status::failed);
}

TEST(ExplodeBitmapMemory, T05FourGiBPcWithOneGiBAvailable)
{
    auto s = sample(4 * GiB, GiB, 500 * MiB);
    auto ok = admit(plan({{Term::decode, 768 * MiB, 0, 0}}), s);
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(ok.value.limit, 768 * MiB); // available minus 256 MiB recovery binds
    EXPECT_STREQ(ok.value.jBinding, "available RAM - recovery");
    auto over = admit(plan({{Term::decode, 768 * MiB + 1, 0, 0}}), s);
    EXPECT_EQ(over.outcome.status, Status::failed);
    EXPECT_EQ(over.value.binding, Term::decode);
    // The packed buffer fits at 1 GiB available; a genuinely pressured machine refuses.
    EXPECT_TRUE(admit(plan({{Term::decode, 400000000, 0, 0}}), s).ok());
    auto low = sample(4 * GiB, 300 * MiB, 500 * MiB);
    auto refused = admit(plan({{Term::decode, 400000000, 0, 0}}), low);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.value.limit, 44 * MiB);
}

TEST(ExplodeBitmapMemory, T05EveryJBindingTermThroughAdmissionLimit)
{
    struct Case { MemorySample m; std::uint64_t expected; char const *term; };
    Memory ceiling{16 * GiB, 16 * GiB, 100 * MiB, true, 0};
    Memory available{16 * GiB, GiB, 100 * MiB, true, 0};
    Memory process{16 * GiB, 16 * GiB, 2500 * MiB, true, 0};
    Memory recovery{16 * GiB, 300 * MiB, 100 * MiB, true, 0};
    Case cases[] = {{ceiling, 1536 * MiB, "1.5 GiB ceiling"},
                    {available, 768 * MiB, "available RAM - recovery"},
                    {process, 316 * MiB, "process cap (3 GiB - E - recovery)"},
                    {recovery, 44 * MiB, "available RAM - recovery"}};
    for (auto &c : cases) {
        std::uint64_t direct = 0;
        ASSERT_TRUE(admissionLimit(c.m, direct).ok());
        auto a = admit(plan({{Term::decode, MiB, 0, 0}}), c.m);
        ASSERT_TRUE(a.ok()) << c.term;
        EXPECT_EQ(a.value.limit, direct);
        EXPECT_EQ(a.value.limit, c.expected) << c.term;
        EXPECT_STREQ(a.value.jBinding, c.term);
        EXPECT_STREQ(bindingJTerm(c.m), c.term);
    }
    // Synthetic A>R (refused by admit and recheck) isolates R/4 in the raw formula.
    Memory synthetic{2 * GiB, 8 * GiB, 100 * MiB, true, 0};
    std::uint64_t j = 0;
    ASSERT_TRUE(admissionLimit(synthetic, j).ok());
    EXPECT_EQ(j, 512 * MiB);
    EXPECT_STREQ(bindingJTerm(synthetic), "physical RAM / 4"); // unique minimum: no tie-break involved
}

// ---- T02 overlap accounting ----
TEST(ExplodeBitmapMemory, T02OverlapAddsAndSequentialDoesNot)
{
    auto s = roomy();
    auto seq = admit(plan({{Term::decode, 300 * MiB, 0, 0}, {Term::canonical, 300 * MiB, 1, 1}}), s);
    ASSERT_TRUE(seq.ok());
    EXPECT_EQ(seq.value.peak, 300 * MiB);
    auto both = admit(plan({{Term::decode, 300 * MiB, 0, 1}, {Term::canonical, 300 * MiB, 1, 1}}), s);
    ASSERT_TRUE(both.ok());
    EXPECT_EQ(both.value.peak, 600 * MiB);
    EXPECT_EQ(both.value.peakPhase, 1u);
    // Per-stage caps report each stage's own peak.
    EXPECT_EQ(both.value.stageCaps[static_cast<unsigned>(Stage::decode)], 300 * MiB);
    EXPECT_EQ(both.value.stageCaps[static_cast<unsigned>(Stage::canonical)], 300 * MiB);
}

TEST(ExplodeBitmapMemory, T02RecoveryRedoGenerationsAndQueuedResultsAllCount)
{
    // 512 MiB available leaves J=256 MiB: 4 x 64 MiB fits, one more byte does not.
    auto s = sample(4 * GiB, 512 * MiB, 500 * MiB);
    auto build = [&](std::uint64_t extra) {
        return admit(plan({{Term::recovery, 64 * MiB, 0, 2}, {Term::redo, 64 * MiB, 0, 2},
                           {Term::generation, 64 * MiB, 0, 2}, {Term::queued, 64 * MiB + extra, 0, 2}}), s);
    };
    EXPECT_TRUE(build(0).ok());
    auto refused = build(1);
    EXPECT_EQ(refused.outcome.status, Status::failed);
    EXPECT_EQ(refused.value.binding, Term::queued);
    EXPECT_STREQ(refused.outcome.diagnostic, "Plan exceeds RAM limit: queued results");
    // The same bytes in disjoint phases are sequential and fit.
    auto seq = admit(plan({{Term::recovery, 200 * MiB, 0, 0}, {Term::generation, 200 * MiB, 1, 1},
                           {Term::cache, 200 * MiB, 2, 2}}), s);
    EXPECT_TRUE(seq.ok());
    EXPECT_EQ(seq.value.peak, 200 * MiB);
}

TEST(ExplodeBitmapMemory, T02OverlapMatrix)
{
    // Three 100 MiB terms; lifetimes [first,last] for each of the matrix rows; expected peak in MiB.
    struct Row { unsigned a0, a1, b0, b1, c0, c1; std::uint64_t peak; };
    Row rows[] = {{0, 0, 1, 1, 2, 2, 100}, {0, 1, 1, 2, 2, 3, 200}, {0, 3, 1, 2, 2, 2, 300},
                  {0, 0, 0, 0, 0, 0, 300}, {0, 1, 2, 3, 1, 2, 200}};
    for (auto &r : rows) {
        auto a = admit(plan({{Term::decode, 100 * MiB, r.a0, r.a1}, {Term::canonical, 100 * MiB, r.b0, r.b1},
                             {Term::topology, 100 * MiB, r.c0, r.c1}}), roomy());
        ASSERT_TRUE(a.ok());
        EXPECT_EQ(a.value.peak, r.peak * MiB);
    }
}

TEST(ExplodeBitmapMemory, T02PlanValidationAndOverflow)
{
    ResourcePlan p;
    EXPECT_FALSE(p.add(Term::decode, 1, 2, 1));
    EXPECT_FALSE(p.add(Term::decode, 1, 0, maxPhases));
    EXPECT_FALSE(p.add(Term::count, 1, 0, 0));
    for (unsigned i = 0; i < maxReservations; ++i) EXPECT_TRUE(p.add(Term::cache, 1, 0, 0));
    EXPECT_FALSE(p.add(Term::cache, 1, 0, 0));
    auto big = plan({{Term::decode, ~std::uint64_t(0), 0, 0}, {Term::canonical, 2, 0, 0}});
    auto a = admit(big, roomy());
    EXPECT_EQ(a.outcome.status, Status::failed);
    EXPECT_STREQ(a.outcome.diagnostic, "Plan arithmetic overflow");
}

// ---- T04 caches and post-commit ----
TEST(ExplodeBitmapMemory, T04PostCommitLowerBound)
{
    std::uint64_t out = 7;
    ASSERT_TRUE(postCommitLowerBound(10, 20, 30, out));
    EXPECT_EQ(out, 90u); // 4B + Q + H
    std::uint64_t h = 0;
    ASSERT_TRUE(checkedBase64Length(3000, h)); // 22 + 4000
    ASSERT_TRUE(postCommitLowerBound(MiB, 3000, h, out));
    EXPECT_EQ(out, 4 * MiB + 3000 + 4022);
    out = 5;
    EXPECT_FALSE(postCommitLowerBound(~std::uint64_t(0) / 4 + 1, 0, 0, out));
    EXPECT_FALSE(postCommitLowerBound(0, ~std::uint64_t(0), 1, out));
    EXPECT_EQ(out, 5u); // preserved on refusal
}

TEST(ExplodeBitmapMemory, T04CachesAndPostCommitCountAgainstLimit)
{
    auto s = sample(4 * GiB, 512 * MiB, 500 * MiB); // J = 256 MiB
    std::uint64_t post = 0;
    ASSERT_TRUE(postCommitLowerBound(20 * MiB, 10 * MiB, 14 * MiB, post)); // 104 MiB
    auto build = [&](std::uint64_t cache) {
        // Build-time scratch is gone after commit; caches and post-commit bound stay.
        return admit(plan({{Term::crop, 20 * MiB, 0, 1}, {Term::cache, cache, 0, 2}, {Term::postCommit, post, 2, 2}}), s);
    };
    auto fits = build(152 * MiB);
    ASSERT_TRUE(fits.ok());
    EXPECT_EQ(fits.value.peak, 256 * MiB);
    EXPECT_EQ(fits.value.peakPhase, 2u);
    auto over = build(152 * MiB + 1);
    EXPECT_EQ(over.outcome.status, Status::failed);
    EXPECT_EQ(over.value.binding, Term::cache);
    EXPECT_STREQ(over.outcome.diagnostic, "Plan exceeds RAM limit: caches");
    // Overlapping crop boxes: B much larger than P still counts fully as 4B decoded caches.
    ASSERT_TRUE(postCommitLowerBound(65 * MiB, 0, 0, post));
    auto amp = admit(plan({{Term::postCommit, post, 0, 0}}), s);
    EXPECT_EQ(amp.value.binding, Term::postCommit);
    EXPECT_FALSE(amp.ok());
}

TEST(ExplodeBitmapMemory, T04StageCapsRefuseEarly)
{
    auto s = roomy();
    EXPECT_TRUE(admit(plan({{Term::topology, 256 * MiB, 0, 0}}), s).ok());
    auto topo = admit(plan({{Term::topology, 256 * MiB + 1, 0, 0}}), s);
    EXPECT_EQ(topo.outcome.status, Status::failed);
    EXPECT_STREQ(topo.outcome.diagnostic, "Plan exceeds fixed cap: topology 256 MiB");
    EXPECT_EQ(topo.value.binding, Term::topology);
    auto prev = admit(plan({{Term::preview, 4 * MiB + 1, 0, 0}}), s);
    EXPECT_STREQ(prev.outcome.diagnostic, "Plan exceeds fixed cap: preview 4 MiB");
    auto hr = admit(plan({{Term::href, 512 * MiB + 1, 0, 0}}), s);
    EXPECT_STREQ(hr.outcome.diagnostic, "Plan exceeds fixed cap: href 512 MiB");
    EXPECT_TRUE(admit(plan({{Term::preview, 4 * MiB, 0, 0}}), s).ok());
    // crop + encoder combined 128 MiB only when alive together.
    EXPECT_TRUE(admit(plan({{Term::crop, 64 * MiB, 0, 0}, {Term::encoder, 64 * MiB, 0, 0}}), s).ok());
    auto bad = admit(plan({{Term::crop, 64 * MiB, 0, 0}, {Term::encoder, 64 * MiB + 1, 0, 0}}), s);
    EXPECT_EQ(bad.outcome.status, Status::failed);
    EXPECT_EQ(bad.value.binding, Term::encoder);
    EXPECT_STREQ(bad.outcome.diagnostic, "Plan exceeds fixed cap: crops plus encoder 128 MiB");
    EXPECT_TRUE(admit(plan({{Term::crop, 100 * MiB, 0, 0}, {Term::encoder, 100 * MiB, 1, 1}}), s).ok());
}

// ---- round 2 ----
TEST(ExplodeBitmapMemory, R2MandatoryTermsRequired)
{
    auto s = roomy();
    struct Case { Term skip; char const *text; };
    for (auto const &c : {Case{Term::recovery, "Plan lacks mandatory term at peak: recovery"},
                          Case{Term::history, "Plan lacks mandatory term at peak: Undo history"},
                          Case{Term::redo, "Plan lacks mandatory term at peak: Redo history"},
                          Case{Term::queued, "Plan lacks mandatory term at peak: queued results"}}) {
        ResourcePlan p;
        for (Term t : {Term::recovery, Term::history, Term::redo, Term::queued})
            if (t != c.skip) ASSERT_TRUE(p.add(t, 0, 0, 0));
        ASSERT_TRUE(p.add(Term::decode, MiB, 0, 0));
        auto a = admit(p, s);
        EXPECT_EQ(a.outcome.status, Status::failed);
        EXPECT_STREQ(a.outcome.diagnostic, c.text);
        ASSERT_TRUE(p.add(c.skip, 0, 0, 0)); // explicit zero satisfies it
        EXPECT_TRUE(admit(p, s).ok());
    }
}

TEST(ExplodeBitmapMemory, R3MandatoryTermsMustCoverThePeakPhase)
{
    auto build = [](unsigned historyLast) {
        ResourcePlan p;
        EXPECT_TRUE(p.add(Term::recovery, 0, 0, 3));
        EXPECT_TRUE(p.add(Term::history, 0, 0, historyLast)); // Undo payload
        EXPECT_TRUE(p.add(Term::redo, 0, 0, 3));
        EXPECT_TRUE(p.add(Term::queued, 0, 0, 3));
        EXPECT_TRUE(p.add(Term::cache, 100 * MiB, 3, 3)); // peak is phase 3
        return p;
    };
    auto bad = admit(build(0), roomy());
    EXPECT_EQ(bad.outcome.status, Status::failed);
    EXPECT_STREQ(bad.outcome.diagnostic, "Plan lacks mandatory term at peak: Undo history");
    auto good = admit(build(3), roomy());
    ASSERT_TRUE(good.ok());
    EXPECT_EQ(good.value.peakPhase, 3u);
}

TEST(ExplodeBitmapMemory, R3CeilingAndLedgerFollowRecheck)
{
    auto s = sample(16 * GiB, 16 * GiB, 100 * MiB); // J = 1536 MiB
    Budget small(100 * MiB);
    ASSERT_TRUE(small.recheck(s).ok());
    EXPECT_EQ(small.limit(), 100 * MiB);
    AdmitBudget view{100 * MiB, 0};
    auto fits = admit(plan({{Term::decode, 100 * MiB, 0, 0}}), s, nullptr, view);
    ASSERT_TRUE(fits.ok());
    EXPECT_EQ(fits.value.limit, small.limit());
    EXPECT_STREQ(fits.value.jBinding, "Budget ceiling");
    EXPECT_EQ(admit(plan({{Term::decode, 100 * MiB + 1, 0, 0}}), s, nullptr, view).outcome.status, Status::failed);
    EXPECT_TRUE(admit(plan({{Term::decode, 100 * MiB + 1, 0, 0}}), s).ok()); // no ceiling: J applies
    // Stale resident: unanchored calls must have resident 0; anchored ones may not exceed the ledger.
    Memory first = admit(plan({{Term::decode, MiB, 0, 0}}), sample(4 * GiB, GiB, 500 * MiB)).value.effective;
    Memory now{4 * GiB, GiB - 200 * MiB, 700 * MiB, true, 200 * MiB};
    auto unanchored = admit(plan({{Term::decode, MiB, 0, 0}}), now);
    EXPECT_EQ(unanchored.outcome.status, Status::failed);
    EXPECT_STREQ(unanchored.outcome.diagnostic, "Resident RAM needs an operation-start anchor");
    auto stale = admit(plan({{Term::decode, MiB, 0, 0}}), now, &first, AdmitBudget{~std::uint64_t(0), 100 * MiB});
    EXPECT_EQ(stale.outcome.status, Status::failed);
    EXPECT_STREQ(stale.outcome.diagnostic, "Invalid resident RAM measurement");
    EXPECT_TRUE(admit(plan({{Term::decode, MiB, 0, 0}}), now, &first, AdmitBudget{~std::uint64_t(0), 200 * MiB}).ok());
}

TEST(ExplodeBitmapMemory, R2ResidentGenerationIsNotCountedTwice)
{
    // Operation start: A0 = 512 MiB, E0 = 500 MiB (J = 256 MiB). Then a 200 MiB generation is built:
    // RSS grows by 200 and available shrinks by 200, with resident = 200.
    auto p = [] { return plan({{Term::generation, 250 * MiB, 0, 0}}); };
    auto first = admit(p(), sample(4 * GiB, 512 * MiB, 500 * MiB));
    ASSERT_TRUE(first.ok());
    Memory now{4 * GiB, 312 * MiB, 700 * MiB, true, 200 * MiB};
    auto restored = admit(p(), now, &first.value.effective, AdmitBudget{~std::uint64_t(0), 200 * MiB});
    ASSERT_TRUE(restored.ok());
    EXPECT_EQ(restored.value.limit, 256 * MiB);
    now.resident = 0; // forgetting resident counts the old generation twice: false refusal
    auto doubled = admit(p(), now, &first.value.effective, AdmitBudget{~std::uint64_t(0), 200 * MiB});
    EXPECT_EQ(doubled.outcome.status, Status::failed);
    EXPECT_LT(doubled.value.limit, 250 * MiB);
    // Budget::recheck with the same sample and the same live bytes reaches the same limit.
    Budget budget(1536 * MiB);
    ASSERT_TRUE(budget.recheck(sample(4 * GiB, 512 * MiB, 500 * MiB)).ok());
    Budget::Token live;
    ASSERT_TRUE(budget.acquire(Stage::composition, 200 * MiB, live).ok());
    now.resident = 200 * MiB;
    ASSERT_TRUE(budget.recheck(now).ok());
    EXPECT_EQ(budget.limit(), restored.value.limit);
}

TEST(ExplodeBitmapMemory, R2NeverOptimisticAboveOperationStart)
{
    auto first = admit(plan({{Term::decode, 100 * MiB, 0, 0}}), sample(16 * GiB, 512 * MiB, 500 * MiB));
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value.limit, 256 * MiB);
    auto later = sample(16 * GiB, 8 * GiB, 500 * MiB); // pressure went away after start
    auto big = plan({{Term::decode, 300 * MiB, 0, 0}});
    EXPECT_TRUE(admit(big, later).ok());                      // unanchored would approve
    auto anchored = admit(big, later, &first.value.effective);
    EXPECT_EQ(anchored.outcome.status, Status::failed);       // anchored at A0 like the Budget
    EXPECT_EQ(anchored.value.limit, 256 * MiB);
    Budget budget(1536 * MiB);
    ASSERT_TRUE(budget.recheck(sample(16 * GiB, 512 * MiB, 500 * MiB)).ok());
    ASSERT_TRUE(budget.recheck(later).ok());
    EXPECT_EQ(budget.limit(), anchored.value.limit);
    // Invalid resident values are refused exactly like recheck.
    Memory bad{4 * GiB, GiB, 500 * MiB, true, 500 * MiB};
    EXPECT_EQ(admit(big, bad).outcome.status, Status::failed);
    bad = {4 * GiB, 4 * GiB, 500 * MiB, true, 100 * MiB};
    EXPECT_EQ(admit(big, bad, &first.value.effective, AdmitBudget{~std::uint64_t(0), GiB}).outcome.status,
              Status::failed); // A + r > R
}

TEST(ExplodeBitmapMemory, R2MacMappingConservative)
{
    VmStats v{8 * GiB, 100, 5000, 1000, 16384, 600 * MiB, 1};
    RawMemory raw;
    ASSERT_TRUE(fromVmStats(v, raw));
    EXPECT_EQ(raw.available, (100 + 1000) * 16384u); // inactive capped by file-backed pages
    EXPECT_EQ(raw.physical, 8 * GiB);
    EXPECT_EQ(raw.footprint, 600 * MiB);
    v.inactivePages = 10; // fewer inactive than external
    ASSERT_TRUE(fromVmStats(v, raw));
    EXPECT_EQ(raw.available, 110 * 16384u);
    v.inactivePages = 5000; v.pressureLevel = 2; // warn halves
    ASSERT_TRUE(fromVmStats(v, raw));
    EXPECT_EQ(raw.available, 1100 * 16384u / 2);
    v.pressureLevel = 4; EXPECT_FALSE(fromVmStats(v, raw)); // critical refuses
    v.pressureLevel = 0; EXPECT_FALSE(fromVmStats(v, raw)); // unknown refuses
    v.pressureLevel = 1; v.pageSize = 0; EXPECT_FALSE(fromVmStats(v, raw));
    v.pageSize = ~std::uint64_t(0); EXPECT_FALSE(fromVmStats(v, raw)); // overflow
    // Speculative pages are already in free; they must not count again through external.
    VmStats spec{8 * GiB, 100, 5000, 1000, 16384, 600 * MiB, 1, 400};
    ASSERT_TRUE(fromVmStats(spec, raw));
    EXPECT_EQ(raw.available, (100 + 600) * 16384u);
    EXPECT_LT(raw.available, (spec.freePages + spec.inactivePages) * spec.pageSize); // conservative
    spec.speculativePages = 2000; // more speculative than external: no file-backed credit at all
    ASSERT_TRUE(fromVmStats(spec, raw));
    EXPECT_EQ(raw.available, 100 * 16384u);
    // Pressured values reach admission as a small A and refuse.
    VmStats tight{8 * GiB, 5000, 0, 0, 16384, GiB, 1}; // ~78 MiB available
    ASSERT_TRUE(fromVmStats(tight, raw));
    auto s = sampleMemory(FakeProbe(raw.physical, raw.available, raw.footprint));
    ASSERT_TRUE(s.ok());
    EXPECT_FALSE(admit(plan({{Term::decode, MiB, 0, 0}}), s.value).ok());
}

TEST(ExplodeBitmapMemory, R2WindowsMappingUsesCommitLimit)
{
    WinStatus w{4 * GiB, 2 * GiB, 600 * MiB, 100 * MiB, 700 * MiB, false};
    RawMemory raw;
    ASSERT_TRUE(fromMemoryStatus(w, raw));
    EXPECT_EQ(raw.available, 600 * MiB); // page-file headroom binds
    EXPECT_EQ(raw.physical, 4 * GiB);
    EXPECT_EQ(raw.footprint, 700 * MiB);
    w.is32Bit = true;
    ASSERT_TRUE(fromMemoryStatus(w, raw));
    EXPECT_EQ(raw.available, 100 * MiB); // address space binds on 32-bit
    w = {4 * GiB, 500 * MiB, 3 * GiB, 0, 700 * MiB, false};
    ASSERT_TRUE(fromMemoryStatus(w, raw));
    EXPECT_EQ(raw.available, 500 * MiB);
}

TEST(ExplodeBitmapMemory, R2TermCapsAndNames)
{
    auto a = admit(plan({{Term::history, 10 * MiB, 0, 1}, {Term::redo, 20 * MiB, 0, 1},
                         {Term::generation, 30 * MiB, 0, 0}, {Term::cache, 40 * MiB, 1, 1}}), roomy());
    ASSERT_TRUE(a.ok());
    auto c = [&](Term t) { return a.value.termCaps[static_cast<unsigned>(t)]; };
    EXPECT_EQ(c(Term::history), 10 * MiB);
    EXPECT_EQ(c(Term::redo), 20 * MiB);
    EXPECT_EQ(c(Term::generation), 30 * MiB);
    EXPECT_EQ(c(Term::cache), 40 * MiB);
    EXPECT_EQ(c(Term::recovery), 0u);
    EXPECT_STREQ(termName(Term::redo), "Redo history");
    EXPECT_STREQ(termName(Term::count), "invalid term"); // no out-of-bounds read
}

// ---- native probe ----
TEST(ExplodeBitmapMemory, NativeSmokePlausibility)
{
    auto r = sampleMemory();
#if defined(__APPLE__) || defined(_WIN32)
    ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_TRUE(r.value.measured);
    EXPECT_GT(r.value.physical, 0u);
    EXPECT_GT(r.value.available, 0u);
    EXPECT_LE(r.value.available, r.value.physical);
    EXPECT_GT(r.value.baseline, 0u);
    EXPECT_LE(r.value.baseline, r.value.physical);
#else
    EXPECT_EQ(r.outcome.status, Status::unavailable);
    EXPECT_FALSE(r.value.measured);
#endif
}

TEST(ExplodeBitmapMemory, P2FullDisplayAndRetiredReaderOverlapAreAdmittedBeforeDispatch)
{
    constexpr std::uint64_t bytes = 100000000;
    auto base = admit(PanelPreparation::resources(5000, 5000, MiB, 0, {}, false), roomy());
    auto one = admit(PanelPreparation::resources(5000, 5000, MiB, 0), roomy());
    auto overlap = admit(PanelPreparation::resources(5000, 5000, MiB, bytes), roomy());
    ASSERT_TRUE(base.ok()) << base.outcome.diagnostic;
    ASSERT_TRUE(one.ok()) << one.outcome.diagnostic;
    ASSERT_TRUE(overlap.ok()) << overlap.outcome.diagnostic;
    EXPECT_EQ(one.value.peak, base.value.peak + bytes);
    EXPECT_EQ(overlap.value.peak, one.value.peak + bytes);
    EXPECT_EQ(one.value.termCaps[unsigned(Term::cache)], bytes);
    auto constrained = sample(16*GiB, base.value.peak + 256*MiB + bytes/2, 100*MiB);
    EXPECT_TRUE(admit(PanelPreparation::resources(5000,5000,MiB,0,{},false), constrained).ok());
    EXPECT_FALSE(admit(PanelPreparation::resources(5000,5000,MiB,0), constrained).ok());
}

TEST(ExplodeBitmapMemory, P3OutlineReservationIsOptionalBoundedAndOwnedBeforeDispatch)
{
    auto budget=std::make_shared<Budget>(Budget::FixedLimitForTest{},128*MiB);
    auto reservation=PanelPreparation::reserveOutlines(budget);
    ASSERT_TRUE(reservation.ok()) << reservation.outcome.diagnostic;
    EXPECT_EQ(budget->reserved(Stage::prepared),outlineByteLimit);
    EXPECT_LE(reservation.value->ledger->limit(),outlineByteLimit);
    Budget::Token pressure;
    ASSERT_TRUE(budget->acquire(Stage::prepared,64*MiB,pressure).ok());
    auto refused=PanelPreparation::reserveOutlines(budget);
    EXPECT_FALSE(refused.ok()); EXPECT_FALSE(refused.value);
    EXPECT_EQ(budget->reserved(),128*MiB);
    reservation.value.reset(); EXPECT_EQ(budget->reserved(),64*MiB);
    EXPECT_FALSE(PanelPreparation::reserveOutlines(budget,0).ok());
    pressure.release(); EXPECT_EQ(budget->reserved(),0u);
}
