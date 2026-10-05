// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "util/bitmap-islands.h"
#include <boost/json.hpp>
#include <glib.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <vector>
#include <thread>
using namespace Inkscape::Bitmap;
namespace {
Result<Regions> labelForTest(RgbaView view, AlphaLut const &lut, Budget &budget,
                             Stop stop = {}, LabelOptions options = {}) noexcept
{
    JobWork meter(std::uint64_t(view.width) * view.height);
    return label(view, lut, budget, meter, stop, options);
}

struct Image {
    unsigned w, h;
    std::vector<std::uint8_t> pixels;
    Image(unsigned width, unsigned height) : w(width), h(height), pixels(w * h * 4) {}
    RgbaView view() const { return {pixels.data(), pixels.size(), w * 4ULL, w, h}; }
};
std::string read(std::string const &path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
std::string hash(AlphaLut const &lut)
{
    auto p = g_compute_checksum_for_data(G_CHECKSUM_SHA256, lut.data(), lut.size());
    std::string s(p); g_free(p); return s;
}
std::vector<unsigned> plane(Regions const &regions)
{
    std::vector<unsigned> result(regions.width * regions.height, UINT32_MAX);
    for (unsigned i = 0; i < regions.runCount; ++i) {
        auto r = regions.runs()[i];
        EXPECT_LT(r.x, r.end); EXPECT_LE(r.end, regions.width);
        EXPECT_LT(r.y, regions.height); EXPECT_LT(r.region, regions.islandCount);
        for (unsigned x = r.x; x < r.end; ++x) {
            auto &p = result[r.y * regions.width + x];
            EXPECT_EQ(p, UINT32_MAX); p = r.region;
        }
    }
    return result;
}
// Independent pixel flood oracle: four neighbours for background, eight for foreground.
void properties(Image const &image, AlphaLut const &lut, Regions const &regions)
{
    auto actual = plane(regions);
    std::vector<int> visited(image.w * image.h, -1);
    unsigned fg = 0, bg = 0;
    auto visible = [&](unsigned i) { auto a = image.pixels[4 * i + 3]; return a && lut[a]; };
    for (unsigned seed = 0; seed < visited.size(); ++seed) {
        ASSERT_LT(actual[seed], regions.islandCount);
        if (visited[seed] >= 0) continue;
        auto index = actual[seed]; auto const &b = regions.islands()[index];
        bool f = visible(seed), exterior = false;
        EXPECT_EQ(b.foreground, f); EXPECT_EQ(b.id, f ? ++fg : ++bg);
        std::vector<unsigned> queue{seed}; visited[seed] = index;
        unsigned x1 = image.w, y1 = image.h, x2 = 0, y2 = 0;
        for (unsigned q = 0; q < queue.size(); ++q) {
            auto p = queue[q]; int x = p % image.w, y = p / image.w;
            EXPECT_EQ(actual[p], index);
            x1 = std::min(x1, unsigned(x)); y1 = std::min(y1, unsigned(y));
            x2 = std::max(x2, unsigned(x + 1)); y2 = std::max(y2, unsigned(y + 1));
            exterior |= !f && (!x || !y || unsigned(x + 1) == image.w || unsigned(y + 1) == image.h);
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if ((!dx && !dy) || (!f && dx && dy)) continue;
                int xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || unsigned(xx) >= image.w || unsigned(yy) >= image.h) continue;
                auto other = yy * image.w + xx;
                if (bool(visible(other)) != f) continue;
                EXPECT_EQ(actual[other], index);
                if (visited[other] < 0) { visited[other] = index; queue.push_back(other); }
            }
        }
        EXPECT_EQ(b.area, queue.size()); EXPECT_EQ(b.exterior, exterior);
        EXPECT_EQ(b.x, x1); EXPECT_EQ(b.y, y1); EXPECT_EQ(b.endX, x2); EXPECT_EQ(b.endY, y2);
    }
    EXPECT_EQ(regions.foregroundCount, fg); EXPECT_EQ(regions.islandCount, fg + bg);
    for (unsigned i = 1; i < regions.islandCount; ++i) {
        auto a = regions.islands()[i - 1], b = regions.islands()[i];
        if (a.foreground == b.foreground)
            EXPECT_TRUE(a.y < b.y || (a.y == b.y && (a.x < b.x || (a.x == b.x && a.id < b.id))));
        else EXPECT_TRUE(a.foreground);
    }
}
}
TEST(ExplodeBitmapLabel, T25ExhaustiveLut)
{
    for (unsigned t = 0; t < 256; ++t) for (unsigned s = 0; s < 128; ++s) {
        auto lut = alphaLut(t, s);
        ASSERT_EQ(lut[0], 0);
        for (unsigned a = 1; a < 256; ++a) {
            unsigned expected;
            int d = int(a) - int(t) + int(s);
            if (!s) expected = a >= t ? 255 : 0;
            else if (d <= 0) expected = 0;
            else if (d >= int(2 * s)) expected = 255;
            else {
                // Exact rational smoothstep, independently rounded to nearest integer.
                std::uint64_t den = 8ULL * s * s * s;
                std::uint64_t num = std::uint64_t(d) * d * (6 * s - 2 * d) * 255;
                expected = (2 * num + den) / (2 * den);
            }
            ASSERT_EQ(lut[a], expected) << "a=" << a << " T=" << t << " S=" << s;
        }
    }
}
TEST(ExplodeBitmapLabel, T22IntermediateOracleAndT25HashRgb)
{
    auto directory = std::string(INKSCAPE_TESTS_DIR) + "/explode-bitmap/";
    auto text = read(directory + "oracle/label-regions.json");
    ASSERT_FALSE(text.empty());
    auto suite = boost::json::parse(text).as_array();
    ASSERT_EQ(suite.size(), 184u);
    for (auto const &entry : suite) {
        auto const &e = entry.as_object();
        auto file = std::string(e.at("file").as_string()); SCOPED_TRACE(file);
        unsigned w = e.at("width").as_int64(), h = e.at("height").as_int64();
        Image image(w, h);
        auto pam = read(directory + "fixtures/pam/" + file);
        auto start = pam.find("ENDHDR\n"); ASSERT_NE(start, std::string::npos); start += 7;
        ASSERT_EQ(pam.size() - start, image.pixels.size());
        std::copy(pam.begin() + start, pam.end(), image.pixels.begin());
        auto original = image.pixels;
        auto lut = alphaLut(e.at("T").as_int64(), e.at("S").as_int64());
        EXPECT_EQ(hash(lut), std::string(e.at("lut_sha256").as_string()));
        Budget budget(Budget::FixedLimitForTest{}, 256 * MiB);
        {
            auto r = labelForTest(image.view(), lut, budget);
            ASSERT_TRUE(r.outcome.ok()) << r.outcome.diagnostic;
            properties(image, lut, r.value);
            auto labels = plane(r.value);
            auto const &pieces = e.at("pieces").as_array();
            ASSERT_EQ(r.value.foregroundCount, pieces.size());
            for (unsigned i = 0; i < pieces.size(); ++i) {
                auto const &p = pieces[i].as_object(); auto b = r.value.islands()[i];
                EXPECT_EQ(b.id, p.at("id").as_int64()); EXPECT_EQ(b.area, p.at("area").as_int64());
                auto const &box = p.at("bbox").as_array();
                EXPECT_EQ(b.x, box[0].as_int64()); EXPECT_EQ(b.y, box[1].as_int64());
                EXPECT_EQ(b.endX - 1, box[2].as_int64()); EXPECT_EQ(b.endY - 1, box[3].as_int64());
                std::vector<unsigned> runs; unsigned len = 0; bool current = false;
                for (unsigned y = b.y; y < b.endY; ++y) for (unsigned x = b.x; x < b.endX; ++x) {
                    bool v = labels[y * w + x] == i;
                    if (v != current) { runs.push_back(len); len = 0; current = v; }
                    ++len;
                }
                runs.push_back(len); auto const &expected = p.at("rle").as_array();
                ASSERT_EQ(runs.size(), expected.size());
                for (unsigned j = 0; j < runs.size(); ++j) EXPECT_EQ(runs[j], expected[j].as_int64());
            }
        }
        EXPECT_EQ(original, image.pixels); EXPECT_EQ(budget.reserved(), 0u);
    }
    // Unmodified FINAL oracle hash, in addition to intermediate v3.2 hashes.
    auto final = boost::json::parse(read(directory + "fixtures/expected/opaque.T128_S40_dpi300.json"));
    EXPECT_EQ(hash(alphaLut(128,40)), std::string(final.as_object().at("lut_sha256").as_string()));
}
TEST(ExplodeBitmapLabel, T22FixedSeedConnectivityAndDegenerateDimensions)
{
    auto start = std::chrono::steady_clock::now();
    for (unsigned seed : {0xEB122u, 0x8414u, 20261001u}) {
        std::mt19937 random(seed);
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            Image image(iteration < 2 ? 1 : 1 + random() % 25, iteration == 2 ? 1 : 1 + random() % 25);
            for (auto &p : image.pixels) p = random() % 256;
            auto lut = alphaLut(random() % 256, random() % 128);
            Budget budget(Budget::FixedLimitForTest{}, 4 * MiB);
            auto r = labelForTest(image.view(), lut, budget);
            ASSERT_TRUE(r.outcome.ok()); properties(image, lut, r.value);
        }
    }
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 3);
    for (auto dimensions : {std::pair{1u,16384u}, {16384u,1u}, {1u,1u}}) {
        Image image(dimensions.first, dimensions.second); Budget budget(Budget::FixedLimitForTest{}, 4 * MiB);
        for (auto alpha : {0,255}) {
            for (unsigned i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = alpha;
            auto r = labelForTest(image.view(), alphaLut(128,40), budget);
            ASSERT_TRUE(r.outcome.ok()); properties(image, alphaLut(128,40), r.value);
        }
    }
}
TEST(ExplodeBitmapLabel, T03CapsBudgetAndAllocationSweep)
{
    Image image(11, 9);
    for (unsigned y = 0; y < image.h; ++y) for (unsigned x = 0; x < image.w; ++x)
        image.pixels[(y * image.w + x) * 4 + 3] = (x % 2 == 0 && y % 2 == 0) ? 255 : 0;
    auto lut = alphaLut(128,40);
    unsigned runs, islands, foreground; std::uint64_t peak;
    {
        Budget budget(Budget::FixedLimitForTest{}, 4 * MiB); auto r = labelForTest(image.view(), lut, budget);
        ASSERT_TRUE(r.outcome.ok()); runs = r.value.runCount; islands = r.value.islandCount; foreground = r.value.foregroundCount;
        peak = runs * (sizeof(Inkscape::Bitmap::Run) + sizeof(unsigned)) + islands * sizeof(Island);
    }
    for (int delta : {-1,0,1}) {
        for (unsigned ceiling = 0; ceiling < 3; ++ceiling) {
            Budget budget(Budget::FixedLimitForTest{}, 4 * MiB); LabelOptions options;
            if (ceiling == 0) options.runs = runs + delta;
            if (ceiling == 1) options.islands = foreground + delta;
            if (ceiling == 2) options.topologyBytes = peak + delta;
            {
                auto r = labelForTest(image.view(), lut, budget, {}, options);
                EXPECT_EQ(r.outcome.ok(), delta >= 0);
                if (delta < 0) { EXPECT_EQ(r.value.runCount, 0u); EXPECT_EQ(r.value.islandCount, 0u); }
            }
            EXPECT_EQ(budget.reserved(), 0u);
        }
        Budget budget(Budget::FixedLimitForTest{}, peak + delta);
        { auto r = labelForTest(image.view(), lut, budget); EXPECT_EQ(r.outcome.ok(), delta >= 0); }
        EXPECT_EQ(budget.reserved(), 0u);
    }
    for (unsigned k = 1; k <= 4; ++k) {
        Budget budget(Budget::FixedLimitForTest{}, 4 * MiB); AllocationFault fault{k,0}; LabelOptions options; options.fault = &fault;
        {
            auto r = labelForTest(image.view(), lut, budget, {}, options);
            EXPECT_EQ(r.outcome.ok(), k == 4); EXPECT_EQ(fault.attempts, std::min(k,3u));
            if (k < 4) EXPECT_EQ(r.value.runs(), nullptr);
        }
        EXPECT_EQ(budget.reserved(), 0u);
    }
    EXPECT_EQ(LabelOptions{}.runs, 16000000u); EXPECT_EQ(LabelOptions{}.islands, 2000000u);
    // No fixed topology ceiling by default; the shared Budget controls admission.
    EXPECT_EQ(LabelOptions{}.topologyBytes, std::numeric_limits<std::uint64_t>::max());
    Budget budget(Budget::FixedLimitForTest{}, 4 * MiB); Budget::Token shared;
    ASSERT_TRUE(budget.acquire(Stage::topology, 1024, shared).ok());
    LabelOptions options; options.topologyBytes = peak + 1023;
    auto r = labelForTest(image.view(), lut, budget, {}, options);
    EXPECT_FALSE(r.outcome.ok()); EXPECT_EQ(budget.reserved(), 1024u);
}
TEST(ExplodeBitmapLabel, T03CheckedInputCancellationAndHardLimits)
{
    Image image(4,4); auto v = image.view(); auto lut = alphaLut(0,0); Budget budget(Budget::FixedLimitForTest{}, 4 * MiB);
    for (unsigned which = 0; which < 7; ++which) {
        auto bad = v;
        if (which == 0) bad.width = 0;
        if (which == 1) bad.height = 16385;
        if (which == 2) bad.stride = 15;
        if (which == 3) bad.bytes = 63;
        if (which == 4) bad.stride = UINT64_MAX;
        if (which == 5) bad.data = nullptr;
        if (which == 6) { bad.width = 16384; bad.height = 16384; }
        auto r = labelForTest(bad,lut,budget); EXPECT_EQ(r.outcome.status, Status::incompatible);
        EXPECT_EQ(budget.reserved(),0u);
    }
    auto canceled = std::make_shared<std::atomic<bool>>(true);
    EXPECT_EQ(labelForTest(v,lut,budget,Stop(canceled)).outcome.status,Status::canceled);
    for (unsigned i = 0; i < 3; ++i) {
        LabelOptions o;
        if (!i) ++o.runs;
        if (i == 1) ++o.islands;
        if (i == 2) ++o.topologyBytes;
        EXPECT_FALSE(labelForTest(v,lut,budget,{},o).outcome.ok());
    }
    // Padding is ignored; transparent samples stay background even with a caller LUT.
    std::vector<std::uint8_t> padded(40,255);
    for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 3; ++x) padded[y * 20 + x * 4 + 3] = 0;
    AlphaLut malicious; malicious.fill(255);
    auto r = labelForTest({padded.data(),padded.size(),20,3,2},malicious,budget);
    ASSERT_TRUE(r.outcome.ok()); EXPECT_EQ(r.value.foregroundCount,0u);
}
TEST(ExplodeBitmapLabel, T03Synthetic25MP)
{
    Image image(5000,5000); // 100 MB input; two pixel passes fit the 100M visit ceiling.
    for (unsigned i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 255;
    Budget budget(Budget::FixedLimitForTest{}, 32 * MiB); auto start = std::chrono::steady_clock::now();
    auto r = labelForTest(image.view(),alphaLut(128,40),budget);
    ASSERT_TRUE(r.outcome.ok()) << r.outcome.diagnostic;
    EXPECT_EQ(r.value.foregroundCount,1u); EXPECT_EQ(r.value.islands()[0].area,25000000u);
    std::cout << "25MP label seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()
              << " retained topology bytes=" << budget.reserved() << '\n';
}
TEST(ExplodeBitmapLabel, T03ActualHardRunAndIslandRefusals)
{
    // Real production caps, not just lower test ceilings. No dense label plane.
    for (bool runCap : {true,false}) {
        Image image(runCap ? 8000 : 4001, runCap ? 2001 : 2001);
        for (unsigned y = 0; y < image.h; ++y) for (unsigned x = 0; x < image.w; ++x)
            image.pixels[(y * image.w + x) * 4 + 3] =
                (x % 2 == 0 && (runCap || y % 2 == 0)) ? 255 : 0;
        Budget budget(Budget::FixedLimitForTest{}, 256 * MiB); AllocationFault fault; LabelOptions options; options.fault = &fault;
        auto r = labelForTest(image.view(),alphaLut(128,40),budget,{},options);
        EXPECT_EQ(r.outcome.status,Status::failed);
        EXPECT_STREQ(r.outcome.diagnostic,runCap ? "Run cap exceeded" : "Island cap exceeded");
        EXPECT_EQ(r.value.runCount,0u); EXPECT_EQ(r.value.islandCount,0u);
        EXPECT_EQ(budget.reserved(),0u);
        EXPECT_EQ(fault.attempts,runCap ? 0u : 1u);
    }
}
TEST(ExplodeBitmapLabel, T22DiagonalBackgroundAndExterior)
{
    // Foreground diagonals connect; background diagonals do not connect.
    Image image(3,3);
    for (unsigned p = 0; p < 9; ++p) image.pixels[p * 4 + 3] = 255;
    image.pixels[3] = 0; image.pixels[4 * 4 + 3] = 0;
    Budget budget(Budget::FixedLimitForTest{}, 4 * MiB); auto lut = alphaLut(128,40);
    auto r = labelForTest(image.view(),lut,budget);
    ASSERT_TRUE(r.outcome.ok()); properties(image,lut,r.value);
    ASSERT_EQ(r.value.foregroundCount,1u); ASSERT_EQ(r.value.islandCount,3u);
    EXPECT_TRUE(r.value.islands()[1].exterior); EXPECT_FALSE(r.value.islands()[2].exterior);
    Image diagonal(2,2); diagonal.pixels[3] = diagonal.pixels[15] = 255;
    auto d = labelForTest(diagonal.view(),lut,budget);
    ASSERT_TRUE(d.outcome.ok()); properties(diagonal,lut,d.value);
    EXPECT_EQ(d.value.foregroundCount,1u); EXPECT_EQ(d.value.islandCount,3u);
}
TEST(ExplodeBitmapLabel, T22BboxOrderingDiffersFromDiscovery)
{
    // First-discovered component extends left below another component's first row.
    Image image(8,5);
    auto pixel = [&](unsigned x,unsigned y) { image.pixels[(y * image.w + x) * 4 + 3] = 255; };
    pixel(3,0); pixel(6,0); pixel(6,1); pixel(6,2);
    for (unsigned x = 0; x <= 6; ++x) pixel(x,3);
    Budget budget(Budget::FixedLimitForTest{}, 4 * MiB); auto lut = alphaLut(128,40);
    auto r = labelForTest(image.view(),lut,budget);
    ASSERT_TRUE(r.outcome.ok()); properties(image,lut,r.value);
    ASSERT_EQ(r.value.foregroundCount,2u);
    EXPECT_EQ(r.value.islands()[0].id,2u); EXPECT_EQ(r.value.islands()[1].id,1u);
    EXPECT_EQ(r.value.islands()[0].x,0u);
}
TEST(ExplodeBitmapLabel, T03MoveOwnershipAndBudgetLifetime)
{
    Image image(6,6); Budget budget(Budget::FixedLimitForTest{}, 4 * MiB);
    std::uint64_t retained;
    {
        auto result = labelForTest(image.view(),alphaLut(128,40),budget);
        ASSERT_TRUE(result.outcome.ok()); retained = budget.reserved(); EXPECT_GT(retained,0u);
        Regions moved = std::move(result.value);
        EXPECT_EQ(result.value.runs(),nullptr); EXPECT_EQ(budget.reserved(),retained);
        auto other = labelForTest(image.view(),alphaLut(128,40),budget);
        ASSERT_TRUE(other.outcome.ok()); EXPECT_EQ(budget.reserved(),2 * retained);
        moved = std::move(other.value); EXPECT_EQ(budget.reserved(),retained);
    }
    EXPECT_EQ(budget.reserved(),0u);
}
TEST(ExplodeBitmapLabel, T03CancellationDuringScan)
{
    Image image(4096,4096);
    for (unsigned i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 255;
    Budget budget(Budget::FixedLimitForTest{}, 4 * MiB);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> finished{false};
    std::atomic<std::int64_t> requestedAt{0};
    auto now = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(); };
    // Trigger after the count pass allocated runs, so this exercises live storage cleanup.
    std::thread requester([&] {
        while (!budget.reserved() && !finished.load()) std::this_thread::yield();
        if (!finished.load()) { requestedAt.store(now()); cancel->store(true); }
    });
    auto result = labelForTest(image.view(),alphaLut(128,40),budget,Stop(cancel));
    auto returnedAt = now(); finished.store(true); requester.join();
    ASSERT_GT(requestedAt.load(),0);
    EXPECT_EQ(result.outcome.status,Status::canceled);
    EXPECT_LT(returnedAt - requestedAt.load(),100000000); // 100 ms acknowledgment ceiling.
    EXPECT_EQ(result.value.runs(),nullptr); EXPECT_EQ(budget.reserved(),0u);
}

TEST(ExplodeBitmapLabel, Round2SharedWorkAndAdversarialTopology)
{
    for (unsigned kind = 0; kind < 3; ++kind) {
        Image image(512,512);
        for (unsigned y = 0; y < image.h; ++y) for (unsigned x = 0; x < image.w; ++x) {
            auto edge = std::min({x, y, image.w - 1 - x, image.h - 1 - y});
            bool f = kind == 0 ? edge % 4 == 0 : kind == 1 ?
                (x % 4 == 0 || (y % 8 == 0 && x % 8 < 4) || (y % 8 == 4 && x % 8 >= 4)) :
                (x % 2 == 0 || x >= image.w - 1 - y);
            image.pixels[(y * image.w + x) * 4 + 3] = f ? 255 : 0;
        }
        Budget budget(Budget::FixedLimitForTest{}, 256 * MiB); JobWork meter(512 * 512);
        auto lut = alphaLut(128,0);
        auto result = label(image.view(), lut, budget, meter);
        ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
        EXPECT_EQ(result.consumed, meter.visits()); EXPECT_GE(result.consumed, 2u * 512 * 512);
        properties(image, lut, result.value); // independent BFS: chains/rings/labyrinth
        auto before = meter.visits(); ASSERT_TRUE(meter.advance(meter.limit() - before).ok());
        auto refused = label(image.view(), lut, budget, meter);
        EXPECT_FALSE(refused.ok()); EXPECT_GT(refused.consumed, 0u);
        EXPECT_EQ(meter.visits(), meter.limit() + refused.consumed);
    }
}
TEST(ExplodeBitmapLabel, Round2CancellationInsideUnionAndSorting)
{
    struct Injection {
        LabelPhase target; std::shared_ptr<std::atomic<bool>> flag;
        std::chrono::steady_clock::time_point requested{};
        bool seen = false;
    };
    for (auto phase : {LabelPhase::unionFind, LabelPhase::sorting}) {
        Image image(512,512);
        for (unsigned y = 0; y < image.h; ++y) for (unsigned x = 0; x < image.w; ++x)
            image.pixels[(y * image.w + x) * 4 + 3] = (x % 4 < 2 && y % 4 < 2) ? 255 : 0;
        Injection state{phase, std::make_shared<std::atomic<bool>>(false)};
        LabelOptions options; options.observerData = &state;
        options.observe = [](LabelPhase current, void *data) noexcept {
            auto &s = *static_cast<Injection *>(data);
            if (current == s.target) { s.seen = true; s.requested = std::chrono::steady_clock::now(); s.flag->store(true); }
        };
        Budget budget(Budget::FixedLimitForTest{}, 256 * MiB); JobWork meter(512 * 512);
        auto result = label(image.view(), alphaLut(128,0), budget, meter, Stop(state.flag), options);
        auto returned = std::chrono::steady_clock::now();
        ASSERT_TRUE(state.seen); EXPECT_EQ(result.outcome.status, Status::canceled);
        EXPECT_LT(returned - state.requested, std::chrono::milliseconds(100));
        EXPECT_EQ(result.consumed, meter.visits()); EXPECT_GT(result.consumed, 2u * 512 * 512 / 2);
        EXPECT_EQ(budget.reserved(), 0u); EXPECT_EQ(result.value.runs(), nullptr);
    }
}
