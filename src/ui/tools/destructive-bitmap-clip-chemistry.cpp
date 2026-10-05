// SPDX-License-Identifier: GPL-2.0-or-later

#include "destructive-bitmap-clip-chemistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <glib.h>
#include <glibmm/i18n.h>

#include "display/cairo-utils.h"
#include "destructive-bitmap-coverage.h"
#include "document-undo.h"
#include "document.h"
#include "enums.h"
#include "livarot/LivarotDefs.h"
#include "object/sp-image.h"
#include "object/sp-clippath.h"
#include "object/sp-item-group.h"
#include "object/sp-mask.h"
#include "object/sp-use.h"
#include "path/offset-shapes.h"
#include "selection.h"
#include "style-enums.h"
#include "style.h"
#include "svg/stringstream.h"
#include "svg/svg.h"
#include "xml/event-fns.h"
#include "xml/href-attribute-helper.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::UI::Tools::DestructiveBitmapClip {
namespace {

// The document adapter owns live SP/XML objects. Its translation unit is
// initialized on the process main thread before actions can be dispatched.
std::thread::id const application_main_thread = std::this_thread::get_id();

bool on_application_main_thread() noexcept
{
    return std::this_thread::get_id() == application_main_thread;
}

struct PreparedSelection {
    SPImage *image = nullptr;
    // Selected root that owns the converted pixels. It equals `image` for a
    // directly selected bitmap and is the exclusively owned wrapper otherwise.
    SPItem *branch = nullptr;
    SPItem *cutter = nullptr;
    Geom::PathVector cutter_pixels;
    FillRule fill_rule = FillRule::NonZero;
    Geom::Affine pixel_to_document;
    Geom::Affine pixel_to_item;
    Geom::Rect viewport;
};

bool finite_affine(Geom::Affine const &affine)
{
    for (unsigned i = 0; i < 6; ++i) {
        if (!std::isfinite(affine[i])) {
            return false;
        }
    }
    return true;
}

// Structural scan of a selected root: how many images its subtree contains, and
// whether collapsing it would silently flatten other artwork or a clone. This
// is deliberately cheap and never renders; the accepted B02 coverage helper
// performs the expensive per-pixel validation later.
struct BranchScan {
    SPImage *image = nullptr;
    std::size_t images = 0;
    bool has_use = false;
    bool visible_non_image = false;
};

void scan_branch(SPItem &item, BranchScan &scan)
{
    if (auto *image = cast<SPImage>(&item)) {
        ++scan.images;
        if (!scan.image) {
            scan.image = image;
        }
        return;
    }
    if (is<SPUse>(&item)) {
        // A use instance is a reference/clone boundary, not an image branch or
        // a closed cutter.
        scan.has_use = true;
        return;
    }
    if (auto *group = cast<SPGroup>(&item)) {
        for (auto &child : group->children) {
            if (auto *child_item = cast<SPItem>(&child)) {
                scan_branch(*child_item, scan);
            }
        }
        return;
    }
    if (!item.isHidden()) {
        scan.visible_non_image = true;
    }
}

bool subtree_contains(SPItem &item, SPItem *target)
{
    if (&item == target) {
        return true;
    }
    if (auto *group = cast<SPGroup>(&item)) {
        for (auto &child : group->children) {
            if (auto *child_item = cast<SPItem>(&child)) {
                if (subtree_contains(*child_item, target)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Chain from `root` down to `leaf`, inclusive. Empty if the chain is not a
// single group descent (should not happen after a successful scan).
std::vector<SPItem *> chain_between(SPItem &root, SPItem &leaf)
{
    std::vector<SPItem *> chain;
    chain.push_back(&root);
    SPItem *current = &root;
    while (current != &leaf) {
        SPItem *next = nullptr;
        if (auto *group = cast<SPGroup>(current)) {
            for (auto &child : group->children) {
                auto *child_item = cast<SPItem>(&child);
                if (child_item && subtree_contains(*child_item, &leaf)) {
                    next = child_item;
                    break;
                }
            }
        }
        if (!next) {
            return {};
        }
        chain.push_back(next);
        current = next;
    }
    return chain;
}

// The next chain element must be the only visible item child of a container.
// Hidden siblings do not render and do not make the context shared.
bool chain_is_exclusive(SPItem &parent, SPItem &expected)
{
    for (auto &child : parent.children) {
        auto *item = cast<SPItem>(&child);
        if (!item || item == &expected || item->isHidden()) {
            continue;
        }
        return false;
    }
    return true;
}

// A compositing context that sits above the conversion boundary stays shared
// with the rest of the document. The destructive operation must not cross or
// double-apply it, so a wrapper branch under such an ancestor is rejected.
// This mirrors the accepted B02 coverage contract
// (`destructive-bitmap-coverage.cpp::has_compositing_context`) and is only
// consulted for a wrapper branch: a directly selected bitmap keeps the
// existing trim-conditional resource policy at commit time.
bool has_compositing_context(SPItem const &item)
{
    if (item.isHidden()) {
        // A hidden ancestor above the branch boundary renders nothing at all.
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

bool is_ancestor_of(SPItem *ancestor, SPItem *node)
{
    for (SPObject *object = node; object; object = object->parent) {
        if (object == ancestor) {
            return true;
        }
    }
    return false;
}

// Strict validation for a selected wrapper group (branch != image). A directly
// selected bitmap keeps the existing trim-conditional resource policy; only a
// wrapper that would have to be collapsed is required to be exclusively owned.
TargetStatus validate_wrapper_branch(SPItem &branch, SPImage &image)
{
    auto const chain = chain_between(branch, image);
    if (chain.size() < 2 || chain.front() != &branch || chain.back() != &image) {
        return TargetStatus::UnsupportedBranch;
    }
    for (auto *item : chain) {
        if (item->isHidden() || item->isLocked()) {
            return TargetStatus::ProtectedObject;
        }
        if (item->cloned || item->hrefcount) {
            return TargetStatus::UnsupportedBranch;
        }
        if (item->isFiltered()) {
            return TargetStatus::UnsupportedBranch;
        }
        if (item->style && item->style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL) {
            return TargetStatus::UnsupportedBranch;
        }
    }
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        if (!chain_is_exclusive(*chain[i], *chain[i + 1])) {
            return TargetStatus::UnsupportedBranch;
        }
    }
    // Anything above the selected wrapper is outside the conversion boundary
    // and stays shared with its siblings. Classify it explicitly instead of
    // silently baking through a mask/clip/filter/opacity/blend it does not own.
    for (SPObject *object = branch.parent; object; object = object->parent) {
        if (auto *item = cast<SPItem>(object)) {
            if (has_compositing_context(*item)) {
                return TargetStatus::UnsupportedBranch;
            }
        }
    }
    return TargetStatus::Resolved;
}

// A cutter must not silently drop a member. `OffsetShapes::item_geometry`
// returns nullopt for a hidden/masked/filtered item and the group recursion
// discards such children without counting them, while a locked member is
// protected content. Scan the whole cutter subtree before `prepare` and reject
// the pair atomically rather than baking a region that omits or includes a
// member that the operation does not actually own.
TargetStatus validate_cutter_subtree(SPItem &item)
{
    if (item.isHidden() || item.isLocked()) {
        return TargetStatus::ProtectedObject;
    }
    // `OffsetShapes::item_geometry` returns nullopt for a plain bitmap and the
    // group recursion discards it without counting a skip, so a bitmap member
    // would otherwise vanish from the prepared region. A bitmap is not a
    // supported closed vector cutter: classify it explicitly.
    if (is<SPImage>(&item)) {
        return TargetStatus::UnsupportedCutter;
    }
    if (item.getMaskObject() || item.isFiltered()) {
        return TargetStatus::UnsupportedCutter;
    }
    if (auto *group = cast<SPGroup>(&item)) {
        for (auto &child : group->children) {
            if (auto *child_item = cast<SPItem>(&child)) {
                if (auto const status = validate_cutter_subtree(*child_item); status != TargetStatus::Resolved) {
                    return status;
                }
            }
        }
    } else if (auto *use = cast<SPUse>(&item)) {
        if (use->child) {
            if (auto const status = validate_cutter_subtree(*use->child); status != TargetStatus::Resolved) {
                return status;
            }
        }
    }
    return TargetStatus::Resolved;
}

TargetStatus resolve_core(std::span<SPItem *const> selected, SPDocument *document, PreparedSelection &result)
{
    result = {};

    if (selected.size() != 2) {
        return TargetStatus::NotPair;
    }
    auto *first = selected[0];
    auto *second = selected[1];
    if (!first || !second) {
        return TargetStatus::NotPair;
    }
    if (first == second) {
        return TargetStatus::NestedOrDuplicateRoots;
    }
    if (is_ancestor_of(first, second) || is_ancestor_of(second, first)) {
        return TargetStatus::NestedOrDuplicateRoots;
    }
    if (is<SPUse>(first) || is<SPUse>(second)) {
        return TargetStatus::UnsupportedBranch;
    }

    BranchScan first_scan;
    BranchScan second_scan;
    scan_branch(*first, first_scan);
    scan_branch(*second, second_scan);

    bool const first_is_branch = first_scan.images > 0;
    bool const second_is_branch = second_scan.images > 0;
    if (first_is_branch == second_is_branch) {
        return TargetStatus::AmbiguousImageBranch;
    }

    auto *branch = first_is_branch ? first : second;
    auto *cutter = first_is_branch ? second : first;
    auto const &scan = first_is_branch ? first_scan : second_scan;

    if (scan.images > 1) {
        return TargetStatus::MultipleImages;
    }
    if (!scan.image) {
        return TargetStatus::AmbiguousImageBranch;
    }
    if (scan.has_use || scan.visible_non_image) {
        return TargetStatus::UnsupportedBranch;
    }

    auto *image = scan.image;
    if (!image->getRepr() || !branch->getRepr() || !cutter->getRepr()) {
        return TargetStatus::NotPair;
    }
    if (image->isHidden() || image->isLocked() || branch->isHidden() || branch->isLocked()) {
        return TargetStatus::ProtectedObject;
    }
    if (cutter->isHidden() || cutter->isLocked()) {
        return TargetStatus::ProtectedObject;
    }
    if (image->document != document || branch->document != document || cutter->document != document) {
        return TargetStatus::NotPair;
    }
    if (image->missing || !image->pixbuf || image->pixbuf->width() <= 0 || image->pixbuf->height() <= 0) {
        return TargetStatus::MissingImage;
    }

    auto const pixel_to_document = image->pixelToDocumentAffine();
    if (!pixel_to_document || pixel_to_document->isSingular() || !finite_affine(*pixel_to_document)) {
        return TargetStatus::InvalidGeometry;
    }

    if (branch != image) {
        auto const wrapper_status = validate_wrapper_branch(*branch, *image);
        if (wrapper_status != TargetStatus::Resolved) {
            return wrapper_status;
        }
    }

    // Reject locked/hidden/masked/filtered cutter members before
    // `OffsetShapes::prepare`, which would otherwise drop some silently.
    if (auto const cutter_status = validate_cutter_subtree(*cutter); cutter_status != TargetStatus::Resolved) {
        return cutter_status;
    }

    result.image = image;
    result.branch = branch;
    result.cutter = cutter;
    result.pixel_to_document = *pixel_to_document;
    result.pixel_to_item = image->c2p;
    result.viewport = image->clipbox;

    std::array<SPItem *, 1> cutter_items = {cutter};
    auto prepared = Inkscape::OffsetShapes::prepare(std::span<SPItem *const>{cutter_items});
    if (!prepared || prepared.sources.size() != 1 || prepared.skipped_count != 0 ||
        prepared.open_subpaths_skipped != 0) {
        return TargetStatus::UnsupportedCutter;
    }

    auto const &source = prepared.sources.front();
    result.cutter_pixels = source.geometry_document * pixel_to_document->inverse();
    result.fill_rule = source.fill_rule == fill_oddEven ? FillRule::EvenOdd : FillRule::NonZero;
    if (result.cutter_pixels.empty()) {
        return TargetStatus::UnsupportedCutter;
    }
    return TargetStatus::Resolved;
}

// Trimming leaves the item's transform and user coordinate system unchanged.
// Mask/clip/opacity coverage is baked and detached before publication, so a
// trim no longer has to rebase those resources. A clone/outside reference of
// this image or an ancestor would still be rendered against the old payload and
// must not be silently changed, and a colour-changing filter is not bakeable
// while preserving source RGB. Reject only when a trim is needed.
bool trim_resources_supported(SPImage &image)
{
    for (auto *object = static_cast<SPObject *>(&image); object; object = object->parent) {
        if (object->hrefcount || object->cloned) {
            return false;
        }
        if (auto *item = cast<SPItem>(object)) {
            if (item->isFiltered()) {
                return false;
            }
        }
    }
    return true;
}

std::optional<Geom::Rect> trim_geometry(PreparedSelection const &prepared, Result const &result)
{
    auto const &mapping = prepared.pixel_to_item;
    // SPImage's viewBox mapping must be a finite positive scale + translation.
    // Rotation, shear and reflection remain in the unchanged item/parent affine.
    for (unsigned i = 0; i < 6; ++i) {
        if (!std::isfinite(mapping[i])) {
            return {};
        }
    }
    if (mapping[0] <= 0 || mapping[3] <= 0 || mapping[1] != 0 || mapping[2] != 0) {
        return {};
    }
    auto geometry = Geom::Rect::from_xywh(result.left, result.top,
                                         result.pixels->width(), result.pixels->height());
    geometry *= mapping;
    if (!geometry.isFinite() || geometry.width() <= 0 || geometry.height() <= 0) {
        return {};
    }
    // A slice viewport can hide source pixels outside its old rectangle.
    // Enlarging that viewport would reveal them. Retained rectangles entirely
    // inside it are safe; other cases require an additional rebased viewport
    // clip and are rejected atomically rather than silently changing appearance.
    auto viewport = prepared.viewport;
    viewport.expandBy(1e-9);
    if (viewport.contains(geometry)) {
        return geometry;
    }

    auto const &old_viewport = prepared.viewport;
    auto const scale = mapping[0];
    if (!old_viewport.isFinite() || old_viewport.width() <= 0 || old_viewport.height() <= 0 ||
        std::abs(scale / mapping[3] - 1) > 1e-12 ||
        std::abs(mapping[4] - old_viewport.left()) > 1e-9 ||
        std::abs(mapping[5] - old_viewport.top()) > 1e-9 ||
        !Geom::are_near(prepared.image->pixbuf->width() * scale / old_viewport.width(), 1.0, Geom::EPSILON) ||
        !Geom::are_near(prepared.image->pixbuf->height() * scale / old_viewport.height(), 1.0, Geom::EPSILON)) {
        return {};
    }
    // SPViewBox averages nearly equal scales and snaps an average near 1 to
    // scale_none == 1.0. The snap can add drift beyond the averaging error;
    // each extent may differ by about 1.5e-6 per source pixel. The EPSILON
    // checks above bound that drift before clamping to the old viewport.
    auto const left = std::max(geometry.left(), old_viewport.left());
    auto const top = std::max(geometry.top(), old_viewport.top());
    auto const right = std::min(geometry.right(), old_viewport.right());
    auto const bottom = std::min(geometry.bottom(), old_viewport.bottom());
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom) ||
        right <= left || bottom <= top) {
        return {};
    }
    return Geom::Rect::from_xywh(left, top, right - left, bottom - top);
}


// Orthogonal rotations/reflections retain the exact intrinsic-grid operation.
bool pixel_axes_oblique(Geom::Affine const &l, int width, int height)
{
    double const epsilon = 1e-9 * std::max({std::abs(l[0]), std::abs(l[1]),
                                          std::abs(l[2]), std::abs(l[3])});
    double const density = std::max(1 / std::hypot(l[0], l[1]), 1 / std::hypot(l[2], l[3]));
    double const span = std::max(width, height) * density;
    return !((std::abs(l[1]) <= epsilon && std::abs(l[2]) <= epsilon) ||
             (std::abs(l[0]) <= epsilon && std::abs(l[3]) <= epsilon) ||
             std::max(std::abs(l[1]), std::abs(l[2])) * span <= 0.05 ||
             std::max(std::abs(l[0]), std::abs(l[3])) * span <= 0.05);
}

struct StraightenPlan {
    CommitStatus status = CommitStatus::RasterizationFailed;
    Result result;
    Geom::Rect frame = Geom::Rect::from_xywh(0, 0, 1, 1);
};

StraightenPlan straighten(PreparedSelection const &prepared, Inkscape::Pixbuf const &source,
                          Coverage const &coverage, Mode mode, std::stop_token cancellation)
{
    auto *parent = cast<SPItem>(prepared.image->parent);
    if (!parent || parent->i2doc_affine().isSingular()) return {.status = CommitStatus::UnsupportedTrim};
    // Bake only the image's own coverage, never the real cutter, in its source grid.
    Geom::PathVector const full_source{Geom::Path(Geom::Rect::from_xywh(0, 0, source.width(), source.height()))};
    auto baked = apply(source, coverage, full_source, FillRule::NonZero, Mode::KeepInside, cancellation);
    auto failed = [](Result const &result) {
        return result.status == Status::Cancelled ? CommitStatus::Cancelled
             : result.status == Status::InvalidInput ? CommitStatus::InvalidGeometry
                                                    : CommitStatus::RasterizationFailed;
    };
    if (!baked.completed()) return {.status = failed(baked)};
    // Straightening rebases the viewport even when the intrinsic bake did not trim.
    // Resource rebasing must still be supported; the viewport is clipped below.
    if (!trim_resources_supported(*prepared.image)) {
        return {.status = CommitStatus::UnsupportedTrim};
    }
    if (baked.all_transparent) return {.status = CommitStatus::StraighteningEmptyRegion};

    auto const &l = prepared.pixel_to_document;
    double const density = std::max(1 / std::hypot(l[0], l[1]), 1 / std::hypot(l[2], l[3]));
    auto const baked_extent = Geom::Rect::from_xywh(baked.left, baked.top, baked.pixels->width(), baked.pixels->height());
    auto const viewport_pixels = prepared.viewport * prepared.pixel_to_item.inverse();
    auto const visible = Geom::intersect(baked_extent, viewport_pixels);
    if (!visible || visible->width() <= 0 || visible->height() <= 0) {
        return {.status = CommitStatus::StraighteningEmptyRegion};
    }
    auto region = *visible * l;
    auto const cutter_document = prepared.cutter_pixels * l;
    auto const cutter_bounds = Geom::bounds_exact(cutter_document);
    if (!cutter_bounds || !cutter_bounds->isFinite() || !region.isFinite() || !std::isfinite(density)) {
        return {.status = CommitStatus::InvalidGeometry};
    }
    double left = region.left(), top = region.top(), right = region.right(), bottom = region.bottom();
    if (mode == Mode::KeepInside) {
        left = std::max(left, cutter_bounds->left());
        top = std::max(top, cutter_bounds->top());
        right = std::min(right, cutter_bounds->right());
        bottom = std::min(bottom, cutter_bounds->bottom());
    }
    if (right <= left || bottom <= top) return {.status = CommitStatus::StraighteningEmptyRegion};
    auto grid_size = [](double span) {
        // Do not add a column for a whole-pixel span perturbed by affine roundoff.
        double const rounded = std::round(span);
        return std::abs(span - rounded) <= 8 * std::numeric_limits<double>::epsilon() * std::max(1.0, span)
             ? rounded : std::ceil(span);
    };
    double const width = grid_size((right - left) * density);
    double const height = grid_size((bottom - top) * density);
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(width) || !std::isfinite(height) ||
        width > 16384 || height > 16384 || width * height > 100000000) {
        return {.status = CommitStatus::StraighteningTooLarge};
    }
    if (width <= 0 || height <= 0) return {.status = CommitStatus::StraighteningEmptyRegion};
    // Anchor the grid at the exact region corner; rounding only increases density.
    double const density_x = width / (right - left), density_y = height / (bottom - top);
    auto const document_to_target = Geom::Translate(-left, -top) * Geom::Scale(density_x, density_y);
    auto const baked_to_target = Geom::Translate(baked.left, baked.top) * l * document_to_target;

    // Cairo needs premultiplied ARGB32; convert only a detached copy and return
    // straight RGBA to the engine. Never change the live source's representation.
    Inkscape::Pixbuf cairo_source(*baked.pixels);
    cairo_source.ensurePixelFormat(Inkscape::Pixbuf::PF_CAIRO);
    using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
    using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
    using Pattern = std::unique_ptr<cairo_pattern_t, decltype(&cairo_pattern_destroy)>;
    Surface surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(width),
                                               static_cast<int>(height)), cairo_surface_destroy);
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) return {};
    Context context(cairo_create(surface.get()), cairo_destroy);
    Pattern pattern(cairo_pattern_create_for_surface(cairo_source.getSurfaceRaw()), cairo_pattern_destroy);
    auto const inverse = baked_to_target.inverse();
    cairo_matrix_t matrix{inverse[0], inverse[1], inverse[2], inverse[3], inverse[4], inverse[5]};
    cairo_pattern_set_matrix(pattern.get(), &matrix);
    cairo_pattern_set_extend(pattern.get(), CAIRO_EXTEND_NONE);
    auto const rendering = prepared.image->style->image_rendering.computed;
    bool const nearest = rendering == SP_CSS_IMAGE_RENDERING_OPTIMIZESPEED ||
                         rendering == SP_CSS_IMAGE_RENDERING_PIXELATED ||
                         rendering == SP_CSS_IMAGE_RENDERING_CRISPEDGES;
    cairo_pattern_set_filter(pattern.get(), nearest ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
    cairo_set_operator(context.get(), CAIRO_OPERATOR_SOURCE);
    cairo_set_source(context.get(), pattern.get());
    if (!viewport_pixels.contains(baked_extent)) {
        cairo_save(context.get());
        cairo_matrix_t forward{baked_to_target[0], baked_to_target[1], baked_to_target[2],
                               baked_to_target[3], baked_to_target[4], baked_to_target[5]};
        cairo_transform(context.get(), &forward);
        cairo_rectangle(context.get(), visible->left() - baked.left, visible->top() - baked.top,
                        visible->width(), visible->height());
        cairo_restore(context.get()); // The path remains in device coordinates.
        cairo_clip(context.get());
    }
    cairo_paint(context.get());
    if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS ||
        cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) return {};
    if (cancellation.stop_requested()) return {.status = CommitStatus::Cancelled};
    cairo_surface_flush(surface.get());
    Inkscape::Pixbuf straight(surface.release()); // takes surface ownership
    straight.ensurePixelFormat(Inkscape::Pixbuf::PF_GDK);
    auto *raw = straight.getPixbufRaw(false);
    auto *source_raw = baked.pixels->getPixbufRaw(false);
    Inkscape::copy_supported_pixbuf_metadata(source_raw, raw);
    if (g_strcmp0(gdk_pixbuf_get_option(source_raw, "icc-profile"),
                 gdk_pixbuf_get_option(raw, "icc-profile")) != 0) return {};
    // PNG dpi options are integral. Use Dx for both (rounding adds <1 px per axis);
    // set canonical strings before apply(), whose crop checks these options.
    auto const dpi = std::to_string(static_cast<int>(std::clamp(std::round(density_x * 96),
                                      1.0, static_cast<double>(std::numeric_limits<int>::max()))));
    for (auto const *key : {"x-dpi", "y-dpi"}) {
        gdk_pixbuf_remove_option(raw, key);
        if (!gdk_pixbuf_set_option(raw, key, dpi.c_str())) return {};
    }
    auto result = apply(straight, cutter_document * document_to_target, prepared.fill_rule, mode, cancellation);
    if (!result.completed()) return {.status = failed(result)};
    auto frame = Geom::Rect::from_xywh(left + result.left / density_x, top + result.top / density_y,
                                      result.pixels->width() / density_x, result.pixels->height() / density_y);
    auto old_item_box = Geom::Rect::from_xywh(0, 0, source.width(), source.height());
    old_item_box *= prepared.pixel_to_item;
    // Meet can leave viewport margins and slice can hide pixels. Compare with
    // the old visual frame, rather than its larger viewport or hidden extent.
    auto old_frame = Geom::Rect(std::max(old_item_box.left(), prepared.viewport.left()),
                               std::max(old_item_box.top(), prepared.viewport.top()),
                               std::min(old_item_box.right(), prepared.viewport.right()),
                               std::min(old_item_box.bottom(), prepared.viewport.bottom()));
    old_frame *= prepared.image->i2doc_affine();
    double const one_pixel = 1 / density;
    bool const smaller = frame.left() > old_frame.left() + one_pixel ||
                         frame.top() > old_frame.top() + one_pixel ||
                         frame.right() < old_frame.right() - one_pixel ||
                         frame.bottom() < old_frame.bottom() - one_pixel;
    // Result::changed includes an alpha-only trim. Compare in the untrimmed
    // straight grid so removing transparent rows alone cannot destroy Redo.
    bool pixels_differ = false;
    for (int y = 0; y < straight.height(); ++y) {
        if (cancellation.stop_requested()) return {.status = CommitStatus::Cancelled};
        for (int x = 0; x < straight.width(); ++x) {
            auto const *a = straight.pixels() + y * straight.rowstride() + 4 * x;
            int const rx = x - result.left, ry = y - result.top;
            if (rx >= 0 && ry >= 0 && rx < result.pixels->width() && ry < result.pixels->height()) {
                auto const *b = result.pixels->pixels() + ry * result.pixels->rowstride() + 4 * rx;
                pixels_differ |= std::memcmp(a, b, 4) != 0;
            } else {
                pixels_differ |= a[0] || a[1] || a[2] || a[3];
            }
        }
    }
    if (!pixels_differ && !smaller) return {.status = CommitStatus::NoChange};
    return {.status = CommitStatus::CommittedStraightened, .result = std::move(result), .frame = frame};
}

std::vector<SPItem *> selected_items(Inkscape::Selection &selection)
{
    std::vector<SPItem *> items;
    for (auto *object : selection.objects()) {
        if (auto *item = cast<SPItem>(object)) {
            items.push_back(item);
        }
    }
    return items;
}

CommitStatus prepare_selection(Inkscape::Selection &selection, PreparedSelection &result)
{
    auto const items = selected_items(selection);
    switch (resolve_core(std::span<SPItem *const>{items}, selection.document(), result)) {
        case TargetStatus::Resolved:
            return CommitStatus::NoChange;
        case TargetStatus::InvalidGeometry:
            // A singular/nonfinite image pixel-to-document mapping remains the
            // geometry failure exposed to the action adapter.
            return CommitStatus::InvalidGeometry;
        case TargetStatus::UnsupportedCutter:
            // Preserve the pre-B03 user-facing outcome: a cutter that supplies
            // no supported closed region is reported through the existing
            // cutter-specific InvalidGeometry message, not the generic
            // InvalidSelection one. The typed TargetStatus still distinguishes
            // it from an image mapping failure for callers of resolve_targets.
            return CommitStatus::InvalidGeometry;
        default:
            return CommitStatus::InvalidSelection;
    }
}

ResolvedTargets resolve_from(std::span<SPItem *const> selected, SPDocument *document)
{
    ResolvedTargets resolved;
    PreparedSelection prepared;
    auto const status = resolve_core(selected, document, prepared);
    if (status == TargetStatus::Resolved) {
        // A rejection exposes no resolved roles: the operation is atomic.
        resolved.image = prepared.image;
        resolved.branch = prepared.branch;
        resolved.cutter = prepared.cutter;
    }
    resolved.status = status;
    return resolved;
}

} // namespace

CommitStatus commit_status_for(DestructiveBitmapCoverage::Status status) noexcept
{
    using CoverageStatus = DestructiveBitmapCoverage::Status;
    switch (status) {
        case CoverageStatus::Cancelled:
            return CommitStatus::Cancelled;
        case CoverageStatus::MissingImage:
        case CoverageStatus::InvalidInput:
        case CoverageStatus::SingularMapping:
            return CommitStatus::InvalidGeometry;
        case CoverageStatus::UnsupportedSharedAncestor:
        case CoverageStatus::UnsupportedEffect:
            return CommitStatus::InvalidSelection;
        case CoverageStatus::RasterizationFailed:
            return CommitStatus::RasterizationFailed;
        case CoverageStatus::Completed:
            // The commit only consults this mapping for a failed evaluation, so
            // a Completed status never reaches here; keep the historical
            // fall-through value instead of inventing a success status.
            break;
    }
    return CommitStatus::RasterizationFailed;
}

bool selection_is_eligible(Inkscape::Selection &selection) noexcept
{
    g_return_val_if_fail(on_application_main_thread(), false);
    try {
        PreparedSelection prepared;
        return prepare_selection(selection, prepared) == CommitStatus::NoChange;
    } catch (...) {
        // Action sensitivity runs from selection signals and must fail closed.
        return false;
    }
}

ResolvedTargets resolve_targets(Inkscape::Selection &selection) noexcept
{
    if (!on_application_main_thread()) {
        ResolvedTargets resolved;
        resolved.status = TargetStatus::UnsupportedBranch;
        return resolved;
    }
    try {
        auto const items = selected_items(selection);
        return resolve_from(std::span<SPItem *const>{items}, selection.document());
    } catch (...) {
        // Resolution is read-only; fail closed with no resolved roles.
        ResolvedTargets resolved;
        resolved.status = TargetStatus::UnsupportedBranch;
        return resolved;
    }
}

ResolvedTargets resolve_targets(std::vector<SPItem *> const &selected, SPDocument &document) noexcept
{
    if (!on_application_main_thread()) {
        ResolvedTargets resolved;
        resolved.status = TargetStatus::UnsupportedBranch;
        return resolved;
    }
    try {
        return resolve_from({selected.data(), selected.size()}, &document);
    } catch (...) {
        ResolvedTargets resolved;
        resolved.status = TargetStatus::UnsupportedBranch;
        return resolved;
    }
}

bool commit_preconditions_hold(SPImage const *image, SPItem const *branch, SPItem const *cutter) noexcept
{
    if (!image || !branch || !cutter) {
        return false;
    }
    // Collapsing a selected single-image wrapper group into one standalone
    // bitmap needs reparenting and transform composition that is not proven
    // safe here. The resolver still classifies the pair, but the transaction
    // rejects it atomically instead of silently flattening the group.
    if (branch != image) {
        return false;
    }
    // The cutter is left exactly as it was (CLIP-1), so objects that
    // reference or clone it are unaffected.
    // Replacing the decoded payload of a referenced/cloned image would also
    // change every unselected instance. Reject before any mutation.
    if (image->hrefcount || image->cloned || branch->hrefcount || branch->cloned) {
        return false;
    }
    return true;
}

CommitStatus commit_selection(Inkscape::Selection &selection, Mode mode)
{
    return commit_selection(selection, mode, std::stop_token{});
}

CommitStatus commit_selection(Inkscape::Selection &selection, Mode mode,
                              std::stop_token cancellation)
{
    return commit_selection(selection, mode, cancellation, false);
}

CommitStatus commit_selection(Inkscape::Selection &selection, Mode mode,
                              std::stop_token cancellation, bool caller_owned_settlement)
{
    g_return_val_if_fail(on_application_main_thread(), CommitStatus::InvalidSelection);
    // Reentrancy: a document observer may synchronously dispatch this action
    // again from the publication notifications. The second entry must not touch
    // the half-published tuple.
    static bool commit_in_progress = false;
    if (commit_in_progress) {
        return CommitStatus::RasterizationFailed;
    }
    commit_in_progress = true;
    struct ResetCommitFlag {
        ~ResetCommitFlag() { commit_in_progress = false; }
    } reset_commit_flag;

    PreparedSelection prepared;
    std::string encoded;
    bool all_transparent = false;
    bool straightened = false;
    bool cover_clip = false;
    bool cover_mask = false;
    bool cover_opacity = false;
    std::optional<Geom::Rect> geometry;
    std::vector<XML::Node::AttributeUpdate> attributes;
    try {
        auto const preparation = prepare_selection(selection, prepared);
        if (preparation != CommitStatus::NoChange) {
            return preparation;
        }
        if (cancellation.stop_requested()) {
            return CommitStatus::Cancelled;
        }
        if (!commit_preconditions_hold(prepared.image, prepared.branch, prepared.cutter)) {
            return CommitStatus::InvalidSelection;
        }

        straightened = pixel_axes_oblique(prepared.pixel_to_document,
                                           prepared.image->pixbuf->width(), prepared.image->pixbuf->height());
        if (straightened) {
            auto *parent = cast<SPItem>(prepared.image->parent);
            if (!parent || parent->i2doc_affine().isSingular()) return CommitStatus::UnsupportedTrim;
        }

        // Deliberately synchronous: Gio action sequences, save, export, Undo,
        // and shutdown have no completion barrier for fire-and-forget work.
        // The raster core holds one output image and one 256x256 mask tile, so
        // repeated activation cannot accumulate workers or image snapshots.
        auto const canonical = prepared.image->pixbuf;

        // Native coverage over the selected image's own mask/clip/opacity.
        // The status gate makes a failed evaluation impossible to forward as
        // B01's null-pointer "full coverage" sentinel.
        auto coverage = DestructiveBitmapCoverage::evaluate(*prepared.branch, *prepared.image,
                                                            cancellation);
        if (!coverage.completed()) {
            // One shared mapping (see commit_status_for) so the dry run and the
            // real transaction cannot disagree about a coverage refusal.
            return commit_status_for(coverage.status);
        }
        auto coverage_view = coverage.view();
        if (!coverage_view) {
            return CommitStatus::RasterizationFailed;
        }

        Result result;
        if (straightened) {
            auto plan = straighten(prepared, *canonical, *coverage_view, mode, cancellation);
            if (plan.status != CommitStatus::CommittedStraightened) return plan.status;
            result = std::move(plan.result);
            geometry = plan.frame;
        } else {
            result = apply(*canonical, *coverage_view, prepared.cutter_pixels, prepared.fill_rule,
                           mode, cancellation);
            if (!result.completed()) {
                if (result.status == Status::Cancelled) {
                    return CommitStatus::Cancelled;
                }
                return result.status == Status::InvalidInput ? CommitStatus::InvalidGeometry
                                                             : CommitStatus::RasterizationFailed;
            }
            // The cutter stays in place, so equal pixels (and no trim) change
            // nothing: a legitimate no-op without an Undo step.
            if (!result.changed) {
                return CommitStatus::NoChange;
            }
            if (result.trimmed()) {
                if (!trim_resources_supported(*prepared.image)) {
                    return CommitStatus::UnsupportedTrim;
                }
                geometry = trim_geometry(prepared, result);
                if (!geometry) {
                    return CommitStatus::UnsupportedTrim;
                }
            }
        }

        auto encoded_result = sp_image_encode_png_data_uri(*result.pixels);
        if (!encoded_result) {
            return CommitStatus::EncodingFailed;
        }
        encoded = std::move(*encoded_result);
        all_transparent = result.all_transparent;

        auto *document = selection.document();
        auto *repr = prepared.image->getRepr();
        if (!document || prepared.image->document != document || prepared.cutter->document != document || !repr ||
            !prepared.cutter->getRepr() || prepared.image->pixbuf != canonical ||
            prepared.image->pixelToDocumentAffine() != std::optional{prepared.pixel_to_document}) {
            return CommitStatus::InvalidSelection;
        }
        cover_clip = prepared.image->getClipObject() != nullptr;
        cover_mask = prepared.image->getMaskObject() != nullptr;
        cover_opacity = prepared.image->style && prepared.image->style->opacity.as_double() < 0.995;
        // Prepare the entire tuple before mutating XML. Reuse the same SVG
        // number formatting as Node::setAttributeSvgDouble.
        if (geometry) {
            auto number = [](double value) {
                SVGOStringStream stream;
                stream << value;
                return stream.str();
            };
            attributes = {
                {"x", number(straightened ? 0 : geometry->left())},
                {"y", number(straightened ? 0 : geometry->top())},
                {"width", number(geometry->width())}, {"height", number(geometry->height())},
                {"preserveAspectRatio", "none"}
            };
        }
        if (straightened) {
            auto *parent = cast<SPItem>(prepared.image->parent);
            if (!parent || parent->i2doc_affine().isSingular()) return CommitStatus::UnsupportedTrim;
            auto const transform = Geom::Translate(geometry->min()) * parent->i2doc_affine().inverse();
            if (!finite_affine(transform)) return CommitStatus::InvalidGeometry;
            auto const written = sp_svg_transform_write(transform);
            attributes.push_back({"transform", transform.isIdentity() ? std::nullopt
                                      : std::optional<std::string>{written.c_str()}});
        }
        attributes.push_back({getHrefAttribute(*repr).first, std::move(encoded)});
        attributes.push_back({"sodipodi:absref", std::nullopt});
    } catch (...) {
        // All caught failures occur before XML commit.
        return CommitStatus::RasterizationFailed;
    }

    auto *document = selection.document();
    if (!document) {
        return CommitStatus::InvalidSelection;
    }
    auto *repr = prepared.image->getRepr();
    auto *xml_document = document->getReprDoc();
    // Remember the pre-command selection by id so a rollback can restore the
    // document's original selection even if SPObjects are rebuilt.
    std::vector<std::string> selection_ids;
    for (auto *object : selection.objects()) {
        if (object && object->getRepr() && object->getRepr()->attribute("id")) {
            selection_ids.emplace_back(object->getRepr()->attribute("id"));
        }
    }

    // Document-wide publication inside one XML transaction. All fallible pixel,
    // coverage and encoding work finished above; any throw here rolls the whole
    // tuple back before an Undo entry is recorded.
    try {
        {
            Inkscape::XML::Document::MutationScope mutation(*xml_document);
            repr->setAttributesAtomically(std::move(attributes));
            // Detach the baked compositing references so the canvas cannot
            // apply the converted mask/clip/opacity a second time. The private
            // definitions stay in defs; this is not a document-wide cleanup.
            if (cover_clip) {
                repr->setAttribute("clip-path", nullptr);
            }
            if (cover_mask) {
                repr->setAttribute("mask", nullptr);
            }
            if (cover_opacity) {
                auto *css = sp_repr_css_attr(repr, "style");
                sp_repr_css_unset_property(css, "opacity");
                sp_repr_css_set(repr, css, "style");
                sp_repr_css_attr_unref(css);
            }
        }
        document->ensureUpToDate();
    } catch (...) {
        sp_repr_rollback(xml_document);
        sp_repr_begin_transaction(xml_document);
        document->ensureUpToDate();
        std::vector<SPItem *> restored;
        for (auto const &id : selection_ids) {
            if (auto *item = cast<SPItem>(document->getObjectById(id.c_str()))) {
                restored.push_back(item);
            }
        }
        selection.setList(restored);
        return CommitStatus::RasterizationFailed;
    }

    // The resulting bitmap is the sole selection target; the cutter stays in
    // the document at its position, layer and stacking order (CLIP-1).
    selection.set(prepared.image);
    auto const inverse = mode == Mode::KeepOutside;
    if (!caller_owned_settlement) Inkscape::DocumentUndo::done(
        document,
        inverse ? RC_("Undo", "Destructive Inverse Clip Bitmap")
                : RC_("Undo", "Destructive Clip Bitmap"),
        inverse ? "object-destructive-inverse-clip" : "object-destructive-clip");
    if (straightened) {
        return all_transparent ? CommitStatus::CommittedStraightenedAllTransparent : CommitStatus::CommittedStraightened;
    }
    return all_transparent ? CommitStatus::CommittedAllTransparent : CommitStatus::Committed;
}

} // namespace Inkscape::UI::Tools::DestructiveBitmapClip
