// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_SELECTION_ROTATION_FEEDBACK_H
#define INKSCAPE_SELECTION_ROTATION_FEEDBACK_H

#include <cstddef>
#include <locale>
#include <memory>
#include <optional>
#include <2geom/point.h>
#include <glibmm/ustring.h>

class SPDesktop;

namespace Inkscape {

// Provisional screen-space thresholds; physical mouse/trackpad qualification
// remains required. Angular caps prevent a small radius becoming a hard grid.
constexpr double ROTATION_DETENT_STEP_DEGREES = 15.0;
constexpr double ROTATION_DETENT_CAPTURE_PX = 4.0;
constexpr double ROTATION_DETENT_RELEASE_PX = 8.0;
constexpr double ROTATION_DETENT_CAPTURE_DEGREES = 3.0;
constexpr double ROTATION_DETENT_RELEASE_DEGREES = 6.0;
constexpr double ROTATION_DETENT_MIN_RADIUS_PX = 24.0;
constexpr double ROTATION_ANGLE_MIN_RADIUS_PX = 4.0;

/** Lift an accepted rotation to the revolution nearest a continuous raw angle. */
double equivalent_rotation_degrees(double radians, double reference_degrees) noexcept;

/**
 * Unwrap pointer rotation relative to the fixed gesture baseline, not the last
 * snapped transform. Samples must be less than half a turn apart: no pointer
 * sampler can recover revolutions that happened entirely between two events.
 */
class ContinuousRotationAngle final
{
public:
    std::optional<double> update(double wrapped_radians, double radius_px) noexcept;
    void reset() noexcept;
    double degrees() const noexcept { return _degrees; }

private:
    double _degrees = 0.0;
    std::optional<double> _last_degrees = 0.0;
};

struct RotationDetentLatch {
    std::optional<double> degrees;
    bool engaged() const noexcept { return degrees.has_value(); }
    void reset() noexcept { degrees.reset(); }
};

struct RotationDetentResult {
    double degrees = 0.0;
    bool engaged = false;
};

/**
 * Soft detents in accumulated gesture-relative degrees. Distances are angular
 * arc lengths at the pointer's current screen radius. Explicit constraints or
 * an accepted geometric snap clear the latch and win without a second snap.
 */
RotationDetentResult evaluate_rotation_detent(double degrees, double radius_px,
                                             bool enabled, bool higher_priority_snap,
                                             RotationDetentLatch &latch) noexcept;

Glib::ustring format_selection_rotation(double degrees);
Glib::ustring format_selection_rotation(double degrees, std::locale const &locale);

/** One non-pickable, view-local label; no SVG, idle source, or document ownership. */
class SelectionRotationOverlay final
{
public:
    explicit SelectionRotationOverlay(SPDesktop &desktop);
    ~SelectionRotationOverlay();
    SelectionRotationOverlay(SelectionRotationOverlay const &) = delete;
    SelectionRotationOverlay &operator=(SelectionRotationOverlay const &) = delete;

    void update(double degrees, Geom::Point const &handle, bool accented);
    Glib::ustring const &label() const;
    bool visible() const noexcept;
    bool accented() const noexcept;
    double contrast_ratio() const noexcept;
    std::size_t canvas_item_count() const noexcept { return 1; }

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Inkscape

#endif // INKSCAPE_SELECTION_ROTATION_FEEDBACK_H
