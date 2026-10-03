// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_SELECTION_RESIZE_FEEDBACK_H
#define INKSCAPE_SELECTION_RESIZE_FEEDBACK_H

#include <cstddef>
#include <cstdint>
#include <locale>
#include <memory>

#include <2geom/affine.h>
#include <2geom/rect.h>
#include <glibmm/ustring.h>

class SPDesktop;

namespace Inkscape {

namespace Util {
class Unit;
}

struct SelectionDimensionLabels {
    Glib::ustring width;
    Glib::ustring height;
};

// Shared presentation primitives; resize behavior remains unchanged.
uint32_t selection_feedback_readable_foreground(uint32_t preferred, uint32_t background) noexcept;
double selection_feedback_contrast_ratio(uint32_t foreground, uint32_t background) noexcept;
Geom::Rect clamp_selection_feedback_label(Geom::Rect label, Geom::Rect const &viewport);

/** Axis-aligned bounds of all four transformed corners. */
Geom::Rect transformed_selection_bounds(Geom::Rect const &bounds, Geom::Affine const &affine);

/** Format a document-coordinate length in a supplied display unit. */
Glib::ustring format_selection_dimension(double length_px, Util::Unit const &unit);
Glib::ustring format_selection_dimension(double length_px, Util::Unit const &unit,
                                         std::locale const &locale);

SelectionDimensionLabels format_selection_dimensions(Geom::Rect const &bounds_px,
                                                      Util::Unit const &unit);
SelectionDimensionLabels format_selection_dimensions(Geom::Rect const &bounds_px,
                                                      Util::Unit const &unit,
                                                      std::locale const &locale);

/**
 * Two non-pickable, view-local labels used by the Selector during a resize.
 * The supplied unit is frozen for the lifetime of the overlay.
 */
class SelectionDimensionOverlay final
{
public:
    SelectionDimensionOverlay(SPDesktop &desktop, Util::Unit const &unit,
                              bool width_active, bool height_active);
    ~SelectionDimensionOverlay();

    SelectionDimensionOverlay(SelectionDimensionOverlay const &) = delete;
    SelectionDimensionOverlay &operator=(SelectionDimensionOverlay const &) = delete;

    void update(Geom::Rect const &bounds, bool width_accent = false,
                bool height_accent = false);

    SelectionDimensionLabels const &labels() const;
    Geom::Rect const &bounds() const;
    std::size_t canvas_item_count() const noexcept;
    bool width_visible() const noexcept;
    bool height_visible() const noexcept;
    bool width_accented() const noexcept;
    bool height_accented() const noexcept;
    double width_contrast_ratio() const noexcept;
    double height_contrast_ratio() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Inkscape

#endif // INKSCAPE_SELECTION_RESIZE_FEEDBACK_H
