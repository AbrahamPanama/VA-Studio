// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/** @file Implementation of Path::extrema()
 */
/* An algorithm to find the points on a path where a given coordinate
 * attains its minimum and maximum values.
 *
 * Authors:
 *   Rafał Siejakowski <rs@rs-math.net>
 *
 * Copyright 2022 the Authors.
 */

#include <2geom/curve.h>
#include <2geom/path.h>
#include <2geom/point.h>

namespace Geom {

/** Returns +1 for positive numbers, -1 for negative numbers, and 0 otherwise. */
inline static float sign(double number)
{
    if (number > 0.0) {
        return 1.0;
    } else if (number < 0.0) {
        return -1.0;
    }
    return 0.0;
}

/** @brief Determine whether the d-coordinate increases or decreases at the given path time.
 *
 * @param path A path.
 * @param time A forward-normalized time on the given path.
 * @param d The coordinate about which we want to know whether it increases.
 * @return +1.0 if the coordinate increases, -1.0 if it decreases, 0.0 if it remains constant.
*/
static float find_direction_of_travel(Path const &path, PathTime const &time, Dim2 d)
{
    if (time.t == 0.0) { // We're at a node point
        if (time.curve_index == 0) { // Starting point of the path.
            if (path.closed()) {
                return sign(path.initialUnitTangent()[d] + path.finalUnitTangent()[d]);
            } else {
                return sign(path.initialUnitTangent()[d]);
            }
        } else if (time.curve_index == path.size()) { // End point of the path.
            if (path.closed()) {
                return sign(path.initialUnitTangent()[d] + path.finalUnitTangent()[d]);
            } else {
                return sign(path.finalUnitTangent()[d]);
            }
        }

        // Otherwise, check the average of the two unit tangent vectors.
        auto const outgoing_tangent = path.curveAt(time.curve_index).unitTangentAt(0.0);
        auto const incoming_tangent = path.curveAt(time.curve_index - 1).unitTangentAt(1.0);
        return sign(outgoing_tangent[d] + incoming_tangent[d]);
    }
    // We're in the middle of a curve
    return sign(path.curveAt(time.curve_index).unitTangentAt(time.t)[d]);
}

/* Find information about the points on the path where the specified
 * coordinate attains its minimum and maximum values.
 */
PathExtrema Path::extrema(Dim2 d) const
{
    auto const ZERO_TIME = PathTime(0, 0);

    // Handle the trivial case of an empty path.
    if (empty()) {
        auto const origin = initialPoint();
        return PathExtrema{
            .min_point = origin,
            .max_point = origin,
            .glance_direction_at_min = 0.0,
            .glance_direction_at_max = 0.0,
            .min_time = ZERO_TIME,
            .max_time = ZERO_TIME
        };
    }

    // Set up the simultaneous min-max search
    Point min_point = initialPoint(), max_point = min_point;
    auto min_time = ZERO_TIME, max_time = ZERO_TIME;
    unsigned curve_counter = 0;

    /// A closure to update the current minimum and maximum values.
    auto const update_minmax = [&](Point const &new_point, Coord t) {
        if (new_point[d] < min_point[d]) {
            min_point = new_point;
            min_time = PathTime(curve_counter, t);
        } else if (new_point[d] > max_point[d]) {
            max_point = new_point;
            max_time = PathTime(curve_counter, t);
        }
    };

    // Iterate through the curves, searching for min and max.
    for (auto const &curve : _data->curves) {
        // Check the starting node first
        update_minmax(curve.initialPoint(), 0.0);

        // Check the critical points (zeroes of the derivative).
        std::unique_ptr<Curve> const derivative{curve.derivative()};
        for (auto root : derivative->roots(0.0, d)) {
            update_minmax(curve.pointAt(root), root);
        }
        curve_counter++;
    }

    auto const other = other_dimension(d);
    return PathExtrema{
        .min_point = min_point,
        .max_point = max_point,
        .glance_direction_at_min = find_direction_of_travel(*this, min_time, other),
        .glance_direction_at_max = find_direction_of_travel(*this, max_time, other),
        .min_time = min_time,
        .max_time = max_time
    };
}

} // namespace Geom

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
