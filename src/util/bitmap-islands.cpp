// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/bitmap-islands.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace Inkscape::Bitmap {
/* AlphaKiller alpha formula: MIT License

    Copyright (c) 2026 Abraham Saenz

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

*/
AlphaLut alphaLut(unsigned T, unsigned S) noexcept
{
    T = std::min(T, 255u); S = std::min(S, 127u);
    AlphaLut table{};
    auto low = static_cast<int>(T) - static_cast<int>(S);
    auto high = T + S;
    for (unsigned a = 1; a < 256; ++a) {
        double t = (static_cast<int>(a) - low) / static_cast<double>(std::max(1u, 2 * S));
        table[a] = !S ? (a >= T ? 255 : 0) : static_cast<int>(a) <= low ? 0 :
                   a >= high ? 255 : static_cast<unsigned>(std::floor(t * t * (3 - 2 * t) * 255 + .5));
    }
    return table;
}
namespace {
struct Poll {
    JobWork &work;
    PhaseTimer timer;
    Stop stop;
    Outcome outcome;
    std::uint64_t pending = 0;
    LabelOptions options;
    bool sawUnion = false, sawSort = false;
    std::uint64_t consumed = 0;
    void observe(LabelPhase phase) noexcept {
        auto &seen = phase == LabelPhase::unionFind ? sawUnion : sawSort;
        if (!seen && options.observe) options.observe(phase, options.observerData);
        seen = true;
    }
    bool tick(std::uint64_t n = 1) noexcept {
        pending += n;
        if (pending < 256) return true;
        consumed += pending; outcome = work.advance(pending); pending = 0;
        auto timed = timer.check(stop); if (!timed.ok()) outcome = timed;
        return outcome.ok();
    }
    bool flush() noexcept { consumed += pending; outcome = work.advance(pending); pending = 0;
        auto timed = timer.check(stop); if (!timed.ok()) outcome = timed; return outcome.ok(); }
};
// Iterative union-find; minimum run index preserves row-major discovery ids.
std::uint32_t root(Run *runs, std::uint32_t i, Poll &poll) noexcept
{
    while (runs[i].region != i) {
        poll.observe(LabelPhase::unionFind);
        if (!poll.tick()) break;
        runs[i].region = runs[runs[i].region].region;
        i = runs[i].region;
    }
    return i;
}
bool less(Island const &a, Island const &b) noexcept
{
    if (a.foreground != b.foreground) return a.foreground;
    if (a.y != b.y) return a.y < b.y;
    if (a.x != b.x) return a.x < b.x;
    return a.id < b.id;
}
// Bounded iterative heapsort; unlike library sort, every comparison polls.
bool sort(Island *a, std::uint32_t n, Poll &poll) noexcept
{
    auto sift = [&](std::uint32_t i, std::uint32_t count) {
        while (i < count / 2) {
            poll.observe(LabelPhase::sorting);
            if (!poll.tick()) return false;
            auto child = 2 * i + 1;
            if (child + 1 < count && less(a[child], a[child + 1])) ++child;
            if (!less(a[i], a[child])) break;
            std::swap(a[i], a[child]); i = child;
        }
        return true;
    };
    for (auto i = n / 2; i; --i) if (!sift(i - 1, n)) return false;
    for (auto i = n; i > 1; --i) {
        std::swap(a[0], a[i - 1]);
        if (!sift(0, i - 1)) return false;
    }
    return true;
}
}
Result<Regions> label(RgbaView view, AlphaLut const &lut, Budget &budget, JobWork &work, Stop stop, LabelOptions options) noexcept
{
    Poll poll{work, PhaseTimer{}, stop, {}, 0, options};
    auto fail = [&](Outcome o) -> Result<Regions> {
        poll.flush(); // retain every performed visit, including partial chunks on refusal
        return {o, {}, poll.consumed};
    };
    auto valid = view.validate();
    if (!valid.ok()) return fail(valid);
    if (options.runs > 16000000 || options.islands > 2000000 || options.topologyBytes > 256 * MiB)
        return fail({Status::incompatible, "Label ceilings may only be lowered"});
    if (!poll.flush()) return fail(poll.outcome);
    auto foreground = [&](std::uint32_t x, std::uint32_t y) {
        auto alpha = view.data[y * view.stride + 4ULL * x + 3];
        return alpha != 0 && lut[alpha] != 0;
    };
    // Count before allocation. Include background runs needed by enclosure.
    std::uint32_t count = 0;
    for (std::uint32_t y = 0; y < view.height; ++y) {
        bool previous = false;
        for (std::uint32_t x = 0; x < view.width; ++x) {
            if (!poll.tick()) return fail(poll.outcome);
            bool f = foreground(x, y);
            if (!x || f != previous) {
                if (count == options.runs) return fail({Status::failed, "Run cap exceeded"});
                ++count;
            }
            previous = f;
        }
    }
    if (!poll.flush()) return fail(poll.outcome);
    poll.timer = PhaseTimer{}; // count pass finished; joining phase starts
    Regions result;
    PlainBuffer map;
    auto allocate = [&](PlainBuffer &buffer, std::uint64_t n, std::uint64_t size) {
        std::uint64_t bytes;
        if (!checkedMul(n, size, bytes) || budget.reserved(Stage::topology) > options.topologyBytes ||
            bytes > options.topologyBytes - budget.reserved(Stage::topology))
            return Outcome{Status::failed, "Combined topology byte cap exceeded"};
        return buffer.allocate(budget, Stage::topology, n, size, options.fault, stop);
    };
    auto o = allocate(result._runs, count, sizeof(Run));
    if (!o.ok()) return fail(o);
    auto runs = reinterpret_cast<Run *>(result._runs.data());
    std::uint32_t next = 0, prevStart = 0, prevEnd = 0;
    for (std::uint32_t y = 0; y < view.height; ++y) {
        auto start = next;
        for (std::uint32_t x = 0; x < view.width;) {
            auto left = x; bool f = foreground(x, y);
            do { ++x; if (!poll.tick()) return fail(poll.outcome); }
            while (x < view.width && foreground(x, y) == f);
            runs[next] = {left, x, y, next}; ++next;
        }
        auto p = prevStart;
        for (auto i = start; i < next; ++i) {
            bool f = foreground(runs[i].x, y);
            while (p < prevEnd && runs[p].end < runs[i].x) {
                ++p; if (!poll.tick()) return fail(poll.outcome);
            }
            for (auto j = p; j < prevEnd && runs[j].x <= runs[i].end; ++j) {
                if (!poll.tick()) return fail(poll.outcome);
                if (f != foreground(runs[j].x, y - 1)) continue;
                if (!f && (runs[j].end <= runs[i].x || runs[j].x >= runs[i].end)) continue;
                auto a = root(runs, i, poll), b = root(runs, j, poll);
                runs[std::max(a, b)].region = std::min(a, b);
            }
        }
        prevStart = start; prevEnd = next;
    }
    if (!poll.flush()) return fail(poll.outcome);
    poll.timer = PhaseTimer{};
    std::uint32_t islands = 0, fg = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        runs[i].region = root(runs, i, poll);
        if (runs[i].region == i) {
            if (foreground(runs[i].x, runs[i].y)) {
                if (fg == options.islands) return fail({Status::failed, "Island cap exceeded"});
                ++fg;
            }
            ++islands;
        }
    }
    o = allocate(map, count, sizeof(std::uint32_t)); if (!o.ok()) return fail(o);
    o = allocate(result._islands, islands, sizeof(Island)); if (!o.ok()) return fail(o);
    auto indices = reinterpret_cast<std::uint32_t *>(map.data());
    auto records = reinterpret_cast<Island *>(result._islands.data());
    std::uint32_t index = 0, fid = 0, bid = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto &r = runs[i];
        if (r.region == i) {
            bool f = foreground(r.x, r.y);
            indices[i] = index;
            records[index++] = {f ? ++fid : ++bid, r.x, r.y, r.end, r.y + 1, 0, f, false};
        }
        auto &b = records[indices[r.region]];
        b.x = std::min(b.x, r.x); b.endX = std::max(b.endX, r.end);
        b.endY = r.y + 1; b.area += r.end - r.x;
        b.exterior |= !b.foreground && (!r.x || r.end == view.width || !r.y || r.y + 1 == view.height);
    }
    if (!poll.flush()) return fail(poll.outcome);
    poll.timer = PhaseTimer{};
    if (!sort(records, islands, poll)) return fail(poll.outcome);
    if (!poll.flush()) return fail(poll.outcome);
    poll.timer = PhaseTimer{};
    // ids are class-local; translate roots to sorted region indices without changing ids.
    for (std::uint32_t i = 0; i < islands; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        indices[(records[i].foreground ? 0 : fg) + records[i].id - 1] = i;
    }
    // Root ids must be retained separately from the translation table.
    // Reuse the now dead root map in-place after assigning discovery ids to root runs.
    fid = bid = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        if (runs[i].region == i) {
            bool f = foreground(runs[i].x, runs[i].y);
            auto slot = f ? fid++ : fg + bid++;
            runs[i].region = indices[slot];
        } else {
            runs[i].region = runs[runs[i].region].region;
        }
    }
    if (!poll.flush()) return fail(poll.outcome);
    result.width = view.width; result.height = view.height; result.runCount = count;
    result.islandCount = islands; result.foregroundCount = fg;
    return {{Status::changed, ""}, std::move(result), poll.consumed};
}
} // namespace Inkscape::Bitmap
