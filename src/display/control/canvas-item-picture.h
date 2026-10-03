// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_CANVAS_ITEM_PICTURE_H
#define SEEN_CANVAS_ITEM_PICTURE_H

/**
 * A pre-rendered picture of part of the drawing, drawn through a transform.
 * The Selector moves one while dragging objects, so a move repaints a
 * picture instead of re-rendering the objects on every pointer motion.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cairo.h>
#include <2geom/affine.h>

#include "canvas-item.h"

namespace Inkscape {

class CanvasItemPicture final : public CanvasItem
{
public:
    /// \a surface (one reference is taken) maps to desktop coordinates through
    /// \a surface_to_desktop, in surface pixels.
    CanvasItemPicture(CanvasItemGroup *group, cairo_surface_t *surface, Geom::Affine const &surface_to_desktop);

    /// Extra transform in desktop coordinates, applied after surface_to_desktop.
    void set_transform(Geom::Affine const &transform);

    bool contains(Geom::Point const &, double = 0) override { return false; }

    cairo_surface_t *surface() const { return _surface; }
    Geom::Affine const &surfaceToDesktop() const { return _surface_to_desktop; }

protected:
    ~CanvasItemPicture() override;

    void _update(bool propagate) override;
    void _render(CanvasItemBuffer &buf) const override;

private:
    Geom::Affine _surfaceToCanvas() const;

    cairo_surface_t *_surface = nullptr;
    Geom::Affine _surface_to_desktop;
    Geom::Affine _transform;
};

} // namespace Inkscape

#endif // SEEN_CANVAS_ITEM_PICTURE_H
