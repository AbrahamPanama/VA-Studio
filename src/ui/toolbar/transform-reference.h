// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef INKSCAPE_UI_TOOLBAR_TRANSFORM_REFERENCE_H
#define INKSCAPE_UI_TOOLBAR_TRANSFORM_REFERENCE_H

#include <cstdint>

#include <2geom/point.h>
#include <2geom/rect.h>

namespace Inkscape::UI::Toolbar {

enum class TransformReferencePoint : std::uint8_t {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight
};

struct ReferenceBounds {
    double x0;
    double y0;
    double x1;
    double y1;
};

TransformReferencePoint transform_reference_from_index(int index);
int transform_reference_index(TransformReferencePoint point);
Geom::Point normalized_reference(TransformReferencePoint point);
Geom::Point reference_position(Geom::Rect const &bounds, TransformReferencePoint point);
ReferenceBounds bounds_from_reference(Geom::Point const &reference,
                                      double width,
                                      double height,
                                      TransformReferencePoint point);
ReferenceBounds resize_around_reference(Geom::Rect const &bounds,
                                        double width,
                                        double height,
                                        TransformReferencePoint point);

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_TRANSFORM_REFERENCE_H

