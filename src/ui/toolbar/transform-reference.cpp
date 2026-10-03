// SPDX-License-Identifier: GPL-2.0-or-later

#include "transform-reference.h"

namespace Inkscape::UI::Toolbar {

TransformReferencePoint transform_reference_from_index(int index)
{
    if (index < 0 || index > 8) {
        return TransformReferencePoint::TopLeft;
    }
    return static_cast<TransformReferencePoint>(index);
}

int transform_reference_index(TransformReferencePoint point)
{
    auto const index = static_cast<int>(point);
    return index >= 0 && index <= 8 ? index : 0;
}

Geom::Point normalized_reference(TransformReferencePoint point)
{
    auto const index = transform_reference_index(point);
    return Geom::Point{(index % 3) * 0.5, (index / 3) * 0.5};
}

Geom::Point reference_position(Geom::Rect const &bounds, TransformReferencePoint point)
{
    auto const reference = normalized_reference(point);
    return Geom::Point{bounds.left() + bounds.width() * reference.x(),
                       bounds.top() + bounds.height() * reference.y()};
}

ReferenceBounds bounds_from_reference(Geom::Point const &reference,
                                      double width,
                                      double height,
                                      TransformReferencePoint point)
{
    auto const normalized = normalized_reference(point);
    auto const x0 = reference.x() - normalized.x() * width;
    auto const y0 = reference.y() - normalized.y() * height;
    return {x0, y0, x0 + width, y0 + height};
}

ReferenceBounds resize_around_reference(Geom::Rect const &bounds,
                                        double width,
                                        double height,
                                        TransformReferencePoint point)
{
    return bounds_from_reference(reference_position(bounds, point), width, height, point);
}

} // namespace Inkscape::UI::Toolbar
