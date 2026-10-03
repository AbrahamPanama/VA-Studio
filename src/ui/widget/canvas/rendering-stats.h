// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_WIDGET_CANVAS_RENDERING_STATS_H
#define INKSCAPE_UI_WIDGET_CANVAS_RENDERING_STATS_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace Inkscape::UI::Widget::CanvasDetail {

class ScopedTimer;

/**
 * Cumulative diagnostics shared by the canvas and its rendering workers.
 *
 * Durations are elapsed time, not processor time. Parallel worker durations can
 * sum to more than the elapsed reporting interval. Counters are never reset;
 * consumers keep their own reporting baseline. Values saturate on overflow.
 */
class RenderingStats
{
public:
    enum class Timing : std::size_t {
        Raster, BufferWait, Commit, Paint, Queue, Update, Redraw, Dispatch, Snapshot,
        CallbackWait, RedrawContext, MotionHandler, EventHandler, Count
    };

    enum class Event : std::size_t {
        Tiles, Pixels, Redraws, Timeouts, StoreRecreated, StoreShifted,
        RedrawRequests, CoalescedRequests, IdleCallbacks, InstantCallbacks,
        MotionReceived, MotionIgnored, ButtonMotion, ButtonPresses, ButtonReleases, PaintLaunches, DeadlineCallbacks, Count
    };

    static constexpr auto TimingCount = static_cast<std::size_t>(Timing::Count);
    static constexpr auto EventCount = static_cast<std::size_t>(Event::Count);

    static constexpr std::array TimingNames{
        "raster", "buffer_wait", "commit", "paint", "queue", "update", "redraw", "dispatch", "snapshot",
        "callback_wait", "redraw_context", "motion_handler", "event_handler"
    };
    static constexpr std::array EventNames{
        "tiles", "pixels", "redraws", "timeouts", "store_recreated", "store_shifted",
        "redraw_requests", "coalesced_requests", "idle_callbacks", "instant_callbacks",
        "motion_received", "motion_ignored", "button_motion", "button_presses", "button_releases", "paint_launches", "deadline_callbacks"
    };
    static_assert(TimingNames.size() == TimingCount);
    static_assert(EventNames.size() == EventCount);
    static constexpr unsigned CsvVersion = 4;

    static std::string csv_header()
    {
        std::string header = "sample_us,interval_ms,canvas_id,gtk_renderer,canvas_backend,logical_width,logical_height,scale,threads,gtk_cycles,schema_version,render_mode,split_mode,dragging";
        for (auto name : EventNames) {
            header += ',';
            header += name;
        }
        for (auto name : TimingNames) {
            for (auto suffix : {"_count", "_total_us", "_lifetime_max_us"}) {
                header += ',';
                header += name;
                header += suffix;
            }
        }
        return header;
    }

    struct DurationSample {
        std::uint64_t count = 0;
        std::uint64_t total_us = 0;
        std::uint64_t max_us = 0; // Lifetime maximum; do not subtract baselines.
    };

    struct Snapshot {
        std::array<DurationSample, TimingCount> timings{};
        std::array<std::uint64_t, EventCount> events{};

        DurationSample const &operator[](Timing timing) const noexcept
        {
            return timings[static_cast<std::size_t>(timing)];
        }

        std::uint64_t operator[](Event event) const noexcept
        {
            return events[static_cast<std::size_t>(event)];
        }
    };

    void set_enabled(bool enable) noexcept
    {
        auto state = _state.load(std::memory_order_relaxed);
        while (static_cast<bool>(state & 1) != enable) {
            // Change generation as well as the enabled bit. A timer spanning a
            // pause must not count that pause when diagnostics are enabled again.
            auto const next = (state + 2) ^ 1;
            if (_state.compare_exchange_weak(state, next, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    bool enabled() const noexcept
    {
        return _state.load(std::memory_order_relaxed) & 1;
    }

    void record(Timing timing, std::uint64_t elapsed_us) noexcept
    {
        if (enabled()) {
            record_enabled(timing, elapsed_us);
        }
    }

    void add(Event event, std::uint64_t amount = 1) noexcept
    {
        if (enabled()) {
            add_saturating(_events[static_cast<std::size_t>(event)], amount);
        }
    }

    /**
     * Each field is read atomically, but the whole snapshot is not a transaction.
     * A concurrently recorded duration may appear in count and total_us on
     * adjacent samples. Interval means are therefore approximate while active;
     * after workers have stopped, the snapshot contains their complete totals.
     */
    Snapshot snapshot() const noexcept
    {
        Snapshot result;
        for (std::size_t i = 0; i < TimingCount; ++i) {
            result.timings[i] = {
                _timings[i].count.load(std::memory_order_relaxed),
                _timings[i].total_us.load(std::memory_order_relaxed),
                _timings[i].max_us.load(std::memory_order_relaxed)
            };
        }
        for (std::size_t i = 0; i < EventCount; ++i) {
            result.events[i] = _events[i].load(std::memory_order_relaxed);
        }
        return result;
    }

private:
    friend class ScopedTimer;

    struct AtomicDuration {
        std::atomic<std::uint64_t> count{0};
        std::atomic<std::uint64_t> total_us{0};
        std::atomic<std::uint64_t> max_us{0};
    };

    // Bit 0 is enabled; the other bits identify successive collection periods.
    std::atomic<std::uint64_t> _state{0};
    std::array<AtomicDuration, TimingCount> _timings{};
    std::array<std::atomic<std::uint64_t>, EventCount> _events{};

    static void add_saturating(std::atomic<std::uint64_t> &counter, std::uint64_t amount) noexcept
    {
        auto value = counter.load(std::memory_order_relaxed);
        constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
        while (value != limit && amount != 0) {
            auto const next = amount > limit - value ? limit : value + amount;
            if (counter.compare_exchange_weak(value, next, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    void record_enabled(Timing timing, std::uint64_t elapsed_us) noexcept
    {
        auto &sample = _timings[static_cast<std::size_t>(timing)];
        add_saturating(sample.count, 1);
        add_saturating(sample.total_us, elapsed_us);
        auto maximum = sample.max_us.load(std::memory_order_relaxed);
        while (maximum < elapsed_us &&
               !sample.max_us.compare_exchange_weak(maximum, elapsed_us, std::memory_order_relaxed)) {}
    }
};

/** Scope elapsed-time measurement; a disabled scope never reads the clock. */
class ScopedTimer
{
public:
    ScopedTimer(RenderingStats &stats, RenderingStats::Timing timing) noexcept
        : _stats(stats)
        , _timing(timing)
        , _generation(stats._state.load(std::memory_order_relaxed))
    {
        if (_generation & 1) {
            _start = Clock::now();
        }
    }

    ScopedTimer(ScopedTimer const &) = delete;
    ScopedTimer &operator=(ScopedTimer const &) = delete;

    // Abandon an interval without sampling the clock or publishing a duration.
    // Like destruction, this must run on the thread currently owning the scope.
    void cancel() noexcept { _generation = 0; }

    ~ScopedTimer()
    {
        if (!(_generation & 1) || _stats._state.load(std::memory_order_relaxed) != _generation) {
            return;
        }
        auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - _start).count();
        _stats.record_enabled(_timing, elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0);
    }

private:
    using Clock = std::chrono::steady_clock;
    RenderingStats &_stats;
    RenderingStats::Timing _timing;
    std::uint64_t _generation;
    Clock::time_point _start{};
};

} // namespace Inkscape::UI::Widget::CanvasDetail

#endif // INKSCAPE_UI_WIDGET_CANVAS_RENDERING_STATS_H
