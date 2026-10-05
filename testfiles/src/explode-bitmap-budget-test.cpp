// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "util/bitmap-island-budget.h"
#include <array>
#include <barrier>
#include <limits>
#include <random>
#include <thread>
#include <utility>
using namespace Inkscape::Bitmap;
namespace {
constexpr auto max64 = std::numeric_limits<std::uint64_t>::max();
}
TEST(ExplodeBitmapBudget, T05RamAndRecovery)
{
    std::uint64_t limit = 17;
    EXPECT_EQ(admissionLimit({}, limit).status, Status::unavailable);
    EXPECT_EQ(limit, 17u);
    EXPECT_TRUE(admissionLimit({4096 * MiB, 4096 * MiB, 64 * MiB, true}, limit).ok());
    EXPECT_EQ(limit, 3840 * MiB);
    Memory low{4096 * MiB, 300 * MiB, 64 * MiB, true};
    EXPECT_TRUE(admissionLimit(low, limit).ok());
    EXPECT_EQ(limit, 44 * MiB);
    Budget budget(Budget::FixedLimitForTest{}, 1536 * MiB);
    ASSERT_TRUE(budget.recheck(low).ok());
    PlainBuffer raster;
    AllocationFault allocations;
    EXPECT_EQ(raster.allocate(budget, Stage::decode, 100000000, 4, &allocations).status, Status::failed);
    EXPECT_EQ(allocations.attempts, 0u); // refused before allocation
    EXPECT_EQ(budget.reserved(), 0u);
    EXPECT_TRUE(admissionLimit({4096 * MiB, 1024 * MiB, 2800 * MiB, true}, limit).ok());
    EXPECT_EQ(limit, 768 * MiB);
    for (auto memory : {Memory{4096 * MiB, 256 * MiB, 64 * MiB, true}}) {
        EXPECT_EQ(admissionLimit(memory, limit).status, Status::failed);
    }
}
TEST(ExplodeBitmapBudget, T05EveryStageAndGrowthTransfer)
{
    for (unsigned i = 0; i < static_cast<unsigned>(Stage::count); ++i) {
        auto stage = static_cast<Stage>(i);
        Budget budget(Budget::FixedLimitForTest{}, 100);
        Budget::Token token, rejected;
        ASSERT_TRUE(budget.acquire(stage, 100, token).ok());
        EXPECT_FALSE(budget.acquire(stage, 1, rejected).ok());
        EXPECT_FALSE(rejected);
        EXPECT_FALSE(token.resize(Stage::input, 101).ok());
        EXPECT_EQ(token.bytes(), 100u);
        EXPECT_EQ(budget.reserved(stage), 100u);
        ASSERT_TRUE(token.resize(Stage::rollback, 99).ok());
        EXPECT_EQ(budget.reserved(), 99u);
        token.release(); token.release();
        EXPECT_EQ(budget.reserved(), 0u);
    }
    Budget budget(Budget::FixedLimitForTest{}, max64);
    Budget::Token token, other;
    ASSERT_TRUE(budget.acquire(Stage::input, max64, token).ok());
    EXPECT_FALSE(budget.acquire(Stage::input, 1, other).ok());
    EXPECT_FALSE(token.resize(Stage::count, 0).ok());
    EXPECT_EQ(budget.reserved(), max64);
    token.release();
    for (auto pair : {std::pair{Stage::header, MiB}}) {
        ASSERT_TRUE(budget.acquire(pair.first, pair.second, token).ok());
        EXPECT_FALSE(budget.acquire(pair.first, 1, other).ok());
        EXPECT_FALSE(token.resize(pair.first, pair.second + 1).ok());
        token.release();
    }
}
TEST(ExplodeBitmapBudget, T05PressureRetirementAndSharedLedger)
{
    Budget budget(Budget::FixedLimitForTest{}, 1024 * MiB);
    ASSERT_TRUE(budget.recheck({4096 * MiB, 1024 * MiB, 64 * MiB, true}).ok());
    Budget::Token tone, alpha, retiring, next;
    ASSERT_TRUE(budget.acquire(Stage::composition, 64 * MiB, tone).ok());
    ASSERT_TRUE(budget.acquire(Stage::composition, 64 * MiB, alpha).ok());
    ASSERT_TRUE(budget.acquire(Stage::decode, 64 * MiB, retiring).ok());
    EXPECT_FALSE(budget.recheck({4096 * MiB, 384 * MiB, 64 * MiB, true}).ok());
    EXPECT_EQ(budget.reserved(), 192 * MiB);
    EXPECT_FALSE(budget.acquire(Stage::input, 1, next).ok());
    EXPECT_EQ(budget.recheck({}).status, Status::unavailable);
    EXPECT_FALSE(budget.acquire(Stage::input, 1, next).ok());
    retiring.release(); // reaper finally frees canceled storage
    ASSERT_TRUE(budget.recheck({4096 * MiB, 1024 * MiB, 64 * MiB, true}).ok());
    ASSERT_TRUE(budget.acquire(Stage::rollback, 640 * MiB, next).ok());
    EXPECT_EQ(budget.reserved(), 768 * MiB);
    EXPECT_FALSE(budget.acquire(Stage::input, 1, retiring).ok());
    next.release(); tone.release(); alpha.release();
    std::array<std::thread, 4> threads;
    for (auto &thread : threads) thread = std::thread([&] {
        for (int i = 0; i < 1000; ++i) {
            Budget::Token t;
            EXPECT_TRUE(budget.acquire(Stage::input, 8, t).ok());
        }
    });
    for (auto &thread : threads) thread.join();
    EXPECT_EQ(budget.reserved(), 0u);
}
TEST(ExplodeBitmapBudget, TokenMoveAndDoubleRelease)
{
    Budget budget(Budget::FixedLimitForTest{}, 100);
    Budget::Token a, b;
    ASSERT_TRUE(budget.acquire(Stage::input, 30, a).ok());
    EXPECT_FALSE(budget.acquire(Stage::input, 1, a).ok());
    ASSERT_TRUE(budget.acquire(Stage::decode, 20, b).ok());
    Budget::Token c(std::move(a));
    EXPECT_FALSE(a);
    b = std::move(c); // drops the original 20 bytes
    EXPECT_EQ(budget.reserved(), 30u);
    auto &alias = b;
    b = std::move(alias);
    EXPECT_EQ(b.bytes(), 30u);
    a.release(); c.release(); b.release(); b.release();
    EXPECT_EQ(budget.reserved(), 0u);
    EXPECT_FALSE(b.resize(Stage::input, 1).ok());
}
TEST(ExplodeBitmapBudget, T06FixedSeedArithmeticProperties)
{
    std::mt19937_64 random(0xEB106);
    std::array<std::uint64_t, 10> edges{0, 1, 2, 3, UINT32_MAX, std::uint64_t{UINT32_MAX} + 1,
        std::numeric_limits<std::size_t>::max(), max64 / 2, max64 - 1, max64};
    for (unsigned i = 0; i < 20000; ++i) {
        auto a = i < 100 ? edges[i / 10] : i < 10000 ? random() % 1000000000 : random();
        auto b = i < 100 ? edges[i % 10] : i < 10000 ? random() % 1000000000 : random();
        // Independent base-2^32 product: no compiler-specific integer extension.
        auto a0 = a & UINT32_MAX, a1 = a >> 32, b0 = b & UINT32_MAX, b1 = b >> 32;
        auto lo = a0 * b0;
        auto mid = (lo >> 32) + (a1 * b0 & UINT32_MAX) + (a0 * b1 & UINT32_MAX);
        auto high = a1 * b1 + (a1 * b0 >> 32) + (a0 * b1 >> 32) + (mid >> 32);
        bool productFits = high == 0 && !(a1 && b1);
        auto product = (mid << 32) | (lo & UINT32_MAX);
        bool sumFits = (a + b) >= a;
        std::uint64_t out = 91;
        EXPECT_EQ(checkedAdd(a, b, out), sumFits);
        EXPECT_EQ(out, sumFits ? a + b : 91);
        out = 91;
        EXPECT_EQ(checkedMul(a, b, out), productFits);
        EXPECT_EQ(out, productFits ? product : 91);
        std::uint32_t small = 91;
        EXPECT_EQ(checked32(a, small), a <= UINT32_MAX);
        EXPECT_EQ(small, a <= UINT32_MAX ? static_cast<std::uint32_t>(a) : 91);
        std::size_t native = 91;
        EXPECT_EQ(checkedSize(a, native), a <= std::numeric_limits<std::size_t>::max());
    }
    std::int32_t signedOut = 17;
    EXPECT_FALSE(checkedInt32(std::int64_t{INT32_MIN} - 1, signedOut));
    EXPECT_FALSE(checkedInt32(std::int64_t{INT32_MAX} + 1, signedOut));
    EXPECT_EQ(signedOut, 17);
    EXPECT_TRUE(checkedInt32(INT32_MIN, signedOut));
    EXPECT_EQ(signedOut, INT32_MIN);
}
TEST(ExplodeBitmapBudget, T06SizeExpressionsAnd32BitHarness)
{
    std::uint64_t n = 0, offset = 0;
    std::uint32_t small = 17;
    ASSERT_TRUE(checkedMul(65536, 65536, n));
    EXPECT_FALSE(checked32(n, small));
    EXPECT_EQ(small, 17u);
    EXPECT_FALSE(checkedAdd(max64, 2, n)); // gutters
    EXPECT_FALSE(checkedMul(max64, MiB, n));
    EXPECT_FALSE(checkedAdd(max64, 2, n)); // ceil(Q/3)
    ASSERT_TRUE(checkedMul(16384, 4, n));
    ASSERT_TRUE(checkedMul(16383, n, offset));
    ASSERT_TRUE(checkedMul(16383, 4, n));
    ASSERT_TRUE(checkedAdd(offset, n, offset));
    EXPECT_EQ(offset, 1073741820u);
    EXPECT_FALSE(checkedMul(max64, 4, n)); // negative stride converted to unsigned
    ASSERT_TRUE(checkedAdd(UINT32_MAX, 1, n)); // PNG/base64 chunk accumulation
    EXPECT_FALSE(checked32(n, small));
}
TEST(ExplodeBitmapBudget, T07EveryAllocationFailureAndEarlyReturn)
{
    constexpr unsigned stages = static_cast<unsigned>(Stage::count);
    for (unsigned k = 1; k <= stages + 1; ++k) {
        Budget budget(Budget::FixedLimitForTest{}, 4096);
        AllocationFault fault{k, 0};
        {
            std::array<PlainBuffer, stages> buffers;
            for (unsigned i = 0; i < stages; ++i) {
                auto result = buffers[i].allocate(budget, static_cast<Stage>(i), 16, 4, &fault);
                if (i + 1 == k) {
                    EXPECT_EQ(result.status, Status::failed);
                    EXPECT_NE(result.diagnostic[0], '\0');
                    EXPECT_EQ(buffers[i].data(), nullptr);
                    EXPECT_EQ(budget.reserved(), i * 64u);
                    break;
                }
                ASSERT_EQ(result.status, Status::changed);
                EXPECT_EQ(budget.reserved(), (i + 1) * 64u);
            }
        }
        EXPECT_EQ(budget.reserved(), 0u);
        EXPECT_EQ(fault.attempts, std::min(k, stages));
    }
}
TEST(ExplodeBitmapBudget, T07BufferOwnershipCancellationAndOverflow)
{
    Budget budget(Budget::FixedLimitForTest{}, 256);
    PlainBuffer a, b;
    ASSERT_TRUE(a.allocate(budget, Stage::input, 64, 1).ok());
    a.data()[0] = std::byte{42};
    ASSERT_TRUE(b.allocate(budget, Stage::decode, 32, 1).ok());
    b = std::move(a);
    EXPECT_EQ(b.size(), 64u);
    EXPECT_EQ(b.data()[0], std::byte{42});
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(budget.reserved(), 64u);
    EXPECT_FALSE(b.allocate(budget, Stage::input, 1, 1).ok());
    b.reset(); b.reset();
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    EXPECT_EQ(a.allocate(budget, Stage::input, 1, 1, nullptr, Stop(cancel)).status, Status::canceled);
    EXPECT_FALSE(a.allocate(budget, Stage::input, max64, 4).ok());
    EXPECT_EQ(a.allocate(budget, Stage::input, 0, 4).status, Status::unchanged);
    EXPECT_EQ(budget.reserved(), 0u);
    auto start = PhaseTimer::Clock::time_point{};
    JobWork work(0);
    PhaseTimer timer(start);
    EXPECT_EQ(timer.check(Stop(cancel), start).status, Status::canceled);
    ASSERT_TRUE(work.advance(100000000).ok());
    EXPECT_FALSE(work.advance(1).ok());
    EXPECT_FALSE(work.advance(max64).ok());
    EXPECT_FALSE(timer.check({}, start + std::chrono::seconds(30)).ok());
    EXPECT_EQ(work.visits(), 100000001u);
}

TEST(ExplodeBitmapBudget, SharedVisitLimitAndPhaseDeadline)
{
    for (auto [pixels, limit] : {std::pair{25000000ULL, 300000000ULL},
                                 std::pair{100000000ULL, 900000000ULL}}) {
        JobWork work(pixels);
        EXPECT_EQ(work.limit(), limit);
        ASSERT_TRUE(work.advance(160875485).ok()); // dense 150 count/encode plus grid scans
        ASSERT_TRUE(work.advance(limit - work.visits()).ok());
        EXPECT_FALSE(work.advance(1).ok());
    }
    JobWork oversized(100000001);
    EXPECT_FALSE(oversized.advance(1).ok());
    auto start = PhaseTimer::Clock::time_point{};
    PhaseTimer timer(start);
    EXPECT_TRUE(timer.check({}, start + std::chrono::seconds(29)).ok());
    EXPECT_FALSE(timer.check({}, start + std::chrono::seconds(30)).ok());
}

TEST(ExplodeBitmapBudget, Round2AdmissionTermsAndLiveRecheck)
{
    std::uint64_t cap = 7;
    for (auto m : {Memory{16384 * MiB, 16384 * MiB, 64 * MiB, true},
                   Memory{2048 * MiB, 8192 * MiB, 64 * MiB, true},
                   Memory{8192 * MiB, 1024 * MiB, 64 * MiB, true},
                   Memory{8192 * MiB, 8192 * MiB, 2800 * MiB, true},
                   Memory{8192 * MiB, 300 * MiB, 64 * MiB, true}}) {
        ASSERT_TRUE(admissionLimit(m, cap).ok());
        auto expected = m.available - 256 * MiB;
        EXPECT_EQ(cap, expected);
    }
    // Synthetic A>R isolates R/4; actual probe admission rejects inconsistent inputs.
    Budget invalid(1536 * MiB);
    EXPECT_TRUE(invalid.recheck({2048 * MiB, 8192 * MiB, 64 * MiB, true}).ok());
    EXPECT_EQ(admissionLimit({8192 * MiB, 8192 * MiB, 0, true}, cap).status, Status::unavailable);
    Budget budget(1536 * MiB); Budget::Token live, split, next;
    EXPECT_FALSE(budget.acquire(Stage::input, 1, live).ok());
    ASSERT_TRUE(budget.recheck({4096 * MiB, 1024 * MiB, 64 * MiB, true}).ok());
    ASSERT_TRUE(budget.acquire(Stage::input, 200 * MiB, live).ok());
    ASSERT_TRUE(budget.recheck({4096 * MiB, 824 * MiB, 264 * MiB, true, 200 * MiB}).ok());
    EXPECT_EQ(budget.limit(), 768 * MiB); // restore resident bytes before subtracting recovery
    ASSERT_TRUE(budget.acquire(Stage::decode, 56 * MiB, next).ok());
    EXPECT_EQ(budget.recheck({}).status, Status::unavailable);
    EXPECT_EQ(budget.limit(), 768 * MiB);
    ASSERT_TRUE(live.split(100 * MiB, split).ok());
    EXPECT_EQ(budget.reserved(), 256 * MiB);
    EXPECT_FALSE(live.split(1, split).ok());
    EXPECT_FALSE(split.split(101 * MiB, live).ok());
    EXPECT_TRUE(split.transfer(Stage::rollback).ok());
    EXPECT_TRUE(live.resize(Stage::input, 99 * MiB).ok());
    EXPECT_FALSE(live.resize(Stage::input, 100 * MiB).ok());
}
TEST(ExplodeBitmapBudget, Round2ArithmeticAndGrow)
{
    std::uint32_t narrow = 17;
    EXPECT_FALSE(checkedNarrow(std::uint64_t{UINT32_MAX} + 1, narrow)); EXPECT_EQ(narrow, 17u);
    std::uint64_t n = 17;
    EXPECT_FALSE(checkedUnsigned(-1, n)); EXPECT_EQ(n, 17u);
    EXPECT_TRUE(checkedUnsigned(INT64_MAX, n)); EXPECT_EQ(n, std::uint64_t{INT64_MAX});
    EXPECT_FALSE(checkedCeilDiv(1, 0, n)); EXPECT_EQ(n, std::uint64_t{INT64_MAX});
    EXPECT_TRUE(checkedCeilDiv(max64, 3, n)); EXPECT_EQ(n, max64 / 3);
    for (unsigned q = 0; q < 10000; ++q) {
        ASSERT_TRUE(checkedBase64Length(q, n)); EXPECT_EQ(n, 22 + 4 * ((q + 2) / 3));
    }
    n = 17; EXPECT_FALSE(checkedBase64Length(max64, n)); EXPECT_EQ(n, 17u);
    EXPECT_FALSE(checkedBase64Length(1, n, max64)); EXPECT_EQ(n, 17u);
    Budget budget(Budget::FixedLimitForTest{}, 128); PlainBuffer b;
    ASSERT_TRUE(b.allocate(budget, Stage::input, 32, 1).ok()); b.data()[31] = std::byte{42};
    EXPECT_FALSE(b.grow(budget, Stage::input, 97, 1).ok()); EXPECT_EQ(b.size(), 32u);
    AllocationFault fault{1,0}; EXPECT_FALSE(b.grow(budget, Stage::input, 64, 1, &fault).ok());
    EXPECT_EQ(budget.reserved(), 32u); EXPECT_EQ(b.data()[31], std::byte{42});
    ASSERT_TRUE(b.grow(budget, Stage::decode, 96, 1).ok());
    EXPECT_EQ(budget.reserved(), 96u); EXPECT_EQ(b.data()[31], std::byte{42});
    auto flag = std::make_shared<std::atomic<bool>>(true); Stop stop(flag); flag.reset();
    EXPECT_TRUE(stop.requested()); EXPECT_EQ(b.grow(budget, Stage::input, 100, 1, nullptr, stop).status, Status::canceled);
    Budget scratch(Budget::FixedLimitForTest{}, 1024 * MiB); Budget::Token crop, encoder;
    ASSERT_TRUE(scratch.acquire(Stage::crop, 64 * MiB, crop).ok());
    EXPECT_TRUE(scratch.acquire(Stage::encoder, 64 * MiB + 1, encoder).ok());
    encoder.release();
    EXPECT_TRUE(scratch.acquire(Stage::encoder, 64 * MiB, encoder).ok());
}
TEST(ExplodeBitmapBudget, Round2ContendedLimitAndRecheck)
{
    Budget budget(2); ASSERT_TRUE(budget.recheck({4096 * MiB, 1024 * MiB, 64 * MiB, true}).ok());
    std::atomic<unsigned> holders{0}, peak{0}; std::array<std::thread, 4> threads;
    std::barrier wave(4);
    for (auto &t : threads) t = std::thread([&] {
        Budget::Token first;
        if (budget.acquire(Stage::input, 1, first).ok()) ++holders;
        wave.arrive_and_wait(); EXPECT_EQ(holders.load(), 2u);
        wave.arrive_and_wait(); if (first) --holders; first.release();
        wave.arrive_and_wait();
        for (unsigned i = 0; i < 1000; ++i) {
            Budget::Token token;
            if (budget.acquire(Stage::input, 1, token).ok()) {
                auto n = ++holders; EXPECT_LE(n, 2u);
                auto old = peak.load(); while (old < n && !peak.compare_exchange_weak(old, n)) {}
                std::this_thread::yield(); --holders;
            }
            EXPECT_TRUE(budget.recheck({4096 * MiB, 1024 * MiB, 64 * MiB, true}).ok());
        }
    });
    for (auto &t : threads) t.join();
    EXPECT_GT(peak.load(), 0u); EXPECT_EQ(budget.reserved(), 0u);
}

TEST(ExplodeBitmapBudget, R3RefreshedHeadroomRetainsStructuredRefusalAndStorage)
{
    Budget budget(UINT64_MAX);
    ASSERT_TRUE(budget.recheck({4096*MiB, 512*MiB, 500*MiB, true}).ok());
    Budget::Token live, rejected;
    ASSERT_TRUE(budget.acquire(Stage::decode, 200*MiB, live).ok());
    auto result = budget.recheck({4096*MiB, 400*MiB, 500*MiB, true});
    EXPECT_FALSE(result.ok()); EXPECT_TRUE(result.insufficientMemory);
    EXPECT_STREQ(result.diagnostic, "Not enough memory: OS headroom / operation budget; estimated need 200.00 MiB, available 144.00 MiB.");
    EXPECT_EQ(budget.limit(), 144*MiB); EXPECT_EQ(budget.reserved(), 200*MiB);
    EXPECT_FALSE(budget.acquire(Stage::input, 1, rejected).ok()); EXPECT_FALSE(rejected);
    Outcome delivered = result;
    EXPECT_STREQ(delivered.diagnostic, result.diagnostic); // ownership survives delivery
    live.release(); EXPECT_EQ(budget.reserved(), 0u);
}
