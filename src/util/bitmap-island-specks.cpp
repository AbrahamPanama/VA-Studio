// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/bitmap-island-specks.h"
#include <algorithm>
#include <array>
#include <cmath>
namespace Inkscape::Bitmap {
Result<OrthogonalMetric> OrthogonalMetric::fromDpi(double x, double y) noexcept
{
    Result<OrthogonalMetric> r;
    r.value._x = (300.0 / 25.4) * (x / 300.0);
    r.value._y = (300.0 / 25.4) * (y / 300.0);
    if (!r.value.valid()) {
        r.value = {}; r.outcome = {Status::incompatible, "Invalid orthogonal pixel metric"};
    }
    return r;
}
bool OrthogonalMetric::valid() const noexcept
{
    // Bounding grid coordinates by 32767 lets these checks exclude overflow and
    // underflow in every later squared distance/product, including anisotropy.
    auto ratio = _x / _y;
    return std::isfinite(_x) && std::isfinite(_y) && _x > 0 && _y > 0 &&
        std::isnormal(_x * _x) && std::isnormal(_y * _y) &&
        std::isnormal(2 * _x * _y) && std::isnormal(ratio * ratio) &&
        std::isfinite(2 * _x * _y) && std::isfinite(_x * _x) &&
        std::isfinite(ratio * ratio * 32767.0 * 32767.0 * 2);
}
namespace {
struct Box {
    std::uint32_t x, y, endX, endY, owner, id;
    std::uint32_t left = UINT32_MAX, right = UINT32_MAX;
};
struct Link {
    std::uint32_t parent, target, id, sourceId;
    double distance;
};
struct Poll {
    JobWork &work; Stop stop; PhaseTimer timer;
    Outcome outcome{}; std::uint64_t pending = 0;
    bool flush() noexcept {
        outcome = work.advance(pending); pending = 0;
        auto timed = timer.check(stop); if (!timed.ok()) outcome = timed;
        return outcome.ok();
    }
    bool tick() noexcept { ++pending; return pending < 256 || flush(); }
};
double gap(Box const &a, Box const &b, double scale) noexcept
{
    // Runs are unions of closed cells: adjacent cells have zero gap. All bounds
    // are half-open integers, and subtraction occurs only in the larger branch.
    double dx = a.x > b.endX ? a.x - b.endX : b.x > a.endX ? b.x - a.endX : 0;
    double dy = a.y > b.endY ? a.y - b.endY : b.y > a.endY ? b.y - a.endY : 0;
    dy *= scale; return dx * dx + dy * dy;
}
std::uint32_t root(Link *links, std::uint32_t a, Poll &poll) noexcept
{
    while (links[a].parent != a) {
        if (!poll.tick()) return exteriorPiece;
        links[a].parent = links[links[a].parent].parent; a = links[a].parent;
    }
    return a;
}
} // namespace
Result<std::uint64_t> speckWorkReserve(Partition const &in, OrthogonalMetric const &metric,
                                      JobWork &work, Stop stop) noexcept
{
    Result<std::uint64_t> result;
    Poll poll{work, stop, PhaseTimer{}};
    auto fail = [&](Outcome o) { poll.flush(); result.outcome = o; return result; };
    if (!metric.valid()) return fail({Status::incompatible, "Invalid orthogonal pixel metric"});
    std::uint64_t f = 0, queries = 0, base, term;
    unsigned levels = 0;
    for (auto n = in.runCount; n; n >>= 1) ++levels;
    // dy is a closed-cell gap; a run can reach ceil(y pixels/mm)+1 rows
    // on either side. Clip before conversion, including extreme anisotropy.
    auto radius = std::min(double(in.height), std::ceil(std::sqrt(metric.reachSquared()) / metric.yScale()) + 1);
    auto rows = std::min(std::uint64_t(in.height), 2 * std::uint64_t(radius) + 1);
    for (std::uint32_t i = 0; i < in.runCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto r = in.run(i); if (!r.foreground) continue;
        ++f;
        if (r.piece >= in.pieceCount) return fail({Status::incompatible, "Run owner out of bounds"});
        if (in.pieces()[r.piece].area <= metric.areaLimit()) ++queries;
    }
    // 5R includes this scan and rebuilds; 6F unpadded construction/query scans; region/id, unions,
    // output passes and sorting. Query allowance scales with rows and depth.
    if (!checkedMul(in.runCount, 5, base) || !checkedMul(f, 6, term) || !checkedAdd(base, term, base) ||
        !checkedMul(in.regionCount, 9 + 2 * levels, term) || !checkedAdd(base, term, base) ||
        !checkedMul(queries, rows, term) || !checkedMul(term, 2 * levels + 1, term) ||
        !checkedAdd(base, term, base) || !checkedAdd(base, in.height + 1, base))
        return fail({Status::failed, "Speck work reserve overflow"});
    if (!poll.flush()) return fail(poll.outcome);
    result.value = base; return result;
}
Result<Partition> attach(Partition const &in, OrthogonalMetric const &metric,
                         Budget &budget, JobWork &work, Stop stop) noexcept
{
    auto start = work.visits(); Poll poll{work, stop, PhaseTimer{}};
    Partition out; PlainBuffer linksBuffer, treeBuffer, regionIds;
    auto fail = [&](Outcome o) {
        poll.flush(); Result<Partition> r; r.outcome = o;
        r.consumed = work.visits() - start; return r;
    };
    if (!poll.flush()) return fail(poll.outcome);
    if (!metric.valid() || !in.width || !in.height || in.width > 32767 || in.height > 32767 ||
        in.runCount > 16000000 || in.pieceCount > 2000000 || !in.sourceRuns() ||
        !in.runOwners() || !in.owners() || !in.rows() || !in.pieceOffsets() ||
        (in.pieceCount && (!in.pieces() || !in.pieceRuns())))
        return fail({Status::incompatible, "Expected immutable enclosure Partition and orthogonal metric"});
    auto allocate = [&](PlainBuffer &b, std::uint64_t count, std::uint64_t size) {
        std::uint64_t bytes, total;
        if (!checkedMul(count, size, bytes) || !checkedAdd(budget.reserved(Stage::topology), bytes, total) ||
            total > 256 * MiB) { poll.outcome = {Status::failed, "Topology byte cap exceeded"}; return false; }
        poll.outcome = b.allocate(budget, Stage::topology, count, size, nullptr, stop);
        out.topologyPeak = std::max(out.topologyPeak, total); return poll.outcome.ok();
    };
    if (!allocate(linksBuffer, in.pieceCount, sizeof(Link)) ||
        !allocate(regionIds, in.regionCount, sizeof(std::uint32_t))) return fail(poll.outcome);
    auto links = reinterpret_cast<Link *>(linksBuffer.data());
    auto ids = reinterpret_cast<std::uint32_t *>(regionIds.data());
    bool any = false;
    for (std::uint32_t i = 0; i < in.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        links[i] = {i, exteriorPiece, UINT32_MAX, 0, metric.reachSquared()};
        if (!in.pieces()[i].id || in.pieces()[i].id > in.foregroundCount)
            return fail({Status::incompatible, "Piece discovery id out of bounds"});
        any |= in.pieces()[i].area <= metric.areaLimit();
    }
    for (std::uint32_t i = 0; i < in.regionCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        ids[i] = 0;
    }
    // Recover original discovery ids from the first source sample of each
    // region, not its sorted region index or post-enclosure piece index.
    std::uint32_t foregroundRuns = 0, nextId = 0;
    for (std::uint32_t i = 0; i < in.runCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto r = in.sourceRuns()[i];
        if (r.region < in.foregroundCount) {
            ++foregroundRuns; if (!ids[r.region]) ids[r.region] = ++nextId;
        }
    }
    if (any && in.pieceCount > 1) {
        // Balanced, implicit BVH over row-major foreground run rectangles. It
        // indexes closed cell gaps without expanding long runs into pixels.
        // Each node has a lower distance bound, smallest original id, and an
        // optional homogeneous owner for excluding whole source subtrees.
        // The tree's depth is <=24; no recursion or unmetered library sorting.
        if (!foregroundRuns) return fail({Status::incompatible, "Missing foreground runs"});
        if (!poll.flush()) return fail(poll.outcome);
        auto reserve = speckWorkReserve(in, metric, work, stop);
        if (!reserve.ok()) return fail(reserve.outcome);
        if (reserve.value > work.limit() - work.visits())
            return fail({Status::failed, "Speck work reserve exceeded"});
        // Preorder tree: exactly 2F-1 nodes, contiguous row-major subtrees.
        auto nodes = 2 * foregroundRuns - 1;
        if (!allocate(treeBuffer, nodes, sizeof(Box))) return fail(poll.outcome);
        auto tree = reinterpret_cast<Box *>(treeBuffer.data());
        struct Build { std::uint32_t node, count; };
        std::array<Build, 25> build{}; unsigned depth = 1;
        build[0] = {0, foregroundRuns}; std::uint32_t run = 0;
        while (depth) {
            if (!poll.tick()) return fail(poll.outcome);
            auto [node, count] = build[--depth];
            if (count == 1) {
                while (run < in.runCount) {
                    if (!poll.tick()) return fail(poll.outcome);
                    auto r = in.run(run++); if (!r.foreground) continue;
                    tree[node] = {r.x, r.y, r.end, r.y + 1, r.piece, ids[in.sourceRuns()[run-1].region]};
                    break;
                }
            } else {
                auto leftCount = count / 2;
                tree[node] = {0, 0, 0, 0, exteriorPiece, UINT32_MAX,
                              node + 1, node + 2 * leftCount};
                build[depth++] = {tree[node].right, count - leftCount};
                build[depth++] = {tree[node].left, leftCount};
            }
        }
        for (auto i = nodes; i;) {
            --i; if (!poll.tick()) return fail(poll.outcome);
            auto &v = tree[i]; if (v.left == UINT32_MAX) continue;
            auto a = tree[v.left], b = tree[v.right];
            v.x = std::min(a.x,b.x); v.y = std::min(a.y,b.y);
            v.endX = std::max(a.endX,b.endX); v.endY = std::max(a.endY,b.endY);
            v.owner = a.owner == b.owner ? a.owner : exteriorPiece; v.id = std::min(a.id,b.id);
        }
        auto scale = metric.yScale();
        for (std::uint32_t i = 0; i < nodes; ++i) {
            if (!poll.tick()) return fail(poll.outcome);
            auto query = tree[i]; if (query.left != UINT32_MAX) continue;
            auto owner = query.owner;
            if (in.pieces()[owner].area > metric.areaLimit()) continue;
            auto &best = links[owner];
            std::array<std::uint32_t, 25> stack{}; unsigned size = 1; stack[0] = 0;
            while (size) {
                if (!poll.tick()) return fail(poll.outcome);
                auto node = stack[--size]; auto b = tree[node];
                if (b.id == UINT32_MAX || b.owner == owner) continue;
                auto d = gap(query, b, scale);
                if (d > best.distance || (d == best.distance && b.id >= best.id)) continue;
                if (b.left == UINT32_MAX) {
                    best.target = b.owner; best.id = b.id; best.distance = d;
                } else {
                    auto left = b.left, right = b.right;
                    auto dl = gap(query, tree[left], scale), dr = gap(query, tree[right], scale);
                    // Visit nearer subtree first, ties by minimum source id.
                    bool firstLeft = dl < dr || (dl == dr && tree[left].id < tree[right].id);
                    stack[size++] = firstLeft ? right : left; stack[size++] = firstLeft ? left : right;
                }
            }
        }
    }
    treeBuffer.reset(); regionIds.reset();
    // Candidates are frozen before the first union. Root choice is minimum
    // discovery id; processing order cannot change the undirected union result.
    for (std::uint32_t i = 0; i < in.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        if (links[i].target == exteriorPiece) continue;
        auto a = root(links, i, poll), b = root(links, links[i].target, poll);
        if (a == exteriorPiece || b == exteriorPiece) return fail(poll.outcome);
        if (in.pieces()[a].id > in.pieces()[b].id) std::swap(a, b);
        links[b].parent = a;
    }
    for (std::uint32_t i = 0; i < in.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto a = root(links, i, poll); if (a == exteriorPiece) return fail(poll.outcome);
        links[i].parent = a; if (a == i) links[i].sourceId = out.pieceCount++;
    }
    out.speckJoins = in.pieceCount - out.pieceCount;
    for (std::uint32_t i = 0; i < in.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        if (in.pieces()[i].area <= metric.areaLimit() && links[i].target == exteriorPiece)
            ++out.retainedIsolatedSpecks;
    }
    if (out.pieceCount > 20000) return fail({Status::failed, "Final piece cap exceeded"});
    if (!allocate(out._pieces, out.pieceCount, sizeof(Piece))) return fail(poll.outcome);
    auto pieces = reinterpret_cast<Piece *>(out._pieces.data());
    for (std::uint32_t i = 0; i < in.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        if (links[i].parent == i) pieces[links[i].sourceId] = {in.pieces()[i].id, in.width, in.height, 0, 0, 0};
    }
    for (std::uint32_t i = 0; i < in.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto a = in.pieces()[i]; auto &b = pieces[links[links[i].parent].sourceId];
        b.x = std::min(b.x,a.x); b.y = std::min(b.y,a.y); b.endX = std::max(b.endX,a.endX);
        b.endY = std::max(b.endY,a.endY);
        if (!checkedAdd(b.area,a.area,b.area)) return fail({Status::failed, "Speck area overflow"});
    }
    if (!poll.flush()) return fail(poll.outcome);
    auto sorted = sortPieces(pieces, out.pieceCount, work, stop); if (!sorted.ok()) return fail(sorted);
    // Original IDs are sparse; use the source foreground count for id->index.
    if (!allocate(regionIds, std::uint64_t(in.foregroundCount) + 1, sizeof(std::uint32_t))) return fail(poll.outcome);
    ids = reinterpret_cast<std::uint32_t *>(regionIds.data());
    for (std::uint32_t i = 0; i < out.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        if (!pieces[i].id || pieces[i].id > in.foregroundCount)
            return fail({Status::incompatible, "Piece discovery id out of bounds"});
        ids[pieces[i].id] = i;
    }
    for (std::uint32_t i = 0; i < in.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        links[i].sourceId = ids[in.pieces()[links[i].parent].id];
    }
    regionIds.reset();
    if (!allocate(out._owners, in.regionCount, 4) || !allocate(out._runOwners, in.runCount, 4) ||
        !allocate(out._rows, std::uint64_t(in.height) + 1, 4) ||
        !allocate(out._pieceOffsets, std::uint64_t(out.pieceCount) + 1, 4) ||
        !allocate(out._pieceRuns, in.runCount, 4)) return fail(poll.outcome);
    auto owners = reinterpret_cast<std::uint32_t *>(out._owners.data());
    auto runOwners = reinterpret_cast<std::uint32_t *>(out._runOwners.data());
    auto rows = reinterpret_cast<std::uint32_t *>(out._rows.data());
    auto offsets = reinterpret_cast<std::uint32_t *>(out._pieceOffsets.data());
    auto list = reinterpret_cast<std::uint32_t *>(out._pieceRuns.data());
    for (std::uint32_t i = 0; i < in.regionCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto a = in.owners()[i]; owners[i] = a == exteriorPiece ? a : links[a].sourceId;
    }
    for (std::uint32_t i = 0; i <= out.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        offsets[i] = 0;
    }
    for (std::uint32_t i = 0; i < in.runCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto a = in.sourceRuns()[i]; runOwners[i] = owners[a.region];
        if (runOwners[i] != exteriorPiece) ++offsets[runOwners[i]];
    }
    for (std::uint32_t i = 0; i <= in.height; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        rows[i] = in.rows()[i];
    }
    for (std::uint32_t i = 1; i < out.pieceCount; ++i) {
        if (!poll.tick()) return fail(poll.outcome);
        offsets[i] += offsets[i-1];
    }
    if (out.pieceCount) offsets[out.pieceCount] = offsets[out.pieceCount-1];
    for (auto i = in.runCount; i; --i) {
        if (!poll.tick()) return fail(poll.outcome);
        auto a = runOwners[i-1]; if (a != exteriorPiece) list[--offsets[a]] = i-1;
    }
    out.width = in.width; out.height = in.height; out.runCount = in.runCount; out.regionCount = in.regionCount;
    out.foregroundCount = in.foregroundCount; out.enclosureMerges = in.enclosureMerges;
    out.boundaries = in.boundaries; out.graphEdges = in.graphEdges; out._sourceRuns = in.sourceRuns();
    out.topologyPeak = std::max(out.topologyPeak, in.topologyPeak);
    if (!poll.flush()) return fail(poll.outcome);
    Result<Partition> result; result.outcome = {Status::changed, ""};
    result.consumed = work.visits() - start; result.value = std::move(out); return result;
}
} // namespace Inkscape::Bitmap
