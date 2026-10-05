// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Helpers for SPItem -> gdk_pixbuf related stuff
 *
 * Authors:
 *   John Cliff <simarilius@yahoo.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2008 John Cliff
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <algorithm>
#include <stdexcept>
#include <2geom/transforms.h>

#include "document.h"

#include "display/cairo-utils.h"
#include "display/drawing-context.h"
#include "display/drawing.h"
#include "helper/pixbuf-ops.h"
#include "object/sp-root.h"
#include "util/scope_exit.h"
#include "object/sp-item.h"
#include "util/units.h"

namespace Inkscape::Bitmap {

CommittedArena::CommittedArena(SPDocument *document)
    : _document(document)
    , _drawing(std::make_unique<Drawing>())
    , _key(SPItem::display_key_new(1))
{
    try {
        _drawing->setRoot(_document->getRoot()->invoke_show(*_drawing, _key, SP_ITEM_SHOW_DISPLAY));
    } catch (...) {
        // A partially shown tree must not stay attached to the document.
        try { _document->getRoot()->invoke_hide(_key); } catch (...) {}
        throw;
    }
}

CommittedArena::~CommittedArena()
{
    if (!_hidden && !hide()) g_warning("CommittedArena: invoke_hide failed; Drawing intentionally leaked");
}

bool CommittedArena::hide() noexcept
{
    if (_hidden) return true;
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            if (_hide_probe) _hide_probe();
            _document->getRoot()->invoke_hide(_key);
            _hidden = true;
            return true;
        } catch (...) {
        }
    }
    // Views may still point into the Drawing: never destroy it.
    (void)_drawing.release();
    _hidden = true;
    return false;
}

void CommittedArena::hideExcept(std::vector<SPItem const *> const &items)
{
    _document->getRoot()->invoke_hide_except(_key, items);
}

namespace { thread_local std::uint64_t tile_limit_pixels = 0; }
TileLimitScope::TileLimitScope(std::uint64_t pixels) : _previous(tile_limit_pixels) { tile_limit_pixels = pixels; }
TileLimitScope::~TileLimitScope() { tile_limit_pixels = _previous; }
std::uint64_t TileLimitScope::current() { return tile_limit_pixels; }

RenderOutcome::RenderOutcome() : outcome{Status::failed, "Not rendered"} {}
RenderOutcome::~RenderOutcome() = default;
RenderOutcome::RenderOutcome(RenderOutcome &&) noexcept = default;
RenderOutcome &RenderOutcome::operator=(RenderOutcome &&) noexcept = default;

namespace {

RenderOutcome failure(Outcome o)
{
    RenderOutcome r;
    r.outcome = o;
    if (r.outcome.ok()) r.outcome = {Status::failed, "Render produced no bitmap"};
    return r;
}

/// Intermediate (pattern tile / filter) scratch admitted on top of the final bitmap: twice the
/// bitmap, at least 32 MiB. Drawing nodes themselves are not metered (documented gap).
std::uint64_t scratchCeiling(std::uint64_t bytes) { return std::max<std::uint64_t>(2 * bytes, 32 * MiB); }

/// legacy: exceptions propagate to the caller exactly as before this package (callers such as the
/// PDF/print rasteriser report them); otherwise every exception becomes a failed outcome.
RenderOutcome render(RenderRequest const &req, Budget *budget, bool legacy)
{
    if (!req.document || !req.document->getRoot()) return failure({Status::failed, "No document"});
    if (req.area.hasZeroArea()) return failure({Status::failed, "Render area is empty"});

    double const scale_factor = Util::Quantity::convert(req.dpi, "px", "in");
    double const w = req.exact_size ? (*req.exact_size)[0] : std::ceil(scale_factor * req.area.width());
    double const h = req.exact_size ? (*req.exact_size)[1] : std::ceil(scale_factor * req.area.height());
    // Cairo cannot create image surfaces beyond 32767 px per side.
    if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > 32767 || h > 32767)
        return failure({Status::failed, "Bitmap size is outside the supported range"});
    int const width = static_cast<int>(w), height = static_cast<int>(h);

    int const stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    std::uint64_t bytes = 0, reserve = 0;
    if (stride <= 0 || !checkedMul(static_cast<std::uint64_t>(stride), static_cast<std::uint64_t>(height), bytes) ||
        !checkedAdd(bytes, scratchCeiling(bytes), reserve))
        return failure({Status::failed, "Bitmap size overflows"});

    RenderOutcome result;
    if (budget) {
        auto const reserved = budget->acquire(Stage::composition, reserve, result.token);
        if (!reserved.ok()) return failure(reserved);
    }
    TileLimitScope tile_limit(scratchCeiling(bytes) / 4);

    cairo_surface_t *surface = nullptr;
    // Destroy the pixels first, then release the reservation (ledger contract).
    auto drop = [&] {
        if (surface) cairo_surface_destroy(surface);
        surface = nullptr;
        result.pixbuf.reset();
        result.token.release();
    };
    auto fail = [&](char const *why) {
        drop();
        result.outcome = {Status::failed, why};
        return std::move(result);
    };
    auto guard = scope_exit([&] { if (surface) cairo_surface_destroy(surface); });
    try {
        req.document->ensureUpToDate();
        CommittedArena arena(req.document);
        arena.setHideProbe(req.hide_probe);
        auto &drawing = arena.drawing();
        result.output_key = arena.key();

        // Exact-extent publication reverses these per-axis scales. Filling the
        // whole grid avoids squeezing fractional edge padding into the artwork.
        double const scale_x = (req.exact_size || req.fit_to_pixel_grid) ? width / req.area.width() : scale_factor;
        double const scale_y = (req.exact_size || req.fit_to_pixel_grid) ? height / req.area.height() : scale_factor;
        Geom::Affine const affine = Geom::Translate(-req.area.min()) * Geom::Scale(scale_x, scale_y);
        drawing.root()->setTransform(affine);
        drawing.setExact(); // Maximum quality for blurs.
        drawing.setAntialiasingOverride(req.antialias);

        // Hide all items we don't want, instead of showing only requested items,
        // because that would not work if the shown item references something in defs.
        if (!req.items.empty()) arena.hideExcept(req.items);

        auto const final_area = Geom::IntRect::from_xywh(0, 0, width, height);
        drawing.update(final_area);

        if (req.set_opaque) {
            // Required by sp_asbitmap_render().
            for (auto item : req.items) {
                if (auto ai = item->get_arenaitem(arena.key())) ai->setOpacity(1.0);
            }
        }

        surface = req.create_surface ? req.create_surface(width, height)
                                     : cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            g_warning("sp_generate_internal_bitmap: not enough memory to create pixel buffer. Need %llu.",
                      static_cast<unsigned long long>(bytes));
            if (arena.hide()) return fail("Cannot create the pixel buffer");
            return fail("Cannot create the pixel buffer and cleanup failed");
        }

        cairo_status_t context_status = CAIRO_STATUS_SUCCESS;
        {
            DrawingContext dc(surface, Geom::Point(0, 0));
            if (req.checkerboard_color) {
                auto pattern = ink_cairo_pattern_create_checkerboard(*req.checkerboard_color);
                dc.save();
                dc.transform(Geom::Scale(req.device_scale));
                dc.setOperator(CAIRO_OPERATOR_SOURCE);
                dc.setSource(pattern->cobj());
                dc.paint();
                dc.restore();
            }
            drawing.render(dc, final_area, DrawingItem::RENDER_BYPASS_CACHE);
            context_status = cairo_status(dc.raw()); // the surface status alone does not show context errors
        }
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS || context_status != CAIRO_STATUS_SUCCESS) {
            (void)arena.hide();
            return fail("Cairo reported an error while rendering");
        }
        if (req.device_scale != 1.0) cairo_surface_set_device_scale(surface, req.device_scale, req.device_scale);

        // A failed cleanup means document views may be stale: report instead of returning pixels.
        if (!arena.hide()) return fail("Cleanup of the offscreen arena failed");

        result.pixbuf.reset(new Pixbuf(surface));
        surface = nullptr; // owned by the Pixbuf
        result.outcome = {Status::changed, ""};
        return result;
    } catch (std::bad_alloc const &) {
        if (legacy) throw;
        return fail("Out of memory while rendering");
    } catch (std::length_error const &) {
        if (legacy) throw;
        return fail("Intermediate render memory limit exceeded");
    } catch (std::exception const &e) {
        if (legacy) throw;
        g_warning("sp_generate_internal_bitmap: %s", e.what());
        return fail("Rendering failed");
    } catch (...) {
        if (legacy) throw;
        return fail("Rendering failed");
    }
}

} // namespace

RenderOutcome renderCommitted(RenderRequest const &request, Budget &budget)
{
    return render(request, &budget, false);
}

} // namespace Inkscape::Bitmap

/**
    Generates a bitmap from given items. The bitmap is stored in RAM and not written to file.
    Always renders committed state through a private output key (see Bitmap::CommittedArena).
    @param document Inkscape document.
    @param area     Export area in document units.
    @param dpi      Resolution.
    @param items    Vector of pointers to SPItems to export. Export all items if empty.
    @param opaque   Set items opacity to 1 (used by Cairo renderer for filtered objects rendered as bitmaps).
    @return The created GdkPixbuf structure or nullptr if rendering failed.
*/
Inkscape::Pixbuf *sp_generate_internal_bitmap(SPDocument *document,
                                              Geom::Rect const &area,
                                              double dpi,
                                              std::vector<SPItem const *> items,
                                              bool opaque,
                                              uint32_t const *checkerboard_color,
                                              double device_scale,
                                              std::optional<Inkscape::Antialiasing> antialias)
{
    Inkscape::Bitmap::RenderRequest request;
    request.document = document;
    request.area = area;
    request.dpi = dpi;
    request.items = std::move(items);
    request.set_opaque = opaque;
    request.checkerboard_color = checkerboard_color;
    request.device_scale = device_scale;
    request.antialias = antialias;
    // Legacy callers keep their unmetered behavior; failure is a nullptr, never a blank bitmap.
    auto outcome = Inkscape::Bitmap::render(request, nullptr, true);
    return outcome.ok() ? outcome.pixbuf.release() : nullptr;
}

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
