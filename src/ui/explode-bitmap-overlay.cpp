// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/explode-bitmap-overlay.h"
#include <algorithm>
#include <cmath>
#include <bit>
#include <thread>
#include "desktop.h"
#include "ui/widget/canvas.h"
#include "document.h"
#include "display/control/canvas-item.h"
#include "display/control/canvas-item-drawing.h"
#include "display/drawing.h"
#include "display/drawing-image.h"
#include "display/control/canvas-item-ptr.h"
#include "object/sp-image.h"
#include "selection.h"
#include "xml/node.h"
#include "xml/node-observer.h"
namespace Inkscape::Bitmap {
namespace {
constexpr Outcome overflow{Status::unavailable, "Piece outlines are unavailable. The piece count is exact."};
constexpr Outcome stale{Status::canceled, "Image changed; recalculating."};
Geom::Affine matrix(GridTransform const &m) { return {m[0], m[1], m[2], m[3], m[4], m[5]}; }
// Difference of foreground intervals on adjacent rows. Cursor monotonicity makes
// every row linear even with millions of runs; no per-pixel or per-piece scan.
template <typename Emit>
Outcome scan(Partition const &p, JobWork &work, Stop stop, Emit emit)
{
    PhaseTimer timer;
    auto tick = [&]() -> Outcome {
        if (stop.requested()) return {Status::canceled, "Operation canceled."};
        auto o = timer.check(); return o.ok() ? work.advance(1) : o;
    };
    for (unsigned y = 0; y < p.height; ++y) {
        auto o = tick(); if (!o.ok()) return o;
        for (unsigned i = p.rows()[y]; i < p.rows()[y+1]; ++i) {
            o = tick(); if (!o.ok()) return o;
            auto r = p.run(i); if (!r.foreground) continue;
            // Same-piece foreground runs may be split at original boundaries.
            for (bool right : {false, true}) {
                auto adjacent = right ? i + 1 : i - 1;
                bool joined = right ? adjacent < p.rows()[y+1] : i > p.rows()[y];
                if (joined) { auto a = p.run(adjacent); joined = a.foreground && a.piece == r.piece; }
                if (!joined && !emit(OutlineSegment{right ? r.end : r.x, y, right ? r.end : r.x, y+1, r.piece})) return overflow;
            }
        }
        for (bool bottom : {false, true}) {
            bool edge = bottom ? y+1 == p.height : y == 0;
            unsigned row = bottom ? y+1 : y-1;
            unsigned j = edge ? 0 : p.rows()[row], end = edge ? 0 : p.rows()[row+1];
            for (unsigned i = p.rows()[y]; i < p.rows()[y+1]; ++i) {
                o = tick(); if (!o.ok()) return o;
                auto r = p.run(i); if (!r.foreground) continue;
                auto x = r.x;
                while (j < end && p.run(j).end <= x) { o = tick(); if (!o.ok()) return o; ++j; }
                auto k = j;
                while (x < r.end) {
                    o = tick(); if (!o.ok()) return o;
                    auto next = r.end; bool covered = false;
                    if (k < end) {
                        auto a = p.run(k); next = std::min(next, a.end);
                        covered = a.foreground && a.piece == r.piece;
                    }
                    if (!covered && !emit(OutlineSegment{x, bottom ? y+1 : y, next, bottom ? y+1 : y, r.piece})) return overflow;
                    x = next; if (k < end && p.run(k).end <= x) ++k;
                }
            }
        }
    }
    return {};
}
struct Drawing {
    std::shared_ptr<OutlineStorage const> storage;
    std::atomic<bool> active{true};
    Geom::Affine doc2dt; // captured on main, never fetched from a render worker
    OverlayTestHooks *hooks = nullptr;
};
bool axisAligned(Geom::Affine const &m) {
    return (m[1] == 0 && m[2] == 0) || (m[0] == 0 && m[3] == 0);
}
Geom::Rect projectedBounds(OutlineStorage const &s, Geom::Affine const &m) {
    Geom::Rect bounds(Geom::Point(s.bounds[0],s.bounds[1]),Geom::Point(s.bounds[2],s.bounds[3]));
    return bounds*m;
}
bool invertible(Geom::Affine const &m) {
    for (unsigned i = 0; i < 6; ++i) if (!std::isfinite(m[i])) return false;
    auto det = m[0]*m[3] - m[1]*m[2];
    return std::isfinite(det) && det != 0;
}
struct TileRange { unsigned x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
TileRange tileRange(OutlineStorage const &s, Geom::Affine const &m, Geom::Rect rect) {
    if (!invertible(m) || !s.columns || !s.rows) return {};
    rect.expandBy(4); // conservative logical margin for the device-pixel strokes
    auto source = rect * m.inverse(); // all four corners, also for shear/reflection
    if (!source.isFinite()) return {0, 0, s.columns, s.rows};
    auto tile = [](double v, unsigned limit) {
        return unsigned(std::clamp(std::floor(v / outlineTileSize), 0.0, double(limit)));
    };
    return {tile(source.left(), s.columns), tile(source.top(), s.rows),
            tile(source.right() + outlineTileSize, s.columns),
            tile(source.bottom() + outlineTileSize, s.rows)};
}
template <typename F>
void visit(OutlineStorage const &s, TileRange r, F f) {
    for (auto y = r.y0; y < r.y1; ++y) for (auto x = r.x0; x < r.x1; ++x) {
        auto const &tile = s.index()[y*s.columns+x];
        for (auto i = tile.begin; i < tile.begin + tile.count; ++i) f(s.data()[i]);
    }
}
// Rasterize a square-capped axis-aligned edge into an A8 coverage mask. Keep
// subpixel positions (8 fractional bits), union overlapping edge coverage with
// max, and composite each colour once. No staircase vertex is simplified.
void cover(unsigned char *pixels, int stride, int width, int height,
           double left, double top, double right, double bottom) {
    auto fixed=[&](double v) { return int(std::floor(std::clamp(v, -1.0, double(std::max(width,height))+1)*256+0.5)); };
    int l=std::clamp(fixed(left),0,width*256), r=std::clamp(fixed(right),0,width*256);
    int t=std::clamp(fixed(top),0,height*256), b=std::clamp(fixed(bottom),0,height*256);
    if (l>=r || t>=b) return;
    int first=l>>8, last=(r+255)>>8;
    for (int y=t>>8;y<(b+255)>>8;++y) {
        int yc=std::min(b,(y+1)*256)-std::max(t,y*256);
        auto row=pixels+y*stride;
        for (int x=first;x<last;++x) {
            int xc=std::min(r,(x+1)*256)-std::max(l,x*256);
            auto coverage=static_cast<unsigned char>((xc*yc*255+32768)>>16);
            row[x]=std::max(row[x],coverage);
        }
    }
}
void renderAligned(Drawing const &drawing, Geom::Affine const &m, CanvasItemBuffer &buf, Geom::Rect const &bounds) {
    // Clip in floating point before converting to integer device coordinates.
    // Panning can place the entire outline far outside the canvas integer range.
    double left=std::max(bounds.min().x(),double(buf.rect.min().x()));
    double top=std::max(bounds.min().y(),double(buf.rect.min().y()));
    double right=std::min(bounds.max().x(),double(buf.rect.max().x()));
    double bottom=std::min(bounds.max().y(),double(buf.rect.max().y()));
    if (left>=right || top>=bottom) return;
    auto rect=Geom::Rect(Geom::Point(left,top),Geom::Point(right,bottom)).roundOutwards();
    int scale=buf.device_scale, width=rect.width()*scale, height=rect.height()*scale;
    auto halo=Cairo::ImageSurface::create(Cairo::Surface::Format::A8,width,height);
    auto accent=Cairo::ImageSurface::create(Cairo::Surface::Format::A8,width,height);
    auto hp=halo->get_data(), ap=accent->get_data();
    auto const &s=*drawing.storage;
    std::uint64_t visited = 0;
    visit(s, tileRange(s, m, Geom::Rect(buf.rect)), [&](OutlineSegment a) {
        if (!drawing.active.load(std::memory_order_acquire)) return;
        auto p=(Geom::Point(a.x,a.y)*m-rect.min())*scale;
        auto q=(Geom::Point(a.endX,a.endY)*m-rect.min())*scale;
        double l=std::min(p.x(),q.x()), r=std::max(p.x(),q.x());
        double t=std::min(p.y(),q.y()), b=std::max(p.y(),q.y());
        cover(hp,halo->get_stride(),width,height,l-1.5,t-1.5,r+1.5,b+1.5);
        cover(ap,accent->get_stride(),width,height,l-0.5,t-0.5,r+0.5,b+0.5);
        if ((++visited & 4095) == 0 && drawing.hooks && drawing.hooks->afterStroke)
            drawing.hooks->afterStroke(drawing.hooks->context);
    });
    if (drawing.active.load(std::memory_order_acquire) && drawing.hooks && drawing.hooks->afterStroke)
        drawing.hooks->afterStroke(drawing.hooks->context);
    if (!drawing.active.load(std::memory_order_acquire)) return;
    halo->mark_dirty(); accent->mark_dirty();
    cairo_surface_set_device_scale(halo->cobj(),scale,scale);
    cairo_surface_set_device_scale(accent->cobj(),scale,scale);
    auto offset=rect.min()-buf.rect.min();
    buf.cr->set_source_rgba(0,0,0,0.55); buf.cr->mask(halo,offset.x(),offset.y());
    buf.cr->set_source_rgba(0,0.85,1,1); buf.cr->mask(accent,offset.x(),offset.y());
}
// Global 16 x 16 device-pixel coverage samples. A bitwise union is independent
// of segment order, tile boundaries and buffer clipping; colours are composited
// once. The geometry stays exact (no contour simplification or integer snapping).
void renderAffine(Drawing const &drawing, Geom::Affine const &m, CanvasItemBuffer &buf) {
    int scale=buf.device_scale, width=buf.rect.width()*scale, height=buf.rect.height()*scale;
    if (width<=0 || height<=0) return;
    using Samples=std::array<std::uint64_t,4>;
    std::vector<Samples> coverage(std::size_t(width)*height);
    auto surface=Cairo::ImageSurface::create(Cairo::Surface::Format::A8,width,height);
    auto origin=buf.rect.min()*scale;
    for (double half : {1.5,.5}) {
        std::fill(coverage.begin(),coverage.end(),Samples{});
        visit(*drawing.storage,tileRange(*drawing.storage,m,Geom::Rect(buf.rect)),[&](OutlineSegment edge) {
            if (!drawing.active.load(std::memory_order_acquire)) return;
            auto p=(Geom::Point(edge.x,edge.y)*m)*scale, q=(Geom::Point(edge.endX,edge.endY)*m)*scale;
            auto direction=q-p; auto length=std::hypot(direction.x(),direction.y());
            if (!length || !std::isfinite(length)) return;
            direction *= half/length;
            Geom::Point normal(-direction.y(),direction.x());
            std::array<Geom::Point,4> corners{p-direction+normal,p-direction-normal,q+direction-normal,q+direction+normal};
            double top=corners[0].y(),bottom=top;
            for(auto c:corners) { top=std::min(top,c.y()); bottom=std::max(bottom,c.y()); }
            auto sampleIndex=[](double v,int maximum) {
                return int(std::clamp(std::ceil(v*16-.5),0.,double(maximum)));
            };
            int begin=sampleIndex(top-origin.y(),height*16),end=sampleIndex(bottom-origin.y(),height*16);
            for(int sy=begin;sy<end;++sy) {
                double y=origin.y()+(sy+.5)/16;
                double left=INFINITY,right=-INFINITY;
                for(unsigned i=0;i<4;++i) {
                    auto a=corners[i],b=corners[(i+1)%4];
                    if ((a.y()<=y && y<b.y()) || (b.y()<=y && y<a.y())) {
                        auto x=a.x()+(y-a.y())*(b.x()-a.x())/(b.y()-a.y());
                        left=std::min(left,x); right=std::max(right,x);
                    }
                }
                if (!(left<right)) continue;
                int first=sampleIndex(left-origin.x(),width*16),last=sampleIndex(right-origin.x(),width*16);
                for(int sx=first;sx<last;) {
                    int n=std::min(last-sx,16-sx%16);
                    auto bits=((std::uint64_t(1)<<n)-1) << (16*(sy%4)+sx%16);
                    coverage[std::size_t(sy/16)*width+sx/16][(sy%16)/4] |= bits;
                    sx+=n;
                }
            }
        });
        if (!drawing.active.load(std::memory_order_acquire)) return;
        surface->flush();
        auto pixels=surface->get_data(); auto stride=surface->get_stride();
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            auto const &bits=coverage[std::size_t(y)*width+x];
            unsigned count=0; for(auto word:bits) count+=std::popcount(word);
            pixels[y*stride+x]=(count*255+128)/256;
        }
        surface->mark_dirty(); cairo_surface_set_device_scale(surface->cobj(),scale,scale);
        if(half==1.5)buf.cr->set_source_rgba(0,0,0,.55);else buf.cr->set_source_rgba(0,.85,1,1);
        buf.cr->mask(surface,0,0);
    }
    if (drawing.hooks && drawing.hooks->afterStroke) drawing.hooks->afterStroke(drawing.hooks->context);
}
void renderDrawing(Drawing const &drawing, Geom::Affine const &m, CanvasItemBuffer &buf) {
    auto bounds = projectedBounds(*drawing.storage, m); bounds.expandBy(4);
    auto cr = buf.cr; cr->save(); cr->push_group();
    if (axisAligned(m) && !(drawing.hooks && drawing.hooks->legacyStroke)) {
        renderAligned(drawing, m, buf, bounds);
    } else {
        renderAffine(drawing,m,buf);
    }
    auto tile = cr->pop_group();
    if (drawing.active.load(std::memory_order_acquire)) { cr->set_source(tile); cr->paint(); }
    cr->restore();
}
class OutlineItem final : public CanvasItem {
public:
    OutlineItem(CanvasItemGroup *parent, std::shared_ptr<Drawing> drawing)
        : CanvasItem(parent), _drawing(std::move(drawing)) {
        _name = "Explode Bitmap solid outlines"; request_update();
        startCameraCallback();
    }
    ~OutlineItem() override { stopCameraCallback(); }
    void startCameraCallback() {
        if (_tick) return;
        _observedArea.reset();
        // scroll_absolute has no camera signal and does not update CanvasItems.
        // Observe on main only; frozen readers keep their previous camera until
        // the canvas unsnapshots and processes the requested update.
        _tick = get_canvas()->add_tick_callback([this](auto const &) {
            auto area = get_canvas()->get_area_world();
            if (!_observedArea || *_observedArea != area) { _observedArea = area; request_update(); }
            return true;
        });
    }
    void stopCameraCallback() {
        if (_tick) get_canvas()->remove_tick_callback(std::exchange(_tick, 0));
    }
    bool cameraCallbackRegistered() const { return _tick != 0; }
    void replace(std::shared_ptr<Drawing> drawing) {
        if (drawing) { startCameraCallback(); _publishVisibility = true; }
        else stopCameraCallback();
        _pending = std::move(drawing); request_redraw();
        if (_scheduled) return;
        _scheduled = true;
        try {
            if (!_pending && _context->snapshotted() && _drawing && _drawing->hooks &&
                _drawing->hooks->retirement && _drawing->hooks->retirement->fail()) throw std::bad_alloc();
            defer([this] { _drawing = std::move(_pending); _scheduled = false; request_update(); });
        } catch (...) { _scheduled = false; throw; }
    }
    OutlineCamera camera() const { return _camera; }
    sigc::signal<void (OutlineVisibility)> visibility;
private:
    std::shared_ptr<Drawing> _drawing, _pending;
    bool _scheduled = false;
    unsigned _tick = 0;
    std::optional<Geom::IntRect> _observedArea;
    OutlineCamera _camera;
    bool _publishVisibility = true;
    void _update(bool) override {
        request_redraw(); _bounds = {};
        if (!_drawing) return;
        auto const &s = *_drawing->storage;
        auto m = matrix(s.pixelToDocument) * _drawing->doc2dt * affine();
        // Canvas stores may temporarily use an older affine while decoupled.
        // Map the current desktop viewport into that store's coordinates.
        auto canvas = get_canvas();
        auto viewport = Geom::Rect(canvas->get_area_world());
        auto live = canvas->get_affine();
        if (invertible(live)) viewport *= live.inverse() * affine();
        auto previous = _camera.visibility;
        _camera = outlineCamera(s, m, viewport);
        if (invertible(m)) {
            _bounds = projectedBounds(s,m);
            if (!_bounds->isFinite()) { _bounds = {}; _camera.visibility = OutlineVisibility::unavailable; }
            else _bounds->expandBy(4);
        }
        request_redraw();
        if (std::exchange(_publishVisibility, false) || previous != _camera.visibility)
            visibility.emit(_camera.visibility);
    }
    void _render(CanvasItemBuffer &buf) const override {
        auto drawing = _drawing;
        if (!drawing || !drawing->active.load(std::memory_order_acquire) ||
            _camera.visibility != OutlineVisibility::exact || buf.device_scale < 1) return;
        auto m = matrix(drawing->storage->pixelToDocument) * drawing->doc2dt * affine();
        renderDrawing(*drawing, m, buf);
    }
};
} // namespace
OutlineCamera outlineCamera(OutlineStorage const &s, Geom::Affine const &m, Geom::Rect const &viewport)
{
    if (!invertible(m) || !s.index() || !viewport.isFinite()) return {};
    auto r = tileRange(s,m,viewport);
    std::uint64_t count = 0;
    for (auto y = r.y0; y < r.y1; ++y) for (auto x = r.x0; x < r.x1; ++x)
        count += s.index()[y*s.columns+x].count;
    return {count > outlineDrawSegmentLimit ? OutlineVisibility::tooDense : OutlineVisibility::exact, count};
}
Outcome renderOutlinePreview(ExactOutlines const &outlines, CanvasItemBuffer &buf, Geom::Affine const &m)
{
    if (!outlines.storage || buf.device_scale < 1) return overflow;
    auto camera = outlineCamera(*outlines.storage,m,Geom::Rect(buf.rect));
    if (camera.visibility == OutlineVisibility::tooDense)
        return {Status::unavailable, "Zoom in to see piece outlines."};
    if (camera.visibility != OutlineVisibility::exact) return overflow;
    Drawing drawing; drawing.storage=outlines.storage;
    renderDrawing(drawing,m,buf);
    return {};
}
Result<ExactOutlines> prepareOutlines(Partition const &p, FinalGrid const &grid, Budget &budget,
                                     JobWork &work, Stop stop, AllocationFault *fault, std::uint64_t segmentBudget) noexcept
{
    try {
        if (!p.rows() || !p.sourceRuns() || p.width != grid.width || p.height != grid.height || p.pieceCount > 20000)
            return {{Status::incompatible, "Outlines require the exact final-grid partition."}};
        char serialized[256];
        if (!serializeGridTransform(grid.pixelToDocument, serialized, sizeof(serialized)))
            return {{Status::incompatible, "Invalid outline geometry."}};
        auto data = std::make_shared<OutlineStorage>();
        data->columns = p.width / outlineTileSize + 1;
        data->rows = p.height / outlineTileSize + 1;
        auto tileCount = std::uint64_t(data->columns)*data->rows;
        auto overhead = tileCount*sizeof(OutlineTile) + p.pieceCount*sizeof(Piece);
        if (overhead > outlineByteLimit) return {overflow};
        auto o = data->tiles.allocate(budget, Stage::prepared, tileCount, sizeof(OutlineTile), fault, stop);
        if (!o.ok()) return {o};
        auto tiles = reinterpret_cast<OutlineTile *>(data->tiles.data());
        std::fill_n(tiles, tileCount, OutlineTile{});
        // Split only at source tile boundaries; these collinear subdivisions
        // preserve scan's exact foreground partition, holes, and piece identity.
        auto split = [&](OutlineSegment a, auto emit) {
            if (a.x == a.endX) return emit(a);
            while (a.x < a.endX) {
                auto b = a; b.endX = std::min(a.endX, (a.x/outlineTileSize+1)*outlineTileSize);
                if (!emit(b)) return false;
                a.x = b.endX;
            }
            return true;
        };
        auto key = [&](OutlineSegment a) { return std::uint64_t(a.y/outlineTileSize)*data->columns + a.x/outlineTileSize; };
        std::uint64_t count = 0;
        auto cap = std::min(segmentBudget, (outlineByteLimit-overhead)/sizeof(OutlineSegment));
        o = scan(p, work, stop, [&](auto a) { return split(a, [&](auto b) {
            if (++count > cap) return false;
            ++tiles[key(b)].count; return true;
        }); });
        if (!o.ok()) return {o};
        std::uint64_t offset = 0;
        for (std::uint64_t i=0; i<tileCount; ++i) {
            tiles[i].begin = offset; offset += tiles[i].count; tiles[i].count = 0;
        }
        o = data->segments.allocate(budget, Stage::prepared, count, sizeof(OutlineSegment), fault, stop);
        if (!o.ok()) return {o};
        o = data->pieces.allocate(budget, Stage::prepared, p.pieceCount, sizeof(Piece), fault, stop);
        if (!o.ok()) return {o};
        auto out = reinterpret_cast<OutlineSegment *>(data->segments.data());
        o = scan(p, work, stop, [&](auto a) { return split(a, [&](auto b) {
            auto &tile = tiles[key(b)]; out[tile.begin + tile.count++] = b; return true;
        }); });
        if (!o.ok()) return {o};
        std::copy_n(p.pieces(), p.pieceCount, reinterpret_cast<Piece *>(data->pieces.data()));
        data->count = count; data->pieceCount = p.pieceCount; data->pixelToDocument = grid.pixelToDocument;
        data->bounds={p.width,p.height,0,0};
        PhaseTimer geometryTimer;
        for (std::uint64_t i = 0; i < count; ++i) {
            o = geometryTimer.check(stop); if (!o.ok()) return {o};
            o = work.advance(1); if (!o.ok()) return {o};
            auto a = out[i];
            data->bounds[0]=std::min(data->bounds[0],a.x); data->bounds[1]=std::min(data->bounds[1],a.y);
            data->bounds[2]=std::max(data->bounds[2],a.endX); data->bounds[3]=std::max(data->bounds[3],a.endY);
            data->horizontalLength+=a.endX-a.x; data->verticalLength+=a.endY-a.y;
            for (auto q : {Geom::Point(a.x, a.y) * matrix(grid.pixelToDocument), Geom::Point(a.endX, a.endY) * matrix(grid.pixelToDocument)})
                if (!std::isfinite(q.x()) || !std::isfinite(q.y()) || std::abs(q.x()) > 1e7 || std::abs(q.y()) > 1e7)
                    return {{Status::incompatible, "Invalid outline placement."}};
        }
        ExactOutlines result; result.storage = std::move(data);
        return {{}, std::move(result)};
    } catch (...) { return {{Status::failed, "Could not allocate solid outlines."}}; }
}
struct BitmapOverlay::State : XML::NodeObserver {
    static State *owners;
    State *next = owners;
    std::thread::id main = std::this_thread::get_id();
    SPDesktop *desktop = nullptr;
    XML::Node *root = nullptr;
    Generation generation = 0;
    bool accepting = false;
    DependencyToken token;
    std::shared_ptr<Drawing> drawing;
    CanvasItemPtr<OutlineItem> item;
    sigc::connection destroyed, replaced, closed, selected, modified, tool, viewChanged, viewRemoved, dependencyChanged;
    sigc::signal<void (OutlineVisibility)> visibility;
    sigc::connection visibilityChanged;
    OverlayTestHooks *hooks;
    State(OverlayTestHooks *h) : hooks(h) { owners = this; }
    void checkThread() const { g_assert(std::this_thread::get_id() == main); }
    void clear() noexcept {
        checkThread(); accepting = false;
        dependencyChanged.disconnect();
        if (drawing) drawing->active.store(false, std::memory_order_release);
        try { if (item) item->replace(nullptr); } catch (...) {} // gate already retired; later replacement retries
        visibilityChanged.disconnect();
        drawing.reset(); token = {};
    }
    void detach() noexcept {
        clear();
        if (auto retired = item.release()) { try { retired->unlink(); } catch (...) {} } // parent retains it on failure
        destroyed.disconnect(); replaced.disconnect(); closed.disconnect();
        selected.disconnect(); modified.disconnect(); tool.disconnect(); viewChanged.disconnect(); viewRemoved.disconnect();
        if (root) root->removeSubtreeObserver(*this);
        root = nullptr; desktop = nullptr;
    }
    ~State() override {
        detach(); auto ptr = &owners; while (*ptr != this) ptr = &(*ptr)->next; *ptr = next;
    }
    void bind(SPDesktop &d) {
        desktop = &d; root = d.getDocument()->getReprRoot(); root->addSubtreeObserver(*this);
        destroyed = d.connectDestroy([this](auto) { detach(); });
        replaced = d.connectDocumentReplaced([this](auto, auto) { detach(); });
        closed = d.getDocument()->connectDestroy([this] { detach(); });
        selected = d.getSelection()->connectChanged([this](auto) {
            // The panel uses the same captured-dependency predicate in current().
            // It compares live selection identity, so an identical notification
            // retains installed outlines while a real selection change retires them.
            if (!valid(token)) clear();
        });
        modified = d.getSelection()->connectModifiedFirst([this](auto, auto) { clear(); });
        tool = d.connectEventContextChanged([this](auto, auto tool) { if (!compatibleExplodeBitmapTool(tool)) clear(); });
        auto view = d.getCanvasDrawing()->get_drawing();
        viewChanged = view->connectDrawingUpdated([this] { if (drawing && !valid(token)) clear(); });
        viewRemoved = view->connectItemDeleted([this](auto) { clear(); });
    }
    void notifyChildAdded(XML::Node &, XML::Node &, XML::Node *) override { clear(); }
    void notifyChildRemoved(XML::Node &, XML::Node &, XML::Node *) override { clear(); }
    void notifyChildOrderChanged(XML::Node &, XML::Node &, XML::Node *, XML::Node *) override { clear(); }
    void notifyAttributeChanged(XML::Node &, GQuark, Util::ptr_shared, Util::ptr_shared) override { clear(); }
    void notifyContentChanged(XML::Node &, Util::ptr_shared, Util::ptr_shared) override { clear(); }
    void notifyElementNameChanged(XML::Node &, GQuark, GQuark) override { clear(); }
};
BitmapOverlay::State *BitmapOverlay::State::owners = nullptr;
BitmapOverlay::BitmapOverlay(OverlayTestHooks *hooks) : _state(std::make_unique<State>(hooks)) {}
BitmapOverlay::~BitmapOverlay() = default;
void BitmapOverlay::clear() noexcept { _state->clear(); }
Outcome BitmapOverlay::beginGeneration(SPDesktop &d, Generation generation)
{
    auto &s = *_state; s.checkThread();
    if (!generation || generation <= s.generation) return stale;
    s.clear(); s.generation = generation;
    if (!d.getDocument() || !d.getCanvasTemp()) return {Status::unavailable, "Bitmap view is unavailable."};
    for (auto owner = State::owners; owner; owner = owner->next)
        if (owner != &s && owner->desktop == &d) return {Status::unavailable, "This view already owns a bitmap overlay."};
    try { if (s.desktop != &d) { s.detach(); s.bind(d); } }
    catch (...) { s.detach(); return {Status::failed, "Could not observe bitmap view."}; }
    s.accepting = true; return {};
}
Outcome BitmapOverlay::installOutlines(SPDesktop &d, ExactOutlines const &outlines, Generation generation)
{
    auto &s = *_state; s.checkThread();
    if (!s.accepting || s.desktop != &d || generation != s.generation || generation != outlines.generation) return stale;
    s.clear(); // replacement/refusal cannot leave the previous ready preview visible
    if (!valid(outlines.dependencies)) return stale;
    auto const &data = outlines.storage;
    if (outlines.approximate || !data || !data->count || !data->pieceCount)
        return {Status::unavailable, "Only a current exact final-grid result with at least one piece is ready."};
    if (!invertible(matrix(data->pixelToDocument) * d.doc2dt() * d.d2w())) return overflow;
    if (!data->index() || data->count > outlineSegmentLimit || data->bytes() > outlineByteLimit) return overflow;
    auto image = cast<SPImage>(d.getSelection()->singleItem());
    auto view = image ? cast<DrawingImage>(image->get_arenaitem(d.dkey)) : nullptr;
    if (!view) return stale;
    try {
        s.dependencyChanged = view->connectDependencyChanged([&s]() noexcept { s.clear(); });
        s.drawing = std::make_shared<Drawing>(); s.drawing->storage = data;
        s.drawing->doc2dt = d.doc2dt(); s.drawing->hooks = s.hooks;
        if (s.item) s.item->replace(s.drawing);
        else s.item = make_canvasitem<OutlineItem>(d.getCanvasTemp(), s.drawing);
        s.visibilityChanged = s.item->visibility.connect([&s](auto state) { s.visibility.emit(state); });
        s.token = outlines.dependencies;
        return {Status::changed, "Exact solid outlines ready."};
    } catch (...) { s.clear(); return {Status::failed, "Could not install solid outlines."}; }
}
bool BitmapOverlay::ready() const {
    auto &s = *_state; s.checkThread();
    if (s.drawing && (!valid(s.token) || !s.drawing->active.load(std::memory_order_acquire))) s.clear();
    return s.drawing && s.drawing->active.load(std::memory_order_acquire);
}
OutlineCamera BitmapOverlay::camera() const { _state->checkThread(); return _state->drawing ? _state->item->camera() : OutlineCamera{}; }
bool BitmapOverlay::cameraCallbackRegistered() const { _state->checkThread(); return _state->item && _state->item->cameraCallbackRegistered(); }
sigc::connection BitmapOverlay::connectVisibility(sigc::slot<void (OutlineVisibility)> const &slot) { return _state->visibility.connect(slot); }
CanvasItem *BitmapOverlay::canvasItem() const { _state->checkThread(); return _state->drawing ? _state->item.get() : nullptr; }
Outcome BitmapOverlay::zoomToPiece(std::uint32_t piece, Generation generation)
{
    auto &s = *_state; s.checkThread();
    if (generation != s.generation || !ready()) return stale;
    auto const &data = *s.drawing->storage;
    if (piece >= data.pieceCount) return {Status::incompatible, "No such piece."};
    auto p = reinterpret_cast<Piece const *>(data.pieces.data())[piece];
    auto bounds = Geom::Rect(Geom::Point(p.x, p.y), Geom::Point(p.endX, p.endY));
    bounds *= matrix(data.pixelToDocument) * s.desktop->doc2dt();
    s.desktop->set_display_area(bounds, 32);
    return {Status::changed, "Showing selected piece."};
}
} // namespace Inkscape::Bitmap

// Contour preview: immutable fitted curves survive frozen canvas readers; only
// the atomic retirement flag crosses the main/render-thread boundary.
#include "ui/explode-bitmap-panel-preparation.h"
namespace Inkscape::Bitmap {
namespace {
struct ContourDrawing {
    std::shared_ptr<PanelPreparation::ContourProduct const> product;
    Geom::Affine pixelToDesktop;
    std::uint32_t rgba;
    std::atomic<bool> active{true};
};
class ContourItem final : public CanvasItem {
public:
    ContourItem(CanvasItemGroup *parent, std::shared_ptr<ContourDrawing> drawing)
        : CanvasItem(parent), _drawing(std::move(drawing)) { _name = "Explode Bitmap contours"; request_update(); }
    void renderPieceForTest(CanvasItemBuffer &buf, unsigned piece) const {
        if (piece < _drawing->product->fitted.pieceCount) renderPieces(buf, piece, piece + 1);
    }
private:
    std::shared_ptr<ContourDrawing> _drawing;
    void _update(bool) override {
        request_redraw(); _bounds = {};
        auto const &f = _drawing->product->fitted;
        auto m = _drawing->pixelToDesktop * affine();
        // Include cubic control points: conservative even with overshooting fits.
        auto include = [&](ContourPoint p) {
            auto q = Geom::Point(p.x, p.y) * m;
            if (_bounds) _bounds->expandTo(q); else _bounds = Geom::Rect(q, q);
        };
        for (unsigned r = 0; r < f.ringCount; ++r) include(f.rings()[r].start);
        for (unsigned i = 0; i < f.segmentCount; ++i) {
            auto const &s = f.segments()[i]; include(s.end);
            if (s.cubic) { include(s.c1); include(s.c2); }
        }
        if (_bounds) _bounds->expandBy(3);
        request_redraw();
    }
    void _render(CanvasItemBuffer &buf) const override {
        renderPieces(buf, 0, _drawing->product->fitted.pieceCount);
    }
    void renderPieces(CanvasItemBuffer &buf, unsigned begin, unsigned end) const {
        auto const &d = *_drawing;
        if (!d.active.load(std::memory_order_acquire)) return;
        auto const &f = d.product->fitted;
        auto m = d.pixelToDesktop * affine();
        auto point = [&](ContourPoint p) { return Geom::Point(p.x, p.y) * m - buf.rect.min(); };
        auto cr = buf.cr; cr->save(); cr->push_group();
        cr->set_dash(std::vector<double>{}, 0);
        cr->set_line_width(1.5 / std::max(1, buf.device_scale));
        cr->set_line_join(Cairo::Context::LineJoin::ROUND);
        cr->set_source_rgba((d.rgba >> 24)/255.0, ((d.rgba >> 16)&255)/255.0,
                            ((d.rgba >> 8)&255)/255.0, (d.rgba&255)/255.0);
        for (unsigned i = begin; i < end && d.active.load(std::memory_order_acquire); ++i) {
            auto const &piece = f.pieces()[i]; cr->begin_new_path();
            for (unsigned r = piece.ringBegin; r < piece.ringEnd; ++r) {
                auto const &ring = f.rings()[r]; auto p = point(ring.start); cr->move_to(p.x(), p.y());
                for (unsigned j = ring.begin; j < ring.end; ++j) {
                    auto const &s = f.segments()[j]; auto e = point(s.end);
                    if (s.cubic) { auto a = point(s.c1), b = point(s.c2); cr->curve_to(a.x(), a.y(), b.x(), b.y(), e.x(), e.y()); }
                    else cr->line_to(e.x(), e.y());
                }
                cr->close_path();
            }
            cr->stroke(); // one compound path per piece, holes included
        }
        auto tile = cr->pop_group();
        if (d.active.load(std::memory_order_acquire)) { cr->set_source(tile); cr->paint(); }
        cr->restore();
    }
};
}
struct ContourOverlay::State : XML::NodeObserver {
    SPDesktop *desktop = nullptr;
    XML::Node *root = nullptr;
    Generation generation = 0;
    bool accepting = false;
    DependencyToken token;
    std::shared_ptr<ContourDrawing> drawing;
    CanvasItemPtr<ContourItem> item;
    sigc::connection destroyed, replaced, closed, selected, modified, tool, viewChanged, viewRemoved, dependencyChanged;
    void clear() noexcept {
        accepting = false; dependencyChanged.disconnect();
        if (drawing) drawing->active.store(false, std::memory_order_release);
        if (auto retired = item.release()) { try { retired->unlink(); } catch (...) {} }
        drawing.reset(); token = {};
    }
    void detach() noexcept {
        clear(); destroyed.disconnect(); replaced.disconnect(); closed.disconnect();
        selected.disconnect(); modified.disconnect(); tool.disconnect(); viewChanged.disconnect(); viewRemoved.disconnect();
        if (root) root->removeSubtreeObserver(*this);
        root = nullptr; desktop = nullptr;
    }
    ~State() override { detach(); }
    void bind(SPDesktop &d) {
        desktop = &d; root = d.getDocument()->getReprRoot(); root->addSubtreeObserver(*this);
        destroyed = d.connectDestroy([this](auto) { detach(); });
        replaced = d.connectDocumentReplaced([this](auto, auto) { detach(); });
        closed = d.getDocument()->connectDestroy([this] { detach(); });
        selected = d.getSelection()->connectChanged([this](auto) { if (!valid(token)) clear(); });
        modified = d.getSelection()->connectModifiedFirst([this](auto, auto) { clear(); });
        tool = d.connectEventContextChanged([this](auto, auto tool) { if (!compatibleExplodeBitmapTool(tool)) clear(); });
        auto view = d.getCanvasDrawing()->get_drawing();
        viewChanged = view->connectDrawingUpdated([this] { if (drawing && !valid(token)) clear(); });
        viewRemoved = view->connectItemDeleted([this](auto) { clear(); });
    }
    void notifyChildAdded(XML::Node &, XML::Node &, XML::Node *) override { clear(); }
    void notifyChildRemoved(XML::Node &, XML::Node &, XML::Node *) override { clear(); }
    void notifyChildOrderChanged(XML::Node &, XML::Node &, XML::Node *, XML::Node *) override { clear(); }
    void notifyAttributeChanged(XML::Node &, GQuark, Util::ptr_shared, Util::ptr_shared) override { clear(); }
    void notifyContentChanged(XML::Node &, Util::ptr_shared, Util::ptr_shared) override { clear(); }
    void notifyElementNameChanged(XML::Node &, GQuark, GQuark) override { clear(); }
};
ContourOverlay::ContourOverlay() : _state(std::make_unique<State>()) {}
ContourOverlay::~ContourOverlay() = default;
void ContourOverlay::clear() noexcept { _state->clear(); }
Outcome ContourOverlay::beginGeneration(SPDesktop &d, Generation generation) {
    auto &s = *_state;
    if (!generation || generation <= s.generation) return stale;
    s.clear(); s.generation = generation;
    if (!d.getDocument() || !d.getCanvasTemp()) return {Status::unavailable, "Bitmap view is unavailable."};
    try { if (s.desktop != &d) { s.detach(); s.bind(d); } }
    catch (...) { s.detach(); return {Status::failed, "Could not observe contour view."}; }
    s.accepting = true; return {};
}
Outcome ContourOverlay::install(SPDesktop &d, std::shared_ptr<PanelPreparation::ContourProduct const> product,
                               GridTransform const &transform, DependencyToken token, Generation generation, std::uint32_t rgba) {
    auto &s = *_state;
    if (!s.accepting || s.desktop != &d || generation != s.generation) return stale;
    s.clear();
    if (!valid(token)) return stale;
    if (!product) return {Status::unavailable, "No fitted contours."};
    auto image = cast<SPImage>(d.getSelection()->singleItem());
    auto view = image ? cast<DrawingImage>(image->get_arenaitem(d.dkey)) : nullptr;
    if (!view) return stale;
    try {
        s.dependencyChanged = view->connectDependencyChanged([&s]() noexcept { s.clear(); });
        s.drawing = std::make_shared<ContourDrawing>(); s.drawing->product = std::move(product);
        s.drawing->pixelToDesktop = matrix(transform) * d.doc2dt(); s.drawing->rgba = rgba;
        s.item = make_canvasitem<ContourItem>(d.getCanvasTemp(), s.drawing); s.token = std::move(token);
        return {Status::changed, "Contour preview ready."};
    } catch (...) { s.clear(); return {Status::failed, "Could not install contour preview."}; }
}
bool ContourOverlay::ready() const {
    auto &s = *_state;
    if (s.drawing && !valid(s.token)) s.clear();
    return s.drawing && s.drawing->active.load(std::memory_order_acquire);
}
CanvasItem *ContourOverlay::canvasItem() const { return ready() ? _state->item.get() : nullptr; }
void ContourOverlay::renderPieceForTest(CanvasItemBuffer &buf, unsigned piece) const {
    if (ready()) _state->item->renderPieceForTest(buf, piece);
}
} // namespace Inkscape::Bitmap
