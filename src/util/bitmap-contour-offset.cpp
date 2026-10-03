// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/bitmap-contour-offset.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Inkscape::Bitmap {
namespace {
struct Meter {
    Budget &budget; JobWork &work; Stop stop; ContourOptions options;
    PhaseTimer timer{};
    Outcome outcome{Status::unchanged, ""};
    std::uint64_t pending = 0, peak = 0;
    bool flush() noexcept {
        outcome = work.advance(pending); pending = 0;
        auto t = timer.check(stop); if (!t.ok()) outcome = t;
        return outcome.ok();
    }
    bool tick() noexcept { return ++pending < 128 || flush(); }
    bool fail(char const *s) noexcept { outcome = {Status::failed, s}; return false; }
    bool alloc(PlainBuffer &b, std::uint64_t n, std::size_t size) noexcept {
        outcome = b.allocate(budget, Stage::topology, n, size, options.fault, stop);
        if (!outcome.ok()) return false;
        peak = std::max(peak, budget.reserved());
        if (options.observe) options.observe(ContourPhase::allocation, options.observerData);
        return flush();
    }
};
template<class T> T *data(PlainBuffer &b) { return reinterpret_cast<T *>(b.data()); }
struct Segment { double ax, ay, bx, by; };
struct Node { double lx, ly, hx, hy; std::uint32_t begin, end, left, right; };
// Balanced, immutable BVH over contour order. No unmetered library sorting,
// recursion is bounded by log2(2M) and every node/segment visit is charged.
struct Index {
    PlainBuffer segments, nodes;
    Segment *s = nullptr; Node *n = nullptr; std::uint32_t used = 0;
    Meter &m;
    std::uint32_t build(std::uint32_t lo, std::uint32_t hi) noexcept {
        auto id = used++; auto &v = n[id]; v.begin = lo; v.end = hi;
        v.lx = v.ly = std::numeric_limits<double>::infinity(); v.hx = v.hy = -v.lx;
        if (hi - lo <= 8) {
            v.left = v.right = UINT32_MAX;
            for (auto i = lo; i < hi; ++i) {
                if (!m.tick()) return id;
                v.lx = std::min({v.lx,s[i].ax,s[i].bx}); v.ly = std::min({v.ly,s[i].ay,s[i].by});
                v.hx = std::max({v.hx,s[i].ax,s[i].bx}); v.hy = std::max({v.hy,s[i].ay,s[i].by});
            }
        } else {
            v.left = build(lo, lo + (hi-lo)/2); if (!m.outcome.ok()) return id;
            v.right = build(lo + (hi-lo)/2, hi); if (!m.outcome.ok()) return id;
            auto a = n[v.left], b = n[v.right];
            v.lx = std::min(a.lx,b.lx); v.ly = std::min(a.ly,b.ly);
            v.hx = std::max(a.hx,b.hx); v.hy = std::max(a.hy,b.hy);
        }
        m.tick(); return id;
    }
    bool inside(std::uint32_t id, double x, double y, bool &parity) noexcept {
        if (!m.tick()) return false;
        auto const &v = n[id];
        if (y < v.ly || y >= v.hy || x >= v.hx) return true;
        if (v.left != UINT32_MAX)
            return inside(v.left,x,y,parity) && inside(v.right,x,y,parity);
        for (auto i=v.begin; i<v.end; ++i) {
            if (!m.tick()) return false;
            auto const &a=s[i];
            if ((a.ay>y)!=(a.by>y) && x < a.ax+(y-a.ay)*(a.bx-a.ax)/(a.by-a.ay)) parity=!parity;
        }
        return true;
    }
    bool value(double x, double y, double offset, double &v) noexcept {
        double best=std::numeric_limits<double>::infinity(); bool parity=false;
        if (!nearest(0,x,y,best) || !inside(0,x,y,parity)) return false;
        v=(parity ? 1 : -1)*std::sqrt(best)+offset;
        return true;
    }
    static double bound(Node const &v, double x, double y) noexcept {
        auto dx = std::max({v.lx-x,0.,x-v.hx}), dy = std::max({v.ly-y,0.,y-v.hy});
        return dx*dx+dy*dy;
    }
    bool nearest(std::uint32_t id, double x, double y, double &best) noexcept {
        if (!m.tick()) return false;
        auto const &v = n[id]; if (bound(v,x,y) > best) return true;
        if (v.left == UINT32_MAX) {
            for (auto i=v.begin; i<v.end; ++i) {
                if (!m.tick()) return false;
                auto const &a=s[i]; auto dx=a.bx-a.ax, dy=a.by-a.ay;
                auto den=dx*dx+dy*dy;
                auto t=den ? std::clamp(((x-a.ax)*dx+(y-a.ay)*dy)/den,0.,1.) : 0.;
                auto ex=x-a.ax-t*dx, ey=y-a.ay-t*dy; best=std::min(best,ex*ex+ey*ey);
            }
            return true;
        }
        auto a=v.left,b=v.right;
        if (bound(n[b],x,y)<bound(n[a],x,y)) std::swap(a,b);
        return nearest(a,x,y,best) && nearest(b,x,y,best);
    }
};
// Felzenszwalb-Huttenlocher lower envelope of parabolas, in physical units.
bool edt(float *f, std::uint32_t count, std::uint32_t stride, double spacing,
         double *input, double *z, std::uint32_t *v, Meter &m) noexcept {
    for (std::uint32_t q=0;q<count;++q) { if (!m.tick()) return false; input[q]=f[std::uint64_t(q)*stride]; }
    int k=-1; auto a=spacing*spacing;
    for (std::uint32_t q=0;q<count;++q) {
        if (!m.tick()) return false;
        if (!std::isfinite(input[q])) continue;
        double cross=-std::numeric_limits<double>::infinity();
        while (k>=0) {
            if (!m.tick()) return false;
            auto p=v[k]; cross=((input[q]-input[p])/a+double(q)*q-double(p)*p)/(2.*(q-p));
            if (cross>z[k]) break; --k;
        }
        ++k; v[k]=q; z[k]=k ? cross : -std::numeric_limits<double>::infinity();
        z[k+1]=std::numeric_limits<double>::infinity();
    }
    if (k<0) return true;
    int at=0;
    for (std::uint32_t q=0;q<count;++q) {
        if (!m.tick()) return false;
        while (z[at+1]<q) { if (!m.tick()) return false; ++at; }
        auto d=double(q)-v[at]; f[std::uint64_t(q)*stride]=float(a*d*d+input[v[at]]);
    }
    return true;
}
Result<DistanceField> distance(RingSet const &rings, double ox, double oy, std::uint32_t w,
    std::uint32_t h, double sx, double sy, double level, bool all, Meter &m, Index *retained = nullptr) noexcept {
    auto start=m.work.visits(); DistanceField out;
    auto fail=[&](Outcome o) { m.flush(); Result<DistanceField> r; r.outcome=o; r.consumed=m.work.visits()-start; return r; };
    auto refuse=[&](char const *s) { m.fail(s); return fail(m.outcome); };
    if (!m.flush()) return fail(m.outcome);
    if (!w || !h || !std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(sx) || !std::isfinite(sy) ||
        sx<=0 || sy<=0 || !std::isfinite(sx*sx) || !std::isfinite(sy*sy) ||
        sx*sx<std::numeric_limits<float>::min() || sy*sy<std::numeric_limits<float>::min() ||
        sx*double(w)>std::sqrt(double(std::numeric_limits<float>::max())/4) ||
        sy*double(h)>std::sqrt(double(std::numeric_limits<float>::max())/4) || !std::isfinite(level) || rings.pointCount>2000000 || rings.ringCount>50000 ||
        (rings.ringCount && (!rings.points() || !rings.rings()))) return refuse("Invalid distance input");
    auto cells=std::uint64_t(w)*h;
    PlainBuffer signs, scratch;
    if (!m.alloc(out.storage,cells,sizeof(float)) || !m.alloc(signs,cells,1)) return fail(m.outcome);
    auto f=data<float>(out.storage); auto mask=data<std::uint8_t>(signs);
    for (std::uint64_t i=0;i<cells;++i) { if (!m.tick()) return fail(m.outcome); mask[i]=0; }
    Index temporary{{},{},nullptr,nullptr,0,m};
    auto &index=retained ? *retained : temporary;
    std::uint32_t leaves=1;
    while (leaves<(rings.pointCount+7)/8) { if (!m.tick()) return fail(m.outcome); leaves*=2; }
    if (!m.alloc(index.segments,rings.pointCount,sizeof(Segment)) ||
        !m.alloc(index.nodes,std::uint64_t(leaves)*2-1,sizeof(Node))) return fail(m.outcome);
    index.s=data<Segment>(index.segments); index.n=data<Node>(index.nodes);
    std::uint32_t count=0;
    for (std::uint32_t r=0;r<rings.ringCount;++r) {
        auto ring=rings.rings()[r];
        if (ring.end>rings.pointCount || ring.end<=ring.begin) return refuse("Invalid distance ring");
        for (auto i=ring.begin,j=ring.end-1;i<ring.end;j=i++) {
            if (!m.tick()) return fail(m.outcome);
            if (count>=rings.pointCount) return refuse("Invalid distance ring spans");
            auto a=rings.points()[j],b=rings.points()[i];
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) return refuse("Nonfinite distance segment");
            index.s[count++]={a.x*sx,a.y*sy,b.x*sx,b.y*sy};
            // Even-odd scan conversion at sample centres; half-open y avoids
            // double toggles at vertices. No per-pixel point-in-polygon scan.
            if (a.y>b.y) std::swap(a,b);
            auto lo=std::clamp(std::ceil(a.y-oy-.5),0.,double(h));
            auto hi=std::clamp(std::ceil(b.y-oy-.5),0.,double(h));
            for (auto y=std::uint32_t(lo);y<std::uint32_t(hi);++y) {
                if (!m.tick()) return fail(m.outcome);
                auto x=a.x+(oy+y+.5-a.y)*(b.x-a.x)/(b.y-a.y);
                auto ix=std::clamp(std::ceil(x-ox-.5),0.,double(w));
                if (ix<w) mask[std::uint64_t(y)*w+std::uint32_t(ix)]^=1;
            }
        }
    }
    for (std::uint32_t y=0;y<h;++y) {
        std::uint8_t inside=0;
        for (std::uint32_t x=0;x<w;++x) { if (!m.tick()) return fail(m.outcome); auto i=std::uint64_t(y)*w+x; inside^=mask[i]; mask[i]=inside; }
    }
    if (count) { index.build(0,count); if (!m.outcome.ok()) return fail(m.outcome); }
    for (std::uint32_t y=0;y<h;++y) for (std::uint32_t x=0;x<w;++x) {
        if (!m.tick()) return fail(m.outcome); auto i=std::uint64_t(y)*w+x;
        bool boundary=(x && mask[i]!=mask[i-1]) || (x+1<w && mask[i]!=mask[i+1]) ||
            (y && mask[i]!=mask[i-w]) || (y+1<h && mask[i]!=mask[i+w]);
        f[i]=boundary ? 0 : std::numeric_limits<float>::infinity();
    }
    auto line=std::max(w,h);
    if (!m.alloc(scratch,std::uint64_t(line)+1,2*sizeof(double)+sizeof(std::uint32_t))) return fail(m.outcome);
    auto input=data<double>(scratch), z=input+line+1;
    auto v=reinterpret_cast<std::uint32_t *>(z+line+1);
    for (std::uint32_t y=0;y<h;++y) if (!edt(f+std::uint64_t(y)*w,w,1,sx,input,z,v,m)) return fail(m.outcome);
    for (std::uint32_t x=0;x<w;++x) if (!edt(f+x,h,w,sy,input,z,v,m)) return fail(m.outcome);
    scratch.reset();
    for (std::uint64_t i=0;i<cells;++i) {
        if (!m.tick()) return fail(m.outcome);
        // Empty rings have no finite distance; keep a finite negative field
        // for traceLevel. Otherwise the broad field is within two max-axis px.
        auto d=std::isfinite(f[i]) ? std::sqrt(double(f[i])) : double(std::numeric_limits<float>::max())/4;
        f[i]=float(mask[i] ? d : -d);
    }
    auto band=2*std::max(sx,sy);
    for (std::uint32_t y=0;y+1<h;++y) for (std::uint32_t x=0;x+1<w;++x) {
        if (!m.tick()) return fail(m.outcome); auto i=std::uint64_t(y)*w+x;
        auto lo=std::min({f[i],f[i+1],f[i+w],f[i+w+1]});
        auto hi=std::max({f[i],f[i+1],f[i+w],f[i+w+1]});
        if (all || (lo<=level+band && hi>=level-band)) {
            mask[i]|=2; mask[i+1]|=2; mask[i+w]|=2; mask[i+w+1]|=2;
        }
    }
    for (std::uint32_t y=0;y<h;++y) for (std::uint32_t x=0;x<w;++x) {
        if (!m.tick()) return fail(m.outcome); auto i=std::uint64_t(y)*w+x;
        if (count && (all || (mask[i]&2))) {
            auto best=std::numeric_limits<double>::infinity();
            if (!index.nearest(0,(ox+x+.5)*sx,(oy+y+.5)*sy,best)) return fail(m.outcome);
            auto d=std::sqrt(best); if (!std::isfinite(d) || d>std::numeric_limits<float>::max()/2) return refuse("Distance overflow");
            f[i]=float((mask[i]&1) ? d : -d);
        }
    }
    if (!m.flush()) return fail(m.outcome);
    out.field={f,w,h,ox,oy}; out.peakBudgetBytes=m.peak;
    Result<DistanceField> result; result.outcome={Status::changed,""}; result.consumed=m.work.visits()-start;
    result.value=std::move(out); return result;
}
// traceLevel supplies the edge graph and hierarchy. Replace each interpolated
// lattice-edge crossing by a root of the actual polygon distance, so even a
// subpixel offset converges to the original chamfers instead of a sampled SDF.
bool refineCrossings(RingSet &rings, FieldView field, double sx, double sy,
                     double offset, Index &index, Meter &m) noexcept {
    auto points=const_cast<ContourPoint *>(rings.points());
    for (std::uint32_t i=0;i<rings.pointCount;++i) {
        if (!m.tick()) return false;
        auto &p=points[i];
        auto gx=p.x-field.originX-.5, gy=p.y-field.originY-.5;
        // The fixed lattice coordinate is exact. A near-vertex horizontal
        // crossing must not be mistaken for a vertical edge.
        bool vertical=gx==std::round(gx);
        double lo=std::floor(vertical ? gy : gx), hi=lo+1;
        double fixed=vertical ? std::round(gx) : std::round(gy);
        auto sample=[&](double t,double &v) {
            double x=field.originX+.5+(vertical ? fixed : t);
            double y=field.originY+.5+(vertical ? t : fixed);
            return index.value(x*sx,y*sy,offset,v);
        };
        double a,b;
        if (!sample(lo,a) || !sample(hi,b)) return false;
        // A lattice vertex at the target level can belong to either edge.
        // Float sampling can also create an endpoint within float roundoff.
        double t=vertical ? gy : gx;
        if (std::abs(a)<1e-12*std::min(sx,sy)) t=lo;
        else if (std::abs(b)<1e-12*std::min(sx,sy)) t=hi;
        else if ((a>0)!=(b>0)) {
            for (unsigned iteration=0;iteration<48;++iteration) {
                if (!m.tick()) return false;
                t=lo+(hi-lo)*a/(a-b);
                // Safeguard the secant only after its first accurate attempt.
                if (iteration && (t<lo+.05*(hi-lo) || t>hi-.05*(hi-lo))) t=(lo+hi)/2;
                double v; if (!sample(t,v)) return false;
                if (std::abs(v)<1e-10*std::min(sx,sy) || hi-lo<1e-9) break;
                if ((v>0)==(a>0)) { lo=t; a=v; } else { hi=t; b=v; }
            }
        } else return m.fail("Offset crossing is not bracketed");
        if (vertical) { p.x=field.originX+.5+fixed; p.y=field.originY+.5+t; }
        else { p.x=field.originX+.5+t; p.y=field.originY+.5+fixed; }
    }
    auto records=const_cast<ContourRing *>(rings.rings());
    for (std::uint32_t k=0;k<rings.ringCount;++k) {
        auto &r=records[k]; auto origin=points[r.begin];
        r.area=0; r.minX=r.maxX=origin.x; r.minY=r.maxY=origin.y;
        for (auto i=r.begin,j=r.end-1;i<r.end;j=i++) {
            if (!m.tick()) return false;
            auto a=points[j],b=points[i];
            r.area+=((a.x-origin.x)*(b.y-origin.y)-(b.x-origin.x)*(a.y-origin.y))*.5;
            r.minX=std::min(r.minX,b.x); r.maxX=std::max(r.maxX,b.x);
            r.minY=std::min(r.minY,b.y); r.maxY=std::max(r.maxY,b.y);
        }
    }
    return m.flush();
}
template<class T> bool appendStorage(PlainBuffer &b, std::uint32_t used, std::uint32_t needed, Meter &m) noexcept {
    if (needed<=b.size()/sizeof(T)) return true;
    PlainBuffer next;
    if (!m.alloc(next,std::max<std::uint64_t>(needed,std::uint64_t(used)*2),sizeof(T))) return false;
    for (std::uint32_t i=0;i<used;++i) { if (!m.tick()) return false; data<T>(next)[i]=data<T>(b)[i]; }
    b=std::move(next); return true;
}
} // namespace
Result<DistanceField> signedDistance(RingSet const &r,double x,double y,std::uint32_t w,std::uint32_t h,
    double sx,double sy,Budget &b,JobWork &work,Stop stop) noexcept {
    Meter m{b,work,stop,{}}; return distance(r,x,y,w,h,sx,sy,0,true,m);
}
Result<ContourSet> offsetContours(RgbaView rgba, Partition const &partition, OffsetSettings settings,
    Budget &b, JobWork &work, Stop stop, ContourOptions options) noexcept {
    Meter m{b,work,stop,options}; auto start=work.visits(); ContourSet out;
    auto fail=[&](Outcome o) { m.flush(); Result<ContourSet> r; r.outcome=o; r.consumed=work.visits()-start; return r; };
    auto refuse=[&](char const *s) { m.fail(s); return fail(m.outcome); };
    if (!m.flush()) return fail(m.outcome);
    auto valid=rgba.validate(); if (!valid.ok()) return fail(valid);
    if (!std::isfinite(settings.offsetMm) || !std::isfinite(settings.gapToleranceMm) || settings.gapToleranceMm<0 ||
        !std::isfinite(settings.dpiX) || !std::isfinite(settings.dpiY) || settings.dpiX<=0 || settings.dpiY<=0 ||
        partition.width!=rgba.width || partition.height!=rgba.height || !partition.pieceOffsets() ||
        (partition.pieceCount && !partition.pieces())) return refuse("Invalid offset settings or partition");
    if (settings.offsetMm==0 && settings.gapToleranceMm==0)
        return traceContours(rgba,partition,b,work,stop,options);
    auto sx=25.4/settings.dpiX, sy=25.4/settings.dpiY, radius=settings.gapToleranceMm/2;
    auto px=2+std::ceil((radius+std::max(0.,settings.offsetMm))/sx);
    auto py=2+std::ceil((radius+std::max(0.,settings.offsetMm))/sy);
    if (!std::isfinite(px) || !std::isfinite(py) || px>UINT32_MAX/2 || py>UINT32_MAX/2) return refuse("Offset padding overflow");
    auto pointCap=std::min(options.maxPoints,8000000u), ringCap=std::min(options.maxRings,50000u);
    if (!m.alloc(out._pieces,partition.pieceCount,sizeof(PieceContour))) return fail(m.outcome);
    for (std::uint32_t p=0;p<partition.pieceCount;++p) {
        if (!m.flush()) return fail(m.outcome);
        auto support=pieceSupportField(rgba,partition,p,b,work,stop,options);
        if (!support.ok()) return fail(support.outcome);
        m.peak=std::max(m.peak,support.value.peakBudgetBytes);
        auto local=options; local.maxPoints=std::min(2000000u,pointCap-out.pointCount); local.maxRings=ringCap-out.ringCount;
        auto rings=traceLevel(support.value.field,b,work,stop,local);
        if (!rings.ok()) return fail(rings.outcome);
        m.peak=std::max(m.peak,rings.value.peakBudgetBytes); support.value.storage.reset();
        auto piece=partition.pieces()[p]; std::uint32_t w,h;
        if (!checked32(std::uint64_t(piece.endX-piece.x)+2*std::uint64_t(px),w) ||
            !checked32(std::uint64_t(piece.endY-piece.y)+2*std::uint64_t(py),h)) return refuse("Offset box overflow");
        double steps[3]={radius,-radius,settings.offsetMm};
        for (auto offset:steps) {
            if (!offset || !rings.value.ringCount) continue;
            Index index{{},{},nullptr,nullptr,0,m};
            auto field=distance(rings.value,piece.x-px,piece.y-py,w,h,sx,sy,-offset,false,m,&index);
            if (!field.ok()) return fail(field.outcome);
            auto values=data<float>(field.value.storage);
            for (std::uint64_t i=0;i<std::uint64_t(w)*h;++i) {
                if (!m.tick()) return fail(m.outcome); values[i]=float(double(values[i])+offset);
            }
            if (!m.flush()) return fail(m.outcome);
            auto next=traceLevel(field.value.field,b,work,stop,local);
            if (!next.ok()) return fail(next.outcome);
            m.peak=std::max(m.peak,next.value.peakBudgetBytes);
            if (!refineCrossings(next.value,field.value.field,sx,sy,offset,index,m)) return fail(m.outcome);
            rings=std::move(next);
        }
        auto const &r=rings.value;
        if (!appendStorage<ContourPoint>(out._points,out.pointCount,out.pointCount+r.pointCount,m) ||
            !appendStorage<ContourRing>(out._rings,out.ringCount,out.ringCount+r.ringCount,m)) return fail(m.outcome);
        for (std::uint32_t i=0;i<r.pointCount;++i) { if (!m.tick()) return fail(m.outcome); data<ContourPoint>(out._points)[out.pointCount+i]=r.points()[i]; }
        for (std::uint32_t i=0;i<r.ringCount;++i) {
            if (!m.tick()) return fail(m.outcome); auto ring=r.rings()[i];
            ring.begin+=out.pointCount; ring.end+=out.pointCount;
            if (ring.parent!=UINT32_MAX) ring.parent+=out.ringCount;
            data<ContourRing>(out._rings)[out.ringCount+i]=ring;
        }
        data<PieceContour>(out._pieces)[p]={p,out.ringCount,out.ringCount+r.ringCount,!r.ringCount};
        out.pointCount+=r.pointCount; out.ringCount+=r.ringCount; ++out.pieceCount;
    }
    if (!m.flush()) return fail(m.outcome);
    Result<ContourSet> result; result.outcome={Status::changed,""}; result.consumed=work.visits()-start;
    out.peakBudgetBytes=m.peak; result.value=std::move(out); return result;
}
} // namespace Inkscape::Bitmap
