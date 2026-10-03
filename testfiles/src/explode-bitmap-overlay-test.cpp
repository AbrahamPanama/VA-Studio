// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <bit>
#include <iostream>
#include <set>
#include <tuple>
#include "desktop.h"
#include "document.h"
#include "display/control/canvas-item.h"
#include "display/control/canvas-item-context.h"
#include "display/control/canvas-item-drawing.h"
#include "display/control/canvas-item-ptr.h"
#include "display/drawing.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "selection.h"
#include "object/sp-image.h"
#include "object/sp-namedview.h"
#include "ui/explode-bitmap-overlay.h"
#include "xml/node.h"
#include "xml/repr.h"
using namespace Inkscape;
using namespace Inkscape::Bitmap;
namespace {
using Clock = std::chrono::steady_clock;
using Edge = std::tuple<unsigned, unsigned, unsigned, unsigned>;
struct Fixture {
    Budget budget{Budget::FixedLimitForTest{}, 512 * MiB};
    std::vector<std::uint8_t> pixels;
    JobWork work;
    Result<Regions> regions;
    Result<Partition> partition;
    Result<ExactOutlines> result;
    double outlineBuildMs = 0;
    Fixture(unsigned w, unsigned h, std::vector<unsigned> const &visible, GridTransform m = {1,0,0,1,0,0})
        : pixels(w*h*4), work(w*h) {
        for (auto at : visible) pixels[at*4+3] = 255;
        regions = label({pixels.data(), pixels.size(), w*4ULL, w, h}, alphaLut(0,0), budget, work);
        if (!regions.ok()) return;
        partition = enclose(regions.value, budget, work);
        if (!partition.ok()) return;
        FinalGrid grid; grid.width = w; grid.height = h; grid.pixelToDocument = m;
        auto start=Clock::now();
        result = prepareOutlines(partition.value, grid, budget, work);
        outlineBuildMs=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    }
};
class Overlay : public testing::Test {
protected:
    void SetUp() override {
        static auto app = [] { g_setenv("INKSCAPE_APP_ID_TAG", "eb6overlay", true); return new InkscapeApplication; }();
        ASSERT_TRUE(app->gtk_app()); if (!Application::exists()) Application::create(false);
        document = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'><image id='im' width='10' height='10' href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=='/></svg>");
        document->ensureUpToDate(); desktop = std::make_unique<SPDesktop>(document->getNamedView());
        desktop->getSelection()->set(cast<SPItem>(document->getObjectById("im")));
        PlatformEvidence e; e.platform = EvidencePlatform::Mac; e.observationNs = 20000;
        lease = std::make_unique<DependencyLease>(*desktop, e);
    }
    ExactOutlines outlines(Fixture &f, Generation generation, std::uint64_t cap = outlineSegmentLimit) {
        EXPECT_TRUE(f.result.ok()) << f.result.outcome.diagnostic;
        document->ensureUpToDate(); auto target = resolve(*desktop, Intent::Explode); EXPECT_TRUE(target.ok()) << target.outcome.diagnostic;
        auto r = f.result.value; r.dependencies = capture(target.value); EXPECT_TRUE(bool(r.dependencies));
        r.generation = generation; r.limits.outlineUnits = cap; return r;
    }
    std::vector<unsigned char> render(int scale = 1, int size = 64, Clock::duration *elapsed = nullptr) {
        auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, size*scale, size*scale);
        auto cr = Cairo::Context::create(surface); cr->scale(scale, scale);
        CanvasItemBuffer b{Geom::IntRect(0,0,size,size), scale, cr, false};
        auto start=Clock::now();
        if (auto item = overlay.canvasItem()) { item->update(false); item->render(b); }
        if (elapsed) *elapsed=Clock::now()-start;
        surface->flush(); auto p = surface->get_data();
        return {p, p + surface->get_stride()*surface->get_height()};
    }
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<DependencyLease> lease;
    OverlayTestHooks hooks;
    BitmapOverlay overlay{&hooks}; // releases canvas before desktop and fixture storage
};
TEST(Outlines, T22ExhaustiveSmallForegroundPerimeterOracle) {
    for (unsigned mask = 0; mask < 512; ++mask) {
        std::vector<unsigned> visible; std::set<Edge> expected, actual;
        auto occupied = [&](int x, int y) { return x>=0 && x<3 && y>=0 && y<3 && (mask & (1u<<(y*3+x))); };
        for (unsigned y=0; y<3; ++y) for (unsigned x=0; x<3; ++x) if (occupied(x,y)) {
            visible.push_back(y*3+x);
            if (!occupied(x-1,y)) expected.emplace(x,y,x,y+1);
            if (!occupied(x+1,y)) expected.emplace(x+1,y,x+1,y+1);
            if (!occupied(x,y-1)) expected.emplace(x,y,x+1,y);
            if (!occupied(x,y+1)) expected.emplace(x,y+1,x+1,y+1);
        }
        Fixture f(3,3,visible); ASSERT_TRUE(f.result.ok()) << mask << f.result.outcome.diagnostic;
        auto &s = *f.result.value.storage;
        for (std::uint64_t i=0; i<s.count; ++i) {
            auto a=s.data()[i]; EXPECT_LT(a.piece, s.pieceCount);
            for (unsigned x=a.x,y=a.y; x<a.endX || y<a.endY; x+=(a.x<a.endX),y+=(a.y<a.endY))
                EXPECT_TRUE(actual.emplace(x,y,x+(a.x<a.endX),y+(a.y<a.endY)).second) << mask;
        }
        EXPECT_EQ(actual,expected) << mask; // holes stay transparent; no duplicate/internal seams
    }
}
TEST_F(Overlay, T22ActualEdgesOutsidePageAndNoCropBoxOrGutter) {
    Fixture f(5,5,{0,1,5,24},{0,2,-3,0,40,-10});
    ASSERT_TRUE(f.result.ok()); EXPECT_EQ(f.result.value.storage->count, 11u);
    EXPECT_EQ(f.result.value.storage->pixelToDocument, (GridTransform{0,2,-3,0,40,-10}));
    ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,1),1).ok());
    auto item=overlay.canvasItem(); ASSERT_TRUE(item); EXPECT_FALSE(item->is_pickable()); item->update(false);
    ASSERT_TRUE(item->get_bounds()); EXPECT_LT(item->get_bounds()->min().y(),0); // never page-clamped
    auto before=sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_EQ(overlay.zoomToPiece(1,1).status,Status::changed);
    EXPECT_EQ(overlay.zoomToPiece(2,1).status,Status::incompatible);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before); overlay.clear();
}
TEST_F(Overlay, T22AccentHaloSolidAndDevicePixelWidths) {
    Fixture f(20,20,{0,1,20,399},{1,0,0,1,8,8});
    ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,1),1).ok());
    for (int scale : {1,2}) {
        auto bytes=render(scale); auto alpha=[&](int x,int y) { return bytes[(y*64*scale+x)*4+3]; };
        EXPECT_GT(alpha(8*scale,8*scale),0); EXPECT_EQ(alpha(16*scale,16*scale),0);
        EXPECT_EQ(alpha(8*scale-3,8*scale),0); // halo stays in device-pixel width
        bool accent=false,halo=false;
        for (std::size_t i=0;i<bytes.size();i+=4) {
            accent |= bytes[i]>bytes[i+2] && bytes[i+1]>bytes[i+2];
            halo |= bytes[i+3]>0 && bytes[i]==0 && bytes[i+1]==0 && bytes[i+2]==0;
        }
        EXPECT_TRUE(accent); EXPECT_TRUE(halo);
    }
    overlay.clear();
}
TEST_F(Overlay, P3GlobalLatencyCapDoesNotDiscardExactGeometry) {
    Fixture f(3,1,{0,2}); auto o=outlines(f,1,0);
    ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    EXPECT_TRUE(overlay.installOutlines(*desktop,o,1).ok()); EXPECT_TRUE(overlay.ready());
    ASSERT_TRUE(overlay.beginGeneration(*desktop,2).ok()); o=outlines(f,2); o.approximate=true;
    EXPECT_EQ(overlay.installOutlines(*desktop,o,2).status,Status::unavailable);
    ASSERT_TRUE(overlay.beginGeneration(*desktop,3).ok()); o=outlines(f,3);
    auto fake=std::make_shared<OutlineStorage>(); fake->count=outlineSegmentLimit+1; fake->pieceCount=2; o.storage=fake;
    EXPECT_EQ(overlay.installOutlines(*desktop,o,3).status,Status::unavailable); EXPECT_FALSE(overlay.ready());
}
TEST_F(Overlay, T29TenThousandTicketsOnlyCurrentGenerationReady) {
    Fixture f(3,1,{0,2}); auto first=outlines(f,1);
    for (Generation i=1;i<=10000;++i) ASSERT_TRUE(overlay.beginGeneration(*desktop,i).ok());
    EXPECT_EQ(overlay.installOutlines(*desktop,first,1).status,Status::canceled);
    EXPECT_FALSE(overlay.ready()); auto last=outlines(f,10000);
    ASSERT_TRUE(overlay.installOutlines(*desktop,last,10000).ok()); EXPECT_TRUE(overlay.ready());
    EXPECT_EQ(overlay.installOutlines(*desktop,first,1).status,Status::canceled); EXPECT_TRUE(overlay.ready());
    auto onlyItem=overlay.canvasItem();
    for (Generation i=10001;i<=11000;++i) {
        ASSERT_TRUE(overlay.beginGeneration(*desktop,i).ok()); auto current=outlines(f,i);
        ASSERT_TRUE(overlay.installOutlines(*desktop,current,i).ok()); EXPECT_EQ(overlay.canvasItem(),onlyItem);
    }
    overlay.clear(); EXPECT_FALSE(overlay.ready()); EXPECT_EQ(overlay.canvasItem(),nullptr);
    EXPECT_EQ(overlay.installOutlines(*desktop,last,10000).status,Status::canceled);
}
TEST_F(Overlay, T29ChangeRevertSelectionCloseAndSingleOwner) {
    Fixture f(3,1,{0,2}); ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    auto o=outlines(f,1); ASSERT_TRUE(overlay.installOutlines(*desktop,o,1).ok());
    BitmapOverlay second; EXPECT_EQ(second.beginGeneration(*desktop,1).status,Status::unavailable);
    auto node=document->getObjectById("im")->getRepr(); node->setAttribute("opacity","0.5"); node->setAttribute("opacity",nullptr);
    EXPECT_EQ(overlay.canvasItem(),nullptr); EXPECT_FALSE(overlay.ready());
    ASSERT_TRUE(overlay.beginGeneration(*desktop,2).ok()); o=outlines(f,2);
    ASSERT_TRUE(overlay.installOutlines(*desktop,o,2).ok()); desktop->getSelection()->clear(); EXPECT_FALSE(overlay.ready());
    desktop->getSelection()->set(cast<SPItem>(document->getObjectById("im")));
    ASSERT_TRUE(overlay.beginGeneration(*desktop,3).ok()); o=outlines(f,3);
    ASSERT_TRUE(overlay.installOutlines(*desktop,o,3).ok()); desktop.reset(); EXPECT_EQ(overlay.canvasItem(),nullptr);
    EXPECT_EQ(overlay.zoomToPiece(0,3).status,Status::canceled); document.reset();
}
class SnapshotProbe final : public CanvasItem {
public:
    using CanvasItem::CanvasItem;
    CanvasItemContext &context() { return *_context; }
private:
    void _update(bool) override {}
    void _render(CanvasItemBuffer &) const override {}
};
TEST_F(Overlay, P3R2EachDenseInstallationPublishesCurrentVisibility) {
    std::vector<unsigned> cells;
    for(unsigned y=0;y<1000;++y) for(unsigned x=5;x<1000;x+=10) cells.push_back(y*1000+x);
    Fixture f(1000,1000,cells,{.0001,0,0,.0001,0,0}); ASSERT_TRUE(f.result.ok());
    unsigned notifications=0;
    auto connection=overlay.connectVisibility([&](OutlineVisibility state) {
        EXPECT_EQ(state,OutlineVisibility::tooDense); ++notifications;
    });
    for(Generation generation:{1,2}) {
        ASSERT_TRUE(overlay.beginGeneration(*desktop,generation).ok());
        ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,generation),generation).ok());
        auto item=overlay.canvasItem(); ASSERT_TRUE(item); item->update(false);
        EXPECT_EQ(overlay.camera().visibility,OutlineVisibility::tooDense);
        EXPECT_EQ(notifications,generation);
        item->update(false); EXPECT_EQ(notifications,generation); // ordinary unchanged camera is quiet
        overlay.clear();
    }
    connection.disconnect();
}
TEST_F(Overlay, P3R2ClearRemovesCallbackBeforeDeferredRetirementAndInstallRestoresIt) {
    Fixture f(3,1,{0,2}); auto probe=make_canvasitem<SnapshotProbe>(desktop->getCanvasTemp());
    ASSERT_FALSE(overlay.cameraCallbackRegistered());
    ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,1),1).ok());
    auto item=overlay.canvasItem(); ASSERT_TRUE(item); item->update(false);
    EXPECT_TRUE(overlay.cameraCallbackRegistered());
    probe->context().snapshot(); overlay.clear();
    EXPECT_FALSE(overlay.cameraCallbackRegistered()); // item is still retained, with a frozen reader
    probe->context().unsnapshot();
    ASSERT_TRUE(overlay.beginGeneration(*desktop,2).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,2),2).ok());
    EXPECT_EQ(overlay.canvasItem(),item); EXPECT_TRUE(overlay.cameraCallbackRegistered());
    overlay.clear(); overlay.clear(); EXPECT_FALSE(overlay.cameraCallbackRegistered());
}
TEST_F(Overlay, P3OldExtentLengthAffineAndScaleGatesDoNotRetire) {
    std::vector<unsigned> cells;
    for (unsigned y=0;y<3000;++y) cells.push_back(y*3);
    Fixture f(3,3000,cells); ASSERT_TRUE(f.result.ok());
    ASSERT_GT(f.result.value.storage->count,5000u);
    auto probe=make_canvasitem<SnapshotProbe>(desktop->getCanvasTemp());
    ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,1,0),1).ok());
    // The reflected/sheared strip's interior crosses the 64 x 64 render buffer:
    // (0,1488) -> (10.7,56.7), (0,1512) -> (17.9,8.7); determinant = -4.12.
    for (auto m : {Geom::Affine(10,0,0,10,8.3,8.7), Geom::Affine(2,0.4,0.3,-2,-435.7,3032.7)}) {
        probe->context().setAffine(m); overlay.canvasItem()->update(true);
        for (int scale : {1,2,3}) {
            auto bytes=render(scale); EXPECT_TRUE(overlay.ready());
            EXPECT_TRUE(std::any_of(bytes.begin(),bytes.end(),[](auto c){return c!=0;}));
        }
    }
    probe->context().setAffine(Geom::identity()); overlay.clear();
}
TEST(Outlines, P3TileIndexMatchesIndependentMaskWithHolesGapsLabyrinthAndSpecks) {
    constexpr unsigned w=260,h=190;
    std::vector<unsigned> cells; std::set<Edge> expected, actual;
    auto occupied=[](int x,int y) {
        if (x<0 || y<0 || x>=int(w) || y>=int(h)) return false;
        return (x>=10 && x<150 && y>=10 && y<140 && !(x>20 && x<140 && y>20 && y<130)) ||
               (x>=151 && x<240 && y>=10 && y<140 && (y%4==0 || x==151 || (x==239 && y%8<4))) ||
               (x==259 && y==189);
    };
    for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x) if (occupied(x,y)) {
        cells.push_back(y*w+x);
        if (!occupied(x-1,y)) expected.emplace(x,y,x,y+1);
        if (!occupied(x+1,y)) expected.emplace(x+1,y,x+1,y+1);
        if (!occupied(x,y-1)) expected.emplace(x,y,x+1,y);
        if (!occupied(x,y+1)) expected.emplace(x,y+1,x+1,y+1);
    }
    Fixture f(w,h,cells); ASSERT_TRUE(f.result.ok()); auto &data=*f.result.value.storage;
    std::uint64_t count=0;
    for (unsigned ty=0;ty<data.rows;++ty) for(unsigned tx=0;tx<data.columns;++tx) {
        auto tile=data.index()[ty*data.columns+tx]; EXPECT_EQ(tile.begin,count); count+=tile.count;
        for (auto i=tile.begin;i<tile.begin+tile.count;++i) {
            auto a=data.data()[i]; EXPECT_EQ(a.x/64,tx); EXPECT_EQ(a.y/64,ty);
            EXPECT_LE(a.endX,(tx+1)*64); EXPECT_LE(a.endY,(ty+1)*64);
            for (unsigned x=a.x,y=a.y;x<a.endX || y<a.endY;x+=(a.x<a.endX),y+=(a.y<a.endY))
                ASSERT_TRUE(actual.emplace(x,y,x+(a.x<a.endX),y+(a.y<a.endY)).second);
        }
    }
    EXPECT_EQ(count,data.count); EXPECT_EQ(actual,expected);
}
TEST(Outlines, P3IndexedTiledRendersMatchUnlimitedReference) {
    std::vector<unsigned> cells;
    for(unsigned y=0;y<180;++y) for(unsigned x=0;x<260;++x)
        if (y<2 || y>177 || x<2 || x>257 || (x==63 && y<130) || (y==64 && x<200)) cells.push_back(y*260+x);
    Fixture f(260,180,cells); ASSERT_TRUE(f.result.ok());
    auto const &data=*f.result.value.storage;
    for(auto m:{Geom::Affine(.73,0,0,.81,-33.25,-20.7), Geom::Affine(-.73,0,0,.81,170.25,-20.7),
                Geom::Affine(.7,.3,-.3,.7,-12.25,-18.7), Geom::Affine(-.7,.2,.4,.8,150.3,-30.2)})
    for(int scale:{1,2}) {
        auto whole=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,192*scale,160*scale);
        auto cr=Cairo::Context::create(whole); cr->scale(scale,scale);
        CanvasItemBuffer buffer{Geom::IntRect(0,0,192,160),scale,cr,false};
        // Independent unlimited drawing visits every segment, without tile culling.
        bool aligned=m[1]==0 && m[2]==0;
        if(aligned) {
            // Independent analytical union of square-capped cell edges, with
            // the same documented 8-bit subpixel quantization as the renderer.
            auto mask=[&](double half) {
                auto surf=Cairo::ImageSurface::create(Cairo::Surface::Format::A8,192*scale,160*scale);
                auto pixels=surf->get_data(); auto stride=surf->get_stride();
                for (std::uint64_t i=0;i<data.count;++i) {
                    auto e=data.data()[i]; auto a=(Geom::Point(e.x,e.y)*m)*scale,b=(Geom::Point(e.endX,e.endY)*m)*scale;
                    auto quant=[](double v){return std::floor(v*256+.5)/256;};
                    double l=quant(std::min(a.x(),b.x())-half),r=quant(std::max(a.x(),b.x())+half);
                    double t=quant(std::min(a.y(),b.y())-half),bt=quant(std::max(a.y(),b.y())+half);
                    for(int y=std::max(0,int(std::floor(t)));y<std::min(160*scale,int(std::ceil(bt)));++y)
                    for(int x=std::max(0,int(std::floor(l)));x<std::min(192*scale,int(std::ceil(r)));++x) {
                        auto c=std::lround(255*std::max(0.,std::min(x+1.,r)-std::max(double(x),l))*
                                             std::max(0.,std::min(y+1.,bt)-std::max(double(y),t)));
                        pixels[y*stride+x]=std::max(pixels[y*stride+x],static_cast<unsigned char>(c));
                    }
                }
                surf->mark_dirty(); cairo_surface_set_device_scale(surf->cobj(),scale,scale); return surf;
            };
            cr->set_source_rgba(0,0,0,.55); cr->mask(mask(1.5),0,0);
            cr->set_source_rgba(0,.85,1,1); cr->mask(mask(.5),0,0);
        } else {
            // Unlimited independent oracle: point-in-square-stroke using
            // along/perpendicular distances at every global coverage sample.
            // No tile lookup, polygon/scanline intersections, or production renderer.
            for(double half:{1.5,.5}) {
                using Bits=std::array<std::uint64_t,4>;
                std::vector<Bits> covered(192*160*scale*scale);
                for(std::uint64_t i=0;i<data.count;++i) {
                    auto e=data.data()[i]; auto a=(Geom::Point(e.x,e.y)*m)*scale,b=(Geom::Point(e.endX,e.endY)*m)*scale;
                    double dx=b.x()-a.x(),dy=b.y()-a.y(),length=std::hypot(dx,dy); dx/=length;dy/=length;
                    double margin=2*half;
                    int x0=std::max(0,int(std::floor(std::min(a.x(),b.x())-margin)));
                    int x1=std::min(192*scale,int(std::ceil(std::max(a.x(),b.x())+margin)));
                    int y0=std::max(0,int(std::floor(std::min(a.y(),b.y())-margin)));
                    int y1=std::min(160*scale,int(std::ceil(std::max(a.y(),b.y())+margin)));
                    for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)for(int sy=0;sy<16;++sy)for(int sx=0;sx<16;++sx) {
                        double px=x+(sx+.5)/16-a.x(),py=y+(sy+.5)/16-a.y();
                        double along=px*dx+py*dy, across=-px*dy+py*dx;
                        if(along>=-half && along<length+half && across>=-half && across<half)
                            covered[y*192*scale+x][sy/4] |= std::uint64_t(1)<<(16*(sy%4)+sx);
                    }
                }
                auto mask=Cairo::ImageSurface::create(Cairo::Surface::Format::A8,192*scale,160*scale);
                for(int y=0;y<160*scale;++y)for(int x=0;x<192*scale;++x) {
                    unsigned count=0;for(auto word:covered[y*192*scale+x])count+=std::popcount(word);
                    mask->get_data()[y*mask->get_stride()+x]=(count*255+128)/256;
                }
                mask->mark_dirty();cairo_surface_set_device_scale(mask->cobj(),scale,scale);
                if(half==1.5)cr->set_source_rgba(0,0,0,.55);else cr->set_source_rgba(0,.85,1,1);
                cr->mask(mask,0,0);
            }
        }
        whole->flush();
        auto production=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,192*scale,160*scale);
        auto pc=Cairo::Context::create(production); pc->scale(scale,scale);
        CanvasItemBuffer pb{Geom::IntRect(0,0,192,160),scale,pc,false};
        ASSERT_TRUE(renderOutlinePreview(f.result.value,pb,m).ok()); production->flush();
        for(int i=0;i<production->get_stride()*production->get_height();++i)
            ASSERT_EQ(production->get_data()[i],whole->get_data()[i]) << "whole ref m=" << m[1] << " byte=" << i;
        for(int y=0;y<160;y+=32) for(int x=0;x<192;x+=32) {
            auto tile=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,32*scale,32*scale);
            auto tc=Cairo::Context::create(tile); tc->scale(scale,scale);
            CanvasItemBuffer b{Geom::IntRect(x,y,x+32,y+32),scale,tc,false};
            ASSERT_TRUE(renderOutlinePreview(f.result.value,b,m).ok()); tile->flush();
            for(int row=0;row<32*scale;++row) for(int col=0;col<32*scale*4;++col)
                ASSERT_EQ(tile->get_data()[row*tile->get_stride()+col],
                          whole->get_data()[(y*scale+row)*whole->get_stride()+x*scale*4+col])
                    << "m=" << m[0] << "," << m[1] << "," << m[2] << "," << m[3] << " scale=" << scale << " tile=" << x << ',' << y << " pixel=" << col/4 << ',' << row;
        }
    }
}
TEST(Outlines, P3VisibleBudgetBoundaryAndOffscreenTiles) {
    Budget budget{Budget::FixedLimitForTest{},MiB}; OutlineStorage storage;
    storage.columns=2; storage.rows=1;
    ASSERT_TRUE(storage.tiles.allocate(budget,Stage::prepared,2,sizeof(OutlineTile)).ok());
    auto tiles=reinterpret_cast<OutlineTile *>(storage.tiles.data());
    tiles[0]={0,75000}; tiles[1]={75000,75000};
    auto both=Geom::Rect(0,0,128,64);
    EXPECT_EQ(outlineCamera(storage,Geom::identity(),both).visibility,OutlineVisibility::exact);
    ++tiles[1].count;
    EXPECT_EQ(outlineCamera(storage,Geom::identity(),both).visibility,OutlineVisibility::tooDense);
    auto one=outlineCamera(storage,Geom::identity(),Geom::Rect(0,0,32,32));
    EXPECT_EQ(one.visibility,OutlineVisibility::exact); EXPECT_EQ(one.visibleSegments,75000u);
}
TEST(Outlines, P3DenseSparse5000MeasurementsAndRecoveryWithoutAnalysis) {
    for(bool dense:{false,true}) {
        std::vector<unsigned> cells;
        if(dense) {
            for(unsigned y=0;y<5000;++y) for(unsigned i=0;i<150;++i) cells.push_back(y*5000+16+33*i);
        } else {
            for(unsigned y=100;y<400;++y) for(unsigned x=100;x<4900;++x) cells.push_back(y*5000+x);
            cells.push_back(4999*5000+4999);
        }
        Fixture f(5000,5000,cells); ASSERT_TRUE(f.result.ok()) << f.result.outcome.diagnostic;
        auto outlines=f.result.value; auto storage=outlines.storage;
        auto visits=f.work.visits();
        std::cout << "P3 fixture=" << (dense?"dense":"sparse") << " build_ms=" << f.outlineBuildMs
                  << " index_bytes=" << storage->bytes() << " segments=" << storage->count << '\n';
        for(int scale:{1,2}) for(double zoom:{.12,4.,16.,.12}) {
            Geom::Affine m(zoom,0,0,zoom,zoom==.12?100.:-100*zoom+.25,zoom==.12?0.:-100*zoom+.75);
            auto camera=outlineCamera(*storage,m,Geom::Rect(0,0,800,600));
            std::vector<double> samples;
            for(unsigned repeat=0;repeat<5;++repeat) {
                auto surface=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,800*scale,600*scale);
                auto cr=Cairo::Context::create(surface); cr->scale(scale,scale);
                CanvasItemBuffer b{Geom::IntRect(0,0,800,600),scale,cr,false};
                auto t=Clock::now(); auto result=renderOutlinePreview(outlines,b,m); surface->flush();
                samples.push_back(std::chrono::duration<double,std::milli>(Clock::now()-t).count());
                if(dense && zoom==.12) {
                    EXPECT_EQ(camera.visibility,OutlineVisibility::tooDense); EXPECT_FALSE(result.ok());
                    EXPECT_STREQ(result.diagnostic,"Zoom in to see piece outlines.");
                    EXPECT_TRUE(std::all_of(surface->get_data(),surface->get_data()+surface->get_stride()*600*scale,[](auto c){return c==0;}));
                } else {
                    EXPECT_EQ(camera.visibility,OutlineVisibility::exact); EXPECT_TRUE(result.ok());
                    EXPECT_TRUE(std::any_of(surface->get_data(),surface->get_data()+surface->get_stride()*600*scale,[](auto c){return c!=0;}));
                }
            }
            std::sort(samples.begin(),samples.end());
            std::cout << "P3 fixture=" << (dense?"dense":"sparse") << " scale=" << scale << " zoom=" << zoom
                      << " visible=" << camera.visibleSegments << " draw_median_ms=" << samples[2]
                      << " draw_max_ms=" << samples.back() << " state=" << int(camera.visibility) << '\n';
            EXPECT_EQ(f.work.visits(),visits); EXPECT_EQ(outlines.storage,storage);
        }
        for(auto m:{Geom::Affine(0,0,0,1,0,0),Geom::Affine(1,0,0,1,INFINITY,0)})
            EXPECT_EQ(outlineCamera(*storage,m,Geom::Rect(0,0,800,600)).visibility,OutlineVisibility::unavailable);
    }
}
TEST_F(Overlay, CoverageClipsTilesAndVeryLargeOffscreenPans) {
    Fixture f(16,16,{0,1,16,255},{1,0,0,1,8.25,8.5});
    ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok()); auto o=outlines(f,1);
    ASSERT_TRUE(overlay.installOutlines(*desktop,o,1).ok()); auto full=render();
    auto m=Geom::Affine(1,0,0,1,8.25,8.5)*desktop->doc2dt();
    for (int x:{0,16,32,48}) {
        auto surface=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,16,64);
        auto cr=Cairo::Context::create(surface); CanvasItemBuffer b{Geom::IntRect(x,0,x+16,64),1,cr,false};
        ASSERT_TRUE(renderOutlinePreview(o,b,m).ok()); surface->flush();
        for (int y=0;y<64;++y) EXPECT_TRUE(std::equal(full.data()+(y*64+x)*4,full.data()+(y*64+x+16)*4,
                                                   surface->get_data()+y*surface->get_stride()));
    }
    for (double offset:{-1e15,1e15}) {
        auto surface=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,64,64);
        auto cr=Cairo::Context::create(surface); CanvasItemBuffer b{Geom::IntRect(0,0,64,64),1,cr,false};
        ASSERT_TRUE(renderOutlinePreview(o,b,m*Geom::Translate(offset,offset)).ok()); surface->flush();
        EXPECT_TRUE(std::all_of(surface->get_data(),surface->get_data()+surface->get_stride()*64,[](auto v){return v==0;}));
    }
    overlay.clear();
}
TEST_F(Overlay, AxisAlignedCoverageMatchesIndependentCellEdgeOracle) {
    std::vector<unsigned> cells{0,1,2,8,10,16,17,18,63}; // ring with a hole and a separate island
    std::vector<Edge> edges;
    auto occupied=[&](int x,int y) {
        return x>=0 && y>=0 && x<8 && y<8 && std::find(cells.begin(),cells.end(),y*8+x)!=cells.end();
    };
    for (auto cell:cells) {
        unsigned x=cell%8,y=cell/8;
        if (!occupied(x-1,y)) edges.emplace_back(x,y,x,y+1);
        if (!occupied(x+1,y)) edges.emplace_back(x+1,y,x+1,y+1);
        if (!occupied(x,y-1)) edges.emplace_back(x,y,x+1,y);
        if (!occupied(x,y+1)) edges.emplace_back(x,y+1,x+1,y+1);
    }
    Generation gen=0;
    for (auto transform : {GridTransform{1,0,0,1,8,8},GridTransform{0.3,0,0,0.7,8.25,9.3},
                           GridTransform{-0.7,0,0,0.3,24,8.5},GridTransform{0,0.4,-0.8,0,24.3,8.2}}) {
        Fixture f(8,8,cells,transform);
        ASSERT_TRUE(overlay.beginGeneration(*desktop,++gen).ok());
        ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,gen),gen).ok());
        auto m=Geom::Affine(transform[0],transform[1],transform[2],transform[3],transform[4],transform[5])*desktop->doc2dt();
        for (int scale:{1,2}) {
            auto bytes=render(scale);
            for (int y=0;y<64*scale;++y) for (int x=0;x<64*scale;++x) {
                double coverage[2]{};
                for (auto [ax,ay,bx,by]:edges) {
                    auto a=(Geom::Point(ax,ay)*m)*scale, b=(Geom::Point(bx,by)*m)*scale;
                    for (int pass=0;pass<2;++pass) {
                        double half=pass?0.5:1.5;
                        double w=std::max(0.0,std::min(double(x+1),std::max(a.x(),b.x())+half)-std::max(double(x),std::min(a.x(),b.x())-half));
                        double h=std::max(0.0,std::min(double(y+1),std::max(a.y(),b.y())+half)-std::max(double(y),std::min(a.y(),b.y())-half));
                        coverage[pass]=std::max(coverage[pass],w*h);
                    }
                }
                auto actual=&bytes[(y*64*scale+x)*4];
                double alpha=coverage[1]+0.55*coverage[0]*(1-coverage[1]);
                // Cairo's 8-bit colour composition and coverage quantization each round once.
                ASSERT_NEAR(actual[3],255*alpha,2) << x << ',' << y;
                ASSERT_NEAR(actual[0],255*coverage[1],2) << x << ',' << y;
                ASSERT_NEAR(actual[1],255*0.85*coverage[1],2) << x << ',' << y;
                EXPECT_EQ(actual[2],0);
            }
        }
        overlay.clear();
    }
}
TEST_F(Overlay, T22DocumentTransformBothAxisOrientations) {
    Fixture f(3,1,{0,2},{2,0,0,2,8,12});
    Generation gen=0;
    for (bool down : {true,false}) {
        overlay.clear(); document->getNamedView()->set_y_axis_down(down); document->ensureUpToDate();
        ASSERT_EQ(desktop->yaxisdown(),down);
        desktop->getSelection()->set(cast<SPItem>(document->getObjectById("im")));
        auto probe=make_canvasitem<SnapshotProbe>(desktop->getCanvasTemp());
        probe->context().setAffine(Geom::identity());
        ASSERT_TRUE(overlay.beginGeneration(*desktop,++gen).ok());
        ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,gen),gen).ok());
        auto item=overlay.canvasItem(); item->update(false); ASSERT_TRUE(item->get_bounds());
        auto expected=Geom::Rect(Geom::Point(8,12),Geom::Point(14,14)); expected*=desktop->doc2dt(); expected.expandBy(4);
        EXPECT_EQ(*item->get_bounds(),expected);
        auto bytes=render(); auto y=down?12:50;
        EXPECT_GT(bytes[(y*64+8)*4+3],0); EXPECT_EQ(bytes[((down?50:12)*64+8)*4+3],0);
        EXPECT_TRUE(overlay.zoomToPiece(0,gen).ok()); overlay.clear();
    }
}
TEST_F(Overlay, T29InvalidationAfterStrokedBatchDiscardsWholeGeneration) {
    std::vector<unsigned> visible; for(unsigned i=0;i<1250;++i) visible.push_back((i/50)*200+i%50*2);
    Fixture f(100,50,visible,{0.5,0,0,0.5,8,8});
    auto probe=make_canvasitem<SnapshotProbe>(desktop->getCanvasTemp());
    ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,1),1).ok()); auto item=overlay.canvasItem(); item->update(false);
    probe->context().snapshot();
    // Native revision emission after the first 4096-edge halo+accent strokes.
    struct Change { SPImage *image; unsigned key; unsigned calls=0; } change{cast<SPImage>(document->getObjectById("im")),desktop->dkey};
    hooks.context=&change; hooks.afterStroke=[](void *p) noexcept { auto &c=*static_cast<Change *>(p); ++c.calls; c.image->composedViewGeneration(c.key); };
    auto surface=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,64,64);
    auto cr=Cairo::Context::create(surface); cr->set_source_rgb(0.2,0.3,0.4); cr->paint(); surface->flush();
    std::vector<unsigned char> before(surface->get_data(),surface->get_data()+surface->get_stride()*64);
    CanvasItemBuffer b{Geom::IntRect(0,0,64,64),1,cr,false}; item->render(b); surface->flush();
    EXPECT_EQ(change.calls,1u); EXPECT_EQ(overlay.canvasItem(),nullptr);
    EXPECT_TRUE(std::equal(before.begin(),before.end(),surface->get_data())); // preserves underlying bitmap
    hooks.afterStroke=nullptr; probe->context().unsnapshot();
}
TEST_F(Overlay, T29RetirementAllocationFaultIsNoexceptAndRetryable) {
    Fixture f(3,1,{0,2}); auto probe=make_canvasitem<SnapshotProbe>(desktop->getCanvasTemp());
    ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,1),1).ok()); auto item=overlay.canvasItem(); item->update(false);
    probe->context().snapshot(); AllocationFault fault{1}; hooks.retirement=&fault;
    auto image=cast<SPImage>(document->getObjectById("im"));
    EXPECT_NO_THROW(image->composedViewGeneration(desktop->dkey)); EXPECT_EQ(fault.attempts,1u);
    EXPECT_EQ(overlay.canvasItem(),nullptr); EXPECT_FALSE(overlay.ready());
    hooks.retirement=nullptr;
    ASSERT_TRUE(overlay.beginGeneration(*desktop,2).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,2),2).ok());
    probe->context().unsnapshot(); EXPECT_TRUE(overlay.ready());
    auto bytes=render(); EXPECT_TRUE(std::any_of(bytes.begin(),bytes.end(),[](auto c) { return c!=0; }));
    overlay.clear();
}
TEST_F(Overlay, T29ReplacementDocumentAndRetiredLeaseRefuseReadiness) {
    Fixture f(3,1,{0,2}); ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    auto o=outlines(f,1); ASSERT_TRUE(overlay.installOutlines(*desktop,o,1).ok());
    lease->invalidate(); EXPECT_FALSE(overlay.ready()); EXPECT_EQ(overlay.canvasItem(),nullptr);
    ASSERT_TRUE(overlay.beginGeneration(*desktop,2).ok()); o=outlines(f,2);
    ASSERT_TRUE(overlay.installOutlines(*desktop,o,2).ok());
    auto other=SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'/>");
    desktop->setDocument(other.get()); EXPECT_FALSE(overlay.ready()); EXPECT_EQ(overlay.canvasItem(),nullptr);
    desktop.reset();
}
TEST_F(Overlay, T29ViewPixelReplacementAndAbsentViewClearImmediately) {
    auto probe=make_canvasitem<SnapshotProbe>(desktop->getCanvasTemp());
    auto native=desktop->getCanvasDrawing()->get_drawing();
    Fixture f(3,1,{0,2}); auto image=cast<SPImage>(document->getObjectById("im"));
    for (Generation gen=1;gen<=3;++gen) {
        ASSERT_TRUE(overlay.beginGeneration(*desktop,gen).ok()); auto o=outlines(f,gen);
        ASSERT_TRUE(overlay.installOutlines(*desktop,o,gen).ok()); auto frozen=overlay.canvasItem();
        frozen->update(false); probe->context().snapshot(); native->snapshot();
        if (gen==1) EXPECT_TRUE(image->setViewPixbuf(desktop->dkey,nullptr));
        else EXPECT_NE(image->composedViewGeneration(desktop->dkey),0u);
        EXPECT_EQ(overlay.canvasItem(),nullptr); // no ready() polling or native drawing update
        auto surface=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,64,64);
        auto cr=Cairo::Context::create(surface); CanvasItemBuffer b{Geom::IntRect(0,0,64,64),1,cr,false};
        frozen->render(b); surface->flush();
        EXPECT_TRUE(std::all_of(surface->get_data(),surface->get_data()+surface->get_stride()*64,[](auto c) { return c==0; }));
        native->unsnapshot(); probe->context().unsnapshot();
    }
    ASSERT_TRUE(overlay.beginGeneration(*desktop,4).ok()); auto o=outlines(f,4);
    ASSERT_TRUE(overlay.installOutlines(*desktop,o,4).ok()); image->invoke_hide(desktop->dkey);
    EXPECT_EQ(overlay.canvasItem(),nullptr); EXPECT_FALSE(overlay.ready());
    ASSERT_TRUE(overlay.beginGeneration(*desktop,5).ok());
    EXPECT_EQ(overlay.installOutlines(*desktop,o,4).status,Status::canceled);
}
TEST_F(Overlay, T29RebindDisconnectsPreviousView) {
    Fixture f(3,1,{0,2}); ASSERT_TRUE(overlay.beginGeneration(*desktop,1).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,1),1).ok());
    auto previous=std::move(desktop); auto previousLease=std::move(lease);
    desktop=std::make_unique<SPDesktop>(document->getNamedView());
    desktop->getSelection()->set(cast<SPItem>(document->getObjectById("im")));
    PlatformEvidence e; e.platform=EvidencePlatform::Mac; e.observationNs=20000;
    lease=std::make_unique<DependencyLease>(*desktop,e);
    ASSERT_TRUE(overlay.beginGeneration(*desktop,2).ok());
    ASSERT_TRUE(overlay.installOutlines(*desktop,outlines(f,2),2).ok());
    auto image=cast<SPImage>(document->getObjectById("im"));
    ASSERT_NE(image->composedViewGeneration(previous->dkey),0u); EXPECT_TRUE(overlay.ready());
    previousLease.reset(); previous.reset(); EXPECT_TRUE(overlay.ready()); overlay.clear();
}
TEST(Outlines, T19ReservationFailureAndCancellationLeaveNoStorage) {
    Fixture f(3,1,{0,2}); Budget small{Budget::FixedLimitForTest{},1}; JobWork work(3);
    FinalGrid g; g.width=3; g.height=1; g.pixelToDocument={1,0,0,1,0,0};
    auto refused=prepareOutlines(f.partition.value,g,small,work); EXPECT_FALSE(refused.ok()); EXPECT_FALSE(refused.value.storage); EXPECT_EQ(small.reserved(),0u);
    auto flag=std::make_shared<std::atomic<bool>>(true);
    auto exact=prepareOutlines(f.partition.value,g,f.budget,work,{},nullptr,8); EXPECT_TRUE(exact.ok());
    auto capped=prepareOutlines(f.partition.value,g,f.budget,work,{},nullptr,7); EXPECT_FALSE(capped.ok()); EXPECT_FALSE(capped.value.storage);
    auto canceled=prepareOutlines(f.partition.value,g,f.budget,work,Stop(flag)); EXPECT_EQ(canceled.outcome.status,Status::canceled); EXPECT_FALSE(canceled.value.storage);
}
} // namespace
