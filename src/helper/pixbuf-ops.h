// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_HELPER_PIXBUF_OPS_H
#define INKSCAPE_HELPER_PIXBUF_OPS_H

/*
 * Helpers for SPItem -> gdk_pixbuf related stuff
 *
 * Authors:
 *   John Cliff <simarilius@yahoo.com>
 *
 * Copyright (C) 2008 John Cliff
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <memory>
#include <array>
#include <optional>
#include <vector>
#include <cstdint>
#include <2geom/rect.h>
#include <2geom/forward.h>
#include "display/drawing-item.h"
#include "util/bitmap-island-budget.h"

class SPDocument;
class SPItem;
typedef struct _cairo_surface cairo_surface_t;
namespace Inkscape { class Pixbuf; class Drawing; }

namespace Inkscape::Bitmap {

/**
 * An offscreen arena that can only show COMMITTED document state.
 *
 * It owns a private Drawing and a freshly allocated display key that no canvas, panel or
 * preview ever sees, so a view-only pixbuf or eraser preview installed on a view key
 * (SPImage::setViewPixbuf) cannot reach it. The key is never caller-supplied. The destructor
 * hides the items on every exit path, including exceptions.
 */
class CommittedArena {
public:
    /// Shows the document root. Throws (after cleaning up) if showing fails.
    explicit CommittedArena(SPDocument *document);
    ~CommittedArena();
    CommittedArena(CommittedArena const &) = delete;
    CommittedArena &operator=(CommittedArena const &) = delete;

    Drawing &drawing() { return *_drawing; }
    unsigned key() const { return _key; }
    /**
     * Hide the document views now (also done by the destructor). Retries once. If hiding still
     * fails, the Drawing is deliberately leaked so views left in the document never point to
     * deleted DrawingItems; returns false so the caller reports the failure.
     */
    bool hide() noexcept;
    bool hidden() const { return _hidden; }
    /// Test seam: called at the start of every hide attempt (may throw to model a failure).
    void setHideProbe(void (*probe)()) { _hide_probe = probe; }
    /// Hide everything but @a items (needed so referenced defs stay available).
    void hideExcept(std::vector<SPItem const *> const &items);

private:
    SPDocument *_document;
    std::unique_ptr<Drawing> _drawing;
    unsigned _key = 0;
    bool _hidden = false;
    void (*_hide_probe)() = nullptr;
};

/**
 * Upper bound, in pixels, for one pattern tile surface while an output render is active on this
 * thread (0 = no scope; DrawingPattern then applies its fixed ceiling). Output renders derive it
 * from the pixel buffer they reserved, so pattern tiles cannot exceed the admitted scratch.
 */
class TileLimitScope {
public:
    explicit TileLimitScope(std::uint64_t pixels);
    ~TileLimitScope();
    TileLimitScope(TileLimitScope const &) = delete;
    TileLimitScope &operator=(TileLimitScope const &) = delete;
    static std::uint64_t current();
private:
    std::uint64_t _previous;
};

struct RenderRequest {
    SPDocument *document = nullptr;
    Geom::Rect area;                  ///< Document units.
    double dpi = 96.0;
    /// Opt-in: map the exact area to the full ceil-sized grid, without padding.
    /// Default callers retain uniform DPI scaling and fractional edge padding.
    bool fit_to_pixel_grid = false;
    std::optional<std::array<unsigned, 2>> exact_size; // Request-only exact sampling, same document rectangle.
    std::vector<SPItem const *> items; ///< Empty renders everything.
    bool set_opaque = false;
    uint32_t const *checkerboard_color = nullptr;
    double device_scale = 1.0;
    std::optional<Inkscape::Antialiasing> antialias;
    /// Optional surface allocator (test seam); nullptr uses cairo_image_surface_create(ARGB32).
    cairo_surface_t *(*create_surface)(int width, int height) = nullptr;
    /// Test seam forwarded to CommittedArena::setHideProbe.
    void (*hide_probe)() = nullptr;
};

struct RenderOutcome {
    RenderOutcome();
    ~RenderOutcome();
    RenderOutcome(RenderOutcome &&) noexcept;
    RenderOutcome &operator=(RenderOutcome &&) noexcept;

    Outcome outcome;                    ///< Never changed/unchanged-with-pixbuf unless a pixbuf exists.
    /// Declared BEFORE pixbuf: members die in reverse order, so the pixels are freed first and the
    /// reservation (pixels + intermediate scratch ceiling) is released only afterwards.
    Budget::Token token;
    std::unique_ptr<Inkscape::Pixbuf> pixbuf;
    unsigned output_key = 0;            ///< The private committed display key used (0 if none).
    bool ok() const noexcept { return outcome.ok() && pixbuf; }
};

/**
 * Render committed document state to a bitmap. Never returns a blank or partial bitmap as
 * success: any NULL/errored surface, budget refusal or exception yields a failed outcome with
 * no pixbuf, and the arena items are hidden and the surface released.
 */
RenderOutcome renderCommitted(RenderRequest const &request, Budget &budget);

} // namespace Inkscape::Bitmap

Inkscape::Pixbuf *sp_generate_internal_bitmap(SPDocument *document,
                                              Geom::Rect const &area,
                                              double dpi,
                                              std::vector<SPItem const *> items = {},
                                              bool set_opaque = false,
                                              uint32_t const *checkerboard_color = nullptr,
                                              double device_scale = 1.0,
                                              std::optional<Inkscape::Antialiasing> antialias = {});
#endif // INKSCAPE_HELPER_PIXBUF_OPS_H
