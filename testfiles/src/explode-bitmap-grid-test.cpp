// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <random>
#include <chrono>
#include "object/sp-clippath.h"
#include "object/sp-shape.h"
#include "object/sp-item-group.h"
#include "xml/document.h"
#include "xml/node.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <2geom/transforms.h>
#include "ui/explode-bitmap-grid.h"
#include "ui/tools/destructive-bitmap-coverage.h"
#include "util/bitmap-islands.h"
#include "display/cairo-utils.h"
#include "display/nr-filter.h"
#include "svg/svg.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "display/drawing.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "bitmap-adjustment-chemistry.h"
using namespace Inkscape;
using namespace Inkscape::Bitmap;
namespace {
TargetSnapshot target(unsigned w, unsigned h, GridTransform transform = {1,0,0,1,0,0})
{
    TargetSnapshot s; s.bitmap = 1; s.destinationParent = 2; s.generation = 3;
    s.supportability = Supportability::Supported;
    TargetContext own; own.identity = 1; own.parent = 2; own.itemToDocument = transform;
    own.pixelToItem = {1,0,0,1,0,0}; own.viewport = {0,0,double(w),double(h)};
    TargetContext parent; parent.identity = 2; parent.itemToDocument = {1,0,0,1,0,0};
    s.contexts = {own, parent}; return s;
}
DecodedRaster raster(Budget &b, unsigned w, unsigned h)
{
    DecodedRaster r; r.width = w; r.height = h;
    EXPECT_TRUE(r.pixels.allocate(b, Stage::decode, std::uint64_t(w)*h, 4).ok());
    auto p = reinterpret_cast<unsigned char *>(r.pixels.data());
    for (unsigned i = 0; i < w*h; ++i) { p[4*i] = i%256; p[4*i+1] = (i*37)%256; p[4*i+2] = 200; p[4*i+3] = 255; }
    return r;
}
Geom::Affine affine(GridTransform const &a) { return {a[0],a[1],a[2],a[3],a[4],a[5]}; }
}
TEST(ExplodeBitmapGrid, T20ExactTransformsSerializeAndParentCompensates)
{
    Budget b(Budget::FixedLimitForTest{}, 16*MiB); auto r = raster(b, 5, 3);
    for (GridTransform a : {GridTransform{1.25,0,0,2.75,1e6+.123456789,-1e6+.987654321},
                           GridTransform{-2,0,0,3,-10.3,8.7}, GridTransform{0,2,-3,0,6,7},
                           GridTransform{0,-2,3,0,-6,7}, GridTransform{-2,0,0,-3,6,-7},
                           GridTransform{1e-8,0,0,1e-8,0,0}}) {
        auto s = target(5,3,a); s.contexts[1].itemToDocument = {2,.3,-.2,1.5,100,-200};
        auto result = prepareGrid(s,r,{},b); ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
        auto &g = result.value; EXPECT_EQ(g.sampling, GridSampling::Exact);
        EXPECT_EQ(g.width,5); EXPECT_EQ(g.height,3); EXPECT_EQ(std::memcmp(g.pixels.data(),r.pixels.data(),r.pixels.size()),0);
        auto restored = affine(g.pixelToParent)*affine(s.contexts[1].itemToDocument);
        for (Geom::Point p : {Geom::Point(0,0),Geom::Point(5,3),Geom::Point(2,1)})
            EXPECT_LT(Geom::distance(p*restored,p*affine(a)),1e-6);
        char xml[256]; ASSERT_GT(serializeGridTransform(g.pixelToParent,xml,sizeof(xml)),0);
        Geom::Affine reopened; ASSERT_TRUE(sp_svg_transform_read(xml,&reopened));
        for (unsigned i=0;i<6;++i) EXPECT_EQ(reopened[i],g.pixelToParent[i]);
        // Neighboring integer crop endpoints map from one matrix, with no mm rounding.
        auto end = Geom::Point(2,1)*reopened, start = Geom::Point(2,1)*affine(g.pixelToParent);
        EXPECT_EQ(end,start); EXPECT_DOUBLE_EQ(g.dpiX,96/std::hypot(a[0],a[1]));
    }
}
TEST(ExplodeBitmapGrid, T20InvalidGeometryRefusesBeforeAllocation)
{
    Budget b(Budget::FixedLimitForTest{}, MiB); auto r=raster(b,5,3); auto live=b.reserved();
    for (GridTransform a : {GridTransform{0,0,0,1,0,0},GridTransform{1,1,1,1,0,0},
                           GridTransform{1e-7,0,0,1,0,0},GridTransform{1,0,0,1,1e7,0},
                           GridTransform{NAN,0,0,1,0,0},GridTransform{1,0,0,INFINITY,0,0}}) {
        auto result=prepareGrid(target(5,3,a),r,{},b); EXPECT_FALSE(result.ok()); EXPECT_EQ(b.reserved(),live);
    }
    auto s=target(5,3); s.contexts[0].viewport[2]=4.999; EXPECT_FALSE(prepareGrid(s,r,{},b).ok());
    s.contexts[0].viewport[2]=-1; EXPECT_FALSE(prepareGrid(s,r,{},b).ok());
    s=target(5,3); s.contexts[0].pixelToItem[0]=0; EXPECT_FALSE(prepareGrid(s,r,{},b).ok());
    char tiny[8]; EXPECT_EQ(serializeGridTransform({1,0,0,1,0,0},tiny,sizeof(tiny)),0);
}
TEST(ExplodeBitmapGrid, T20MeetNoneAndAllAlignmentOffsets)
{
    Budget b(Budget::FixedLimitForTest{},MiB); auto r=raster(b,3,2);
    for (double x : {0.,2.,4.}) for (double y : {0.,3.,6.}) {
        auto s=target(3,2); s.contexts[0].pixelToItem={2.5,0,0,1.5,x,y}; s.contexts[0].viewport={0,0,12,9};
        auto g=prepareGrid(s,r,{},b); ASSERT_TRUE(g.ok()); EXPECT_EQ(g.value.sampling,GridSampling::Exact);
        EXPECT_EQ(g.value.pixelToDocument,s.contexts[0].pixelToItem);
    }
    EXPECT_TRUE(prepareGrid(target(3,2,{1e-6,0,0,1,0,0}),r,{},b).ok());
    EXPECT_FALSE(prepareGrid(target(3,2,{.999e-6,0,0,1,0,0}),r,{},b).ok());
}
TEST(ExplodeBitmapGrid, ImportedDecimalAspectSnappingIsNotCropping) {
    Budget b(Budget::FixedLimitForTest{}, 32*MiB); auto r = raster(b, 1536, 1024);
    // Default 96 dpi import into an mm document, serialized to ten digits.
    double w = 406.4, h = 270.9333333;
    double scale = (w/1536 + h/1024)/2; // SPViewBox's near-uniform averaging
    auto s = target(1536, 1024);
    s.contexts[0].pixelToItem = {scale,0,0,scale,-60.79017417,-17.39375};
    s.contexts[0].viewport = {-60.79017417,-17.39375,w,h};
    EXPECT_GT(1024*scale-h, 1e-9); // The old absolute tolerance really fails.
    auto result = prepareGrid(s,r,{},b); ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
    EXPECT_EQ(result.value.sampling, GridSampling::Exact);
    EXPECT_EQ(result.value.width,1536u); EXPECT_EQ(result.value.height,1024u);
    EXPECT_EQ(result.value.pixelToDocument,s.contexts[0].pixelToItem);
    s.contexts[0].viewport[3] -= 0.01; EXPECT_FALSE(prepareGrid(s,r,{},b).ok());
}
TEST(ExplodeBitmapGrid, T21StraightenDensityFiltersAndNoSilentDownsample)
{
    Budget b(Budget::FixedLimitForTest{},16*MiB); auto r=raster(b,5,3);
    double q=std::sqrt(.5);
    for (auto a : {GridTransform{q,q,-q,q,7.25,-3.5},GridTransform{1,.5,.25,1,7.25,-3.5}}) {
        auto s=target(5,3,a); auto nearest=prepareGrid(s,r,{.pixelated=true},b); auto good=prepareGrid(s,r,{},b);
        ASSERT_TRUE(nearest.ok()); ASSERT_TRUE(good.ok()); EXPECT_EQ(nearest.value.sampling,GridSampling::Nearest);
        EXPECT_EQ(good.value.sampling,GridSampling::Good); EXPECT_EQ(good.value.pixelToDocument[1],0);
        EXPECT_EQ(good.value.pixelToDocument[2],0); EXPECT_GT(good.value.width*good.value.height,15);
        auto region=Geom::Rect::from_xywh(0,0,5,3)*affine(a);
        EXPECT_NEAR(good.value.pixelToDocument[4],region.left(),1e-12);
        EXPECT_NEAR(good.value.pixelToDocument[5],region.top(),1e-12);
        auto dpi=96*std::max(1/std::hypot(a[0],a[1]),1/std::hypot(a[2],a[3]));
        EXPECT_GE(good.value.dpiX,dpi-1e-10); EXPECT_GE(good.value.dpiY,dpi-1e-10);
        EXPECT_NE(nearest.value.recipeHash,good.value.recipeHash);
    }
    auto expanded=target(5,3,{1,0,1,1e-8,0,0});
    EXPECT_FALSE(prepareGrid(expanded,r,{},b).ok());
    EXPECT_FALSE(UI::Tools::DestructiveBitmapCoverage::gridGeometry(Geom::Affine(1,.5,.5,1,0,0),16384,16384));
}
TEST(ExplodeBitmapGrid, T25AlphaClipOpacityOrderRawRgbAndProfile)
{
    Budget b(Budget::FixedLimitForTest{},16*MiB); auto r=raster(b,256,1); auto s=target(256,1);
    auto p=reinterpret_cast<unsigned char *>(r.pixels.data()); for(unsigned i=0;i<256;++i)p[4*i+3]=i;
    ASSERT_TRUE(r.profile.allocate(b,Stage::decode,128,1).ok()); r.profileBytes=128;
    std::memset(r.profile.data(),42,128); s.contexts[0].opacity=.5; s.retainedAncestorOpacity=.4;
    GridCoverage clip; ASSERT_TRUE(clip.pixels.allocate(b,Stage::composition,256,1).ok());
    std::memset(clip.pixels.data(),127,256); clip.width=256;clip.height=1;clip.bitmap=1;clip.generation=3;s.contexts[0].clip=true;
    Recipe recipe{.threshold=80,.softness=20,.coverage=&clip};
    auto g=prepareGrid(s,r,recipe,b); ASSERT_TRUE(g.ok()) << g.outcome.diagnostic; auto lut=alphaLut(80,20);
    auto out=reinterpret_cast<unsigned char const *>(g.value.pixels.data());
    for(unsigned i=0;i<256;++i) {
        unsigned a=(unsigned(lut[i])*127+127)/255; a=(a*128+127)/255;
        EXPECT_EQ(out[4*i+3],a); for(unsigned c=0;c<3;++c)EXPECT_EQ(out[4*i+c],a?p[4*i+c]:0);
    }
    EXPECT_EQ(std::memcmp(g.value.profile.data(),r.profile.data(),128),0);
    auto hash=g.value.recipeHash; recipe.bypassAlpha=true; g=prepareGrid(s,r,recipe,b); ASSERT_TRUE(g.ok());
    EXPECT_NE(g.value.recipeHash,hash); EXPECT_EQ(reinterpret_cast<unsigned char const *>(g.value.pixels.data())[4*100+3],25);
    clip.generation=4; EXPECT_FALSE(prepareGrid(s,r,recipe,b).ok());
}
TEST(ExplodeBitmapGrid, BudgetFaultCancelAndSharedWorkBalance)
{
    Budget b(Budget::FixedLimitForTest{},MiB); auto r=raster(b,5,3);ASSERT_TRUE(r.profile.allocate(b,Stage::decode,16,1).ok());r.profileBytes=16;
    auto baseline=b.reserved();auto s=target(5,3);
    for(unsigned k=1;k<=2;++k) { AllocationFault f{k}; Recipe recipe{.fault=&f};
        auto g=prepareGrid(s,r,recipe,b); EXPECT_FALSE(g.ok()); EXPECT_EQ(b.reserved(),baseline); }
    auto flag=std::make_shared<std::atomic<bool>>(true); EXPECT_EQ(prepareGrid(s,r,{},b,Stop(flag)).outcome.status,Status::canceled);
    EXPECT_EQ(b.reserved(),baseline);
    JobWork work(15); ASSERT_TRUE(work.advance(work.limit()).ok()); Recipe recipe{.work=&work};
    EXPECT_FALSE(prepareGrid(s,r,recipe,b).ok()); EXPECT_EQ(b.reserved(),baseline);
    Budget small(Budget::FixedLimitForTest{},59); EXPECT_FALSE(prepareGrid(s,r,{},small).ok()); EXPECT_EQ(small.reserved(),0);

}
TEST(ExplodeBitmapGrid, T25CanvasToneByteParityAndMainOwnClip)
{
    if(!Application::exists()) Application::create(false);
    auto rawFixture=gdk_pixbuf_new(GDK_COLORSPACE_RGB,TRUE,8,1,1);ASSERT_TRUE(rawFixture);
    gdk_pixbuf_fill(rawFixture,0xff0000ff);Pixbuf fixture(rawFixture);
    auto uri=sp_image_encode_png_data_uri(fixture);ASSERT_TRUE(uri);
    auto svg=Glib::ustring::compose(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="1">
    <defs><clipPath id="clip"><rect width="2" height="1"/></clipPath></defs>
    <g id="parent" opacity=".4"><image id="image" width="4" height="1" preserveAspectRatio="none" style="image-rendering:pixelated"
    href="%1"/></g></svg>)svg",*uri);
    auto doc=SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(doc);doc->ensureUpToDate();auto image=cast<SPImage>(doc->getObjectById("image"));ASSERT_TRUE(image);
    Budget b(Budget::FixedLimitForTest{},16*MiB); auto r=raster(b,1,1);
    auto s=target(1,1);s.bitmap=reinterpret_cast<std::uintptr_t>(image);s.contexts[0].identity=s.bitmap;
    auto captured=captureGridCoverage(*image,s,b);ASSERT_TRUE(captured.ok())<<captured.outcome.diagnostic;
    EXPECT_EQ(std::to_integer<unsigned>(captured.value.pixels.data()[0]),255); // ancestor stays live
    auto root=doc->getRoot(); Drawing drawing;auto key=SPItem::display_key_new(1);
    drawing.setRoot(root->invoke_show(drawing,key,SP_ITEM_SHOW_DISPLAY)); drawing.update();
    struct HideView {SPRoot *root;unsigned key;~HideView(){root->invoke_hide(key);}} shown{root,key};
    auto surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,4,1);
    { DrawingSurface dest(surface,Geom::IntPoint(0,0));DrawingContext dc(dest);
      drawing.render(dc,Geom::IntRect::from_xywh(0,0,4,1));cairo_surface_flush(surface);
      std::uint32_t baseline;std::memcpy(&baseline,cairo_image_surface_get_data(surface),4);
      ASSERT_GT(baseline>>24,0u) << "Baseline native image must be visible before tone parity."; }
    for(double brightness : {-100.,-35.,0.,35.,100.}) {
        Filters::BitmapToneSettings tone;tone.brightness=brightness;tone.contrast=25;tone.shadows=-20;
        s.contexts[0].tone=tone;s.contexts[0].viewport={0,0,1,1};
        auto raw=reinterpret_cast<unsigned char *>(r.pixels.data());raw[0]=255;raw[1]=0;raw[2]=0;raw[3]=255;
        auto g=prepareGrid(s,r,{},b);ASSERT_TRUE(g.ok())<<g.outcome.diagnostic;
        image->get_arenaitem(key)->setFilterRenderer(BitmapAdjustments::build_tone_preview_renderer(image,image->get_arenaitem(key),tone));
        auto cr=cairo_create(surface);cairo_set_operator(cr,CAIRO_OPERATOR_CLEAR);cairo_paint(cr);cairo_destroy(cr);
        drawing.update(); DrawingSurface dest(surface,Geom::IntPoint(0,0));DrawingContext dc(dest);
        drawing.render(dc,Geom::IntRect::from_xywh(0,0,4,1));cairo_surface_flush(surface);
        std::uint32_t canvas;std::memcpy(&canvas,cairo_image_surface_get_data(surface),4);
        auto pixel=reinterpret_cast<unsigned char const *>(g.value.pixels.data());
        EXPECT_NEAR((canvas>>16)&255,premul_alpha(pixel[0],102),1);
        EXPECT_NEAR((canvas>>8)&255,premul_alpha(pixel[1],102),1);EXPECT_NEAR(canvas&255,premul_alpha(pixel[2],102),1);
    }
    cairo_surface_destroy(surface);
    image->getRepr()->setAttribute("clip-path","url(#clip)"); image->getRepr()->setAttribute("opacity",".5");doc->ensureUpToDate();
    captured=captureGridCoverage(*image,s,b);ASSERT_TRUE(captured.ok()); // source pixel clip antialias only; own opacity omitted
    EXPECT_NEAR(std::to_integer<unsigned>(captured.value.pixels.data()[0]),128,1);
    Status worker=Status::unchanged;std::thread t([&]{worker=captureGridCoverage(*image,s,b).outcome.status;});t.join();
    EXPECT_EQ(worker,Status::unavailable);
}

TEST(ExplodeBitmapGrid, T21BarcodeAndOnePixelGapsMatchWholeSurfaceCairo)
{
    Budget b(Budget::FixedLimitForTest{},16*MiB); auto r=raster(b,31,5);
    auto p=reinterpret_cast<unsigned char *>(r.pixels.data());
    for(unsigned y=0;y<5;++y) for(unsigned x=0;x<31;++x) p[4*(y*31+x)+3]=x%3==1?0:255;
    double q=std::sqrt(.5);
    for(auto a : {GridTransform{q,q,-q,q,-1.2,3.7},GridTransform{1,.25,.5,1,-1.2,3.7}})
    for(bool pixelated : {false,true}) {
        auto s=target(31,5,a); Recipe recipe{.pixelated=pixelated};
        auto g=prepareGrid(s,r,recipe,b);ASSERT_TRUE(g.ok())<<g.outcome.diagnostic;
        auto &grid=g.value;
        auto src=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,31,5);
        auto raw=cairo_image_surface_get_data(src);
        for(unsigned i=0;i<155;++i) {unsigned alpha=p[4*i+3];std::uint32_t v=(alpha<<24)|
            (premul_alpha(p[4*i],alpha)<<16)|(premul_alpha(p[4*i+1],alpha)<<8)|premul_alpha(p[4*i+2],alpha);
            std::memcpy(raw+4*i,&v,4);}
        cairo_surface_mark_dirty(src);
        auto dest=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,grid.width,grid.height);
        auto pattern=cairo_pattern_create_for_surface(src);auto inverse=(affine(a)*affine(grid.pixelToDocument).inverse()).inverse();
        cairo_matrix_t matrix{inverse[0],inverse[1],inverse[2],inverse[3],inverse[4],inverse[5]};
        cairo_pattern_set_matrix(pattern,&matrix);cairo_pattern_set_extend(pattern,CAIRO_EXTEND_NONE);
        cairo_pattern_set_filter(pattern,pixelated?CAIRO_FILTER_NEAREST:CAIRO_FILTER_GOOD);
        auto cr=cairo_create(dest);cairo_set_operator(cr,CAIRO_OPERATOR_SOURCE);cairo_set_source(cr,pattern);cairo_paint(cr);
        ASSERT_EQ(cairo_status(cr),CAIRO_STATUS_SUCCESS);cairo_surface_flush(dest);
        auto output=reinterpret_cast<unsigned char const *>(grid.pixels.data());auto expected=cairo_image_surface_get_data(dest);
        unsigned visible=0;
        for(unsigned y=0;y<grid.height;++y)for(unsigned x=0;x<grid.width;++x) {
            std::uint32_t v;std::memcpy(&v,expected+y*cairo_image_surface_get_stride(dest)+4*x,4);
            auto out=output+4*(y*grid.width+x);EXPECT_EQ(out[3],v>>24);visible+=out[3]>0;
            EXPECT_NEAR(premul_alpha(out[0],out[3]),(v>>16)&255,1);
            EXPECT_NEAR(premul_alpha(out[1],out[3]),(v>>8)&255,1);EXPECT_NEAR(premul_alpha(out[2],out[3]),v&255,1);
        }
        EXPECT_GT(visible,0);cairo_destroy(cr);cairo_pattern_destroy(pattern);cairo_surface_destroy(dest);cairo_surface_destroy(src);
    }
}
TEST(ExplodeBitmapGrid, T25AllRawToneBinsAndLowAlphaRgb)
{
    Budget b(Budget::FixedLimitForTest{},MiB);auto r=raster(b,256,1);auto s=target(256,1);
    auto raw=reinterpret_cast<unsigned char *>(r.pixels.data());
    for(double brightness : {-100.,0.,100.}) {
        s.contexts[0].tone={brightness,35,-10,100,-100,50};
        auto table=Filters::build_bitmap_tone_table(s.contexts[0].tone);
        auto g=prepareGrid(s,r,{},b);ASSERT_TRUE(g.ok());auto pixels=reinterpret_cast<unsigned char const *>(g.value.pixels.data());
        for(unsigned i=0;i<256;++i)for(unsigned c=0;c<3;++c)
            EXPECT_EQ(pixels[4*i+c],std::round(table[raw[4*i+c]]*255));
        for(unsigned i=0;i<256;++i)raw[4*i+3]=1;
        Recipe bypass{.faintFloor=0,.bypassAlpha=true};g=prepareGrid(s,r,bypass,b);ASSERT_TRUE(g.ok());
        pixels=reinterpret_cast<unsigned char const *>(g.value.pixels.data());
        for(unsigned i=0;i<256;++i)for(unsigned c=0;c<3;++c)
            EXPECT_EQ(pixels[4*i+c],std::round(table[raw[4*i+c]]*255));
        for(unsigned i=0;i<256;++i)raw[4*i+3]=255;
    }
}

TEST(ExplodeBitmapGrid, T21ExpandedDestinationAndMidBakeCancelBalance)
{
    Budget tight(Budget::FixedLimitForTest{},192*1024);auto r=raster(tight,128,128);auto baseline=tight.reserved();
    auto s=target(128,128,{1,.5,.5,1,0,0});
    auto g=prepareGrid(s,r,{},tight);EXPECT_FALSE(g.ok());EXPECT_EQ(tight.reserved(),baseline);
    Budget b(Budget::FixedLimitForTest{},16*MiB);auto rr=raster(b,512,512);auto live=b.reserved();
    for(unsigned k=1;k<=3;++k) {AllocationFault fault{k};Recipe recipe{.fault=&fault};
        auto failed=prepareGrid(target(512,512,{1,.5,.5,1,0,0}),rr,recipe,b);
        EXPECT_FALSE(failed.ok());EXPECT_EQ(b.reserved(),live);}
    for (auto phase : {GridPhase::Bake, GridPhase::Straighten}) for (unsigned failAt : {0u,1u}) {
        struct Barrier {
            GridPhase phase;
            std::mutex mutex; std::condition_variable changed;
            bool reached=false, resume=false, done=false, timedOut=false;
            Outcome outcome;
        } barrier{phase};
        AllocationFault fault{failAt}; Recipe recipe{.fault=&fault};
        recipe.checkpointData=&barrier;
        recipe.checkpoint=[](GridPhase p, void *data) {
            auto &b=*static_cast<Barrier *>(data);
            if (p!=b.phase) return;
            std::unique_lock lock(b.mutex); b.reached=true; b.changed.notify_all();
            b.timedOut=!b.changed.wait_for(lock,std::chrono::seconds(5),[&]{return b.resume;});
        };
        auto flag=std::make_shared<std::atomic<bool>>(false);
        std::thread worker([&]{
            auto outcome=prepareGrid(target(512,512,{1,.5,.5,1,0,0}),rr,recipe,b,Stop(flag)).outcome;
            std::lock_guard lock(barrier.mutex); barrier.outcome=outcome;
            barrier.done=true; barrier.changed.notify_all();
        });
        bool signaled;
        { std::unique_lock lock(barrier.mutex);
          signaled=barrier.changed.wait_for(lock,std::chrono::seconds(5),[&]{return barrier.reached||barrier.done;});
          flag->store(true); barrier.resume=true; barrier.changed.notify_all(); }
        worker.join();
        EXPECT_TRUE(signaled) << "Checkpoint wait expired: " << barrier.outcome.diagnostic;
        EXPECT_FALSE(barrier.timedOut) << "Checkpoint resume expired: " << barrier.outcome.diagnostic;
        EXPECT_EQ(barrier.reached,failAt==0) << "Early return: " << barrier.outcome.diagnostic;
        EXPECT_EQ(barrier.outcome.status,failAt ? Status::failed : Status::canceled)
            << barrier.outcome.diagnostic;
        EXPECT_EQ(b.reserved(),live);
    }
}

namespace {
std::unique_ptr<SPDocument> clipDocument(unsigned w, unsigned h, std::string const &children)
{
    if (!Application::exists()) Application::create(false);
    auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,TRUE,8,w,h);
    gdk_pixbuf_fill(raw,0xff0000ff); Pixbuf fixture(raw);
    auto uri=sp_image_encode_png_data_uri(fixture);
    auto svg="<svg xmlns=\"http://www.w3.org/2000/svg\"><defs><clipPath id=\"clip\">"+children+
        "</clipPath></defs><image id=\"image\" width=\""+std::to_string(w)+"\" height=\""+std::to_string(h)+
        "\" clip-path=\"url(#clip)\" href=\""+*uri+"\"/></svg>";
    auto doc=SPDocument::createNewDocFromMem(svg); doc->ensureUpToDate(); return doc;
}
TargetSnapshot clipTarget(SPImage &image)
{
    auto s=target(image.pixbuf->width(),image.pixbuf->height());
    s.bitmap=reinterpret_cast<std::uintptr_t>(&image); s.contexts[0].identity=s.bitmap; return s;
}
std::string rectangles(unsigned n)
{
    std::string s;
    while(n--) s+="<rect width=\"512\" height=\"512\" style=\"stroke-dasharray:1,2,3,4\"/>";
    return s;
}
}
TEST(ExplodeBitmapGrid, ClipMaterializationFaultCleansNativeViews)
{
    auto doc=clipDocument(1,1,rectangles(1)); auto image=cast<SPImage>(doc->getObjectById("image"));
    auto clip=image->getClipObject(); auto first=cast<SPItem>(clip->firstChild());
    struct FaultShape : SPShape {
        SPItem *previous; bool entered=false, cleanupFault=false;
        DrawingItem *show(Drawing &, unsigned, unsigned) override {
            entered=true; EXPECT_EQ(previous->views.size(),1u); throw std::bad_alloc();
        }
        void hide(unsigned) override { cleanupFault=true; throw std::bad_alloc(); }
    };
    auto fault=new FaultShape; fault->previous=first;
    auto repr=doc->getReprDoc()->createElement("svg:path");
    clip->attach(fault,clip->lastChild()); fault->invoke_build(doc.get(),repr,1); sp_object_unref(fault,nullptr);
    Budget b(Budget::FixedLimitForTest{},16*MiB);
    auto captured=captureGridCoverage(*image,clipTarget(*image),b);
    EXPECT_EQ(captured.outcome.status,Status::failed); EXPECT_TRUE(fault->entered); EXPECT_TRUE(fault->cleanupFault);
    EXPECT_TRUE(first->views.empty()); EXPECT_TRUE(fault->views.empty()); EXPECT_EQ(b.reserved(),0u);
    clip->detach(fault); GC::release(repr);
    // The same resource must safely show again, update and tear down after failure.
    doc->ensureUpToDate(); captured=captureGridCoverage(*image,clipTarget(*image),b);
    ASSERT_TRUE(captured.ok())<<captured.outcome.diagnostic; EXPECT_TRUE(first->views.empty());
}
TEST(ExplodeBitmapGrid, ClipCleanupFaultUnlinksNestedNativeViews)
{
    auto doc=clipDocument(256,256,"<g><g><rect width=\"256\" height=\"256\"/></g></g>");
    auto image=cast<SPImage>(doc->getObjectById("image")); auto clip=image->getClipObject();
    struct FaultGroup : SPGroup {
        bool fault=true; unsigned failures=0;
        void hide(unsigned key) override {
            if (fault) { ++failures; throw std::bad_alloc(); }
            SPGroup::hide(key);
        }
    };
    auto group=new FaultGroup;
    auto repr=doc->getReprDoc()->createElement("svg:g");
    clip->attach(group,nullptr); group->invoke_build(doc.get(),repr,1); sp_object_unref(group,nullptr);
    Drawing liveDrawing; auto liveKey=SPItem::display_key_new(1);
    liveDrawing.setRoot(doc->getRoot()->invoke_show(liveDrawing,liveKey,SP_ITEM_SHOW_DISPLAY));
    auto liveClipKey=image->get_arenaitem(liveKey)->key()+ITEM_KEY_CLIP;
    // Inject at the first cleanup child while all nested native views still exist.
    Budget b(Budget::FixedLimitForTest{},64*MiB);
    auto captured=captureGridCoverage(*image,clipTarget(*image),b);
    EXPECT_EQ(captured.outcome.status,Status::failed); EXPECT_EQ(group->failures,1u);
    EXPECT_EQ(b.reserved(),0u);
    auto clean=[&](auto &&self, SPObject &object)->void {
        if(auto item=cast<SPItem>(&object)) {
            EXPECT_EQ(item->views.size(),1u); EXPECT_NE(item->get_arenaitem(liveClipKey),nullptr);
        }
        for(auto &child:object.children) self(self,child);
    };
    clean(clean,*clip); EXPECT_EQ(clip->children.size(),2u);
    group->fault=false; doc->ensureUpToDate();
    captured=captureGridCoverage(*image,clipTarget(*image),b);
    ASSERT_TRUE(captured.ok())<<captured.outcome.diagnostic;
    EXPECT_EQ(std::to_integer<unsigned>(captured.value.pixels.data()[0]),255u);
    clean(clean,*clip); doc->getRoot()->invoke_hide(liveKey); GC::release(repr);
}
TEST(ExplodeBitmapGrid, TenNestedGroupsChargeEveryNativeSurface)
{
    namespace C=UI::Tools::DestructiveBitmapCoverage;
    std::string children="<rect width=\"512\" height=\"512\"/>";
    for(unsigned i=0;i<10;++i) children="<g>"+children+"</g>";
    for(unsigned size : {128u,256u,512u}) {
        auto doc=clipDocument(size,size,children); auto image=cast<SPImage>(doc->getObjectById("image"));
        C::ClipComplexity c; ASSERT_EQ(C::estimateGridClip(image->getClipObject(),size,size,c),nullptr);
        EXPECT_EQ(c.groups,10u); Budget b(Budget::FixedLimitForTest{},64*MiB);
        auto captured=captureGridCoverage(*image,clipTarget(*image),b);
        if(c.units<=C::ClipMainUnitLimit) {
            ASSERT_TRUE(captured.ok())<<captured.outcome.diagnostic;
            for(std::size_t i=0;i<captured.value.pixels.size();++i)
                ASSERT_EQ(std::to_integer<unsigned>(captured.value.pixels.data()[i]),255u)<<i;
            captured.value={};
        } else EXPECT_STREQ(captured.outcome.diagnostic,"Own clip main-thread complexity limit (100 ms).");
        EXPECT_EQ(b.reserved(),0u);
    }
}
TEST(ExplodeBitmapGrid, ClipTreeBudgetAndMainComplexityRefuseBeforeShow)
{
    auto doc=clipDocument(1,1,rectangles(400)); auto image=cast<SPImage>(doc->getObjectById("image"));
    Budget tiny(Budget::FixedLimitForTest{},MiB);
    auto captured=captureGridCoverage(*image,clipTarget(*image),tiny);
    EXPECT_STREQ(captured.outcome.diagnostic,"Own clip tree materialization exceeds memory budget.");
    EXPECT_EQ(tiny.reserved(),0u);
    Budget b(Budget::FixedLimitForTest{},64*MiB); captured=captureGridCoverage(*image,clipTarget(*image),b);
    EXPECT_STREQ(captured.outcome.diagnostic,"Own clip main-thread complexity limit (100 ms).");
    EXPECT_EQ(b.reserved(),0u);
    for(auto &child:image->getClipObject()->children) EXPECT_TRUE(cast<SPItem>(&child)->views.empty());
    // Raster area also participates even for one simple clip child.
    doc=clipDocument(4096,4096,rectangles(1)); image=cast<SPImage>(doc->getObjectById("image"));
    captured=captureGridCoverage(*image,clipTarget(*image),b);
    EXPECT_STREQ(captured.outcome.diagnostic,"Own clip main-thread complexity limit (100 ms).");
    EXPECT_EQ(b.reserved(),0u);
}
TEST(ExplodeBitmapGrid, OwnClipTinyUniformScaleAccepted)
{
    auto doc=clipDocument(1,1,"<rect width=\"1\" height=\"1\"/>");
    auto image=cast<SPImage>(doc->getObjectById("image"));
    image->getRepr()->setAttribute("transform","scale(1e-10)"); doc->ensureUpToDate();
    Budget b(Budget::FixedLimitForTest{},16*MiB);
    auto g=captureGridCoverage(*image,clipTarget(*image),b);
    ASSERT_TRUE(g.ok())<<g.outcome.diagnostic; EXPECT_EQ(std::to_integer<unsigned>(g.value.pixels.data()[0]),255u);
}
TEST(ExplodeBitmapGrid, OppositeRotationsFingerprintResampling)
{
    Budget b(Budget::FixedLimitForTest{},MiB); auto r=raster(b,2,2); double q=std::sqrt(.5);
    auto plus=prepareGrid(target(2,2,{q,q,-q,q,2*q,0}),r,{.pixelated=true},b);
    auto minus=prepareGrid(target(2,2,{q,-q,q,q,0,2*q}),r,{.pixelated=true},b);
    ASSERT_TRUE(plus.ok()); ASSERT_TRUE(minus.ok());
    EXPECT_EQ(plus.value.pixelToDocument,minus.value.pixelToDocument);
    EXPECT_NE(std::memcmp(plus.value.pixels.data(),minus.value.pixels.data(),plus.value.pixels.size()),0);
    EXPECT_NE(plus.value.recipeHash,minus.value.recipeHash);
}
TEST(ExplodeBitmapGrid, ClipAdmissionCalibrationOnThisHost)
{
    namespace C=UI::Tools::DestructiveBitmapCoverage;
    // Deterministic crossing cubics with even-odd fill; no repeated identical curves.
    auto curves=[](unsigned n,unsigned seed, bool wide=false) {
        std::mt19937 rng(seed); auto xy=[&]{return std::to_string(int(rng()%(wide?2049:513))-(wide?768:0));};
        std::string path="<path clip-rule=\"evenodd\" d=\"M0,0";
        for(unsigned i=0;i<n;++i) {
            for(auto command : {" C"," "," "}) path+=command+xy()+","+xy();
            path+=" L"+xy()+","+xy();
        }
        return path+" Z\"/>";
    };
    auto manySubpaths=[](unsigned n) { std::string subpaths="<path clip-rule=\"evenodd\" d=\"";
    for(unsigned i=0;i<n;++i) {
        auto x=std::to_string((i*17)%500), y=std::to_string((i*31)%500);
        subpaths+="M"+x+","+y+" c12,-10 -12,20 8,8 l-8,0 z ";
    }
    return subpaths+"\"/>"; };
    std::string nested="<rect width=\"512\" height=\"512\"/>";
    for(unsigned i=0;i<10;++i) nested="<g>"+nested+"</g>";
    // Oversize probes use legacy evaluate only in this bounded calibration test.
    // Production capture must refuse them before show; admitted captures include
    // census, ledger, show/update/render, copy and cleanup in their timing.
    for(auto fixture : {std::tuple{"rects",64u,rectangles(170)}, {"nested",128u,nested}, {"nested",256u,nested}, {"nested",512u,nested},
                        {"subpaths",512u,manySubpaths(400)}, {"crossing-17",512u,curves(800,17)},
                        {"crossing-89",512u,curves(800,89)}, {"crossing-wide",512u,curves(800,17,true)}, {"crossing-small",512u,curves(200,17)},
                        {"crossing-small-89",512u,curves(200,89)}, {"subpaths-small",512u,manySubpaths(50)}, {"subpaths-admitted",128u,manySubpaths(50)},
                        {"crossing-edge",512u,curves(20,17,true)}, {"crossing-dense-admitted",64u,curves(140,89,true)}}) {
        auto [name,size,children]=fixture; auto doc=clipDocument(size,size,children);
        auto image=cast<SPImage>(doc->getObjectById("image")); C::ClipComplexity c;
        ASSERT_EQ(C::estimateGridClip(image->getClipObject(),size,size,c),nullptr);
        bool admitted=c.units<=C::ClipMainUnitLimit; Budget b(Budget::FixedLimitForTest{},64*MiB);
        std::int64_t worst=0;
        for(unsigned run=0;run<5;++run) {
            auto start=std::chrono::steady_clock::now();
            if(admitted) {
                auto captured=captureGridCoverage(*image,clipTarget(*image),b);
                ASSERT_TRUE(captured.ok())<<captured.outcome.diagnostic;
            } else { auto measured=C::evaluate(*image,*image); ASSERT_TRUE(measured.completed()); }
            auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            worst=std::max(worst,ns); EXPECT_EQ(b.reserved(),0u);
        }
        if(!admitted) {
            auto refused=captureGridCoverage(*image,clipTarget(*image),b);
            EXPECT_STREQ(refused.outcome.diagnostic,"Own clip main-thread complexity limit (100 ms).");
            for(auto &child:image->getClipObject()->children) EXPECT_TRUE(cast<SPItem>(&child)->views.empty());
        }
        std::cout<<"CLIP_CALIBRATION fixture="<<name<<" size="<<size<<" units="<<c.units
                 <<" groups="<<c.groups<<" tree_bytes="<<c.bytes<<" admitted="<<admitted
                 <<" worst_ns="<<worst<<" ns_per_unit="<<double(worst)/c.units<<std::endl;
    }
}

TEST(ExplodeBitmapGrid, FaintFloorAllPercentAndAlphaBinsWithAndWithoutRefinement)
{
    Budget b(Budget::FixedLimitForTest{}, MiB); auto r = raster(b, 256, 1); auto s = target(256, 1);
    auto raw = reinterpret_cast<unsigned char *>(r.pixels.data());
    for (unsigned a = 0; a < 256; ++a) raw[4*a+3] = a;
    EXPECT_EQ(Recipe{}.faintFloor, 5u);
    // A low T/S deliberately lets refinement produce alpha BELOW the input floor.
    // The floor is applied to source alpha once, before refinement.
    auto legacy = alphaLut(8, 10);
    for (bool off : {false, true}) for (unsigned floor = 0; floor <= 25; ++floor) {
        Recipe recipe{.threshold=8, .softness=10, .faintFloor=floor, .bypassAlpha=off};
        auto grid = prepareGrid(s, r, recipe, b); ASSERT_TRUE(grid.ok());
        for (unsigned a = 0; a < 256; ++a) {
            auto expected = a <= floor*255/100 ? 0 : off ? a : legacy[a];
            ASSERT_EQ(grid.value.view().data[4*a+3], expected) << floor << ":" << off << ":" << a;
        }
    }
    Recipe recipe{.bypassAlpha=true};
    auto grid = prepareGrid(s, r, recipe, b); ASSERT_TRUE(grid.ok());
    EXPECT_EQ(grid.value.view().data[4*12+3], 0); // 5% cutoff is floor(12.75).
    EXPECT_EQ(grid.value.view().data[4*13+3], 13);
    EXPECT_EQ(grid.value.view().data[4*15+3], 15); // 6% represented in 8-bit alpha survives unchanged.
    recipe.faintFloor = 26; EXPECT_FALSE(prepareGrid(s, r, recipe, b).ok());
    // Recipe identity changes even when the image bytes do not.
    raw[3] = 255;
    auto one = raster(b, 1, 1); auto t = target(1, 1);
    auto a = prepareGrid(t, one, {.faintFloor=0}, b);
    auto c = prepareGrid(t, one, {.faintFloor=5}, b);
    ASSERT_TRUE(a.ok()); ASSERT_TRUE(c.ok());
    EXPECT_EQ(std::memcmp(a.value.pixels.data(), c.value.pixels.data(), 4), 0);
    EXPECT_NE(a.value.recipeHash, c.value.recipeHash);
}
