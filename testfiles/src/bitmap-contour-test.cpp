// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "util/bitmap-contour.h"
#include "util/bitmap-island-specks.h"
#include <png.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
using namespace Inkscape::Bitmap;
namespace {
struct Image {
    unsigned w, h;
    std::vector<std::uint8_t> data;
    Image(unsigned x, unsigned y) : w(x), h(y), data(std::size_t(x) * y * 4) {}
    void put(unsigned x, unsigned y, unsigned a = 255) { data[(std::size_t(y) * w + x) * 4 + 3] = a; }
    void box(unsigned x, unsigned y, unsigned ex, unsigned ey, unsigned a = 255) {
        for (auto j = y; j < ey; ++j) for (auto i = x; i < ex; ++i) put(i, j, a);
    }
    RgbaView view() const { return {data.data(), data.size(), std::uint64_t(w) * 4, w, h}; }
};
struct Fixture {
    Budget budget{Budget::FixedLimitForTest{}, 512 * MiB};
    JobWork work{25000000};
    Result<Regions> regions;
    Result<Partition> partition;
    void prepare(Image const &im) {
        regions = label(im.view(), alphaLut(0, 0), budget, work);
        ASSERT_TRUE(regions.ok()) << regions.outcome.diagnostic;
        partition = enclose(regions.value, budget, work);
        ASSERT_TRUE(partition.ok()) << partition.outcome.diagnostic;
    }
    Result<ContourSet> trace(Image const &im, Stop stop = {}, ContourOptions o = {}) {
        return traceContours(im.view(), partition.value, budget, work, stop, o);
    }
};
std::vector<float> squareField() {
    std::vector<float> f(14 * 14, -.5f);
    for (unsigned y = 2; y < 12; ++y) for (unsigned x = 2; x < 12; ++x) f[y * 14 + x] = .5f;
    return f;
}
double perimeter(ContourPoint const *p, ContourRing const &r) {
    double sum = 0;
    for (auto i = r.begin, j = r.end - 1; i < r.end; j = i++) sum += std::hypot(p[i].x - p[j].x, p[i].y - p[j].y);
    return sum;
}
void empty(Result<RingSet> const &r) {
    EXPECT_FALSE(r.ok()); EXPECT_EQ(r.value.pointCount, 0u); EXPECT_EQ(r.value.ringCount, 0u);
    EXPECT_EQ(r.value.points(), nullptr); EXPECT_EQ(r.value.rings(), nullptr);
}
void empty(Result<ContourSet> const &r) {
    EXPECT_FALSE(r.ok()); EXPECT_EQ(r.value.pointCount, 0u); EXPECT_EQ(r.value.ringCount, 0u);
    EXPECT_EQ(r.value.pieceCount, 0u); EXPECT_EQ(r.value.points(), nullptr);
    EXPECT_EQ(r.value.rings(), nullptr); EXPECT_EQ(r.value.pieces(), nullptr);
}
}
TEST(BitmapContour, OpaqueSquareOracle) {
    Image im(10, 10); im.box(0, 0, 10, 10); Fixture f; f.prepare(im);
    auto r = f.trace(im); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    ASSERT_EQ(r.value.ringCount, 1u); ASSERT_EQ(r.value.pieceCount, 1u);
    auto ring = r.value.rings()[0]; EXPECT_NEAR(ring.area, 99.5, 1e-9);
    EXPECT_NEAR(perimeter(r.value.points(), ring), 36 + 2 * std::sqrt(2.), 1e-9);
    EXPECT_EQ(ring.minX, 0); EXPECT_EQ(ring.maxX, 10); EXPECT_EQ(ring.minY, 0); EXPECT_EQ(ring.maxY, 10);
    EXPECT_EQ(ring.parent, UINT32_MAX); EXPECT_EQ(ring.depth, 0u); EXPECT_FALSE(r.value.pieces()[0].noContour);
}
TEST(BitmapContour, SupersampledDisk) {
    Image im(90, 90);
    for (unsigned y = 0; y < im.h; ++y) for (unsigned x = 0; x < im.w; ++x) {
        unsigned covered = 0;
        for (unsigned sy = 0; sy < 16; ++sy) for (unsigned sx = 0; sx < 16; ++sx) {
            auto dx = x + (sx + .5) / 16 - 45, dy = y + (sy + .5) / 16 - 45;
            covered += dx * dx + dy * dy <= 40 * 40;
        }
        im.put(x, y, std::lround(255. * covered / 256));
    }
    Fixture f; f.prepare(im); auto r = f.trace(im); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    ASSERT_EQ(r.value.ringCount, 1u); auto area = std::acos(-1.) * 1600;
    EXPECT_NEAR(r.value.rings()[0].area, area, .005 * area);
}
TEST(BitmapContour, DonutDotAndGlobalParents) {
    Image im(45, 22); im.box(1, 1, 21, 21); im.box(5, 5, 17, 17, 0); im.box(9, 9, 13, 13);
    im.box(24, 1, 44, 21); im.box(28, 5, 40, 17, 0); im.box(32, 9, 36, 13);
    Fixture f; f.prepare(im); ASSERT_EQ(f.partition.value.pieceCount, 2u);
    auto r = f.trace(im); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.ringCount, 6u);
    for (unsigned p = 0; p < 2; ++p) {
        auto pc = r.value.pieces()[p]; ASSERT_EQ(pc.ringEnd - pc.ringBegin, 3u);
        for (unsigned k = 0; k < 3; ++k) {
            auto ring = r.value.rings()[pc.ringBegin + k];
            EXPECT_EQ(ring.depth, k); EXPECT_EQ(ring.parent, k ? pc.ringBegin + k - 1 : UINT32_MAX);
            EXPECT_EQ(ring.area > 0, k != 1);
            double shoelace=0;
            for (auto i=ring.begin,j=ring.end-1;i<ring.end;j=i++) {
                auto a=r.value.points()[j],b=r.value.points()[i];
                shoelace+=(a.x*b.y-b.x*a.y)*.5;
            }
            EXPECT_EQ(shoelace>0,ring.depth%2==0);
            EXPECT_NEAR(shoelace,ring.area,1e-9);
        }
    }
}
TEST(BitmapContour, SaddlesUseBilinearDeterminant) {
    Budget b(Budget::FixedLimitForTest{}, 32 * MiB); JobWork work(1000);
    // Padded central cell: both diagonal masks, both determinant signs, and
    // exact ties. Count closed components, independently of vertex ordering.
    for (bool reverse : {false, true}) for (float positive : {.25f, 1.f, 2.f}) {
        std::vector<float> field(16, -1);
        field[reverse ? 6 : 5] = positive; field[reverse ? 9 : 10] = positive;
        auto r = traceLevel({field.data(), 4, 4, 0, 0}, b, work);
        ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
        EXPECT_EQ(r.value.ringCount, positive < 1 ? 2u : 1u) << reverse << ' ' << positive;
    }
    // Unequal magnitudes distinguish determinant from average-centre heuristics.
    std::vector<float> field(16, -1); field[5] = 8; field[10] = .1f; field[6] = -2; field[9] = -1;
    auto r = traceLevel({field.data(), 4, 4, 0, 0}, b, work);
    ASSERT_TRUE(r.ok()); EXPECT_EQ(r.value.ringCount, 2u);
}
TEST(BitmapContour, ExcludesOtherPieceInBoundingBox) {
    Image im(20, 20); im.box(1, 1, 18, 3); im.box(1, 3, 3, 18); im.box(1, 16, 18, 18); im.box(8, 8, 12, 12);
    Fixture f; f.prepare(im); ASSERT_EQ(f.partition.value.pieceCount, 2u);
    auto r = f.trace(im); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.ringCount, 2u);
    EXPECT_EQ(r.value.pieces()[0].ringEnd, 1u); EXPECT_EQ(r.value.pieces()[1].ringBegin, 1u);
    EXPECT_NEAR(r.value.rings()[1].area, 15.5, 1e-9);
}
TEST(BitmapContour, OwnedTransparentHole) {
    Image im(20, 20); im.box(1, 1, 19, 19); im.box(6, 6, 14, 14, 0);
    Fixture f; f.prepare(im); bool ownedHole = false;
    for (unsigned k = 0; k < f.partition.value.runCount; ++k) {
        auto run = f.partition.value.run(k); ownedHole |= !run.foreground && run.piece == 0;
    }
    ASSERT_TRUE(ownedHole); auto r = f.trace(im); ASSERT_TRUE(r.ok()); ASSERT_EQ(r.value.ringCount, 2u);
    EXPECT_NEAR(r.value.rings()[1].area, -63.5, 1e-9); EXPECT_EQ(r.value.rings()[1].parent, 0u);
}
TEST(BitmapContour, SubthresholdOnlySuppressesItsPiece) {
    Image im(30, 10); im.box(0, 0, 10, 10, 127); im.box(20, 0, 30, 10, 128);
    Fixture f; f.prepare(im); ASSERT_EQ(f.partition.value.pieceCount, 2u);
    auto r = f.trace(im); ASSERT_TRUE(r.ok()); ASSERT_EQ(r.value.pieceCount, 2u);
    EXPECT_TRUE(r.value.pieces()[0].noContour); EXPECT_EQ(r.value.pieces()[0].ringBegin, r.value.pieces()[0].ringEnd);
    EXPECT_FALSE(r.value.pieces()[1].noContour); EXPECT_EQ(r.value.ringCount, 1u);
}
TEST(BitmapContour, DeterminismAndMoveOwnership) {
    auto field = squareField(); Budget b(Budget::FixedLimitForTest{}, 32 * MiB); JobWork w(1000);
    auto a = traceLevel({field.data(), 14, 14, -2, -2}, b, w);
    auto c = traceLevel({field.data(), 14, 14, -2, -2}, b, w);
    ASSERT_TRUE(a.ok()); ASSERT_TRUE(c.ok()); ASSERT_EQ(a.value.pointCount, c.value.pointCount);
    EXPECT_EQ(std::memcmp(a.value.points(), c.value.points(), a.value.pointCount * sizeof(ContourPoint)), 0);
    RingSet moved = std::move(a.value); EXPECT_EQ(a.value.pointCount, 0u); EXPECT_EQ(a.value.points(), nullptr);
    a.value = std::move(moved); EXPECT_EQ(moved.ringCount, 0u);
}
TEST(BitmapContour, StopAtEveryPhaseRollsBack) {
    Image im(10, 10); im.box(0, 0, 10, 10); Fixture f; f.prepare(im); auto baseline = f.budget.reserved();
    struct Hook { ContourPhase phase; std::shared_ptr<std::atomic<bool>> flag; };
    for (auto phase : {ContourPhase::field, ContourPhase::trace, ContourPhase::stitch, ContourPhase::hierarchy, ContourPhase::output, ContourPhase::allocation}) {
        Hook h{phase, std::make_shared<std::atomic<bool>>(false)}; ContourOptions o;
        o.observerData = &h; o.observe = [](ContourPhase p, void *v) noexcept {
            auto &h = *static_cast<Hook *>(v); if (p == h.phase) h.flag->store(true);
        };
        auto r = f.trace(im, Stop(h.flag), o); empty(r); EXPECT_EQ(r.outcome.status, Status::canceled);
        EXPECT_EQ(f.budget.reserved(), baseline);
    }
    auto flag = std::make_shared<std::atomic<bool>>(true); auto r = f.trace(im, Stop(flag));
    empty(r); EXPECT_EQ(r.outcome.status, Status::canceled); EXPECT_EQ(f.budget.reserved(), baseline);
}
TEST(BitmapContour, VisitCapRollsBack) {
    auto field = squareField(); Budget b(Budget::FixedLimitForTest{}, 32 * MiB); JobWork w(0);
    ASSERT_TRUE(w.advance(w.limit() - 10).ok()); auto r = traceLevel({field.data(), 14, 14, 0, 0}, b, w);
    empty(r); EXPECT_EQ(r.outcome.status, Status::failed); EXPECT_EQ(b.reserved(), 0u);
}
TEST(BitmapContour, BudgetRefusalRollsBack) {
    auto field = squareField();
    for (auto bytes : {0u, 256u, 2048u, 4096u}) {
        Budget b(Budget::FixedLimitForTest{}, bytes); JobWork w(1000);
        auto r = traceLevel({field.data(), 14, 14, 0, 0}, b, w); empty(r);
        EXPECT_EQ(r.outcome.status, Status::failed); EXPECT_EQ(b.reserved(), 0u);
    }
}
TEST(BitmapContour, AllocationFaultEveryAllocationRollsBack) {
    Image im(30, 10); im.box(0, 0, 10, 10); im.box(20, 0, 30, 10); Fixture f; f.prepare(im);
    auto baseline = f.budget.reserved(); AllocationFault count; ContourOptions o; o.fault = &count;
    { auto r = f.trace(im, {}, o); ASSERT_TRUE(r.ok()); }
    ASSERT_GT(count.attempts, 10u);
    for (std::uint64_t k = 1; k <= count.attempts; ++k) {
        AllocationFault fault{k}; o.fault = &fault;
        auto r = f.trace(im, {}, o); empty(r); EXPECT_EQ(r.outcome.status, Status::failed) << k;
        EXPECT_EQ(f.budget.reserved(), baseline) << k;
    }
}
TEST(BitmapContour, LowerOnlyCapsAndNoPartialPieces) {
    Image im(30, 10); im.box(0, 0, 10, 10); im.box(20, 0, 30, 10); Fixture f; f.prepare(im);
    auto baseline = f.budget.reserved(); ContourOptions o; o.maxPoints = 60;
    { auto r = f.trace(im, {}, o); empty(r);
      EXPECT_STREQ(r.outcome.diagnostic, "Contour point cap exceeded"); EXPECT_EQ(f.budget.reserved(), baseline); }
    o.maxPoints = UINT32_MAX; o.maxRings = 1;
    { auto r = f.trace(im, {}, o); empty(r); EXPECT_EQ(f.budget.reserved(), baseline); }
    o.maxRings = UINT32_MAX; auto r = f.trace(im, {}, o); ASSERT_TRUE(r.ok()); EXPECT_EQ(r.value.pointCount, 80u);
}
TEST(BitmapContour, RawPointCapDefaultsAndLoweredPieceRefusal) {
    ContourOptions o; EXPECT_EQ(o.maxPoints, 8000000u); EXPECT_EQ(o.maxRings, 50000u);
    auto field = squareField(); Budget budget(Budget::FixedLimitForTest{}, 32 * MiB); JobWork work(1000);
    // Lower the raw point cap to test refusal without a million-point fixture.
    o.maxPoints = 39;
    {
        auto r = traceLevel({field.data(), 14, 14, 0, 0}, budget, work, {}, o);
        empty(r); EXPECT_EQ(r.outcome.status, Status::failed);
        EXPECT_STREQ(r.outcome.diagnostic, "Contour point cap exceeded");
    }
    EXPECT_EQ(budget.reserved(), 0u);
    for (auto cap : {40u, 2000000u, 8000000u, UINT32_MAX}) {
        o.maxPoints = cap;
        {
            auto r = traceLevel({field.data(), 14, 14, 0, 0}, budget, work, {}, o);
            ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; EXPECT_EQ(r.value.pointCount, 40u);
        }
        EXPECT_EQ(budget.reserved(), 0u);
    }
}
TEST(BitmapContour, RawPointsBeyondSerializedOutputLimit) {
    Image im(1000,220); im.box(0,0,1000,1);
    for (unsigned x=0;x<1000;x+=2) im.box(x,1,x+1,220);
    Fixture f; f.prepare(im); ASSERT_EQ(f.partition.value.pieceCount,1u);
    auto r=f.trace(im); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_GE(r.value.pointCount,210000u); EXPECT_LT(r.value.pointCount,2000000u);
    EXPECT_EQ(r.value.ringCount,1u);
}
TEST(BitmapContour, CollapsedZeroRingIsDropped) {
    Budget b(Budget::FixedLimitForTest{},32*MiB); JobWork w(1000);
    float field[9]={-1,-1,-1,-1,0,-1,-1,-1,-1};
    auto r=traceLevel({field,3,3,0,0},b,w); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_EQ(r.value.ringCount,0u); EXPECT_EQ(r.value.pointCount,0u);
}
TEST(BitmapContour, EmptyPartitionAndAllNegativeField) {
    Image im(10, 10); Fixture f; f.prepare(im); auto r = f.trace(im);
    ASSERT_TRUE(r.ok()); EXPECT_EQ(r.value.pieceCount, 0u); EXPECT_EQ(r.value.pointCount, 0u);
    float values[4] = {-1, -1, -1, -1}; auto level = traceLevel({values, 2, 2, 0, 0}, f.budget, f.work);
    ASSERT_TRUE(level.ok()); EXPECT_EQ(level.value.ringCount, 0u); EXPECT_EQ(level.value.points(), nullptr);
}
TEST(BitmapContour, InvalidAndOpenFieldsRefused) {
    Budget b(Budget::FixedLimitForTest{}, 32 * MiB); JobWork w(1000);
    empty(traceLevel({}, b, w));
    float field[4] = {1, -1, 1, -1}; empty(traceLevel({field, 2, 2, 0, 0}, b, w));
    field[0] = std::numeric_limits<float>::quiet_NaN(); empty(traceLevel({field, 2, 2, 0, 0}, b, w));
    EXPECT_EQ(b.reserved(), 0u);
}
TEST(BitmapContour, ExactZeroVerticesAreNotRepeated) {
    Budget b(Budget::FixedLimitForTest{}, 32 * MiB); JobWork w(1000);
    std::vector<float> field(25, -1);
    for (unsigned y = 1; y <= 3; ++y) for (unsigned x = 1; x <= 3; ++x) field[y * 5 + x] = 0;
    field[12] = 1;
    auto r = traceLevel({field.data(), 5, 5, 0, 0}, b, w); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    ASSERT_EQ(r.value.ringCount, 1u); EXPECT_EQ(r.value.rings()[0].area, 4);
    auto ring = r.value.rings()[0];
    for (auto i = ring.begin, j = ring.end - 1; i < ring.end; j = i++) {
        auto a = r.value.points()[i], c = r.value.points()[j]; EXPECT_TRUE(a.x != c.x || a.y != c.y);
    }
}
TEST(BitmapContour, TinyRingAndTranslatedField) {
    Budget b(Budget::FixedLimitForTest{}, 32 * MiB); JobWork w(1000);
    float field[9] = {-1,-1,-1,-1,1e-6f,-1,-1,-1,-1};
    auto r = traceLevel({field, 3, 3, 0, 0}, b, w); ASSERT_TRUE(r.ok()); ASSERT_EQ(r.value.ringCount, 1u);
    EXPECT_GT(r.value.rings()[0].area, 0); EXPECT_LT(r.value.rings()[0].area, 1e-10);
    auto square = squareField(); auto translated = traceLevel({square.data(), 14, 14, 1000000, -1000000}, b, w);
    ASSERT_TRUE(translated.ok()); EXPECT_NEAR(translated.value.rings()[0].area, 99.5, 1e-9);
}
TEST(BitmapContour, MeasureLimits5000) {
    auto root = std::getenv("VACARDS_EB_B1_MEASURE_ROOT");
    if (!root) GTEST_SKIP() << "Opt in with VACARDS_EB_B1_MEASURE_ROOT=.../limits-5000";
    for (auto suffix : {"/blobs-150-5000x5000.png", "/sparse/blobs-150-5000x5000.png"}) {
        auto path = std::string(root) + suffix;
        png_image png{}; png.version = PNG_IMAGE_VERSION;
        ASSERT_TRUE(png_image_begin_read_from_file(&png, path.c_str())) << png.message;
        png.format = PNG_FORMAT_RGBA; Image im(png.width, png.height);
        auto decoded = png_image_finish_read(&png, nullptr, im.data.data(), 0, nullptr);
        auto diagnostic = std::string(png.message); png_image_free(&png); ASSERT_TRUE(decoded) << diagnostic;
        Fixture f;
        // Keep the decoded RGBA reservation in the same ledger as all topology.
        Budget::Token input; ASSERT_TRUE(f.budget.acquire(Stage::input, im.data.size(), input).ok());
        f.prepare(im); ASSERT_EQ(f.partition.value.pieceCount, 150u);
        auto metric = OrthogonalMetric::fromDpi(300, 300); ASSERT_TRUE(metric.ok());
        auto attached = attach(f.partition.value, metric.value, f.budget, f.work); ASSERT_TRUE(attached.ok()) << attached.outcome.diagnostic;
        ASSERT_EQ(attached.value.pieceCount, 150u);
        auto before = f.work.visits(); auto start = std::chrono::steady_clock::now();
        struct Peak { Budget *budget; std::uint64_t bytes; } peak{&f.budget, f.budget.reserved()};
        ContourOptions options; options.observerData = &peak;
        options.observe = [](ContourPhase, void *v) noexcept {
            auto &p = *static_cast<Peak *>(v); p.bytes = std::max(p.bytes, p.budget->reserved());
        };
        auto result = traceContours(im.view(), attached.value, f.budget, f.work, {}, options);
        auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        EXPECT_TRUE(result.ok()) << result.outcome.diagnostic;
        if (!result.ok()) empty(result);
        else EXPECT_EQ(result.value.peakBudgetBytes, peak.bytes);
        std::cout << "B1_MEASURE " << suffix << " seconds=" << seconds << " visits=" << f.work.visits() - before
                  << " peak_budget_bytes=" << peak.bytes << " rings=" << result.value.ringCount
                  << " points=" << result.value.pointCount << " outcome=" << (result.ok() ? "success" : "refused")
                  << " diagnostic=" << result.outcome.diagnostic << '\n';
    }
}
