// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "util/bitmap-island-specks.h"
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <random>
#include <thread>
#include <vector>
using namespace Inkscape::Bitmap;
namespace {
using Clock = std::chrono::steady_clock;
struct Image {
    unsigned w, h; std::vector<std::uint8_t> data;
    Image(unsigned x, unsigned y) : w(x), h(y), data(std::size_t(x) * y * 4) {}
    void put(unsigned x, unsigned y) { data[(std::size_t(y) * w + x) * 4 + 3] = 255; }
    void block(unsigned x, unsigned y, unsigned width, unsigned height) {
        for (unsigned yy = y; yy < y + height; ++yy)
            for (unsigned xx = x; xx < x + width; ++xx) put(xx, yy);
    }
    RgbaView view() const { return {data.data(), data.size(), w * 4ULL, w, h}; }
};
std::string read(std::filesystem::path const &p)
{
    std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}};
}
std::vector<unsigned> plane(Partition const &p, bool holes = false)
{
    std::vector<unsigned> result(std::size_t(p.width) * p.height, UINT32_MAX);
    std::vector<unsigned> coverage(result.size()), indexed(p.runCount);
    std::vector<std::uint64_t> areas(p.pieceCount);
    for (unsigned i = 0; i < p.runCount; ++i) {
        auto r = p.run(i); EXPECT_LT(r.x, r.end); EXPECT_LE(r.end, p.width);
        EXPECT_LT(r.y, p.height); EXPECT_EQ(r.piece, p.owners()[p.sourceRuns()[i].region]);
        if (r.foreground) EXPECT_NE(r.piece, exteriorPiece);
        for (unsigned x = r.x; x < r.end; ++x) {
            auto at = r.y * p.width + x; ++coverage[at];
            result[at] = r.piece == exteriorPiece || (!r.foreground && !holes) ? 0 : p.pieces()[r.piece].id;
            if (r.foreground) ++areas[r.piece];
        }
    }
    for (auto v : coverage) EXPECT_EQ(v, 1u);
    for (unsigned i = 0; i < p.pieceCount; ++i) {
        EXPECT_EQ(areas[i], p.pieces()[i].area);
        if (i) EXPECT_TRUE(pieceLess(p.pieces()[i-1], p.pieces()[i]));
        for (auto at = p.pieceOffsets()[i]; at < p.pieceOffsets()[i+1]; ++at) {
            auto r = p.pieceRuns()[at]; EXPECT_LT(r, p.runCount);
            ++indexed[r]; EXPECT_EQ(p.run(r).piece, i);
        }
    }
    for (unsigned i = 0; i < p.runCount; ++i) EXPECT_EQ(indexed[i], p.run(i).piece == exteriorPiece ? 0u : 1u);
    for (unsigned y = 0; y < p.height; ++y)
        for (auto at = p.rows()[y]; at < p.rows()[y+1]; ++at) EXPECT_EQ(p.run(at).y, y);
    EXPECT_EQ(p.rows()[p.height], p.runCount);
    return result;
}
// Independent all-cell-pairs reference: no spatial tree or run-gap code. The
// frozen enclosure is the stage input; dense foreground discovery floods recover
// the target-cell tie key independently of source region indices.
std::vector<unsigned> brute(Partition const &p, OrthogonalMetric const &metric, bool holes = false)
{
    unsigned n = p.width * p.height;
    std::vector<unsigned> owner(n, exteriorPiece), original(n), parent(p.pieceCount), target(p.pieceCount, exteriorPiece);
    for (unsigned i = 0; i < p.runCount; ++i) {
        auto r = p.run(i);
        for (unsigned x = r.x; x < r.end; ++x) if (r.foreground || holes) owner[r.y*p.width+x] = r.piece;
    }
    unsigned id = 0;
    for (unsigned at = 0; at < n; ++at) {
        if (owner[at] == exteriorPiece || original[at]) continue;
        // Holes do not enter the original foreground flood.
        bool foreground = false;
        for (auto r = p.rows()[at/p.width]; r < p.rows()[at/p.width+1]; ++r) {
            auto v = p.run(r); if (at%p.width >= v.x && at%p.width < v.end) foreground = v.foreground;
        }
        if (!foreground) continue;
        std::vector<unsigned> q{at}; original[at] = ++id;
        for (unsigned k = 0; k < q.size(); ++k) {
            int x = q[k] % p.width, y = q[k] / p.width;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                int xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || unsigned(xx) >= p.width || unsigned(yy) >= p.height) continue;
                auto next = unsigned(yy) * p.width + unsigned(xx);
                if (original[next] || owner[next] == exteriorPiece) continue;
                bool f = false;
                for (auto r = p.rows()[yy]; r < p.rows()[yy+1]; ++r) {
                    auto v = p.run(r); if (unsigned(xx) >= v.x && unsigned(xx) < v.end) f = v.foreground;
                }
                if (f) { original[next] = id; q.push_back(next); }
            }
        }
    }
    for (unsigned a = 0; a < p.pieceCount; ++a) {
        if (p.pieces()[a].area > metric.areaLimit()) continue;
        double best = metric.reachSquared(); unsigned key = UINT32_MAX;
        for (unsigned u = 0; u < n; ++u) if (original[u] && owner[u] == a)
            for (unsigned v = 0; v < n; ++v) if (original[v] && owner[v] != a) {
                double dx = std::max(0, std::abs(int(u%p.width)-int(v%p.width))-1);
                double dy = std::max(0, std::abs(int(u/p.width)-int(v/p.width))-1) * metric.yScale();
                double d = dx*dx + dy*dy;
                if (d < best || (d == best && original[v] < key)) {
                    best = d; key = original[v]; target[a] = owner[v];
                }
            }
    }
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](unsigned a) { while (parent[a] != a) a = parent[a]; return a; };
    for (unsigned i = 0; i < p.pieceCount; ++i) if (target[i] != exteriorPiece) {
        auto a = root(i), b = root(target[i]);
        if (p.pieces()[a].id > p.pieces()[b].id) std::swap(a,b); parent[b] = a;
    }
    std::vector<unsigned> result(n);
    for (unsigned i = 0; i < n; ++i) if (owner[i] != exteriorPiece && (holes || original[i]))
        result[i] = p.pieces()[root(owner[i])].id;
    return result;
}
struct Job {
    Budget budget{Budget::FixedLimitForTest{}, 256*MiB}; JobWork work;
    Result<Regions> regions; Result<Partition> enclosed;
    explicit Job(Image const &im, AlphaLut lut = alphaLut(128,0)) : work(std::uint64_t(im.w)*im.h) {
        regions = label(im.view(), lut, budget, work);
        if (regions.ok()) enclosed = enclose(regions.value, budget, work);
    }
};
void compare(Image const &im, OrthogonalMetric metric)
{
    Job j(im); ASSERT_TRUE(j.regions.ok()); ASSERT_TRUE(j.enclosed.ok());
    auto before = plane(j.enclosed.value, true); auto visits = j.work.visits();
    auto out = attach(j.enclosed.value, metric, j.budget, j.work);
    ASSERT_TRUE(out.ok()) << out.outcome.diagnostic;
    EXPECT_EQ(out.consumed, j.work.visits()-visits);
    EXPECT_EQ(plane(out.value), brute(j.enclosed.value,metric));
    EXPECT_EQ(plane(out.value,true), brute(j.enclosed.value,metric,true));
    EXPECT_EQ(before, plane(j.enclosed.value,true));
    EXPECT_EQ(out.value.sourceRuns(), j.regions.value.runs());
    EXPECT_EQ(out.value.foregroundCount, j.enclosed.value.foregroundCount);
    EXPECT_EQ(out.value.enclosureMerges, j.enclosed.value.enclosureMerges);
}
} // namespace
TEST(ExplodeBitmapSpecks, T22CommittedFinalOracle)
{
    std::filesystem::path dir = std::string(INKSCAPE_TESTS_DIR) + "/explode-bitmap/fixtures";
    unsigned cases = 0; std::vector<std::filesystem::path> files;
    for (auto const &f : std::filesystem::directory_iterator(dir / "expected"))
        if (f.path().extension() == ".json") files.push_back(f.path());
    std::sort(files.begin(),files.end());
    for (auto const &file : files) {
        SCOPED_TRACE(file.filename().string()); auto e = boost::json::parse(read(file)).as_object();
        Image im(e.at("width").as_int64(),e.at("height").as_int64());
        auto name = file.filename().string(); name = name.substr(0,name.find(".T"));
        auto pam = read(dir / "pam" / (name + ".pam")); auto at = pam.find("ENDHDR\n");
        ASSERT_NE(at,std::string::npos); at += 7; ASSERT_EQ(pam.size()-at,im.data.size());
        std::copy(pam.begin()+at,pam.end(),im.data.begin()); auto saved = im.data;
        auto const &dpi = e.at("dpi"); double d = dpi.is_double() ? dpi.as_double() : dpi.as_int64();
        auto metric = OrthogonalMetric::fromDpi(d,d); ASSERT_TRUE(metric.ok());
        Job j(im,alphaLut(e.at("T").as_int64(),e.at("S").as_int64()));
        ASSERT_TRUE(j.enclosed.ok()) << j.enclosed.outcome.diagnostic;
        auto result = attach(j.enclosed.value,metric.value,j.budget,j.work);
        ASSERT_TRUE(result.ok()) << result.outcome.diagnostic; auto const &p = result.value;
        auto labels = plane(p); plane(p,true);
        auto expected = e.at("pieces").as_array(); ASSERT_EQ(p.pieceCount,expected.size());
        EXPECT_EQ(p.pieceCount,e.at("piece_count").as_int64());
        for (unsigned i = 0; i < p.pieceCount; ++i) {
            auto const &v = p.pieces()[i]; auto const &entry = expected[i].as_object();
            EXPECT_EQ(v.id,entry.at("id").as_int64()); EXPECT_EQ(v.area,entry.at("area").as_int64());
            auto box = entry.at("bbox").as_array(); EXPECT_EQ(v.x,box[0].as_int64()); EXPECT_EQ(v.y,box[1].as_int64());
            EXPECT_EQ(v.endX-1,box[2].as_int64()); EXPECT_EQ(v.endY-1,box[3].as_int64());
            std::vector<unsigned> rle; unsigned length = 0; bool state = false;
            for (unsigned y = v.y; y < v.endY; ++y) for (unsigned x = v.x; x < v.endX; ++x) {
                bool member = labels[y*im.w+x] == v.id;
                if (member != state) { rle.push_back(length); length = 0; state = member; } ++length;
            }
            rle.push_back(length); auto runs = entry.at("rle").as_array(); ASSERT_EQ(rle.size(),runs.size());
            for (unsigned k = 0; k < runs.size(); ++k) EXPECT_EQ(rle[k],runs[k].as_int64());
        }
        EXPECT_EQ(im.data,saved); ++cases;
    }
    EXPECT_EQ(cases,138u); // 23 fixtures, four 300 dpi and two 25.4 dpi settings
}
TEST(ExplodeBitmapSpecks, T27MetricBoundariesTiesBridgesAnisotropy)
{
    auto one = OrthogonalMetric::fromDpi(25.4,25.4); ASSERT_TRUE(one.ok());
    EXPECT_DOUBLE_EQ(one.value.areaLimit(),2); EXPECT_DOUBLE_EQ(one.value.reachSquared(),1);
    auto three = OrthogonalMetric::fromDpi(300,300); ASSERT_TRUE(three.ok());
    double px = 300.0/25.4*(300.0/300.0); EXPECT_DOUBLE_EQ(three.value.areaLimit(),2*px*px);
    for (double bad : {0.0,-1.0,double(INFINITY),double(NAN),1e-300,1e300})
        EXPECT_FALSE(OrthogonalMetric::fromDpi(bad,300).ok());
    for (unsigned area : {1u,2u,3u}) {
        Image im(12,5); im.block(0,0,3,3); im.block(4,1,area,1);
        Job j(im); ASSERT_TRUE(j.enclosed.ok()); auto out = attach(j.enclosed.value,one.value,j.budget,j.work);
        ASSERT_TRUE(out.ok()); EXPECT_EQ(out.value.pieceCount,area <= 2 ? 1u : 2u); compare(im,one.value);
    }
    for (unsigned distance : {1u,2u}) {
        // These are distinct regions, with true closed-cell gaps at/over 1 px.
        Image im(12,7); im.block(0,0,3,3); im.put(3+distance,1);
        Job j(im); ASSERT_TRUE(j.enclosed.ok()); ASSERT_EQ(j.enclosed.value.pieceCount,2u);
        auto out = attach(j.enclosed.value,one.value,j.budget,j.work);
        ASSERT_TRUE(out.ok()); EXPECT_EQ(out.value.pieceCount,distance <= 1 ? 1u : 2u); compare(im,one.value);
    }
    Image tie(12,5); tie.block(0,0,3,3); tie.block(8,0,3,3); tie.put(5,1);
    // x=5 is gap 2 from both closed blocks; choose original id 1.
    auto two = OrthogonalMetric::fromDpi(50.8,50.8).value; compare(tie,two);
    Job t(tie); auto joined = attach(t.enclosed.value,two,t.budget,t.work); ASSERT_TRUE(joined.ok());
    EXPECT_EQ(plane(joined.value)[17],1u);
    Image bridge(16,5); bridge.block(0,0,3,3); bridge.put(4,1); bridge.put(6,1); bridge.block(8,0,3,3);
    compare(bridge,one.value); // frozen nearest choices join opposite ends independently
    Image chain(11,3); chain.put(1,1); chain.put(3,1); chain.put(5,1); chain.put(7,1);
    compare(chain,one.value);
    Job c(chain); auto linked = attach(c.enclosed.value,one.value,c.budget,c.work);
    ASSERT_TRUE(linked.ok()); EXPECT_EQ(linked.value.pieceCount,1u);
    Image axes(10,10); axes.block(0,0,3,3); axes.put(4,1); axes.put(1,4);
    auto anisotropic = OrthogonalMetric::fromDpi(25.4,12.7).value; compare(axes,anisotropic);
    Job a(axes); auto axis = attach(a.enclosed.value,anisotropic,a.budget,a.work); ASSERT_TRUE(axis.ok());
    auto labels = plane(axis.value); EXPECT_EQ(labels[14],1u); EXPECT_NE(labels[41],1u);
    // Owned transparent holes follow attachment without becoming foreground.
    Image ring(15,15); ring.block(0,0,15,1); ring.block(0,14,15,1);
    ring.block(0,0,1,15); ring.block(14,0,1,15); ring.put(7,7); compare(ring,one.value);
}
TEST(ExplodeBitmapSpecks, T27FixedSeedBruteProperties)
{
    auto start = Clock::now();
    for (unsigned seed : {0xEB122u,0xEB127u,20261001u}) {
        std::mt19937 rng(seed);
        for (unsigned trial = 0; trial < 100; ++trial) {
            Image im(4+rng()%9,4+rng()%9);
            for (unsigned y = 0; y < im.h; ++y) for (unsigned x = 0; x < im.w; ++x)
                if (rng()%5 == 0) im.put(x,y);
            auto metric = OrthogonalMetric::fromDpi(12.7*(1+rng()%8),12.7*(1+rng()%8));
            ASSERT_TRUE(metric.ok()); compare(im,metric.value);
        }
    }
    auto elapsed = std::chrono::duration<double>(Clock::now()-start).count();
    std::cout << "300 brute properties seconds=" << elapsed << '\n'; EXPECT_LT(elapsed,3);
}
TEST(ExplodeBitmapSpecks, T27TwentyThousandEquidistant)
{
    Image im(405,805);
    for (unsigned y = 0; y < 200; ++y) for (unsigned x = 0; x < 100; ++x) im.put(2+4*x,2+4*y);
    auto start = Clock::now(); Job j(im); ASSERT_TRUE(j.enclosed.ok()); ASSERT_EQ(j.enclosed.value.pieceCount,20000u);
    JobWork reserved(std::uint64_t(im.w)*im.h); ASSERT_TRUE(reserved.advance(reserved.limit()-20000000).ok());
    auto result = attach(j.enclosed.value,OrthogonalMetric::fromDpi(76.2,76.2).value,j.budget,reserved);
    ASSERT_TRUE(result.ok()) << result.outcome.diagnostic; EXPECT_EQ(result.value.pieceCount,1u);
    EXPECT_LT(result.consumed,20000000u); plane(result.value);
    auto elapsed = std::chrono::duration<double>(Clock::now()-start).count();
    std::cout << "20000 equidistant seconds=" << elapsed << " visits=" << result.consumed
              << " topologyPeak=" << result.value.topologyPeak << '\n'; EXPECT_LT(elapsed,3);
}
TEST(ExplodeBitmapSpecks, BudgetCapsCancellationAndLifetime)
{
    Image im(40,40); im.block(0,0,4,4); im.put(6,1); im.put(8,1);
    Job j(im); ASSERT_TRUE(j.enclosed.ok()); auto metric = OrthogonalMetric::fromDpi(50.8,50.8).value;
    auto live = j.budget.reserved(); Budget tiny(Budget::FixedLimitForTest{},1);
    JobWork fresh(1600); auto refused = attach(j.enclosed.value,metric,tiny,fresh);
    EXPECT_FALSE(refused.ok()); EXPECT_EQ(refused.value.pieceCount,0u); EXPECT_EQ(tiny.reserved(),0u);
    Budget limited(Budget::FixedLimitForTest{},256*MiB); Budget::Token occupied;
    ASSERT_TRUE(limited.acquire(Stage::topology,256*MiB-1,occupied).ok());
    auto cap = attach(j.enclosed.value,metric,limited,fresh); EXPECT_FALSE(cap.ok());
    EXPECT_STREQ(cap.outcome.diagnostic,"Topology byte cap exceeded"); EXPECT_EQ(limited.reserved(),256*MiB-1);
    JobWork exhausted(1600); ASSERT_TRUE(exhausted.advance(exhausted.limit()).ok());
    auto work = attach(j.enclosed.value,metric,j.budget,exhausted); EXPECT_FALSE(work.ok());
    EXPECT_EQ(j.budget.reserved(),live);
    auto flag = std::make_shared<std::atomic<bool>>(true); auto start = Clock::now();
    auto canceled = attach(j.enclosed.value,metric,j.budget,fresh,Stop(flag));
    EXPECT_EQ(canceled.outcome.status,Status::canceled); EXPECT_EQ(j.budget.reserved(),live);
    EXPECT_LT((std::chrono::duration<double,std::milli>(Clock::now()-start).count()),100);
    Image many(405,805); for (unsigned y = 0; y < 200; ++y) for (unsigned x = 0; x < 100; ++x) many.put(2+4*x,2+4*y);
    Job m(many); ASSERT_TRUE(m.enclosed.ok()); flag->store(false);
    // Raise Stop once the loop is provably running (its atomic visit meter has moved), not after a timed sleep: on
    // Windows a 1 ms sleep sometimes outlasted the whole attach, which then finished uncanceled.
    Clock::time_point requested; std::atomic<bool> done{false}; auto const base = m.work.visits();
    std::thread cancel([&] {
        while (!done.load() && m.work.visits() <= base) std::this_thread::yield();
        requested = Clock::now(); flag->store(true);
    });
    auto stopped = attach(m.enclosed.value,metric,m.budget,m.work,Stop(flag)); auto finished = Clock::now();
    done.store(true); cancel.join();
    EXPECT_EQ(stopped.outcome.status,Status::canceled);
    auto delay = std::chrono::duration<double,std::milli>(finished-requested).count();
    std::cout << "in-loop cancellation ms=" << delay << '\n'; EXPECT_LT(delay,100);
    auto output = attach(j.enclosed.value,metric,j.budget,fresh); ASSERT_TRUE(output.ok());
    auto expected = plane(output.value,true); j.enclosed.value = {};
    Partition moved = std::move(output.value); EXPECT_EQ(output.value.pieceCount,0u);
    EXPECT_EQ(plane(moved,true),expected); moved = {}; EXPECT_EQ(j.budget.reserved(),j.regions.value.runCount*sizeof(Inkscape::Bitmap::Run)+j.regions.value.islandCount*sizeof(Island));
    Image tooMany(405,809); for (unsigned y = 0; y < 201; ++y) for (unsigned x = 0; x < 100; ++x) tooMany.put(2+4*x,2+4*y);
    Job large(tooMany); ASSERT_TRUE(large.enclosed.ok());
    auto pieces = attach(large.enclosed.value,OrthogonalMetric::fromDpi(1,1).value,large.budget,large.work);
    EXPECT_FALSE(pieces.ok()); EXPECT_STREQ(pieces.outcome.diagnostic,"Final piece cap exceeded");
}
TEST(ExplodeBitmapSpecks, T27FloatingBoundaryAndFrozenAreas)
{
    // One ULP below/at/above exact 1px/mm. The explicit integer-cell ±1
    // fixtures above also check the user-visible inclusive limits.
    Image im(12,5); im.block(0,0,3,3); im.block(4,1,2,1);
    for (double dpi : {std::nextafter(25.4,0.0),25.4,std::nextafter(25.4,100.0)}) {
        auto metric = OrthogonalMetric::fromDpi(dpi,dpi); ASSERT_TRUE(metric.ok());
        Job j(im); ASSERT_TRUE(j.enclosed.ok());
        auto result = attach(j.enclosed.value,metric.value,j.budget,j.work); ASSERT_TRUE(result.ok());
        EXPECT_EQ(result.value.pieceCount,metric.value.areaLimit() >= 2 && metric.value.reachSquared() >= 1 ? 1u : 2u);
        compare(im,metric.value);
    }
    // Each 2px speck remains eligible even after its neighbour joins. Frozen
    // choices connect this entire chain although the growing area exceeds 2.
    Image chain(17,5); chain.block(0,0,3,3);
    for (unsigned x : {4u,7u,10u,13u}) chain.block(x,1,2,1);
    auto metric = OrthogonalMetric::fromDpi(25.4,25.4).value; compare(chain,metric);
    Job j(chain); auto result = attach(j.enclosed.value,metric,j.budget,j.work);
    ASSERT_TRUE(result.ok()); EXPECT_EQ(result.value.pieceCount,1u); EXPECT_EQ(result.value.pieces()[0].area,17u);
    // Transparent interior remains a hole owned by the original enclosure;
    // an exterior speck can enlarge its bbox without claiming exterior pixels.
    Image ring(20,12); ring.block(0,0,10,1); ring.block(0,9,10,1);
    ring.block(0,0,1,10); ring.block(9,0,1,10); ring.put(11,5); ring.put(4,4);
    compare(ring,metric);
}
TEST(ExplodeBitmapSpecks, BudgetAndVisitMinusZeroPlusOne)
{
    Image im(12,5); im.block(0,0,3,3); im.put(4,1); im.put(6,1);
    Job j(im); ASSERT_TRUE(j.enclosed.ok()); auto metric = OrthogonalMetric::fromDpi(25.4,25.4).value;
    // A distinct ledger makes the attachment's exact live scratch/output peak
    // measurable, while the input remains alive on its own ledger.
    Budget measure(Budget::FixedLimitForTest{},256*MiB); JobWork work(60);
    auto success = attach(j.enclosed.value,metric,measure,work); ASSERT_TRUE(success.ok());
    auto peak = success.value.topologyPeak;
    // topologyPeak also includes inherited enclosure history. Use a baseline
    // enclosure with its historical peak lower than attachment's index overlap.
    EXPECT_GT(peak,0u); success.value = {};
    EXPECT_EQ(measure.reserved(),0u);
    // Preflight may refuse before spending the full allowance. Find the
    // admission threshold, then qualify its exact -1/0/+1 boundary.
    std::uint64_t visitLow = 0, visitHigh = work.limit();
    while (visitLow < visitHigh) {
        auto mid = visitLow+(visitHigh-visitLow)/2; JobWork shared(60);
        ASSERT_TRUE(shared.advance(shared.limit()-mid).ok());
        auto result = attach(j.enclosed.value,metric,j.budget,shared);
        if (result.ok()) visitHigh = mid; else visitLow = mid+1;
    }
    for (int delta : {-1,0,1}) {
        JobWork shared(60); ASSERT_TRUE(shared.advance(shared.limit()-visitLow-delta).ok());
        auto result = attach(j.enclosed.value,metric,j.budget,shared);
        EXPECT_EQ(result.ok(),delta >= 0); EXPECT_LE(result.consumed,visitLow+delta);
        if (!result.ok()) EXPECT_EQ(result.value.pieceCount,0u);
    }
    // Binary-search minimum byte allowance, then qualify exact -1/0/+1.
    std::uint64_t low = 0, high = peak;
    while (low < high) {
        auto mid = low+(high-low)/2; Budget b(Budget::FixedLimitForTest{},mid); JobWork fresh(60);
        auto result = attach(j.enclosed.value,metric,b,fresh);
        if (result.ok()) high = mid; else low = mid+1;
    }
    for (int delta : {-1,0,1}) {
        Budget b(Budget::FixedLimitForTest{},low+delta); JobWork fresh(60);
        auto result = attach(j.enclosed.value,metric,b,fresh); EXPECT_EQ(result.ok(),delta >= 0);
        result.value = {}; EXPECT_EQ(b.reserved(),0u);
    }
    EXPECT_FALSE(attach(j.enclosed.value,OrthogonalMetric{},j.budget,work).ok());
    Partition empty; EXPECT_FALSE(attach(empty,metric,j.budget,work).ok());
}
TEST(ExplodeBitmapSpecks, FinalPieceCapMinusZeroPlusOne)
{
    // Tiny physical speck area means these single cells are non-specks. This
    // qualifies retained output counts, independently of the dense-join case.
    auto metric = OrthogonalMetric::fromDpi(1,1).value;
    for (unsigned count : {19999u,20000u,20001u}) {
        Image im(405,805);
        for (unsigned i = 0; i < count; ++i) im.put(2+4*(i%100),2+4*(i/100));
        Job j(im); ASSERT_TRUE(j.enclosed.ok());
        ASSERT_EQ(j.enclosed.value.pieceCount,count);
        auto live = j.budget.reserved();
        auto result = attach(j.enclosed.value,metric,j.budget,j.work);
        EXPECT_EQ(result.ok(),count <= 20000);
        if (result.ok()) {
            EXPECT_EQ(result.value.pieceCount,count);
            EXPECT_EQ(result.value.pieces()[count-1].id,count);
        } else {
            EXPECT_STREQ(result.outcome.diagnostic,"Final piece cap exceeded");
            EXPECT_EQ(result.value.pieceCount,0u);
        }
        result.value = {}; EXPECT_EQ(j.budget.reserved(),live);
    }
}

TEST(ExplodeBitmapSpecks, ProductionDpiIntegerBoundaries)
{
    auto metric = OrthogonalMetric::fromDpi(300,300).value;
    // 2*(300/25.4)^2 = 279.000558...; both specks are gap 11
    // from a 400px non-speck. 279 = 9*31, 280 = 10*28.
    for (unsigned area : {279u,280u}) {
        Image im(50,40); im.block(0,0,20,20);
        im.block(31,0,area == 279 ? 9 : 10,area == 279 ? 31 : 28);
        Job j(im); ASSERT_TRUE(j.enclosed.ok()); ASSERT_EQ(j.enclosed.value.pieceCount,2u);
        auto out = attach(j.enclosed.value,metric,j.budget,j.work); ASSERT_TRUE(out.ok());
        EXPECT_EQ(out.value.pieceCount,area == 279 ? 1u : 2u);
        EXPECT_EQ(out.value.speckJoins,area == 279 ? 1u : 0u);
    }
    for (auto gaps : {std::pair{11u,0u}, {12u,0u}, {11u,4u}, {9u,8u}}) {
        Image im(50,50); im.block(0,0,20,20); im.put(20+gaps.first,20+gaps.second);
        Job j(im); ASSERT_TRUE(j.enclosed.ok()); ASSERT_EQ(j.enclosed.value.pieceCount,2u);
        auto out = attach(j.enclosed.value,metric,j.budget,j.work); ASSERT_TRUE(out.ok());
        auto joins = gaps.first*gaps.first + gaps.second*gaps.second <= 139;
        EXPECT_EQ(out.value.pieceCount,joins ? 1u : 2u);
        EXPECT_EQ(out.value.retainedIsolatedSpecks,joins ? 0u : 1u);
    }
    // Hand computation in mm: dx=11,dy=4 is (11*25.4/300)^2+
    // (4*25.4/254)^2 = .867379...+.16 = 1.027379... (>1).
    // dx=10,dy=4 gives .716844...+.16 = .876844... (<1).
    // No production metric helper participates in these expected results.
    for (unsigned dx : {10u,11u}) {
        Image im(50,50); im.block(0,0,20,20); im.put(20+dx,24);
        Job j(im); ASSERT_TRUE(j.enclosed.ok());
        auto out = attach(j.enclosed.value,OrthogonalMetric::fromDpi(300,254).value,j.budget,j.work);
        ASSERT_TRUE(out.ok()); EXPECT_EQ(out.value.pieceCount,dx == 10 ? 1u : 2u);
    }
}
TEST(ExplodeBitmapSpecks, ProductionHatchAndReachRefusals)
{
    // 500 isolated 1x100 specks; origins are 14px apart, closed-cell gap 13.
    // More than the old flat reserve, but admitted by the job's real allowance.
    Image im(7002,104);
    for (unsigned x = 0; x < 500; ++x) im.block(1+14*x,2,1,100);
    Job j(im); ASSERT_TRUE(j.enclosed.ok()) << j.enclosed.outcome.diagnostic;
    auto metric = OrthogonalMetric::fromDpi(300,300).value;
    JobWork estimateWork(std::uint64_t(im.w)*im.h);
    auto allowance = speckWorkReserve(j.enclosed.value,metric,estimateWork);
    ASSERT_TRUE(allowance.ok()); EXPECT_GT(allowance.value,20000000u);
    auto result = attach(j.enclosed.value,metric,j.budget,j.work);
    ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
    EXPECT_LE(result.consumed,allowance.value);
    EXPECT_EQ(result.value.pieceCount,500u); EXPECT_EQ(result.value.retainedIsolatedSpecks,500u);
    EXPECT_EQ(result.value.speckJoins,0u);
    JobWork oldReserve(std::uint64_t(im.w)*im.h);
    ASSERT_TRUE(oldReserve.advance(oldReserve.limit()-20000000).ok());
    auto old = attach(j.enclosed.value,metric,j.budget,oldReserve);
    EXPECT_FALSE(old.ok()); EXPECT_STREQ(old.outcome.diagnostic,"Speck work reserve exceeded");
    EXPECT_LT(old.consumed,300000u); // refuse before the 38M-visit query workload
    std::cout << "500 hatch visits=" << result.consumed << " peak=" << result.value.topologyPeak << '\n';
    for (auto dpi : {std::pair{2400.,2400.}, {300.,25400.}}) {
        JobWork fresh(std::uint64_t(im.w)*im.h); auto live = j.budget.reserved();
        auto refused = attach(j.enclosed.value,OrthogonalMetric::fromDpi(dpi.first,dpi.second).value,j.budget,fresh);
        EXPECT_FALSE(refused.ok()); EXPECT_STREQ(refused.outcome.diagnostic,"Speck work reserve exceeded");
        EXPECT_EQ(refused.value.pieceCount,0u); EXPECT_EQ(j.budget.reserved(),live);
        EXPECT_EQ(refused.consumed,2u*j.enclosed.value.runCount + j.enclosed.value.regionCount + j.enclosed.value.pieceCount);
        EnclosureOptions options; options.speckDpiX = dpi.first; options.speckDpiY = dpi.second;
        JobWork enclosureWork(std::uint64_t(im.w)*im.h);
        auto enclosure = enclose(j.regions.value,j.budget,enclosureWork,{},options);
        EXPECT_FALSE(enclosure.ok()); EXPECT_STREQ(enclosure.outcome.diagnostic,"Speck work reserve exceeded");
        EXPECT_EQ(enclosure.value.pieceCount,0u); EXPECT_EQ(j.budget.reserved(),live);
    }
}
TEST(ExplodeBitmapSpecks, MaximumRunAndUnpaddedMemory)
{
    // Alternating rows attain the maximum possible runs per row. The entire
    // checkerboard is 8-connected: no query tree is needed for this non-speck.
    Image maximum(513,512);
    for (unsigned y = 0; y < maximum.h; ++y) for (unsigned x = 0; x < maximum.w; ++x)
        if ((x+y)%2) maximum.put(x,y);
    Job m(maximum); ASSERT_TRUE(m.enclosed.ok());
    EXPECT_EQ(m.regions.value.runCount,maximum.w*maximum.h);
    auto out = attach(m.enclosed.value,OrthogonalMetric::fromDpi(300,300).value,m.budget,m.work);
    ASSERT_TRUE(out.ok()); EXPECT_EQ(out.value.pieceCount,1u);
    auto savedRuns = m.enclosed.value.runCount; m.enclosed.value.runCount = 16000001;
    JobWork capWork(maximum.w*maximum.h);
    auto cap = attach(m.enclosed.value,OrthogonalMetric::fromDpi(300,300).value,m.budget,capWork);
    m.enclosed.value.runCount = savedRuns;
    EXPECT_FALSE(cap.ok()); EXPECT_EQ(cap.consumed,0u);
    // F=257 is just above a power of two. Exact tree bytes are (2F-1)*32,
    // not nextpow2(F)*2*24. Scratch peak measured on a separate ledger.
    Image im(1030,3); for (unsigned i = 0; i < 257; ++i) im.put(1+4*i,1);
    Job j(im); ASSERT_TRUE(j.enclosed.ok());
    auto treePeak = (2*257-1)*32u+257*24u+j.enclosed.value.regionCount*4u;
    Budget b(Budget::FixedLimitForTest{},treePeak); JobWork work(im.w*im.h);
    auto attached = attach(j.enclosed.value,OrthogonalMetric::fromDpi(76.2,76.2).value,b,work);
    ASSERT_TRUE(attached.ok()); EXPECT_EQ(attached.value.pieceCount,1u);
    EXPECT_EQ(attached.value.topologyPeak,std::max(std::uint64_t(treePeak),j.enclosed.value.topologyPeak));
    Budget below(Budget::FixedLimitForTest{},treePeak-1); JobWork oneShort(im.w*im.h);
    auto padded = attach(j.enclosed.value,OrthogonalMetric::fromDpi(76.2,76.2).value,below,oneShort);
    EXPECT_FALSE(padded.ok()); EXPECT_EQ(below.reserved(),0u);
    // Invalid source ids must refuse before indexing, and leave input intact.
    auto pieces = const_cast<Piece *>(j.enclosed.value.pieces()); auto saved = pieces[0].id;
    pieces[0].id = j.enclosed.value.foregroundCount+1;
    JobWork invalid(im.w*im.h); auto refused = attach(j.enclosed.value,OrthogonalMetric::fromDpi(300,300).value,j.budget,invalid);
    pieces[0].id = saved;
    EXPECT_FALSE(refused.ok()); EXPECT_EQ(refused.outcome.status,Status::incompatible);
    EXPECT_EQ(refused.value.pieceCount,0u);
}
