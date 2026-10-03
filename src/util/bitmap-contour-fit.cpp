// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/bitmap-contour-fit.h"
#include <2geom/bezier-utils.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace Inkscape::Bitmap {
FittedContourSet &FittedContourSet::operator=(FittedContourSet &&o) noexcept
{
    if (this != &o) {
        segmentCount = std::exchange(o.segmentCount, 0);
        ringCount = std::exchange(o.ringCount, 0); pieceCount = std::exchange(o.pieceCount, 0);
        anchorCount = std::exchange(o.anchorCount, 0); serializedBytes = std::exchange(o.serializedBytes, 0);
        _segments = std::move(o._segments); _rings = std::move(o._rings); _pieces = std::move(o._pieces);
    }
    return *this;
}
namespace {
constexpr auto none = UINT32_MAX;
// Bound the uninterruptible external call. Long curved spans acquire extra
// anchors; long exactly straight spans never enter the fitter.
constexpr std::uint32_t fitSpan = 128;
struct Poll {
    JobWork &work; Stop stop; FitOptions const &options;
    PhaseTimer timer;
    Outcome outcome{};
    unsigned pending = 0;
    bool flush() noexcept {
        auto charged = work.advance(pending); pending = 0;
        auto timed = timer.check(stop);
        if (!charged.ok()) outcome = charged;
        if (!timed.ok()) outcome = timed;
        return outcome.ok();
    }
    bool tick() noexcept { ++pending; return pending < 128 || flush(); }
    bool phase(FitPhase p) noexcept {
        if (!flush()) return false;
        if (options.observe) options.observe(p, options.observerData);
        return flush();
    }
    bool fail(char const *s) noexcept { outcome = {Status::failed, s}; return false; }
};
template <typename T> T *data(PlainBuffer &b) noexcept { return reinterpret_cast<T *>(b.data()); }
template <typename T>
bool reserve(PlainBuffer &b, std::uint32_t n, std::uint32_t cap, Budget &budget, Poll &p) noexcept
{
    if (n > cap) return p.fail("Fitted contour anchor cap exceeded");
    auto old = b.size() / sizeof(T);
    if (n <= old) return true;
    auto capacity = std::min<std::uint64_t>(cap, std::max<std::uint64_t>(n, std::max<std::uint64_t>(32, old * 2)));
    PlainBuffer next;
    p.outcome = next.allocate(budget, Stage::topology, capacity, sizeof(T), p.options.fault, p.stop);
    if (!p.outcome.ok() || !p.phase(FitPhase::allocation)) return false;
    for (std::uint64_t i = 0; i < capacity; ++i) {
        if (!p.tick()) return false;
        // Zero padding too: consumers may compare the plain output bytewise.
        std::memset(data<T>(next) + i, 0, sizeof(T));
        if (i < old) std::memcpy(data<T>(next) + i, data<T>(b) + i, sizeof(T));
        else new (data<T>(next) + i) T{};
    }
    b = std::move(next); return p.flush();
}
ContourPoint add(ContourPoint a, ContourPoint b) { return {a.x + b.x, a.y + b.y}; }
ContourPoint sub(ContourPoint a, ContourPoint b) { return {a.x - b.x, a.y - b.y}; }
ContourPoint mul(ContourPoint a, double t) { return {a.x * t, a.y * t}; }
double dot(ContourPoint a, ContourPoint b) { return a.x * b.x + a.y * b.y; }
double length(ContourPoint a) { return std::hypot(a.x, a.y); }
bool finite(ContourPoint a) { return std::isfinite(a.x) && std::isfinite(a.y); }
bool equal(ContourPoint a, ContourPoint b) { return a.x == b.x && a.y == b.y; }
double distanceSq(ContourPoint p, ContourPoint a, ContourPoint b) {
    auto d = sub(b, a); auto n = dot(d, d);
    auto t = n ? std::clamp(dot(sub(p, a), d) / n, 0., 1.) : 0.;
    auto e = sub(p, add(a, mul(d, t))); return dot(e, e);
}
struct Span { std::uint32_t a, b; };
// Return an ordered closed copy split at a deterministic approximate diameter.
// The two arcs are independent DP problems; the closing edge participates.
bool simplify(ContourPoint const *input, std::uint32_t n, double eps,
              PlainBuffer &ordered, PlainBuffer &kept, PlainBuffer &stack,
              Budget &budget, Poll &p) noexcept
{
    if (!reserve<ContourPoint>(ordered, n + 1, 2000001, budget, p) ||
        !reserve<unsigned char>(kept, n + 1, 2000001, budget, p) ||
        !reserve<Span>(stack, n + 1, 2000001, budget, p)) return false;
    auto far = [&](std::uint32_t seed, std::uint32_t &out) {
        double best = -1; out = seed;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!p.tick()) return false;
            auto d = sub(input[i], input[seed]); auto ds = dot(d, d);
            if (ds > best) { best = ds; out = i; }
        }
        return true;
    };
    std::uint32_t a, b;
    if (!far(0, a) || !far(a, b)) return false;
    if (a == b) return p.fail("Degenerate input contour ring");
    auto points = data<ContourPoint>(ordered); auto flags = data<unsigned char>(kept);
    for (std::uint32_t i = 0; i <= n; ++i) {
        if (!p.tick()) return false;
        points[i] = input[(a + i) % n]; flags[i] = 0;
    }
    auto split = (b + n - a) % n; flags[0] = flags[split] = flags[n] = 1;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!p.tick()) return false;
        auto u = sub(points[i], points[(i + n - 1) % n]); auto v = sub(points[i + 1], points[i]);
        // Include exact 45-degree marching-square chamfers conservatively too.
        if (dot(u, v) <= std::sqrt(.5) * length(u) * length(v) * (1 + 1e-14)) flags[i] = 1;
    }
    auto runDP = [&]() {
        std::uint32_t top = 0, prev = 0;
        for (std::uint32_t i = 1; i <= n; ++i) {
            if (!p.tick()) return false;
            if (flags[i]) { data<Span>(stack)[top++] = {prev, i}; prev = i; }
        }
        auto tolerance = eps * eps * (1 - 1e-12);
        while (top) {
            if (!p.tick()) return false;
            auto s = data<Span>(stack)[--top]; double worst = -1; auto at = s.a;
            for (auto i = s.a + 1; i < s.b; ++i) {
                if (!p.tick()) return false;
                auto ds = distanceSq(points[i], points[s.a], points[s.b]);
                if (ds > worst) { worst = ds; at = i; }
            }
            if (worst > tolerance) {
                flags[at] = 1;
                data<Span>(stack)[top++] = {s.a, at}; data<Span>(stack)[top++] = {at, s.b};
            } else if (s.b - s.a + 1 > fitSpan && worst > 1e-24) {
                // Keep each nontrivial external fit small enough for bounded polling.
                at = s.a + (s.b - s.a) / 2; flags[at] = 1;
                data<Span>(stack)[top++] = {s.a, at}; data<Span>(stack)[top++] = {at, s.b};
            }
        }
        return p.flush();
    };
    if (!runDP()) return false;
    // A small ring must still have at least three distinct anchors.
    unsigned count = 0;
    for (std::uint32_t i = 0; i < n; ++i) { if (!p.tick()) return false; count += flags[i] != 0; }
    if (count < 3) {
        double best = -1; std::uint32_t extra = 0;
        for (std::uint32_t i = 1; i < n; ++i) {
            if (!p.tick()) return false;
            if (i == split) continue;
            auto d = distanceSq(points[i], points[0], points[split]);
            if (d > best) { best = d; extra = i; }
        }
        flags[extra] = 1;
        // Adding an anchor changes its two chords; certify those new spans
        // again instead of assuming the previous diameter bound still holds.
        if (!runDP()) return false;
    }
    return p.flush();
}
struct Cubic { ContourPoint q[4]; };
ContourPoint eval(Cubic const &c, double t) {
    auto u = 1 - t;
    return add(add(mul(c.q[0], u*u*u), mul(c.q[1], 3*u*u*t)),
               add(mul(c.q[2], 3*u*t*t), mul(c.q[3], t*t*t)));
}
ContourPoint derivative(Cubic const &c, double t) {
    auto u = 1 - t;
    return mul(add(add(mul(sub(c.q[1], c.q[0]), u*u), mul(sub(c.q[2], c.q[1]), 2*u*t)),
                   mul(sub(c.q[3], c.q[2]), t*t)), 3);
}
Cubic interval(Cubic const &c, double a, double b) {
    auto s = eval(c, a), e = eval(c, b);
    return {{s, add(s, mul(derivative(c, a), (b-a)/3)), sub(e, mul(derivative(c, b), (b-a)/3)), e}};
}
// A continuous, onto correspondence, not a vertex-only distance check:
// chord-length parameterize each input edge, restrict the cubic to that
// interval, and bound cubic-minus-line by its Bernstein control hull. This
// certifies both directions of the Hausdorff bound for the whole polylines.
bool deviation(Cubic const &c, ContourPoint const *points, unsigned n, double eps, Poll &p) {
    double total = 0;
    for (unsigned i = 1; i < n; ++i) { if (!p.tick()) return false; total += length(sub(points[i], points[i-1])); }
    if (!(total > 0) || !std::isfinite(total)) return false;
    double sum = 0, prev = 0;
    for (unsigned i = 1; i < n; ++i) {
        if (!p.tick()) return false;
        sum += length(sub(points[i], points[i-1])); auto t = i + 1 == n ? 1. : sum / total;
        auto part = interval(c, prev, t);
        for (unsigned k = 0; k < 4; ++k) {
            auto line = add(points[i-1], mul(sub(points[i], points[i-1]), k / 3.));
            if (!(length(sub(part.q[k], line)) <= eps * (1 - 1e-10))) return false;
        }
        prev = t;
    }
    return true;
}
bool monotone(Cubic const &c) {
    auto axis = sub(c.q[3], c.q[0]);
    if (!(dot(axis, axis) > 0)) return false;
    for (unsigned i = 1; i < 4; ++i) if (dot(sub(c.q[i], c.q[i-1]), axis) < 0) return false;
    return true; // strict endpoint displacement and nonnegative derivative
}
struct Box { double minX, minY, maxX, maxY; };
Box hull(Cubic const &c) {
    Box b{c.q[0].x, c.q[0].y, c.q[0].x, c.q[0].y};
    for (unsigned i = 1; i < 4; ++i) {
        b.minX = std::min(b.minX, c.q[i].x); b.maxX = std::max(b.maxX, c.q[i].x);
        b.minY = std::min(b.minY, c.q[i].y); b.maxY = std::max(b.maxY, c.q[i].y);
    }
    // Outward padding keeps near-tangent pairs on the refusal/fallback side
    // of floating-point subdivision error rather than certifying a false gap.
    auto scale = std::max({1., std::abs(b.minX), std::abs(b.maxX), std::abs(b.minY), std::abs(b.maxY)});
    auto guard = 64 * std::numeric_limits<double>::epsilon() * scale;
    b.minX -= guard; b.minY -= guard; b.maxX += guard; b.maxY += guard;
    return b;
}
bool separate(Box a, Box b) { return a.maxX < b.minX || b.maxX < a.minX || a.maxY < b.minY || b.maxY < a.minY; }
Cubic curve(ContourPoint start, ContourSegment const &s) {
    if (s.cubic) return {{start, s.c1, s.c2, s.end}};
    auto d = sub(s.end, start); return {{start, add(start, mul(d, 1./3)), add(start, mul(d, 2./3)), s.end}};
}
long double cross(ContourPoint a, ContourPoint b, ContourPoint c) {
    return (static_cast<long double>(b.x)-a.x)*(static_cast<long double>(c.y)-a.y) -
           (static_cast<long double>(b.y)-a.y)*(static_cast<long double>(c.x)-a.x);
}
bool lineHit(Cubic const &a, Cubic const &b) {
    if (separate(hull(a), hull(b))) return false;
    auto x = cross(a.q[0], a.q[3], b.q[0]), y = cross(a.q[0], a.q[3], b.q[3]);
    auto u = cross(b.q[0], b.q[3], a.q[0]), v = cross(b.q[0], b.q[3], a.q[3]);
    return ((x <= 0 && y >= 0) || (y <= 0 && x >= 0)) && ((u <= 0 && v >= 0) || (v <= 0 && u >= 0));
}
// Adjacent spans may meet only at their shared anchor. A separating plane
// through it proves this without discarding a neighbourhood of the endpoint.
bool adjacentSafe(Cubic const &a, Cubic const &b) {
    auto in = sub(a.q[3], a.q[0]), out = sub(b.q[3], b.q[0]);
    auto il = length(in), ol = length(out);
    if (!(il > 0 && ol > 0)) return false;
    auto axis = add(mul(in, 1/il), mul(out, 1/ol));
    for (unsigned k = 0; k < 3; ++k) if (!(dot(sub(a.q[k], a.q[3]), axis) < 0)) return false;
    for (unsigned k = 1; k < 4; ++k) if (!(dot(sub(b.q[k], b.q[0]), axis) > 0)) return false;
    return true;
}
// Recursive hull separation is a conservative certificate for actual curves,
// including tangencies: unresolved pairs cause fallback, never acceptance.
bool curveHit(Cubic const &a, Cubic const &b, unsigned depth, Poll &p) {
    if (!p.tick()) return true;
    auto ba = hull(a), bb = hull(b);
    if (separate(ba, bb)) return false;
    if (depth == 24) return true;
    if (std::max(ba.maxX-ba.minX, ba.maxY-ba.minY) >= std::max(bb.maxX-bb.minX, bb.maxY-bb.minY)) {
        return curveHit(interval(a, 0, .5), b, depth+1, p) || curveHit(interval(a, .5, 1), b, depth+1, p);
    }
    return curveHit(a, interval(b, 0, .5), depth+1, p) || curveHit(a, interval(b, .5, 1), depth+1, p);
}
// Interior extrema of a scalar cubic, sorted deterministically.
unsigned extrema(double v0, double v1, double v2, double v3, double *t) {
    double a = -v0+3*v1-3*v2+v3, b = 2*(v0-2*v1+v2), c = v1-v0;
    unsigned n = 0;
    if (a == 0) { if (b != 0) { auto r = -c/b; if (r > 0 && r < 1) t[n++] = r; } }
    else {
        auto disc = b*b-4*a*c;
        if (disc >= 0) {
            auto q = -.5*(b+std::copysign(std::sqrt(disc), b));
            auto r = q/a; if (r > 0 && r < 1) t[n++] = r;
            if (q != 0) { r = c/q; if (r > 0 && r < 1 && (!n || r != t[0])) t[n++] = r; }
        }
    }
    if (n == 2 && t[1] < t[0]) std::swap(t[0], t[1]);
    return n;
}
void metadata(FittedRing &r, ContourSegment const *segments, Poll &p) {
    r.area = 0; r.minX = r.maxX = r.start.x; r.minY = r.maxY = r.start.y;
    auto start = r.start; long double area = 0;
    for (auto i = r.begin; i < r.end; ++i) {
        if (!p.tick()) return;
        auto const &s = segments[i]; auto c = curve(start, s);
        auto include = [&](ContourPoint q) {
            r.minX = std::min(r.minX, q.x); r.maxX = std::max(r.maxX, q.x);
            r.minY = std::min(r.minY, q.y); r.maxY = std::max(r.maxY, q.y);
        };
        include(s.end);
        if (s.cubic) {
            double t[2];
            auto nx = extrema(c.q[0].x,c.q[1].x,c.q[2].x,c.q[3].x,t);
            for (unsigned k = 0; k < nx; ++k) include(eval(c,t[k]));
            auto ny = extrema(c.q[0].y,c.q[1].y,c.q[2].y,c.q[3].y,t);
            for (unsigned k = 0; k < ny; ++k) include(eval(c,t[k]));
        }
        // Analytic integral of x*y' - y*x', translated to the ring origin.
        long double x[4], y[4];
        auto coefficients = [](double a, double b, double c, double d, double origin, long double *out) {
            long double aa = static_cast<long double>(a)-origin, bb = static_cast<long double>(b)-origin;
            long double cc = static_cast<long double>(c)-origin, dd = static_cast<long double>(d)-origin;
            out[0]=aa; out[1]=3*(bb-aa); out[2]=3*(aa-2*bb+cc); out[3]=-aa+3*bb-3*cc+dd;
        };
        coefficients(c.q[0].x,c.q[1].x,c.q[2].x,c.q[3].x,r.start.x,x);
        coefficients(c.q[0].y,c.q[1].y,c.q[2].y,c.q[3].y,r.start.y,y);
        for (unsigned j = 0; j < 4; ++j) for (unsigned k = 1; k < 4; ++k) area += (x[j]*k*y[k]-y[j]*k*x[k])/(j+k);
        start = s.end;
    }
    r.area = static_cast<double>(area/2);
}
bool contains(FittedRing const &r, ContourSegment const *segments, ContourPoint point, Poll &p) {
    bool inside = false; auto start = r.start;
    for (auto i = r.begin; i < r.end; ++i) {
        if (!p.tick()) return false;
        auto c = curve(start, segments[i]); start = segments[i].end;
        double critical[2], cuts[4] = {0};
        auto count = extrema(c.q[0].y,c.q[1].y,c.q[2].y,c.q[3].y,critical);
        for (unsigned k = 0; k < count; ++k) cuts[k+1] = critical[k];
        cuts[count+1] = 1;
        for (unsigned k = 0; k <= count; ++k) {
            double a = cuts[k], b = cuts[k+1]; auto ay = eval(c,a).y, by = eval(c,b).y;
            if ((ay > point.y) == (by > point.y)) continue;
            bool up = by > ay;
            for (unsigned j = 0; j < 54; ++j) {
                if (!p.tick()) return false;
                auto mid = (a+b)/2;
                if ((eval(c,mid).y < point.y) == up) a = mid; else b = mid;
            }
            if (eval(c,(a+b)/2).x > point.x) inside = !inside;
        }
    }
    return inside;
}
struct Entry { Box box; std::uint32_t id, ring; };
bool less(Entry const &a, Entry const &b) { return a.box.minX < b.box.minX || (a.box.minX == b.box.minX && a.id < b.id); }
bool sort(Entry *v, std::uint32_t n, Poll &p) {
    auto sift = [&](std::uint32_t root, std::uint32_t end) {
        while (std::uint64_t(root)*2+1 < end) {
            if (!p.tick()) return false;
            auto child = root*2+1;
            if (child+1 < end && less(v[child],v[child+1])) ++child;
            if (!less(v[root],v[child])) break;
            std::swap(v[root],v[child]); root=child;
        }
        return true;
    };
    for (auto i=n/2; i; --i) if (!sift(i-1,n)) return false;
    for (auto i=n; i>1; --i) { std::swap(v[0],v[i-1]); if (!sift(0,i-1)) return false; }
    return p.flush();
}
// Mark unsafe spans as well as implicated rings. Area and hierarchy failures
// have no unique responsible span and retain ring marks for the last resort.
// The sweep uses control hulls, followed by actual curve separation.
bool validate(ContourSet const &input, PieceContour const &piece, FittedRing *rings,
              ContourSegment const *segments, PlainBuffer &entries, PlainBuffer &bad, PlainBuffer &badSpans,
              Budget &budget, Poll &p, bool &valid) {
    auto begin = piece.ringBegin, end = piece.ringEnd; valid = true;
    if (begin == end) return true;
    auto segmentBegin = rings[begin].begin, segmentEnd = rings[end-1].end;
    if (!reserve<Entry>(entries,segmentEnd-segmentBegin,50000,budget,p) ||
        !reserve<unsigned char>(badSpans,segmentEnd-segmentBegin,50000,budget,p)) return false;
    auto spans = data<unsigned char>(badSpans);
    auto marks = data<unsigned char>(bad); auto v = data<Entry>(entries); unsigned n=0;
    for (auto ri=begin; ri<end; ++ri) {
        if (!p.tick()) return false;
        marks[ri]=0; auto &r=rings[ri]; metadata(r,segments,p);
        if (!p.outcome.ok()) return false;
        if (!std::isfinite(r.area) || r.area == 0 || (r.area > 0) != (input.rings()[ri].area > 0)) marks[ri]=1;
        auto start=r.start;
        for (auto j=r.begin; j<r.end; ++j) {
            if (!p.tick()) return false;
            auto c=curve(start,segments[j]); start=segments[j].end;
            spans[j-segmentBegin]=0;
            if (segments[j].cubic && !monotone(c)) marks[ri]=spans[j-segmentBegin]=1;
            v[n++]={hull(c),j,ri};
        }
    }
    if (!sort(v,n,p)) return false;
    for (unsigned i=0; i<n; ++i) for (auto j=i+1; j<n && v[j].box.minX <= v[i].box.maxX; ++j) {
        if (!p.tick()) return false;
        auto a=v[i], b=v[j];
        if (separate(a.box,b.box)) continue;
        auto make = [&](Entry const &e) {
            auto const &r=rings[e.ring];
            return curve(e.id == r.begin ? r.start : segments[e.id-1].end,segments[e.id]);
        };
        auto ca=make(a), cb=make(b); bool hit;
        auto const &r=rings[a.ring];
        if (a.ring == b.ring && (a.id+1 == b.id || b.id+1 == a.id ||
            (a.id == r.begin && b.id+1 == r.end) || (b.id == r.begin && a.id+1 == r.end))) {
            bool forward = a.id+1 == b.id || (a.id+1 == r.end && b.id == r.begin);
            hit = forward ? !adjacentSafe(ca,cb) : !adjacentSafe(cb,ca);
        } else if (!segments[a.id].cubic && !segments[b.id].cubic) hit=lineHit(ca,cb);
        else hit=curveHit(ca,cb,0,p);
        if (!p.outcome.ok()) return false;
        if (hit) {
            marks[a.ring]=marks[b.ring]=1;
            // A line already is its simplified fallback. Replace only the
            // cubic members of an uncertified pair, then check the whole piece.
            if (segments[a.id].cubic) spans[a.id-segmentBegin]=1;
            if (segments[b.id].cubic) spans[b.id-segmentBegin]=1;
        }
    }
    for (auto ri=begin; ri<end; ++ri) {
        if (!p.tick()) return false;
        auto const &r=rings[ri]; std::uint32_t parent=none; double area=INFINITY;
        for (auto j=begin; j<end; ++j) {
            if (!p.tick()) return false;
            auto const &s=rings[j]; auto sa=std::abs(s.area);
            if (j == ri || sa <= std::abs(r.area) || sa >= area || r.start.x < s.minX || r.start.x > s.maxX ||
                r.start.y < s.minY || r.start.y > s.maxY) continue;
            if (contains(s,segments,r.start,p)) { parent=j; area=sa; }
            if (!p.outcome.ok()) return false;
        }
        if (parent != input.rings()[ri].parent || r.depth != (parent == none ? 0 : rings[parent].depth+1)) {
            marks[ri]=1;
            if (parent != none) marks[parent]=1;
            if (input.rings()[ri].parent != none) marks[input.rings()[ri].parent]=1;
        }
    }
    for (auto ri=begin; ri<end; ++ri) { if (!p.tick()) return false; if (marks[ri]) valid=false; }
    return p.flush();
}
} // namespace
Result<FittedContourSet> fitContours(ContourSet const &input, FitSettings settings, Budget &budget,
                                    JobWork &work, Stop stop, FitOptions options) noexcept
{
    auto before=work.visits(); Poll p{work,stop,options,PhaseTimer{}};
    FittedContourSet result;
    PlainBuffer ordered, kept, stack, lines, entries, bad, badSpans, restore, geomPoints;
    auto failed = [&]() {
        auto outcome=p.outcome; p.flush();
        Result<FittedContourSet> r; r.outcome=outcome.ok() ? p.outcome : outcome; r.consumed=work.visits()-before; return r;
    };
    auto refuse = [&](char const *s) { p.fail(s); return failed(); };
    auto cap=std::min(options.maxAnchors,200000u), pieceCap=std::min(options.maxAnchorsPerPiece,50000u);
    auto byteCap=std::min(options.maxSerializedBytes,16*MiB);
    auto eps=.05+.0195*settings.smoothing;
    if (!p.phase(FitPhase::simplify)) return failed();
    if (!std::isfinite(settings.smoothing) || settings.smoothing < 0 || settings.smoothing > 100)
        return refuse("Invalid contour smoothing (expected 0-100)");
    if ((input.pointCount && !input.points()) || (input.ringCount && !input.rings()) ||
        (input.pieceCount && !input.pieces()) || input.pointCount > 8000000 || input.ringCount > cap)
        return refuse("Invalid input contour set or fitted anchor cap");
    if (!reserve<FittedRing>(result._rings,input.ringCount,cap,budget,p) ||
        !reserve<FittedPiece>(result._pieces,input.pieceCount,input.pieceCount,budget,p) ||
        !reserve<unsigned char>(bad,input.ringCount,cap,budget,p) ||
        !reserve<unsigned char>(restore,input.ringCount,cap,budget,p)) return failed();
    auto rings=data<FittedRing>(result._rings);
    std::uint32_t expectedRing=0, expectedPoint=0;
    for (std::uint32_t pi=0; pi<input.pieceCount; ++pi) {
        if (!p.tick()) return failed();
        auto const &piece=input.pieces()[pi];
        if (piece.ringBegin != expectedRing || piece.ringEnd < piece.ringBegin || piece.ringEnd > input.ringCount ||
            piece.noContour != (piece.ringBegin == piece.ringEnd)) return refuse("Invalid contour piece ranges");
        for (auto ri=piece.ringBegin; ri<piece.ringEnd; ++ri) {
            if (!p.tick()) return failed(); auto const &r=input.rings()[ri];
            if (r.begin != expectedPoint || r.end < r.begin || r.end > input.pointCount || r.end-r.begin < 3 ||
                r.end-r.begin > 2000000 || !std::isfinite(r.area) || r.area == 0 ||
                (r.parent != none && (r.parent < piece.ringBegin || r.parent >= piece.ringEnd || r.parent == ri)))
                return refuse("Invalid input contour ring");
            for (auto j=r.begin; j<r.end; ++j) {
                if (!p.tick()) return failed();
                if (!finite(input.points()[j]) || equal(input.points()[j],input.points()[j+1 == r.end ? r.begin : j+1]))
                    return refuse("Invalid contour vertex");
            }
            expectedPoint=r.end;
        }
        auto pieceStart=result.segmentCount;
        // If a simplified fallback is unsafe, restore only the implicated
        // rings with zero-tolerance DP and lines. Rebuild ranges without ever
        // dropping a ring. Every retry restores at least one additional ring.
        for (unsigned attempt=0; attempt<=piece.ringEnd-piece.ringBegin; ++attempt) {
            result.segmentCount=pieceStart;
            for (auto ri=piece.ringBegin; ri<piece.ringEnd; ++ri) {
                if (!p.phase(FitPhase::simplify)) return failed();
                auto const &raw=input.rings()[ri]; auto n=raw.end-raw.begin;
                bool rawFallback=data<unsigned char>(restore)[ri] != 0;
                if (!simplify(input.points()+raw.begin,n,rawFallback ? 0 : eps,ordered,kept,stack,budget,p)) return failed();
                auto pts=data<ContourPoint>(ordered); auto flags=data<unsigned char>(kept);
                FittedRing r{}; std::memset(&r,0,sizeof(r)); r.begin=result.segmentCount; r.start=pts[0]; r.depth=raw.depth; r.parent=raw.parent; r.fallback=rawFallback;
                std::uint32_t prev=0;
                for (std::uint32_t j=1; j<=n; ++j) {
                    if (!p.tick()) return failed(); if (!flags[j]) continue;
                    if (result.segmentCount-pieceStart >= pieceCap) return refuse("Fitted contour per-piece anchor cap exceeded");
                    if (!reserve<ContourSegment>(result._segments,result.segmentCount+1,cap,budget,p) ||
                        !reserve<ContourSegment>(lines,result.segmentCount+1,cap,budget,p)) return failed();
                    ContourSegment s{}; std::memset(&s,0,sizeof(s)); s.end=pts[j];
                    double worst=0;
                    for (auto k=prev+1; k<j; ++k) { if (!p.tick()) return failed(); worst=std::max(worst,distanceSq(pts[k],pts[prev],pts[j])); }
                    data<ContourSegment>(lines)[result.segmentCount]=s;
                    if (!rawFallback && worst > 1e-24) {
                        if (!p.phase(FitPhase::fitting)) return failed();
                        auto count=j-prev+1;
                        if (count > fitSpan) return refuse("Unbounded cubic fitting span");
                        if (!reserve<Geom::Point>(geomPoints,count,fitSpan,budget,p)) return failed();
                        for (unsigned k=0; k<count; ++k) {
                            if (!p.tick()) return failed();
                            new (data<Geom::Point>(geomPoints)+k) Geom::Point(pts[prev+k].x,pts[prev+k].y);
                        }
                        Budget::Token transient;
                        p.outcome=budget.acquire(Stage::topology,std::uint64_t(count)*64,transient);
                        if (!p.outcome.ok() || !p.phase(FitPhase::fitterReservation)) return failed();
                        if (options.fault && options.fault->fail()) return refuse("Cubic fitter allocation fault");
                        // Charge the bounded opaque work conservatively before entering 2geom.
                        p.outcome=work.advance(std::uint64_t(count)*32);
                        if (!p.outcome.ok() || !p.flush()) return failed();
                        Cubic c{}; int fitted;
                        try {
                            if (options.fitCubic) fitted=options.fitCubic(c.q,pts+prev,count,eps*eps);
                            else {
                                Geom::Point output[4];
                                fitted=Geom::bezier_fit_cubic_full(output,nullptr,data<Geom::Point>(geomPoints),count,
                                                                 Geom::Point(0,0),Geom::Point(0,0),eps*eps,1);
                                if (fitted == 1) for (unsigned k=0; k<4; ++k) c.q[k]={output[k].x(),output[k].y()};
                            }
                        } catch (std::bad_alloc const &) { return refuse("Cubic fitter allocation failed"); }
                        catch (...) { return refuse("Cubic fitter exception"); }
                        transient.release();
                        if (!p.flush()) return failed();
                        bool good=fitted == 1 && equal(c.q[0],pts[prev]) && equal(c.q[3],pts[j]);
                        for (auto q:c.q) good=good && finite(q);
                        if (good) good=monotone(c) && deviation(c,pts+prev,count,eps,p);
                        if (!p.outcome.ok()) return failed();
                        if (good) { s.cubic=true; s.c1=c.q[1]; s.c2=c.q[2]; }
                        else r.fallback=true;
                    }
                    data<ContourSegment>(result._segments)[result.segmentCount++]=s; prev=j;
                }
                r.end=result.segmentCount; rings[ri]=r;
            }
            if (!p.phase(FitPhase::validation)) return failed();
            bool valid;
            if (!validate(input,piece,rings,data<ContourSegment>(result._segments),entries,bad,badSpans,budget,p,valid)) return failed();
            if (valid) break;
            // Each retry removes at least one cubic. Certified spans survive
            // localized failures; every mixed ring is re-certified as a whole.
            while (!valid) {
                bool changed=false;
                auto segments=data<ContourSegment>(result._segments);
                for (auto ri=piece.ringBegin; ri<piece.ringEnd; ++ri) {
                    if (!p.tick()) return failed();
                    auto &r=rings[ri];
                    for (auto j=r.begin; j<r.end; ++j) {
                        if (!p.tick()) return failed();
                        if (!data<unsigned char>(badSpans)[j-pieceStart] || !segments[j].cubic) continue;
                        segments[j]=data<ContourSegment>(lines)[j];
                        r.fallback=true; changed=true;
                    }
                }
                if (!changed) {
                    // Unlocalized orientation/containment failures, or unsafe
                    // lines, get the whole-ring fallback only after span retries.
                    for (auto ri=piece.ringBegin; ri<piece.ringEnd; ++ri) {
                        if (!p.tick()) return failed();
                        auto &r=rings[ri]; if (!data<unsigned char>(bad)[ri]) continue;
                        for (auto j=r.begin; j<r.end; ++j) {
                            if (!p.tick()) return failed();
                            if (!segments[j].cubic) continue;
                            segments[j]=data<ContourSegment>(lines)[j];
                            r.fallback=true; changed=true;
                        }
                    }
                }
                if (!changed) break;
                if (!validate(input,piece,rings,segments,entries,bad,badSpans,budget,p,valid)) return failed();
            }
            if (valid) break;
            bool restored=false;
            for (auto ri=piece.ringBegin; ri<piece.ringEnd; ++ri) {
                if (!p.tick()) return failed();
                if (data<unsigned char>(bad)[ri] && !data<unsigned char>(restore)[ri]) {
                    data<unsigned char>(restore)[ri]=1; restored=true;
                }
            }
            if (!restored) return refuse("Input contour topology cannot be certified");
        }
        auto &fittedPiece=data<FittedPiece>(result._pieces)[pi];
        fittedPiece.piece=piece.piece; fittedPiece.ringBegin=piece.ringBegin;
        fittedPiece.ringEnd=piece.ringEnd; fittedPiece.noContour=piece.noContour;
        expectedRing=piece.ringEnd;
    }
    if (expectedRing != input.ringCount || expectedPoint != input.pointCount) return refuse("Incomplete input contour ranges");
    if (!p.phase(FitPhase::output)) return failed();
    // Conservative SVG estimate: each binary64 coordinate needs at most 24
    // characters (17 significant digits plus sign/exponent), plus separators.
    // M x y, L x y or C x y x y x y, and Z per ring. Count unique anchors,
    // including start but excluding the repeated closing endpoint: one/segment.
    std::uint64_t bytes=0;
    for (std::uint32_t i=0; i<input.ringCount; ++i) {
        if (!p.tick()) return failed(); bytes+=52; // move + close
        for (auto j=rings[i].begin; j<rings[i].end; ++j) {
            if (!p.tick()) return failed(); bytes+=data<ContourSegment>(result._segments)[j].cubic ? 152 : 52;
        }
        if (bytes > byteCap) return refuse("Fitted contour serialized path cap exceeded");
    }
    result.ringCount=input.ringCount; result.pieceCount=input.pieceCount;
    result.anchorCount=result.segmentCount; result.serializedBytes=bytes;
    if (!p.flush()) return failed();
    Result<FittedContourSet> out; out.outcome={Status::changed,""}; out.consumed=work.visits()-before; out.value=std::move(result); return out;
}
} // namespace Inkscape::Bitmap
