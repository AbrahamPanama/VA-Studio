// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/bitmap-island-enclosure.h"
#include "util/bitmap-island-specks.h"
#include <algorithm>
namespace Inkscape::Bitmap {
Partition &Partition::operator=(Partition &&o) noexcept
{
    if (this == &o) return *this;
    width = std::exchange(o.width, 0); height = std::exchange(o.height, 0);
    pieceCount = std::exchange(o.pieceCount, 0); runCount = std::exchange(o.runCount, 0);
    regionCount = std::exchange(o.regionCount, 0); foregroundCount = std::exchange(o.foregroundCount, 0);
    enclosureMerges = std::exchange(o.enclosureMerges, 0);
    speckJoins = std::exchange(o.speckJoins, 0);
    retainedIsolatedSpecks = std::exchange(o.retainedIsolatedSpecks, 0);
    boundaries = std::exchange(o.boundaries, 0); graphEdges = std::exchange(o.graphEdges, 0);
    topologyPeak = std::exchange(o.topologyPeak, 0); _sourceRuns = std::exchange(o._sourceRuns, nullptr);
    _pieces = std::move(o._pieces); _runOwners = std::move(o._runOwners); _owners = std::move(o._owners);
    _rows = std::move(o._rows); _pieceOffsets = std::move(o._pieceOffsets); _pieceRuns = std::move(o._pieceRuns);
    return *this;
}
bool pieceLess(Piece const &a, Piece const &b) noexcept
{
    return a.y != b.y ? a.y < b.y : a.x != b.x ? a.x < b.x : a.id < b.id;
}
namespace {
struct Node { std::uint32_t offset, degree, cursor, owner, depth; };
struct Poll {
    JobWork &work;
    Stop stop;
    EnclosureOptions const &options;
    PhaseTimer timer;
    Outcome outcome;
    std::uint64_t pending = 0, consumed = 0;
    EnclosurePhase current = EnclosurePhase::graph;
    bool observeNext = false;
    std::uint64_t limit = 50000000;
    bool flush() noexcept {
        consumed += pending; outcome = work.advance(pending); pending = 0;
        auto timed = timer.check(stop); if (!timed.ok()) outcome = timed;
        return outcome.ok();
    }
    bool tick() noexcept {
        if (observeNext) { observeNext = false;
            if (options.observe) options.observe(current, options.observerData); }
        if (consumed + pending >= limit) {
            flush(); outcome = {Status::failed, "Enclosure visit cap exceeded"}; return false;
        }
        ++pending; return pending < 256 || flush();
    }
    bool phase(EnclosurePhase p) noexcept {
        if (!flush()) return false;
        timer = PhaseTimer{};
        current = p; observeNext = true;
        return flush();
    }
};
// Two linear sweeps over row-major runs, never over a dense pixel label plane.
// A vertical overlap is a 4-neighbour contact; diagonals deliberately add no edge.
// Both passes use identical order. Parallel edges cost O(runs), not O(pixels).
template <typename Emit>
bool contacts(Regions const &r, Emit emit, Poll &poll) noexcept
{
    auto runs = r.runs(); auto islands = r.islands(); auto border = r.islandCount;
    for (std::uint32_t i = 0; i < r.islandCount; ++i) {
        if (!poll.tick()) return false;
        if (islands[i].exterior && !emit(i, border)) return false;
    }
    std::uint32_t previous = 0, previousEnd = 0, begin = 0;
    while (begin < r.runCount) {
        auto end = begin;
        while (end < r.runCount && runs[end].y == runs[begin].y) {
            if (!poll.tick()) return false;
            auto const &a = runs[end];
            if (islands[a.region].foreground &&
                (!a.x || a.end == r.width || !a.y || a.y + 1 == r.height))
                if (!emit(a.region, border)) return false;
            if (end > begin && islands[runs[end - 1].region].foreground != islands[a.region].foreground)
                if (!emit(runs[end - 1].region, a.region)) return false;
            ++end;
        }
        auto a = previous, b = begin;
        while (a < previousEnd && b < end) {
            if (!poll.tick()) return false;
            auto const &u = runs[a]; auto const &v = runs[b];
            if (u.x < v.end && v.x < u.end && islands[u.region].foreground != islands[v.region].foreground)
                if (!emit(u.region, v.region)) return false;
            if (u.end <= v.end) ++a; else ++b;
        }
        previous = begin; previousEnd = end; begin = end;
    }
    return true;
}
bool sort(Piece *p, std::uint32_t n, Poll &poll) noexcept
{
    auto sift = [&](std::uint32_t i, std::uint32_t size) {
        while (i < size / 2) {
            if (!poll.tick()) return false;
            auto child = 2 * i + 1;
            if (child + 1 < size && pieceLess(p[child], p[child + 1])) ++child;
            if (!pieceLess(p[i], p[child])) break;
            std::swap(p[i], p[child]); i = child;
        }
        return true;
    };
    for (auto i = n / 2; i; --i) if (!sift(i - 1, n)) return false;
    for (auto i = n; i > 1; --i) {
        if (!poll.tick()) return false;
        std::swap(p[0], p[i - 1]); if (!sift(0, i - 1)) return false;
    }
    return true;
}
} // namespace
Outcome sortPieces(Piece *pieces, std::uint32_t count, JobWork &work, Stop stop) noexcept
{
    EnclosureOptions options;
    Poll poll{work, stop, options, PhaseTimer{}, {}};
    if (!poll.flush() || !sort(pieces, count, poll)) return poll.outcome;
    poll.flush(); return poll.outcome;
}
Result<Partition> enclose(Regions const &r, Budget &budget, JobWork &work, Stop stop, EnclosureOptions o) noexcept
{
    Poll poll{work, stop, o, PhaseTimer{}, {}};
    Partition p; PlainBuffer nodes, edges, queue;
    auto fail = [&](Outcome outcome) {
        poll.flush(); Result<Partition> result;
        result.outcome = outcome; result.consumed = poll.consumed; return result;
    };
    auto refused = [&](char const *message) { return fail({Status::failed, message}); };
    if (!poll.flush()) return fail(poll.outcome);
    EnclosureOptions hard;
    if (o.nodes > hard.nodes || o.edges > hard.edges || o.queue > hard.queue ||
        o.boundaries > hard.boundaries || o.topologyBytes > hard.topologyBytes || o.visits > hard.visits)
        return refused("Enclosure ceilings may only be lowered");
    if (!r.width || !r.height || !r.runs() || !r.islands() || !r.islandCount ||
        r.foregroundCount > r.islandCount || r.runCount > 16000000)
        return fail({Status::incompatible, "Expected immutable labeled Regions"});
    std::uint64_t specksShare, base, term;
    if (!checkedMul(r.runCount, 11, base) || !checkedMul(r.islandCount, 9, term) ||
        !checkedAdd(base, term, base)) return refused("Speck work reserve overflow");
    specksShare = std::max(std::uint64_t(20000000), base);
    auto remaining = work.visits() < work.limit() ? work.limit() - work.visits() : 0;
    poll.limit = std::min(o.visits, remaining > specksShare ? remaining - specksShare : 0);
    auto n = std::uint64_t(r.islandCount) + 1;
    if (n > o.nodes) return refused("Graph node cap exceeded");
    // All stage allocations (including input Regions and live overlap elsewhere)
    // count against the combined ceiling. Clients serialize topology growth.
    auto allocate = [&](PlainBuffer &b, std::uint64_t count, std::uint64_t size) {
        std::uint64_t bytes, total;
        if (!checkedMul(count, size, bytes) || !checkedAdd(budget.reserved(Stage::topology), bytes, total) ||
            total > o.topologyBytes) { poll.outcome = {Status::failed, "Topology byte cap exceeded"}; return false; }
        poll.outcome = b.allocate(budget, Stage::topology, count, size, o.fault, stop);
        if (!poll.outcome.ok()) return false;
        p.topologyPeak = std::max(p.topologyPeak, total); return true;
    };
    if (!allocate(nodes, n, sizeof(Node))) return fail(poll.outcome);
    auto g = reinterpret_cast<Node *>(nodes.data());
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        g[i] = {0, 0, 0, exteriorPiece, exteriorPiece};
    }
    if (!poll.phase(EnclosurePhase::graph)) return fail(poll.outcome);
    std::uint64_t count = 0;
    auto counter = [&](std::uint32_t a, std::uint32_t b) {
        if (++count > o.boundaries) { poll.outcome = {Status::failed, "Boundary contact cap exceeded"}; return false; }
        if (count > o.edges) { poll.outcome = {Status::failed, "Graph edge cap exceeded"}; return false; }
        ++g[a].degree; ++g[b].degree; return true;
    };
    if (!contacts(r, counter, poll)) return fail(poll.outcome);
    p.boundaries = p.graphEdges = count;
    if (!allocate(edges, count * 2, sizeof(std::uint32_t))) return fail(poll.outcome);
    auto adj = reinterpret_cast<std::uint32_t *>(edges.data());
    std::uint32_t offset = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        g[i].offset = g[i].cursor = offset; offset += g[i].degree;
    }
    auto writer = [&](std::uint32_t a, std::uint32_t b) {
        adj[g[a].cursor++] = b; adj[g[b].cursor++] = a; return true;
    };
    if (!contacts(r, writer, poll)) return fail(poll.outcome);
    // Deduplicate in place, preserving first-contact order. Node.cursor is a
    // neighbour stamp, so no hash table, sorting or extra buffer is needed.
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        g[i].cursor = exteriorPiece;
    }
    std::uint32_t compact = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto start = g[i].offset, degree = g[i].degree; g[i].offset = compact;
        for (std::uint32_t j = 0; j < degree; ++j) {
            if (!poll.tick()) return fail(poll.outcome);
            auto neighbour = adj[start + j];
            if (g[neighbour].cursor != i) { g[neighbour].cursor = i; adj[compact++] = neighbour; }
        }
        g[i].degree = compact - g[i].offset;
    }
    p.graphEdges = compact / 2;
    // Every node enqueues exactly once. Refuse before allocating a queue too large.
    if (n > o.queue) return refused("Graph queue cap exceeded");
    if (!allocate(queue, n, sizeof(std::uint32_t))) return fail(poll.outcome);
    auto todo = reinterpret_cast<std::uint32_t *>(queue.data());
    if (!poll.phase(EnclosurePhase::traversal)) return fail(poll.outcome);
    auto border = r.islandCount;
    g[border].depth = 0; todo[0] = border; std::uint32_t tail = 1;
    for (std::uint32_t head = 0; head < tail; ++head) {
        if (!poll.tick()) return fail(poll.outcome);
        auto a = todo[head];
        for (std::uint32_t j = 0; j < g[a].degree; ++j) {
            if (!poll.tick()) return fail(poll.outcome);
            auto b = adj[g[a].offset + j]; if (g[b].depth != exteriorPiece) continue;
            // Exterior background collapses to depth zero. A depth-one foreground
            // starts a piece; every descendant inherits the grandparent's owner.
            g[b].depth = b >= r.foregroundCount && r.islands()[b].exterior ? 0 : g[a].depth + 1;
            g[b].owner = b < r.foregroundCount && g[b].depth == 1 ? b : g[a].owner;
            todo[tail++] = b;
        }
    }
    if (tail != n) return refused("Disconnected region graph");
    edges.reset(); // no graph contacts survive into output allocation
    if (!poll.phase(EnclosurePhase::partition)) return fail(poll.outcome);
    for (std::uint32_t i = 0; i < r.islandCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        todo[i] = g[i].owner;
        if (i < r.foregroundCount && todo[i] == i) ++p.pieceCount;
    }
    nodes.reset(); // queue alone retains root ownership
    if (!allocate(p._pieces, p.pieceCount, sizeof(Piece)) ||
        !allocate(p._owners, r.islandCount, sizeof(std::uint32_t))) return fail(poll.outcome);
    auto pieces = reinterpret_cast<Piece *>(p._pieces.data());
    auto owners = reinterpret_cast<std::uint32_t *>(p._owners.data());
    std::uint32_t index = 0;
    for (std::uint32_t i = 0; i < r.foregroundCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        if (todo[i] == i) { owners[i] = index; auto b = r.islands()[i];
            pieces[index++] = {b.id, b.x, b.y, b.endX, b.endY, 0}; }
    }
    for (std::uint32_t i = 0; i < r.foregroundCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto b = r.islands()[i]; auto &piece = pieces[owners[todo[i]]];
        piece.id = std::min(piece.id, b.id); piece.x = std::min(piece.x, b.x); piece.y = std::min(piece.y, b.y);
        piece.endX = std::max(piece.endX, b.endX); piece.endY = std::max(piece.endY, b.endY); piece.area += b.area;
    }
    // Convert roots to discovery ids before reusing owners as id -> sorted index.
    for (std::uint32_t i = 0; i < r.islandCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        if (todo[i] != exteriorPiece) todo[i] = r.islands()[todo[i]].id;
    }
    if (!poll.phase(EnclosurePhase::sorting) || !sort(pieces, p.pieceCount, poll)) return fail(poll.outcome);
    for (std::uint32_t i = 0; i < p.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        owners[pieces[i].id - 1] = i;
    }
    for (std::uint32_t i = 0; i < r.islandCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        if (todo[i] != exteriorPiece) todo[i] = owners[todo[i] - 1];
    }
    for (std::uint32_t i = 0; i < r.islandCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        owners[i] = todo[i];
    }
    queue.reset();
    if (!allocate(p._runOwners, r.runCount, sizeof(std::uint32_t)) ||
        !allocate(p._rows, std::uint64_t(r.height) + 1, sizeof(std::uint32_t)) ||
        !allocate(p._pieceOffsets, std::uint64_t(p.pieceCount) + 1, sizeof(std::uint32_t)) ||
        !allocate(p._pieceRuns, r.runCount, sizeof(std::uint32_t))) return fail(poll.outcome);
    auto runOwners = reinterpret_cast<std::uint32_t *>(p._runOwners.data());
    auto rows = reinterpret_cast<std::uint32_t *>(p._rows.data());
    auto offsets = reinterpret_cast<std::uint32_t *>(p._pieceOffsets.data());
    auto list = reinterpret_cast<std::uint32_t *>(p._pieceRuns.data());
    for (std::uint32_t i = 0; i <= p.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        offsets[i] = 0;
    }
    std::uint32_t row = 0;
    for (std::uint32_t i = 0; i < r.runCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto a = r.runs()[i]; runOwners[i] = owners[a.region];
        if (runOwners[i] != exteriorPiece) ++offsets[runOwners[i]];
        if (a.y == row) rows[row++] = i;
    }
    rows[r.height] = r.runCount;
    // End offsets become reverse-fill cursors, then naturally become starts.
    for (std::uint32_t i = 1; i < p.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        offsets[i] += offsets[i - 1];
    }
    if (p.pieceCount) offsets[p.pieceCount] = offsets[p.pieceCount - 1];
    for (std::uint32_t i = r.runCount; i; --i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto owner = runOwners[i - 1]; if (owner != exteriorPiece) list[--offsets[owner]] = i - 1;
    }
    p._sourceRuns = r.runs(); p.foregroundCount = r.foregroundCount;
    p.enclosureMerges = r.foregroundCount - p.pieceCount;
    if (!poll.flush()) return fail(poll.outcome);
    p.width = r.width; p.height = r.height; p.runCount = r.runCount; p.regionCount = r.islandCount;
    auto metric = OrthogonalMetric::fromDpi(o.speckDpiX, o.speckDpiY);
    if (!metric.ok()) return fail(metric.outcome);
    if (p.runCount > poll.limit - poll.consumed) return refused("Enclosure visit cap exceeded");
    auto reserveStart = work.visits();
    auto reserve = speckWorkReserve(p, metric.value, work, stop);
    // The reserve scan is itself enclosure work, charged to this phase ceiling.
    poll.consumed += work.visits() - reserveStart;
    if (!reserve.ok()) return fail(reserve.outcome);
    if (poll.consumed > poll.limit) return refused("Enclosure visit cap exceeded");
    if (reserve.value > work.limit() - work.visits()) return refused("Speck work reserve exceeded");
    Result<Partition> result; result.outcome = {Status::changed, ""};
    result.consumed = poll.consumed; result.value = std::move(p); return result;
}
} // namespace Inkscape::Bitmap
