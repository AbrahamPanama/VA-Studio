// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "util/bitmap-contour-offset.h"
#include "util/bitmap-island-specks.h"
#include <png.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
using namespace Inkscape::Bitmap;
namespace {
struct Image {
    unsigned w,h; std::vector<std::uint8_t> pixels;
    Image(unsigned w,unsigned h):w(w),h(h),pixels(std::size_t(w)*h*4) {}
    void put(unsigned x,unsigned y,unsigned a=255) { pixels[(std::size_t(y)*w+x)*4+3]=a; }
    void box(unsigned x,unsigned y,unsigned ex,unsigned ey,unsigned a=255) {
        for (auto j=y;j<ey;++j) for (auto i=x;i<ex;++i) put(i,j,a);
    }
    RgbaView view() const { return {pixels.data(),pixels.size(),std::uint64_t(w)*4,w,h}; }
};
struct Fixture {
    Budget budget{Budget::FixedLimitForTest{},512*MiB}; JobWork work{25000000};
    Result<Regions> regions; Result<Partition> partition;
    void prepare(Image const &im) {
        regions=label(im.view(),alphaLut(0,0),budget,work); ASSERT_TRUE(regions.ok()) << regions.outcome.diagnostic;
        partition=enclose(regions.value,budget,work); ASSERT_TRUE(partition.ok()) << partition.outcome.diagnostic;
    }
    Result<ContourSet> offset(Image const &im,double o,double gap=0,double dx=25.4,double dy=25.4,
        Stop stop={},ContourOptions options={}) {
        return offsetContours(im.view(),partition.value,{o,gap,dx,dy},budget,work,stop,options);
    }
};
void empty(Result<ContourSet> const &r) {
    EXPECT_FALSE(r.ok()); EXPECT_EQ(r.value.pieceCount,0u); EXPECT_EQ(r.value.ringCount,0u); EXPECT_EQ(r.value.pointCount,0u);
    EXPECT_EQ(r.value.points(),nullptr); EXPECT_EQ(r.value.rings(),nullptr); EXPECT_EQ(r.value.pieces(),nullptr);
}
bool contains(ContourSet const &r,double x,double y) {
    bool inside=false;
    for (unsigned k=0;k<r.ringCount;++k) {
        auto ring=r.rings()[k];
        for (auto i=ring.begin,j=ring.end-1;i<ring.end;j=i++) {
            auto a=r.points()[i],b=r.points()[j];
            if ((a.y>y)!=(b.y>y) && x<(b.x-a.x)*(y-a.y)/(b.y-a.y)+a.x) inside=!inside;
        }
    }
    return inside;
}
double boundaryDistance(ContourSet const &r,ContourPoint p) {
    double best=std::numeric_limits<double>::infinity();
    for (unsigned k=0;k<r.ringCount;++k) {
        auto ring=r.rings()[k];
        for (auto i=ring.begin,j=ring.end-1;i<ring.end;j=i++) {
            auto a=r.points()[j],b=r.points()[i]; double dx=b.x-a.x,dy=b.y-a.y;
            auto den=dx*dx+dy*dy;
            auto t=den ? std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/den,0.,1.) : 0.;
            best=std::min(best,std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy));
        }
    }
    return best;
}
template<class F> void wholeBoundary(ContourSet const &r,F visit) {
    for (unsigned k=0;k<r.ringCount;++k) {
        auto ring=r.rings()[k];
        for (auto i=ring.begin,j=ring.end-1;i<ring.end;j=i++)
            for (unsigned n=0;n<=16;++n) {
                auto a=r.points()[j],b=r.points()[i]; double t=n/16.;
                visit(ContourPoint{a.x+t*(b.x-a.x),a.y+t*(b.y-a.y)});
            }
    }
}
double perimeter(ContourSet const &r) {
    double p=0;
    for (unsigned k=0;k<r.ringCount;++k) { auto ring=r.rings()[k];
        for (auto i=ring.begin,j=ring.end-1;i<ring.end;j=i++) p+=std::hypot(r.points()[i].x-r.points()[j].x,r.points()[i].y-r.points()[j].y);
    } return p;
}
}
TEST(BitmapContourOffset,StraightEdgesAndZeroContinuity) {
    Image im(80,80); im.box(10,10,70,70); Fixture f; f.prepare(im);
    for (double o:{-.001,.001,-1.,1.,-8.,8.,0.}) {
        auto r=f.offset(im,o); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.ringCount,1u);
        auto ring=r.value.rings()[0];
        EXPECT_NEAR(ring.minX,10-o,.001); EXPECT_NEAR(ring.minY,10-o,.001);
        EXPECT_NEAR(ring.maxX,70+o,.001); EXPECT_NEAR(ring.maxY,70+o,.001);
    }
}
TEST(BitmapContourOffset,ZeroIsExactlyB1WithoutDistanceAllocations) {
    Image im(10,10); im.box(0,0,10,10); Fixture f; f.prepare(im);
    AllocationFault a,b; ContourOptions oa,ob; oa.fault=&a; ob.fault=&b;
    auto base=traceContours(im.view(),f.partition.value,f.budget,f.work,{},oa);
    auto zero=f.offset(im,0,0,25.4,25.4,{},ob); ASSERT_TRUE(base.ok()); ASSERT_TRUE(zero.ok());
    ASSERT_EQ(zero.value.pointCount,base.value.pointCount); ASSERT_EQ(zero.value.ringCount,base.value.ringCount);
    EXPECT_EQ(a.attempts,b.attempts);
    EXPECT_EQ(std::memcmp(zero.value.points(),base.value.points(),base.value.pointCount*sizeof(ContourPoint)),0);
    auto x=zero.value.rings()[0],y=base.value.rings()[0];
    EXPECT_EQ(x.area,y.area); EXPECT_EQ(x.depth,y.depth); EXPECT_EQ(x.parent,y.parent);
}
TEST(BitmapContourOffset,WholeContourContinuityAndContainment) {
    // Outer chamfers, hole chamfers and a concave notch, including unequal DPI.
    for (bool complex:{false,true}) for (double dpiY:{25.4,50.8}) {
        unsigned size=complex ? 20 : 10;
        Image im(size,size); im.box(0,0,size,size);
        if (complex) { im.box(5,5,10,10,0); im.box(14,0,16,8,0); }
        Fixture f; f.prepare(im); auto base=f.offset(im,0,0,25.4,dpiY); ASSERT_TRUE(base.ok());
        for (double sign:{-1.,1.}) {
            double previous=10;
            for (double size:{1.,.1,1e-2,1e-4,1e-6}) {
                auto r=f.offset(im,sign*size,0,25.4,dpiY); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
                double maximum=0;
                wholeBoundary(r.value,[&](ContourPoint p) {
                    auto d=boundaryDistance(base.value,p); maximum=std::max(maximum,d);
                    if (sign>0 && contains(base.value,p.x,p.y)) EXPECT_LE(d,1e-3);
                    if (sign<0 && !contains(base.value,p.x,p.y)) EXPECT_LE(d,1e-3);
                });
                wholeBoundary(base.value,[&](ContourPoint p) {
                    auto d=boundaryDistance(r.value,p); maximum=std::max(maximum,d);
                    if (sign>0 && !contains(r.value,p.x,p.y)) EXPECT_LE(d,1e-3);
                    if (sign<0 && contains(r.value,p.x,p.y)) EXPECT_LE(d,1e-3);
                });
                EXPECT_LT(maximum,previous);
                // Once the notch closes at a larger offset, its old bottom is
                // far from the new boundary. The distance bound is asymptotic.
                if (size<=1e-2) EXPECT_LE(maximum,4*size+1e-8);
                previous=maximum;
            }
        }
    }
}
TEST(BitmapContourOffset,RawPointsBeyondSerializedOutputLimit) {
    Image im(1000,220); im.box(0,0,1000,1);
    for (unsigned x=0;x<1000;x+=2) im.box(x,1,x+1,220);
    Fixture f; f.prepare(im); auto r=f.offset(im,0); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_GE(r.value.pointCount,210000u); EXPECT_LT(r.value.pointCount,2000000u);
    // A long strip isolates raw output admission from the comb's expensive
    // distance queries and exercises B2 assembly with a nonzero offset.
    Image strip(14000,16);
    for (unsigned y=0;y<16;y+=2) strip.box(0,y,14000,y+1);
    Fixture g; g.prepare(strip); ASSERT_EQ(g.partition.value.pieceCount,8u);
    auto expanded=g.offset(strip,.001); ASSERT_TRUE(expanded.ok()) << expanded.outcome.diagnostic;
    EXPECT_GE(expanded.value.pointCount,210000u); EXPECT_LT(expanded.value.pointCount,2000000u);
}
TEST(BitmapContourOffset,ExactExtinctionPreservesOtherPiece) {
    Image im(20,10); im.box(0,0,3,3); im.box(10,0,20,10); Fixture f; f.prepare(im);
    ASSERT_EQ(f.partition.value.pieceCount,2u); auto r=f.offset(im,-1.5);
    ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.pieceCount,2u);
    EXPECT_TRUE(r.value.pieces()[0].noContour);
    EXPECT_EQ(r.value.pieces()[0].ringBegin,r.value.pieces()[0].ringEnd);
    EXPECT_FALSE(r.value.pieces()[1].noContour); ASSERT_EQ(r.value.ringCount,1u);
    Image control(20,10); control.box(10,0,20,10); Fixture g; g.prepare(control);
    auto survivor=g.offset(control,-1.5); ASSERT_TRUE(survivor.ok());
    ASSERT_EQ(r.value.pointCount,survivor.value.pointCount);
    EXPECT_EQ(std::memcmp(r.value.points(),survivor.value.points(),r.value.pointCount*sizeof(ContourPoint)),0);
}
TEST(BitmapContourOffset,DiskRadius) {
    // An analytic circle field avoids conflating the radius oracle with 8-bit
    // alpha quantization and coverage reconstruction in the input image.
    constexpr unsigned size=180; constexpr double centre=90,radius=60;
    Budget budget(Budget::FixedLimitForTest{},64*MiB); JobWork work(25000000);
    std::vector<float> values(size*size);
    for (unsigned y=0;y<size;++y) for (unsigned x=0;x<size;++x)
        values[y*size+x]=radius-std::hypot(x+.5-centre,y+.5-centre);
    auto zero=traceLevel({values.data(),size,size,0,0},budget,work); ASSERT_TRUE(zero.ok());
    for (double o:{-4.,8.,24.}) {
        auto field=signedDistance(zero.value,0,0,size,size,1,1,budget,work);
        ASSERT_TRUE(field.ok()) << field.outcome.diagnostic;
        auto f=field.value.view();
        for (unsigned i=0;i<size*size;++i) values[i]=float(double(f.values[i])+o);
        auto r=traceLevel({values.data(),size,size,0,0},budget,work);
        ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.ringCount,1u);
        EXPECT_NEAR(std::sqrt(r.value.rings()[0].area/std::acos(-1.)),radius+o,.01);
        for (unsigned i=0;i<r.value.pointCount;++i)
            EXPECT_NEAR(std::hypot(r.value.points()[i].x-centre,r.value.points()[i].y-centre),radius+o,.01);
    }
}
TEST(BitmapContourOffset,DistanceMatchesIndependentSegmentOracle) {
    Budget b(Budget::FixedLimitForTest{},32*MiB); JobWork work(10000);
    std::vector<float> values(24*24,-.5f);
    for (unsigned y=3;y<21;++y) for (unsigned x=4;x<18;++x) values[y*24+x]=.5f;
    auto rings=traceLevel({values.data(),24,24,0,0},b,work); ASSERT_TRUE(rings.ok());
    auto field=signedDistance(rings.value,-2,-2,28,28,.2,.1,b,work); ASSERT_TRUE(field.ok());
    for (unsigned y=0;y<28;++y) for (unsigned x=0;x<28;++x) {
        double expected=1e20;
        for (unsigned k=0;k<rings.value.ringCount;++k) {
            auto ring=rings.value.rings()[k];
            for (auto i=ring.begin,j=ring.end-1;i<ring.end;j=i++) {
                auto a=rings.value.points()[j],c=rings.value.points()[i];
                double ax=a.x*.2,ay=a.y*.1,dx=(c.x-a.x)*.2,dy=(c.y-a.y)*.1;
                double px=(x-2.+.5)*.2,py=(y-2.+.5)*.1;
                auto t=std::clamp(((px-ax)*dx+(py-ay)*dy)/(dx*dx+dy*dy),0.,1.);
                expected=std::min(expected,std::hypot(px-ax-t*dx,py-ay-t*dy));
            }
        }
        EXPECT_NEAR(std::abs(field.value.view().values[y*28+x]),expected,1e-6);
    }
}
TEST(BitmapContourOffset,RefinedCrossingsOnQuantizedDisk) {
    Image im(150,150);
    for (unsigned y=0;y<im.h;++y) for (unsigned x=0;x<im.w;++x)
        im.put(x,y,std::lround(255*std::clamp(60.5-std::hypot(x+.5-75,y+.5-75),0.,1.)));
    for (double offset:{-4.,8.,24.}) {
        Fixture f; f.prepare(im);
        auto support=pieceSupportField(im.view(),f.partition.value,0,f.budget,f.work); ASSERT_TRUE(support.ok());
        auto zero=traceLevel(support.value.field,f.budget,f.work); ASSERT_TRUE(zero.ok());
        auto piece=f.partition.value.pieces()[0]; auto pad=2+std::ceil(std::max(0.,offset));
        unsigned w=piece.endX-piece.x+2*pad,h=piece.endY-piece.y+2*pad;
        auto d=signedDistance(zero.value,piece.x-pad,piece.y-pad,w,h,1,1,f.budget,f.work); ASSERT_TRUE(d.ok());
        std::vector<float> v(std::size_t(w)*h);
        for (std::size_t i=0;i<v.size();++i) v[i]=float(double(d.value.view().values[i])+offset);
        auto exact=traceLevel({v.data(),w,h,piece.x-pad,piece.y-pad},f.budget,f.work); ASSERT_TRUE(exact.ok());
        auto hybrid=f.offset(im,offset); ASSERT_TRUE(hybrid.ok());
        ASSERT_EQ(hybrid.value.pointCount,exact.value.pointCount); ASSERT_EQ(hybrid.value.ringCount,exact.value.ringCount);
        // The old full sampled field has the same edge graph. The corrected
        // vertices now satisfy the polygon's actual iso-distance independently.
        auto base=f.offset(im,0); ASSERT_TRUE(base.ok());
        for (unsigned i=0;i<hybrid.value.pointCount;++i)
            EXPECT_NEAR(boundaryDistance(base.value,hybrid.value.points()[i]),std::abs(offset),1e-7);
        std::cout << "QUANTIZED_DISK offset=" << offset << " exact_radius="
                  << std::sqrt(exact.value.rings()[0].area/std::acos(-1.)) << '\n';
    }
}
TEST(BitmapContourOffset,SteinerConvexTracedPolygon) {
    Image im(100,100); im.box(10,10,90,90); Fixture f; f.prepare(im);
    auto base=f.offset(im,0); ASSERT_TRUE(base.ok()); auto a=base.value.rings()[0].area, p=perimeter(base.value);
    for (double o:{1.,8.,24.}) {
        auto r=f.offset(im,o); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.ringCount,1u);
        auto expected=a+p*o+std::acos(-1.)*o*o; EXPECT_NEAR(r.value.rings()[0].area,expected,expected*.005);
    }
}
TEST(BitmapContourOffset,ClosingMeshHoleAndSlit300Dpi) {
    Image im(150,150); im.box(5,5,145,145);
    // Four-pixel holes/slit are 0.3387mm: conservatively wider than 0.3mm.
    for (unsigned y=15;y<65;y+=10) for (unsigned x=15;x<135;x+=10) im.box(x,y,x+4,y+4,0);
    im.box(40,90,64,114,0); // 2.032mm hole
    im.box(100,5,104,85,0);
    Fixture f; f.prepare(im); auto r=f.offset(im,0,.5,300,300);
    ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.ringCount,2u);
    unsigned holes=0; for (unsigned i=0;i<r.value.ringCount;++i) holes+=r.value.rings()[i].depth==1;
    EXPECT_EQ(holes,1u);
    // Slit closure restores the region around its former centre.
    EXPECT_TRUE(contains(r.value,102,40)); EXPECT_FALSE(contains(r.value,52,102));
    for (unsigned y=15;y<65;y+=10) for (unsigned x=15;x<135;x+=10) EXPECT_TRUE(contains(r.value,x+2,y+2));
    auto unclosed=f.offset(im,0,0,300,300); ASSERT_TRUE(unclosed.ok());
    EXPECT_FALSE(contains(unclosed.value,102,40)); EXPECT_GT(unclosed.value.ringCount,50u);
}
TEST(BitmapContourOffset,VanishingIsPerPiece) {
    Image im(90,50); im.box(2,2,8,8); im.box(30,5,75,45); Fixture f; f.prepare(im);
    auto r=f.offset(im,-8); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.pieceCount,2u);
    EXPECT_TRUE(r.value.pieces()[0].noContour); EXPECT_FALSE(r.value.pieces()[1].noContour); EXPECT_EQ(r.value.ringCount,1u);
}
TEST(BitmapContourOffset,AnisotropicMillimetres) {
    Image im(100,100); im.box(20,20,80,80); Fixture f; f.prepare(im);
    auto r=f.offset(im,1,0,300,150); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.ringCount,1u);
    auto ring=r.value.rings()[0]; EXPECT_NEAR((20-ring.minX)*25.4/300,1,1e-5);
    EXPECT_NEAR((20-ring.minY)*25.4/150,1,1e-5);
}
TEST(BitmapContourOffset,DistanceLowerLayerSignsAndUnits) {
    Budget b(Budget::FixedLimitForTest{},32*MiB); JobWork w(10000);
    std::vector<float> values(24*24,-.5f);
    for (unsigned y=2;y<22;++y) for (unsigned x=2;x<22;++x) values[y*24+x]=.5f;
    auto rings=traceLevel({values.data(),24,24,0,0},b,w); ASSERT_TRUE(rings.ok());
    auto d=signedDistance(rings.value,-5,-5,34,34,.1,.2,b,w); ASSERT_TRUE(d.ok()) << d.outcome.diagnostic;
    auto f=d.value.view(); EXPECT_NEAR(f.values[17*34+6],-.05,1e-7); EXPECT_NEAR(f.values[17*34+7],.05,1e-7);
    EXPECT_NEAR(f.values[6*34+17],-.1,1e-7); EXPECT_NEAR(f.values[7*34+17],.1,1e-7);
}
TEST(BitmapContourOffset,Deterministic) {
    Image im(45,45); im.box(4,4,41,41); im.box(15,15,29,29,0); Fixture f; f.prepare(im);
    auto a=f.offset(im,.8,.5),b=f.offset(im,.8,.5); ASSERT_TRUE(a.ok()); ASSERT_TRUE(b.ok());
    ASSERT_EQ(a.value.pointCount,b.value.pointCount); ASSERT_EQ(a.value.ringCount,b.value.ringCount);
    EXPECT_EQ(std::memcmp(a.value.points(),b.value.points(),a.value.pointCount*sizeof(ContourPoint)),0);
    for (unsigned i=0;i<a.value.ringCount;++i) {
        auto x=a.value.rings()[i],y=b.value.rings()[i]; EXPECT_EQ(x.area,y.area); EXPECT_EQ(x.parent,y.parent); EXPECT_EQ(x.depth,y.depth);
    }
}
TEST(BitmapContourOffset,StopBudgetVisitsAndPointCapsAreAtomic) {
    Image im(32,32); im.box(2,2,30,30); Fixture f; f.prepare(im); auto held=f.budget.reserved();
    auto flag=std::make_shared<std::atomic<bool>>(true);
    auto stopped=f.offset(im,1,.5,25.4,25.4,Stop(flag)); empty(stopped); EXPECT_EQ(stopped.outcome.status,Status::canceled);
    EXPECT_EQ(f.budget.reserved(),held);
    Budget tiny(Budget::FixedLimitForTest{},1); JobWork w(10000);
    auto denied=offsetContours(im.view(),f.partition.value,{},tiny,w); empty(denied); EXPECT_EQ(denied.outcome.status,Status::failed); EXPECT_EQ(tiny.reserved(),0u);
    JobWork exhausted(0); ASSERT_TRUE(exhausted.advance(exhausted.limit()).ok());
    auto visits=offsetContours(im.view(),f.partition.value,{},f.budget,exhausted); empty(visits); EXPECT_EQ(visits.outcome.status,Status::failed);
    ContourOptions cap; cap.maxPoints=4; auto capped=f.offset(im,1,0,25.4,25.4,{},cap); empty(capped);
    EXPECT_EQ(f.budget.reserved(),held);
}
TEST(BitmapContourOffset,EveryAllocationFaultIsAtomic) {
    Image im(20,20); im.box(2,2,18,18);
    std::uint64_t attempts=0;
    { Fixture f; f.prepare(im); AllocationFault fault; ContourOptions o; o.fault=&fault;
      auto r=f.offset(im,1,.5,25.4,25.4,{},o); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; attempts=fault.attempts; }
    ASSERT_GT(attempts,20u);
    for (std::uint64_t i=1;i<=attempts;++i) {
        Fixture f; f.prepare(im); auto held=f.budget.reserved(); AllocationFault fault{i,0}; ContourOptions o; o.fault=&fault;
        auto r=f.offset(im,1,.5,25.4,25.4,{},o); empty(r); EXPECT_EQ(r.outcome.status,Status::failed) << i; EXPECT_EQ(f.budget.reserved(),held) << i;
    }
}
TEST(BitmapContourOffset,StopDuringDistanceAllocation) {
    Image im(30,30); im.box(2,2,28,28); Fixture f; f.prepare(im); auto held=f.budget.reserved();
    struct State { std::shared_ptr<std::atomic<bool>> flag; unsigned calls=0; } state{std::make_shared<std::atomic<bool>>(false)};
    ContourOptions o; o.observerData=&state;
    o.observe=[](ContourPhase phase,void *v) noexcept { auto &s=*static_cast<State *>(v); if (phase==ContourPhase::allocation && ++s.calls==10) s.flag->store(true); };
    auto r=f.offset(im,1,.5,25.4,25.4,Stop(state.flag),o); empty(r); EXPECT_EQ(r.outcome.status,Status::canceled); EXPECT_EQ(f.budget.reserved(),held);
}
TEST(BitmapContourOffset,InvalidSettingsAndDistanceRefusals) {
    Image im(10,10); im.box(1,1,9,9); Fixture f; f.prepare(im); auto held=f.budget.reserved();
    for (auto settings:{OffsetSettings{0,-1,96,96},OffsetSettings{0,0,0,96},
                       OffsetSettings{INFINITY,0,96,96},OffsetSettings{0,NAN,96,96}}) {
        auto r=offsetContours(im.view(),f.partition.value,settings,f.budget,f.work); empty(r); EXPECT_EQ(f.budget.reserved(),held);
    }
    auto support=pieceSupportField(im.view(),f.partition.value,0,f.budget,f.work); ASSERT_TRUE(support.ok());
    auto rings=traceLevel(support.value.field,f.budget,f.work); ASSERT_TRUE(rings.ok());
    Budget tiny(Budget::FixedLimitForTest{},1); JobWork work(10000);
    auto denied=signedDistance(rings.value,0,0,10,10,1,1,tiny,work);
    EXPECT_FALSE(denied.ok()); EXPECT_EQ(denied.value.view().values,nullptr); EXPECT_EQ(tiny.reserved(),0u);
    auto flag=std::make_shared<std::atomic<bool>>(true);
    auto stopped=signedDistance(rings.value,0,0,10,10,1,1,f.budget,work,Stop(flag));
    EXPECT_EQ(stopped.outcome.status,Status::canceled); EXPECT_EQ(stopped.value.view().values,nullptr);
}
TEST(BitmapContourOffset,JobWorkPerPixelLimitArithmetic) {
    JobWork analysis(25000000), contour(25000000,40);
    EXPECT_EQ(analysis.limit(),300000000u);
    EXPECT_EQ(contour.limit(),1100000000u);
    EXPECT_TRUE(contour.advance(contour.limit()).ok());
    EXPECT_FALSE(contour.advance(1).ok());
    JobWork maximumPixels(100000000,40), zeroPixels(0,40), zeroMultiplier(25000000,0);
    EXPECT_EQ(maximumPixels.limit(),4100000000ull);
    EXPECT_EQ(zeroPixels.limit(),100000000u);
    EXPECT_EQ(zeroMultiplier.limit(),100000000u);
    constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
    JobWork lastValid(1,maximum-100000000);
    EXPECT_EQ(lastValid.limit(),maximum);
    EXPECT_TRUE(lastValid.advance(maximum).ok());
    JobWork invalidPixels(100000001,40), multiplyOverflow(2,maximum), addOverflow(1,maximum-99999999);
    for (auto refused:{&invalidPixels,&multiplyOverflow,&addOverflow}) {
        EXPECT_EQ(refused->limit(),0u);
        EXPECT_FALSE(refused->advance(0).ok());
    }
}
TEST(BitmapContourOffset,MeasureLimits5000) {
    auto root=std::getenv("VACARDS_EB_B2_MEASURE_ROOT");
    if (!root) GTEST_SKIP() << "Opt in with VACARDS_EB_B2_MEASURE_ROOT=.../limits-5000";
    for (auto suffix:{"/blobs-150-5000x5000.png","/sparse/blobs-150-5000x5000.png"}) {
        auto path=std::string(root)+suffix;
        FILE *file=std::fopen(path.c_str(),"rb"); ASSERT_NE(file,nullptr);
        auto png=png_create_read_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr); ASSERT_NE(png,nullptr);
        auto info=png_create_info_struct(png); ASSERT_NE(info,nullptr);
        ASSERT_EQ(setjmp(png_jmpbuf(png)),0); png_init_io(png,file); png_read_info(png,info);
        png_uint_32 ppmX=0,ppmY=0; int unit=0; ASSERT_TRUE(png_get_pHYs(png,info,&ppmX,&ppmY,&unit)); ASSERT_EQ(unit,PNG_RESOLUTION_METER);
        double dpiX=ppmX*.0254,dpiY=ppmY*.0254;
        png_destroy_read_struct(&png,&info,nullptr); std::fclose(file);
        png_image image{}; image.version=PNG_IMAGE_VERSION; ASSERT_TRUE(png_image_begin_read_from_file(&image,path.c_str()));
        image.format=PNG_FORMAT_RGBA; Image im(image.width,image.height);
        auto decoded=png_image_finish_read(&image,nullptr,im.pixels.data(),0,nullptr); png_image_free(&image); ASSERT_TRUE(decoded);
        Fixture f; Budget::Token input; ASSERT_TRUE(f.budget.acquire(Stage::input,im.pixels.size(),input).ok());
        f.prepare(im); ASSERT_EQ(f.partition.value.pieceCount,150u);
        auto metric=OrthogonalMetric::fromDpi(dpiX,dpiY); ASSERT_TRUE(metric.ok());
        auto attached=attach(f.partition.value,metric.value,f.budget,f.work); ASSERT_TRUE(attached.ok()) << attached.outcome.diagnostic;
        JobWork contourWork(std::uint64_t(im.w)*im.h,100);
        struct Peak {
            Budget &b; JobWork &work; std::uint64_t bytes, last=0, visits[4]{};
            unsigned traces=0, bucket=3; bool hierarchy=false, tracing=false;
            void account() noexcept {
                auto now=work.visits(); visits[bucket]+=now-last; last=now;
            }
        } peak{f.budget,contourWork,f.budget.reserved()};
        ContourOptions o; o.observerData=&peak;
        o.observe=[](ContourPhase phase,void *v) noexcept {
            auto &p=*static_cast<Peak *>(v); p.bytes=std::max(p.bytes,p.b.reserved());
            // These fixtures use four nonempty traces per piece: support,
            // dilation, erosion, offset. The first allocation after hierarchy
            // starts the next distance pass, after traceLevel has flushed.
            if (phase==ContourPhase::allocation && p.hierarchy) {
                p.account(); p.bucket=(p.traces-1)%4; p.hierarchy=false; p.tracing=false;
            } else if (phase==ContourPhase::trace) {
                p.account(); p.bucket=3; ++p.traces; p.tracing=true;
            } else if (phase==ContourPhase::hierarchy) {
                p.hierarchy=true;
            }
        };
        auto before=contourWork.visits(); auto start=std::chrono::steady_clock::now();
        auto result=offsetContours(im.view(),attached.value,{1,.5,dpiX,dpiY},f.budget,contourWork,{},o);
        auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        peak.account();
        EXPECT_EQ(peak.visits[0]+peak.visits[1]+peak.visits[2]+peak.visits[3],contourWork.visits()-before);
        EXPECT_TRUE(result.ok()) << result.outcome.diagnostic;
        if (!result.ok()) empty(result); else EXPECT_EQ(result.value.peakBudgetBytes,peak.bytes);
        std::cout << "B2_MEASURE " << suffix << " dpi=" << dpiX << 'x' << dpiY << " seconds=" << seconds
          << " visits=" << contourWork.visits()-before << " work_limit=" << contourWork.limit() << " peak_budget_bytes=" << peak.bytes
          << " rings=" << result.value.ringCount << " points=" << result.value.pointCount
          << " outcome=" << (result.ok()?"success":"refused") << " diagnostic=" << result.outcome.diagnostic << '\n';
        char const *steps[]={"closing_dilate","closing_erode","offset","tracing_support_output"};
        std::cout << "B2_STAGES " << suffix;
        for (unsigned i=0;i<4;++i) std::cout << ' ' << steps[i] << "_visits=" << peak.visits[i];
        std::cout << " traces_started=" << peak.traces << " refusal_step="
                  << (result.ok()?"none":(peak.tracing?"tracing":steps[peak.bucket])) << '\n';
    }
}
// Own main keeps this a plain gtest suite, without application initialization.
int main(int argc,char **argv) { ::testing::InitGoogleTest(&argc,argv); return RUN_ALL_TESTS(); }
