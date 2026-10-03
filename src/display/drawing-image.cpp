// SPDX-License-Identifier: GPL-2.0-or-later
#include "display/preview-render-budget.h"
/**
 * @file
 * Bitmap image belonging to an SVG drawing.
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2011 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <2geom/bezier-curve.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "drawing.h"
#include "async/bitmap-job-reaper.h"
#include "drawing-context.h"
#include "drawing-image.h"
#include "nr-filter.h"
#include "cairo-utils.h"
#include "cairo-templates.h"

namespace Inkscape {
namespace {

void append_stamp_path(DrawingContext &dc, DrawingImageEraseShape shape)
{
    dc.newPath();
    if (shape == DrawingImageEraseShape::Round) {
        cairo_arc(dc.raw(), 0.0, 0.0, 1.0, 0.0, 2.0 * M_PI);
    } else {
        cairo_rectangle(dc.raw(), -1.0, -1.0, 2.0, 2.0);
    }
}

double smoothstep(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

void erase_stamp(DrawingContext &dc, DrawingImageEraseStamp const &stamp)
{
    if (stamp.unit_to_pixel.isSingular()) {
        return;
    }

    auto const opacity = std::clamp(stamp.opacity, 0.0, 1.0);
    auto const hardness = std::clamp(stamp.hardness, 0.0, 1.0);
    if (opacity <= 0.0) {
        return;
    }

    DrawingContext::Save save(dc);
    dc.transform(stamp.unit_to_pixel);

    if (hardness >= 0.999) {
        append_stamp_path(dc, stamp.shape);
        dc.setSource(0.0, 0.0, 0.0, opacity);
        dc.fill();
        return;
    }

    if (stamp.shape == DrawingImageEraseShape::Round) {
        append_stamp_path(dc, stamp.shape);
        auto *gradient = cairo_pattern_create_radial(0.0, 0.0, 0.0, 0.0, 0.0, 1.0);
        cairo_pattern_add_color_stop_rgba(gradient, 0.0, 0.0, 0.0, 0.0, opacity);
        cairo_pattern_add_color_stop_rgba(gradient, hardness, 0.0, 0.0, 0.0, opacity);
        // Approximate the same smoothstep falloff used by the committed pixel
        // operation so a soft live preview does not visibly jump on release.
        constexpr int stops = 24;
        for (int i = 1; i <= stops; ++i) {
            auto const t = static_cast<double>(i) / stops;
            auto const radius = hardness + (1.0 - hardness) * t;
            auto const alpha = opacity * (1.0 - smoothstep(t));
            cairo_pattern_add_color_stop_rgba(gradient, radius, 0.0, 0.0, 0.0, alpha);
        }
        dc.setSource(gradient);
        cairo_pattern_destroy(gradient);
        dc.fill();
        return;
    }

    // Cairo has a radial gradient but no square-distance gradient. Build a
    // small set of nested square layers whose cumulative alpha follows the
    // same smooth falloff used by the destructive pixel operation.
    constexpr int layers = 24;
    double cumulative = 0.0;
    for (int i = 1; i <= layers; ++i) {
        auto const t = static_cast<double>(i) / layers;
        auto const extent = 1.0 - (1.0 - hardness) * t;
        auto const desired = opacity * smoothstep(t);
        auto const incremental = cumulative < 1.0 ? (desired - cumulative) / (1.0 - cumulative) : 0.0;
        cumulative = desired;
        if (incremental <= 0.0) {
            continue;
        }
        dc.newPath();
        cairo_rectangle(dc.raw(), -extent, -extent, 2.0 * extent, 2.0 * extent);
        dc.setSource(0.0, 0.0, 0.0, incremental);
        dc.fill();
    }
}

void erase_preview(DrawingContext &dc, std::shared_ptr<DrawingImageErasePreview const> const &preview)
{
    // Stamps wholly outside the visible clip cannot change any pixel; skip
    // them before building a path, gradient or fill. The clip is in the
    // current (image pixel) user space.
    double cx1 = 0.0, cy1 = 0.0, cx2 = 0.0, cy2 = 0.0;
    cairo_clip_extents(dc.raw(), &cx1, &cy1, &cx2, &cy2);
    Geom::Rect const visible(Geom::Point(cx1, cy1), Geom::Point(cx2, cy2));
    bool const cull = std::isfinite(cx1) && std::isfinite(cy1) && std::isfinite(cx2) && std::isfinite(cy2);

    for (auto const *chunk = preview.get(); chunk; chunk = chunk->previous.get()) {
        for (auto const &stamp : chunk->stamps) {
            if (cull) {
                Geom::Rect const bounds = Geom::Rect(Geom::Point(-1.0, -1.0), Geom::Point(1.0, 1.0)) *
                                          stamp.unit_to_pixel;
                if (!bounds.intersects(visible)) {
                    continue;
                }
            }
            erase_stamp(dc, stamp);
        }
    }
}

} // namespace

DrawingImageErasePreview::~DrawingImageErasePreview()
{
    // Take the tail and unlink it one node at a time while this thread is the
    // sole owner, so destruction depth stays constant for any chain length.
    auto tail = std::move(previous);
    while (tail && tail.use_count() == 1) {
        auto next = std::move(const_cast<DrawingImageErasePreview &>(*tail).previous);
        tail = std::move(next);
    }
}

DrawingImage::DrawingImage(Drawing &drawing)
    : DrawingItem(drawing)
    , style_image_rendering(SP_CSS_IMAGE_RENDERING_AUTO)
    , _extend(CAIRO_EXTEND_NONE) // NONE prevents artifacts in surrounding empty space
{
    static std::atomic<std::uint64_t> next_incarnation{0};
    auto value = next_incarnation.load();
    // Never wrap or reuse; at exhaustion new views stay unavailable (incarnation 0).
    while (value != UINT64_MAX && !next_incarnation.compare_exchange_weak(value, value + 1)) {}
    _incarnation = value == UINT64_MAX ? 0 : value + 1;
}

void DrawingImage::_bumpRevision()
{
    if (_revision != UINT64_MAX) ++_revision;
    _dependency_changed.emit(); // synchronous even when the stamp is unavailable
}

Bitmap::ViewDependencyStamp DrawingImage::dependencyStamp() const
{
    Bitmap::assertBitmapMainThread();
    if (!_incarnation || _revision == UINT64_MAX) return {};
    return {_incarnation, _revision};
}

struct DrawingImage::PendingInstallation {
    std::shared_ptr<Pixbuf const> pixels;
    std::optional<std::unique_ptr<Filters::Filter>> renderer;
    Bitmap::SuppressedOwnEffects effects = Bitmap::SuppressedOwnEffects::None;
    Bitmap::Generation generation = 0;
    bool replace_pixels = true;
};

void DrawingImage::setPixbuf(std::shared_ptr<Inkscape::Pixbuf const> pixbuf)
{
    // Legacy previews derive from canonical pixels and need native own effects.
    setCanonicalPixbuf(std::move(pixbuf));
}

void DrawingImage::setCanonicalPixbuf(std::shared_ptr<Pixbuf const> pixels)
{
    // Called by the existing main-thread SPImage update/show path, including
    // headless render clients that do not start the bitmap job service.
    if (_generation != UINT64_MAX) ++_generation;
    _bumpRevision();
    _pending_generation = _accepted_generation = 0;
    _installation.reset(); // also a queue-order barrier for subsequent composed work
    defer([this, pixels = std::move(pixels)] () mutable {
        _pixbuf = std::move(pixels);
        _suppressed = Bitmap::SuppressedOwnEffects::None;
        _markForUpdate(STATE_ALL, false);
    });
}

Bitmap::Generation DrawingImage::composedGeneration()
{
    Bitmap::assertBitmapMainThread();
    // Zero is reserved for unavailable/consumed tickets. Do not wrap/reuse.
    if (_generation == UINT64_MAX) return 0;
    _bumpRevision();
    _accepted_generation = 0; // retire queued payload immediately, retaining only the queue slot
    if (_installation) *_installation = PendingInstallation{};
    return _pending_generation = ++_generation;
}

bool DrawingImage::setComposedPixels(std::shared_ptr<Pixbuf const> pixels,
                                    Bitmap::SuppressedOwnEffects effects, Bitmap::Generation generation,
                                    std::unique_ptr<Filters::Filter> *renderer, bool replace_pixels)
{
    Bitmap::assertBitmapMainThread();
    if (!generation || generation != _pending_generation) return false;
    _pending_generation = 0;
    _accepted_generation = generation;
    _bumpRevision();
    bool const enqueue = !_installation;
    if (enqueue) _installation = std::make_shared<PendingInstallation>();
    auto &pending = *_installation;
    pending.pixels = replace_pixels ? std::move(pixels) : nullptr;
    pending.effects = effects;
    pending.generation = generation;
    pending.replace_pixels = replace_pixels;
    if (renderer) pending.renderer.emplace(std::move(*renderer));
    else pending.renderer.reset();
    if (enqueue) defer([this, weak = std::weak_ptr(_installation)] {
        auto pending = weak.lock();
        if (!pending || pending != _installation) return;
        _installation.reset();
        if (!pending->generation || pending->generation != _accepted_generation) return;
        if (pending->replace_pixels) {
            _pixbuf = std::move(pending->pixels);
            _suppressed = pending->effects;
        }
        // Unsnapshot has ended: this setter executes inside the same mutation.
        if (pending->renderer) setFilterRenderer(std::move(*pending->renderer));
        _markForUpdate(STATE_ALL, false);
    });
    return true;
}

bool DrawingImage::suppressesOwnEffect(Bitmap::SuppressedOwnEffects effect) const
{
    return (static_cast<unsigned>(_suppressed) & static_cast<unsigned>(effect)) != 0;
}

void DrawingImage::setErasePreview(std::shared_ptr<DrawingImageErasePreview const> preview)
{
    _bumpRevision();
    defer([this, preview = std::move(preview)] () mutable {
        _erase_preview = std::move(preview);
        _markForUpdate(STATE_ALL, false);
    });
}

void DrawingImage::setScale(double sx, double sy)
{
    defer([=, this] {
        _scale = Geom::Scale(sx, sy);
        _markForUpdate(STATE_ALL, false);
    });
}

void DrawingImage::setOrigin(Geom::Point const &origin)
{
    defer([=, this] {
        _origin = origin;
        _markForUpdate(STATE_ALL, false);
    });
}

void DrawingImage::setClipbox(Geom::Rect const &box)
{
    defer([=, this] {
        _clipbox = box;
        _markForUpdate(STATE_ALL, false);
    });
}

void DrawingImage::setExtend(cairo_extend_t extend)
{
    defer([=, this] {
        _extend = extend;
        _markForUpdate(STATE_ALL, false);
    });
}

Geom::Rect DrawingImage::bounds() const
{
    if (!_pixbuf) return _clipbox;

    double pw = _pixbuf->width();
    double ph = _pixbuf->height();
    double vw = pw * _scale[Geom::X];
    double vh = ph * _scale[Geom::Y];
    Geom::Point wh(vw, vh);
    Geom::Rect view(_origin, _origin+wh);
    Geom::OptRect res = _clipbox & view;
    Geom::Rect ret = res ? *res : _clipbox;

    return ret;
}

void DrawingImage::setStyle(SPStyle const *style, SPStyle const *context_style)
{
    DrawingItem::setStyle(style, context_style);

    auto image_rendering = SP_CSS_IMAGE_RENDERING_AUTO;
    if (_style) {
        image_rendering = _style->image_rendering.computed;
    }

    defer([=, this] {
        style_image_rendering = image_rendering;
    });
}

unsigned DrawingImage::_updateItem(Geom::IntRect const &, UpdateContext const &, unsigned, unsigned)
{
    // Calculate bbox
    if (_pixbuf) {
        Geom::Rect r = bounds() * _ctm;
        PreviewRenderBudget::rect(r);
        _bbox = r.roundOutwards();
    } else {
        _bbox = Geom::OptIntRect();
    }

    return STATE_ALL;
}

unsigned DrawingImage::_renderItem(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &/*area*/, unsigned flags, DrawingItem const */*stop_at*/) const
{
    bool const outline = (flags & RENDER_OUTLINE) && !_drawing.imageOutlineMode();

    if (!outline) {
        if (!_pixbuf) return RENDER_OK;
        if (_scale.vector().x() * _scale.vector().y() == 0.0) return RENDER_OK;

        Inkscape::DrawingContext::Save save(dc);
        dc.transform(_ctm);
        // With EXTEND_NONE the bitmap already samples transparent outside its
        // pixels. A second antialiased rectangle darkens that same edge again,
        // making a lossless alpha trim visibly change it at fractional zoom.
        // Keep the viewport clip when it actually crops the image (slice) or
        // when a nontransparent extension mode requires it.
        Geom::Rect const image_box(_origin, _origin + Geom::Point(
            _pixbuf->width() * _scale[Geom::X], _pixbuf->height() * _scale[Geom::Y]));
        if (_extend != CAIRO_EXTEND_NONE || !_clipbox.contains(image_box)) {
            dc.newPath();
            dc.rectangle(_clipbox);
            dc.clip();
        }

        dc.translate(_origin);
        dc.scale(_scale);
        // const_cast required since Cairo needs to modify the internal refcount variable, but we do not want to give up the
        // benefits of const for the rest of our code. The underlying object is guaranteed to be non-const, so this is well-defined.
        // It is also thread-safe to modify the refcount in this way, since Cairo uses atomics internally.
        dc.setSource(const_cast<cairo_surface_t*>(_pixbuf->getSurfaceRaw()), 0, 0);
        dc.patternSetExtend(_extend);

        // See: http://www.w3.org/TR/SVG/painting.html#ImageRenderingProperty
        //      https://drafts.csswg.org/css-images-3/#the-image-rendering
        //      style.h/style.cpp, cairo-render-context.cpp
        //
        // CSS 3 defines:
        //   'optimizeSpeed' as alias for "pixelated"
        //   'optimizeQuality' as alias for "smooth"
        switch (style_image_rendering) {
            case SP_CSS_IMAGE_RENDERING_OPTIMIZESPEED:
            case SP_CSS_IMAGE_RENDERING_PIXELATED:
            // we don't have an implementation for crisp-edges, but it should *not* smooth or blur
            case SP_CSS_IMAGE_RENDERING_CRISPEDGES:
                dc.patternSetFilter( CAIRO_FILTER_NEAREST );
                break;
            case SP_CSS_IMAGE_RENDERING_AUTO:
            case SP_CSS_IMAGE_RENDERING_OPTIMIZEQUALITY:
            default:
                // In recent Cairo, BEST used Lanczos3, which is prohibitively slow
                dc.patternSetFilter( CAIRO_FILTER_GOOD );
                break;
        }

        // Handle an exceptional case where the greyscale color mode needs to be applied per-image.
        bool const greyscale_exception = (flags & RENDER_OUTLINE) && _drawing.colorMode() == ColorMode::GRAYSCALE;
        bool const erase_preview_active = _erase_preview && !_erase_preview->stamps.empty();
        if (greyscale_exception || erase_preview_active) {
            dc.pushGroup();
        }

        dc.paint();

        if (erase_preview_active) {
            DrawingContext::Save erase_state(dc);
            dc.setOperator(CAIRO_OPERATOR_DEST_OUT);
            erase_preview(dc, _erase_preview);
        }

        if (greyscale_exception || erase_preview_active) {
            if (greyscale_exception) {
                ink_cairo_surface_filter(dc.rawTarget(), dc.rawTarget(), _drawing.grayscaleMatrix());
            }
            dc.popGroupToSource();
            dc.paint();
        }

    } else { // outline; draw a rect instead

        auto rgba = Colors::Color(_drawing.imageOutlineColor());

        {   Inkscape::DrawingContext::Save save(dc);
            dc.transform(_ctm);
            dc.newPath();

            Geom::Rect r = bounds();
            Geom::Point c00 = r.corner(0);
            Geom::Point c01 = r.corner(3);
            Geom::Point c11 = r.corner(2);
            Geom::Point c10 = r.corner(1);

            dc.moveTo(c00);
            // the box
            dc.lineTo(c10);
            dc.lineTo(c11);
            dc.lineTo(c01);
            dc.lineTo(c00);
            // the diagonals
            dc.lineTo(c11);
            dc.moveTo(c10);
            dc.lineTo(c01);
        }

        dc.setLineWidth(0.5);
        dc.setSource(rgba);
        dc.stroke();
    }
    return RENDER_OK;
}

/** Calculates the closest distance from p to the segment a1-a2*/
static double distance_to_segment(Geom::Point const &p, Geom::Point const &a1, Geom::Point const &a2)
{
    Geom::LineSegment l(a1, a2);
    Geom::Point np = l.pointAt(l.nearestTime(p));
    return Geom::distance(np, p);
}

DrawingItem *DrawingImage::_pickItem(Geom::Point const &p, double delta, Geom::OptIntRect const &area_world, unsigned flags)
{
    if (!_pixbuf) return nullptr;

    bool outline = (flags & PICK_OUTLINE) && !_drawing.imageOutlineMode();

    if (outline) {
        Geom::Rect r = bounds();
        Geom::Point pick = p * _ctm.inverse();

        // find whether any side or diagonal is within delta
        // to do so, iterate over all pairs of corners
        for (unsigned i = 0; i < 3; ++i) { // for i=3, there is nothing to do
            for (unsigned j = i+1; j < 4; ++j) {
                if (distance_to_segment(pick, r.corner(i), r.corner(j)) < delta) {
                    return this;
                }
            }
        }
        return nullptr;

    } else {
        auto pixels = _pixbuf->pixels();
        int width = _pixbuf->width();
        int height = _pixbuf->height();
        size_t rowstride = _pixbuf->rowstride();

        Geom::Point tp = p * _ctm.inverse();
        Geom::Rect r = bounds();

        if (!r.contains(tp))
            return nullptr;

        double vw = width * _scale[Geom::X];
        double vh = height * _scale[Geom::Y];
        int ix = floor((tp[Geom::X] - _origin[Geom::X]) / vw * width);
        int iy = floor((tp[Geom::Y] - _origin[Geom::Y]) / vh * height);

        if ((ix < 0) || (iy < 0) || (ix >= width) || (iy >= height))
            return nullptr;

        auto pix_ptr = pixels + iy * rowstride + ix * 4;
        // pick if the image is less than 99% transparent
        guint32 alpha = 0;
        if (_pixbuf->pixelFormat() == Inkscape::Pixbuf::PF_CAIRO) {
            guint32 px = *reinterpret_cast<guint32 const *>(pix_ptr);
            alpha = (px & 0xff000000) >> 24;
        } else if (_pixbuf->pixelFormat() == Inkscape::Pixbuf::PF_GDK) {
            alpha = pix_ptr[3];
        } else {
            throw std::runtime_error("Unrecognized pixel format");
        }
        float alpha_f = (alpha / 255.0f) *
            (suppressesOwnEffect(Bitmap::SuppressedOwnEffects::Opacity) ? 1.0f : _opacity);
        return alpha_f > 0.01 ? this : nullptr;
    }
}

} // namespace Inkscape

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
