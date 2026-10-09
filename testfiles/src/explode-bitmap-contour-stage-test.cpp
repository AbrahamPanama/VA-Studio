// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <glibmm/init.h>
#include <glib.h>
#include <png.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include "ui/explode-bitmap-panel-preparation.h"
using namespace Inkscape::Bitmap;
using namespace Inkscape::Bitmap::PanelPreparation;
namespace {
struct Image {
    unsigned w=96, h=96;
    std::vector<unsigned char> pixels = std::vector<unsigned char>(w*h*4);
    void box(unsigned x, unsigned y, unsigned ex, unsigned ey, unsigned a=255) {
        for (auto j=y;j<ey;++j) for (auto i=x;i<ex;++i) pixels[(j*w+i)*4+3]=a;
    }
};
Image two() { Image im; im.box(10,10,30,30); im.box(60,60,85,85); return im; }
Image donut() { Image im; im.box(8,8,88,88); im.box(24,24,72,72,0); im.box(40,40,56,56); return im; }
Image contourBudgetComb() {
    Image im; im.w=1122; im.h=1402; im.pixels.assign(std::uint64_t(im.w)*im.h*4,0);
    auto opaque=[&](unsigned x,unsigned y) { im.pixels[(std::uint64_t(y)*im.w+x)*4+3]=255; };
    for (unsigned y=40;y<1360;++y) for (unsigned x=24;x<29;++x) opaque(x,y);
    for (unsigned y=40;y<1360;y+=4) {
        auto row=(y-40)/4;
        auto end=250+(row*7919+row*row*17+123)%860;
        for (unsigned x=29;x<end;++x) opaque(x,y);
    }
    return im;
}
TargetSnapshot geometry(unsigned w,unsigned h) {
    TargetSnapshot t; t.bitmap=1; t.destinationParent=2; t.generation=3;
    t.supportability=Supportability::Supported;
    TargetContext own; own.identity=1; own.parent=2;
    own.pixelToItem={1,0,0,1,0,0}; own.itemToDocument={1,0,0,1,0,0};
    own.viewport={0,0,double(w),double(h)};
    TargetContext parent; parent.identity=2; parent.itemToDocument={1,0,0,1,0,0};
    t.contexts={own,parent}; return t;
}
struct Counts {
    std::array<unsigned,6> calls{};
    std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
    int cancel=-1;
    static void observe(PreparationPhase p,void *v) noexcept {
        auto &c=*static_cast<Counts *>(v); ++c.calls[unsigned(p)];
        if (int(p)==c.cancel) c.stop->store(true);
    }
    Observer observer() { return {observe,this}; }
};
// Stop is injected locally at a real stage boundary, not via a GUI/event race.
struct TestInput : JobPayload { JobInput inner; std::shared_ptr<std::atomic<bool>> stop; };
JobResult runTest(JobInput const &job,Stop,JobWork &work,JobReporter &reporter) {
    auto const &in=static_cast<TestInput const &>(*job.storage.payload);
    return in.inner.work(in.inner,Stop(in.stop),work,reporter);
}
struct Run { JobResult result; double seconds=0; std::uint64_t sampledPeak=0; };
Run execute(JobInput job, Counts &counts) {
    Glib::init(); recordBitmapMainThread();
    Run r; bool done=false; auto budget=job.storage.budget;
    auto payload=std::make_shared<TestInput>(); payload->inner=std::move(job); payload->stop=counts.stop;
    JobInput outer; outer.pixels=payload->inner.pixels; outer.work=runTest; outer.storage.payload=payload;
    BitmapJobs jobs([&](Ticket,JobResult v) { r.result=std::move(v); done=true; },{},JobClock::now,false);
    jobs.request(std::move(outer));
    auto start=JobClock::now();
    while ((!done || jobs.active()) && JobClock::now()-start<std::chrono::seconds(90)) {
        jobs.poll(); Glib::MainContext::get_default()->iteration(false);
        r.sampledPeak=std::max(r.sampledPeak,budget->reserved());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    r.seconds=std::chrono::duration<double>(JobClock::now()-start).count()-.25; // exclude mandated debounce
    EXPECT_TRUE(done); jobs.close(); drainReaper(); EXPECT_TRUE(waitForBitmapReaper(std::chrono::seconds(2)));
    return r;
}
JobInput input(Image const &im, ContourRecipe contour, Counts &counts, std::uint64_t ceiling=1536*MiB,
               AllocationFault *retainedFault=nullptr, AllocationFault *contourFault=nullptr,
               Recipe const *recipe=nullptr) {
    auto b=std::make_shared<Budget>(Budget::FixedLimitForTest{},ceiling);
    auto in=std::make_shared<Input>(); in->contour=contour; in->observer=counts.observer();
    in->target=geometry(im.w,im.h); in->recipe.bypassAlpha=true;
    if (recipe) in->recipe=*recipe;
    in->retainedGridFault=retainedFault; in->contourFault=contourFault;
    png_image png{}; png.version=PNG_IMAGE_VERSION; png.width=im.w; png.height=im.h; png.format=PNG_FORMAT_RGBA;
    png_alloc_size_t size=0;
    EXPECT_TRUE(png_image_write_to_memory(&png,nullptr,&size,0,im.pixels.data(),0,nullptr));
    std::vector<unsigned char> bytes(size);
    EXPECT_TRUE(png_image_write_to_memory(&png,bytes.data(),&size,0,im.pixels.data(),0,nullptr));
    auto encoded=g_base64_encode(bytes.data(),size);
    JobInput job; job.work=calculate; job.pixels=std::uint64_t(im.w)*im.h;
    job.storage.budget=b; job.storage.bytes.assign(encoded,encoded+std::strlen(encoded)); g_free(encoded);
    in->decodedBytes=size+4; job.storage.payload=in;
    EXPECT_TRUE(b->acquire(Stage::input,job.storage.bytes.size()+sizeof(Input)+256,job.storage.reservation).ok());
    return job;
}
std::shared_ptr<Output const> output(Run const &r) {
    EXPECT_TRUE(r.result.ok()) << r.result.outcome.diagnostic;
    return std::dynamic_pointer_cast<Output const>(r.result.value.payload);
}
Run recompute(std::shared_ptr<AnalysisState const> state, ContourRecipe recipe, Counts &counts,
              AnalysisIdentity const *expected=nullptr) {
    auto in=std::make_shared<ContourInput>(); in->analysis=std::move(state); in->contour=recipe; in->observer=counts.observer();
    in->expectedIdentity=expected ? *expected : in->analysis->identity;
    JobInput job; job.storage.budget=in->analysis->budget; job.storage.payload=in;
    job.pixels=std::uint64_t(in->analysis->grid.width)*in->analysis->grid.height; job.work=calculateContours;
    return execute(std::move(job),counts);
}
constexpr ContourRecipe enabled{true,0,0,50};
void samePieces(EncodedPieces const &a, EncodedPieces const &b) {
    ASSERT_EQ(a.count(),b.count());
    for(unsigned i=0;i<a.count();++i) {
        auto const &x=a.piece(i), &y=b.piece(i);
        EXPECT_EQ(x.x,y.x); EXPECT_EQ(x.y,y.y); EXPECT_EQ(x.width,y.width); EXPECT_EQ(x.height,y.height);
        ASSERT_EQ(x.size,y.size); EXPECT_EQ(std::memcmp(x.data,y.data,x.size),0);
    }
}
void sameBitmapProducts(Output const &baseline, Output const &refused) {
    EXPECT_EQ(refused.count,baseline.count);
    EXPECT_TRUE(refused.explodeOutcome.ok()); EXPECT_EQ(refused.explodeOutcome.status,baseline.explodeOutcome.status);
    samePieces(baseline.pieces,refused.pieces);
    EXPECT_EQ(refused.adjustmentOutcome.status,baseline.adjustmentOutcome.status);
    ASSERT_TRUE(baseline.adjustment); ASSERT_TRUE(refused.adjustment);
    EXPECT_TRUE(refused.adjustmentOutcome.ok()); EXPECT_TRUE(refused.pngStarted);
    ASSERT_EQ(refused.adjustment->image.count(),1u);
    samePieces(baseline.adjustment->image,refused.adjustment->image);
}
void sameFitted(FittedContourSet const &a, FittedContourSet const &b) {
    EXPECT_EQ(a.pieceCount,b.pieceCount); EXPECT_EQ(a.ringCount,b.ringCount);
    EXPECT_EQ(a.segmentCount,b.segmentCount); EXPECT_EQ(a.anchorCount,b.anchorCount);
    EXPECT_EQ(a.serializedBytes,b.serializedBytes);
    for (unsigned i=0;i<std::min(a.pieceCount,b.pieceCount);++i) {
        auto const &x=a.pieces()[i], &y=b.pieces()[i];
        EXPECT_EQ(x.piece,y.piece); EXPECT_EQ(x.ringBegin,y.ringBegin);
        EXPECT_EQ(x.ringEnd,y.ringEnd); EXPECT_EQ(x.noContour,y.noContour);
    }
    for (unsigned i=0;i<std::min(a.ringCount,b.ringCount);++i) {
        auto const &x=a.rings()[i], &y=b.rings()[i];
        EXPECT_EQ(x.begin,y.begin); EXPECT_EQ(x.end,y.end);
        EXPECT_EQ(x.start.x,y.start.x); EXPECT_EQ(x.start.y,y.start.y);
        EXPECT_EQ(x.area,y.area); EXPECT_EQ(x.minX,y.minX); EXPECT_EQ(x.minY,y.minY);
        EXPECT_EQ(x.maxX,y.maxX); EXPECT_EQ(x.maxY,y.maxY);
        EXPECT_EQ(x.depth,y.depth); EXPECT_EQ(x.parent,y.parent); EXPECT_EQ(x.fallback,y.fallback);
    }
    for (unsigned i=0;i<std::min(a.segmentCount,b.segmentCount);++i) {
        auto const &x=a.segments()[i], &y=b.segments()[i];
        EXPECT_EQ(x.cubic,y.cubic);
        EXPECT_EQ(x.c1.x,y.c1.x); EXPECT_EQ(x.c1.y,y.c1.y);
        EXPECT_EQ(x.c2.x,y.c2.x); EXPECT_EQ(x.c2.y,y.c2.y);
        EXPECT_EQ(x.end.x,y.end.x); EXPECT_EQ(x.end.y,y.end.y);
    }
}
}
TEST(ExplodeBitmapContourStage, DisabledMatchesPinnedCommittedOracle) {
    // Measured from committed preparation e2fb73f244 (before B4), not a second
    // run of this worker. Evidence: EB-B4/R1/baseline-test.log and scratch sources.
    // resources() and calculate() charge both Output and PreparedAlpha metadata.
    // PreparedAlpha embeds Prepared: its added contour pointer (8 bytes) and
    // ContourStyle (24-byte std::string + 8-byte double) add 40 bytes on ARM64.
    // No display stride padding: ARGB32 is exactly 96*4 bytes per row.
    // Admission peak = 8,544,164 pinned + 128 Output + 40 PreparedAlpha
    //                  + 96*96*4 display backing = 8,581,196 bytes.
    // This fixture supplies no Input::display, so live bytes gain metadata only.
    // Fixed metadata now includes owned, allocation-free OOM diagnostics.
    // Keep its growth below 4 KiB, independent of raster size.
    constexpr std::uint64_t baselineOutputSize=928, baselineBudgetPeak=8544164;
    // Pinned PreparedAlpha was 2320; current ARM64 layout is 2360 bytes.
    constexpr std::uint64_t baselinePreparedAlphaSize=2320;
    auto metadata=sizeof(Output)-baselineOutputSize+sizeof(PreparedAlpha)-baselinePreparedAlphaSize;
    constexpr std::uint64_t displayBytes=96u*96u*4u;
    ASSERT_LE(metadata,4096u);
    for (bool nested : {false,true}) for (auto recipe : {ContourRecipe{},ContourRecipe{false,9,7,99}}) {
        Counts c; auto r=execute(input(nested ? donut() : two(),recipe,c),c); auto x=output(r); ASSERT_TRUE(x);
        EXPECT_FALSE(x->analysis); EXPECT_FALSE(x->contours.product); EXPECT_EQ(x->contours.visits,0u);
        EXPECT_EQ(c.calls,(std::array<unsigned,6>{1,1,1,1,0,0}));
        EXPECT_EQ(x->count,nested ? 1u : 2u); EXPECT_EQ(x->initial,2u);
        EXPECT_EQ(x->enclosed,nested ? 1u : 0u); EXPECT_EQ(x->joined,0u); EXPECT_EQ(x->isolated,0u);
        EXPECT_EQ(x->topologyRuns,nested ? 384u : 186u);
        EXPECT_EQ(x->topologyPeak,nested ? 13360u : 7072u);
        EXPECT_TRUE(x->explodeOutcome.ok()); ASSERT_EQ(x->pieces.count(),nested ? 1u : 2u);
        std::array<std::array<std::int64_t,4>,2> rectangles = nested
            ? std::array<std::array<std::int64_t,4>,2>{{{7,7,82,82},{}}}
            : std::array<std::array<std::int64_t,4>,2>{{{9,9,22,22},{59,59,27,27}}};
        for(unsigned i=0;i<x->pieces.count();++i) {
            auto const &p=x->pieces.piece(i);
            EXPECT_EQ((std::array<std::int64_t,4>{p.x,p.y,p.width,p.height}),rectangles[i]);
        }
        // P3 (EB-P3-report.md): replace the fixed 4 MiB outline subledger with
        // an optional pre-dispatch reservation (at most 64 MiB). This input()
        // supplies no outlineReservation, so its outline charge is zero.
        // 4 MiB = 4*1024*1024 = 4194304; ARM64 metadata = 128+40 = 168.
        // Two pieces: 4309283-4194304+168 = 115147 retained bytes.
        // Nested:     4309411-4194304+168 = 115275 retained bytes.
        EXPECT_EQ(x->budget->reserved(),(nested ? 4309411u : 4309283u)-4*MiB+metadata);
        auto admitted=admit(resources(96,96,1000,0,recipe),Memory{8192*MiB,4096*MiB,256*MiB,true});
        // P3 resources() likewise removes the fixed outline term; optional
        // outlines are reserved separately. Display backing = 96*96*4 = 36864.
        // Peak = 8544164-4194304+(128+40)+36864 = 4386892 bytes.
        ASSERT_TRUE(admitted.ok()); EXPECT_EQ(admitted.value.peak,baselineBudgetPeak-4*MiB+metadata+displayBytes);
    }
}
TEST(ExplodeBitmapContourStage, DonutAndDotKeepThreeNestedRings) {
    Counts c; auto r=execute(input(donut(),enabled,c),c); auto o=output(r); ASSERT_TRUE(o);
    ASSERT_TRUE(o->contours.outcome.ok()) << o->contours.outcome.diagnostic;
    ASSERT_TRUE(o->contours.product); EXPECT_EQ(o->count,1u);
    auto const &f=o->contours.product->fitted; EXPECT_EQ(f.pieceCount,1u); ASSERT_EQ(f.ringCount,3u);
    std::array<unsigned,3> depths{}; for(unsigned i=0;i<3;++i) ++depths.at(f.rings()[i].depth);
    EXPECT_EQ(depths,(std::array<unsigned,3>{1,1,1}));
}
TEST(ExplodeBitmapContourStage, TwoPiecesHaveOneFittedSetEach) {
    Counts c; auto r=execute(input(two(),enabled,c),c); auto o=output(r); ASSERT_TRUE(o);
    ASSERT_TRUE(o->contours.outcome.ok()) << o->contours.outcome.diagnostic;
    ASSERT_TRUE(o->contours.product); auto const &f=o->contours.product->fitted;
    EXPECT_EQ(o->count,2u); ASSERT_EQ(f.pieceCount,2u); EXPECT_EQ(f.ringCount,2u);
    for(unsigned i=0;i<2;++i) { EXPECT_EQ(f.pieces()[i].piece,i); EXPECT_EQ(f.pieces()[i].ringEnd-f.pieces()[i].ringBegin,1u); }
}
TEST(ExplodeBitmapContourStage, PositiveGapGetsThreePassAllowanceAndZeroMatchesLegacy) {
    Counts analysisCounts;
    auto analyzed=execute(input(contourBudgetComb(),{true,3.6,0,0},analysisCounts),analysisCounts);
    auto prepared=output(analyzed); ASSERT_TRUE(prepared); ASSERT_TRUE(prepared->analysis);
    auto state=prepared->analysis;
    auto pixels=std::uint64_t(state->grid.width)*state->grid.height;
    auto legacyLimit=100000000u+100u*pixels;

    auto const &zero=prepared->contours;
    ASSERT_TRUE(zero.outcome.ok()) << zero.outcome.diagnostic;
    ASSERT_TRUE(zero.product);
    Budget legacyBudget(Budget::FixedLimitForTest{},1536*MiB);
    JobWork legacyWork(pixels,100);
    auto raw=offsetContours(state->grid,state->partition,{3.6,0,state->dpiX,state->dpiY},legacyBudget,legacyWork);
    ASSERT_TRUE(raw.ok()) << raw.outcome.diagnostic;
    auto fitted=fitContours(raw.value,{0},legacyBudget,legacyWork);
    ASSERT_TRUE(fitted.ok()) << fitted.outcome.diagnostic;
    EXPECT_EQ(zero.visits,legacyWork.visits());
    EXPECT_LE(zero.visits,legacyLimit);
    sameFitted(zero.product->fitted,fitted.value);

    for (auto gap : {0.1,0.5,1.0,2.0}) {
        Counts counts;
        auto run=recompute(state,{true,3.6,gap,0},counts);
        ASSERT_TRUE(run.result.ok());
        auto result=std::dynamic_pointer_cast<ContourResult const>(run.result.value.payload);
        ASSERT_TRUE(result); ASSERT_TRUE(result->outcome.ok()) << "gap=" << gap << ": " << result->outcome.diagnostic
            << " visits=" << result->visits << " legacyLimit=" << legacyLimit << " threePassLimit=" << 100000000u+300u*pixels;
        ASSERT_TRUE(result->product);
        if (gap==0.1) EXPECT_GT(result->visits,legacyLimit);
    }
}
TEST(ExplodeBitmapContourStage, RecomputeRetainsBorrowedRunsAfterOriginalRetiresAndSkipsAnalysis) {
    Counts c; auto r=execute(input(two(),enabled,c),c); auto o=output(r); ASSERT_TRUE(o); ASSERT_TRUE(o->analysis);
    auto state=o->analysis; ASSERT_TRUE(o->contours.product);
    EXPECT_EQ(state->identity,analysisIdentity(o->grid,geometry(96,96)));
    auto left=o->contours.product->fitted.rings()[0].minX;
    r.result.value={}; o.reset(); // only the immutable analysis survives
    Counts next; auto edited=recompute(state,{true,25.4/96,0,0},next);
    ASSERT_TRUE(edited.result.ok()); auto f=std::dynamic_pointer_cast<ContourResult const>(edited.result.value.payload);
    ASSERT_TRUE(f); ASSERT_TRUE(f->outcome.ok()) << f->outcome.diagnostic; ASSERT_TRUE(f->product);
    EXPECT_NEAR(f->product->fitted.rings()[0].minX,left-1,.05);
    EXPECT_EQ(next.calls,(std::array<unsigned,6>{0,0,0,0,1,1}));
    EXPECT_EQ(state->partition.runCount,state->regions.runCount);
}
TEST(ExplodeBitmapContourStage, TransparencyEditRejectsStaleAnalysisWithoutContourWork) {
    auto im=two(); im.box(10,10,30,30,128);
    Recipe recipe; recipe.bypassAlpha=false; recipe.threshold=128; recipe.softness=0;
    Counts first; auto r=execute(input(im,enabled,first,1536*MiB,nullptr,nullptr,&recipe),first);
    auto old=output(r); ASSERT_TRUE(old); ASSERT_TRUE(old->analysis);
    ASSERT_EQ(old->count,2u);
    auto state=old->analysis;
    ++recipe.threshold; // T changes; the old pixels/partition remain retained.
    Counts changed; auto newer=execute(input(im,{},changed,1536*MiB,nullptr,nullptr,&recipe),changed);
    auto current=output(newer); ASSERT_TRUE(current);
    ASSERT_EQ(current->count,1u);
    auto expected=analysisIdentity(current->grid,geometry(96,96));
    ASSERT_NE(expected.recipeHash,state->identity.recipeHash);
    Counts next; auto rejected=recompute(state,{true,.1,0,50},next,&expected);
    ASSERT_TRUE(rejected.result.ok());
    auto f=std::dynamic_pointer_cast<ContourResult const>(rejected.result.value.payload); ASSERT_TRUE(f);
    EXPECT_EQ(f->outcome.refusal,ContourRefusal::staleAnalysis); EXPECT_EQ(f->outcome.status,Status::unavailable);
    ASSERT_TRUE(f->outcome.failure); EXPECT_EQ(f->outcome.failure->stage,CliBitmapStage::Contour);
    EXPECT_EQ(f->outcome.failure->reason,CliBitmapReason::StaleCapture);
    EXPECT_FALSE(f->product); EXPECT_EQ(f->visits,0u);
    EXPECT_EQ(next.calls,(std::array<unsigned,6>{}));
    EXPECT_EQ(changed.calls[4],0u); EXPECT_EQ(changed.calls[5],0u);
}
TEST(ExplodeBitmapContourStage, CandidateAndTargetIdentityChangesRejectBeforeContourWork) {
    Counts first; auto r=execute(input(two(),enabled,first),first); auto o=output(r);
    ASSERT_TRUE(o); ASSERT_TRUE(o->analysis);
    auto original=o->analysis->identity;
    std::array<AnalysisIdentity,8> changed; changed.fill(original);
    ++changed[0].candidateIdentity; ++changed[1].target.document; ++changed[2].target.desktop;
    ++changed[3].target.bitmap; ++changed[4].target.destinationParent; ++changed[5].target.documentSerial;
    ++changed[6].target.incarnation; ++changed[7].target.generation;
    for (auto const &expected : changed) {
        Counts c; auto rejected=recompute(o->analysis,enabled,c,&expected); ASSERT_TRUE(rejected.result.ok());
        auto f=std::dynamic_pointer_cast<ContourResult const>(rejected.result.value.payload); ASSERT_TRUE(f);
        EXPECT_EQ(f->outcome.refusal,ContourRefusal::staleAnalysis); EXPECT_FALSE(f->outcome.ok());
        EXPECT_FALSE(f->product); EXPECT_EQ(f->visits,0u); EXPECT_EQ(c.calls,(std::array<unsigned,6>{}));
    }
}
TEST(ExplodeBitmapContourStage, RefusalPreservesCountAndEncodedExplode) {
    Counts c; auto r=execute(input(two(),{true,0,0,101},c),c); auto o=output(r); ASSERT_TRUE(o);
    EXPECT_FALSE(o->contours.outcome.ok()); EXPECT_FALSE(o->contours.product);
    EXPECT_EQ(o->count,2u); EXPECT_TRUE(o->explodeOutcome.ok()); EXPECT_TRUE(o->adjustmentOutcome.ok());
    EXPECT_EQ(o->pieces.count(),2u); EXPECT_TRUE(o->pngStarted); EXPECT_TRUE(o->analysis);
}
TEST(ExplodeBitmapContourStage, RetainedGridAllocationRefusalPreservesExplodeAndRealApply) {
    auto im=two(); im.box(10,10,30,30,200);
    Recipe recipe; recipe.threshold=128; recipe.softness=40;
    Counts base; auto r=execute(input(im,{},base,1536*MiB,nullptr,nullptr,&recipe),base);
    auto baseline=output(r); ASSERT_TRUE(baseline); ASSERT_EQ(baseline->count,2u);
    AllocationFault fault{1}; Counts c;
    auto failed=execute(input(im,enabled,c,1536*MiB,&fault,nullptr,&recipe),c);
    auto o=output(failed); ASSERT_TRUE(o); ASSERT_EQ(fault.attempts,1u);
    EXPECT_EQ(o->contours.outcome.status,Status::failed); EXPECT_FALSE(o->contours.product);
    EXPECT_FALSE(o->analysis); EXPECT_EQ(c.calls,(std::array<unsigned,6>{1,1,1,2,0,0}));
    sameBitmapProducts(*baseline,*o);
}
TEST(ExplodeBitmapContourStage, ContourAllocationRefusalPreservesExplodeAndRealApply) {
    auto im=two(); im.box(10,10,30,30,200);
    Recipe recipe; recipe.threshold=128; recipe.softness=40;
    Counts base; auto r=execute(input(im,{},base,1536*MiB,nullptr,nullptr,&recipe),base);
    auto baseline=output(r); ASSERT_TRUE(baseline); ASSERT_EQ(baseline->count,2u);
    AllocationFault fault{1}; Counts c;
    auto failed=execute(input(im,enabled,c,1536*MiB,nullptr,&fault,&recipe),c);
    auto o=output(failed); ASSERT_TRUE(o); ASSERT_EQ(fault.attempts,1u);
    EXPECT_EQ(o->contours.outcome.status,Status::failed); EXPECT_FALSE(o->contours.product);
    EXPECT_TRUE(o->analysis); EXPECT_EQ(c.calls,(std::array<unsigned,6>{1,1,1,2,1,0}));
    sameBitmapProducts(*baseline,*o);
}
TEST(ExplodeBitmapContourStage, StopAtOffsetAndFitIsTypedAndPublishesNothing) {
    for (auto phase : {PreparationPhase::contourOffset,PreparationPhase::contourFit}) {
        Counts c; c.cancel=int(phase); auto r=execute(input(two(),enabled,c),c);
        EXPECT_EQ(r.result.outcome.status,Status::canceled); EXPECT_FALSE(r.result.value.payload);
    }
}
TEST(ExplodeBitmapContourStage, AdmissionIncludesContourPeakAndRetainedAnalysis) {
    Memory memory{8192*MiB,4096*MiB,256*MiB,true};
    auto base=admit(resources(96,96,1000,0),memory,nullptr,{128*MiB,0}); ASSERT_TRUE(base.ok());
    auto with=admit(resources(96,96,1000,0,enabled),memory,nullptr,{128*MiB,0});
    EXPECT_FALSE(with.ok()); EXPECT_GT(with.value.peak,base.value.peak+256*MiB);
    EXPECT_FALSE(admit(contourResources(32*MiB),memory,nullptr,{256*MiB,0}).ok());
    EXPECT_TRUE(admit(resources(5000,5000,MiB,0,enabled),memory).ok());
}
TEST(ExplodeBitmapContourStage, Measure5000OptIn) {
    auto root=std::getenv("INKSCAPE_EB_B4_MEASURE");
    if (!root || !*root) GTEST_SKIP() << "Set INKSCAPE_EB_B4_MEASURE to limits-5000 fixture directory.";
    for(auto name : {"blobs-150-5000x5000.png","sparse/blobs-150-5000x5000.png"}) {
        auto path=std::filesystem::path(root)/name;
        auto file=path.string(); // Windows path::c_str() is wchar_t; libpng takes a narrow path.
        png_image png{}; png.version=PNG_IMAGE_VERSION; ASSERT_TRUE(png_image_begin_read_from_file(&png,file.c_str()));
        png.format=PNG_FORMAT_RGBA; Image im; im.w=png.width; im.h=png.height; im.pixels.resize(PNG_IMAGE_SIZE(png));
        ASSERT_TRUE(png_image_finish_read(&png,nullptr,im.pixels.data(),0,nullptr)); png_image_free(&png);
        Counts c; auto r=execute(input(im,{true,0,.5,50},c),c); auto o=output(r); ASSERT_TRUE(o);
        ASSERT_TRUE(o->contours.outcome.ok()) << o->contours.outcome.diagnostic; ASSERT_TRUE(o->contours.product);
        auto print=[&](char const *kind,double total,ContourResult const &f,std::uint64_t peak) {
            std::cout << "B4_MEASURE," << name << ',' << kind << ',' << total << ',' << f.seconds << ','
                      << std::max(peak,f.peakBudget) << ',' << f.product->fitted.ringCount << ','
                      << f.product->fitted.anchorCount << '\n';
        };
        print("full",r.seconds,o->contours,r.sampledPeak);
        Counts next; auto rr=recompute(o->analysis,{true,.1,.5,50},next);
        ASSERT_TRUE(rr.result.ok()); auto f=std::dynamic_pointer_cast<ContourResult const>(rr.result.value.payload);
        ASSERT_TRUE(f); ASSERT_TRUE(f->outcome.ok()) << f->outcome.diagnostic; ASSERT_TRUE(f->product);
        EXPECT_EQ(next.calls,(std::array<unsigned,6>{0,0,0,0,1,1}));
        print("contour-only",rr.seconds,*f,rr.sampledPeak);
    }
}
