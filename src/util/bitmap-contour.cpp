// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/bitmap-contour.h"
#include <algorithm>
#include <cmath>

namespace Inkscape::Bitmap {
RingSet &RingSet::operator=(RingSet &&o) noexcept
{
    if (this != &o) {
        peakBudgetBytes = std::exchange(o.peakBudgetBytes, 0);
        pointCount = std::exchange(o.pointCount, 0); ringCount = std::exchange(o.ringCount, 0);
        _points = std::move(o._points); _rings = std::move(o._rings);
    }
    return *this;
}
ContourSet &ContourSet::operator=(ContourSet &&o) noexcept
{
    if (this != &o) {
        peakBudgetBytes = std::exchange(o.peakBudgetBytes, 0);
        pointCount = std::exchange(o.pointCount, 0); ringCount = std::exchange(o.ringCount, 0);
        pieceCount = std::exchange(o.pieceCount, 0);
        _points = std::move(o._points); _rings = std::move(o._rings); _pieces = std::move(o._pieces);
    }
    return *this;
}
namespace {
constexpr auto none = UINT32_MAX;
struct Poll {
    JobWork &work;
    Stop stop;
    ContourOptions const &options;
    PhaseTimer timer;
    Outcome outcome;
    std::uint64_t pending = 0, peak = 0;
    void sample(Budget const &budget) noexcept {
        peak = std::max(peak, budget.reserved());
        if (options.observe) options.observe(ContourPhase::allocation, options.observerData);
    }
    bool flush() noexcept {
        outcome = work.advance(pending); pending = 0;
        auto timed = timer.check(stop); if (!timed.ok()) outcome = timed;
        return outcome.ok();
    }
    bool tick() noexcept { ++pending; return pending < 256 || flush(); }
    bool phase(ContourPhase p) noexcept {
        if (!flush()) return false;
        if (options.observe) options.observe(p, options.observerData);
        return flush();
    }
    bool fail(char const *why) noexcept { outcome = {Status::failed, why}; return false; }
};
// Growth copies are explicitly metered, including the live overlap. Small
// element-wise copies keep Stop/time polling bounded in every internal loop.
template <typename T>
bool reserve(PlainBuffer &buffer, std::uint32_t count, std::uint32_t cap,
             Budget &budget, Poll &poll) noexcept
{
    if (count > cap) return poll.fail(std::is_same_v<T, ContourRing> ? "Contour ring cap exceeded" : "Contour point cap exceeded");
    auto old = buffer.size() / sizeof(T);
    if (count <= old) return true;
    auto capacity = std::min<std::uint64_t>(cap, std::max<std::uint64_t>(count, std::max<std::uint64_t>(64, old * 2)));
    PlainBuffer next;
    poll.outcome = next.allocate(budget, Stage::topology, capacity, sizeof(T), poll.options.fault, poll.stop);
    if (!poll.outcome.ok()) return false;
    poll.sample(budget);
    auto dst = reinterpret_cast<T *>(next.data()); auto src = reinterpret_cast<T const *>(buffer.data());
    for (std::size_t i = 0; i < old; ++i) {
        if (!poll.tick()) return false;
        dst[i] = src[i];
    }
    // Initialize spare capacity: later growth must never read uninitialized T.
    for (std::size_t i = old; i < capacity; ++i) {
        if (!poll.tick()) return false;
        dst[i] = {};
    }
    buffer = std::move(next); return poll.flush();
}
struct Node { ContourPoint point; std::uint32_t a, b; bool seen; };
}
Result<RingSet> traceLevel(FieldView f, Budget &budget, JobWork &work, Stop stop, ContourOptions o) noexcept
{
    auto start = work.visits(); Poll poll{work, stop, o, PhaseTimer{}, {}};
    RingSet result; PlainBuffer nodes, rows;
    auto fail = [&](Outcome outcome) { poll.flush(); Result<RingSet> r; r.outcome = outcome; r.consumed = work.visits() - start; return r; };
    auto refuse = [&](char const *why) { poll.flush(); return fail({Status::failed, why}); };
    auto pointCap = std::min(o.maxPoints, 2000000u), ringCap = std::min(o.maxRings, 50000u);
    if (!poll.phase(ContourPhase::trace)) return fail(poll.outcome);
    std::uint64_t cells;
    if (!f.values || f.width < 2 || f.height < 2 || !std::isfinite(f.originX) || !std::isfinite(f.originY) ||
        !checkedMul(f.width, f.height, cells) || cells > SIZE_MAX / sizeof(float)) return refuse("Invalid contour field");
    poll.outcome = rows.allocate(budget, Stage::topology, std::uint64_t(f.width) * 3, sizeof(std::uint32_t), o.fault, stop);
    if (!poll.outcome.ok()) return fail(poll.outcome);
    poll.sample(budget);
    auto top = reinterpret_cast<std::uint32_t *>(rows.data()); auto bottom = top + f.width; auto vertical = bottom + f.width;
    for (std::uint32_t x = 0; x < f.width; ++x) { if (!poll.tick()) return fail(poll.outcome); top[x] = none; }
    std::uint32_t count = 0;
    for (std::uint32_t y = 0; y + 1 < f.height; ++y) {
        for (std::uint32_t x = 0; x < f.width; ++x) {
            if (!poll.tick()) return fail(poll.outcome);
            bottom[x] = vertical[x] = none;
        }
        for (std::uint32_t x = 0; x + 1 < f.width; ++x) {
            if (!poll.tick()) return fail(poll.outcome);
            auto at = std::uint64_t(y) * f.width + x;
            double v[4] = {f.values[at], f.values[at + 1], f.values[at + f.width + 1], f.values[at + f.width]};
            unsigned mask = 0;
            for (unsigned k = 0; k < 4; ++k) {
                if (!std::isfinite(v[k])) return refuse("Nonfinite contour sample");
                if (v[k] >= 0) mask |= 1u << k;
            }
            if (!mask || mask == 15) continue;
            // Shared lattice edges, not rounded coordinates, identify vertices.
            std::uint32_t *slots[4] = {top + x, vertical + x + 1, bottom + x, vertical + x};
            unsigned first[4] = {0, 1, 3, 0}, last[4] = {1, 2, 2, 3};
            auto vertex = [&](unsigned edge, std::uint32_t &id) {
                auto &slot = *slots[edge];
                if (slot == none) {
                    if (!reserve<Node>(nodes, count + 1, pointCap, budget, poll)) return false;
                    auto t = v[first[edge]] / (v[first[edge]] - v[last[edge]]);
                    double px = x + (edge == 1 ? 1 : (edge == 0 || edge == 2 ? t : 0));
                    double py = y + (edge == 2 ? 1 : (edge == 1 || edge == 3 ? t : 0));
                    slot = count++;
                    reinterpret_cast<Node *>(nodes.data())[slot] = {{f.originX + px + .5, f.originY + py + .5}, none, none, false};
                }
                id = slot; return true;
            };
            auto segment = [&](unsigned a, unsigned b) {
                std::uint32_t i, j;
                if (!vertex(a, i) || !vertex(b, j)) return false;
                auto n = reinterpret_cast<Node *>(nodes.data());
                if (n[i].b != none || n[j].b != none) return poll.fail("Branching contour");
                (n[i].a == none ? n[i].a : n[i].b) = j;
                (n[j].a == none ? n[j].a : n[j].b) = i;
                return true;
            };
            bool ok = true;
            switch (mask) {
                case 1: case 14: ok = segment(3, 0); break;
                case 2: case 13: ok = segment(0, 1); break;
                case 3: case 12: ok = segment(3, 1); break;
                case 4: case 11: ok = segment(1, 2); break;
                case 6: case 9: ok = segment(0, 2); break;
                case 7: case 8: ok = segment(3, 2); break;
                case 5: case 10: {
                    auto q = v[0] * v[2] - v[1] * v[3];
                    bool positive = mask == 5 ? q >= 0 : q > 0;
                    ok = positive ? segment(0, 1) && segment(2, 3) : segment(3, 0) && segment(1, 2);
                    break;
                }
            }
            if (!ok) return fail(poll.outcome);
        }
        std::swap(top, bottom);
    }
    rows.reset();
    if (!poll.phase(ContourPhase::stitch)) return fail(poll.outcome);
    if (!reserve<ContourPoint>(result._points, count, pointCap, budget, poll)) return fail(poll.outcome);
    auto n = reinterpret_cast<Node *>(nodes.data()); auto points = reinterpret_cast<ContourPoint *>(result._points.data());
    for (std::uint32_t seed = 0; seed < count; ++seed) {
        if (!poll.tick()) return fail(poll.outcome);
        if (n[seed].seen) continue;
        ContourRing ring{result.pointCount, 0, 0, n[seed].point.x, n[seed].point.y, n[seed].point.x, n[seed].point.y, 0, none};
        auto current = seed, previous = none;
        do {
            if (!poll.tick()) return fail(poll.outcome);
            auto &node = n[current];
            if (node.seen || node.a == none || node.b == none) return refuse("Open or nonclosing contour");
            node.seen = true;
            // Exact-zero samples can place two distinct edge intersections at
            // the same vertex. Retain the geometry without duplicate vertices.
            if (result.pointCount == ring.begin || points[result.pointCount - 1].x != node.point.x ||
                points[result.pointCount - 1].y != node.point.y) points[result.pointCount++] = node.point;
            ring.minX = std::min(ring.minX, node.point.x); ring.maxX = std::max(ring.maxX, node.point.x);
            ring.minY = std::min(ring.minY, node.point.y); ring.maxY = std::max(ring.maxY, node.point.y);
            auto next = node.a == previous ? node.b : node.a;
            // Translate the shoelace sum to avoid cancellation at large origins.
            auto a = node.point, b = n[next].point, origin = n[seed].point;
            ring.area += ((a.x - origin.x) * (b.y - origin.y) - (b.x - origin.x) * (a.y - origin.y)) * .5;
            previous = current; current = next;
        } while (current != seed);
        if (result.pointCount > ring.begin + 1 && points[result.pointCount - 1].x == points[ring.begin].x &&
            points[result.pointCount - 1].y == points[ring.begin].y) --result.pointCount;
        ring.end = result.pointCount;
        if (!std::isfinite(ring.area)) return refuse("Nonfinite contour ring area");
        // A level can extinguish a component at an exact-zero sample. Its
        // closed edge graph is valid, but contributes no polygon or points.
        if (ring.end - ring.begin < 3 || ring.area == 0) {
            result.pointCount = ring.begin;
            continue;
        }
        if (!reserve<ContourRing>(result._rings, result.ringCount + 1, ringCap, budget, poll)) return fail(poll.outcome);
        reinterpret_cast<ContourRing *>(result._rings.data())[result.ringCount++] = ring;
    }
    nodes.reset();
    if (!poll.phase(ContourPhase::hierarchy)) return fail(poll.outcome);
    auto rings = reinterpret_cast<ContourRing *>(result._rings.data());
    for (std::uint32_t i = 0; i < result.ringCount; ++i) {
        auto &r = rings[i]; auto p = points[r.begin];
        double smallest = std::numeric_limits<double>::infinity();
        for (std::uint32_t j = 0; j < result.ringCount; ++j) {
            if (!poll.tick()) return fail(poll.outcome);
            auto const &s = rings[j]; auto area = std::abs(s.area);
            if (i == j || area <= std::abs(r.area) || area >= smallest ||
                p.x < s.minX || p.x > s.maxX || p.y < s.minY || p.y > s.maxY) continue;
            bool inside = false;
            for (auto k = s.begin, prev = s.end - 1; k < s.end; prev = k++) {
                if (!poll.tick()) return fail(poll.outcome);
                auto a = points[k], b = points[prev];
                if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
            }
            if (inside) { r.parent = j; smallest = area; }
        }
    }
    for (std::uint32_t i = 0; i < result.ringCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto &r = rings[i];
        for (auto p = r.parent; p != none; p = rings[p].parent) {
            if (!poll.tick()) return fail(poll.outcome);
            ++r.depth;
        }
        if ((r.area > 0) != (r.depth % 2 == 0)) {
            for (auto a = r.begin, b = r.end - 1; a < b; ++a, --b) {
                if (!poll.tick()) return fail(poll.outcome);
                std::swap(points[a], points[b]);
            }
            r.area = -r.area;
        }
    }
    if (!poll.flush()) return fail(poll.outcome);
    Result<RingSet> out; out.outcome = {Status::changed, ""}; out.consumed = work.visits() - start;
    result.peakBudgetBytes = poll.peak;
    out.value = std::move(result); return out;
}

namespace {
Result<SupportField> buildPieceSupportField(RgbaView rgba, Partition const &partition, std::uint32_t i,
                                          Budget &budget, JobWork &work, Stop stop, ContourOptions o, Poll &poll) noexcept
{
    auto start = work.visits();
    auto fail = [&](Outcome outcome) { poll.flush(); Result<SupportField> r; r.outcome = outcome; r.consumed = work.visits() - start; return r; };
    auto refuse = [&](char const *why) { return fail({Status::failed, why}); };
    auto valid = rgba.validate(); if (!valid.ok()) return fail(valid);
    if (i >= partition.pieceCount || partition.width != rgba.width || partition.height != rgba.height ||
        !partition.pieces() || !partition.pieceOffsets() || !partition.pieceRuns()) return refuse("Invalid contour partition");
    auto const &piece = partition.pieces()[i];
    if (piece.x >= piece.endX || piece.y >= piece.endY || piece.endX > rgba.width || piece.endY > rgba.height)
        return refuse("Invalid contour piece bounds");
    std::uint32_t width, height;
    if (!checked32(std::uint64_t(piece.endX) - piece.x + 4, width) ||
        !checked32(std::uint64_t(piece.endY) - piece.y + 4, height)) return refuse("Contour field size overflow");
    PlainBuffer field;
    poll.outcome = field.allocate(budget, Stage::topology, std::uint64_t(width) * height, sizeof(float), o.fault, stop);
    if (!poll.outcome.ok()) return fail(poll.outcome);
    poll.sample(budget);
    auto values = reinterpret_cast<float *>(field.data());
    for (std::uint64_t k = 0; k < std::uint64_t(width) * height; ++k) {
        if (!poll.tick()) return fail(poll.outcome);
        values[k] = -.5f;
    }
    for (auto k = partition.pieceOffsets()[i]; k < partition.pieceOffsets()[i + 1]; ++k) {
        if (!poll.tick()) return fail(poll.outcome);
        auto run = partition.run(partition.pieceRuns()[k]);
        if (!run.foreground) continue;
        if (run.piece != i || run.x < piece.x || run.end > piece.endX || run.y < piece.y || run.y >= piece.endY)
            return refuse("Invalid contour piece run");
        for (auto x = run.x; x < run.end; ++x) {
            if (!poll.tick()) return fail(poll.outcome);
            values[std::uint64_t(run.y - piece.y + 2) * width + x - piece.x + 2] =
                float(double(rgba.data[std::uint64_t(run.y) * rgba.stride + std::uint64_t(x) * 4 + 3]) / 255 - .5);
        }
    }
    if (!poll.flush()) return fail(poll.outcome);
    Result<SupportField> out; out.outcome = {Status::changed, ""}; out.consumed = work.visits() - start;
    out.value.field = {values, width, height, double(piece.x) - 2, double(piece.y) - 2};
    out.value.storage = std::move(field); out.value.peakBudgetBytes = poll.peak;
    return out;
}

} // namespace
Result<SupportField> pieceSupportField(RgbaView rgba, Partition const &partition, std::uint32_t i,
                                      Budget &budget, JobWork &work, Stop stop, ContourOptions o) noexcept
{
    Poll poll{work, stop, o, PhaseTimer{}, {}};
    return buildPieceSupportField(rgba, partition, i, budget, work, stop, o, poll);
}

Result<ContourSet> traceContours(RgbaView rgba, Partition const &partition, Budget &budget,
                                 JobWork &work, Stop stop, ContourOptions o) noexcept
{
    auto start = work.visits(); Poll poll{work, stop, o, PhaseTimer{}, {}};
    ContourSet result;
    auto fail = [&](Outcome outcome) { poll.flush(); Result<ContourSet> r; r.outcome = outcome; r.consumed = work.visits() - start; return r; };
    auto refuse = [&](char const *why) { poll.flush(); return fail({Status::failed, why}); };
    if (!poll.flush()) return fail(poll.outcome);
    auto valid = rgba.validate(); if (!valid.ok()) return fail(valid);
    if (partition.width != rgba.width || partition.height != rgba.height ||
        !partition.pieceOffsets() || (partition.pieceCount && !partition.pieces())) return refuse("Invalid contour partition");
    auto pointCap = std::min(o.maxPoints, 8000000u), ringCap = std::min(o.maxRings, 50000u);
    poll.outcome = result._pieces.allocate(budget, Stage::topology, partition.pieceCount, sizeof(PieceContour), o.fault, stop);
    if (!poll.outcome.ok()) return fail(poll.outcome);
    poll.sample(budget);
    for (std::uint32_t i = 0; i < partition.pieceCount; ++i) {
        if (!poll.phase(ContourPhase::field)) return fail(poll.outcome);
        auto support = buildPieceSupportField(rgba, partition, i, budget, work, stop, o, poll);
        if (!support.ok()) return fail(support.outcome);
        poll.peak = std::max(poll.peak, support.value.peakBudgetBytes);
        auto options = o; options.maxPoints = std::min(2000000u, pointCap - result.pointCount);
        options.maxRings = ringCap - result.ringCount;
        auto traced = traceLevel(support.value.field, budget, work, stop, options);
        if (!traced.ok()) return fail(traced.outcome);
        poll.peak = std::max(poll.peak, traced.value.peakBudgetBytes);
        support.value.storage.reset();
        if (!poll.phase(ContourPhase::output)) return fail(poll.outcome);
        auto const &r = traced.value;
        if (!reserve<ContourPoint>(result._points, result.pointCount + r.pointCount, pointCap, budget, poll) ||
            !reserve<ContourRing>(result._rings, result.ringCount + r.ringCount, ringCap, budget, poll)) return fail(poll.outcome);
        auto points = reinterpret_cast<ContourPoint *>(result._points.data()); auto rings = reinterpret_cast<ContourRing *>(result._rings.data());
        for (std::uint32_t k = 0; k < r.pointCount; ++k) {
            if (!poll.tick()) return fail(poll.outcome);
            points[result.pointCount + k] = r.points()[k];
        }
        for (std::uint32_t k = 0; k < r.ringCount; ++k) {
            if (!poll.tick()) return fail(poll.outcome);
            auto ring = r.rings()[k]; ring.begin += result.pointCount; ring.end += result.pointCount;
            if (ring.parent != none) ring.parent += result.ringCount;
            rings[result.ringCount + k] = ring;
        }
        reinterpret_cast<PieceContour *>(result._pieces.data())[i] = {i, result.ringCount, result.ringCount + r.ringCount, !r.ringCount};
        result.pointCount += r.pointCount; result.ringCount += r.ringCount; ++result.pieceCount;
    }
    if (!poll.flush()) return fail(poll.outcome);
    Result<ContourSet> out; out.outcome = {Status::changed, ""}; out.consumed = work.visits() - start;
    result.peakBudgetBytes = poll.peak;
    out.value = std::move(result); return out;
}
} // namespace Inkscape::Bitmap
