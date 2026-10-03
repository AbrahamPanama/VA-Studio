// SPDX-License-Identifier: GPL-2.0-or-later

#include "selection-resize-feedback.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#include <gdkmm/rgba.h>
#include <sigc++/scoped_connection.h>

#include "desktop.h"
#include "display/control/canvas-item-ptr.h"
#include "display/control/canvas-item-text.h"
#include "ui/widget/canvas.h"
#include "util/units.h"

namespace Inkscape {
namespace {

constexpr double LABEL_GAP = 8.0;
constexpr double VIEWPORT_MARGIN = 2.0;
constexpr double NARROW_VIEWPORT_WIDTH = 300.0;
constexpr double NARROW_VIEWPORT_HEIGHT = 180.0;
constexpr double MINIMUM_TEXT_CONTRAST = 4.5;

std::locale current_numeric_locale()
{
    try {
        return std::locale("");
    } catch (std::runtime_error const &) {
        return std::locale::classic();
    }
}

int dimension_precision(Util::Unit const &unit)
{
    // The Selector HUD intentionally has a compact, stable V1 precision policy. The global
    // unit parser currently leaves several built-in units at its three-digit fallback, even
    // where units.xml requests two digits, so relying on defaultDigits() would make common
    // millimetre and inch dimensions noisier than the Select toolbar/reference design.
    if (unit.abbr == "m" || unit.abbr == "ft" || unit.abbr == "pc") {
        return 3;
    }
    if (unit.abbr == "px" || unit.abbr == "mm" || unit.abbr == "cm" ||
        unit.abbr == "in" || unit.abbr == "pt") {
        return 2;
    }
    return std::max(0, unit.defaultDigits());
}

uint32_t rgba32(Gdk::RGBA const &color, double alpha)
{
    auto channel = [] (double value) {
        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    };
    return channel(color.get_red()) << 24 |
           channel(color.get_green()) << 16 |
           channel(color.get_blue()) << 8 |
           channel(alpha);
}

double linear_color_channel(uint32_t color, unsigned shift) noexcept
{
    auto const encoded = static_cast<double>((color >> shift) & 0xff) / 255.0;
    return encoded <= 0.04045 ? encoded / 12.92
                              : std::pow((encoded + 0.055) / 1.055, 2.4);
}

double relative_luminance(uint32_t color) noexcept
{
    return 0.2126 * linear_color_channel(color, 24) +
           0.7152 * linear_color_channel(color, 16) +
           0.0722 * linear_color_channel(color, 8);
}

double contrast_ratio(uint32_t foreground, uint32_t background) noexcept
{
    auto const foreground_luminance = relative_luminance(foreground);
    auto const background_luminance = relative_luminance(background);
    auto const lighter = std::max(foreground_luminance, background_luminance);
    auto const darker = std::min(foreground_luminance, background_luminance);
    return (lighter + 0.05) / (darker + 0.05);
}

uint32_t ensure_readable_foreground(uint32_t preferred, uint32_t background) noexcept
{
    if (contrast_ratio(preferred, background) >= MINIMUM_TEXT_CONTRAST) {
        return preferred;
    }

    constexpr uint32_t black = 0x000000ff;
    constexpr uint32_t white = 0xffffffff;
    return contrast_ratio(black, background) >= contrast_ratio(white, background)
               ? black
               : white;
}

Geom::Rect clamp_label(Geom::Rect label, Geom::Rect const &viewport)
{
    auto delta = Geom::Point();
    if (label.left() < viewport.left() + VIEWPORT_MARGIN) {
        delta[Geom::X] = viewport.left() + VIEWPORT_MARGIN - label.left();
    } else if (label.right() > viewport.right() - VIEWPORT_MARGIN) {
        delta[Geom::X] = viewport.right() - VIEWPORT_MARGIN - label.right();
    }
    if (label.top() < viewport.top() + VIEWPORT_MARGIN) {
        delta[Geom::Y] = viewport.top() + VIEWPORT_MARGIN - label.top();
    } else if (label.bottom() > viewport.bottom() - VIEWPORT_MARGIN) {
        delta[Geom::Y] = viewport.bottom() - VIEWPORT_MARGIN - label.bottom();
    }
    return Geom::Rect(label.min() + delta, label.max() + delta);
}

} // namespace

uint32_t selection_feedback_readable_foreground(uint32_t preferred, uint32_t background) noexcept
{
    return ensure_readable_foreground(preferred, background);
}

double selection_feedback_contrast_ratio(uint32_t foreground, uint32_t background) noexcept
{
    return contrast_ratio(foreground, background);
}

Geom::Rect clamp_selection_feedback_label(Geom::Rect label, Geom::Rect const &viewport)
{
    return clamp_label(label, viewport);
}

Geom::Rect transformed_selection_bounds(Geom::Rect const &bounds, Geom::Affine const &affine)
{
    auto first = bounds.corner(0) * affine;
    Geom::Rect result(first, first);
    for (unsigned i = 1; i < 4; ++i) {
        result.expandTo(bounds.corner(i) * affine);
    }
    return result;
}

Glib::ustring format_selection_dimension(double length_px, Util::Unit const &unit,
                                         std::locale const &locale)
{
    double value = Util::Quantity::convert(std::abs(length_px), "px", &unit);
    if (!std::isfinite(value)) {
        return Glib::ustring{"\u2014 "} + unit.abbr;
    }

    int const precision = dimension_precision(unit);
    double const rounds_to_zero = 0.5 * std::pow(10.0, -precision);
    if (std::abs(value) < rounds_to_zero) {
        value = 0.0;
    }

    std::ostringstream stream;
    stream.imbue(locale);
    stream << std::fixed << std::setprecision(precision) << value;
    return Glib::ustring{stream.str()} + " " + unit.abbr;
}

Glib::ustring format_selection_dimension(double length_px, Util::Unit const &unit)
{
    return format_selection_dimension(length_px, unit, current_numeric_locale());
}

SelectionDimensionLabels format_selection_dimensions(Geom::Rect const &bounds_px,
                                                      Util::Unit const &unit,
                                                      std::locale const &locale)
{
    return {
        format_selection_dimension(bounds_px.width(), unit, locale),
        format_selection_dimension(bounds_px.height(), unit, locale)
    };
}

SelectionDimensionLabels format_selection_dimensions(Geom::Rect const &bounds_px,
                                                      Util::Unit const &unit)
{
    return format_selection_dimensions(bounds_px, unit, current_numeric_locale());
}

struct SelectionDimensionOverlay::Impl {
    Impl(SPDesktop &desktop, Util::Unit const &unit, bool width_active, bool height_active)
        : desktop(desktop)
        , unit(unit)
        , width_active(width_active)
        , height_active(height_active)
        , width(make_canvasitem<CanvasItemText>(desktop.getCanvasTemp(), Geom::Point(), ""))
        , height(make_canvasitem<CanvasItemText>(desktop.getCanvasTemp(), Geom::Point(), ""))
    {
        auto canvas = desktop.getCanvas();
        Gdk::RGBA foreground{"#ffffff"};
        Gdk::RGBA background{"#202124"};
        Gdk::RGBA selected_background{"#1c58c6"};
        Gdk::RGBA selected_foreground{"#ffffff"};
        auto style = canvas->get_style_context();
        style->lookup_color("theme_fg_color", foreground);
        style->lookup_color("theme_bg_color", background);
        style->lookup_color("theme_selected_bg_color", selected_background);
        style->lookup_color("theme_selected_fg_color", selected_foreground);

        foreground_color = rgba32(foreground, 1.0);
        inactive_color = rgba32(foreground, 0.68);
        background_color = rgba32(background, 0.88);
        accent_background_color = rgba32(selected_background, 1.0);
        accent_foreground_color = ensure_readable_foreground(
            rgba32(selected_foreground, 1.0), accent_background_color);

        for (auto *label : {width.get(), height.get()}) {
            label->set_fontsize(11.0);
            label->set_border(4.0);
            label->set_bg_radius(0.35);
            label->set_background(background_color);
            label->set_pickable(false);
            label->set_visible(false);
            label->raise_to_top();
        }
        width->set_name("selector-resize-width");
        height->set_name("selector-resize-height");

        zoom_connection = desktop.signal_zoom_changed.connect([this] (double) {
            if (has_bounds) {
                place();
            }
        });
    }

    ~Impl()
    {
        zoom_connection.disconnect();
        width.reset();
        height.reset();
    }

    void update(Geom::Rect const &new_bounds, bool width_accent, bool height_accent)
    {
        bounds = new_bounds;
        has_bounds = true;
        width_is_accented = width_accent;
        height_is_accented = height_accent;
        labels = format_selection_dimensions(bounds, unit);
        width->set_text(labels.width);
        height->set_text(labels.height);
        apply_colors(*width, width_accent, width_active,
                     width_foreground_color, width_background_color);
        apply_colors(*height, height_accent, height_active,
                     height_foreground_color, height_background_color);
        place();
    }

    void apply_colors(CanvasItemText &label, bool accented, bool active,
                      uint32_t &current_foreground, uint32_t &current_background)
    {
        current_foreground = accented ? accent_foreground_color
                                      : active ? foreground_color : inactive_color;
        current_background = accented ? accent_background_color : background_color;
        label.set_fill(current_foreground);
        label.set_background(current_background);
    }

    void place()
    {
        auto canvas = desktop.getCanvas();
        auto const area = canvas->get_area_world();
        if (area.width() <= 0 || area.height() <= 0) {
            // Unit tests can own an unrealized desktop. Keep both labels valid but hidden.
            width_is_visible = false;
            height_is_visible = false;
            width->set_visible(false);
            height->set_visible(false);
            return;
        }

        auto const viewport = Geom::Rect(area);
        auto const world_bounds = transformed_selection_bounds(bounds, desktop.d2w());
        bool const narrow = viewport.width() < NARROW_VIEWPORT_WIDTH ||
                            viewport.height() < NARROW_VIEWPORT_HEIGHT;

        auto const width_size = width->get_text_size().dimensions();
        double width_y = world_bounds.top() - LABEL_GAP - width_size[Geom::Y];
        if (width_y < viewport.top() + VIEWPORT_MARGIN) {
            if (world_bounds.bottom() + LABEL_GAP + width_size[Geom::Y] <= viewport.bottom()) {
                width_y = world_bounds.bottom() + LABEL_GAP;
            } else {
                width_y = world_bounds.top() + LABEL_GAP;
            }
        }
        auto width_box = Geom::Rect::from_xywh(world_bounds.midpoint()[Geom::X] - width_size[Geom::X] / 2.0,
                                               width_y, width_size[Geom::X], width_size[Geom::Y]);
        width_box = clamp_label(width_box, viewport);

        auto const height_size = height->get_text_size().dimensions();
        double height_x = world_bounds.left() - LABEL_GAP - height_size[Geom::X];
        if (height_x < viewport.left() + VIEWPORT_MARGIN) {
            if (world_bounds.right() + LABEL_GAP + height_size[Geom::X] <= viewport.right()) {
                height_x = world_bounds.right() + LABEL_GAP;
            } else {
                height_x = world_bounds.left() + LABEL_GAP;
            }
        }
        auto height_box = Geom::Rect::from_xywh(height_x,
                                                world_bounds.midpoint()[Geom::Y] - height_size[Geom::Y] / 2.0,
                                                height_size[Geom::X], height_size[Geom::Y]);
        height_box = clamp_label(height_box, viewport);

        width->set_anchor({0, 0});
        height->set_anchor({0, 0});
        width->set_adjust({0, 0});
        height->set_adjust({0, 0});
        width->set_coord(desktop.w2d(width_box.min()));
        height->set_coord(desktop.w2d(height_box.min()));

        width_is_visible = width_active || !narrow;
        height_is_visible = height_active || !narrow;
        width->set_visible(width_is_visible);
        height->set_visible(height_is_visible);
    }

    SPDesktop &desktop;
    Util::Unit const &unit;
    bool width_active = false;
    bool height_active = false;
    CanvasItemPtr<CanvasItemText> width;
    CanvasItemPtr<CanvasItemText> height;
    SelectionDimensionLabels labels;
    Geom::Rect bounds;
    bool has_bounds = false;
    bool width_is_visible = false;
    bool height_is_visible = false;
    bool width_is_accented = false;
    bool height_is_accented = false;
    uint32_t foreground_color = 0xffffffff;
    uint32_t inactive_color = 0xffffffad;
    uint32_t background_color = 0x202124e0;
    uint32_t accent_foreground_color = 0xffffffff;
    uint32_t accent_background_color = 0x1c58c6ff;
    uint32_t width_foreground_color = 0xffffffff;
    uint32_t width_background_color = 0x202124e0;
    uint32_t height_foreground_color = 0xffffffff;
    uint32_t height_background_color = 0x202124e0;
    sigc::scoped_connection zoom_connection;
};

SelectionDimensionOverlay::SelectionDimensionOverlay(SPDesktop &desktop, Util::Unit const &unit,
                                                     bool width_active, bool height_active)
    : _impl(std::make_unique<Impl>(desktop, unit, width_active, height_active))
{}

SelectionDimensionOverlay::~SelectionDimensionOverlay() = default;

void SelectionDimensionOverlay::update(Geom::Rect const &bounds, bool width_accent,
                                       bool height_accent)
{
    _impl->update(bounds, width_accent, height_accent);
}

SelectionDimensionLabels const &SelectionDimensionOverlay::labels() const
{
    return _impl->labels;
}

Geom::Rect const &SelectionDimensionOverlay::bounds() const
{
    return _impl->bounds;
}

std::size_t SelectionDimensionOverlay::canvas_item_count() const noexcept
{
    return 2;
}

bool SelectionDimensionOverlay::width_visible() const noexcept
{
    return _impl->width_is_visible;
}

bool SelectionDimensionOverlay::height_visible() const noexcept
{
    return _impl->height_is_visible;
}

bool SelectionDimensionOverlay::width_accented() const noexcept
{
    return _impl->width_is_accented;
}

bool SelectionDimensionOverlay::height_accented() const noexcept
{
    return _impl->height_is_accented;
}

double SelectionDimensionOverlay::width_contrast_ratio() const noexcept
{
    return contrast_ratio(_impl->width_foreground_color, _impl->width_background_color);
}

double SelectionDimensionOverlay::height_contrast_ratio() const noexcept
{
    return contrast_ratio(_impl->height_foreground_color, _impl->height_background_color);
}

} // namespace Inkscape
