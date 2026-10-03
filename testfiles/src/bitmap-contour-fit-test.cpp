// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <2geom/bezier-utils.h>
#include "util/bitmap-contour-fit.h"
#include "util/bitmap-island-specks.h"
#include <png.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <type_traits>
#include <vector>
using namespace Inkscape::Bitmap;
namespace {
struct Image {
    unsigned w, h;
    std::vector<std::uint8_t> pixels;
    Image(unsigned x, unsigned y) : w(x), h(y), pixels(std::size_t(x)*y*4) {}
    void put(unsigned x, unsigned y, unsigned a=255) { pixels[(std::size_t(y)*w+x)*4+3]=a; }
    void box(unsigned x, unsigned y, unsigned ex, unsigned ey, unsigned a=255) {
        for (auto j=y; j<ey; ++j) for (auto i=x; i<ex; ++i) put(i,j,a);
    }
    RgbaView view() const { return {pixels.data(),pixels.size(),std::uint64_t(w)*4,w,h}; }
};
struct Fixture {
    Budget budget{Budget::FixedLimitForTest{},512*MiB}; JobWork work{25000000};
    Result<Regions> regions; Result<Partition> partition; Result<ContourSet> contours;
    void prepare(Image const &im) {
        regions=label(im.view(),alphaLut(0,0),budget,work); ASSERT_TRUE(regions.ok()) << regions.outcome.diagnostic;
        partition=enclose(regions.value,budget,work); ASSERT_TRUE(partition.ok()) << partition.outcome.diagnostic;
        contours=traceContours(im.view(),partition.value,budget,work); ASSERT_TRUE(contours.ok()) << contours.outcome.diagnostic;
    }
    Result<FittedContourSet> fit(double smoothing=50, Stop stop={}, FitOptions o={}) {
        return fitContours(contours.value,{smoothing},budget,work,stop,o);
    }
};
ContourPoint lerp(ContourPoint a, ContourPoint b, double t) { return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t}; }
ContourPoint evaluate(ContourPoint a, ContourSegment const &s, double t) {
    if (!s.cubic) return lerp(a,s.end,t);
    auto u=1-t;
    return {u*u*u*a.x+3*u*u*t*s.c1.x+3*u*t*t*s.c2.x+t*t*t*s.end.x,
            u*u*u*a.y+3*u*u*t*s.c1.y+3*u*t*t*s.c2.y+t*t*t*s.end.y};
}
double distance(ContourPoint p, ContourPoint a, ContourPoint b) {
    auto dx=b.x-a.x,dy=b.y-a.y,nn=dx*dx+dy*dy;
    auto t=nn ? std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/nn,0.,1.) : 0.;
    return std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy);
}
std::vector<ContourPoint> flatten(FittedContourSet const &f, unsigned ri, unsigned samples=256) {
    auto const &r=f.rings()[ri]; std::vector<ContourPoint> points{r.start}; auto a=r.start;
    for (auto i=r.begin; i<r.end; ++i) {
        auto const &s=f.segments()[i]; auto n=s.cubic ? samples : 1;
        for (unsigned j=1; j<=n; ++j) points.push_back(evaluate(a,s,double(j)/n));
        a=s.end;
    }
    return points;
}
double toPolyline(ContourPoint p, std::vector<ContourPoint> const &v) {
    double best=INFINITY;
    for (unsigned i=1; i<v.size(); ++i) best=std::min(best,distance(p,v[i-1],v[i]));
    return best;
}
// Independent symmetric dense sampling oracle, including input EDGE interiors.
void deviationOracle(ContourSet const &raw, FittedContourSet const &fit, double eps) {
    ASSERT_EQ(raw.ringCount,fit.ringCount);
    for (unsigned ri=0; ri<raw.ringCount; ++ri) {
        auto r=raw.rings()[ri]; std::vector<ContourPoint> original(raw.points()+r.begin,raw.points()+r.end);
        original.push_back(original.front()); auto output=flatten(fit,ri);
        auto const &fr=fit.rings()[ri];
        ASSERT_GE(fr.end-fr.begin,3u);
        EXPECT_DOUBLE_EQ(fit.segments()[fr.end-1].end.x,fr.start.x);
        EXPECT_DOUBLE_EQ(fit.segments()[fr.end-1].end.y,fr.start.y);
        for (unsigned j=0;j+1<original.size();++j) {
            auto a=original[(j+original.size()-2)%(original.size()-1)],b=original[j],c=original[j+1];
            auto ux=b.x-a.x,uy=b.y-a.y,vx=c.x-b.x,vy=c.y-b.y;
            auto angle=std::acos(std::clamp((ux*vx+uy*vy)/(std::hypot(ux,uy)*std::hypot(vx,vy)),-1.,1.));
            if (angle > std::acos(-1.)/4+1e-9) {
                bool found=fr.start.x==b.x && fr.start.y==b.y;
                for (auto k=fr.begin;k<fr.end;++k) found=found || (fit.segments()[k].end.x==b.x && fit.segments()[k].end.y==b.y);
                EXPECT_TRUE(found) << "corner ring=" << ri << " vertex=" << j;
            }
        }
        double worst=0;
        for (auto p:output) worst=std::max(worst,toPolyline(p,original));
        for (unsigned i=1; i<original.size(); ++i) for (unsigned k=0; k<=8; ++k)
            worst=std::max(worst,toPolyline(lerp(original[i-1],original[i],k/8.),output));
        EXPECT_LE(worst,eps+1e-9) << "ring=" << ri;
    }
}
long double orientation(ContourPoint a, ContourPoint b, ContourPoint c) {
    return (static_cast<long double>(b.x)-a.x)*(static_cast<long double>(c.y)-a.y)-
           (static_cast<long double>(b.y)-a.y)*(static_cast<long double>(c.x)-a.x);
}
bool hit(ContourPoint a, ContourPoint b, ContourPoint c, ContourPoint d) {
    if (std::max(a.x,b.x)<std::min(c.x,d.x) || std::max(c.x,d.x)<std::min(a.x,b.x) ||
        std::max(a.y,b.y)<std::min(c.y,d.y) || std::max(c.y,d.y)<std::min(a.y,b.y)) return false;
    auto x=orientation(a,b,c),y=orientation(a,b,d),u=orientation(c,d,a),v=orientation(c,d,b);
    return ((x<=0 && y>=0)||(x>=0 && y<=0)) && ((u<=0 && v>=0)||(u>=0 && v<=0));
}
void intersectionOracle(FittedContourSet const &fit) {
    for (unsigned pi=0; pi<fit.pieceCount; ++pi) {
        auto pc=fit.pieces()[pi];
        for (auto ri=pc.ringBegin; ri<pc.ringEnd; ++ri) {
            auto a=flatten(fit,ri,32);
            for (unsigned i=1; i<a.size(); ++i) for (auto j=i+2; j<a.size(); ++j) {
                if (i==1 && j+1==a.size()) continue;
                ASSERT_FALSE(hit(a[i-1],a[i],a[j-1],a[j])) << "self ring=" << ri;
            }
            for (auto rj=ri+1; rj<pc.ringEnd; ++rj) {
                auto b=flatten(fit,rj,32);
                for (unsigned i=1; i<a.size(); ++i) for (unsigned j=1; j<b.size(); ++j)
                    ASSERT_FALSE(hit(a[i-1],a[i],b[j-1],b[j])) << "pair=" << ri << ',' << rj;
            }
        }
    }
}
void empty(Result<FittedContourSet> const &r) {
    EXPECT_FALSE(r.ok()); EXPECT_EQ(r.value.segmentCount,0u); EXPECT_EQ(r.value.ringCount,0u); EXPECT_EQ(r.value.pieceCount,0u);
    EXPECT_EQ(r.value.anchorCount,0u); EXPECT_EQ(r.value.serializedBytes,0u);
    EXPECT_EQ(r.value.segments(),nullptr); EXPECT_EQ(r.value.rings(),nullptr); EXPECT_EQ(r.value.pieces(),nullptr);
}
Image disk(unsigned size=90, double radius=38) {
    Image im(size,size); auto centre=size/2.;
    for (unsigned y=0; y<size; ++y) for (unsigned x=0; x<size; ++x) {
        unsigned covered=0;
        for (unsigned j=0; j<8; ++j) for (unsigned i=0; i<8; ++i)
            covered+=std::hypot(x+(i+.5)/8-centre,y+(j+.5)/8-centre)<=radius;
        im.put(x,y,std::lround(covered*255./64));
    }
    return im;
}
Image star() {
    Image im(110,110); std::vector<ContourPoint> polygon;
    for (unsigned i=0; i<10; ++i) {
        auto t=i*std::acos(-1.)/5-std::acos(-1.)/2, r=i%2 ? 19. : 48.;
        polygon.push_back({55+r*std::cos(t),55+r*std::sin(t)});
    }
    auto inside=[&](double x,double y) {
        bool in=false;
        for (unsigned i=0,j=9;i<10;j=i++) {
            auto a=polygon[i],b=polygon[j];
            if ((a.y>y)!=(b.y>y) && x<(b.x-a.x)*(y-a.y)/(b.y-a.y)+a.x) in=!in;
        }
        return in;
    };
    for (unsigned y=0;y<im.h;++y) for (unsigned x=0;x<im.w;++x) {
        unsigned covered=0;
        for (unsigned j=0;j<8;++j) for (unsigned i=0;i<8;++i) covered+=inside(x+(i+.5)/8,y+(j+.5)/8);
        im.put(x,y,std::lround(covered*255./64));
    }
    return im;
}
// The public result has no per-span flags. A nonstraight raw span emitted as
// a line is a fallback; exact straight spans are ordinary line output.
unsigned fallbackSpans(ContourSet const &raw, FittedContourSet const &fit, unsigned ri) {
    auto const &r=raw.rings()[ri]; auto const &fr=fit.rings()[ri];
    auto same=[](ContourPoint a,ContourPoint b) { return a.x==b.x && a.y==b.y; };
    auto at=r.begin;
    while (at<r.end && !same(raw.points()[at],fr.start)) ++at;
    EXPECT_LT(at,r.end); if (at==r.end) return 0;
    unsigned fallbacks=0; auto start=fr.start;
    for (auto j=fr.begin;j<fr.end;++j) {
        auto const &segment=fit.segments()[j]; double worst=0; unsigned visited=0;
        do {
            at=at+1==r.end ? r.begin : at+1;
            worst=std::max(worst,distance(raw.points()[at],start,segment.end));
            ++visited;
        } while (!same(raw.points()[at],segment.end) && visited<=r.end-r.begin);
        EXPECT_LE(visited,r.end-r.begin);
        fallbacks+=!segment.cubic && worst>1e-12;
        start=segment.end;
    }
    return fallbacks;
}
struct SingleBadSpan {
    static inline unsigned calls=0, failAt=0;
    static inline bool stalled=false;
    static inline ContourPoint start{},end{};
    static int fit(ContourPoint *out,ContourPoint const *points,std::uint32_t n,double tolerance) {
        Geom::Point input[128],output[4];
        for (unsigned i=0;i<n;++i) input[i]={points[i].x,points[i].y};
        auto fitted=Geom::bezier_fit_cubic_full(output,nullptr,input,n,Geom::Point(0,0),Geom::Point(0,0),tolerance,1);
        if (fitted==1) for (unsigned i=0;i<4;++i) out[i]={output[i].x(),output[i].y()};
        if (++calls==failAt) {
            start=points[0]; end=points[n-1];
            if (stalled && fitted==1) { out[1]=start; return 1; }
            // Monotone, finite controls with correct endpoints, but far outside
            // tolerance: exercises the continuous deviation certificate.
            auto dx=end.x-start.x,dy=end.y-start.y;
            out[0]=start; out[3]=end;
            out[1]={start.x+dx/3-10*dy,start.y+dy/3+10*dx};
            out[2]={start.x+2*dx/3-10*dy,start.y+2*dy/3+10*dx};
            return 1;
        }
        return fitted;
    }
};

}
TEST(BitmapContourFit, DiskDeviationAtAllSmoothingLevels) {
    auto im=disk(); Fixture f; f.prepare(im); unsigned cubics=0;
    for (double s:{0.,50.,100.}) {
        SCOPED_TRACE(s); auto r=f.fit(s); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
        deviationOracle(f.contours.value,r.value,.05+.0195*s);
        for (unsigned i=0;i<r.value.segmentCount;++i) cubics+=r.value.segments()[i].cubic;
        EXPECT_LT(r.value.anchorCount,f.contours.value.pointCount);
    }
    EXPECT_GT(cubics,0u); // Exercises the linked fitter, not only line fallbacks.
}
TEST(BitmapContourFit, SmallRingsAreNeverDroppedAtHighSmoothing) {
    for (double radius:{1.25,1.75,2.25,2.75}) {
        auto im=disk(16,radius); Fixture f; f.prepare(im); ASSERT_EQ(f.contours.value.ringCount,1u);
        for (double s:{0.,50.,100.}) {
            auto r=f.fit(s); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
            ASSERT_EQ(r.value.ringCount,1u); EXPECT_GE(r.value.anchorCount,3u);
            deviationOracle(f.contours.value,r.value,.05+.0195*s); intersectionOracle(r.value);
        }
    }
}
TEST(BitmapContourFit, StarDeviationAtAllSmoothingLevels) {
    auto im=star(); Fixture f; f.prepare(im);
    for (double s:{0.,50.,100.}) {
        SCOPED_TRACE(s); auto r=f.fit(s); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
        deviationOracle(f.contours.value,r.value,.05+.0195*s); intersectionOracle(r.value);
    }
}
TEST(BitmapContourFit, SquareCornersAndStraightSides) {
    Image im(20,20); im.box(3,3,17,17); Fixture f; f.prepare(im);
    for (double s:{0.,50.,100.}) {
        auto r=f.fit(s); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
        ASSERT_EQ(r.value.ringCount,1u); EXPECT_EQ(r.value.anchorCount,8u);
        auto const &ring=r.value.rings()[0]; std::vector<ContourPoint> anchors{ring.start};
        for (auto i=ring.begin;i<ring.end;++i) { EXPECT_FALSE(r.value.segments()[i].cubic); anchors.push_back(r.value.segments()[i].end); }
        for (auto p:std::vector<ContourPoint>{{3.5,3},{16.5,3},{17,3.5},{17,16.5},{16.5,17},{3.5,17},{3,16.5},{3,3.5}})
            EXPECT_TRUE(std::any_of(anchors.begin(),anchors.end(),[&](auto a){return a.x==p.x && a.y==p.y;}));
        EXPECT_DOUBLE_EQ(ring.area,195.5); EXPECT_DOUBLE_EQ(ring.minX,3); EXPECT_DOUBLE_EQ(ring.maxX,17);
        EXPECT_DOUBLE_EQ(ring.minY,3); EXPECT_DOUBLE_EQ(ring.maxY,17);
    }
}
TEST(BitmapContourFit, DonutDotAndGlobalHierarchy) {
    Image im(90,44);
    for (unsigned x:{0u,45u}) {
        im.box(x+1,1,x+43,43); im.box(x+8,8,x+36,36,0); im.box(x+18,18,x+26,26);
    }
    Fixture f; f.prepare(im); ASSERT_EQ(f.contours.value.ringCount,6u);
    for (double s:{0.,50.,100.}) {
        auto r=f.fit(s); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic; ASSERT_EQ(r.value.ringCount,6u);
        for (unsigned i=0;i<6;++i) {
            auto a=f.contours.value.rings()[i]; auto b=r.value.rings()[i];
            EXPECT_EQ(b.depth,a.depth); EXPECT_EQ(b.parent,a.parent); EXPECT_EQ(b.area>0,a.area>0);
            EXPECT_EQ(b.depth,i%3); EXPECT_EQ(b.parent,i%3 ? i-1 : UINT32_MAX);
        }
        intersectionOracle(r.value);
    }
}
TEST(BitmapContourFit, CurvedDonutDotKeepsContainment) {
    Image im(100,100);
    for (unsigned y=0;y<im.h;++y) for (unsigned x=0;x<im.w;++x) {
        auto d=std::hypot(x+.5-50,y+.5-50);
        double signedDistance=std::max(std::min(42-d,d-25),9-d);
        im.put(x,y,std::lround(255*std::clamp(.5+signedDistance,0.,1.)));
    }
    Fixture f; f.prepare(im); ASSERT_EQ(f.contours.value.ringCount,3u);
    unsigned cubics=0;
    for (double s:{0.,50.,100.}) {
        auto r=f.fit(s); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
        for (unsigned i=0;i<3;++i) {
            EXPECT_EQ(r.value.rings()[i].depth,i); EXPECT_EQ(r.value.rings()[i].parent,i ? i-1 : UINT32_MAX);
        }
        for (unsigned i=0;i<r.value.segmentCount;++i) cubics+=r.value.segments()[i].cubic;
        deviationOracle(f.contours.value,r.value,.05+.0195*s); intersectionOracle(r.value);
    }
    EXPECT_GT(cubics,0u);
}
TEST(BitmapContourFit, InvalidFittedGeometryFallsBackWithoutDroppingRings) {
    auto im=disk(); Fixture f; f.prepare(im); FitOptions options;
    options.fitCubic=[](ContourPoint *out,ContourPoint const *points,std::uint32_t n,double) {
        out[0]=points[0]; out[3]=points[n-1];
        out[1]={points[0].x+100,points[0].y+100};
        out[2]={points[n-1].x-100,points[n-1].y-100};
        return 1;
    };
    auto r=f.fit(100,{},options); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    ASSERT_EQ(r.value.ringCount,1u); EXPECT_TRUE(r.value.rings()[0].fallback);
    for (unsigned i=0;i<r.value.segmentCount;++i) EXPECT_FALSE(r.value.segments()[i].cubic);
    deviationOracle(f.contours.value,r.value,2); intersectionOracle(r.value);
}
TEST(BitmapContourFit, LabyrinthHasNoIntersections) {
    Image im(86,86); im.box(1,1,85,85);
    // Deep alternating slits and separate enclosed holes challenge both self
    // separation and multiple-ring topology at the maximum tolerance.
    for (unsigned y=7;y<80;y+=12) im.box(y%24==7 ? 1 : 9,y,y%24==7 ? 77 : 85,y+3,0);
    for (unsigned y=12;y<78;y+=12) for (unsigned x=14;x<74;x+=12) im.box(x,y,x+4,y+3,0);
    Fixture f; f.prepare(im); ASSERT_GT(f.contours.value.ringCount,10u);
    for (double s:{0.,50.,100.}) {
        auto r=f.fit(s); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
        EXPECT_EQ(r.value.ringCount,f.contours.value.ringCount); intersectionOracle(r.value);
        for (unsigned i=0;i<r.value.ringCount;++i) { EXPECT_EQ(r.value.rings()[i].depth,f.contours.value.rings()[i].depth); EXPECT_EQ(r.value.rings()[i].parent,f.contours.value.rings()[i].parent); }
    }
}
TEST(BitmapContourFit, FitterFailureFallsBackOnlyItsRing) {
    auto im=disk(); im.box(1,1,4,4); Fixture f; f.prepare(im); ASSERT_EQ(f.contours.value.ringCount,2u);
    FitOptions o; o.fitCubic=[](ContourPoint *,ContourPoint const *,std::uint32_t,double) { return -1; };
    auto r=f.fit(50,{},o); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    unsigned fallbacks=0;
    for (unsigned i=0;i<r.value.ringCount;++i) {
        auto ring=r.value.rings()[i]; fallbacks+=ring.fallback;
        if (ring.fallback) for (auto j=ring.begin;j<ring.end;++j) EXPECT_FALSE(r.value.segments()[j].cubic);
    }
    EXPECT_EQ(fallbacks,1u); deviationOracle(f.contours.value,r.value,1.025);
}
TEST(BitmapContourFit, SingleBadSpanKeepsCertifiedCubics) {
    auto im=disk(); Fixture f; f.prepare(im); auto baseline=f.fit(100);
    ASSERT_TRUE(baseline.ok()); ASSERT_EQ(baseline.value.ringCount,1u);
    ASSERT_FALSE(baseline.value.rings()[0].fallback);
    FitOptions o; o.fitCubic=SingleBadSpan::fit;
    SingleBadSpan::calls=0; SingleBadSpan::failAt=3; SingleBadSpan::stalled=false;
    auto r=f.fit(100,{},o); ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    ASSERT_EQ(r.value.ringCount,1u); ASSERT_EQ(r.value.segmentCount,baseline.value.segmentCount);
    EXPECT_TRUE(r.value.rings()[0].fallback);
    unsigned cubics=0,changed=0; auto start=r.value.rings()[0].start;
    for (unsigned i=0;i<r.value.segmentCount;++i) {
        auto const &s=r.value.segments()[i]; auto const &original=baseline.value.segments()[i];
        cubics+=s.cubic;
        if (std::memcmp(&s,&original,sizeof(s))!=0) {
            ++changed; EXPECT_TRUE(original.cubic); EXPECT_FALSE(s.cubic);
            EXPECT_DOUBLE_EQ(start.x,SingleBadSpan::start.x); EXPECT_DOUBLE_EQ(start.y,SingleBadSpan::start.y);
            EXPECT_DOUBLE_EQ(s.end.x,SingleBadSpan::end.x); EXPECT_DOUBLE_EQ(s.end.y,SingleBadSpan::end.y);
            EXPECT_DOUBLE_EQ(s.end.x,original.end.x); EXPECT_DOUBLE_EQ(s.end.y,original.end.y);
            std::cout << "B3_SPAN_FALLBACK ring=0 span=" << i << " reason=deviation start="
                      << start.x << ',' << start.y << " end=" << s.end.x << ',' << s.end.y << '\n';
        }
        start=s.end;
    }
    EXPECT_EQ(changed,1u); EXPECT_EQ(fallbackSpans(f.contours.value,r.value,0),1u);
    EXPECT_GT(cubics,r.value.segmentCount/2);
    deviationOracle(f.contours.value,r.value,2); intersectionOracle(r.value);
    // Mixed outputs must remain byte-identical on repeated input and failure.
    SingleBadSpan::calls=0; auto repeat=f.fit(100,{},o); ASSERT_TRUE(repeat.ok());
    ASSERT_EQ(repeat.value.segmentCount,r.value.segmentCount);
    EXPECT_EQ(std::memcmp(repeat.value.segments(),r.value.segments(),r.value.segmentCount*sizeof(ContourSegment)),0);
    EXPECT_EQ(std::memcmp(repeat.value.rings(),r.value.rings(),sizeof(FittedRing)),0);
}
TEST(BitmapContourFit, UncertifiedAdjacentPairFallsBackOnlyItsSpans) {
    auto im=disk(32,8); Fixture f; f.prepare(im); auto baseline=f.fit(100);
    ASSERT_TRUE(baseline.ok()); ASSERT_EQ(baseline.value.ringCount,1u);
    ASSERT_FALSE(baseline.value.rings()[0].fallback);
    FitOptions o; o.fitCubic=SingleBadSpan::fit;
    SingleBadSpan::calls=0; SingleBadSpan::failAt=3; SingleBadSpan::stalled=true;
    auto r=f.fit(100,{},o); SingleBadSpan::stalled=false;
    ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    ASSERT_EQ(r.value.ringCount,1u); ASSERT_EQ(r.value.segmentCount,baseline.value.segmentCount);
    unsigned bad=UINT32_MAX; auto start=baseline.value.rings()[0].start;
    for (unsigned i=0;i<baseline.value.segmentCount;++i) {
        auto const &s=baseline.value.segments()[i];
        if (start.x==SingleBadSpan::start.x && start.y==SingleBadSpan::start.y &&
            s.end.x==SingleBadSpan::end.x && s.end.y==SingleBadSpan::end.y) bad=i;
        start=s.end;
    }
    ASSERT_NE(bad,UINT32_MAX); auto previous=(bad+r.value.segmentCount-1)%r.value.segmentCount;
    unsigned changed=0,cubics=0;
    for (unsigned i=0;i<r.value.segmentCount;++i) {
        auto const &s=r.value.segments()[i]; auto const &original=baseline.value.segments()[i];
        cubics+=s.cubic;
        if (std::memcmp(&s,&original,sizeof(s))!=0) {
            ++changed; EXPECT_TRUE(i==bad || i==previous); EXPECT_TRUE(original.cubic); EXPECT_FALSE(s.cubic);
            std::cout << "B3_SPAN_FALLBACK ring=0 span=" << i << " reason=intersection_certificate\n";
        }
    }
    // The stalled derivative remains monotone and within epsilon, but makes
    // the strict adjacent separating-plane certificate unresolved. Both pair
    // members fall back; all other spans survive the whole-ring recheck.
    EXPECT_TRUE(r.value.rings()[0].fallback); EXPECT_EQ(changed,2u);
    EXPECT_EQ(fallbackSpans(f.contours.value,r.value,0),2u);
    EXPECT_GT(cubics,r.value.segmentCount/2);
    deviationOracle(f.contours.value,r.value,2); intersectionOracle(r.value);
}
TEST(BitmapContourFit, OutputCapsRefuseWithoutPartialResults) {
    auto im=disk(); Fixture f; f.prepare(im); auto baseline=f.budget.reserved();
    for (unsigned type=0;type<3;++type) {
        FitOptions o;
        if (type==0) o.maxAnchors=3; if (type==1) o.maxAnchorsPerPiece=3; if (type==2) o.maxSerializedBytes=64;
        auto r=f.fit(50,{},o); empty(r); EXPECT_EQ(r.outcome.status,Status::failed); EXPECT_EQ(f.budget.reserved(),baseline);
    }
    FitOptions high; high.maxAnchors=UINT32_MAX; high.maxAnchorsPerPiece=UINT32_MAX; high.maxSerializedBytes=UINT64_MAX;
    auto a=f.fit(),b=f.fit(50,{},high); ASSERT_TRUE(a.ok()); ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value.anchorCount,b.value.anchorCount); EXPECT_EQ(a.value.serializedBytes,b.value.serializedBytes);
}
TEST(BitmapContourFit, DeterminismMoveAndAccounting) {
    static_assert(!std::is_copy_constructible_v<FittedContourSet>);
    auto im=disk(); Fixture f; f.prepare(im); auto before=f.work.visits(); auto a=f.fit();
    ASSERT_TRUE(a.ok()); EXPECT_EQ(a.consumed,f.work.visits()-before); auto b=f.fit(); ASSERT_TRUE(b.ok());
    ASSERT_EQ(a.value.segmentCount,b.value.segmentCount); ASSERT_EQ(a.value.ringCount,b.value.ringCount);
    EXPECT_EQ(std::memcmp(a.value.segments(),b.value.segments(),a.value.segmentCount*sizeof(ContourSegment)),0);
    EXPECT_EQ(std::memcmp(a.value.rings(),b.value.rings(),a.value.ringCount*sizeof(FittedRing)),0);
    EXPECT_EQ(std::memcmp(a.value.pieces(),b.value.pieces(),a.value.pieceCount*sizeof(FittedPiece)),0);
    EXPECT_EQ(a.value.anchorCount,a.value.segmentCount);
    auto bytes=52*a.value.ringCount;
    for (unsigned i=0;i<a.value.segmentCount;++i) bytes+=a.value.segments()[i].cubic ? 152 : 52;
    EXPECT_EQ(a.value.serializedBytes,bytes);
    auto moved=std::move(a.value); EXPECT_EQ(a.value.segments(),nullptr); EXPECT_EQ(a.value.anchorCount,0u);
    a.value=std::move(moved); EXPECT_EQ(moved.ringCount,0u);
}
TEST(BitmapContourFit, StopAtEveryPhaseRollsBack) {
    auto im=disk(); Fixture f; f.prepare(im); auto baseline=f.budget.reserved();
    struct Hook { FitPhase phase; std::shared_ptr<std::atomic<bool>> flag; };
    for (auto phase:{FitPhase::simplify,FitPhase::fitting,FitPhase::validation,FitPhase::output,FitPhase::allocation,FitPhase::fitterReservation}) {
        Hook hook{phase,std::make_shared<std::atomic<bool>>(false)}; FitOptions o; o.observerData=&hook;
        o.observe=[](FitPhase p,void *v) noexcept { auto &h=*static_cast<Hook *>(v); if (h.phase==p) h.flag->store(true); };
        auto r=f.fit(50,Stop(hook.flag),o); empty(r); EXPECT_EQ(r.outcome.status,Status::canceled); EXPECT_EQ(f.budget.reserved(),baseline);
    }
    auto flag=std::make_shared<std::atomic<bool>>(true); auto r=f.fit(50,Stop(flag)); empty(r); EXPECT_EQ(r.outcome.status,Status::canceled);
}
TEST(BitmapContourFit, BudgetAndEveryAllocationFaultRollBack) {
    auto im=disk(); Fixture f; f.prepare(im);
    Budget tiny(Budget::FixedLimitForTest{},1); JobWork w(10000);
    auto r=fitContours(f.contours.value,{},tiny,w); empty(r); EXPECT_EQ(r.outcome.status,Status::failed); EXPECT_EQ(tiny.reserved(),0u);
    auto baseline=f.budget.reserved(); AllocationFault counter; FitOptions o; o.fault=&counter;
    { auto good=f.fit(50,{},o); ASSERT_TRUE(good.ok()); }
    auto attempts=counter.attempts; ASSERT_GT(attempts,10u);
    for (std::uint64_t i=1;i<=attempts;++i) {
        SCOPED_TRACE(i); AllocationFault fault{i,0}; o.fault=&fault;
        auto failed=f.fit(50,{},o); empty(failed); EXPECT_EQ(failed.outcome.status,Status::failed); EXPECT_EQ(f.budget.reserved(),baseline);
    }
}
TEST(BitmapContourFit, ExceptionsAreTypedRefusalsAndFitterReservationIsLive) {
    auto im=disk(); Fixture f; f.prepare(im); auto baseline=f.budget.reserved();
    struct Probe { Budget *budget; std::uint64_t peak; } probe{&f.budget,baseline}; FitOptions o; o.observerData=&probe;
    o.observe=[](FitPhase phase,void *v) noexcept {
        auto &p=*static_cast<Probe *>(v); if (phase==FitPhase::fitterReservation) p.peak=std::max(p.peak,p.budget->reserved());
    };
    for (bool oom:{false,true}) {
        o.fitCubic=oom ? +[](ContourPoint *,ContourPoint const *,std::uint32_t,double)->int { throw std::bad_alloc(); }
                      : +[](ContourPoint *,ContourPoint const *,std::uint32_t,double)->int { throw 7; };
        auto r=f.fit(50,{},o); empty(r); EXPECT_EQ(r.outcome.status,Status::failed); EXPECT_EQ(f.budget.reserved(),baseline);
        EXPECT_NE(std::strstr(r.outcome.diagnostic,oom ? "allocation" : "exception"),nullptr);
    }
    EXPECT_GT(probe.peak,baseline);
}
TEST(BitmapContourFit, VisitRefusalAndInvalidSettings) {
    auto im=disk(); Fixture f; f.prepare(im); auto baseline=f.budget.reserved(); JobWork exhausted(1);
    ASSERT_TRUE(exhausted.advance(exhausted.limit()).ok());
    auto r=fitContours(f.contours.value,{},f.budget,exhausted); empty(r); EXPECT_EQ(r.outcome.status,Status::failed);
    for (double s:{-1.,101.,double(INFINITY),std::nan("")}) { auto invalid=f.fit(s); empty(invalid); }
    EXPECT_EQ(f.budget.reserved(),baseline);
}
TEST(BitmapContourFit, NoContourPiecesStayPresent) {
    Image im(30,10); im.box(0,0,10,10,127); im.box(20,0,30,10,128); Fixture f; f.prepare(im);
    auto r=f.fit(); ASSERT_TRUE(r.ok()); ASSERT_EQ(r.value.pieceCount,2u);
    EXPECT_TRUE(r.value.pieces()[0].noContour); EXPECT_EQ(r.value.pieces()[0].ringBegin,r.value.pieces()[0].ringEnd);
    EXPECT_FALSE(r.value.pieces()[1].noContour); EXPECT_EQ(r.value.ringCount,1u);
}
TEST(BitmapContourFit, MeasureLimits5000) {
    auto root=std::getenv("VACARDS_EB_B3_MEASURE_ROOT");
    if (!root) GTEST_SKIP() << "Opt in with VACARDS_EB_B3_MEASURE_ROOT=.../limits-5000";
    for (auto suffix:{"/sparse/blobs-150-5000x5000.png","/blobs-150-5000x5000.png"}) {
        auto path=std::string(root)+suffix; png_image png{}; png.version=PNG_IMAGE_VERSION;
        ASSERT_TRUE(png_image_begin_read_from_file(&png,path.c_str())) << png.message;
        png.format=PNG_FORMAT_RGBA; Image im(png.width,png.height);
        auto decoded=png_image_finish_read(&png,nullptr,im.pixels.data(),0,nullptr);
        auto diagnostic=std::string(png.message); png_image_free(&png); ASSERT_TRUE(decoded) << diagnostic;
        Fixture f; Budget::Token input; ASSERT_TRUE(f.budget.acquire(Stage::input,im.pixels.size(),input).ok());
        f.regions=label(im.view(),alphaLut(0,0),f.budget,f.work); ASSERT_TRUE(f.regions.ok()) << f.regions.outcome.diagnostic;
        f.partition=enclose(f.regions.value,f.budget,f.work); ASSERT_TRUE(f.partition.ok()) << f.partition.outcome.diagnostic;
        auto metric=OrthogonalMetric::fromDpi(300,300); ASSERT_TRUE(metric.ok());
        auto attached=attach(f.partition.value,metric.value,f.budget,f.work); ASSERT_TRUE(attached.ok()) << attached.outcome.diagnostic;
        f.contours=traceContours(im.view(),attached.value,f.budget,f.work); ASSERT_TRUE(f.contours.ok()) << f.contours.outcome.diagnostic;
        auto visits=f.work.visits();
        for (double s:{0.,50.,100.}) {
            // Alternative smoothing settings are distinct jobs. Keep the same
            // B1 preprocessing charge in each meter; never reset within a job.
            JobWork work(25000000); ASSERT_TRUE(work.advance(visits).ok());
            auto start=std::chrono::steady_clock::now(); auto r=fitContours(f.contours.value,{s},f.budget,work);
            auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            unsigned fallbacks=0,wholeRings=0,spans=0,cubics=0;
            for (unsigned i=0;i<r.value.ringCount;++i) {
                auto const &ring=r.value.rings()[i]; unsigned ringCubics=0;
                for (auto j=ring.begin;j<ring.end;++j) ringCubics+=r.value.segments()[j].cubic;
                fallbacks+=ring.fallback; wholeRings+=ring.fallback && !ringCubics;
                cubics+=ringCubics; spans+=fallbackSpans(f.contours.value,r.value,i);
            }
            ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
            EXPECT_EQ(r.value.ringCount,f.contours.value.ringCount);
            EXPECT_EQ(r.value.pieceCount,150u);
            std::cout << "B3_MEASURE " << suffix << " smoothing=" << s << " seconds=" << seconds
                      << " raw_points=" << f.contours.value.pointCount << " raw_rings=" << f.contours.value.ringCount
                      << " anchors=" << r.value.anchorCount << " serialized_bytes=" << r.value.serializedBytes
                      << " fallback_rings=" << fallbacks << " whole_line_fallback_rings=" << wholeRings
                      << " partial_fallback_rings=" << fallbacks-wholeRings << " fallback_spans=" << spans
                      << " cubic_spans=" << cubics << " visits=" << r.consumed
                      << " outcome=" << (r.ok() ? "success" : "refused") << " diagnostic=" << r.outcome.diagnostic << '\n';
        }
    }
}
