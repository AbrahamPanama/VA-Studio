// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in EB7 measurements, never a regression-label member. See internal evidence notes.
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstdio>
#include <stdexcept>
#include <map>
#include <sstream>
#include "display/control/canvas-item.h"
#include "display/control/canvas-item-context.h"
#include "display/control/canvas-item-ptr.h"
#include <thread>
#include <gtkmm/window.h>
#include "bitmap-explode-chemistry.h"
#include "desktop.h"
#include "document.h"
#include "display/cairo-utils.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "selection.h"
#include "ui/dialog/explode-bitmap.h"
#include "ui/explode-bitmap-overlay.h"
#include "ui/explode-bitmap-panel-preparation.h"
#include "ui/explode-bitmap-undo.h"
#include "xml/repr.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
namespace Inkscape::UI::Dialog {
using namespace Bitmap;
namespace {
std::uint64_t ns(JobClock::time_point a, JobClock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();
}
std::string quote(std::string const &s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { r += '\\'; r += c; }
        else if (c < 32) { char b[7]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += c;
    }
    return r + '"';
}
std::string env(char const *k) { auto v = std::getenv(k); return v ? v : "missing"; }
std::uint64_t rss() {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{}; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &n) == KERN_SUCCESS)
        return info.resident_size;
#endif
    RawMemory m; return nativeMemoryProbe().read(m) ? m.footprint : 0;
}
struct Sample {
    std::string phase, input, diagnostic;
    unsigned repeat; std::uint64_t units, pixels, bytes, elapsed, baseline, peak, ledger;
    bool ok;
    std::uint64_t cropPixels = 0;
};
Sample finishedBeforeStop(Sample sample, Outcome result) {
    sample.phase="stop_not_exercised";
    sample.diagnostic=std::string("phase finished before request: ")+result.diagnostic;
    sample.ok=result.ok();
    return sample;
}
TEST(CalibrationEvidence, EarlyStopPreservesActualCompletionOrRefusal) {
    for(auto status:{Status::changed,Status::unchanged,Status::unavailable,Status::failed,Status::canceled}) {
        Outcome result{status,"original phase diagnostic"};
        auto sample=finishedBeforeStop({},result);
        EXPECT_EQ(sample.phase,"stop_not_exercised"); EXPECT_EQ(sample.ok,result.ok());
        EXPECT_EQ(sample.diagnostic,"phase finished before request: original phase diagnostic");
    }
}
// Explicit headless corpus oracle. No application, desktop or window is created.
// Keep private fixture paths and the output outside the source tree.
TEST(CalibrationEvidence, CorpusCounts) {
    auto manifest = std::getenv("EB_LIM2_CORPUS");
    if (!manifest) GTEST_SKIP() << "Set EB_LIM2_CORPUS and EB_LIM2_COUNTS for local corpus evidence.";
    auto output = std::getenv("EB_LIM2_COUNTS"); ASSERT_TRUE(output);
    std::ifstream files(manifest); ASSERT_TRUE(files);
    std::ofstream counts(output); ASSERT_TRUE(counts);
    std::string path;
    while (std::getline(files, path)) {
        SCOPED_TRACE(path);
        std::ifstream file(path, std::ios::binary); ASSERT_TRUE(file);
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        for (bool refine : {false, true}) {
            Budget budget(Budget::FixedLimitForTest{}, 1536*MiB);
            auto decoded = decode({{bytes.data(), bytes.size(), {}}, {}}, budget);
            ASSERT_TRUE(decoded.ok()) << decoded.outcome.diagnostic;
            auto &r = decoded.value;
            FinalGrid grid; grid.width=r.width; grid.height=r.height;
            grid.dpiX=grid.dpiY=96;
            // PNG pHYs, matching the imported-image fixture's physical density.
            auto be32=[&](std::size_t at) { return (std::uint32_t(bytes[at])<<24) |
                (std::uint32_t(bytes[at+1])<<16) | (std::uint32_t(bytes[at+2])<<8) | bytes[at+3]; };
            for (std::size_t at=8; at+12<=bytes.size();) {
                auto length=be32(at); if (length>bytes.size()-at-12) break;
                if (length==9 && !std::memcmp(bytes.data()+at+4,"pHYs",4) && bytes[at+16]==1) {
                    if (auto x=be32(at+8)) grid.dpiX=x*.0254;
                    if (auto y=be32(at+12)) grid.dpiY=y*.0254;
                }
                at+=length+12;
            }
            grid.pixels=std::move(r.pixels);
            Recipe recipe; recipe.bypassAlpha=!refine; recipe.threshold=128; recipe.softness=40;
            auto lut=recipeAlphaLut(recipe); JobWork work(std::uint64_t(grid.width)*grid.height);
            auto regions=label(grid.view(),lut,budget,work); ASSERT_TRUE(regions.ok()) << regions.outcome.diagnostic;
            EnclosureOptions eo; eo.speckDpiX=grid.dpiX; eo.speckDpiY=grid.dpiY;
            auto enclosed=enclose(regions.value,budget,work,{},eo); ASSERT_TRUE(enclosed.ok()) << enclosed.outcome.diagnostic;
            auto metric=OrthogonalMetric::fromDpi(grid.dpiX,grid.dpiY); ASSERT_TRUE(metric.ok());
            auto partition=attach(enclosed.value,metric.value,budget,work); ASSERT_TRUE(partition.ok()) << partition.outcome.diagnostic;
            counts << "{\"file\":" << quote(path) << ",\"refine\":" << (refine?"true":"false")
                   << ",\"width\":" << grid.width << ",\"height\":" << grid.height
                   << ",\"count\":" << partition.value.pieceCount << "}\n";
            counts.flush();
        }
    }
}
class Evidence {
public:
    explicit Evidence(std::string path) : path(std::move(path)) { save(); }
    template <typename F> void measure(std::string phase, std::string input, unsigned repeat,
                                      std::uint64_t units, std::uint64_t pixels, std::uint64_t bytes,
                                      Budget *budget, F f) {
        std::atomic<bool> finished{false}; std::atomic<std::uint64_t> peak{rss()};
        auto baseline = peak.load();
        std::thread sampler([&] { while (!finished.load()) {
            peak.store(std::max(peak.load(), rss())); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } });
        auto start = JobClock::now(); Outcome result;
        try { result = f(); } catch (...) { result = {Status::failed,"measurement callback threw"}; }
        auto end = JobClock::now();
        peak.store(std::max(peak.load(), rss())); finished = true; sampler.join();
        samples.push_back({std::move(phase),std::move(input),result.diagnostic,repeat,units,pixels,bytes,
                           ns(start,end),baseline,peak.load(),budget ? budget->reserved() : 0,result.ok()});
        save();
        auto const &s = samples.back(); std::cout << s.phase << " " << s.input << " #" << repeat << " "
            << s.elapsed/1e6 << " ms " << s.diagnostic << std::endl;
    }
    void save() const {
        auto memory = sampleMemory(); double load[3]{};
#ifndef _WIN32
        getloadavg(load, 3);
#endif
        std::string cpu = env("PROCESSOR_IDENTIFIER");
#ifdef __APPLE__
        char value[256]{}; size_t length = sizeof value;
        if (!sysctlbyname("machdep.cpu.brand_string", value, &length, nullptr, 0)) cpu = value;
#endif
        std::ofstream out(path, std::ios::trunc);
        out << "{\n\"schema\":\"PlatformEvidence-EB7-promote-v2\",\"platform\":"
#ifdef __APPLE__
            << "\"macOS\""
#elif defined(_WIN32)
            << "\"Windows\""
#else
            << "\"unqualified\""
#endif
            << ",\"cpu\":" << quote(cpu) << ",\"ram_bytes\":" << memory.value.physical
            << ",\"build_id\":" << quote(env("EB7_BUILD_ID")) << ",\"binary_sha256\":" << quote(env("EB7_BINARY_SHA256"))
            << ",\"source_manifest_sha256\":" << quote(env("EB7_SOURCE_SHA256"))
            << ",\"scope\":" << quote(env("EB7_SCOPE")) << ",\"source_mp\":" << quote(env("EB7_SOURCE_MP"))
            << ",\"load_1_5_15\":[" << load[0] << ',' << load[1] << ',' << load[2] << ']'
            << ",\"chosen_percentile\":100,\"cost_margin_multiplier\":2,\"rss_sampling_ms\":1,\n\"samples\":[\n";
        for (size_t i=0;i<samples.size();++i) {
            auto const &s = samples[i];
            if (i) out << ",\n";
            out << "{\"phase\":" << quote(s.phase) << ",\"input\":" << quote(s.input) << ",\"repeat\":" << s.repeat
                << ",\"units\":" << s.units << ",\"pixels\":" << s.pixels << ",\"crop_pixels\":" << s.cropPixels << ",\"bytes\":" << s.bytes
                << ",\"pixels_per_second\":" << (s.elapsed ? std::uint64_t(double(s.pixels)*1e9/s.elapsed) : 0)
                << ",\"bytes_per_second\":" << (s.elapsed ? std::uint64_t(double(s.bytes)*1e9/s.elapsed) : 0)
                << ",\"ns\":" << s.elapsed << ",\"baseline_rss\":" << s.baseline << ",\"peak_rss\":" << s.peak
                << ",\"ledger_end_bytes\":" << s.ledger << ",\"ok\":" << (s.ok ? "true" : "false")
                << ",\"diagnostic\":" << quote(s.diagnostic) << '}';
        }
        std::map<std::string,std::uint64_t> worst;
        for (auto const &s : samples) worst[s.phase] = std::max(worst[s.phase],s.elapsed);
        out << "\n],\"worst_ns\":{"; bool first=true;
        for (auto const &[phase,value] : worst) { if (!first) out << ','; first=false; out << quote(phase) << ':' << value; }
        out << "},\"qualification\":\"optional diagnostic only; never consulted by production admission\"}\n";
        if (!out) throw std::runtime_error("Cannot save EB7 evidence");
    }
    std::string path; std::vector<Sample> samples;
};
TargetSnapshot plainTarget(unsigned w,unsigned h,bool skew=false) {
    TargetSnapshot s; s.bitmap=1; s.destinationParent=2; s.generation=3; s.supportability=Supportability::Supported;
    TargetContext c; c.identity=1; c.parent=2; c.itemToDocument={.32,0,skew?.08:0,.32,0,0};
    c.pixelToItem={1,0,0,1,0,0}; c.viewport={0,0,double(w),double(h)};
    TargetContext p; p.identity=2; p.itemToDocument={1,0,0,1,0,0}; s.contexts={c,p}; return s;
}
TEST(CalibrationEvidence, LimitsPanelWorkerWithoutWindow) {
    auto folder=std::getenv("EB_LIM2_FIXTURES");
    if (!folder) GTEST_SKIP() << "Set EB_LIM2_FIXTURES for local ceiling evidence.";
    recordBitmapMainThread();
    {
        std::ifstream file(std::string(folder)+"/blobs-150-5001x4000.png",std::ios::binary);
        ASSERT_TRUE(file); std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
        Budget budget(Budget::FixedLimitForTest{},1536*MiB);
        auto decoded=decode({{bytes.data(),bytes.size(),{}},{}},budget); ASSERT_TRUE(decoded.ok());
        EXPECT_EQ(decoded.value.width,5001u); EXPECT_EQ(decoded.value.height,4000u);
        auto limits=measuredLimits(PlatformEvidence{EvidencePlatform::Mac});
        LatencyWork work; work.width=decoded.value.width; work.height=decoded.value.height;
        EXPECT_FALSE(admitLatency(work,limits).ok());
        auto fit=fitExplodeSourceSize(work.width,work.height);
        EXPECT_EQ(fit.width,5000u); EXPECT_EQ(fit.height,3999u);
        EXPECT_EQ(explodeSourceSizeMessage(work.width,work.height),
            "This image is 5001 × 4000 px. Explode Bitmap works with images up to 5000 × 5000 px.");
    }
    for (unsigned n : {150u,151u}) {
        SCOPED_TRACE(n);
        std::ifstream file(std::string(folder)+"/blobs-"+std::to_string(n)+"-5000x5000.png",std::ios::binary);
        ASSERT_TRUE(file); std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
        auto memory=sampleMemory(); ASSERT_TRUE(memory.ok());
        auto plan=PanelPreparation::resources(5000,5000,bytes.size(),0);
        auto admission=admit(plan,memory.value);
        auto constrained=admit(plan,{8192*MiB,300*MiB,512*MiB,true});
        EXPECT_FALSE(constrained.ok()); // A - recovery = 44 MiB cannot hold 8P + overhead.
        EXPECT_EQ(constrained.value.limit,44*MiB);
        EXPECT_TRUE(admit(plan,{8192*MiB,768*MiB,512*MiB,true}).ok());
        std::cout << "LIM2 panel n=" << n << " available=" << memory.value.available
                  << " baseline=" << memory.value.baseline << " J=" << admission.value.limit
                  << " plan_peak=" << admission.value.peak
                  << " admission_margin=" << std::int64_t(admission.value.limit)-std::int64_t(admission.value.peak) << " admitted=" << admission.ok() << std::endl;
        ASSERT_TRUE(admission.ok()) << admission.outcome.diagnostic;
        JobInput job; job.pixels=25000000;
        job.work=+[](JobInput const &job,Stop stop,JobWork &work,JobReporter &reporter) {
            auto result=PanelPreparation::calculate(job,stop,work,reporter);
            std::cout << "LIM2 panel_work visits=" << work.visits() << " limit=" << work.limit() << std::endl;
            return result;
        };
        job.storage.budget=std::make_shared<Budget>(1536*MiB);
        ASSERT_TRUE(job.storage.budget->recheck(memory.value).ok());
        auto base64=g_base64_encode(bytes.data(),bytes.size()); ASSERT_TRUE(base64);
        auto input=std::make_shared<PanelPreparation::Input>(); input->decodedBytes=bytes.size();
        input->target=plainTarget(5000,5000); input->recipe.threshold=128; input->recipe.softness=40;
        job.storage.payload=input;
        ASSERT_TRUE(job.storage.budget->acquire(Stage::input,std::strlen(base64)+sizeof(*input),job.storage.reservation).ok());
        job.storage.bytes.assign(base64,base64+std::strlen(base64)); g_free(base64);
        bool done=false; JobResult result; unsigned progress=0;
        auto start=JobClock::now();
        std::atomic<std::uint64_t> workerPeak{rss()};
        std::jthread sampler([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                workerPeak.store(std::max(workerPeak.load(),rss()));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        BitmapJobs jobs([&](Ticket,JobResult r){ result=std::move(r); done=true; },
                        [&](Ticket,JobProgress){ ++progress; },JobClock::now,false);
        jobs.request(std::move(job));
        while (!done && JobClock::now()-start<std::chrono::seconds(45)) {
            Glib::MainContext::get_default()->iteration(false); // plain channel wake, no window
            jobs.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        sampler.request_stop(); sampler.join();
        workerPeak.store(std::max(workerPeak.load(),rss()));
        ASSERT_TRUE(done); ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
        auto out=std::dynamic_pointer_cast<PanelPreparation::Output const>(result.value.payload); ASSERT_TRUE(out);
        EXPECT_TRUE(out->explodeOutcome.ok()) << out->explodeOutcome.diagnostic;
        EXPECT_EQ(out->count,n); EXPECT_GT(progress,0u);
        if (n==150) { EXPECT_EQ(out->pieces.count(),150u); EXPECT_TRUE(out->pngStarted); }
        else { EXPECT_EQ(out->pieces.count(),0u); EXPECT_FALSE(out->pngStarted); }
        std::cout << "LIM2 panel_result n=" << n << " elapsed_ms=" << ns(start,JobClock::now())/1e6
                  << " peak_rss=" << workerPeak.load()
                  << " progress_events=" << progress << " pieces=" << out->pieces.count()
                  << " topology_peak=" << out->topologyPeak << " ledger=" << out->budget->reserved()
                  << " outlines=" << bool(out->outlines.storage) << " diagnostic=" << out->explodeOutcome.diagnostic << std::endl;
        jobs.close();
    }
    drainReaper(); EXPECT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
}
std::string pngUri(unsigned w,unsigned h,unsigned count=0,bool noise=false,unsigned block=3) {
    Pixbuf p(gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,w,h)); auto data=p.pixels();
    std::memset(data,0,p.rowstride()*h); std::uint32_t seed=0xeba7;
    for (unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        auto q=data+y*p.rowstride()+4*x; seed=1664525*seed+1013904223;
        q[0]=seed>>24; q[1]=seed>>16; q[2]=seed>>8;
        q[3]=noise ? ((y/block)%2 && (x/block)%2 ? 200 : 0) : 0;
    }
    if (!noise) for(unsigned i=0;i<count;++i) data[(i/200*3)*p.rowstride()+(i%200*3)*4+3]=200;
    auto uri=sp_image_encode_png_data_uri(p); if(!uri) throw std::runtime_error("fixture PNG failed"); return *uri;
}
std::vector<std::uint8_t> encoded(std::string const &uri) {
    gsize n=0; auto p=g_base64_decode(uri.c_str()+22,&n); std::vector<std::uint8_t> r(p,p+n); g_free(p); return r;
}
class ContextProbe final : public CanvasItem {
public:
    using CanvasItem::CanvasItem;
    CanvasItemContext &context() { return *_context; }
private:
    void _update(bool) override {}
    void _render(CanvasItemBuffer &) const override {}
};
// Latch inside a real ceiling-size decoder call to measure nonjoining panel
// destruction even when native panel preparation refuses its larger RAM plan.
struct ClosingDecode : JobPayload {
    mutable std::atomic<bool> entered{false}, release{false};
    static JobResult run(JobInput const &input,Stop stop,JobWork &,JobReporter &) {
        auto &gate=static_cast<ClosingDecode const &>(*input.storage.payload);
        ValidatedInput in{{input.storage.bytes.data(),input.storage.bytes.size(),"image/png"},{}};
        in.user=const_cast<ClosingDecode *>(&gate);
        in.progress=[](void *data,std::uint64_t,std::uint64_t,unsigned phase) {
            auto &g=*static_cast<ClosingDecode *>(data);
            if(phase!=1 || g.entered.exchange(true)) return;
            auto until=JobClock::now()+std::chrono::seconds(2);
            while(!g.release.load() && JobClock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        return {decode(in,*input.storage.budget,stop).outcome,{}};
    }
};
struct Fault {
    JobClock::time_point rollbackStart{}; bool fired=false;
    static void callback(PublishStage stage,unsigned,void *data) {
        auto &f=*static_cast<Fault *>(data);
        if(stage==PublishStage::Settlement) { f.fired=true; throw std::bad_alloc(); }
        if(stage==PublishStage::Rollback) f.rollbackStart=JobClock::now();
    }
};
}
// Reuse the panel's existing test friendship without modifying the panel-owned files.
struct ExplodeBitmapPanelTest : testing::Test {
    void SetUp() override {
        static auto *app=[] { g_setenv("INKSCAPE_APP_ID_TAG","eb7calibration",true); return new InkscapeApplication; }();
        ASSERT_TRUE(app->gtk_app()); if(!Application::exists()) Application::create(false); recordBitmapMainThread();
    }
    void publication(Evidence &e,unsigned n,unsigned rep,bool fail,unsigned mp=0,bool fixed=false) {
        unsigned w=fixed ? 5000 : mp ? mp*1000 : 900, h=fixed ? 5000 : mp ? 1000 : 6;
        auto name=std::string(n>MaxExplodePieces ? "stress-" : "pieces-")+std::to_string(n)+(fixed ? "-5000x5000" : mp ? "-"+std::to_string(mp)+"MP" : "-small");
        Budget b(Budget::FixedLimitForTest{},1536*MiB); FinalGrid grid;
        grid.width=w; grid.height=h; grid.dpiX=grid.dpiY=300;
        grid.pixelToParent=grid.pixelToDocument={.32,0,0,.32,0,0};
        ASSERT_TRUE(grid.pixels.allocate(b,Stage::prepared,std::uint64_t(w)*h,4).ok());
        auto data=reinterpret_cast<unsigned char *>(grid.pixels.data());
        std::memset(data,0,grid.pixels.size());
        if (fixed) {
            // Explicit local fixtures: anti-aliased blobs and holes, never private images in source.
            auto path=env("EB_LIM2_FIXTURES")+"/blobs-"+std::to_string(n)+"-5000x5000.png";
            std::ifstream file(path,std::ios::binary); ASSERT_TRUE(file) << path;
            std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
            Result<DecodedRaster> decoded;
            e.measure("decode",name,rep,n,w*h,bytes.size(),&b,[&] {
                decoded=decode({{bytes.data(),bytes.size(),{}},{}},b); return decoded.outcome;
            });
            ASSERT_TRUE(decoded.ok()) << decoded.outcome.diagnostic;
            ASSERT_EQ(decoded.value.width,w); ASSERT_EQ(decoded.value.height,h);
            Recipe recipe; recipe.threshold=128; recipe.softness=40;
            auto lut=recipeAlphaLut(recipe);
            e.measure("alpha_preparation",name,rep,n,w*h,bytes.size(),&b,[&] {
                std::memcpy(data,decoded.value.pixels.data(),grid.pixels.size());
                for (std::uint64_t i=0;i<std::uint64_t(w)*h;++i) data[4*i+3]=lut[data[4*i+3]];
                return Outcome{};
            });
        } else if (mp) {
            // Two disconnected interleaved combs, both with almost full-source
            // crop boxes. EB7_ENTROPY=noise also probes the high-entropy RAM envelope.
            bool noise=env("EB7_ENTROPY")=="noise";
            std::uint32_t seed=0xeba7;
            for(unsigned y=2;y<h-2;++y) for(unsigned x=2;x<w-2;++x) {
                seed=1664525*seed+1013904223;
                bool left=x==2 && y<h-5, right=x==w-3 && y>=5;
                bool a=(y>=2 && y<240) || (y>=502 && y<740);
                bool c=(y>=246 && y<484) || (y>=746 && y<h-2);
                bool visible=left || right || (a && x<w-5) || (c && x>=5);
                auto q=data+4*(std::uint64_t(y)*w+x);
                if(visible) { q[0]=noise ? seed>>24 : 70; q[1]=noise ? seed>>16 : 180; q[2]=noise ? seed>>8 : 210; q[3]=200; }
            }
            for(unsigned i=0;i<n-2;++i) {
                auto q=data+4*(std::uint64_t(492)*w+10+3*i);
                q[0]=70; q[1]=180; q[2]=210; q[3]=200;
            }
        } else for(unsigned i=0;i<n;++i) data[4*(3*i)+3]=200;
        auto gdk=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,w,h); ASSERT_TRUE(gdk);
        for(unsigned y=0;y<h;++y) std::memcpy(gdk_pixbuf_get_pixels(gdk)+y*gdk_pixbuf_get_rowstride(gdk),data+std::uint64_t(y)*w*4,w*4);
        Pixbuf source(gdk); auto uri=sp_image_encode_png_data_uri(source); ASSERT_TRUE(uri);
        auto doc=SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='"+std::to_string(w*.32)+"' height='"+std::to_string(h*.32)+"'><image id='im' width='"+std::to_string(w*.32)+"' height='"+std::to_string(h*.32)+"' href='"+*uri+"'/></svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        auto desktop=std::make_unique<SPDesktop>(doc->getNamedView());
        desktop->zoom_absolute({0,0},248.0/(w*.32));
        auto image=cast<SPImage>(doc->getObjectById("im")); desktop->getSelection()->set(image); doc->ensureUpToDate();
        JobWork work(std::uint64_t(w)*h); AlphaLut lut; for(unsigned i=0;i<256;++i) lut[i]=i;
        Result<Regions> regions; Result<Partition> partition;
        e.measure("count",name,rep,n,w*h,0,&b,[&] {
            regions=label(grid.view(),lut,b,work); if(!regions.ok()) return regions.outcome;
            EnclosureOptions eo; eo.speckDpiX=grid.dpiX; eo.speckDpiY=grid.dpiY;
            auto enclosed=enclose(regions.value,b,work,{},eo); if(!enclosed.ok()) return enclosed.outcome;
            partition=fixed ? attach(enclosed.value,OrthogonalMetric::fromDpi(grid.dpiX,grid.dpiY).value,b,work) : std::move(enclosed);
            return partition.outcome;
        });
        ASSERT_TRUE(partition.ok()) << partition.outcome.diagnostic;
        std::cout << "LIM2 count=" << partition.value.pieceCount << " topology_peak=" << partition.value.topologyPeak
                  << " visits=" << work.visits() << " work_limit=" << work.limit() << std::endl;
        Result<EncodedPieces> pieces;
        e.measure("encoding",name,rep,n,w*h,0,&b,[&] {
            EncodeOptions options; options.work=&work;
            pieces=encode(grid,partition.value,b,{},options); return pieces.outcome;
        });
        if(!pieces.ok()) {
            e.samples.push_back({"fixture_refused",name,pieces.outcome.diagnostic,rep,n,w*h,0,0,rss(),rss(),b.reserved(),false}); e.save(); return;
        }
        ASSERT_EQ(pieces.value.count(),n);
        e.samples.back().bytes=pieces.value.encodedBytes; e.samples.back().cropPixels=pieces.value.cropArea; e.save();
        std::cout << "LIM2 crop_pixels=" << pieces.value.cropArea << " png_bytes=" << pieces.value.encodedBytes
                  << " href_bytes=" << pieces.value.hrefBytes << " ledger=" << b.reserved()
                  << " encoded_visits=" << work.visits() << std::endl;
        EXPECT_LE(pieces.value.cropArea,2ULL*w*h);
        if(mp && !fixed) EXPECT_GE(pieces.value.cropArea,1.98*w*h);
        PlatformEvidence diagnostic; diagnostic.platform=EvidencePlatform::Mac; diagnostic.observationNs=1;
        DependencyLease lease(*desktop,diagnostic); auto target=resolve(*desktop,Intent::Explode); ASSERT_TRUE(target.ok());
        Prepared p; p.target=target.value;
        e.measure("capture",name,rep,0,w*h,uri->size(),&b,[&] {
            p.dependencies=capture(target.value); return p.dependencies ? Outcome{} : Outcome{Status::failed,"capture failed"};
        });
        e.samples.back().units=lease.nativeWatchCount(); e.samples.back().cropPixels=pieces.value.cropArea; e.save(); ASSERT_TRUE(p.dependencies);
        if(mp && !fixed && !fail && n==150) {
            auto outlines=prepareOutlines(partition.value,grid,b,work); ASSERT_TRUE(outlines.ok()) << outlines.outcome.diagnostic;
            outlines.value.dependencies=p.dependencies; outlines.value.generation=1; outlines.value.limits.outlineUnits=outlineSegmentLimit;
            unsigned strokes=0; OverlayTestHooks hooks;
            hooks.afterStroke=[](void *v) noexcept { ++*static_cast<unsigned *>(v); }; hooks.context=&strokes;
            auto probe=make_canvasitem<ContextProbe>(desktop->getCanvasTemp());
            probe->context().setAffine(Geom::Scale(248.0/(w*.32)));
            BitmapOverlay overlay(&hooks); ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
            auto installed=overlay.installOutlines(*desktop,outlines.value,1);
            ASSERT_TRUE(installed.ok()) << installed.diagnostic << " segments=" << outlines.value.storage->count;
            auto item=overlay.canvasItem(); ASSERT_TRUE(item); item->update(false); ASSERT_TRUE(item->get_bounds());
            auto bounds=*item->get_bounds();
            auto rect=Geom::IntRect(int(std::floor(bounds.min().x())),int(std::floor(bounds.min().y())),int(std::ceil(bounds.max().x())),int(std::ceil(bounds.max().y())));
            // Render every edge into one complete buffer, including update/flush;
            // verify both the actual stroke callback and nonempty pixels.
            auto surface=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,rect.width(),rect.height());
            auto cr=Cairo::Context::create(surface); CanvasItemBuffer buffer{rect,1,cr,false};
            e.measure("outline",name,rep,outlines.value.storage->count,w*h,0,&b,[&] {
                item->update(false); item->render(buffer); surface->flush();
                return strokes && overlay.ready() ? Outcome{} : Outcome{Status::failed,"outline did not draw"};
            });
            EXPECT_TRUE(std::any_of(surface->get_data(),surface->get_data()+surface->get_stride()*surface->get_height(),[](auto v){return v!=0;}));
            e.samples.back().cropPixels=pieces.value.cropArea; e.save();
        }
        p.session=sessionJobIdentity(logicalImageIdentity(*image),target.value); p.grid=&grid; p.pieces=&pieces.value; p.budget=&b;
        p.limits=measuredLimits(macDependencyEvidence());
        // Timing-only opt-in: preserve the native refusal separately. This does
        // not change production admission or any work/topology/encoding cap.
        struct DiagnosticRoom : MemoryProbe {
            bool read(RawMemory &m) const noexcept override { m={8192*MiB,6144*MiB,256*MiB}; return true; }
        } diagnosticRoom;
        if (env("EB_LIM2_DIAGNOSTIC_RAM")=="1") p.probe=&diagnosticRoom;
        if (fixed) {
            // Exact publication plan for this simple source (id=im, label=Bitmap,
            // no clip/filter references, no pre-existing history).
            auto sourceXml=sp_repr_write_buf(image->getRepr(),0,false,Glib::QueryQuark(0u),0,0).raw();
            auto originalBytes=sourceXml.size();
            auto nodeBytes=std::uint64_t(n)*(4096+6*(2+6));
            auto staging=3*pieces.value.hrefBytes+nodeBytes;
            auto recovery=8ULL*w*h+2*originalBytes;
            std::uint64_t lower=0;
            ASSERT_TRUE(postCommitLowerBound(pieces.value.cropArea,pieces.value.encodedBytes,pieces.value.hrefBytes,lower));
            auto history=preflightUndo(*doc,{false,0,originalBytes+staging});
            auto peak=staging+nodeBytes+recovery+8*pieces.value.cropArea+lower+history.historyTermBytes+history.redoTermBytes;
            auto actual=sampleMemory(); ASSERT_TRUE(actual.ok()); std::uint64_t nativeJ=0;
            ASSERT_TRUE(admissionLimit(actual.value,nativeJ).ok());
            std::cout << "LIM2 publication_plan peak=" << peak << " retained_ledger=" << b.reserved()
                      << " native_available=" << actual.value.available << " native_J=" << nativeJ
                      << " need=" << peak+b.reserved()
                      << " admission_margin=" << std::int64_t(nativeJ)-std::int64_t(peak+b.reserved())
                      << " recovery=" << recovery << " diagnostic_probe=" << bool(p.probe) << std::endl;
        }
        if(n==300) {
            p.limits.extremeStressForTest=true;
            p.limits.publicationUnits=p.limits.rollbackUnits=p.limits.historyUnits=300;
        }
        p.activation=std::make_shared<DependencyRequest>(); auto before=sp_repr_save_buf(doc->getReprDoc()).raw();
        JobClock::time_point publishEnd;
        Fault f; if(fail) p.hooks={Fault::callback,&f}; Outcome result;
        e.measure(fail?"publication_with_rollback":"publication",name,rep,n,w*h,pieces.value.encodedBytes,&b,[&] {
            result=publishExplode(*desktop,p,1); publishEnd=JobClock::now(); return fail && f.fired && result.status==Status::failed ? Outcome{} : result;
        });
        e.samples.back().cropPixels=pieces.value.cropArea; e.save();
        if(fixed) ASSERT_EQ(result.status,fail ? Status::failed : Status::changed) << result.diagnostic;
        if(fail) {
            if(fixed) ASSERT_TRUE(f.fired);
            if(!f.fired) return;
            EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(),before);
            auto copy=e.samples.back(); copy.phase="rollback"; copy.elapsed=ns(f.rollbackStart,publishEnd);
            e.samples.push_back(copy); e.save();
            std::cout << "rollback " << name << " #" << rep << " " << copy.elapsed/1e6 << " ms" << std::endl;
            return;
        }
        if(result.status!=Status::changed) return;
        auto items=desktop->getSelection()->items_vector(); ASSERT_EQ(items.size(),n);
        desktop->getSelection()->clear();
        e.measure("select",name,rep,n,w*h,0,&b,[&] { desktop->getSelection()->setList(items); doc->ensureUpToDate(); return Outcome{}; });
        e.measure("undo",name,rep,n,w*h,0,&b,[&] { bool ok=DocumentUndo::undo(doc.get()); doc->ensureUpToDate(); return ok?Outcome{}:Outcome{Status::failed,"Undo failed"}; });
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(),before);
        e.measure("redo",name,rep,n,w*h,0,&b,[&] { bool ok=DocumentUndo::redo(doc.get()); doc->ensureUpToDate(); return ok?Outcome{}:Outcome{Status::failed,"Redo failed"}; });
        for(auto i=e.samples.size()-3;i<e.samples.size();++i) e.samples[i].cropPixels=pieces.value.cropArea;
        e.save();
    }
    void documentCapture(Evidence &e,unsigned rep,unsigned n) {
        auto uri=pngUri(3,1,1); std::string body="<image id='im' width='3' height='1' href='"+uri+"'/>";
        for(unsigned i=1;i<n;++i) body+="<rect width='1' height='1'/>";
        auto doc=SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'>"+body+"</svg>"); ASSERT_TRUE(doc);
        doc->ensureUpToDate(); auto desktop=std::make_unique<SPDesktop>(doc->getNamedView());
        desktop->getSelection()->set(cast<SPImage>(doc->getObjectById("im"))); doc->ensureUpToDate();
        PlatformEvidence diagnostic; diagnostic.platform=EvidencePlatform::Mac; diagnostic.observationNs=1;
        DependencyLease lease(*desktop,diagnostic); auto target=resolve(*desktop,Intent::Explode); ASSERT_TRUE(target.ok());
        e.measure("document_capture","objects-"+std::to_string(n),rep,n,3,uri.size(),nullptr,[&] {
            bool admitted=bool(capture(target.value));
            return admitted ? Outcome{} : Outcome{Status::unavailable,"Diagnostic observation limit refused capture"};
        });
        EXPECT_TRUE(e.samples.back().ok);
        // Preserve the graphical input size in the name and the actual native
        // watch count (root/defs/namedview included) in the raw measurement.
        e.samples.back().units=lease.nativeWatchCount(); e.save();
    }
    void backends(Evidence &e,unsigned rep,unsigned mp) {
        unsigned w=mp*1000,h=1000;
        Pixbuf pixels(gdk_pixbuf_new(GDK_COLORSPACE_RGB,false,8,w,h));
        auto data=pixels.pixels(); std::uint32_t seed=0xeba7;
        for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
            auto q=data+y*pixels.rowstride()+3*x; seed=1664525*seed+1013904223;
            q[0]=seed>>24; q[1]=seed>>16; q[2]=seed>>8;
        }
        for(auto format:{"png","jpeg","webp"}) {
            gchar *buffer=nullptr; gsize length=0; GError *error=nullptr;
            std::vector<std::uint8_t> bytes;
            if(std::string(format)=="webp" && env("EB7_WEBP_DIR")!="missing") {
                std::ifstream in(env("EB7_WEBP_DIR")+"/"+std::to_string(mp)+"MP.webp",std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in),{});
            } else if(gdk_pixbuf_save_to_buffer(pixels.getPixbufRaw(),&buffer,&length,format,&error,nullptr)) {
                bytes.assign(buffer,buffer+length);
            }
            g_free(buffer);
            auto name=std::string(format)+"-"+std::to_string(mp)+"MP";
            if(bytes.empty()) {
                e.samples.push_back({"backend_unmeasured",name,error?error->message:"No encoded fixture / WebP saver",rep,0,w*h,0,0,rss(),rss(),0,false});
                if(error) g_error_free(error); e.save(); continue;
            }
            if(error) g_error_free(error);
            std::string mime=std::string("image/")+format; Budget budget(Budget::FixedLimitForTest{},1536*MiB);
            Outcome decoded;
            e.measure("decode",name,rep,w*h,w*h,bytes.size(),&budget,[&] {
                std::thread worker([&] { auto r=decode({{bytes.data(),bytes.size(),mime},{}},budget);
                    decoded=r.outcome; if(r.ok() && (r.value.width!=w || r.value.height!=h)) decoded={Status::failed,"wrong decoded dimensions"}; });
                worker.join(); return decoded;
            });
            if(!decoded.ok()) continue;
            auto flag=std::make_shared<std::atomic<bool>>(false); Stop stop(flag);
            std::atomic<bool> started{false},done{false}; JobClock::time_point end;
            Outcome result;
            std::thread worker([&] { started=true; result=decode({{bytes.data(),bytes.size(),mime},{}},budget,stop).outcome; end=JobClock::now(); done=true; });
            while(!started.load()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(1+rep));
            bool sent=!done.load(); auto requested=JobClock::now(); flag->store(true); worker.join();
            e.samples.push_back({sent?"stop_ack":"stop_not_exercised",name,result.diagnostic,rep,1,w*h,bytes.size(),sent&&end>=requested?ns(requested,end):0,rss(),rss(),budget.reserved(),sent&&result.status==Status::canceled}); e.save();
        }
    }
    void workers(Evidence &e,unsigned rep,unsigned w,unsigned h,std::string name) {
        auto uri=pngUri(w,h,0,true,16); auto bytes=encoded(uri);
        Budget b(Budget::FixedLimitForTest{},1536*MiB); Result<DecodedRaster> raster;
        e.measure("decode",name,rep,w*h,w*h,bytes.size(),&b,[&] { raster=decode({{bytes.data(),bytes.size(),"image/png"},{}},b); return raster.outcome; }); ASSERT_TRUE(raster.ok());
        auto target=plainTarget(w,h); Result<FinalGrid> grid; Recipe recipe{128,40};
        e.measure("grid",name,rep,w*h,w*h,raster.value.pixels.size(),&b,[&] { grid=prepareGrid(target,raster.value,recipe,b); return grid.outcome; }); ASSERT_TRUE(grid.ok());
        JobWork work(w*h); AlphaLut lut; for(unsigned i=0;i<256;++i) lut[i]=i;
        Result<Regions> regions;
        e.measure("label",name,rep,w*h,w*h,grid.value.pixels.size(),&b,[&] { regions=label(grid.value.view(),lut,b,work); return regions.outcome; }); ASSERT_TRUE(regions.ok());
        Result<Partition> partition;
        e.measure("enclosure",name,rep,w*h,w*h,grid.value.pixels.size(),&b,[&] { partition=enclose(regions.value,b,work); return partition.outcome; }); if(!partition.ok()) { EXPECT_NE(partition.outcome.status,Status::failed); return; }
        auto metric=OrthogonalMetric::fromDpi(300,300); Result<Partition> attached;
        e.measure("specks",name,rep,w*h,w*h,grid.value.pixels.size(),&b,[&] { attached=attach(partition.value,metric.value,b,work); return attached.outcome; }); if(!attached.ok()) { EXPECT_NE(attached.outcome.status,Status::failed); return; }
        Result<EncodedPieces> png;
        e.measure("png",name,rep,w*h,w*h,grid.value.pixels.size(),&b,[&] { png=encode(grid.value,attached.value,b); return png.outcome; });
        // Stop from a separate thread during REAL phase work, not an already-canceled call.
        for (unsigned phase=0;phase<6;++phase) {
            auto flag=std::make_shared<std::atomic<bool>>(false); Stop stop(flag); std::atomic<bool> done{false};
            JobClock::time_point requested{}; bool sent=false;
            std::thread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(1+rep%3));
                if(!done.load()) { requested=JobClock::now(); sent=true; flag->store(true); } });
            auto start=JobClock::now(); Outcome result; JobWork fresh(w*h);
            switch(phase) {
                case 0: result=decode({{bytes.data(),bytes.size(),"image/png"},{}},b,stop).outcome; break;
                case 1: result=prepareGrid(target,raster.value,recipe,b,stop).outcome; break;
                case 2: result=label(grid.value.view(),lut,b,fresh,stop).outcome; break;
                case 3: result=enclose(regions.value,b,fresh,stop).outcome; break;
                case 4: result=attach(partition.value,metric.value,b,fresh,stop).outcome; break;
                default: result=encode(grid.value,attached.value,b,stop).outcome; break;
            }
            auto end=JobClock::now(); done=true; cancel.join();
            if(sent && end>=requested) e.samples.push_back({"stop_ack",name+"-phase-"+std::to_string(phase),result.diagnostic,rep,phase,w*h,bytes.size(),
                                         ns(requested,end),rss(),rss(),b.reserved(),result.status==Status::canceled});
            else e.samples.push_back(finishedBeforeStop({{},name+"-phase-"+std::to_string(phase),{},
                rep,phase,w*h,bytes.size(),ns(start,end),rss(),rss(),b.reserved(),false},result));
            e.save();
        }
    }
    void compressedCodec(Evidence &e,unsigned rep) {
        // One very small compressed input expands into 4 MP inside one loader write.
        auto gdk=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,2048,2048); ASSERT_TRUE(gdk);
        gdk_pixbuf_fill(gdk,0x4070b0c8); Pixbuf pixels(gdk);
        auto uri=sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri); auto bytes=encoded(*uri);
        auto memory=sampleMemory(); ASSERT_TRUE(memory.ok()); Budget budget(1536*MiB);
        ASSERT_TRUE(budget.recheck(memory.value).ok());
        e.measure("decode","compressible-4MP",rep,4194304,4194304,bytes.size(),&budget,[&] {
            return decode({{bytes.data(),bytes.size(),"image/png"},{}},budget).outcome;
        });
        auto flag=std::make_shared<std::atomic<bool>>(false); Stop stop(flag);
        std::atomic<bool> done{false}; JobClock::time_point requested{}; bool sent=false;
        std::thread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(!done.load()) { requested=JobClock::now(); sent=true; flag->store(true); } });
        auto result=decode({{bytes.data(),bytes.size(),"image/png"},{}},budget,stop);
        auto end=JobClock::now(); done=true; cancel.join();
        if(sent && end>=requested) e.samples.push_back({"stop_ack","compressible-4MP-decode",result.outcome.diagnostic,rep,1,4194304,bytes.size(),
            ns(requested,end),rss(),rss(),budget.reserved(),result.outcome.status==Status::canceled});
        else e.samples.push_back({"stop_not_exercised","compressible-4MP-decode","completed before request",rep,1,4194304,bytes.size(),0,rss(),rss(),budget.reserved(),false});
        e.save();
    }
    void panelClose(Evidence &e,unsigned rep,unsigned mp,bool latched=false) {
        unsigned w=mp*1000,h=1000; Pixbuf pixels(gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,w,h));
        gdk_pixbuf_fill(pixels.getPixbufRaw(),0x4070b000);
        for(unsigned i=0;i<100;++i) pixels.pixels()[i*3*4+3]=200;
        auto uri=sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
        auto doc=SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'><image id='im' width='640' height='320' href='"+*uri+"'/></svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate(); auto desktop=std::make_unique<SPDesktop>(doc->getNamedView());
        desktop->getSelection()->set(cast<SPImage>(doc->getObjectById("im"))); doc->ensureUpToDate();
        Application::instance().add_desktop(desktop.get()); Gtk::Window host;
        ExplodeBitmapPanel::Options options; options.evidence=diagnosticDependencyEvidence();
        auto panel=std::make_unique<ExplodeBitmapPanel>(options); panel->setDesktop(desktop.get()); host.set_child(*panel); host.present();
        auto until=JobClock::now()+std::chrono::seconds(2);
        while(!panel->_jobs.active() && panel->_state==ExplodeBitmapPanel::State::Counting && JobClock::now()<until) {
            panel->_jobs.poll(); Glib::MainContext::get_default()->iteration(false); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::shared_ptr<ClosingDecode> gate;
        if(latched) {
            // Native RAM admission for the decoder is retained. The job bypasses
            // only the unrelated whole-panel topology envelope, explicitly diagnostic.
            auto bytes=encoded(*uri); auto budget=std::make_shared<Budget>(1536*MiB);
            ASSERT_TRUE(budget->recheck(sampleMemory().value).ok());
            JobInput job; job.storage.budget=budget; job.pixels=std::uint64_t(w)*h; job.work=ClosingDecode::run;
            ASSERT_TRUE(budget->acquire(Stage::input,bytes.size()+4096,job.storage.reservation).ok());
            job.storage.bytes=std::move(bytes); gate=std::make_shared<ClosingDecode>(); job.storage.payload=gate;
            panel->_jobs.request(std::move(job));
            auto deadline=JobClock::now()+std::chrono::seconds(3);
            while(!gate->entered.load() && JobClock::now()<deadline) {
                panel->_jobs.poll(); Glib::MainContext::get_default()->iteration(false); std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            EXPECT_TRUE(gate->entered.load());
        }
        bool active=panel->_jobs.active(); auto status=panel->_status.get_text().raw();
        e.measure(latched?"panel_close_active_decode":"panel_close",std::to_string(mp)+"MP",rep,active?1:0,w*h,uri->size(),nullptr,[&] {
            host.unset_child(); panel.reset(); return latched && (!active || !gate->entered.load()) ? Outcome{Status::failed,"active decode close not exercised"} : Outcome{};
        });
        if(gate) gate->release=true;
        e.samples.back().diagnostic=latched ? "diagnostic panel-owned active decode; real RAM gate; latched at loader callback" :
            active ? "closed with an active worker" : "closed without active worker: "+status;
        e.save();
        drainReaper(); EXPECT_TRUE(waitForBitmapReaper(std::chrono::seconds(3))); Application::instance().remove_desktop(desktop.get());
    }
    void panel(Evidence &e,unsigned rep,bool production) {
        auto path=env("EB7_FLOWERS_PAM"); std::ifstream pam(path,std::ios::binary);
        ASSERT_TRUE(pam); std::string line; unsigned w=0,h=0;
        while(std::getline(pam,line) && line!="ENDHDR") {
            if(line.starts_with("WIDTH ")) w=std::stoul(line.substr(6));
            if(line.starts_with("HEIGHT ")) h=std::stoul(line.substr(7));
        }
        ASSERT_EQ(w,1125u); ASSERT_EQ(h,844u);
        std::vector<unsigned char> rgba(std::uint64_t(w)*h*4); pam.read(reinterpret_cast<char *>(rgba.data()),rgba.size()); ASSERT_TRUE(pam);
        auto gdk=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,w,h); ASSERT_TRUE(gdk);
        for(unsigned y=0;y<h;++y) std::memcpy(gdk_pixbuf_get_pixels(gdk)+y*gdk_pixbuf_get_rowstride(gdk),rgba.data()+y*w*4,w*4);
        Pixbuf pixels(gdk); auto uri=sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
        auto doc=SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='500' height='500'><image id='im' width='360' height='270.08' href='"+*uri+"'/></svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate(); auto desktop=std::make_unique<SPDesktop>(doc->getNamedView());
        desktop->zoom_absolute({0,0},.1); auto image=cast<SPImage>(doc->getObjectById("im")); desktop->getSelection()->set(image); doc->ensureUpToDate();
        remember(logicalImageIdentity(*image),SessionRecipe{128,40});
        Application::instance().add_desktop(desktop.get()); Gtk::Window host;
        ExplodeBitmapPanel::Options options;
        if(!production) options.evidence=diagnosticDependencyEvidence();
        auto panel=std::make_unique<ExplodeBitmapPanel>(options); panel->setDesktop(desktop.get()); host.set_child(*panel); host.present();
        auto until=JobClock::now()+std::chrono::seconds(30);
        while(panel->_state==ExplodeBitmapPanel::State::Counting && JobClock::now()<until) {
            panel->_jobs.poll(); Glib::MainContext::get_default()->iteration(false); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        e.measure(production?"production_flowers":"diagnostic_flowers", "1125x844",rep,23,949500,uri->size(),nullptr,[&] {
            if(panel->_state!=ExplodeBitmapPanel::State::Ready) return Outcome{Status::unavailable,"panel refused before ready"};
            panel->activate(Intent::Explode);
            return panel->_state==ExplodeBitmapPanel::State::Done ? Outcome{Status::changed,"production Explode admitted"} : Outcome{Status::unavailable,"panel publication refused"};
        });
        e.samples.back().diagnostic=panel->_status.get_text().raw(); e.save();
        std::cout << "flowers panel state=" << static_cast<unsigned>(panel->_state) << " " << panel->_status.get_text() << std::endl;
        e.measure("panel_close",production?"production-flowers":"diagnostic-flowers",rep,1,949500,uri->size(),nullptr,[&] { host.unset_child(); panel.reset(); return Outcome{}; });
        drainReaper(); EXPECT_TRUE(waitForBitmapReaper(std::chrono::seconds(3))); Application::instance().remove_desktop(desktop.get());
    }
};
TEST_F(ExplodeBitmapPanelTest, Calibrate) {
    auto path=std::getenv("EB7_EVIDENCE_JSON"); if(!path) GTEST_SKIP() << "Set EB7_EVIDENCE_JSON under the build lock (explicit calibration only).";
    ASSERT_TRUE(std::filesystem::is_directory(".vacards-build-lock")) << "Run from build while holding its lock";
    ASSERT_NE(env("EB7_BUILD_ID"),"missing"); Evidence e(path);
    auto scopes=env("EB7_SCOPE"); if(scopes=="missing") scopes="fixed";
    if(scopes=="fixed") {
        // No window is constructed or presented on this scope. Three serial repetitions.
        ASSERT_EQ(g_list_model_get_n_items(gtk_window_get_toplevels()),0u);
        for(unsigned n : {150u,300u}) for(unsigned rep=0;rep<3;++rep) for(bool fail : {false,true}) {
            if (env("EB_LIM2_N")!="missing" && n!=std::stoul(env("EB_LIM2_N"))) continue;
            publication(e,n,rep,fail,0,true); if(HasFatalFailure()) return;
        }
        ASSERT_EQ(g_list_model_get_n_items(gtk_window_get_toplevels()),0u);
        return;
    }
    std::istringstream scopeList(scopes); std::string scopeName;
    while(std::getline(scopeList,scopeName,',')) {
        auto allowed=",small,source,stress,backends,workers,capture,close,close-active,flowers,";
        ASSERT_NE(std::string(allowed).find(","+scopeName+","),std::string::npos) << "Unknown EB7_SCOPE: " << scopeName;
    }
    auto selected=[&](std::string const &scope) { return (","+scopes+",").find(","+scope+",")!=std::string::npos; };
    std::vector<unsigned> sizes; auto list=env("EB7_SOURCE_MP"); if(list=="missing") list="2,4,8,12";
    std::istringstream input(list); std::string value;
    while(std::getline(input,value,',')) { auto mp=std::stoul(value); ASSERT_TRUE(mp==2 || mp==4 || mp==8 || mp==12); sizes.push_back(mp); }
    ASSERT_FALSE(sizes.empty());
    for(unsigned rep=0;rep<3;++rep) {
        if(selected("small")) for(unsigned n:{2u,150u}) for(bool fail:{false,true}) { publication(e,n,rep,fail); if(HasFatalFailure()) return; }
        if(selected("capture")) for(unsigned n:{1000u,4096u,20000u,65536u}) { documentCapture(e,rep,n); if(HasFatalFailure()) return; }
        for(auto mp:sizes) {
            if(selected("source")) for(bool fail:{false,true}) { publication(e,150,rep,fail,mp); if(HasFatalFailure()) return; }
            if(selected("stress")) for(bool fail:{false,true}) { publication(e,300,rep,fail,mp); if(HasFatalFailure()) return; }
            if(selected("close") || selected("close-active")) { panelClose(e,rep,mp,selected("close-active")); if(HasFatalFailure()) return; }
            if(selected("backends")) { backends(e,rep,mp); if(HasFatalFailure()) return; }
            if(selected("workers")) { std::thread worker([&] { workers(e,rep,mp*1000,1000,std::to_string(mp)+"MP-block-noise"); }); worker.join(); if(HasFatalFailure()) return; }
        }
        if(selected("stress")) for(bool fail:{false,true}) { publication(e,300,rep,fail); if(HasFatalFailure()) return; }
        if(selected("flowers")) { panel(e,rep,false); if(HasFatalFailure()) return; }
    }
}
}
