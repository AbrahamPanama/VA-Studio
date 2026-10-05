// SPDX-License-Identifier: GPL-2.0-or-later
#include "bitmap-copy-outcome.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <glibmm/i18n.h>
#include "desktop.h"
#include "ui/explode-bitmap-context.h"
#include "document.h"
#include "display/cairo-utils.h"
#include "helper/pixbuf-ops.h"
#include "inkgc/gc-core.h"
#include <stdexcept>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "object/object-set.h"
#include "object/sp-image.h"
#include "object/sp-lpe-item.h"
#include "selection.h"
#include "ui/explode-bitmap-dependencies.h"
#include "object/sp-item-group.h"
#include "object/sp-namedview.h"
#include "object/sp-root.h"
#include "object/sp-use.h"
#include "object/weakptr.h"
#include "selection-chemistry.h"
#include "style.h"
#include "svg/svg.h"
#include "util/bitmap-memory-admission.h"
#include "util/scope_exit.h"
#include "util/units.h"
#include "xml/repr.h"
#include "xml/node-observer.h"

namespace Inkscape::Bitmap {
namespace {
std::uint64_t nextCandidate = 0;
auto const mainThread = std::this_thread::get_id();
std::uintptr_t identity(void const *p) { return reinterpret_cast<std::uintptr_t>(p); }
BitmapCopyOutcome result(Outcome outcome, CliBitmapStage stage, CliBitmapReason reason) {
    BitmapCopyOutcome r; r.outcome = outcome; r.failure = bitmapFailure(outcome, stage, reason); return r;
}
BitmapCopyOutcome result(Status status, char const *why,
                         CliBitmapReason reason = CliBitmapReason::UnsupportedTarget,
                         CliBitmapStage stage = CliBitmapStage::Resolve) {
    return result(Outcome{status, why}, stage, reason);
}
// Safe conversion requires the current token's atomic settlement fence.
bool validAtomicGuard(DocumentUndo::RollbackableInteraction const *guard, SPDocument const *doc) {
    return guard && guard->validAtomicFor(doc);
}
bool effect(SPItem const *i) {
    auto lpe = cast<SPLPEItem>(i);
    return i->isFiltered() || i->getClipObject() || i->getMaskObject() ||
        (lpe && lpe->hasPathEffect()) || (i->style && i->style->opacity.value < SP_SCALE24_MAX);
}
// Stable minimum singular value: det / largest singular value, after normalization.
double minimumScale(Geom::Affine const &m) {
    double n = std::max({std::abs(m[0]), std::abs(m[1]), std::abs(m[2]), std::abs(m[3])});
    if (!std::isfinite(n) || n <= 0) return 0;
    double a = m[0]/n, b = m[1]/n, c = m[2]/n, d = m[3]/n;
    double q = a*a + b*b + c*c + d*d, det = std::abs(a*d-b*c);
    double hi = std::sqrt((q + std::sqrt(std::max(0.0, q*q-4*det*det))) / 2);
    return hi && det && hi*hi/det <= 1e6 ? n*det/hi : 0;
}
Outcome refresh(Budget &budget, CandidateHooks const &hooks) {
    auto sample = hooks.memory ? sampleMemory(*hooks.memory) : sampleMemory();
    return sample.ok() ? budget.recheck(sample.value) : sample.outcome;
}
// A separate publication epoch: source removals and one image insertion are expected;
// callback edits anywhere else reject, rather than suppressing dependency observation.
struct PublicationAudit : XML::NodeObserver {
    XML::Node *root, *parent, *image;
    std::vector<XML::Node *> sources;
    bool unexpected=false, inserted=false, assignedId=false, deleting=false;
    PublicationAudit(SPDocument &doc, XML::Node *p, XML::Node *i, std::vector<SPItem *> const &items)
        : root(doc.getReprRoot()),parent(p),image(i) {
        sources.reserve(items.size()); for(auto item:items) sources.push_back(item->getRepr());
        root->addSubtreeObserver(*this);
    }
    ~PublicationAudit() override { root->removeSubtreeObserver(*this); }
    void notifyChildAdded(XML::Node &p, XML::Node &c, XML::Node *) override {
        if (&p!=parent || &c!=image || inserted) unexpected=true; inserted=true;
    }
    void notifyChildRemoved(XML::Node &p, XML::Node &c, XML::Node *) override {
        auto found=std::find(sources.begin(),sources.end(),&c);
        if (!deleting || &p!=parent || found==sources.end()) unexpected=true;
        else sources.erase(found);
    }
    void notifyAttributeChanged(XML::Node &n,GQuark key,Util::ptr_shared,Util::ptr_shared) override {
        if (&n!=image || std::strcmp(g_quark_to_string(key),"id") || assignedId) unexpected=true;
        assignedId=true;
    }
    void notifyChildOrderChanged(XML::Node &,XML::Node &,XML::Node *,XML::Node *) override { unexpected=true; }
    void notifyContentChanged(XML::Node &,Util::ptr_shared,Util::ptr_shared) override { unexpected=true; }
    void notifyElementNameChanged(XML::Node &,GQuark,GQuark) override { unexpected=true; }
};

}
struct PreparedBitmapCopy::State {
    BitmapCopyMetadata meta;
    BitmapCopyOptions options;
    PlacementPolicy policy;
    CandidateHooks hooks;
    Budget *budget = nullptr;
    SPDocument *document = nullptr;
    unsigned long serial = 0;
    sigc::connection destroyed;
    SPWeakPtr<SPObject> parent, after;
    std::vector<SPWeakPtr<SPItem>> selected;
    std::vector<std::string> selectionIds;
    Geom::Rect bounds;
    std::string transform, href;
    Budget::Token storage;
    RenderOutcome render; // pixels die before storage
    bool consumed = false, request = false;
    ~State() { destroyed.disconnect(); }
};
PreparedBitmapCopy::PreparedBitmapCopy() = default;
PreparedBitmapCopy::~PreparedBitmapCopy() = default;
PreparedBitmapCopy::PreparedBitmapCopy(PreparedBitmapCopy &&) noexcept = default;
PreparedBitmapCopy &PreparedBitmapCopy::operator=(PreparedBitmapCopy &&) noexcept = default;
BitmapCopyMetadata const &PreparedBitmapCopy::metadata() const {
    static BitmapCopyMetadata const empty; return _state ? _state->meta : empty;
}
Pixbuf const *PreparedBitmapCopy::pixels() const { return _state ? _state->render.pixbuf.get() : nullptr; }
RgbaView PreparedBitmapCopy::rgba() const {
    auto p=pixels(); if (!p || p->pixelFormat()!=Pixbuf::PF_GDK) return {};
    auto raw=p->getPixbufRaw(); auto stride=gdk_pixbuf_get_rowstride(raw);
    return {gdk_pixbuf_get_pixels(raw),std::uint64_t(stride)*p->height(),std::uint64_t(stride),
            unsigned(p->width()),unsigned(p->height())};
}

Result<CandidateGridInput> PreparedBitmapCopy::gridInput(Budget &budget, Stop stop) const noexcept try {
    Result<CandidateGridInput> out;
    auto fail = [&](Status status, char const *why) { out.value={}; out.outcome = {status,why}; return std::move(out); };
    if (std::this_thread::get_id()!=mainThread || !_state || _state->consumed || !_state->document ||
        _state->policy!=PlacementPolicy::ContiguousReplacement || !valid(_state->meta.token,*_state->document))
        return fail(Status::unavailable,"Candidate geometry is stale or unavailable.");
    auto const &s=*_state; auto const &m=s.meta; auto view=rgba();
    if (!view.validate().ok()) return fail(Status::failed,"Candidate RGBA is unavailable.");
    auto &input=out.value; input.identity=m.candidateIdentity;
    auto &g=input.geometry; g.mode=TargetMode::SingleBitmap; g.supportability=Supportability::Supported;
    // Zero denotes an off-document image, never a forged live SPImage address.
    g.bitmap=0; g.destinationParent=m.parent;
    auto parent=cast<SPItem>(s.parent.get());
    if (!parent) return fail(Status::unavailable,"Candidate parent was released.");
    auto matrix=parent->i2doc_affine();
    TargetContext own, destination; own.parent=m.parent; destination.identity=m.parent;
    own.itemToDocument={1,0,0,1,s.bounds.left(),s.bounds.top()};
    own.pixelToItem={s.bounds.width()/m.width,0,0,s.bounds.height()/m.height,0,0};
    own.viewport={0,0,s.bounds.width(),s.bounds.height()};
    for (unsigned k=0;k<6;++k) destination.itemToDocument[k]=matrix[k];
    g.contexts={own,destination};
    auto &r=input.raster; r.width=r.sourceWidth=m.width; r.height=r.sourceHeight=m.height;
    r.hadAlpha=true; r.format=Format::PNG;
    auto allocated=r.pixels.allocate(budget,Stage::prepared,std::uint64_t(m.width)*m.height,4,nullptr,stop);
    if (!allocated.ok()) return fail(allocated.status,allocated.diagnostic);
    for (unsigned y=0;y<m.height;++y) {
        if (stop.requested()) return fail(Status::canceled,"Candidate capture canceled.");
        std::memcpy(r.pixels.data()+std::uint64_t(y)*m.width*4,view.data+y*view.stride,std::size_t(m.width)*4);
    }
    out.outcome={Status::changed,"Candidate grid input captured."}; return out;
} catch (...) { Result<CandidateGridInput> out; out.outcome={Status::failed,"Candidate capture failed."}; return out; }

BitmapCopyOutcome prepareBitmapCopyCore(ObjectSet &set, BitmapCopyOptions const &options,
                                    PlacementPolicy policy, Budget &budget, CandidateHooks hooks,
                                    BitmapCopyRequestOptions const *request, DocumentPublicationContext *context) try {
    if (std::this_thread::get_id() != mainThread) return result(Status::unavailable, "Prepare bitmap copy on the main thread.");
    auto doc = set.document();
    if (!doc) return result(Status::unavailable, "No document.");
    if (set.isEmpty()) return result(Status::unchanged, "Empty selection.");
    bool resize = policy == PlacementPolicy::SingleImageResize;
    bool safe = request || policy != PlacementPolicy::NativeCopy;
    bool replacement = policy != PlacementPolicy::NativeCopy;
    if (safe && !context && (!set.desktop() || set.desktop()->getSelection() != &set))
        return result(Status::unavailable, "Conversion requires the desktop selection.");
    // Safe preparation must not settle pending unrelated XML/automatic updates.
    if (safe && (doc->getRoot()->uflags || doc->getRoot()->mflags))
        return result(Status::unavailable, "Settle pending document updates before conversion.", CliBitmapReason::DocumentBusy);
    if (!safe) doc->ensureUpToDate();
    auto s = std::make_unique<PreparedBitmapCopy::State>();
    s->document = doc; s->serial = doc->serial(); s->budget = &budget;
    s->destroyed = doc->connectDestroy([state = s.get()] { state->document = nullptr; });
    s->options = options; s->policy = policy; s->hooks = hooks; s->request = request;
    s->hooks.renderLimits = nullptr; // Only prepare borrows the caller's admission limits.
    auto &m = s->meta;
    if (safe) {
        if (nextCandidate == UINT64_MAX) return result(Status::unavailable,"Candidate identities exhausted.");
        m.candidateIdentity = ++nextCandidate;
    }
    std::vector<SPItem const *> items;
    if (safe) {
        auto intent = request && request->keep_original ? Intent::BitmapCopy : resize ? Intent::Explode : Intent::ConversionCandidate;
        auto targets = context ? resolve(*context, intent) : resolve(*set.desktop(), intent);
        if (!targets.ok()) {
            auto out=result(targets.outcome.status, targets.outcome.diagnostic);
            out.refusals=std::move(targets.value.refusals);
            if (std::any_of(out.refusals.begin(),out.refusals.end(),[](auto const &r) { return r.reason==Refusal::MissingSource; }))
                out.failure=CliBitmapFailure{CliBitmapStage::Resolve,CliBitmapReason::MissingSource};
            return out;
        }
        if (resize && targets.value.mode != TargetMode::SingleBitmap)
            return result(Status::incompatible, "Resize requires one selected bitmap.");
        m.token = std::move(targets.value);
        for (auto p : m.token.roots) items.push_back(reinterpret_cast<SPItem const *>(p));
        s->options.keep_original = request && request->keep_original; s->options.transparent = true; s->options.commit_undo = false;
    } else {
        auto range = set.items(); items.assign(range.begin(), range.end());
    }
    std::sort(items.begin(), items.end(), sp_item_repr_compare_position_bool);
    if (safe) for (auto item:items) s->selected.emplace_back(const_cast<SPItem *>(item));
    else for (auto item:set.items()) s->selected.emplace_back(item);
    // Use the native selection bounds, including its original parent/child handling.
    auto bbox = set.documentBounds(request && request->bounds == CopyBounds::Geometric ? SPItem::GEOMETRIC_BBOX : SPItem::VISUAL_BBOX);
    if (!bbox || bbox->width()<=0 || bbox->height()<=0 || !bbox->isFinite()) return result(Status::unchanged, "Selection has no visible bounds.");
    s->bounds = *bbox;
    auto top = items.back();
    s->parent = top->parent; s->after = const_cast<SPItem *>(top);
    for (auto p = top->parent; p && p != doc->getRoot(); p = p->parent) {
        if (auto i = cast<SPItem>(p); i && effect(i)) {
            if (replacement) return result(Status::incompatible, "Cannot convert this selection without changing its layer or stacking order. Select contiguous objects in one layer without ancestor effects.");
            s->parent = p->parent; s->after = p;
        }
    }
    // All selected roots' ancestor contexts, not just the topmost root.
    if (replacement) for (auto const &c : m.token.contexts) {
        if (std::find(m.token.roots.begin(), m.token.roots.end(), c.identity) == m.token.roots.end() &&
            std::any_of(items.begin(), items.end(), [&](auto i) { for (auto p=i->parent; p; p=p->parent) if (identity(p)==c.identity) return true; return false; }) &&
            (c.opacity != 1 || c.clip || c.mask || c.filter))
            return result(Status::incompatible, "Ancestor effects cannot be retained without double baking.");
    }
    auto parentItem = cast<SPItem>(s->parent.get());
    if (!parentItem) return result(Status::incompatible, "No bitmap destination parent.");
    m.parent = identity(s->parent.get());
    for (auto p=s->parent.get(); p; p=p->parent) if (auto g=cast<SPGroup>(p); g && g->layerMode()==SPGroup::LAYER) { m.layer=identity(p); break; }
    m.slot = replacement ? items.front()->getRepr()->position() : s->after->getRepr()->position()+1;
    std::size_t renderNodes=items.size()+1;
    m.requestedDpi = std::clamp(options.dpi, 1, BitmapCopyOptions::max_dpi);
    if (safe) {
        double native = 0;
        // Walk rendered instances (including clone shadow children), so a scaled clone
        // uses its own pixel-to-document matrix rather than its original's placement.
        std::vector<SPObject const *> pending(items.begin(),items.end());
        for (std::size_t k=0;k<pending.size();++k) {
            auto object=pending[k];
            if (auto lpe=cast<SPLPEItem>(object); lpe && lpe->hasPathEffect() && (!request || !request->keep_original))
                return result(Status::incompatible,"Live path-effect conversion requires qualified dependency preparation.");
            if (pending.size()>20'000) return result(Status::unavailable,"Candidate instance graph exceeds admission limits.");
            if (auto image=cast<SPImage>(object)) {
                auto scale=minimumScale(image->c2p * image->i2doc_affine());
                if (!(scale>0)) return result(Status::incompatible,"Unsafe pixel-to-document transform.");
                native=std::max(native,96.0/scale);
            }
            for (auto const &child:object->children) {
                if (pending.size()>=20'000) return result(Status::unavailable,"Candidate instance graph exceeds admission limits.");
                pending.push_back(&child);
            }
        }
        renderNodes=pending.size()+1;
        if (!resize && !request) m.requestedDpi = native ? native : std::max(300.0, double(options.dpi));
    }
    if (request) m.requestedDpi = std::holds_alternative<Dpi>(request->sizing) ? std::get<Dpi>(request->sizing).value : 96;
    m.renderDpi = request ? m.requestedDpi : std::min(m.requestedDpi, double(BitmapCopyOptions::max_dpi));
    m.clamped = m.renderDpi < m.requestedDpi;
    m.resolutionReason = m.clamped ? "Native resolution exceeds the 600 dpi ceiling." : "";
    if (!request && !resize && m.renderDpi == 96) s->bounds = s->bounds.roundOutwards();
    // Preserve the renderer's floating-point operation order at fractional boundaries.
    double const scale = Util::Quantity::convert(m.renderDpi, "px", "in");
    double w = std::ceil(scale*s->bounds.width()), h = std::ceil(scale*s->bounds.height());
    if (request && std::holds_alternative<ExactPixels>(request->sizing)) {
        auto size = std::get<ExactPixels>(request->sizing); w = size.width; h = size.height;
    }
    if (hooks.renderLimits && std::isfinite(w) && std::isfinite(h) && w >= 1 && h >= 1 && w <= UINT_MAX && h <= UINT_MAX) {
        LatencyWork work; work.width = w; work.height = h;
        // Use the renderer's actual ceil grid, not source pixels or a unit cost.
        work.render = std::uint64_t(work.width) * work.height;
        // Report the size domain before the aggregate render-work refusal.
        if (w > MaxExplodeSourceAxis || h > MaxExplodeSourceAxis || w > hooks.renderLimits->sourceAxis || h > hooks.renderLimits->sourceAxis) {
            auto out = result(Status::unavailable, ExplodeSourceSizeMessage);
            out.refusedWidth = w; out.refusedHeight = h; return out;
        }
        auto admitted = admitLatency(work, *hooks.renderLimits);
        if (!admitted.ok()) return result(admitted.status, admitted.diagnostic);
    }
    if (!std::isfinite(w) || !std::isfinite(h) || w<1 || h<1 || w>32767 || h>32767 ||
        (safe && (w>16384 || h>16384 || w*h>100'000'000)))
        return result(Status::unavailable, "Candidate grid exceeds raster limits; resolution reduction requires explicit confirmation.", CliBitmapReason::EngineLimit);
    m.width = w; m.height = h;
    m.widthMm = s->bounds.width()*25.4/96; m.heightMm = s->bounds.height()*25.4/96;
    m.dpiX = m.width*25.4/m.widthMm; m.dpiY = m.height*25.4/m.heightMm;
    auto parentMatrix=parentItem->i2doc_affine();
    if (safe && !minimumScale(parentMatrix)) return result(Status::incompatible,"Unsafe parent inverse.");
    auto placement = Geom::Translate(s->bounds.min()) * parentMatrix.inverse();
    if (safe) {
        if (!minimumScale(parentItem->i2doc_affine()) ||
            std::max({std::abs(s->bounds.left()),std::abs(s->bounds.right()),std::abs(s->bounds.top()),std::abs(s->bounds.bottom())})>1e7)
            return result(Status::incompatible,"Unsafe candidate placement geometry.");
        auto roundtrip=placement*parentMatrix;
        for (unsigned k=0;k<6;++k) { double expected=k<4 ? (k==0 || k==3 ? 1 : 0) : s->bounds.min()[k-4];
            if (!std::isfinite(roundtrip[k]) || std::abs(roundtrip[k]-expected)>1e-10*std::max(1.0,std::abs(expected)))
                return result(Status::incompatible,"Candidate placement inverse cannot preserve the confirmed grid.");
        }
        s->transform="matrix(";
        for (unsigned k=0;k<6;++k) { char v[G_ASCII_DTOSTR_BUF_SIZE];
            if (!std::isfinite(placement[k])) return result(Status::incompatible,"Non-finite candidate placement.");
            g_ascii_formatd(v,sizeof(v),"%.17g",placement[k]); if(k) s->transform+=","; s->transform+=v;
        }
        s->transform+=")";
    } else s->transform = sp_svg_transform_write(placement);
    for (auto i : set.items()) s->selectionIds.emplace_back(i->getId() ? i->getId() : "");
    RenderRequest req; req.document = doc; req.area = s->bounds; req.dpi = m.renderDpi; req.items = items;
    req.fit_to_pixel_grid = resize || request;
    if (request && std::holds_alternative<ExactPixels>(request->sizing)) {
        auto size = std::get<ExactPixels>(request->sizing); req.exact_size = std::array<unsigned,2>{size.width,size.height};
    }
    if (!options.antialias || (doc->getNamedView() && !doc->getNamedView()->antialias_rendering)) req.antialias = Antialiasing::None;
    req.create_surface = hooks.createSurface; req.hide_probe = hooks.hideProbe;
    if (safe) {
        auto admission = refresh(budget, hooks); if (!admission.ok()) return result(admission, CliBitmapStage::Analyze, CliBitmapReason::MemoryAdmissionFailed);
        // Reserve PNG worst case, base64 + XML/history copies, new decoded cache and nodes,
        // while renderCommitted separately holds final pixels and intermediate scratch.
        std::uint64_t pixels=0, bytes=0, encoded=0, peak=0;
        if (!checkedMul(m.width,m.height,pixels) || !checkedMul(pixels,4,bytes) ||
            !checkedAdd(bytes,bytes/100+MiB,encoded) || !checkedMul(encoded,12,peak) ||
            !checkedAdd(peak,bytes+4096*renderNodes,peak)) return result(Status::failed,"Candidate memory arithmetic overflow.");
        admission = budget.acquire(Stage::prepared,peak,s->storage);
        if (!admission.ok()) return result(admission, CliBitmapStage::Analyze, CliBitmapReason::MemoryAdmissionFailed);
        s->render = renderCommitted(req,budget);
    } else {
        // Native Copy has no operation ledger (its caller passes Budget(0)), but
        // must use the same OS headroom and recovery reserve before rendering.
        // Preserve its existing renderer, geometry and pixel/size limits.
        auto sample = hooks.memory ? sampleMemory(*hooks.memory) : sampleMemory();
        if (!sample.ok()) return result(sample.outcome, CliBitmapStage::Analyze, CliBitmapReason::MemoryAdmissionFailed);
        std::uint64_t limit = 0, pixels = 0, bytes = 0, encoded = 0, peak = 0;
        auto admission = admissionLimit(sample.value, limit);
        if (!admission.ok()) return result(admission, CliBitmapStage::Analyze, CliBitmapReason::MemoryAdmissionFailed);
        // Same payload/cache estimate as safe Copy; no artificial RAM ceiling.
        if (!checkedMul(m.width, m.height, pixels) || !checkedMul(pixels, 4, bytes) ||
            !checkedAdd(bytes, bytes / 100 + MiB, encoded) || !checkedMul(encoded, 12, peak) ||
            !checkedAdd(peak, bytes + 4096 * renderNodes, peak))
            return result(Status::failed, "Candidate memory arithmetic overflow.");
        if (peak > limit) {
            auto refused = memoryFailure(N_("Bitmap Copy OS headroom"), peak, limit);
            return result(refused, CliBitmapStage::Analyze, CliBitmapReason::MemoryAdmissionFailed);
        }
        s->render.pixbuf.reset(sp_generate_internal_bitmap(doc,s->bounds,m.renderDpi,items,false,nullptr,1,req.antialias));
        s->render.outcome = {s->render.pixbuf ? Status::changed : Status::failed,"Could not render bitmap copy."};
    }
    if (!s->render.ok()) return result(s->render.outcome, CliBitmapStage::Analyze, CliBitmapReason::AnalysisFailed);
    // Metadata and white compositing use the actual rendered grid.
    m.width=s->render.pixbuf->width(); m.height=s->render.pixbuf->height();
    m.dpiX=m.width*25.4/m.widthMm; m.dpiY=m.height*25.4/m.heightMm;
    if (request || !s->options.transparent) {
        auto white = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,m.width,m.height);
        auto cleanup = scope_exit([&] { if (white) cairo_surface_destroy(white); });
        if (cairo_surface_status(white)!=CAIRO_STATUS_SUCCESS) return result(Status::failed,"Could not render white background.");
        auto cr = cairo_create(white);
        auto background = request ? request->background : std::array<double,4>{1,1,1,1};
        cairo_set_source_rgba(cr,background[0],background[1],background[2],background[3]); cairo_paint(cr);
        cairo_set_source_surface(cr,s->render.pixbuf->getSurfaceRaw(),0,0); cairo_paint(cr);
        auto status = cairo_status(cr); cairo_destroy(cr);
        if (status!=CAIRO_STATUS_SUCCESS) return result(Status::failed,"White background render failed.");
        s->render.pixbuf = std::make_unique<Pixbuf>(white); white = nullptr;
    }
    if (safe) {
        auto admission = refresh(budget,hooks); if (!admission.ok()) return result(admission, CliBitmapStage::Analyze, CliBitmapReason::MemoryAdmissionFailed);
        // Canonical straight RGBA is the exact PNG input exposed to downstream preparation.
        s->render.pixbuf->ensurePixelFormat(Pixbuf::PF_GDK);
        auto href = sp_image_encode_png_data_uri(*s->render.pixbuf);
        if (!href) return result(Status::failed,"Candidate PNG preparation failed.", CliBitmapReason::EncodingFailed, CliBitmapStage::Encode);
        s->href = std::move(*href);
        if (!valid(m.token,*doc)) return result(Status::unavailable,"Candidate dependencies changed while rendering.", CliBitmapReason::StaleCapture);
    }
    BitmapCopyOutcome out; out.outcome = {Status::changed,""}; out.candidate._state = std::move(s); return out;
} catch (std::bad_alloc const &) { if (!request && policy==PlacementPolicy::NativeCopy) throw; return result(Status::failed,"Bitmap copy allocation failed.", CliBitmapReason::MemoryAdmissionFailed, CliBitmapStage::Analyze); }
catch (...) { if (!request && policy==PlacementPolicy::NativeCopy) throw; return result(Status::failed,"Bitmap copy preparation failed.", CliBitmapReason::AnalysisFailed, CliBitmapStage::Analyze); }

BitmapCopyOutcome prepareBitmapCopy(ObjectSet &set, BitmapCopyOptions const &options, PlacementPolicy policy,
                                    Budget &budget, CandidateHooks hooks) {
    return prepareBitmapCopyCore(set, options, policy, budget, hooks, nullptr, nullptr);
}
BitmapCopyOutcome prepareBitmapCopy(DocumentPublicationContext &context, BitmapCopyRequestOptions const &request,
                                    Budget &budget, CandidateHooks hooks) {
    if (!context.ownerThread() || !context.getDocument() || context.canceled())
        return result(Status::unavailable,"Bitmap Copy context is unavailable.",CliBitmapReason::StaleCapture);
    if (request.keep_original != (request.placement == PlacementPolicy::NativeCopy) ||
        request.placement == PlacementPolicy::SingleImageResize)
        return result(Status::incompatible,"Invalid request placement policy.");
    if (auto dpi=std::get_if<Dpi>(&request.sizing); dpi && (!std::isfinite(dpi->value) || dpi->value<=0))
        return result(Status::incompatible,"DPI must be finite and positive.");
    if (auto size=std::get_if<ExactPixels>(&request.sizing); size && (!size->width || !size->height))
        return result(Status::incompatible,"Pixel dimensions must be positive.");
    for (auto value:request.background) if(!std::isfinite(value) || value<0 || value>1)
        return result(Status::incompatible,"Background components must be in [0,1].");
    BitmapCopyOptions legacy; legacy.keep_original=request.keep_original; legacy.commit_undo=false;
    return prepareBitmapCopyCore(*context.getSelection(),legacy,request.placement,budget,hooks,&request,&context);
}

BitmapCopyOutcome publishBitmapCopy(ObjectSet &set, PreparedBitmapCopy const &candidate,
                                    DocumentUndo::RollbackableInteraction *guard) try {
    auto result = [](Status status, char const *message,
                     CliBitmapReason reason = CliBitmapReason::PublicationFailed) {
        return Inkscape::Bitmap::result(Outcome{status, message}, CliBitmapStage::Publish, reason);
    };
    auto s = candidate._state.get();
    if (std::this_thread::get_id()!=mainThread || !s || s->consumed || !s->document || !s->parent)
        return result(Status::unavailable,"Bitmap candidate is unavailable or already consumed.", CliBitmapReason::StaleCapture);
    auto doc=s->document; bool safe=s->request || s->policy!=PlacementPolicy::NativeCopy;
    if (set.document()!=doc || doc->serial()!=s->serial) return result(Status::unavailable,"Candidate document changed.", CliBitmapReason::StaleCapture);
    if (safe && !validAtomicGuard(guard,doc)) return result(Status::unavailable,"Conversion requires a caller-owned atomic interaction.", CliBitmapReason::DocumentBusy);
    auto operation=safe ? DocumentUndo::holdInteractionOperation(doc) : std::shared_ptr<void>{};
    if (safe && (!operation || !validAtomicGuard(guard,doc) || !valid(s->meta.token,*doc))) return result(Status::unavailable,"Candidate dependencies changed.", CliBitmapReason::StaleCapture);
    if (safe) { auto a=refresh(*s->budget,s->hooks); if (!a.ok()) return Inkscape::Bitmap::result(a, CliBitmapStage::Publish, CliBitmapReason::MemoryAdmissionFailed); }
    std::vector<SPItem *> originals;
    for (auto const &i:s->selected) { if (!i) return result(Status::unavailable,"Selected source was released.", CliBitmapReason::StaleCapture); originals.push_back(i.get()); }
    // Hold all sources across selection/XML callbacks, including release on deletion.
    for (auto item:originals) sp_object_ref(item,nullptr);
    auto releaseSources=scope_exit([&] { for (auto item:originals) sp_object_unref(item,nullptr); });
    s->consumed=true;
    std::vector<SPItem *> restored; restored.reserve(s->selectionIds.size());
    XML::Node *repr=nullptr;
    auto release=scope_exit([&] { if (repr) GC::release(repr); });
    try {
        repr=doc->getReprDoc()->createElement("svg:image");
        if (safe) { repr->setAttribute("xlink:href",s->href); repr->setAttribute("preserveAspectRatio","none"); }
        else sp_embed_image(repr,s->render.pixbuf.get());
        if (s->request || s->policy == PlacementPolicy::SingleImageResize) {
            // SVG output precision preferences must not round the retained extent.
            char value[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_formatd(value, sizeof(value), "%.17g", s->bounds.width()); repr->setAttribute("width", value);
            g_ascii_formatd(value, sizeof(value), "%.17g", s->bounds.height()); repr->setAttribute("height", value);
        } else {
            repr->setAttributeSvgDouble("width",s->bounds.width()); repr->setAttributeSvgDouble("height",s->bounds.height());
        }
        repr->setAttributeOrRemoveIfEmpty("transform",s->transform);
        std::optional<PublicationAudit> audit;
        if (safe) { audit.emplace(*doc,s->parent->getRepr(),repr,s->options.keep_original ? std::vector<SPItem *>{} : originals);
            if (!validAtomicGuard(guard,doc) || !valid(s->meta.token,*doc)) return result(Status::unavailable,"Candidate changed before publication.", CliBitmapReason::StaleCapture);
            // Delete first in the caller's rollbackable atomic boundary. No duplicate live image.
            set.clear();
            if (audit->unexpected || !validAtomicGuard(guard,doc) || !s->parent)
                throw std::runtime_error("Unexpected conversion callback edit");
            if (!s->options.keep_original) {
                audit->deleting=true;
                ObjectSet deletion(doc); deletion.add(originals.begin(), originals.end()); deletion.deleteItems(true);
                audit->deleting=false;
            }
            if (!validAtomicGuard(guard,doc) || !s->parent || audit->unexpected) throw std::runtime_error("Conversion interrupted");
            s->parent->getRepr()->addChildAtPos(repr,s->meta.slot);
        } else {
            s->parent->getRepr()->addChild(repr,s->after ? s->after->getRepr() : nullptr);
            if (!s->options.keep_original) {
                auto depth=[](SPItem *i) { int d=0; for(auto u=cast<SPUse>(i);u && d<1000;u=cast<SPUse>(u->get_original())) ++d; return d; };
                std::stable_sort(originals.begin(),originals.end(),[&](auto a,auto b) { return depth(a)>depth(b); });
                set.clear();
            ObjectSet deletion(doc); deletion.add(originals.begin(), originals.end()); deletion.deleteItems(true);
            }
        }
        if (s->hooks.afterInsert) s->hooks.afterInsert();
        if (safe && (!validAtomicGuard(guard,doc) || audit->unexpected || !audit->sources.empty())) throw std::runtime_error("Conversion interrupted");
        set.clear(); set.add(repr);
        if (safe) doc->ensureUpToDate(); // qualify the actual decoded image before returning success
        auto image=cast<SPImage>(doc->getObjectByRepr(repr));
        if (!image || (safe && (!validAtomicGuard(guard,doc) || audit->unexpected || image->missing || !image->pixbuf || image->pixbuf->width()!=int(s->meta.width) || image->pixbuf->height()!=int(s->meta.height) || !image->getId() || doc->getObjectById(image->getId())!=image))) throw std::runtime_error("Conversion did not create an image");
        auto out=result(Status::changed,""); out.image=identity(image); out.imageId=image->getId() ? image->getId() : "";
        if (!safe && s->options.commit_undo) DocumentUndo::done(doc,RC_("Undo","Create bitmap"),"selection-make-bitmap-copy");
        return out;
    } catch (...) {
        if (!safe) throw;
        if (safe) {
            guard->rollback();
            // XML restoration recreates object incarnations; restore selection by original ids.
            for (auto const &id:s->selectionIds) if (auto i=cast<SPItem>(doc->getObjectById(id.c_str()))) restored.push_back(i);
            set.clear(); set.add(restored.begin(),restored.end());
        }
        auto failed=result(Status::failed,"Bitmap publication failed; conversion rolled back.");failed.rolledBack=true;return failed;
    }
} catch (...) {
    if (candidate._state && !candidate._state->request && candidate._state->policy==PlacementPolicy::NativeCopy) throw;
    return result(Status::failed,"Bitmap publication allocation failed.", CliBitmapReason::MemoryAdmissionFailed, CliBitmapStage::Publish);
}
} // namespace Inkscape::Bitmap
