// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A pre-rendered picture of part of the drawing, drawn through a transform.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "canvas-item-picture.h"

#include <cmath>

#include <2geom/rect.h>
#include <2geom/transforms.h>

#include "canvas-item-buffer.h"
#include "helper/geom.h"

namespace Inkscape {

CanvasItemPicture::CanvasItemPicture(CanvasItemGroup *group, cairo_surface_t *surface,
                                     Geom::Affine const &surface_to_desktop)
    : CanvasItem(group)
    , _surface(cairo_surface_reference(surface))
    , _surface_to_desktop(surface_to_desktop)
{
    _name = "CanvasItemPicture";
    _pickable = false;
    request_update();
}

CanvasItemPicture::~CanvasItemPicture()
{
    cairo_surface_destroy(_surface);
}

void CanvasItemPicture::set_transform(Geom::Affine const &transform)
{
    defer([=, this] {
        if (_transform == transform) return;
        _transform = transform;
        request_update();
    });
}

Geom::Affine CanvasItemPicture::_surfaceToCanvas() const
{
    return _surface_to_desktop * _transform * affine();
}

void CanvasItemPicture::_update(bool)
{
    request_redraw(); // the old place
    auto const width = cairo_image_surface_get_width(_surface);
    auto const height = cairo_image_surface_get_height(_surface);
    if (width <= 0 || height <= 0) {
        _bounds = {};
        return;
    }
    _bounds = expandedBy(Geom::Rect(0, 0, width, height) * _surfaceToCanvas(), 1);
    request_redraw(); // the new place
}

void CanvasItemPicture::_render(CanvasItemBuffer &buf) const
{
    auto const m = _surfaceToCanvas() * Geom::Translate(-buf.rect.min());
    auto *cr = buf.cr->cobj();
    cairo_save(cr);
    cairo_matrix_t matrix{m[0], m[1], m[2], m[3], m[4], m[5]};
    cairo_transform(cr, &matrix);
    cairo_set_source_surface(cr, _surface, 0, 0);
    // Pixel for pixel (same zoom, whole-pixel offset) stays sharp; anything
    // else is resampled.
    auto const whole = [&](double logical) {
        double const device = logical * buf.device_scale;
        return std::abs(device - std::round(device)) < 1e-3;
    };
    bool const pixel_for_pixel = Geom::are_near(m[0], 1.0 / buf.device_scale, 1e-6) &&
                                 Geom::are_near(m[3], 1.0 / buf.device_scale, 1e-6) &&
                                 Geom::are_near(m[1], 0.0, 1e-9) && Geom::are_near(m[2], 0.0, 1e-9) &&
                                 whole(m[4]) && whole(m[5]);
    cairo_pattern_set_filter(cairo_get_source(cr), pixel_for_pixel ? CAIRO_FILTER_FAST : CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
}

} // namespace Inkscape
