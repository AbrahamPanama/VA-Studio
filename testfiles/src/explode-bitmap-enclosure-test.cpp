// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "util/bitmap-island-enclosure.h"
#include <boost/json.hpp>
#include <glib.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <set>
#include <vector>
using namespace Inkscape::Bitmap;
namespace {
struct Image {
    unsigned w, h;
    std::vector<std::uint8_t> data;
    Image(unsigned x, unsigned y) : w(x), h(y), data(x * y * 4) {}
    void put(unsigned x, unsigned y, bool f) { data[(y * w + x) * 4 + 3] = f ? 255 : 0; }
    RgbaView view() const { return {data.data(), data.size(), w * 4ULL, w, h}; }
};
std::string read(std::string const &name)
{
    std::ifstream in(name, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}};
}
std::vector<unsigned> plane(Partition const &p, bool holes = false)
{
    std::vector<unsigned> ids(p.width * p.height, UINT32_MAX);
    std::vector<bool> seen(ids.size());
    std::vector<std::uint64_t> area(p.pieceCount);
    for (unsigned i = 0; i < p.runCount; ++i) {
        auto r = p.run(i); EXPECT_LT(r.x, r.end); EXPECT_LE(r.end, p.width); EXPECT_LT(r.y, p.height);
        if (r.piece != exteriorPiece) EXPECT_LT(r.piece, p.pieceCount);
        EXPECT_TRUE(!r.foreground || r.piece != exteriorPiece);
        for (unsigned x = r.x; x < r.end; ++x) {
            auto at = r.y * p.width + x; EXPECT_FALSE(seen[at]); seen[at] = true;
            ids[at] = r.piece == exteriorPiece || (!holes && !r.foreground) ? 0 : p.pieces()[r.piece].id;
            if (r.foreground) ++area[r.piece];
        }
    }
    EXPECT_TRUE(std::all_of(seen.begin(), seen.end(), [](bool v) { return v; }));
    for (unsigned i = 0; i < p.pieceCount; ++i) EXPECT_EQ(area[i], p.pieces()[i].area);
    return ids;
}
// Independent dense pixel flood + set graph. It neither consumes Regions nor
// shares the run-contact algorithm. Parent chains walk TWO edges per foreground
// ancestor, exercising repeated grandparent ownership at arbitrary nesting depth.
std::vector<unsigned> reference(Image const &im, AlphaLut const &lut, bool holes)
{
    unsigned n = im.w * im.h;
    std::vector<int> labels(n, -1);
    std::vector<bool> foreground{false}, exterior{true}; std::vector<unsigned> ids{0};
    auto visible = [&](unsigned p) { return lut[im.data[p * 4 + 3]] != 0; };
    unsigned fg = 0;
    for (unsigned seed = 0; seed < n; ++seed) {
        if (labels[seed] >= 0) continue;
        unsigned node = foreground.size(); bool f = visible(seed), border = false;
        std::vector<unsigned> q{seed}; labels[seed] = node;
        for (unsigned at = 0; at < q.size(); ++at) {
            int x = q[at] % im.w, y = q[at] / im.w;
            border |= !x || !y || unsigned(x + 1) == im.w || unsigned(y + 1) == im.h;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if ((!dx && !dy) || (!f && dx && dy)) continue;
                int xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || unsigned(xx) >= im.w || unsigned(yy) >= im.h) continue;
                unsigned p = yy * im.w + xx;
                if (labels[p] < 0 && visible(p) == f) { labels[p] = node; q.push_back(p); }
            }
        }
        foreground.push_back(f); exterior.push_back(border); ids.push_back(f ? ++fg : 0);
    }
    std::vector<std::set<unsigned>> graph(foreground.size());
    auto edge = [&](unsigned a, unsigned b) { if (a != b) { graph[a].insert(b); graph[b].insert(a); } };
    auto node = [&](unsigned p) { auto a = labels[p]; return !foreground[a] && exterior[a] ? 0u : unsigned(a); };
    for (unsigned p = 0; p < n; ++p) {
        auto a = node(p); unsigned x = p % im.w, y = p / im.w;
        if (!x || !y || x + 1 == im.w || y + 1 == im.h) edge(0, a);
        if (x) edge(a, node(p - 1)); if (y) edge(a, node(p - im.w));
    }
    std::vector<int> depth(graph.size(), -1); std::vector<unsigned> prev(graph.size());
    std::vector<unsigned> todo{0}; depth[0] = 0;
    for (unsigned at = 0; at < todo.size(); ++at) for (auto b : graph[todo[at]]) if (depth[b] < 0) {
        depth[b] = depth[todo[at]] + 1; prev[b] = todo[at]; todo.push_back(b);
    }
    std::vector<unsigned> owner(graph.size()), minimum(graph.size(), UINT32_MAX);
    for (unsigned a = 1; a < graph.size(); ++a) if (foreground[a]) {
        unsigned root = a; while (depth[root] > 1) root = prev[prev[root]];
        owner[a] = root; minimum[root] = std::min(minimum[root], ids[a]);
    }
    std::vector<unsigned> result(n);
    for (unsigned p = 0; p < n; ++p) {
        auto a = node(p);
        if (a && foreground[a]) result[p] = minimum[owner[a]];
        else if (a && holes) result[p] = minimum[owner[prev[a]]];
    }
    return result;
}
Image pattern(unsigned size, unsigned kind)
{
    Image im(size, size);
    for (unsigned y = 0; y < size; ++y) for (unsigned x = 0; x < size; ++x) {
        auto d = std::min({x, y, size - 1 - x, size - 1 - y});
        bool f = kind == 0 ? d % 2 == 1 : kind == 1 ?
            (x % 4 == 0 || (y % 8 == 0 && x % 8 < 4) || (y % 8 == 4 && x % 8 >= 4)) :
            kind == 2 ? (y % 2 == 0 || (y % 4 == 1 ? x == size - 1 : x == 0)) :
            kind == 3 ? (x + y) % 2 == 0 : x % 2 == 0 && y % 2 == 0;
        im.put(x, y, f);
    }
    return im;
}
}
TEST(ExplodeBitmapEnclosure, T22OracleAllFixturesSettingsAndDpi)
{
    std::string dir = std::string(INKSCAPE_TESTS_DIR) + "/explode-bitmap/";
    std::string fixtures = dir + "fixtures/pam";
    auto text = read(dir + "oracle/enclosure-regions.json");
    ASSERT_FALSE(text.empty());
    auto suite = boost::json::parse(text).as_array(); ASSERT_EQ(suite.size(), 368u);
    for (auto const &entry : suite) {
        auto const &e = entry.as_object(); auto name = std::string(e.at("file").as_string()); SCOPED_TRACE(name);
        Image im(e.at("width").as_int64(), e.at("height").as_int64());
        auto pam = read(fixtures + "/" + name); auto at = pam.find("ENDHDR\n");
        ASSERT_NE(at, std::string::npos); at += 7; ASSERT_EQ(pam.size() - at, im.data.size());
        std::copy(pam.begin() + at, pam.end(), im.data.begin()); auto saved = im.data;
        auto lut = alphaLut(e.at("T").as_int64(), e.at("S").as_int64());
        Budget b(Budget::FixedLimitForTest{}, 256 * MiB); JobWork work(im.w * im.h);
        auto regions = label(im.view(), lut, b, work); ASSERT_TRUE(regions.ok());
        auto before = work.visits(); auto result = enclose(regions.value, b, work);
        ASSERT_TRUE(result.ok()) << result.outcome.diagnostic; EXPECT_EQ(result.consumed, work.visits() - before);
        auto const &p = result.value; auto labels = plane(p); plane(p, true);
        EXPECT_EQ(labels, reference(im, lut, false)); EXPECT_EQ(plane(p, true), reference(im, lut, true));
        auto const &pieces = e.at("pieces").as_array(); ASSERT_EQ(p.pieceCount, pieces.size());
        for (unsigned i = 0; i < pieces.size(); ++i) {
            auto const &expected = pieces[i].as_object(); auto v = p.pieces()[i];
            EXPECT_EQ(v.id, expected.at("id").as_int64()); EXPECT_EQ(v.area, expected.at("area").as_int64());
            auto const &box = expected.at("bbox").as_array(); EXPECT_EQ(v.x, box[0].as_int64()); EXPECT_EQ(v.y, box[1].as_int64());
            EXPECT_EQ(v.endX - 1, box[2].as_int64()); EXPECT_EQ(v.endY - 1, box[3].as_int64());
            std::vector<unsigned> rle; bool current = false; unsigned len = 0;
            for (unsigned y = v.y; y < v.endY; ++y) for (unsigned x = v.x; x < v.endX; ++x) {
                bool member = labels[y * im.w + x] == v.id;
                if (member != current) { rle.push_back(len); len = 0; current = member; } ++len;
            }
            rle.push_back(len); auto const &runs = expected.at("rle").as_array(); ASSERT_EQ(rle.size(), runs.size());
            for (unsigned j = 0; j < runs.size(); ++j) EXPECT_EQ(rle[j], runs[j].as_int64());
        }
        EXPECT_EQ(saved, im.data);
    }
}
TEST(ExplodeBitmapEnclosure, T22FixedSeedProperties)
{
    auto started = std::chrono::steady_clock::now();
    for (unsigned seed : {0xEB122u, 0xEB127u, 20261001u}) {
        std::mt19937 random(seed);
        for (unsigned i = 0; i < 100; ++i) {
            Image im(1 + random() % 19, 1 + random() % 19);
            for (unsigned p = 3; p < im.data.size(); p += 4) im.data[p] = random() % 256;
            auto lut = alphaLut(random() % 256, random() % 128);
            Budget b(Budget::FixedLimitForTest{}, 4 * MiB); JobWork work(im.w * im.h);
            auto r = label(im.view(), lut, b, work); ASSERT_TRUE(r.ok());
            auto p = enclose(r.value, b, work); ASSERT_TRUE(p.ok()) << p.outcome.diagnostic;
            EXPECT_EQ(plane(p.value), reference(im, lut, false)); EXPECT_EQ(plane(p.value, true), reference(im, lut, true));
        }
    }
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), 3);
}
TEST(ExplodeBitmapEnclosure, T27DeepRingsLabyrinthSerpentineCheckerboards)
{
    for (unsigned kind = 0; kind < 5; ++kind) {
        auto im = pattern(kind == 0 ? 2003 : 512, kind); auto lut = alphaLut(128, 0);
        Budget b(Budget::FixedLimitForTest{}, 256 * MiB); JobWork work(im.w * im.h);
        auto r = label(im.view(), lut, b, work); ASSERT_TRUE(r.ok());
        auto start = std::chrono::steady_clock::now(); auto p = enclose(r.value, b, work);
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        ASSERT_TRUE(p.ok()) << p.outcome.diagnostic; EXPECT_LT(elapsed, 30); EXPECT_LE(work.visits(), work.limit());
        std::cout << "T27 kind=" << kind << " seconds=" << elapsed << " visits=" << p.consumed
                  << " runs=" << r.value.runCount << " regions=" << r.value.islandCount
                  << " contacts=" << p.value.boundaries << " unique=" << p.value.graphEdges
                  << " job=" << work.visits() << " peak=" << p.value.topologyPeak << '\n';
        if (kind == 0) { EXPECT_GT(r.value.foregroundCount, 500u); EXPECT_EQ(p.value.pieceCount, 1u); }
        // Large input stays run-based; independent brute reference on a smaller
        // representative of each adversarial shape keeps the test bounded.
        auto small = pattern(65, kind); Budget sb(Budget::FixedLimitForTest{}, 4 * MiB); JobWork sw(65 * 65);
        auto sr = label(small.view(), lut, sb, sw); ASSERT_TRUE(sr.ok()); auto sp = enclose(sr.value, sb, sw);
        ASSERT_TRUE(sp.ok()); EXPECT_EQ(plane(sp.value, true), reference(small, lut, true));
    }
}
TEST(ExplodeBitmapEnclosure, T03CapsAndBudgetMinusZeroPlusOne)
{
    auto im = pattern(19, 4); auto lut = alphaLut(128, 0);
    std::uint64_t peak, count; unsigned nodes;
    {
        Budget b(Budget::FixedLimitForTest{}, 4 * MiB); JobWork work(im.w * im.h);
        auto r = label(im.view(), lut, b, work); ASSERT_TRUE(r.ok()); nodes = r.value.islandCount + 1;
        auto p = enclose(r.value, b, work); ASSERT_TRUE(p.ok()); peak = p.value.topologyPeak; count = p.value.boundaries;
    }
    for (int delta : {-1, 0, 1}) for (unsigned cap = 0; cap < 6; ++cap) {
        Budget b(Budget::FixedLimitForTest{}, cap == 5 ? peak + delta : 4 * MiB); JobWork work(im.w * im.h);
        auto r = label(im.view(), lut, b, work); ASSERT_TRUE(r.ok()); auto baseline = b.reserved();
        EnclosureOptions o;
        if (cap == 0) o.nodes = nodes + delta; if (cap == 1) o.edges = count + delta;
        if (cap == 2) o.queue = nodes + delta; if (cap == 3) o.boundaries = count + delta;
        if (cap == 4) o.topologyBytes = peak + delta;
        { auto before = work.visits();
          auto p = enclose(r.value, b, work, {}, o);
          EXPECT_EQ(p.consumed, work.visits() - before); EXPECT_EQ(p.ok(), delta >= 0) << "cap=" << cap;
          if (!p.ok()) {
              char const *messages[] = {"Graph node cap exceeded", "Graph edge cap exceeded",
                  "Graph queue cap exceeded", "Boundary contact cap exceeded", "Topology byte cap exceeded",
                  "Reservation exceeds current operation/stage budget"};
              EXPECT_STREQ(p.outcome.diagnostic, messages[cap]);
              EXPECT_EQ(p.value.sourceRuns(), nullptr); EXPECT_EQ(p.value.pieceCount, 0u);
          } }
        EXPECT_EQ(b.reserved(), baseline);
    }
    Budget b(Budget::FixedLimitForTest{}, 4 * MiB); JobWork work(im.w * im.h);
    auto r = label(im.view(), lut, b, work); ASSERT_TRUE(r.ok()); Budget::Token held;
    ASSERT_TRUE(b.acquire(Stage::topology, 1024, held).ok()); EnclosureOptions o; o.topologyBytes = peak + 1023;
    EXPECT_FALSE(enclose(r.value, b, work, {}, o).ok());
}
TEST(ExplodeBitmapEnclosure, T03AllocationSweepSharedWorkAndMoves)
{
    auto im = pattern(19, 4); Budget b(Budget::FixedLimitForTest{}, 4 * MiB); JobWork work(im.w * im.h);
    auto r = label(im.view(), alphaLut(128, 0), b, work); ASSERT_TRUE(r.ok()); auto baseline = b.reserved();
    for (unsigned k = 1; k <= 10; ++k) {
        AllocationFault fault{k, 0}; EnclosureOptions o; o.fault = &fault;
        { auto before = work.visits();
          auto p = enclose(r.value, b, work, {}, o); EXPECT_EQ(p.ok(), k == 10);
          EXPECT_EQ(p.consumed, work.visits() - before);
          EXPECT_EQ(fault.attempts, std::min(k, 9u)); if (k < 10) EXPECT_EQ(p.value.owners(), nullptr); }
        EXPECT_EQ(b.reserved(), baseline);
    }
    { auto p = enclose(r.value, b, work); ASSERT_TRUE(p.ok()); auto live = b.reserved();
      Partition moved = std::move(p.value); EXPECT_EQ(p.value.sourceRuns(), nullptr); EXPECT_EQ(b.reserved(), live);
      EXPECT_EQ(p.value.runCount, 0u); EXPECT_EQ(p.value.pieceCount, 0u);
      EXPECT_EQ(p.value.foregroundCount, 0u); EXPECT_EQ(p.value.enclosureMerges, 0u);
      EXPECT_EQ(p.value.width, 0u); EXPECT_EQ(p.value.height, 0u); EXPECT_EQ(p.value.regionCount, 0u);
      Partition assigned; assigned = std::move(moved); EXPECT_EQ(moved.runCount, 0u);
      EXPECT_EQ(moved.pieceCount, 0u); EXPECT_EQ(moved.sourceRuns(), nullptr); plane(assigned); }
    EXPECT_EQ(b.reserved(), baseline);
    r.value = Regions{}; EXPECT_EQ(b.reserved(), 0u);
    auto r2 = label(im.view(), alphaLut(128, 0), b, work); ASSERT_TRUE(r2.ok()); baseline = b.reserved();
    ASSERT_TRUE(work.advance(work.limit() - work.visits()).ok());
    auto p = enclose(r2.value, b, work); EXPECT_FALSE(p.ok()); EXPECT_EQ(p.consumed, 0u);
    EXPECT_EQ(p.value.sourceRuns(), nullptr); EXPECT_EQ(b.reserved(), baseline);
    for (unsigned cap = 0; cap < 6; ++cap) {
        EnclosureOptions o; if (cap == 0) ++o.nodes; if (cap == 1) ++o.edges; if (cap == 2) ++o.queue;
        if (cap == 3) ++o.boundaries; if (cap == 4) ++o.topologyBytes;
        JobWork fresh(im.w * im.h); Regions empty;
        EXPECT_FALSE(enclose(cap == 5 ? empty : r2.value, b, fresh, {}, o).ok());
    }
}
TEST(ExplodeBitmapEnclosure, T03CancellationAcknowledgedAcrossPhases)
{
    auto im = pattern(512, 4); Budget b(Budget::FixedLimitForTest{}, 256 * MiB); JobWork work(im.w * im.h);
    auto r = label(im.view(), alphaLut(128, 0), b, work); ASSERT_TRUE(r.ok()); auto baseline = b.reserved();
    struct Injection { EnclosurePhase target; std::shared_ptr<std::atomic<bool>> flag;
        std::chrono::steady_clock::time_point at{}; bool seen = false; };
    for (auto phase : {EnclosurePhase::graph, EnclosurePhase::traversal, EnclosurePhase::partition, EnclosurePhase::sorting}) {
        Injection state{phase, std::make_shared<std::atomic<bool>>(false)}; EnclosureOptions o; o.observerData = &state;
        o.observe = [](EnclosurePhase current, void *data) noexcept {
            auto &s = *static_cast<Injection *>(data); if (current == s.target) {
                s.seen = true; s.at = std::chrono::steady_clock::now(); s.flag->store(true); }
        };
        auto before = work.visits();
        auto p = enclose(r.value, b, work, Stop(state.flag), o); auto returned = std::chrono::steady_clock::now();
        EXPECT_EQ(p.consumed, work.visits() - before);
        ASSERT_TRUE(state.seen); EXPECT_EQ(p.outcome.status, Status::canceled);
        EXPECT_LT(returned - state.at, std::chrono::milliseconds(100)); EXPECT_EQ(p.value.sourceRuns(), nullptr);
        EXPECT_EQ(b.reserved(), baseline);
        std::cout << "T03 cancellation phase=" << unsigned(phase) << " ms="
                  << std::chrono::duration<double, std::milli>(returned - state.at).count() << '\n';
    }
}
TEST(ExplodeBitmapEnclosure, T22BorderHolesAndNoProximityContainment)
{
    // A diagonal escape is not a background escape: the centre is a 4-connected
    // hole even though it touches the exterior diagonally at the upper left.
    Image im(9, 9);
    for (unsigned y = 0; y < 9; ++y) for (unsigned x = 0; x < 9; ++x) im.put(x, y, true);
    im.put(0, 0, false); im.put(1, 1, false); im.put(7, 7, false);
    auto lut = alphaLut(128, 0); Budget b(Budget::FixedLimitForTest{}, 4 * MiB); JobWork work(81);
    auto r = label(im.view(), lut, b, work); ASSERT_TRUE(r.ok()); auto p = enclose(r.value, b, work);
    ASSERT_TRUE(p.ok()); EXPECT_EQ(p.value.pieceCount, 1u);
    auto holes = plane(p.value, true); EXPECT_EQ(holes[0], 0u); EXPECT_EQ(holes[10], 1u); EXPECT_EQ(holes[70], 1u);
    EXPECT_EQ(plane(p.value)[10], 0u); // ownership never turns a hole opaque
    // Open contour and a nearby isolated pixel. Closed-cell distance cannot close
    // the contour or pull in the pixel; enclosure has no distance parameter.
    Image open(11, 11);
    for (unsigned y = 1; y < 10; ++y) for (unsigned x = 1; x < 10; ++x)
        if (x == 1 || x == 9 || y == 1 || y == 9) open.put(x, y, true);
    open.put(5, 1, false); open.put(5, 3, true);
    JobWork ow(121); auto ro = label(open.view(), lut, b, ow); ASSERT_TRUE(ro.ok()); auto po = enclose(ro.value, b, ow);
    ASSERT_TRUE(po.ok()); EXPECT_EQ(po.value.pieceCount, 2u);
    EXPECT_EQ(plane(po.value, true), reference(open, lut, true)); EXPECT_EQ(plane(po.value, true)[5 * 11 + 5], 0u);
}
TEST(ExplodeBitmapEnclosure, T22DegenerateAndDeterministicBboxOrder)
{
    auto lut = alphaLut(128, 0);
    for (auto dims : {std::pair{1u, 1u}, {1u, 16384u}, {16384u, 1u}}) for (bool f : {false, true}) {
        Image im(dims.first, dims.second);
        for (unsigned y = 0; y < im.h; ++y) for (unsigned x = 0; x < im.w; ++x) im.put(x, y, f);
        Budget b(Budget::FixedLimitForTest{}, 4 * MiB); JobWork work(im.w * im.h);
        auto r = label(im.view(), lut, b, work); ASSERT_TRUE(r.ok()); auto p = enclose(r.value, b, work);
        ASSERT_TRUE(p.ok()); EXPECT_EQ(p.value.pieceCount, f ? 1u : 0u);
        EXPECT_EQ(plane(p.value, true), reference(im, lut, true));
    }
    Image im(8, 5); im.put(3, 0, true); im.put(6, 0, true); im.put(6, 1, true); im.put(6, 2, true);
    for (unsigned x = 0; x <= 6; ++x) im.put(x, 3, true);
    Budget b(Budget::FixedLimitForTest{}, 4 * MiB); JobWork work(40);
    auto r = label(im.view(), lut, b, work); ASSERT_TRUE(r.ok()); auto p = enclose(r.value, b, work);
    ASSERT_TRUE(p.ok()); ASSERT_EQ(p.value.pieceCount, 2u);
    EXPECT_EQ(p.value.pieces()[0].id, 2u); EXPECT_EQ(p.value.pieces()[1].id, 1u);
    EXPECT_EQ(plane(p.value), reference(im, lut, false));
    auto again = enclose(r.value, b, work); ASSERT_TRUE(again.ok());
    EXPECT_EQ(plane(p.value, true), plane(again.value, true));
}

TEST(ExplodeBitmapEnclosure, TwoDeepPiecesIndexesSortAndVisitShare)
{
    Image im(27, 13); // two separate 11x11 rings with depth-3 islands, then holes
    for (unsigned origin : {1u, 15u}) for (unsigned y = 1; y < 12; ++y)
        for (unsigned x = 0; x < 11; ++x) {
            auto depth = std::min({x, 10 - x, y - 1, 11 - y});
            im.put(origin + x, y, depth % 2 == 0);
        }
    Budget b(Budget::FixedLimitForTest{}, 4 * MiB); JobWork work(im.w * im.h);
    auto r = label(im.view(), alphaLut(128, 0), b, work); ASSERT_TRUE(r.ok());
    auto p = enclose(r.value, b, work); ASSERT_TRUE(p.ok()); auto const &v = p.value;
    ASSERT_EQ(v.pieceCount, 2u); EXPECT_EQ(v.foregroundCount, 6u); EXPECT_EQ(v.enclosureMerges, 4u);
    auto owned = plane(v, true);
    for (unsigned origin : {1u, 15u}) for (unsigned y = 1; y < 12; ++y)
        for (unsigned x = 0; x < 11; ++x) EXPECT_EQ(owned[y * im.w + origin + x], origin == 1 ? 1u : 2u);
    EXPECT_EQ(owned[6 * im.w + 13], 0u); EXPECT_EQ(plane(v), reference(im, alphaLut(128, 0), false));
    EXPECT_LT(v.graphEdges, v.boundaries);
    for (unsigned y = 0; y < v.height; ++y) {
        EXPECT_LT(v.rows()[y], v.rows()[y + 1]);
        for (auto i = v.rows()[y]; i < v.rows()[y + 1]; ++i) EXPECT_EQ(v.run(i).y, y);
    }
    EXPECT_EQ(v.rows()[v.height], v.runCount);
    std::vector<bool> indexed(v.runCount);
    for (unsigned i = 0; i < v.pieceCount; ++i)
        for (auto at = v.pieceOffsets()[i]; at < v.pieceOffsets()[i + 1]; ++at) {
            auto run = v.pieceRuns()[at]; ASSERT_LT(run, v.runCount);
            EXPECT_FALSE(indexed[run]); indexed[run] = true; EXPECT_EQ(v.run(run).piece, i);
        }
    for (unsigned i = 0; i < v.runCount; ++i) EXPECT_EQ(indexed[i], v.run(i).piece != exteriorPiece);
    Piece keys[] = {{3,2,1,3,2,1}, {1,2,1,3,2,1}, {2,0,2,1,3,1}};
    auto before = work.visits(); ASSERT_TRUE(sortPieces(keys, 3, work).ok()); EXPECT_GT(work.visits(), before);
    EXPECT_EQ(keys[0].id, 1u); EXPECT_EQ(keys[1].id, 3u); EXPECT_EQ(keys[2].id, 2u);
    for (int delta : {-1, 0, 1}) {
        EnclosureOptions o; o.visits = p.consumed + delta; JobWork fresh(im.w * im.h);
        auto out = enclose(r.value, b, fresh, {}, o); EXPECT_EQ(out.ok(), delta >= 0);
        EXPECT_LE(out.consumed, o.visits);
        if (!out.ok()) EXPECT_STREQ(out.outcome.diagnostic, "Enclosure visit cap exceeded");
    }
    JobWork shared(im.w * im.h); ASSERT_TRUE(shared.advance(shared.limit() - 20000005).ok());
    auto refused = enclose(r.value, b, shared); EXPECT_FALSE(refused.ok()); EXPECT_EQ(refused.consumed, 5u);
    EXPECT_STREQ(refused.outcome.diagnostic, "Enclosure visit cap exceeded");
    EXPECT_EQ(shared.limit() - shared.visits(), 20000000u);
}
