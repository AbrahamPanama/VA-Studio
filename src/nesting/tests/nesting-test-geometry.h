// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_NESTING_TEST_GEOMETRY_H
#define INKSCAPE_NESTING_TEST_GEOMETRY_H

#include "../nesting-types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace Inkscape::Nesting::Test {

struct PartGeometry
{
    std::uint64_t id = 0;
    std::vector<CollisionComponent> components;
};

struct FixedContainerFixture
{
    std::string id;
    std::vector<Point> container;
    std::vector<std::vector<Point>> container_holes{};
    std::vector<PartGeometry> parts{};
    Options options;
    std::size_t minimum_placed = 0;
};

struct CanonicalRank
{
    double placed_area = 0.0;
    std::uint32_t placed_count = 0;
    double occupied_area = std::numeric_limits<double>::infinity();
};

enum class PointLocation { Outside, Boundary, Inside };

inline double cross(Point const &a, Point const &b, Point const &c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

inline double squared_distance(Point const &a, Point const &b)
{
    auto const dx = a.x - b.x;
    auto const dy = a.y - b.y;
    return dx * dx + dy * dy;
}

inline double coordinate_scale(FixedContainerFixture const &fixture)
{
    double scale = 1.0;
    auto include = [&scale](std::span<Point const> points) {
        for (auto const &point : points) {
            scale = std::max({scale, std::abs(point.x), std::abs(point.y)});
        }
    };
    include(fixture.container);
    for (auto const &hole : fixture.container_holes) {
        include(hole);
    }
    for (auto const &part : fixture.parts) {
        for (auto const &component : part.components) {
            include(component.outer);
            for (auto const &hole : component.holes) {
                include(hole);
            }
        }
    }
    return scale;
}

inline bool point_on_segment(Point const &point, Point const &a, Point const &b, double epsilon)
{
    if (std::abs(cross(a, b, point)) > epsilon * std::max(std::hypot(b.x - a.x, b.y - a.y), 1.0)) {
        return false;
    }
    return point.x >= std::min(a.x, b.x) - epsilon && point.x <= std::max(a.x, b.x) + epsilon &&
           point.y >= std::min(a.y, b.y) - epsilon && point.y <= std::max(a.y, b.y) + epsilon;
}

inline PointLocation point_in_ring(Point const &point, std::span<Point const> ring, double epsilon)
{
    if (ring.size() < 3) {
        return PointLocation::Outside;
    }

    bool inside = false;
    for (std::size_t index = 0, previous = ring.size() - 1; index < ring.size(); previous = index++) {
        auto const &a = ring[previous];
        auto const &b = ring[index];
        if (point_on_segment(point, a, b, epsilon)) {
            return PointLocation::Boundary;
        }
        auto const crosses_ray = (a.y > point.y) != (b.y > point.y);
        if (crosses_ray) {
            auto const crossing_x = (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x;
            if (crossing_x > point.x) {
                inside = !inside;
            }
        }
    }
    return inside ? PointLocation::Inside : PointLocation::Outside;
}

inline PointLocation point_in_region(Point const &point, CollisionComponent const &component, double epsilon)
{
    auto const outer = point_in_ring(point, component.outer, epsilon);
    if (outer != PointLocation::Inside) {
        return outer;
    }
    for (auto const &hole : component.holes) {
        auto const location = point_in_ring(point, hole, epsilon);
        if (location == PointLocation::Boundary) {
            return PointLocation::Boundary;
        }
        if (location == PointLocation::Inside) {
            return PointLocation::Outside;
        }
    }
    return PointLocation::Inside;
}

inline PointLocation point_in_container(Point const &point, FixedContainerFixture const &fixture, double epsilon)
{
    auto const outer = point_in_ring(point, fixture.container, epsilon);
    if (outer != PointLocation::Inside) {
        return outer;
    }
    for (auto const &hole : fixture.container_holes) {
        auto const location = point_in_ring(point, hole, epsilon);
        if (location == PointLocation::Boundary) {
            return PointLocation::Boundary;
        }
        if (location == PointLocation::Inside) {
            return PointLocation::Outside;
        }
    }
    return PointLocation::Inside;
}

inline double point_segment_distance(Point const &point, Point const &a, Point const &b)
{
    auto const dx = b.x - a.x;
    auto const dy = b.y - a.y;
    auto const length_squared = dx * dx + dy * dy;
    if (length_squared == 0.0) {
        return std::sqrt(squared_distance(point, a));
    }
    auto const projection = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / length_squared, 0.0, 1.0);
    Point const nearest{a.x + projection * dx, a.y + projection * dy};
    return std::sqrt(squared_distance(point, nearest));
}

inline int orientation_sign(double value, double epsilon)
{
    if (value > epsilon) {
        return 1;
    }
    if (value < -epsilon) {
        return -1;
    }
    return 0;
}

inline bool segments_intersect(Point const &a, Point const &b, Point const &c, Point const &d, double epsilon)
{
    auto const ab_c = orientation_sign(cross(a, b, c), epsilon);
    auto const ab_d = orientation_sign(cross(a, b, d), epsilon);
    auto const cd_a = orientation_sign(cross(c, d, a), epsilon);
    auto const cd_b = orientation_sign(cross(c, d, b), epsilon);
    if (ab_c * ab_d < 0 && cd_a * cd_b < 0) {
        return true;
    }
    return (ab_c == 0 && point_on_segment(c, a, b, epsilon)) ||
           (ab_d == 0 && point_on_segment(d, a, b, epsilon)) ||
           (cd_a == 0 && point_on_segment(a, c, d, epsilon)) ||
           (cd_b == 0 && point_on_segment(b, c, d, epsilon));
}

inline bool segments_properly_cross(Point const &a, Point const &b, Point const &c, Point const &d,
                                    double epsilon)
{
    auto const ab_c = orientation_sign(cross(a, b, c), epsilon);
    auto const ab_d = orientation_sign(cross(a, b, d), epsilon);
    auto const cd_a = orientation_sign(cross(c, d, a), epsilon);
    auto const cd_b = orientation_sign(cross(c, d, b), epsilon);
    return ab_c * ab_d < 0 && cd_a * cd_b < 0;
}

inline double segment_distance(Point const &a, Point const &b, Point const &c, Point const &d, double epsilon)
{
    if (segments_intersect(a, b, c, d, epsilon)) {
        return 0.0;
    }
    return std::min({point_segment_distance(a, c, d), point_segment_distance(b, c, d),
                     point_segment_distance(c, a, b), point_segment_distance(d, a, b)});
}

inline double ring_distance(std::span<Point const> left, std::span<Point const> right, double epsilon)
{
    auto best = std::numeric_limits<double>::infinity();
    for (std::size_t left_index = 0; left_index < left.size(); ++left_index) {
        auto const &left_a = left[left_index];
        auto const &left_b = left[(left_index + 1) % left.size()];
        for (std::size_t right_index = 0; right_index < right.size(); ++right_index) {
            auto const &right_a = right[right_index];
            auto const &right_b = right[(right_index + 1) % right.size()];
            best = std::min(best, segment_distance(left_a, left_b, right_a, right_b, epsilon));
        }
    }
    return best;
}

inline bool rings_intersect(std::span<Point const> left, std::span<Point const> right, double epsilon)
{
    for (std::size_t left_index = 0; left_index < left.size(); ++left_index) {
        auto const &left_a = left[left_index];
        auto const &left_b = left[(left_index + 1) % left.size()];
        for (std::size_t right_index = 0; right_index < right.size(); ++right_index) {
            auto const &right_a = right[right_index];
            auto const &right_b = right[(right_index + 1) % right.size()];
            if (segments_intersect(left_a, left_b, right_a, right_b, epsilon)) {
                return true;
            }
        }
    }
    return false;
}

inline bool rings_properly_cross(std::span<Point const> left, std::span<Point const> right, double epsilon)
{
    for (std::size_t left_index = 0; left_index < left.size(); ++left_index) {
        auto const &left_a = left[left_index];
        auto const &left_b = left[(left_index + 1) % left.size()];
        for (std::size_t right_index = 0; right_index < right.size(); ++right_index) {
            auto const &right_a = right[right_index];
            auto const &right_b = right[(right_index + 1) % right.size()];
            if (segments_properly_cross(left_a, left_b, right_a, right_b, epsilon)) {
                return true;
            }
        }
    }
    return false;
}

inline std::vector<std::span<Point const>> region_rings(CollisionComponent const &component)
{
    std::vector<std::span<Point const>> result;
    result.reserve(component.holes.size() + 1);
    result.emplace_back(component.outer);
    for (auto const &hole : component.holes) {
        result.emplace_back(hole);
    }
    return result;
}

inline double signed_twice_ring_area(std::span<Point const> ring)
{
    double result = 0.0;
    for (std::size_t index = 0; index < ring.size(); ++index) {
        auto const &left = ring[index];
        auto const &right = ring[(index + 1) % ring.size()];
        result += left.x * right.y - right.x * left.y;
    }
    return result;
}

inline void append_occupied_side_probes(std::vector<Point> &probes, CollisionComponent const &component,
                                        std::span<Point const> ring, bool ring_is_hole, double epsilon)
{
    auto const signed_area = signed_twice_ring_area(ring);
    if (std::abs(signed_area) <= epsilon) {
        return;
    }
    auto const interior_is_left = signed_area > 0.0;
    for (std::size_t index = 0; index < ring.size(); ++index) {
        auto const &a = ring[index];
        auto const &b = ring[(index + 1) % ring.size()];
        auto const dx = b.x - a.x;
        auto const dy = b.y - a.y;
        auto const length = std::hypot(dx, dy);
        if (length <= epsilon) {
            continue;
        }
        auto direction = interior_is_left ? 1.0 : -1.0;
        if (ring_is_hole) {
            direction = -direction;
        }
        auto const probe_distance = std::max(epsilon * 32.0, length * 1.0e-6);
        Point const probe{
            (a.x + b.x) * 0.5 + direction * (-dy / length) * probe_distance,
            (a.y + b.y) * 0.5 + direction * (dx / length) * probe_distance,
        };
        if (point_in_region(probe, component, epsilon) == PointLocation::Inside) {
            probes.emplace_back(probe);
        }
    }
}

inline std::vector<Point> occupied_side_probes(CollisionComponent const &component, double epsilon)
{
    std::vector<Point> probes;
    append_occupied_side_probes(probes, component, component.outer, false, epsilon);
    for (auto const &hole : component.holes) {
        append_occupied_side_probes(probes, component, hole, true, epsilon);
    }
    return probes;
}

inline bool regions_overlap(CollisionComponent const &left, CollisionComponent const &right, double epsilon)
{
    auto const left_rings = region_rings(left);
    auto const right_rings = region_rings(right);
    for (auto const left_ring : left_rings) {
        for (auto const right_ring : right_rings) {
            if (rings_properly_cross(left_ring, right_ring, epsilon)) {
                return true;
            }
        }
    }
    for (auto const &point : left.outer) {
        if (point_in_region(point, right, epsilon) == PointLocation::Inside) {
            return true;
        }
    }
    for (auto const &point : right.outer) {
        if (point_in_region(point, left, epsilon) == PointLocation::Inside) {
            return true;
        }
    }
    for (auto const &probe : occupied_side_probes(left, epsilon)) {
        if (point_in_region(probe, right, epsilon) == PointLocation::Inside) {
            return true;
        }
    }
    for (auto const &probe : occupied_side_probes(right, epsilon)) {
        if (point_in_region(probe, left, epsilon) == PointLocation::Inside) {
            return true;
        }
    }
    return false;
}

inline double region_boundary_distance(CollisionComponent const &left, CollisionComponent const &right,
                                       double epsilon)
{
    auto best = std::numeric_limits<double>::infinity();
    for (auto const left_ring : region_rings(left)) {
        for (auto const right_ring : region_rings(right)) {
            best = std::min(best, ring_distance(left_ring, right_ring, epsilon));
        }
    }
    return best;
}

inline Point transformed(Point const &point, Placement const &placement)
{
    auto const radians = placement.rotation_degrees * std::numbers::pi / 180.0;
    auto const cosine = std::cos(radians);
    auto const sine = std::sin(radians);
    return {point.x * cosine - point.y * sine + placement.translation_x,
            point.x * sine + point.y * cosine + placement.translation_y};
}

inline CollisionComponent transformed(CollisionComponent const &component, Placement const &placement)
{
    CollisionComponent result;
    result.outer.reserve(component.outer.size());
    for (auto const &point : component.outer) {
        result.outer.emplace_back(transformed(point, placement));
    }
    result.holes.reserve(component.holes.size());
    for (auto const &hole : component.holes) {
        auto &result_hole = result.holes.emplace_back();
        result_hole.reserve(hole.size());
        for (auto const &point : hole) {
            result_hole.emplace_back(transformed(point, placement));
        }
    }
    return result;
}

inline double ring_area(std::span<Point const> ring)
{
    return std::abs(signed_twice_ring_area(ring)) * 0.5;
}

inline double part_area(PartGeometry const &part)
{
    double area = 0.0;
    for (auto const &component : part.components) {
        area += ring_area(component.outer);
        for (auto const &hole : component.holes) {
            area -= ring_area(hole);
        }
    }
    return std::max(area, 0.0);
}

inline bool rotation_allowed(double rotation, Options const &options, double epsilon)
{
    auto angle = std::fmod(rotation, 360.0);
    if (angle < 0.0) {
        angle += 360.0;
    }
    auto near_multiple = [angle, epsilon](double step) {
        auto const nearest = std::round(angle / step) * step;
        return std::abs(angle - nearest) <= epsilon || std::abs(angle - nearest - 360.0) <= epsilon;
    };
    switch (options.rotation_mode) {
        case RotationMode::None:
            return std::abs(angle) <= epsilon || std::abs(angle - 360.0) <= epsilon;
        case RotationMode::RightAngles:
            return near_multiple(90.0);
        case RotationMode::Discrete:
            return near_multiple(options.rotation_step_degrees);
        case RotationMode::Free:
            return true;
    }
    return false;
}

inline std::optional<std::string> validate_placements(FixedContainerFixture const &fixture,
                                                      std::span<Placement const> placements)
{
    if (placements.size() != fixture.parts.size()) {
        return "result cardinality differs from input cardinality";
    }
    auto const scale = coordinate_scale(fixture);
    auto const epsilon = scale * 1.0e-8;
    auto const clearance_epsilon = scale * 2.0e-5;
    auto const rotation_epsilon = 2.0e-3;
    // The sheet margin is the only clearance to the container and its holes.
    auto const boundary_clearance = fixture.options.container_margin;

    struct PlacedPart
    {
        std::uint64_t id;
        std::vector<CollisionComponent> components;
    };
    std::vector<PlacedPart> placed_parts;

    for (std::size_t index = 0; index < placements.size(); ++index) {
        auto const &part = fixture.parts[index];
        auto const &placement = placements[index];
        if (placement.part_id != part.id) {
            return "placements are not in input order or lost a stable part id";
        }
        if (!std::isfinite(placement.translation_x) || !std::isfinite(placement.translation_y) ||
            !std::isfinite(placement.rotation_degrees)) {
            return "placement contains a non-finite transform";
        }
        if (!placement.placed) {
            if (placement.translation_x != 0.0 || placement.translation_y != 0.0 ||
                placement.rotation_degrees != 0.0) {
                return "unplaced result contains a non-zero transform";
            }
            continue;
        }
        if (!rotation_allowed(placement.rotation_degrees, fixture.options, rotation_epsilon)) {
            return "placement violates the captured rotation policy";
        }

        auto &placed = placed_parts.emplace_back();
        placed.id = part.id;
        placed.components.reserve(part.components.size());
        for (auto const &component : part.components) {
            auto transformed_component = transformed(component, placement);
            for (auto const &point : transformed_component.outer) {
                auto const location = point_in_container(point, fixture, epsilon);
                if (location == PointLocation::Outside ||
                    (location == PointLocation::Boundary && boundary_clearance > clearance_epsilon)) {
                    std::ostringstream message;
                    message << "part " << part.id << " is outside the usable container";
                    return message.str();
                }
            }
            if (rings_properly_cross(transformed_component.outer, fixture.container, epsilon)) {
                std::ostringstream message;
                message << "part " << part.id << " crosses the container boundary";
                return message.str();
            }
            for (auto const &hole : fixture.container_holes) {
                if (rings_properly_cross(transformed_component.outer, hole, epsilon)) {
                    std::ostringstream message;
                    message << "part " << part.id << " crosses a container hole boundary";
                    return message.str();
                }
                CollisionComponent const hole_region{.outer = hole, .holes = {}};
                if (regions_overlap(transformed_component, hole_region, epsilon)) {
                    std::ostringstream message;
                    message << "part " << part.id << " covers a container hole";
                    return message.str();
                }
            }
            if (boundary_clearance > 0.0) {
                auto distance = ring_distance(transformed_component.outer, fixture.container, epsilon);
                for (auto const &hole : fixture.container_holes) {
                    distance = std::min(distance, ring_distance(transformed_component.outer, hole, epsilon));
                }
                if (distance + clearance_epsilon < boundary_clearance) {
                    std::ostringstream message;
                    message << "part " << part.id << " violates container clearance: " << distance << " < "
                            << boundary_clearance;
                    return message.str();
                }
            }
            placed.components.emplace_back(std::move(transformed_component));
        }
    }

    for (std::size_t left_index = 0; left_index < placed_parts.size(); ++left_index) {
        for (std::size_t right_index = left_index + 1; right_index < placed_parts.size(); ++right_index) {
            auto const &left = placed_parts[left_index];
            auto const &right = placed_parts[right_index];
            auto minimum_distance = std::numeric_limits<double>::infinity();
            for (auto const &left_component : left.components) {
                for (auto const &right_component : right.components) {
                    if (regions_overlap(left_component, right_component, epsilon)) {
                        std::ostringstream message;
                        message << "parts " << left.id << " and " << right.id << " overlap";
                        return message.str();
                    }
                    minimum_distance = std::min(minimum_distance,
                                                region_boundary_distance(left_component, right_component, epsilon));
                }
            }
            if (fixture.options.part_spacing > 0.0 &&
                minimum_distance + clearance_epsilon < fixture.options.part_spacing) {
                std::ostringstream message;
                message << "parts " << left.id << " and " << right.id << " violate spacing: "
                        << minimum_distance << " < " << fixture.options.part_spacing;
                return message.str();
            }
        }
    }

    return std::nullopt;
}

inline CanonicalRank canonical_rank(FixedContainerFixture const &fixture, std::span<Placement const> placements)
{
    CanonicalRank rank;
    auto left = std::numeric_limits<double>::infinity();
    auto top = std::numeric_limits<double>::infinity();
    auto right = -std::numeric_limits<double>::infinity();
    auto bottom = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < placements.size() && index < fixture.parts.size(); ++index) {
        auto const &placement = placements[index];
        if (!placement.placed) {
            continue;
        }
        rank.placed_area += part_area(fixture.parts[index]);
        ++rank.placed_count;
        for (auto const &component : fixture.parts[index].components) {
            for (auto const &point : component.outer) {
                auto const transformed_point = transformed(point, placement);
                left = std::min(left, transformed_point.x);
                top = std::min(top, transformed_point.y);
                right = std::max(right, transformed_point.x);
                bottom = std::max(bottom, transformed_point.y);
            }
        }
    }
    if (rank.placed_count != 0) {
        rank.occupied_area = std::max(0.0, right - left) * std::max(0.0, bottom - top);
    }
    return rank;
}

inline bool rank_not_worse(CanonicalRank const &candidate, CanonicalRank const &incumbent)
{
    auto const area_tolerance = std::max({std::abs(candidate.placed_area), std::abs(incumbent.placed_area), 1.0}) *
                                1.0e-9;
    if (candidate.placed_area > incumbent.placed_area + area_tolerance) {
        return true;
    }
    if (incumbent.placed_area > candidate.placed_area + area_tolerance) {
        return false;
    }
    if (candidate.placed_count != incumbent.placed_count) {
        return candidate.placed_count > incumbent.placed_count;
    }
    auto const occupied_tolerance =
        std::max({std::abs(candidate.occupied_area), std::abs(incumbent.occupied_area), 1.0}) * 1.0e-9;
    return candidate.occupied_area <= incumbent.occupied_area + occupied_tolerance;
}

inline bool ranks_equivalent(CanonicalRank const &left, CanonicalRank const &right)
{
    return rank_not_worse(left, right) && rank_not_worse(right, left);
}

} // namespace Inkscape::Nesting::Test

#endif // INKSCAPE_NESTING_TEST_GEOMETRY_H
