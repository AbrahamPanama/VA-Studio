// SPDX-License-Identifier: GPL-2.0-or-later

#include "selection-rotation-feedback.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <gdkmm/rgba.h>
#include <sigc++/scoped_connection.h>

#include "desktop.h"
#include "display/control/canvas-item-ptr.h"
#include "display/control/canvas-item-text.h"
#include "selection-resize-feedback.h"
#include "ui/widget/canvas.h"

namespace Inkscape {
namespace {

constexpr double radians_per_degree = std::numbers::pi / 180.0;

double principal_degrees(double radians) noexcept
{
    return std::remainder(radians, 2.0 * std::numbers::pi) / radians_per_degree;
}

uint32_t rgba32(Gdk::RGBA const &color)
{
    auto channel = [](double value) {
        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    };
    return channel(color.get_red()) << 24 | channel(color.get_green()) << 16 |
           channel(color.get_blue()) << 8 | 0xff;
}

} // namespace

double equivalent_rotation_degrees(double radians, double reference_degrees) noexcept
{
    if (!std::isfinite(radians) || !std::isfinite(reference_degrees)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return reference_degrees + std::remainder(
        principal_degrees(radians) - std::remainder(reference_degrees, 360.0), 360.0);
}

std::optional<double> ContinuousRotationAngle::update(double radians, double radius_px) noexcept
{
    if (!std::isfinite(radians) || !std::isfinite(radius_px) ||
        radius_px < ROTATION_ANGLE_MIN_RADIUS_PX) {
        // Do not infer a half/full turn from pointer noise at the pivot. On
        // re-entry choose the equivalent baseline angle nearest the last one.
        _last_degrees.reset();
        return {};
    }
    auto const wrapped = principal_degrees(radians);
    if (_last_degrees) {
        _degrees += std::remainder(wrapped - *_last_degrees, 360.0);
    } else {
        _degrees = equivalent_rotation_degrees(radians, _degrees);
    }
    _last_degrees = wrapped;
    return _degrees;
}

void ContinuousRotationAngle::reset() noexcept
{
    _degrees = 0.0;
    _last_degrees = 0.0;
}

RotationDetentResult evaluate_rotation_detent(double degrees, double radius_px,
                                             bool enabled, bool higher_priority_snap,
                                             RotationDetentLatch &latch) noexcept
{
    if (!enabled || higher_priority_snap || !std::isfinite(degrees) ||
        !std::isfinite(radius_px) || radius_px < ROTATION_DETENT_MIN_RADIUS_PX) {
        latch.reset();
        return {degrees, false};
    }

    auto within = [&](double candidate, double pixels, double angular_limit) {
        auto const delta = std::abs(degrees - candidate);
        return delta <= angular_limit && delta * radians_per_degree * radius_px <= pixels;
    };
    if (latch.degrees) {
        if (within(*latch.degrees, ROTATION_DETENT_RELEASE_PX, ROTATION_DETENT_RELEASE_DEGREES)) {
            return {*latch.degrees, true};
        }
        latch.reset();
    }

    auto const candidate = std::round(degrees / ROTATION_DETENT_STEP_DEGREES) *
                           ROTATION_DETENT_STEP_DEGREES;
    if (within(candidate, ROTATION_DETENT_CAPTURE_PX, ROTATION_DETENT_CAPTURE_DEGREES)) {
        latch.degrees = candidate;
        return {candidate, true};
    }
    return {degrees, false};
}

Glib::ustring format_selection_rotation(double degrees, std::locale const &locale)
{
    if (!std::isfinite(degrees)) return "\u2014\u00b0";
    if (std::abs(degrees) < 0.005) degrees = 0.0;
    std::ostringstream stream;
    stream.imbue(locale);
    if (degrees > 0) stream << '+';
    stream << std::fixed << std::setprecision(2) << degrees;
    return Glib::ustring{stream.str()} + "\u00b0";
}

Glib::ustring format_selection_rotation(double degrees)
{
    try {
        return format_selection_rotation(degrees, std::locale(""));
    } catch (std::runtime_error const &) {
        return format_selection_rotation(degrees, std::locale::classic());
    }
}

struct SelectionRotationOverlay::Impl {
    explicit Impl(SPDesktop &desktop)
        : desktop(desktop)
        , text(make_canvasitem<CanvasItemText>(desktop.getCanvasTemp(), Geom::Point(), ""))
    {
        Gdk::RGBA foreground{"#ffffff"}, background{"#202124"};
        Gdk::RGBA accent_foreground{"#ffffff"}, accent_background{"#1c58c6"};
        auto style = desktop.getCanvas()->get_style_context();
        style->lookup_color("theme_fg_color", foreground);
        style->lookup_color("theme_bg_color", background);
        style->lookup_color("theme_selected_fg_color", accent_foreground);
        style->lookup_color("theme_selected_bg_color", accent_background);
        background_color = rgba32(background);
        accent_background_color = rgba32(accent_background);
        foreground_color = selection_feedback_readable_foreground(rgba32(foreground), background_color);
        accent_foreground_color = selection_feedback_readable_foreground(
            rgba32(accent_foreground), accent_background_color);
        text->set_name("selector-rotation-angle");
        text->set_fontsize(11.0);
        text->set_border(4.0);
        text->set_bg_radius(0.35);
        text->set_anchor({0, 0});
        text->set_adjust({0, 0});
        text->set_pickable(false);
        text->set_visible(false);
        text->raise_to_top();
        zoom_connection = desktop.signal_zoom_changed.connect([this](double) {
            if (has_position) place();
        });
    }

    ~Impl() { zoom_connection.disconnect(); }

    void place()
    {
        auto const area = desktop.getCanvas()->get_area_world();
        is_visible = has_position && area.width() > 0 && area.height() > 0;
        if (!is_visible) {
            text->set_visible(false);
            return;
        }
        Geom::Rect const viewport(area);
        auto const point = handle * desktop.d2w();
        auto const size = text->get_text_size().dimensions();
        constexpr double gap = 8.0;
        double x = point.x() + gap;
        double y = point.y() + gap;
        if (x + size.x() > viewport.right()) x = point.x() - gap - size.x();
        if (y + size.y() > viewport.bottom()) y = point.y() - gap - size.y();
        auto const box = clamp_selection_feedback_label(
            Geom::Rect::from_xywh(x, y, size.x(), size.y()), viewport);
        text->set_coord(desktop.w2d(box.min()));
        text->set_visible(true);
    }

    SPDesktop &desktop;
    CanvasItemPtr<CanvasItemText> text;
    sigc::scoped_connection zoom_connection;
    Glib::ustring label;
    Geom::Point handle;
    bool has_position = false;
    bool is_visible = false;
    bool is_accented = false;
    uint32_t foreground_color = 0xffffffff;
    uint32_t background_color = 0x202124ff;
    uint32_t accent_foreground_color = 0xffffffff;
    uint32_t accent_background_color = 0x1c58c6ff;
};

SelectionRotationOverlay::SelectionRotationOverlay(SPDesktop &desktop)
    : _impl(std::make_unique<Impl>(desktop))
{}

SelectionRotationOverlay::~SelectionRotationOverlay() = default;

void SelectionRotationOverlay::update(double degrees, Geom::Point const &handle, bool accented)
{
    _impl->handle = handle;
    _impl->has_position = std::isfinite(degrees) && std::isfinite(handle.x()) && std::isfinite(handle.y());
    _impl->label = format_selection_rotation(degrees);
    _impl->is_accented = accented;
    _impl->text->set_text(_impl->label);
    _impl->text->set_fill(accented ? _impl->accent_foreground_color : _impl->foreground_color);
    _impl->text->set_background(accented ? _impl->accent_background_color : _impl->background_color);
    _impl->place();
}

Glib::ustring const &SelectionRotationOverlay::label() const { return _impl->label; }
bool SelectionRotationOverlay::visible() const noexcept { return _impl->is_visible; }
bool SelectionRotationOverlay::accented() const noexcept { return _impl->is_accented; }
double SelectionRotationOverlay::contrast_ratio() const noexcept
{
    return selection_feedback_contrast_ratio(
        _impl->is_accented ? _impl->accent_foreground_color : _impl->foreground_color,
        _impl->is_accented ? _impl->accent_background_color : _impl->background_color);
}

} // namespace Inkscape
