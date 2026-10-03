// SPDX-License-Identifier: GPL-2.0-or-later

#include "destructive-bitmap-coverage.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include <thread>
#include <limits>
#include "ui/explode-bitmap-grid.h"
#include "display/preview-render-budget.h"

#include <cairo.h>
#include <glib.h>

#include "display/cairo-utils.h"
#include "display/drawing-context.h"
#include "display/drawing-group.h"
#include "display/drawing-item.h"
#include "display/drawing-surface.h"
#include "display/drawing.h"
#include "enums.h"
#include "object/sp-clippath.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-item-group.h"
#include "object/sp-shape.h"
#include "object/sp-mask.h"
#include "style-enums.h"
#include "style.h"

namespace Inkscape::UI::Tools::DestructiveBitmapCoverage {
namespace {

/**
 * White-content leaf. The native `DrawingItem::render()` pipeline composites
 * this opaque white through the item's own opacity, clip and mask, exactly as
 * it composites a real drawing element. Only the alpha channel is consumed, so
 * source RGB is never sampled or resampled.
 */
class CoverageFillItem final : public Inkscape::DrawingItem
{
public:
    CoverageFillItem(Inkscape::Drawing &drawing, Geom::IntRect const &area)
        : DrawingItem(drawing)
        , _area(area)
    {
    }
    ~CoverageFillItem() override = default;

protected:
    unsigned _updateItem(Geom::IntRect const & /*area*/, UpdateContext const & /*ctx*/,
                         unsigned /*flags*/, unsigned /*reset*/) override
    {
        // The coverage canvas is the whole intrinsic source grid, independent
        // of any particular content geometry.
        _bbox = _area;
        return STATE_ALL;
    }

    unsigned _renderItem(DrawingContext &dc, RenderContext & /*rc*/, Geom::IntRect const &area,
                         unsigned /*flags*/, DrawingItem const * /*stop_at*/) const override
    {
        dc.setSource(1.0, 1.0, 1.0, 1.0);
        dc.rectangle(area);
        dc.fill();
        return RENDER_OK;
    }

private:
    Geom::IntRect _area;
};

bool finite(Geom::Affine const &affine)
{
    for (unsigned i = 0; i < 6; ++i) {
        if (!std::isfinite(affine[i])) {
            return false;
        }
    }
    return true;
}

/**
 * A context that keeps compositing the branch with its siblings once the
 * branch is replaced. Baking the branch may not cross it, so it is reported
 * instead of being dropped or applied twice.
 */
bool has_compositing_context(SPItem const &item)
{
    if (item.isHidden()) {
        // A hidden ancestor above the branch boundary means the canvas renders
        // nothing at all; reject rather than inventing coverage for it.
        return true;
    }
    if (item.isFiltered() || item.getClipObject() || item.getMaskObject()) {
        return true;
    }
    if (item.cloned || item.hrefcount) {
        return true;
    }
    if (item.style) {
        if (item.style->opacity.as_double() < 0.995) {
            return true;
        }
        if (item.style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL) {
            return true;
        }
    }
    return false;
}

/**
 * Effects on a covered chain item itself. Pure opacity, masks and clips are
 * supported; colour-changing filters and blends are not bakeable while keeping
 * source RGB and are reported.
 */
bool has_unsupported_effect(SPItem const &item)
{
    if (item.isFiltered()) {
        return true;
    }
    if (item.style) {
        if (item.style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL) {
            return true;
        }
    }
    return false;
}

/** The next chain element must be the only visible item child of a container. */
bool chain_is_exclusive(SPItem &parent, SPItem &expected)
{
    for (auto &child : parent.children) {
        auto *item = cast<SPItem>(&child);
        if (!item || item == &expected) {
            continue;
        }
        if (!item->isHidden()) {
            return false;
        }
    }
    return true;
}

/** RAII teardown for every native view this helper materialises. */
struct ShownViews {
    std::vector<std::pair<SPClipPath *, unsigned>> clips;
    std::vector<std::pair<SPMask *, unsigned>> masks;

    ~ShownViews()
    {
        // Hiding unlinks the mask/clip DrawingItem from its parent (our private
        // coverage tree), so it must happen before that tree is destroyed.
        for (auto &[mask, key] : masks) {
            mask->hide(key);
        }
        for (auto &[clip, key] : clips) {
            clip->hide(key);
        }
    }
};

// Only the admitted EB2 plain tree: no markers, nested effects or paint servers.
// Its public item views can be unlinked postorder without virtual hide() (which
// allocates for groups). The private Drawing is live, unsnapshotted and has no
// observers; unlink and vector erase do not allocate in this context.
void unlink_grid_views(SPObject &object, unsigned key) noexcept
{
    for (auto &child : object.children) unlink_grid_views(child, key);
    if (auto item = cast<SPItem>(&object)) {
        auto &views = item->views;
        views.erase(std::remove_if(views.begin(), views.end(),
                    [key](auto const &v) { return v.key == key; }), views.end());
    }
}
struct ShownGridClip {
    SPClipPath *clip = nullptr;
    unsigned key = 0;
    bool &failed;
    ~ShownGridClip() noexcept
    {
        if (!clip) return;
        try { clip->hide(key); }
        catch (...) {
            failed = true;
            unlink_grid_views(*clip, key);
            // Intrusive-list swap is allocation-free. Temporarily hide the
            // already-clean children from native hide, so it only erases its
            // private root view. Restore the exact object list without signals,
            // parent/XML changes or virtual callbacks before leaving cleanup.
            SPObject::ChildrenList children;
            children.swap(clip->children);
            clip->hide(key);
            children.swap(clip->children);
        }
    }
};

void attach_clip_and_mask(Inkscape::Drawing &drawing, DrawingItem &item, SPItem &spitem,
                          ShownViews &shown)
{
    if (auto *clip = spitem.getClipObject()) {
        auto const key = SPItem::ensure_key(&item) + ITEM_KEY_CLIP;
        shown.clips.emplace_back(clip, key);
        auto *clip_item = clip->show(drawing, key, spitem.geometricBounds());
        item.setClip(clip_item);
    }
    if (auto *mask = spitem.getMaskObject()) {
        auto const key = SPItem::ensure_key(&item) + ITEM_KEY_MASK;
        shown.masks.emplace_back(mask, key);
        auto *mask_item = mask->show(drawing, key, spitem.geometricBounds());
        item.setMask(mask_item);
    }
}

Result fail(Status status)
{
    Result result;
    result.status = status;
    return result;
}

/**
 * The cheap, non-rendering refusals of evaluate(), in the exact order
 * evaluate() performs them and with the same Status it returned. The single
 * shared implementation lets the dry-run preflight predict the coverage step's
 * refusals without materialising any view.
 *
 * On `Status::Completed`, `chain_out` holds branch -> ... -> source and
 * `mapping_out` holds the same pixel-to-document affine evaluate() uses.
 */
Status check_preconditions(SPItem &branch, SPImage &source, std::vector<SPItem *> *chain_out,
                           std::optional<Geom::Affine> *mapping_out)
{
    if (source.missing || !source.pixbuf || source.pixbuf->width() <= 0 ||
        source.pixbuf->height() <= 0) {
        return Status::MissingImage;
    }

    auto const pixel_to_document = source.pixelToDocumentAffine();
    if (!pixel_to_document || !finite(*pixel_to_document) ||
        pixel_to_document->isSingular(1e-12)) {
        return Status::SingularMapping;
    }

    // The branch must be the source or one of its ancestors; the chain is the
    // single parent path from the branch down to the source.
    std::vector<SPItem *> chain;
    {
        std::vector<SPItem *> upward;
        for (SPObject *object = &source; object; object = object->parent) {
            if (auto *item = cast<SPItem>(object)) {
                upward.push_back(item);
            }
            if (object == &branch) {
                break;
            }
        }
        if (upward.empty() || upward.back() != &branch) {
            return Status::InvalidInput;
        }
        chain.assign(upward.rbegin(), upward.rend()); // branch ... source
    }

    // Anything above the branch boundary stays shared; do not silently cross
    // or double-apply it.
    for (SPObject *object = branch.parent; object; object = object->parent) {
        if (auto *item = cast<SPItem>(object)) {
            if (has_compositing_context(*item)) {
                return Status::UnsupportedSharedAncestor;
            }
        }
    }
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        if (!chain_is_exclusive(*chain[i], *chain[i + 1])) {
            return Status::UnsupportedSharedAncestor;
        }
    }
    for (auto *item : chain) {
        if (has_unsupported_effect(*item)) {
            return Status::UnsupportedEffect;
        }
    }

    if (chain_out) {
        *chain_out = std::move(chain);
    }
    if (mapping_out) {
        *mapping_out = pixel_to_document;
    }
    return Status::Completed;
}

} // namespace

Status preflight(SPItem &branch, SPImage &source)
{
    return check_preconditions(branch, source, nullptr, nullptr);
}

static Result evaluate_impl(SPItem &branch, SPImage &source, std::stop_token cancellation, bool own_clip = false, Bitmap::Stop const *own_stop = nullptr)
{
    if (cancellation.stop_requested() || (own_stop && own_stop->requested())) {
        return fail(Status::Cancelled);
    }

    std::vector<SPItem *> chain;
    std::optional<Geom::Affine> mapping;
    if (own_clip) {
        mapping = source.c2p * source.i2doc_affine(); // same native mapping, relative conditioning below
        if (!mapping || !finite(*mapping) || !gridMappingQualified(*mapping)) return fail(Status::SingularMapping);
        chain.push_back(&source);
    }
    if (auto const precondition = own_clip ? Status::Completed : check_preconditions(branch, source, &chain, &mapping);
        precondition != Status::Completed) {
        return fail(precondition);
    }
    auto const &pixel_to_document = *mapping;

    auto const width = source.pixbuf->width();
    auto const height = source.pixbuf->height();
    auto const area = Geom::IntRect::from_xywh(0, 0, width, height);

    Result result;
    result.width = width;
    result.height = height;
    try {
        result.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    } catch (std::bad_alloc const &) {
        return fail(Status::RasterizationFailed);
    }

    if (own_clip) PreviewRenderBudget::surface(width, height);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) {
            cairo_surface_destroy(surface);
        }
        return fail(Status::RasterizationFailed);
    }
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface_owner(surface, cairo_surface_destroy);
    if (auto *clear = cairo_create(surface)) {
        cairo_set_operator(clear, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(clear, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(clear);
        cairo_destroy(clear);
    }

    bool raster_failed = false;
    {
        Inkscape::Drawing drawing;
        ShownViews shown; // legacy coverage; empty for own-clip capture
        ShownGridClip grid_shown{nullptr, 0, raster_failed}; // before Drawing destruction

        DrawingItem *root = nullptr;
        DrawingItem *current = nullptr;
        for (std::size_t i = 0; i < chain.size(); ++i) {
            SPItem *item = chain[i];
            bool const leaf = (i + 1 == chain.size());
            DrawingItem *node = nullptr;
            if (leaf) {
                node = new CoverageFillItem(drawing, area);
            } else {
                node = new DrawingGroup(drawing);
            }
            if (own_clip) drawing.setRoot(node); // owns node even if native clip show fails
            // i2doc already carries the branch's own and all ancestor
            // transforms; the drawing affine (pixel->document)^-1 then lands
            // the whole chain on source pixels.
            node->setTransform(i == 0 ? branch.i2doc_affine() : item->transform);
            // Mirror native invoke_show(): hidden items render nothing.
            node->setVisible(!item->isHidden());
            if (item->style && !own_clip) {
                node->setOpacity(item->style->opacity.as_double());
            }
            if (own_clip) {
                if (auto *clip = item->getClipObject()) {
                    auto key = SPItem::ensure_key(node) + ITEM_KEY_CLIP;
                    grid_shown.clip = clip; grid_shown.key = key; // before ANY native child can throw
                    auto *clip_item = clip->show(drawing, key, item->geometricBounds());
                    node->setClip(clip_item);
                }
            } else attach_clip_and_mask(drawing, *node, *item, shown);
            if (current) {
                current->appendChild(node);
            } else {
                root = node;
            }
            current = node;
        }

        if (!own_clip) drawing.setRoot(root);
        if (own_clip && PreviewRenderBudget::current()) PreviewRenderBudget::current()->bind_drawing(&drawing);

        if (cancellation.stop_requested() || (own_stop && own_stop->requested())) {
            return fail(Status::Cancelled);
        }

        try {
            drawing.update(area, pixel_to_document.inverse());
            DrawingSurface target(surface, Geom::IntPoint(0, 0));
            DrawingContext context(target);
            drawing.render(context, area);
        } catch (std::bad_alloc const &) {
            raster_failed = true;
        }
    }

    if (raster_failed) {
        return fail(Status::RasterizationFailed);
    }

    cairo_surface_flush(surface);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        return fail(Status::RasterizationFailed);
    }

    auto const *data = cairo_image_surface_get_data(surface);
    auto const surface_stride = cairo_image_surface_get_stride(surface);
    if (!data || surface_stride <= 0) {
        return fail(Status::RasterizationFailed);
    }

    for (int y = 0; y < height; ++y) {
        if (cancellation.stop_requested() || (own_stop && own_stop->requested())) {
            // Coarse, atomic cancellation: the native update/render above is not
            // interruptible, so this is the first point after it where a later
            // request can be observed. No partial plane is exposed.
            return fail(Status::Cancelled);
        }
        auto const *row = data + static_cast<std::size_t>(y) * surface_stride;
        auto *out = result.pixels.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            guint32 pixel = 0;
            std::memcpy(&pixel, row + static_cast<std::size_t>(x) * 4, sizeof(pixel));
            out[x] = static_cast<unsigned char>((pixel >> 24) & 0xffu);
        }
    }

    result.status = Status::Completed;
    return result;
}

Result evaluate(SPItem &branch, SPImage &source, std::stop_token cancellation)
{
    return evaluate_impl(branch, source, cancellation);
}
bool gridMappingQualified(Geom::Affine const &a)
{
    if (!finite(a)) return false;
    auto m = std::max({std::abs(a[0]), std::abs(a[1]), std::abs(a[2]), std::abs(a[3])});
    if (!m) return false;
    auto x = a[0]/m, y = a[1]/m, z = a[2]/m, w = a[3]/m;
    auto det = std::abs(x*w-y*z), trace = x*x+y*y+z*z+w*w;
    auto largest = (trace + std::sqrt(std::max(0.0, trace*trace-4*det*det)))/2;
    return det && largest/det <= 1e6 && finite(a.inverse());
}
char const *estimateGridClip(SPClipPath *clip, unsigned w, unsigned h, ClipComplexity &c)
{
    c = {};
    // Bound traversal itself; no geometry/bounds/rendering or heap allocation here.
    auto visit = [&](auto &&self, SPObject &object, unsigned depth) -> char const * {
        if (++c.objects > 4096 || depth > 32) return "Own clip main-thread complexity limit (100 ms).";
        if (auto item = cast<SPItem>(&object)) {
            auto shape = cast<SPShape>(item);
            if ((!shape && !is<SPGroup>(item)) || item->getClipObject() || item->getMaskObject() ||
                item->isFiltered() || (shape && shape->hasMarkers()))
                return "Own clip has uncalibrated materialization dependencies.";
            if (!gridMappingQualified(item->transform)) return "Own clip has ill-conditioned geometry.";
            if (is<SPGroup>(item)) ++c.groups;
            if (shape) {
                ++c.shapes;
                if (auto curve = shape->curve()) for (auto const &path : *curve) {
                    c.pathNodes += path.size_default()+1;
                    if (c.pathNodes > ClipMainUnitLimit) return "Own clip main-thread complexity limit (100 ms).";
                }
            }
        }
        if (object.style) {
            if (object.style->getFillPaintServer() || object.style->getStrokePaintServer())
                return "Own clip has uncalibrated materialization dependencies.";
            c.styleEntries += object.style->stroke_dasharray.values.size()+1;
            if (c.styleEntries > ClipMainUnitLimit) return "Own clip main-thread complexity limit (100 ms).";
        }
        for (auto &child : object.children) if (auto why = self(self, child, depth+1)) return why;
        return nullptr;
    };
    if (clip) if (auto why = visit(visit, *clip, 0)) return why;
    // DrawingItems + style/view records, copied paths and dynamic dash storage.
    // Deliberately conservative, separate from the raster/cache reservation.
    c.bytes = c.objects*8192 + c.pathNodes*512 + c.styleEntries*16;
    c.units = 32*c.objects + 8*c.pathNodes + c.styleEntries +
              ((std::uint64_t(w)*h+255)/256)*(c.shapes+c.groups+1); // every child/group pushes a clip surface
    return nullptr;
}
std::optional<GridGeometry> gridGeometry(Geom::Affine const &l, unsigned w, unsigned h)
{
    if (!w || !h || w > 16384 || h > 16384 || std::uint64_t(w)*h > 100000000 || !finite(l)) return {};
    auto lx = std::hypot(l[0], l[1]), ly = std::hypot(l[2], l[3]);
    if (lx <= 0 || ly <= 0 || !std::isfinite(lx) || !std::isfinite(ly)) return {};
    // Unlike the DC editor's 0.05-pixel approximate classification, EB2 keeps
    // an exact classification. Any residual oblique coefficient resamples once.
    bool exact = (l[1] == 0 && l[2] == 0) || (l[0] == 0 && l[3] == 0);
    if (exact) return GridGeometry{w, h, l, false};
    double density = std::max(1/lx, 1/ly);
    auto region = Geom::Rect::from_xywh(0, 0, w, h)*l;
    if (!region.isFinite() || region.width() <= 0 || region.height() <= 0 || !std::isfinite(density)) return {};
    auto size = [](double span) {
        double rounded = std::round(span);
        return std::abs(span-rounded) <= 8*std::numeric_limits<double>::epsilon()*std::max(1.0, span)
            ? rounded : std::ceil(span);
    };
    double width = size(region.width()*density), height = size(region.height()*density);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        width > 16384 || height > 16384 || width*height > 100000000) return {};
    auto target = Geom::Translate(-region.left(), -region.top()) *
                  Geom::Scale(width/region.width(), height/region.height());
    return GridGeometry{static_cast<unsigned>(width), static_cast<unsigned>(height), target.inverse(), true};
}
} // namespace Inkscape::UI::Tools::DestructiveBitmapCoverage
namespace Inkscape::Bitmap {
namespace { auto const coverageMainThread = std::this_thread::get_id(); }
Result<GridCoverage> captureGridCoverage(SPImage &image, TargetSnapshot const &snapshot, Budget &budget, Stop stop) noexcept try
{
    Result<GridCoverage> result;
    auto refuse = [&](Status status, char const *why) { result.outcome = {status, why}; return std::move(result); };
    if (std::this_thread::get_id() != coverageMainThread) return refuse(Status::unavailable, "Capture clip coverage on main.");
    if (stop.requested()) return refuse(Status::canceled, "Coverage capture canceled.");
    if (snapshot.supportability != Supportability::Supported || snapshot.mode != TargetMode::SingleBitmap ||
        snapshot.bitmap != reinterpret_cast<std::uintptr_t>(&image) || image.getMaskObject() || !image.pixbuf || image.missing)
        return refuse(Status::incompatible, "Current supported own-clip target required.");
    auto w = image.pixbuf->width(), h = image.pixbuf->height();
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return refuse(Status::incompatible, "Native coverage dimension limit.");
    auto count = std::uint64_t(w)*h;
    UI::Tools::DestructiveBitmapCoverage::ClipComplexity complexity;
    if (auto why = UI::Tools::DestructiveBitmapCoverage::estimateGridClip(image.getClipObject(), w, h, complexity))
        return refuse(Status::incompatible, why);
    Budget::Token tree, native;
    if (auto o = budget.acquire(Stage::composition, complexity.bytes, tree); !o.ok())
        return refuse(o.status, "Own clip tree materialization exceeds memory budget.");
    if (complexity.units > UI::Tools::DestructiveBitmapCoverage::ClipMainUnitLimit)
        return refuse(Status::incompatible, "Own clip main-thread complexity limit (100 ms).");
    // Native surface/cache/vector envelope is held until all temporary storage dies.
    auto bytes = 16*count+MiB+8*count*(complexity.shapes+complexity.groups+1);
    if (bytes > 256*MiB || count*(complexity.shapes+complexity.groups+5) > 512*MiB)
        return refuse(Status::incompatible, "Own clip native surface envelope exceeds render budget.");
    auto o = budget.acquire(Stage::composition, bytes+count, native);
    if (!o.ok()) return refuse(o.status, o.diagnostic);
    PreviewRenderBudget render({static_cast<std::size_t>(bytes), 512*MiB, 8192, 4096}, [&] { return stop.requested(); });
    auto detached = UI::Tools::DestructiveBitmapCoverage::evaluate_impl(image, image, {}, true, &stop);
    if (stop.requested()) return refuse(Status::canceled, "Coverage capture canceled.");
    if (!detached.completed()) return refuse(Status::failed, "Own clip rasterization failed.");
    auto &out = result.value;
    o = out.pixels.allocate(budget, Stage::composition, count, 1, nullptr, stop);
    if (!o.ok()) return refuse(o.status, o.diagnostic);
    for (std::uint64_t i = 0; i < count; i += 4096) {
        if (stop.requested()) { out = {}; return refuse(Status::canceled, "Coverage capture canceled."); }
        std::memcpy(out.pixels.data()+i, detached.pixels.data()+i, std::min<std::uint64_t>(4096, count-i));
    }
    out.width = w; out.height = h; out.bitmap = snapshot.bitmap; out.generation = snapshot.generation;
    result.outcome = {Status::changed, "Own clip coverage captured."}; return result;
} catch (...) { Result<GridCoverage> result; result.outcome = {Status::failed, "Coverage render limit/allocation failure."}; return result; }
} // namespace Inkscape::Bitmap
