// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

#include "ui/widget/canvas/rendering-stats.h"

namespace {

using Inkscape::UI::Widget::CanvasDetail::RenderingStats;
using Inkscape::UI::Widget::CanvasDetail::ScopedTimer;
using Timing = RenderingStats::Timing;
using Event = RenderingStats::Event;

TEST(CanvasRenderingStats, DisabledRecordingDoesNotChangeCounters)
{
    RenderingStats stats;
    EXPECT_FALSE(stats.enabled());
    stats.record(Timing::Raster, 123);
    stats.add(Event::Pixels, 1000);
    {
        ScopedTimer timer(stats, Timing::Paint);
    }
    auto const result = stats.snapshot();
    for (auto const &timing : result.timings) {
        EXPECT_EQ(timing.count, 0u);
        EXPECT_EQ(timing.total_us, 0u);
        EXPECT_EQ(timing.max_us, 0u);
    }
    for (auto const event : result.events) {
        EXPECT_EQ(event, 0u);
    }
}

TEST(CanvasRenderingStats, CumulativeSnapshotsSupportIndependentReportingBaselines)
{
    RenderingStats stats;
    stats.set_enabled(true);
    stats.record(Timing::Raster, 19);
    stats.add(Event::Tiles, 3);
    auto const first = stats.snapshot();

    stats.record(Timing::Raster, 7);
    stats.record(Timing::Raster, 0); // Sub-microsecond work remains a sample.
    stats.record(Timing::Commit, 5);
    stats.add(Event::Tiles, 2);
    auto const second = stats.snapshot();

    EXPECT_EQ(second[Timing::Raster].count - first[Timing::Raster].count, 2u);
    EXPECT_EQ(second[Timing::Raster].total_us - first[Timing::Raster].total_us, 7u);
    EXPECT_EQ(second[Event::Tiles] - first[Event::Tiles], 2u);
    EXPECT_EQ(second[Timing::Raster].max_us, 19u); // Lifetime maximum, not interval maximum.
    EXPECT_EQ(second[Timing::Commit].total_us, 5u);
    EXPECT_EQ(first[Timing::Raster].count, 1u); // Snapshot is an independent value.
    EXPECT_EQ(first[Event::Tiles], 3u);
}

TEST(CanvasRenderingStats, PausePreservesHistoryAndIgnoresPausedRecording)
{
    RenderingStats stats;
    stats.set_enabled(true);
    stats.record(Timing::Queue, 10);
    stats.add(Event::Redraws);
    stats.set_enabled(false);
    stats.record(Timing::Queue, 500);
    stats.add(Event::Redraws, 10);
    stats.set_enabled(true);
    stats.record(Timing::Queue, 20);
    stats.add(Event::Redraws);

    auto const result = stats.snapshot();
    EXPECT_EQ(result[Timing::Queue].count, 2u);
    EXPECT_EQ(result[Timing::Queue].total_us, 30u);
    EXPECT_EQ(result[Timing::Queue].max_us, 20u);
    EXPECT_EQ(result[Event::Redraws], 2u);
}

TEST(CanvasRenderingStats, ScopeStartedWhileDisabledDoesNotJoinLaterCollection)
{
    RenderingStats stats;
    {
        ScopedTimer timer(stats, Timing::Paint);
        stats.set_enabled(true);
    }
    EXPECT_EQ(stats.snapshot()[Timing::Paint].count, 0u);
}

TEST(CanvasRenderingStats, ScopeSpanningPauseIsDiscardedAfterReenable)
{
    RenderingStats stats;
    stats.set_enabled(true);
    {
        ScopedTimer timer(stats, Timing::Paint);
        stats.set_enabled(false);
        stats.set_enabled(true);
    }
    EXPECT_EQ(stats.snapshot()[Timing::Paint].count, 0u);
    {
        ScopedTimer timer(stats, Timing::Paint);
        stats.set_enabled(true); // Repeating the current state must not invalidate the scope.
    }
    EXPECT_EQ(stats.snapshot()[Timing::Paint].count, 1u);
}

TEST(CanvasRenderingStats, CountersSaturateInsteadOfWrappingReportingDeltas)
{
    RenderingStats stats;
    stats.set_enabled(true);
    constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
    stats.add(Event::Pixels, limit - 2);
    stats.record(Timing::Raster, limit - 2);
    auto const before = stats.snapshot();
    stats.add(Event::Pixels, 10);
    stats.record(Timing::Raster, 10);
    auto const after = stats.snapshot();

    EXPECT_EQ(after[Event::Pixels], limit);
    EXPECT_EQ(after[Event::Pixels] - before[Event::Pixels], 2u);
    EXPECT_EQ(after[Timing::Raster].total_us, limit);
    EXPECT_EQ(after[Timing::Raster].total_us - before[Timing::Raster].total_us, 2u);
    EXPECT_EQ(after[Timing::Raster].count, 2u);
    EXPECT_EQ(after[Timing::Raster].max_us, limit - 2);
    stats.add(Event::Pixels);
    stats.record(Timing::Raster, 1);
    EXPECT_EQ(stats.snapshot()[Event::Pixels], limit);
    EXPECT_EQ(stats.snapshot()[Timing::Raster].total_us, limit);
}

TEST(CanvasRenderingStats, CancelledScopeDoesNotRecordOrAffectOtherScopes)
{
    RenderingStats stats;
    stats.set_enabled(true);
    {
        ScopedTimer cancelled(stats, Timing::Dispatch);
        ScopedTimer completed(stats, Timing::Snapshot);
        cancelled.cancel();
        cancelled.cancel(); // Cancellation is idempotent.
    }
    auto const result = stats.snapshot();
    EXPECT_EQ(result[Timing::Dispatch].count, 0u);
    EXPECT_EQ(result[Timing::Dispatch].total_us, 0u);
    EXPECT_EQ(result[Timing::Snapshot].count, 1u);
}

TEST(CanvasRenderingStats, ConcurrentWorkersPreserveExactTotalsAfterJoin)
{
    RenderingStats stats;
    stats.set_enabled(true);
    constexpr std::uint64_t worker_count = 4;
    constexpr std::uint64_t iterations = 10000;
    std::atomic<bool> start{false};
    std::atomic<unsigned> finished{0};
    std::array<std::thread, worker_count> workers;
    for (std::uint64_t i = 0; i < worker_count; ++i) {
        workers[i] = std::thread([&, i] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::uint64_t j = 0; j < iterations; ++j) {
                stats.record(Timing::Raster, i + 1);
                stats.add(Event::Tiles);
                stats.add(Event::Pixels, 100 + i);
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    auto previous = stats.snapshot();
    bool monotonic = true;
    start.store(true, std::memory_order_release);
    while (finished.load(std::memory_order_acquire) < worker_count) {
        auto const current = stats.snapshot();
        monotonic = monotonic && current[Timing::Raster].count >= previous[Timing::Raster].count
                    && current[Timing::Raster].total_us >= previous[Timing::Raster].total_us
                    && current[Timing::Raster].max_us >= previous[Timing::Raster].max_us
                    && current[Event::Tiles] >= previous[Event::Tiles]
                    && current[Event::Pixels] >= previous[Event::Pixels];
        previous = current;
        std::this_thread::yield();
    }
    for (auto &worker : workers) {
        worker.join();
    }

    EXPECT_TRUE(monotonic);
    auto const result = stats.snapshot();
    EXPECT_EQ(result[Timing::Raster].count, worker_count * iterations);
    EXPECT_EQ(result[Timing::Raster].total_us, iterations * (1 + 2 + 3 + 4));
    EXPECT_EQ(result[Timing::Raster].max_us, worker_count);
    EXPECT_EQ(result[Event::Tiles], worker_count * iterations);
    EXPECT_EQ(result[Event::Pixels], iterations * (100 + 101 + 102 + 103));
    EXPECT_EQ(result[Timing::Paint].count, 0u);
}

TEST(CanvasRenderingStats, CallbackAndContextScopesPartitionTheQueueInterval)
{
    RenderingStats stats;
    stats.set_enabled(true);
    std::optional<ScopedTimer> queue;
    std::optional<ScopedTimer> callback;
    queue.emplace(stats, Timing::Queue);
    callback.emplace(stats, Timing::CallbackWait);
    callback.reset(); // Callback entered; context work has not started yet.
    EXPECT_EQ(stats.snapshot()[Timing::CallbackWait].count, 1u);
    EXPECT_EQ(stats.snapshot()[Timing::Queue].count, 0u);
    EXPECT_EQ(stats.snapshot()[Timing::RedrawContext].count, 0u);
    {
        ScopedTimer context(stats, Timing::RedrawContext);
    }
    queue.reset();
    auto const result = stats.snapshot();
    EXPECT_EQ(result[Timing::Queue].count, 1u);
    EXPECT_EQ(result[Timing::RedrawContext].count, 1u);
    EXPECT_GE(result[Timing::Queue].total_us,
              result[Timing::CallbackWait].total_us + result[Timing::RedrawContext].total_us);

    queue.emplace(stats, Timing::Queue);
    callback.emplace(stats, Timing::CallbackWait);
    queue->cancel();
    callback->cancel(); // Deactivated canvases must not publish pending waits.
    callback.reset();
    queue.reset();
    EXPECT_EQ(stats.snapshot()[Timing::Queue].count, 1u);
    EXPECT_EQ(stats.snapshot()[Timing::CallbackWait].count, 1u);
}

TEST(CanvasRenderingStats, CsvSchemaHasUniqueColumnsForEveryCounter)
{
    std::istringstream input(RenderingStats::csv_header());
    std::set<std::string> columns;
    for (std::string column; std::getline(input, column, ','); ) {
        EXPECT_FALSE(column.empty());
        EXPECT_TRUE(columns.insert(column).second) << column;
    }
    EXPECT_EQ(columns.size(), 14u + RenderingStats::EventCount + 3 * RenderingStats::TimingCount);
    EXPECT_EQ(columns.count("schema_version"), 1u);
    EXPECT_EQ(columns.count("callback_wait_total_us"), 1u);
    EXPECT_EQ(columns.count("redraw_context_total_us"), 1u);
    EXPECT_EQ(columns.count("motion_received"), 1u);
    EXPECT_EQ(columns.count("motion_handler_total_us"), 1u);
}

} // namespace
