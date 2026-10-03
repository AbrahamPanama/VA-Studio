// SPDX-License-Identifier: GPL-2.0-or-later

#include "bitmap-eraser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cairo.h>
#include <glibmm/i18n.h>
#include <sigc++/scoped_connection.h>

#include "desktop.h"
#include "display/cairo-utils.h"
#include "document-undo.h"
#include "document.h"
#include "message-stack.h"
#include "object/sp-image.h"
#include "object/weakptr.h"
#include "xml/href-attribute-helper.h"

namespace Inkscape::UI::Tools {
namespace {

struct PixelBounds
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool empty() const { return left >= right || top >= bottom; }
};

std::optional<PixelBounds> path_pixel_bounds(Geom::PathVector const &path, int width, int height)
{
    auto const bounds = Geom::bounds_exact(path);
    if (!bounds || !bounds->isFinite())
        return std::nullopt;

    // Cairo can touch the neighboring pixel through antialiasing.
    PixelBounds result;
    result.left = static_cast<int>(std::clamp(std::floor(bounds->left()) - 1.0, 0.0, static_cast<double>(width)));
    result.top = static_cast<int>(std::clamp(std::floor(bounds->top()) - 1.0, 0.0, static_cast<double>(height)));
    result.right = static_cast<int>(std::clamp(std::ceil(bounds->right()) + 1.0, 0.0, static_cast<double>(width)));
    result.bottom = static_cast<int>(std::clamp(std::ceil(bounds->bottom()) + 1.0, 0.0, static_cast<double>(height)));
    if (result.empty())
        return std::nullopt;
    return result;
}

std::vector<unsigned char> copy_region(Inkscape::Pixbuf const &pixbuf, PixelBounds const &bounds)
{
    auto const bytes_per_row = static_cast<std::size_t>(bounds.right - bounds.left) * 4;
    auto const rows = static_cast<std::size_t>(bounds.bottom - bounds.top);
    std::vector<unsigned char> copy(bytes_per_row * rows);
    auto const *pixels = pixbuf.pixels();
    auto const stride = pixbuf.rowstride();

    for (int y = bounds.top; y < bounds.bottom; ++y) {
        auto const *source = pixels + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(bounds.left) * 4;
        auto *destination = copy.data() + static_cast<std::size_t>(y - bounds.top) * bytes_per_row;
        std::memcpy(destination, source, bytes_per_row);
    }
    return copy;
}

bool region_changed(Inkscape::Pixbuf const &pixbuf, PixelBounds const &bounds, std::vector<unsigned char> const &before)
{
    auto const bytes_per_row = static_cast<std::size_t>(bounds.right - bounds.left) * 4;
    auto const *pixels = pixbuf.pixels();
    auto const stride = pixbuf.rowstride();

    for (int y = bounds.top; y < bounds.bottom; ++y) {
        auto const *current = pixels + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(bounds.left) * 4;
        auto const *original = before.data() + static_cast<std::size_t>(y - bounds.top) * bytes_per_row;
        if (std::memcmp(current, original, bytes_per_row) != 0)
            return true;
    }
    return false;
}

/// Bytes of "before" scratch kept while erasing one band of the stroke region.
constexpr std::size_t kEraseBandBytes = std::size_t{1} << 20;

} // namespace

bool erase_bitmap_path(Inkscape::Pixbuf &pixbuf, Geom::PathVector const &pixel_path)
{
    if (pixel_path.empty() || pixbuf.width() <= 0 || pixbuf.height() <= 0)
        return false;

    pixbuf.ensurePixelFormat(Inkscape::Pixbuf::PF_CAIRO);
    auto const bounds = path_pixel_bounds(pixel_path, pixbuf.width(), pixbuf.height());
    if (!bounds)
        return false;

    auto *surface = pixbuf.getSurfaceRaw();
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
        return false;

    cairo_surface_flush(surface);
    auto *cr = cairo_create(surface);
    if (!cr || cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        if (cr)
            cairo_destroy(cr);
        return false;
    }
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);

    // Erase band by band so the "before" snapshot used to detect a change
    // stays small even when the stroke covers a 100 MP image. Bands are
    // integer-aligned clips, so every pixel gets exactly the same coverage as
    // in a single fill.
    auto const band_width_bytes = static_cast<std::size_t>(bounds->right - bounds->left) * 4;
    auto const band_rows = static_cast<int>(
        std::clamp<std::size_t>(kEraseBandBytes / std::max<std::size_t>(band_width_bytes, 1), 1,
                                static_cast<std::size_t>(bounds->bottom - bounds->top)));
    bool changed = false;
    bool succeeded = true;
    for (int top = bounds->top; top < bounds->bottom && succeeded; top += band_rows) {
        PixelBounds const band{bounds->left, top, bounds->right, std::min(top + band_rows, bounds->bottom)};
        auto const before = copy_region(pixbuf, band);

        cairo_reset_clip(cr);
        cairo_new_path(cr);
        cairo_rectangle(cr, band.left, band.top, band.right - band.left, band.bottom - band.top);
        cairo_clip(cr);
        feed_pathvector_to_cairo(cr, pixel_path);
        cairo_fill(cr);
        succeeded = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
        cairo_surface_flush(surface);
        changed = changed || (succeeded && region_changed(pixbuf, band, before));
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    if (!succeeded || !changed)
        return false;
    pixbuf.markDirty();
    return true;
}

namespace {

std::optional<PixelBounds> stamp_pixel_bounds(Inkscape::DrawingImageEraseStamp const &stamp,
                                              int width, int height)
{
    if (stamp.unit_to_pixel.isSingular() || width <= 0 || height <= 0)
        return std::nullopt;

    std::array<Geom::Point, 4> const corners = {
        Geom::Point(-1.0, -1.0) * stamp.unit_to_pixel,
        Geom::Point( 1.0, -1.0) * stamp.unit_to_pixel,
        Geom::Point( 1.0,  1.0) * stamp.unit_to_pixel,
        Geom::Point(-1.0,  1.0) * stamp.unit_to_pixel
    };
    double left = corners[0].x();
    double right = corners[0].x();
    double top = corners[0].y();
    double bottom = corners[0].y();
    for (auto const &point : corners) {
        left = std::min(left, point.x());
        right = std::max(right, point.x());
        top = std::min(top, point.y());
        bottom = std::max(bottom, point.y());
    }

    PixelBounds result;
    result.left = static_cast<int>(std::clamp(std::floor(left) - 1.0, 0.0, static_cast<double>(width)));
    result.top = static_cast<int>(std::clamp(std::floor(top) - 1.0, 0.0, static_cast<double>(height)));
    result.right = static_cast<int>(std::clamp(std::ceil(right) + 1.0, 0.0, static_cast<double>(width)));
    result.bottom = static_cast<int>(std::clamp(std::ceil(bottom) + 1.0, 0.0, static_cast<double>(height)));
    if (result.empty())
        return std::nullopt;
    return result;
}

double stamp_coverage(Inkscape::DrawingImageEraseStamp const &stamp, Geom::Affine const &inverse,
                      double edge, Geom::Point const &pixel)
{
    auto const unit = pixel * inverse;
    auto const distance = stamp.shape == Inkscape::DrawingImageEraseShape::Round
                              ? Geom::L2(unit)
                              : std::max(std::abs(unit.x()), std::abs(unit.y()));

    auto const opacity = std::clamp(stamp.opacity, 0.0, 1.0);
    auto const hardness = std::clamp(stamp.hardness, 0.0, 1.0);

    if (hardness >= 0.999) {
        return opacity * std::clamp((1.0 + 0.5 * edge - distance) / edge, 0.0, 1.0);
    }
    if (distance <= hardness)
        return opacity;
    if (distance >= 1.0)
        return 0.0;
    auto const t = std::clamp((distance - hardness) / (1.0 - hardness), 0.0, 1.0);
    auto const smooth = t * t * (3.0 - 2.0 * t);
    return opacity * (1.0 - smooth);
}

} // namespace

bool erase_bitmap_stamps(Inkscape::Pixbuf &pixbuf,
                         std::span<Inkscape::DrawingImageEraseStamp const> stamps)
{
    if (stamps.empty() || pixbuf.width() <= 0 || pixbuf.height() <= 0)
        return false;

    pixbuf.ensurePixelFormat(Inkscape::Pixbuf::PF_CAIRO);
    auto *pixels = pixbuf.pixels();
    auto const stride = pixbuf.rowstride();
    bool changed = false;

    for (auto const &stamp : stamps) {
        auto const bounds = stamp_pixel_bounds(stamp, pixbuf.width(), pixbuf.height());
        if (!bounds)
            continue;

        auto const inverse = stamp.unit_to_pixel.inverse();
        auto const origin = Geom::Point(0.0, 0.0) * inverse;
        auto const dx = Geom::L2(Geom::Point(1.0, 0.0) * inverse - origin);
        auto const dy = Geom::L2(Geom::Point(0.0, 1.0) * inverse - origin);
        auto const edge = std::clamp(std::max(dx, dy), 1e-6, 1.0);

        for (int y = bounds->top; y < bounds->bottom; ++y) {
            auto *row = pixels + static_cast<std::size_t>(y) * stride;
            for (int x = bounds->left; x < bounds->right; ++x) {
                auto const coverage = stamp_coverage(stamp, inverse, edge, Geom::Point(x + 0.5, y + 0.5));
                if (coverage <= 0.0)
                    continue;

                auto *pixel = row + static_cast<std::size_t>(x) * 4;
                auto const remaining = 1.0 - coverage;
                for (int channel = 0; channel < 4; ++channel) {
                    auto const before = pixel[channel];
                    pixel[channel] = static_cast<unsigned char>(std::lround(before * remaining));
                    changed |= pixel[channel] != before;
                }
            }
        }
    }

    if (changed)
        pixbuf.markDirty();
    return changed;
}

bool bitmap_pixels_equal(Inkscape::Pixbuf const &a, Inkscape::Pixbuf const &b)
{
    if (a.width() != b.width() || a.height() != b.height())
        return false;

    // Compare the visible (premultiplied) pixels. A GDK-layout row is
    // converted into Cairo's layout in a scratch row first, so pixels with
    // alpha 0 are equal whatever colour they carry. No image is copied.
    auto const bytes_per_row = static_cast<std::size_t>(a.width()) * 4;
    bool const a_gdk = a.pixelFormat() == Inkscape::Pixbuf::PF_GDK;
    bool const b_gdk = b.pixelFormat() == Inkscape::Pixbuf::PF_GDK;
    std::vector<unsigned char> a_scratch(a_gdk ? bytes_per_row : 0);
    std::vector<unsigned char> b_scratch(b_gdk ? bytes_per_row : 0);
    for (int y = 0; y < a.height(); ++y) {
        auto const *ap = a.pixels() + static_cast<std::size_t>(y) * a.rowstride();
        auto const *bp = b.pixels() + static_cast<std::size_t>(y) * b.rowstride();
        if (a_gdk) {
            std::memcpy(a_scratch.data(), ap, bytes_per_row);
            ::convert_pixels_pixbuf_to_argb32(a_scratch.data(), a.width(), 1, static_cast<int>(bytes_per_row));
            ap = a_scratch.data();
        }
        if (b_gdk) {
            std::memcpy(b_scratch.data(), bp, bytes_per_row);
            ::convert_pixels_pixbuf_to_argb32(b_scratch.data(), b.width(), 1, static_cast<int>(bytes_per_row));
            bp = b_scratch.data();
        }
        if (std::memcmp(ap, bp, bytes_per_row) != 0)
            return false;
    }
    return true;
}

namespace {

/// Replace the href of every encoded image. Atomic: if an allocation fails
/// half way, everything done so far is rolled back out of the open XML
/// transaction (no Undo step, no leftover change) and std::bad_alloc is rethrown.
/// Without a fence (a document that is not recording Undo) nothing can be rolled back.
template <typename Entries>
void apply_encoded(SPDocument *document, Entries &entries)
{
    auto fence = DocumentUndo::detachPendingChanges(document);
    try {
        for (auto &entry : entries) {
            if (!sp_image_allocation_allowed("apply"))
                throw std::bad_alloc(); // test seam
            auto *image = entry.target->item.get();
            Inkscape::setHrefAttribute(*image->getRepr(), entry.data_uri.c_str());
            std::string().swap(entry.data_uri); // the XML node holds its own copy now
            image->getRepr()->removeAttribute("sodipodi:absref");
        }
        document->ensureUpToDate();
    } catch (std::bad_alloc const &) {
        if (fence) {
            DocumentUndo::rollbackToDetachedChanges(document, *fence);
            try {
                document->ensureUpToDate();
            } catch (...) {
            }
        }
        throw;
    }
    if (fence)
        DocumentUndo::reattachPendingChanges(document, *fence);
}

} // namespace

struct BitmapEraseSession::Target
{
    explicit Target(SPImage *image)
        : item(image)
        , canonical(image ? image->pixbuf : nullptr)
    {}

    SPWeakPtr<SPImage> item;
    std::shared_ptr<Inkscape::Pixbuf const> canonical;
    std::shared_ptr<Inkscape::Pixbuf> working;
    Geom::Affine document_to_pixel;
    bool visible = false;
    bool erase_visible = false;
    std::vector<Inkscape::DrawingImageEraseStamp> stamps;
    std::shared_ptr<Inkscape::DrawingImageErasePreview const> erase_preview;
    std::size_t baked_stamps = 0;  ///< prefix of `stamps` already baked into `working`
    std::size_t chain_length = 0;  ///< chunks in `erase_preview` (not yet baked)
    /// Previous baked buffer, kept (small images only) so the next bake can
    /// reuse it once the drawing no longer references it. It holds the pixels
    /// of canonical + stamps[0, spare_baked).
    std::shared_ptr<Inkscape::Pixbuf> spare;
    std::size_t spare_baked = 0;
    sigc::scoped_connection released;
    sigc::scoped_connection modified;
};

BitmapEraseSession::BitmapEraseSession(SPDesktop *desktop)
    : _desktop(desktop)
{}

BitmapEraseSession::~BitmapEraseSession()
{
    cancel();
}

bool BitmapEraseSession::begin(std::vector<SPImage *> const &images)
{
    cancel();
    if (!_desktop || !_desktop->getDocument())
        return false;

    std::unordered_set<SPImage *> seen;
    _targets.reserve(images.size());
    for (auto *image : images) {
        if (!image || !seen.insert(image).second || image->missing || !image->pixbuf ||
            image->document != _desktop->getDocument() || !image->getRepr() || !image->get_arenaitem(_desktop->dkey)) {
            continue;
        }

        auto const pixel_to_document = image->pixelToDocumentAffine();
        if (!pixel_to_document || pixel_to_document->isSingular())
            continue;

        auto target = std::make_unique<Target>(image);
        target->document_to_pixel = pixel_to_document->inverse();
        target->released = image->connectRelease([this](auto *) {
            if (!_committing)
                cancel();
        });
        target->modified = image->connectModified([this](auto *, auto) {
            if (!_committing)
                cancel();
        });
        _targets.emplace_back(std::move(target));
    }
    return !_targets.empty();
}

Geom::PathVector BitmapEraseSession::toPixels(Target const &target, Geom::PathVector const &stroke_desktop) const
{
    if (!_desktop)
        return {};
    return stroke_desktop * (_desktop->dt2doc() * target.document_to_pixel);
}

std::optional<Inkscape::DrawingImageEraseStamp> BitmapEraseSession::toPixels(
    Target const &target, BitmapBrushStamp const &stamp) const
{
    if (!_desktop || !target.canonical || !(stamp.diameter > 0.0) || !std::isfinite(stamp.diameter))
        return std::nullopt;

    Inkscape::DrawingImageEraseStamp result;
    result.shape = stamp.shape;
    result.hardness = std::clamp(stamp.hardness, 0.0, 1.0);
    result.opacity = std::clamp(stamp.opacity, 0.0, 1.0);
    auto const radius = stamp.diameter * 0.5;
    result.unit_to_pixel = Geom::Scale(radius) * Geom::Translate(stamp.center_desktop) *
                           _desktop->dt2doc() * target.document_to_pixel;

    if (!stamp_pixel_bounds(result, target.canonical->width(), target.canonical->height()))
        return std::nullopt;
    return result;
}

bool BitmapEraseSession::preview(Geom::PathVector const &stroke_desktop)
{
    try {
        return previewPathImpl(stroke_desktop);
    } catch (std::bad_alloc const &) {
        reportOutOfMemory();
        cancel();
        return false;
    }
}

bool BitmapEraseSession::previewPathImpl(Geom::PathVector const &stroke_desktop)
{
    if (!_desktop || stroke_desktop.empty() || _targets.empty())
        return false;

    bool changed = false;
    for (auto &target : _targets) {
        auto *image = target->item.get();
        if (!image || image->document != _desktop->getDocument()) {
            cancel();
            return false;
        }

        // A DrawingImage may be rendered by worker threads. Never mutate a
        // pixbuf after publishing it to the drawing tree; build the next
        // preview copy first, then atomically hand that immutable snapshot to
        // the display item.
        auto const pixel_path = toPixels(*target, stroke_desktop);
        auto const &base = target->working ? *target->working : *target->canonical;
        if (!path_pixel_bounds(pixel_path, base.width(), base.height()))
            continue; // the stroke misses this image: nothing to copy
        std::shared_ptr<Inkscape::Pixbuf> next = sp_image_try_copy_pixbuf(base);
        if (!next) {
            reportOutOfMemory();
            cancel();
            return false;
        }
        if (!erase_bitmap_path(*next, pixel_path))
            continue;

        if (!image->setViewPixbuf(_desktop->dkey, next)) {
            cancel();
            return false;
        }
        target->working = std::move(next);
        target->spare.reset(); // the stamp bookkeeping of the spare no longer matches
        target->visible = true;
        changed = true;
    }
    return changed;
}

bool BitmapEraseSession::preview(std::span<BitmapBrushStamp const> stamps)
{
    try {
        return previewStampsImpl(stamps);
    } catch (std::bad_alloc const &) {
        reportOutOfMemory();
        cancel();
        return false;
    }
}

bool BitmapEraseSession::previewStampsImpl(std::span<BitmapBrushStamp const> stamps)
{
    if (!_desktop || stamps.empty() || _targets.empty())
        return false;

    bool changed = false;
    for (auto &target : _targets) {
        auto *image = target->item.get();
        if (!image || image->document != _desktop->getDocument()) {
            cancel();
            return false;
        }

        std::vector<Inkscape::DrawingImageEraseStamp> converted;
        converted.reserve(stamps.size());
        for (auto const &stamp : stamps) {
            if (auto pixel_stamp = toPixels(*target, stamp)) {
                converted.emplace_back(std::move(*pixel_stamp));
            }
        }
        if (converted.empty())
            continue;

        target->stamps.insert(target->stamps.end(), converted.begin(), converted.end());
        auto preview = std::make_shared<Inkscape::DrawingImageErasePreview>();
        preview->previous = target->erase_preview;
        preview->stamps = std::move(converted);
        if (!image->setViewErasePreview(_desktop->dkey, preview)) {
            cancel();
            return false;
        }
        target->erase_preview = std::move(preview);
        target->erase_visible = true;
        ++target->chain_length;
        changed = true;

        if (target->chain_length >= bakeLimitFor(*target)) {
            // Bake on the main thread and restart the chain. Both publications
            // are deferred in order and replayed together between renders
            // (Drawing::unsnapshot), so a render never shows the baked pixels
            // plus the chain they already contain.
            auto const bake = bakeChain(*target, *image);
            if (bake == BakeResult::OutOfMemory) {
                reportOutOfMemory();
                cancel();
                return false;
            }
            if (bake == BakeResult::Failed) {
                cancel();
                return false;
            }
        }
    }
    return changed;
}

std::size_t BitmapEraseSession::bakeLimitFor(int width, int height)
{
    auto const pixels = static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0));
    return std::clamp<std::size_t>(pixels / 20000, kDefaultBakeChunkLimit, kMaxAutoBakeChunkLimit);
}

std::size_t BitmapEraseSession::bakeLimitFor(Target const &target) const
{
    if (_bake_chunk_limit)
        return _bake_chunk_limit;
    return target.canonical ? bakeLimitFor(target.canonical->width(), target.canonical->height())
                            : kDefaultBakeChunkLimit;
}

BitmapEraseSession::BakeResult BitmapEraseSession::bakeChain(Target &target, SPImage &image)
{
    // Images up to this size keep one spare buffer so a bake does not have to
    // allocate and copy the whole bitmap; larger ones prefer lower memory.
    constexpr std::size_t kSpareMaxBytes = std::size_t{64} << 20;

    std::shared_ptr<Inkscape::Pixbuf> baked;
    std::size_t from = target.baked_stamps;
    bool reused = false;
    if (target.spare && target.spare.use_count() == 1) {
        // Only this session still references the previous buffer, so it can
        // be updated in place: no render can be reading it any more.
        baked = std::move(target.spare);
        from = target.spare_baked;
        reused = true;
    } else {
        std::shared_ptr<Inkscape::Pixbuf> copy =
            sp_image_try_copy_pixbuf(target.working ? *target.working : *target.canonical);
        if (!copy)
            return BakeResult::OutOfMemory;
        baked = std::move(copy);
    }
    target.spare.reset();

    auto const pending = std::span<Inkscape::DrawingImageEraseStamp const>(target.stamps).subspan(from);
    bool const pixels_changed = erase_bitmap_stamps(*baked, pending) || reused;
    if (pixels_changed && !image.setViewPixbuf(_desktop->dkey, baked))
        return BakeResult::Failed;
    if (!image.setViewErasePreview(_desktop->dkey, nullptr))
        return BakeResult::Failed;
    if (pixels_changed) {
        auto const bytes = static_cast<std::size_t>(baked->rowstride()) * static_cast<std::size_t>(baked->height());
        if (target.working && bytes <= kSpareMaxBytes) {
            target.spare = std::move(target.working);
            target.spare_baked = target.baked_stamps;
        }
        target.working = std::move(baked);
        target.visible = true;
    }
    target.baked_stamps = target.stamps.size();
    target.erase_preview.reset();
    target.chain_length = 0;
    return BakeResult::Done;
}

void BitmapEraseSession::reportEncodeFailure() const
{
    if (sp_image_last_encode_failure() == SPImageEncodeFailure::OutOfMemory) {
        reportOutOfMemory();
    } else if (_desktop && _desktop->messageStack()) {
        _desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                                        _("Could not encode the erased bitmap; nothing was changed"));
    }
}

void BitmapEraseSession::reportOutOfMemory() const
{
    if (_desktop && _desktop->messageStack()) {
        _desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                                        _("Not enough memory to apply the eraser on this image"));
    }
}

std::size_t BitmapEraseSession::previewChainLength() const
{
    std::size_t longest = 0;
    for (auto const &target : _targets)
        longest = std::max(longest, target->chain_length);
    return longest;
}

std::shared_ptr<Inkscape::Pixbuf> BitmapEraseSession::effectivePreview(std::size_t index) const
{
    if (index >= _targets.size())
        return nullptr;
    auto const &target = *_targets[index];
    if (!target.canonical)
        return nullptr;
    std::shared_ptr<Inkscape::Pixbuf> result =
        sp_image_try_copy_pixbuf(target.working ? *target.working : *target.canonical);
    if (!result)
        return nullptr;
    erase_bitmap_stamps(*result, std::span<Inkscape::DrawingImageEraseStamp const>(target.stamps)
                                     .subspan(target.baked_stamps));
    return result;
}

bool BitmapEraseSession::commit(Geom::PathVector const &stroke_desktop)
{
    try {
        return commitPathImpl(stroke_desktop);
    } catch (std::bad_alloc const &) {
        _committing = false;
        reportOutOfMemory();
        cancel();
        return false;
    }
}

bool BitmapEraseSession::commitPathImpl(Geom::PathVector const &stroke_desktop)
{
    if (!_desktop || stroke_desktop.empty() || _targets.empty())
        return false;
    auto *document = _desktop->getDocument();
    if (!document)
        return false;

    struct EncodedTarget
    {
        Target *target = nullptr;
        std::string data_uri;
    };

    for (auto &target : _targets)
        target->spare.reset(); // not needed for a path commit; free it before the working copy

    std::vector<EncodedTarget> encoded;
    encoded.reserve(_targets.size());
    for (auto &target : _targets) {
        auto *image = target->item.get();
        if (!image || image->document != document || !image->getRepr() || image->pixbuf != target->canonical) {
            cancel();
            return false;
        }

        // A stroke that misses the bitmap needs no working copy. erase_bitmap_path
        // reports whether any pixel really changed, so no second comparison
        // (and no extra image copies) is needed.
        auto const pixel_path = toPixels(*target, stroke_desktop);
        if (!path_pixel_bounds(pixel_path, target->canonical->width(), target->canonical->height()))
            continue;
        auto pixels = sp_image_try_copy_pixbuf(*target->canonical);
        if (!pixels) {
            reportOutOfMemory();
            cancel();
            return false;
        }
        if (!erase_bitmap_path(*pixels, pixel_path))
            continue;

        auto data_uri = sp_image_encode_png_data_uri(std::move(pixels));
        if (!data_uri) {
            reportEncodeFailure();
            cancel();
            return false;
        }
        encoded.push_back({target.get(), std::move(*data_uri)});
    }

    if (encoded.empty()) {
        cancel();
        return false;
    }

    _committing = true;
    try {
        apply_encoded(document, encoded);
    } catch (std::bad_alloc const &) {
        _committing = false;
        reportOutOfMemory();
        cancel();
        return false;
    }

    // The canonical SPImage pixbufs have now reloaded from the committed PNGs.
    // Keep the transient preview until this point to avoid a one-frame flash.
    // The document change is final now: a failure to refresh the view must
    // not turn this commit into a reported failure.
    try {
        for (auto &entry : encoded) {
            if (auto *image = entry.target->item.get()) {
                image->setViewPixbuf(_desktop->dkey, image->pixbuf);
            }
        }
    } catch (std::bad_alloc const &) {
    }
    _committing = false;
    _targets.clear();
    return true;
}

bool BitmapEraseSession::commit()
{
    try {
        return commitStampsImpl();
    } catch (std::bad_alloc const &) {
        _committing = false;
        reportOutOfMemory();
        cancel();
        return false;
    }
}

bool BitmapEraseSession::commitStampsImpl()
{
    if (!_desktop || _targets.empty())
        return false;
    auto *document = _desktop->getDocument();
    if (!document)
        return false;

    struct EncodedTarget
    {
        Target *target = nullptr;
        std::string data_uri;
    };

    // The spare preview buffers are not needed any more; free them before
    // the commit allocates its own working copy.
    for (auto &target : _targets)
        target->spare.reset();

    std::vector<EncodedTarget> encoded;
    encoded.reserve(_targets.size());
    for (auto &target : _targets) {
        auto *image = target->item.get();
        if (!image || image->document != document || !image->getRepr() || image->pixbuf != target->canonical) {
            cancel();
            return false;
        }
        if (target->stamps.empty() ||
            std::none_of(target->stamps.begin(), target->stamps.end(),
                         [](auto const &stamp) { return stamp.opacity > 0.0; })) {
            continue; // nothing could change: no working copy
        }

        auto pixels = sp_image_try_copy_pixbuf(*target->canonical);
        if (!pixels) {
            reportOutOfMemory();
            cancel();
            return false;
        }
        // erase_bitmap_stamps reports whether any pixel changed; an unchanged
        // result is a no-op without an Undo step.
        if (!erase_bitmap_stamps(*pixels, target->stamps))
            continue;

        auto data_uri = sp_image_encode_png_data_uri(std::move(pixels));
        if (!data_uri) {
            reportEncodeFailure();
            cancel();
            return false;
        }
        encoded.push_back({target.get(), std::move(*data_uri)});
    }

    if (encoded.empty()) {
        cancel();
        return false;
    }

    _committing = true;
    try {
        apply_encoded(document, encoded);
    } catch (std::bad_alloc const &) {
        _committing = false;
        reportOutOfMemory();
        cancel();
        return false;
    }

    // Keep the exact transient preview visible until every replacement PNG is
    // loaded, then remove the view-local masks without touching other views.
    try {
        for (auto &target : _targets) {
            if (auto *image = target->item.get()) {
                image->setViewErasePreview(_desktop->dkey, nullptr);
            }
        }
    } catch (std::bad_alloc const &) {
    }
    _committing = false;
    _targets.clear();
    return true;
}

void BitmapEraseSession::restoreCanonical() noexcept
{
    if (!_desktop)
        return;
    for (auto const &target : _targets) {
        auto *image = target->item.get();
        if (image && target->visible)
            image->setViewPixbuf(_desktop->dkey, target->canonical);
        if (image && target->erase_visible)
            image->setViewErasePreview(_desktop->dkey, nullptr);
    }
}

void BitmapEraseSession::cancel() noexcept
{
    restoreCanonical();
    _targets.clear();
    _committing = false;
}

} // namespace Inkscape::UI::Tools
