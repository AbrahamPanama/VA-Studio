// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * @file
 * @brief Interval arithmetic based intersections
 *//*
 * Copyright 2025 Muhammad Rafay Irfan <rafay119muhammad@gmail.com>
 */
#ifndef LIB2GEOM_SEEN_GEOMETRIC_INTERSECTION_H
#define LIB2GEOM_SEEN_GEOMETRIC_INTERSECTION_H

#include <2geom/arithmetic-interval.h>
#include <2geom/bezier-curve.h>
#include <2geom/coord.h>
#include <vector>

namespace Geom {

/**
 * @brief Represents intersections between geometric objects.
 *
 * This structure is used to represent intersections. In the case of a single point intersection,
 * a single `GeometricIntersection` object is returned with type `GeometricIntersection::Type::POINT`.
 *
 * In the case of an overlap, two `GeometricIntersection` objects are returned:
 * - The first represents the start of the overlap interval and has type
 * `GeometricIntersection::Type::OVERLAP_START_POINT`.
 * - The second represents the end of the overlap interval and has type
 * `GeometricIntersection::Type::OVERLAP_END_POINT`.
 */
struct GeometricIntersection {
    enum class Type { POINT, OVERLAP_END_POINT };

    Type type;

    /// Represents time value on the first curve
    Coord t;

    /// Represents time value on the second curve
    Coord s;
};

std::vector<GeometricIntersection> intersect(LineSegment const &seg1, LineSegment const &seg2,
                                             double epsilon = EPSILON);


} // namespace Geom

#endif // LIB2GEOM_SEEN_GEOMETRIC_INTERSECTION_H