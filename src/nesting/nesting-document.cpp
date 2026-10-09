// SPDX-License-Identifier: GPL-2.0-or-later

#include "nesting-document.h"
#include "sparrow-adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstring>
#include <limits>
#include <deque>
#include <map>
#include <memory>
#include <numbers>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <2geom/curve.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>
#include <2geom/point.h>
#include <2geom/transforms.h>

#include "document-undo.h"
#include "document.h"
#include "selection.h"
#include "id-clash.h"
#include "display/cairo-utils.h"
#include "object/sp-flowtext.h"
#include "object/sp-clippath.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-shape.h"
#include "object/sp-page.h"
#include "object/object-set.h"
#include "page-manager.h"
#include "xml/repr.h"
#include "object/sp-text.h"
#include "path/path-boolop.h"
#include "path/path-outline.h"
#include "path/path-util.h"
#include "style.h"
#include "svg/svg.h"
#include "ui/icon-names.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "xml/subtree-revision.h"

namespace Inkscape::Nesting {

/// A traced alpha boundary in pixel coordinates (or the reason it failed),
/// shared by every bitmap with the same pixels.
struct AlphaTrace
{
    bool traced = false;
    bool too_complex = false;
    std::string failure;
    std::vector<CollisionComponent> components;
};

namespace {

constexpr std::size_t MAX_CONTOUR_POINTS = 100'000;
constexpr unsigned MAX_FLATTEN_DEPTH = 18;
constexpr double MIN_ABSOLUTE_TOLERANCE = 1.0e-6;
constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;
constexpr char NESTING_CONTOUR_ATTRIBUTE[] = "inkscape:nesting-contour";

struct PolygonGeometry
{
    std::vector<Point> outer;
    std::vector<std::vector<Point>> holes;
    ContourSource source = ContourSource::ExactVector;
};

struct GeometryResult
{
    std::optional<PolygonGeometry> geometry;
    std::string error;
    bool too_complex = false;

    [[nodiscard]] explicit operator bool() const noexcept { return geometry.has_value(); }
};

struct PartGeometry
{
    std::vector<CollisionComponent> components;
    ContourSource source = ContourSource::ExactVector;
    RecoveryKind recovery = RecoveryKind::Clean;
    std::string recovery_reason;
};

struct PartGeometryResult
{
    std::optional<PartGeometry> geometry;
    std::string error;
    bool too_complex = false;

    [[nodiscard]] explicit operator bool() const noexcept { return geometry.has_value(); }
};

struct Contour
{
    std::vector<Point> points;
    double signed_area = 0.0;
    double absolute_area = 0.0;
    int parent = -1;
    unsigned depth = 0;
};

struct PixelVertex
{
    int x = 0;
    int y = 0;

    bool operator==(PixelVertex const &) const = default;
};

struct PixelVertexHash
{
    std::size_t operator()(PixelVertex const &point) const noexcept
    {
        auto const x = static_cast<std::uint32_t>(point.x);
        auto const y = static_cast<std::uint32_t>(point.y);
        return (static_cast<std::size_t>(x) << 32U) ^ y;
    }
};

struct AlphaTraceResult
{
    std::vector<CollisionComponent> components;
    bool too_complex = false;
};

// Content-addressed (width, height, alpha hash): a pixbuf address is not a
// reliable identity because pixel data can change in place, and workers must
// not touch pixbufs at all. Shared by the GTK thread and workers.
using AlphaTraceKey = std::tuple<int, int, std::uint64_t>;
std::mutex alpha_trace_mutex;
std::map<AlphaTraceKey, std::shared_ptr<AlphaTrace const>> alpha_trace_cache;
std::deque<AlphaTraceKey> alpha_trace_order;
constexpr std::size_t ALPHA_TRACE_CACHE_SIZE = 64;

std::shared_ptr<AlphaTrace const> cached_alpha_trace(AlphaTraceKey const &key)
{
    std::lock_guard lock{alpha_trace_mutex};
    auto const found = alpha_trace_cache.find(key);
    return found == alpha_trace_cache.end() ? nullptr : found->second;
}

void store_alpha_trace(AlphaTraceKey const &key, std::shared_ptr<AlphaTrace const> trace)
{
    std::lock_guard lock{alpha_trace_mutex};
    if (alpha_trace_cache.emplace(key, std::move(trace)).second) {
        alpha_trace_order.push_back(key);
        while (alpha_trace_order.size() > ALPHA_TRACE_CACHE_SIZE) {
            alpha_trace_cache.erase(alpha_trace_order.front());
            alpha_trace_order.pop_front();
        }
    }
}

bool finite(Point const &point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool finite(Geom::Affine const &affine)
{
    for (unsigned index = 0; index < 6; ++index) {
        if (!std::isfinite(affine[index]))
            return false;
    }
    return true;
}

bool affine_near(Geom::Affine const &left, Geom::Affine const &right, double epsilon = 1.0e-9)
{
    for (unsigned index = 0; index < 6; ++index) {
        auto const scale = std::max({1.0, std::abs(left[index]), std::abs(right[index])});
        if (std::abs(left[index] - right[index]) > epsilon * scale)
            return false;
    }
    return true;
}

double squared_distance(Point const &left, Point const &right)
{
    auto const dx = left.x - right.x;
    auto const dy = left.y - right.y;
    return dx * dx + dy * dy;
}

double squared_distance_to_line(Point const &point, Point const &start, Point const &end)
{
    auto const dx = end.x - start.x;
    auto const dy = end.y - start.y;
    auto const length_squared = dx * dx + dy * dy;
    if (length_squared <= std::numeric_limits<double>::min()) {
        return squared_distance(point, start);
    }
    auto const cross = dx * (start.y - point.y) - (start.x - point.x) * dy;
    return cross * cross / length_squared;
}

Point point_at(Geom::Curve const &curve, double time)
{
    auto const point = curve.pointAt(time);
    return {point[Geom::X], point[Geom::Y]};
}

bool append_flattened_curve(Geom::Curve const &curve, double start_time, double end_time, Point const &start,
                            Point const &end, double tolerance_squared, unsigned depth, std::vector<Point> &points)
{
    if (points.size() >= MAX_CONTOUR_POINTS)
        return false;

    auto const quarter_time = start_time + (end_time - start_time) * 0.25;
    auto const middle_time = start_time + (end_time - start_time) * 0.5;
    auto const three_quarter_time = start_time + (end_time - start_time) * 0.75;
    auto const quarter = point_at(curve, quarter_time);
    auto const middle = point_at(curve, middle_time);
    auto const three_quarter = point_at(curve, three_quarter_time);
    if (!finite(quarter) || !finite(middle) || !finite(three_quarter) || !finite(end))
        return false;

    auto const flat_enough = squared_distance_to_line(quarter, start, end) <= tolerance_squared &&
                             squared_distance_to_line(middle, start, end) <= tolerance_squared &&
                             squared_distance_to_line(three_quarter, start, end) <= tolerance_squared;
    if (flat_enough || depth >= MAX_FLATTEN_DEPTH) {
        points.emplace_back(end);
        return true;
    }

    if (!append_flattened_curve(curve, start_time, middle_time, start, middle, tolerance_squared, depth + 1, points)) {
        return false;
    }
    return append_flattened_curve(curve, middle_time, end_time, middle, end, tolerance_squared, depth + 1, points);
}

double signed_area(std::span<Point const> points)
{
    double twice_area = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        auto const &current = points[index];
        auto const &next = points[(index + 1) % points.size()];
        twice_area += current.x * next.y - next.x * current.y;
    }
    return twice_area * 0.5;
}

double component_area(CollisionComponent const &component)
{
    auto area = std::abs(signed_area(component.outer));
    for (auto const &hole : component.holes) {
        area -= std::abs(signed_area(hole));
    }
    return std::max(0.0, area);
}

double part_area(std::span<CollisionComponent const> components)
{
    double area = 0.0;
    for (auto const &component : components) {
        area += component_area(component);
    }
    return area;
}

double polygon_area(std::span<Point const> outer, std::span<std::vector<Point> const> holes)
{
    auto area = std::abs(signed_area(outer));
    for (auto const &hole : holes) {
        area -= std::abs(signed_area(hole));
    }
    return std::max(0.0, area);
}

/**
 * Exact geometry-preserving cleanup: remove only bit-identical duplicates and
 * points that are exactly collinear and strictly between their neighbours
 * (cross == 0, dot <= 0). Tolerance-based cleanup is deliberately not used here
 * because it can shave sampled raster cells or clip a vector bulge; every
 * accepted cleanup must leave the occupied set unchanged.
 */
void remove_redundant_points(std::vector<Point> &points)
{
    if (points.size() < 2)
        return;

    std::vector<Point> deduplicated;
    deduplicated.reserve(points.size());
    for (auto const &point : points) {
        if (deduplicated.empty() || deduplicated.back().x != point.x || deduplicated.back().y != point.y)
            deduplicated.emplace_back(point);
    }
    if (deduplicated.size() > 1 && deduplicated.front().x == deduplicated.back().x &&
        deduplicated.front().y == deduplicated.back().y) {
        deduplicated.pop_back();
    }
    if (deduplicated.size() < 3) {
        points = std::move(deduplicated);
        return;
    }

    // Single linear pass; the previous restart-on-removal loop was quadratic on
    // long collinear runs (a raster boundary can contain over a million edges).
    std::vector<Point> simplified;
    simplified.reserve(deduplicated.size());
    for (auto const &point : deduplicated) {
        while (simplified.size() >= 2) {
            auto const &previous = simplified[simplified.size() - 2];
            auto const &current = simplified.back();
            auto const cross = (current.x - previous.x) * (point.y - previous.y) -
                               (current.y - previous.y) * (point.x - previous.x);
            auto const dot = (current.x - previous.x) * (current.x - point.x) +
                             (current.y - previous.y) * (current.y - point.y);
            if (cross == 0.0 && dot <= 0.0)
                simplified.pop_back();
            else
                break;
        }
        simplified.push_back(point);
    }

    // Close the ring: the first/last vertices can become collinear or duplicate
    // through the wrap-around, so repeat until stable.
    bool changed = true;
    while (changed && simplified.size() >= 3) {
        changed = false;
        while (simplified.size() >= 3) {
            auto const &previous = simplified[simplified.size() - 2];
            auto const &current = simplified.back();
            auto const &next = simplified.front();
            auto const cross = (current.x - previous.x) * (next.y - previous.y) -
                               (current.y - previous.y) * (next.x - previous.x);
            auto const dot = (current.x - previous.x) * (current.x - next.x) +
                             (current.y - previous.y) * (current.y - next.y);
            if (cross == 0.0 && dot <= 0.0) {
                simplified.pop_back();
                changed = true;
            } else {
                break;
            }
        }
        while (simplified.size() >= 3) {
            auto const &previous = simplified.back();
            auto const &current = simplified.front();
            auto const &next = simplified[1];
            auto const cross = (current.x - previous.x) * (next.y - previous.y) -
                               (current.y - previous.y) * (next.x - previous.x);
            auto const dot = (current.x - previous.x) * (current.x - next.x) +
                             (current.y - previous.y) * (current.y - next.y);
            if (cross == 0.0 && dot <= 0.0) {
                simplified.erase(simplified.begin());
                changed = true;
            } else {
                break;
            }
        }
        if (simplified.size() > 1 && simplified.front().x == simplified.back().x &&
            simplified.front().y == simplified.back().y) {
            simplified.pop_back();
            changed = true;
        }
    }
    points = std::move(simplified);
}

/**
 * Flatten one closed Geom::Path ring. P2 semantics are retained deliberately:
 * the flattened ring is cleaned with the exact collinear-only pass (no
 * tolerance shaving, which could cut sampled cells) and is never split into
 * partial cycles. A degenerate ring may return its cleaned points to the part
 * caller for a conservative hull; other failures remain std::nullopt so parts
 * use complete rendered bounds and containers continue to fail closed.
 */
std::optional<Contour> flatten_contour(Geom::Path const &path, double tolerance, bool *degenerate = nullptr,
                                       std::vector<Point> *degenerate_points = nullptr)
{
    if (degenerate)
        *degenerate = false;
    if (path.empty()) {
        return std::nullopt;
    }

    auto const initial = path.initialPoint();
    Point start{initial[Geom::X], initial[Geom::Y]};
    if (!finite(start))
        return std::nullopt;

    std::vector<Point> points;
    points.reserve(path.size_open() + 1);
    points.emplace_back(start);
    for (auto iterator = path.begin(); iterator != path.end_open(); ++iterator) {
        auto const end = point_at(*iterator, 1.0);
        if (!append_flattened_curve(*iterator, 0.0, 1.0, start, end, tolerance * tolerance, 0, points))
            return std::nullopt;
        start = end;
    }

    remove_redundant_points(points);
    if (points.size() < 3) {
        if (degenerate) {
            *degenerate = true;
            if (degenerate_points)
                degenerate_points->insert(degenerate_points->end(), points.begin(), points.end());
        }
        return std::nullopt;
    }
    if (points.size() > MAX_CONTOUR_POINTS)
        return std::nullopt;
    auto const area = signed_area(points);
    if (!std::isfinite(area))
        return std::nullopt;
    if (std::abs(area) <= tolerance * tolerance) {
        if (degenerate) {
            *degenerate = true;
            if (degenerate_points)
                degenerate_points->insert(degenerate_points->end(), points.begin(), points.end());
        }
        return std::nullopt;
    }
    return Contour{.points = std::move(points), .signed_area = area, .absolute_area = std::abs(area)};
}

// --- P2 shared validity gate, bounded budget and safe recovery -------------

constexpr std::size_t MAX_PART_VALIDATED_VERTICES = 100'000;
constexpr std::size_t MAX_PART_VALIDATED_RINGS = 4'096;
constexpr std::uint64_t MAX_VALIDATION_PAIR_CANDIDATES = 2'000'000;
constexpr std::size_t MAX_RASTER_BOUNDARY_EDGES = 1'048'576;
constexpr std::size_t MAX_RECOVERY_DETAILS = 16;

/**
 * Work accounting shared by every validity pass (document doubles and the
 * exact f32 conversion used by polygon_to_simple). All limits are checked
 * before growing a collection or evaluating the next candidate pair.
 */
struct ValidationBudget
{
    std::size_t vertices = 0;
    std::size_t rings = 0;
    std::uint64_t pair_candidates = 0;
    bool exceeded = false;

    [[nodiscard]] bool charge_vertices(std::size_t count)
    {
        if (exceeded)
            return false;
        vertices += count;
        if (vertices > MAX_PART_VALIDATED_VERTICES) {
            exceeded = true;
            return false;
        }
        return true;
    }
    [[nodiscard]] bool charge_ring()
    {
        if (exceeded)
            return false;
        if (++rings > MAX_PART_VALIDATED_RINGS) {
            exceeded = true;
            return false;
        }
        return true;
    }
    [[nodiscard]] bool charge_pair()
    {
        if (exceeded)
            return false;
        if (++pair_candidates > MAX_VALIDATION_PAIR_CANDIDATES) {
            exceeded = true;
            return false;
        }
        return true;
    }
    /**
     * Non-consuming structural peek. Callers that pre-check a collection before
     * building it must not also charge it: the shared validity gate charges the
     * emitted geometry exactly once, and double charging would silently halve
     * the advertised vertex/ring caps.
     */
    [[nodiscard]] bool rings_fit(std::size_t additional) const
    {
        return !exceeded && rings + additional <= MAX_PART_VALIDATED_RINGS;
    }
};

/**
 * Budgeted point-in-polygon. Every visited edge is charged as one candidate
 * pair, so containment checks against a large outer ring cannot bypass the
 * part work cap. A null budget means the caller already bounded the polygon.
 */
enum class Containment
{
    Inside,
    Outside,
    TooComplex,
};

Containment point_in_polygon_bounded(Point const &point, std::span<Point const> polygon, ValidationBudget *budget)
{
    bool inside = false;
    for (std::size_t current = 0, previous = polygon.size() - 1; current < polygon.size(); previous = current++) {
        if (budget && !budget->charge_pair())
            return Containment::TooComplex;
        auto const &a = polygon[current];
        auto const &b = polygon[previous];
        auto const crosses = (a.y > point.y) != (b.y > point.y);
        if (!crosses)
            continue;
        auto const x = (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x;
        if (point.x < x)
            inside = !inside;
    }
    return inside ? Containment::Inside : Containment::Outside;
}

enum class GeometryValidity
{
    Valid,
    Invalid,
    TooComplex,
};

struct ValidationEdge
{
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;
    std::size_t ring = 0;
    std::size_t index = 0;
};

bool same_point(Point const &left, Point const &right)
{
    return left.x == right.x && left.y == right.y;
}

double orientation(Point const &a, Point const &b, Point const &c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool point_on_segment(Point const &point, Point const &start, Point const &end)
{
    if (orientation(start, end, point) != 0.0)
        return false;
    return std::min(start.x, end.x) <= point.x && point.x <= std::max(start.x, end.x) &&
           std::min(start.y, end.y) <= point.y && point.y <= std::max(start.y, end.y);
}

/** True for proper crossings, endpoint touches and collinear overlaps. */
bool segments_conflict(Point const &a, Point const &b, Point const &c, Point const &d)
{
    auto const ab_c = orientation(a, b, c);
    auto const ab_d = orientation(a, b, d);
    auto const cd_a = orientation(c, d, a);
    auto const cd_b = orientation(c, d, b);
    if (((ab_c > 0.0 && ab_d < 0.0) || (ab_c < 0.0 && ab_d > 0.0)) &&
        ((cd_a > 0.0 && cd_b < 0.0) || (cd_a < 0.0 && cd_b > 0.0))) {
        return true;
    }
    return (ab_c == 0.0 && point_on_segment(c, a, b)) || (ab_d == 0.0 && point_on_segment(d, a, b)) ||
           (cd_a == 0.0 && point_on_segment(a, c, d)) || (cd_b == 0.0 && point_on_segment(b, c, d));
}

/**
 * Validate one component in a single coordinate space: finite coordinates,
 * at least three distinct vertices, nonzero finite area, no zero-length edges,
 * no repeated (non-adjacent) vertices, no non-adjacent edge crossings, touches
 * or overlaps, and hole containment. Edge pairs are swept by bounding-box
 * minimum x with an active set; candidates behind the sweep or with disjoint y
 * bounds are rejected (and counted) before any intersection predicate.
 */
GeometryValidity validate_component_once(CollisionComponent const &component, ValidationBudget &budget,
                                         bool charge_structure)
{
    // Structural limits are checked before any allocation and charged once per
    // geometry: the f32 pass and ancestor revalidation must not halve the
    // advertised vertex/ring caps.
    std::size_t const total_rings = 1 + component.holes.size();
    if (total_rings > MAX_PART_VALIDATED_RINGS)
        return GeometryValidity::TooComplex;
    std::size_t total_vertices = component.outer.size();
    for (auto const &hole : component.holes) {
        if (hole.size() > MAX_PART_VALIDATED_VERTICES - std::min(total_vertices, MAX_PART_VALIDATED_VERTICES))
            return GeometryValidity::TooComplex;
        total_vertices += hole.size();
    }
    if (total_vertices > MAX_PART_VALIDATED_VERTICES)
        return GeometryValidity::TooComplex;
    if (charge_structure) {
        if (!budget.charge_vertices(total_vertices))
            return GeometryValidity::TooComplex;
        for (std::size_t ring = 0; ring < total_rings; ++ring) {
            if (!budget.charge_ring())
                return GeometryValidity::TooComplex;
        }
    }

    std::vector<std::span<Point const>> rings;
    rings.reserve(total_rings);
    rings.emplace_back(component.outer);
    for (auto const &hole : component.holes) {
        rings.emplace_back(hole);
    }

    std::vector<ValidationEdge> edges;
    for (std::size_t ring_index = 0; ring_index < rings.size(); ++ring_index) {
        auto const ring = rings[ring_index];
        if (ring.size() < 3)
            return GeometryValidity::Invalid;
        for (auto const &point : ring) {
            if (!finite(point))
                return GeometryValidity::Invalid;
        }
        auto const area = signed_area(ring);
        if (!std::isfinite(area) || area == 0.0)
            return GeometryValidity::Invalid;
        {
            // Adjacent duplicates are zero-length edges; any other duplicate is
            // a repeated non-adjacent vertex. Both are rejected.
            std::vector<std::pair<double, double>> sorted;
            sorted.reserve(ring.size());
            for (auto const &point : ring) {
                sorted.emplace_back(point.x, point.y);
            }
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
                return GeometryValidity::Invalid;
        }
        edges.reserve(edges.size() + ring.size());
        for (std::size_t index = 0; index < ring.size(); ++index) {
            auto const &a = ring[index];
            auto const &b = ring[(index + 1) % ring.size()];
            edges.push_back({std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.y, b.y), std::max(a.y, b.y),
                             ring_index, index});
        }
    }

    // Hole relationships. The sweep below proves the rings do not cross or
    // touch, so one sample vertex per hole decides containment. Every visited
    // outer/hole edge is charged, so thousands of holes against a large outer
    // ring cannot bypass the pair budget.
    for (std::size_t hole = 1; hole < rings.size(); ++hole) {
        auto const inside_outer = point_in_polygon_bounded(rings[hole].front(), rings[0], &budget);
        if (inside_outer == Containment::TooComplex)
            return GeometryValidity::TooComplex;
        if (inside_outer != Containment::Inside)
            return GeometryValidity::Invalid;
        for (std::size_t other = hole + 1; other < rings.size(); ++other) {
            auto const in_other = point_in_polygon_bounded(rings[hole].front(), rings[other], &budget);
            if (in_other == Containment::TooComplex)
                return GeometryValidity::TooComplex;
            auto const in_hole = point_in_polygon_bounded(rings[other].front(), rings[hole], &budget);
            if (in_hole == Containment::TooComplex)
                return GeometryValidity::TooComplex;
            if (in_other == Containment::Inside || in_hole == Containment::Inside)
                return GeometryValidity::Invalid;
        }
    }

    std::vector<std::size_t> order;
    order.reserve(edges.size());
    for (std::size_t index = 0; index < edges.size(); ++index) {
        order.push_back(index);
    }
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        if (edges[left].min_x != edges[right].min_x)
            return edges[left].min_x < edges[right].min_x;
        return left < right;
    });

    std::vector<std::size_t> active;
    for (auto const edge_index : order) {
        auto const &edge = edges[edge_index];
        std::erase_if(active, [&](std::size_t candidate) { return edges[candidate].max_x < edge.min_x; });
        auto const edge_ring = rings[edge.ring];
        for (auto const candidate : active) {
            auto const &other = edges[candidate];
            if (edge.ring == other.ring) {
                auto const count = edge_ring.size();
                if ((edge.index + 1) % count == other.index || (other.index + 1) % count == edge.index) {
                    continue; // adjacent edges share their endpoint by construction
                }
            }
            if (!budget.charge_pair())
                return GeometryValidity::TooComplex;
            if (edge.min_y > other.max_y || other.min_y > edge.max_y)
                continue; // y-rejected candidate; already counted
            auto const other_ring = rings[other.ring];
            if (segments_conflict(edge_ring[edge.index], edge_ring[(edge.index + 1) % edge_ring.size()],
                                  other_ring[other.index], other_ring[(other.index + 1) % other_ring.size()])) {
                return GeometryValidity::Invalid;
            }
        }
        active.push_back(edge_index);
    }
    return GeometryValidity::Valid;
}

CollisionComponent rounded_to_float(CollisionComponent const &component)
{
    CollisionComponent rounded;
    auto convert = [](std::span<Point const> source, std::vector<Point> &target) {
        target.reserve(source.size());
        for (auto const &point : source) {
            target.push_back({static_cast<double>(static_cast<float>(point.x)),
                              static_cast<double>(static_cast<float>(point.y))});
        }
    };
    convert(component.outer, rounded.outer);
    rounded.holes.resize(component.holes.size());
    for (std::size_t index = 0; index < component.holes.size(); ++index) {
        convert(component.holes[index], rounded.holes[index]);
    }
    return rounded;
}

/** Double pass plus the exact f32 conversion used by polygon_to_simple. */
GeometryValidity validate_component(CollisionComponent const &component, ValidationBudget &budget,
                                    bool charge_structure = true)
{
    auto const as_double = validate_component_once(component, budget, charge_structure);
    if (as_double != GeometryValidity::Valid)
        return as_double;
    auto const as_float = rounded_to_float(component);
    return validate_component_once(as_float, budget, false);
}

GeometryValidity validate_part_components(PartGeometry const &part, ValidationBudget &budget,
                                          bool charge_structure = true)
{
    if (part.components.empty())
        return GeometryValidity::Invalid;
    for (auto const &component : part.components) {
        auto const validity = validate_component(component, budget, charge_structure);
        if (validity != GeometryValidity::Valid)
            return validity;
    }
    return GeometryValidity::Valid;
}

/** Rings/vertices contributed by one component, used for aggregate caps. */
struct StructureCount
{
    std::size_t rings = 0;
    std::size_t vertices = 0;
};

StructureCount structure_count(CollisionComponent const &component)
{
    StructureCount count;
    count.rings = 1 + component.holes.size();
    count.vertices = component.outer.size();
    for (auto const &hole : component.holes)
        count.vertices += hole.size();
    return count;
}

std::optional<double> outward_float_lower(double value)
{
    if (!std::isfinite(value))
        return std::nullopt;
    auto rounded = static_cast<float>(value);
    if (!std::isfinite(rounded))
        return std::nullopt;
    if (static_cast<double>(rounded) > value)
        rounded = std::nextafterf(rounded, -std::numeric_limits<float>::infinity());
    return static_cast<double>(rounded);
}

std::optional<double> outward_float_upper(double value)
{
    if (!std::isfinite(value))
        return std::nullopt;
    auto rounded = static_cast<float>(value);
    if (!std::isfinite(rounded))
        return std::nullopt;
    if (static_cast<double>(rounded) < value)
        rounded = std::nextafterf(rounded, std::numeric_limits<float>::infinity());
    return static_cast<double>(rounded);
}

/**
 * Complete rendered visual extent, rounded outward to f32, validated by a
 * constant-size check that cannot be disabled by main-budget exhaustion.
 * Never derived from surviving collision components. Used for parts only.
 */
std::optional<PolygonGeometry> conservative_bounds_geometry(Geom::OptRect const &bounds, std::string &reason)
{
    if (!bounds || bounds->hasZeroArea()) {
        reason = "rendered visual bounds are empty or unavailable";
        return std::nullopt;
    }
    auto const left = outward_float_lower(bounds->left());
    auto const top = outward_float_lower(bounds->top());
    auto const right = outward_float_upper(bounds->right());
    auto const bottom = outward_float_upper(bounds->bottom());
    if (!left || !top || !right || !bottom) {
        reason = "rendered visual bounds are not representable as finite coordinates";
        return std::nullopt;
    }
    if (!(*left < *right) || !(*top < *bottom)) {
        reason = "rendered visual bounds degenerate after outward rounding";
        return std::nullopt;
    }
    PolygonGeometry geometry;
    geometry.outer = {{*left, *top}, {*right, *top}, {*right, *bottom}, {*left, *bottom}};
    geometry.source = ContourSource::ConservativeBounds;
    ValidationBudget constant_budget;
    CollisionComponent component{.outer = geometry.outer};
    if (validate_component(component, constant_budget) != GeometryValidity::Valid) {
        reason = "rendered visual bounds failed the constant-size validity check";
        return std::nullopt;
    }
    return geometry;
}

std::string object_label(SPItem const *item)
{
    if (!item)
        return "unnamed object";
    if (auto const *label = item->label(); label && *label)
        return label;
    if (auto const *id = item->getId(); id && *id)
        return id;
    return "unnamed object";
}

void hash_byte(std::uint64_t &hash, std::uint8_t byte)
{
    hash ^= byte;
    hash *= FNV_PRIME;
}

void hash_u64(std::uint64_t &hash, std::uint64_t word)
{
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash_byte(hash, static_cast<std::uint8_t>((word >> (byte * 8)) & 0xffU));
    }
}

/**
 * Word-at-a-time mixing for long byte runs: same bytes, same order, two
 * multiplies per 8 bytes. Each step is a bijection of the hash state for a
 * given word, so any single-word edit always changes the result, like
 * byte-wise FNV. The second multiply makes the state change caused by an edit
 * depend on the content: with a single multiply and shift, flipping bit 63 of
 * one word produced a fixed delta that an edit of the next word could cancel.
 * Not std::hash<std::string_view>: MSVC implements that as byte-wise FNV.
 */
void hash_bulk(std::uint64_t &hash, void const *data, std::size_t size)
{
    auto const *bytes = static_cast<unsigned char const *>(data);
    hash_u64(hash, size);
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t word;
        std::memcpy(&word, bytes + i, 8);
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ULL;
        hash ^= hash >> 32;
        hash *= 0xd6e8feb86659fd93ULL;
    }
    for (; i < size; ++i)
        hash_byte(hash, bytes[i]);
}

void hash_text(std::uint64_t &hash, std::string_view text)
{
    if (text.size() > 64) {
        hash_bulk(hash, text.data(), text.size());
    } else {
        for (auto const byte : text)
            hash_byte(hash, static_cast<unsigned char>(byte));
    }
    hash_u64(hash, 1);
}

void hash_text(std::uint64_t &hash, char const *text)
{
    if (!text) {
        hash_u64(hash, 0);
        return;
    }
    hash_text(hash, std::string_view{text});
}

/**
 * Alpha byte of one pixel of a representation-safe Pixbuf accessor row.
 *
 * GdkPixbuf stores bytes R, G, B, A regardless of endianness. Cairo stores a
 * native-endian 32-bit value 0xAARRGGBB (Inkscape's documented layout), so the
 * alpha byte is extracted from the high bits instead of assuming a byte order;
 * a byte-order assumption here would silently read red or blue on big-endian.
 */
std::uint8_t pixel_alpha(Inkscape::Pixbuf const &pixels, guchar const *row, int x)
{
    auto const offset = static_cast<std::ptrdiff_t>(x) * 4;
    if (pixels.pixelFormat() == Inkscape::Pixbuf::PF_CAIRO) {
        std::uint32_t value = 0;
        std::memcpy(&value, row + offset, sizeof(value));
        return static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    }
    return row[offset + 3];
}

/**
 * Representation-independent content hash over decoded bitmap data: dimensions
 * plus the alpha channel. The nesting trace, the rendered silhouette and the
 * part size all depend on alpha, never on colour; hashing raw bytes made the
 * same unchanged image change freshness merely because GUI rendering converted
 * GdkPixbuf RGBA to premultiplied Cairo BGRA/ARGB (a lossy colour round trip
 * that must not be mistaken for a source edit). A pixbuf address alone is not a
 * reliable identity either because pixel data can change in place.
 */
std::uint64_t pixbuf_content_hash(Inkscape::Pixbuf const &pixels)
{
    auto const width = pixels.width();
    auto const height = pixels.height();
    auto const *data = pixels.pixels();
    auto hash = FNV_OFFSET;
    hash_u64(hash, static_cast<std::uint64_t>(width));
    hash_u64(hash, static_cast<std::uint64_t>(height));
    if (data && width > 0 && height > 0) {
        // Rowstride is a layout detail, not content, and is deliberately not
        // hashed; only the alpha values and dimensions define the silhouette.
        auto const rowstride = pixels.rowstride();
        std::vector<std::uint8_t> alpha(static_cast<std::size_t>(width));
        for (int y = 0; y < height; ++y) {
            auto const *row = data + static_cast<std::ptrdiff_t>(y) * rowstride;
            for (int x = 0; x < width; ++x) {
                alpha[static_cast<std::size_t>(x)] = pixel_alpha(pixels, row, x);
            }
            hash_bulk(hash, alpha.data(), alpha.size());
        }
    }
    return hash;
}

/**
 * Pixel hashes for one synchronous GTK-thread call (R4). Never stored across
 * calls: pixbufs can be edited in place at the same address.
 */
struct PixelHashMemo
{
    std::unordered_map<Inkscape::Pixbuf const *, std::uint64_t> values;

    std::uint64_t get(Inkscape::Pixbuf const &pixels)
    {
        auto [entry, inserted] = values.try_emplace(&pixels, 0);
        if (inserted)
            entry->second = pixbuf_content_hash(pixels);
        return entry->second;
    }
};

std::uint64_t pixel_hash(Inkscape::Pixbuf const &pixels, PixelHashMemo *memo)
{
    return memo ? memo->get(pixels) : pixbuf_content_hash(pixels);
}

std::optional<Point> interior_sample(Contour const &contour, double tolerance, ValidationBudget *budget)
{
    auto const &points = contour.points;
    std::size_t longest = 0;
    double longest_length = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        auto const length = squared_distance(points[index], points[(index + 1) % points.size()]);
        if (length > longest_length) {
            longest = index;
            longest_length = length;
        }
    }
    if (longest_length <= std::numeric_limits<double>::min())
        return std::nullopt;

    auto const &start = points[longest];
    auto const &end = points[(longest + 1) % points.size()];
    auto const midpoint = Point{(start.x + end.x) * 0.5, (start.y + end.y) * 0.5};
    auto const inverse_length = 1.0 / std::sqrt(longest_length);
    auto const normal = Point{-(end.y - start.y) * inverse_length, (end.x - start.x) * inverse_length};
    auto distance = std::max(tolerance * 0.25, std::sqrt(longest_length) * 1.0e-8);
    for (unsigned attempt = 0; attempt < 24; ++attempt) {
        for (double direction : {1.0, -1.0}) {
            Point candidate{midpoint.x + normal.x * distance * direction, midpoint.y + normal.y * distance * direction};
            auto const containment = point_in_polygon_bounded(candidate, points, budget);
            if (containment == Containment::TooComplex)
                return std::nullopt;
            if (containment == Containment::Inside)
                return candidate;
        }
        distance *= 0.5;
    }
    return std::nullopt;
}

/**
 * Classify contour nesting. Every candidate contour pair and every visited
 * polygon edge is charged to the caller's budget, so a many-contour path or
 * bitmap cannot trigger an unbounded all-pairs pass before the recovery gate.
 */
bool classify_contours(std::vector<Contour> &contours, double tolerance, ValidationBudget *budget, bool &too_complex)
{
    too_complex = false;
    std::vector<Point> samples;
    samples.reserve(contours.size());
    for (auto const &contour : contours) {
        auto sample = interior_sample(contour, tolerance, budget);
        if (!sample) {
            if (budget && budget->exceeded) {
                too_complex = true;
            }
            return false;
        }
        samples.emplace_back(*sample);
    }

    for (std::size_t child = 0; child < contours.size(); ++child) {
        auto parent_area = std::numeric_limits<double>::infinity();
        for (std::size_t candidate = 0; candidate < contours.size(); ++candidate) {
            if (child == candidate || contours[candidate].absolute_area <= contours[child].absolute_area)
                continue;
            if (budget && !budget->charge_pair()) {
                too_complex = true;
                return false;
            }
            if (contours[candidate].absolute_area < parent_area) {
                auto const containment =
                    point_in_polygon_bounded(samples[child], contours[candidate].points, budget);
                if (containment == Containment::TooComplex) {
                    too_complex = true;
                    return false;
                }
                if (containment == Containment::Inside) {
                    contours[child].parent = static_cast<int>(candidate);
                    parent_area = contours[candidate].absolute_area;
                }
            }
        }
    }

    for (std::size_t index = 0; index < contours.size(); ++index) {
        auto parent = contours[index].parent;
        unsigned depth = 0;
        std::size_t guard = 0;
        while (parent >= 0) {
            ++depth;
            parent = contours[parent].parent;
            if (++guard > contours.size())
                return false;
        }
        contours[index].depth = depth;
    }
    return true;
}

void orient_counterclockwise(std::vector<Point> &polygon)
{
    if (signed_area(polygon) < 0.0)
        std::reverse(polygon.begin(), polygon.end());
}

int edge_direction(PixelVertex const &start, PixelVertex const &end)
{
    if (end.x > start.x)
        return 0;
    if (end.y > start.y)
        return 1;
    if (end.x < start.x)
        return 2;
    return 3;
}

std::optional<AlphaTraceResult> trace_alpha_contours(AlphaPlane const &plane, bool &too_complex)
{
    too_complex = false;
    // The alpha plane was copied through pixel_alpha() during capture
    // (representation safe: GdkPixbuf or premultiplied Cairo).
    auto const width = plane.width;
    auto const height = plane.height;
    if (width <= 0 || height <= 0 || plane.alpha.size() != static_cast<std::size_t>(width) * height)
        return std::nullopt;

    // Bound discovery memory and complexity. Any covered source pixel makes a
    // sample cell opaque, so downsampling remains conservative.
    constexpr int max_trace_dimension = 2048;
    constexpr unsigned alpha_threshold = 8;
    auto const sample_step = std::max(1, (std::max(width, height) + max_trace_dimension - 1) / max_trace_dimension);
    auto const sample_width = (width + sample_step - 1) / sample_step;
    auto const sample_height = (height + sample_step - 1) / sample_step;
    std::vector<std::uint8_t> occupied(static_cast<std::size_t>(sample_width) * sample_height, 0);
    for (int sy = 0; sy < sample_height; ++sy) {
        for (int sx = 0; sx < sample_width; ++sx) {
            bool present = false;
            auto const y_end = std::min(height, (sy + 1) * sample_step);
            auto const x_end = std::min(width, (sx + 1) * sample_step);
            for (int y = sy * sample_step; y < y_end && !present; ++y) {
                auto const *row = plane.alpha.data() + static_cast<std::ptrdiff_t>(y) * width;
                for (int x = sx * sample_step; x < x_end; ++x) {
                    if (row[x] >= alpha_threshold) {
                        present = true;
                        break;
                    }
                }
            }
            occupied[static_cast<std::size_t>(sy) * sample_width + sx] = present;
        }
    }

    auto filled = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < sample_width && y < sample_height &&
               occupied[static_cast<std::size_t>(y) * sample_width + x] != 0;
    };
    std::unordered_map<PixelVertex, std::vector<PixelVertex>, PixelVertexHash> edges;
    // Deterministic cursor of non-empty edge starts. Starts are discovered in
    // scan order and consumed smallest-first so the walk order (and therefore
    // any split of a repeated-vertex walk) never depends on hash iteration.
    std::vector<PixelVertex> starts;
    std::size_t emitted_edges = 0;
    auto add_edge = [&](PixelVertex start, PixelVertex end) {
        if (emitted_edges >= MAX_RASTER_BOUNDARY_EDGES) {
            too_complex = true;
            return;
        }
        ++emitted_edges;
        auto &outgoing = edges[start];
        if (outgoing.empty())
            starts.push_back(start);
        outgoing.push_back(end);
    };
    for (int y = 0; y < sample_height; ++y) {
        for (int x = 0; x < sample_width; ++x) {
            if (!filled(x, y))
                continue;
            if (!filled(x, y - 1)) add_edge({x, y}, {x + 1, y});
            if (!filled(x + 1, y)) add_edge({x + 1, y}, {x + 1, y + 1});
            if (!filled(x, y + 1)) add_edge({x + 1, y + 1}, {x, y + 1});
            if (!filled(x - 1, y)) add_edge({x, y + 1}, {x, y});
        }
    }
    if (too_complex)
        return std::nullopt;
    if (edges.empty())
        return AlphaTraceResult{};

    // Integer-grid cycles. Every emitted ring is rotated to its lexicographically
    // smallest vertex so the stored order is canonical (repeated captures and
    // the shared pixel-space cache produce byte-identical geometry).
    struct RasterCycle
    {
        std::vector<Point> points;
        double signed_area = 0.0;
        double absolute_area = 0.0;
    };
    std::vector<RasterCycle> cycles;
    // Emitted-cycle and vertex caps are checked before any ring is copied into
    // its own vector, so a comb boundary cannot allocate past the advertised
    // raster budget and only then fail a later gate.
    std::size_t emitted_cycle_vertices = 0;

    auto emit_cycle = [&](std::vector<Point> &points) {
        if (cycles.size() >= MAX_PART_VALIDATED_RINGS || emitted_cycle_vertices + points.size() > MAX_CONTOUR_POINTS) {
            too_complex = true;
            return false;
        }
        remove_redundant_points(points);
        if (points.size() < 3)
            return false;
        auto const area = signed_area(points);
        if (!std::isfinite(area) || area == 0.0)
            return false;
        auto const smallest = std::min_element(points.begin(), points.end(), [](Point const &left, Point const &right) {
            return left.x < right.x || (left.x == right.x && left.y < right.y);
        });
        std::rotate(points.begin(), smallest, points.end());
        emitted_cycle_vertices += points.size();
        cycles.push_back({.points = std::move(points), .signed_area = area, .absolute_area = std::abs(area)});
        return true;
    };

    std::sort(starts.begin(), starts.end(), [](PixelVertex const &left, PixelVertex const &right) {
        return right.x < left.x || (right.x == left.x && right.y < left.y);
    });
    while (!starts.empty()) {
        auto const start = starts.back();
        starts.pop_back();
        auto first_edge = edges.find(start);
        if (first_edge == edges.end() || first_edge->second.empty())
            continue;
        auto current = start;
        int incoming = -1;
        std::vector<Point> walk;
        walk.reserve(256);
        // walk_vertices mirrors walk so a split erases only the removed suffix
        // from visit_index. Scanning the whole map at every split was quadratic
        // on comb boundaries and could bypass the work budget.
        std::vector<PixelVertex> walk_vertices;
        walk_vertices.reserve(256);
        std::unordered_map<PixelVertex, std::size_t, PixelVertexHash> visit_index;
        for (std::size_t guard = 0; guard <= MAX_CONTOUR_POINTS; ++guard) {
            auto const seen = visit_index.find(current);
            if (seen != visit_index.end()) {
                // A walk revisiting a non-start vertex is split into exact simple
                // cycles: the closed sub-walk is emitted and the walk continues
                // from the revisited vertex. No boundary edge or occupied pixel
                // is dropped; an unusable split is a diagnosed fallback.
                std::size_t const cut = seen->second;
                if (walk.size() - cut < 3)
                    return std::nullopt;
                if (cycles.size() >= MAX_PART_VALIDATED_RINGS ||
                    emitted_cycle_vertices + (walk.size() - cut) > MAX_CONTOUR_POINTS) {
                    too_complex = true;
                    return std::nullopt;
                }
                std::vector<Point> closed(walk.begin() + static_cast<std::ptrdiff_t>(cut), walk.end());
                if (!emit_cycle(closed))
                    return std::nullopt;
                for (std::size_t index = cut + 1; index < walk_vertices.size(); ++index)
                    visit_index.erase(walk_vertices[index]);
                walk_vertices.resize(cut + 1);
                walk.resize(cut + 1);
            } else {
                if (walk.size() >= MAX_CONTOUR_POINTS) {
                    too_complex = true;
                    return std::nullopt;
                }
                visit_index.emplace(current, walk.size());
                walk_vertices.push_back(current);
                walk.push_back(
                    {static_cast<double>(current.x * sample_step), static_cast<double>(current.y * sample_step)});
            }
            auto edge = edges.find(current);
            if (edge == edges.end() || edge->second.empty()) {
                if (walk.size() >= 3)
                    return std::nullopt; // an open remainder is a trace defect
                break;
            }
            auto &ends = edge->second;
            std::size_t selected = 0;
            if (incoming >= 0 && ends.size() > 1) {
                auto rank = [&](PixelVertex const &end) {
                    auto const turn = (edge_direction(current, end) - incoming + 4) % 4;
                    // Clockwise contours keep the occupied cell on the right:
                    // prefer right, straight, left, then reversal at diagonal touches.
                    return turn == 1 ? 0 : turn == 0 ? 1 : turn == 3 ? 2 : 3;
                };
                for (std::size_t index = 1; index < ends.size(); ++index) {
                    if (rank(ends[index]) < rank(ends[selected]))
                        selected = index;
                }
            }
            auto const next = ends[selected];
            ends.erase(ends.begin() + selected);
            if (ends.empty())
                edges.erase(edge);
            incoming = edge_direction(current, next);
            current = next;
            if (guard == MAX_CONTOUR_POINTS)
                return std::nullopt;
        }
    }
    if (cycles.empty())
        return AlphaTraceResult{};

    // Sign-based classification on the integer grid: with the occupied cell on
    // the right, an outer ring has positive signed area and a hole negative.
    // interior_sample()/point_in_polygon() classification is not used here: it
    // can mis-sample a one-cell-wide hole even though the winding is exact.
    std::vector<std::size_t> outers;
    std::vector<std::size_t> holes;
    for (std::size_t index = 0; index < cycles.size(); ++index) {
        (cycles[index].signed_area > 0.0 ? outers : holes).push_back(index);
    }
    if (outers.empty())
        return std::nullopt;

    // A raster hole that shares an exact vertex with an outer ring is a pinch
    // (kissing hole). Emitting it as a hole would touch its outer boundary, so
    // the whole trace is diagnosed for conservative recovery instead.
    std::map<std::pair<double, double>, bool> outer_vertices;
    for (auto const outer : outers) {
        for (auto const &point : cycles[outer].points)
            outer_vertices.emplace(std::make_pair(point.x, point.y), true);
    }
    for (auto const hole : holes) {
        for (auto const &point : cycles[hole].points) {
            if (outer_vertices.find(std::make_pair(point.x, point.y)) != outer_vertices.end())
                return std::nullopt;
        }
    }

    auto ring_less = [](std::vector<Point> const &left, std::vector<Point> const &right) {
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                                            [](Point const &a, Point const &b) {
                                                return a.x < b.x || (a.x == b.x && a.y < b.y);
                                            });
    };
    std::vector<std::size_t> sorted_outers = outers;
    std::sort(sorted_outers.begin(), sorted_outers.end(), [&](std::size_t left, std::size_t right) {
        if (cycles[left].absolute_area != cycles[right].absolute_area)
            return cycles[left].absolute_area < cycles[right].absolute_area;
        return ring_less(cycles[left].points, cycles[right].points);
    });

    ValidationBudget classify_budget;
    std::vector<std::vector<std::size_t>> holes_of_outer(sorted_outers.size());
    for (auto const hole : holes) {
        bool assigned = false;
        for (std::size_t position = 0; position < sorted_outers.size() && !assigned; ++position) {
            auto const candidate = sorted_outers[position];
            auto const containment =
                point_in_polygon_bounded(cycles[hole].points.front(), cycles[candidate].points, &classify_budget);
            if (containment == Containment::TooComplex) {
                too_complex = true;
                return std::nullopt;
            }
            if (containment == Containment::Inside) {
                // Smallest-area containing outer: sorted_outers is area-ascending.
                holes_of_outer[position].push_back(hole);
                assigned = true;
            }
        }
        if (!assigned)
            return std::nullopt; // a hole outside every outer is ambiguous topology
    }

    std::vector<CollisionComponent> components;
    components.reserve(sorted_outers.size());
    for (std::size_t position = 0; position < sorted_outers.size(); ++position) {
        auto &cycle = cycles[sorted_outers[position]];
        orient_counterclockwise(cycle.points);
        auto smallest = std::min_element(cycle.points.begin(), cycle.points.end(), [](Point const &left, Point const &right) {
            return left.x < right.x || (left.x == right.x && left.y < right.y);
        });
        std::rotate(cycle.points.begin(), smallest, cycle.points.end());
        CollisionComponent component{.outer = std::move(cycle.points)};
        auto assigned_holes = holes_of_outer[position];
        std::sort(assigned_holes.begin(), assigned_holes.end(), [&](std::size_t left, std::size_t right) {
            return ring_less(cycles[left].points, cycles[right].points);
        });
        for (auto const hole : assigned_holes) {
            orient_counterclockwise(cycles[hole].points);
            auto hole_smallest =
                std::min_element(cycles[hole].points.begin(), cycles[hole].points.end(), [](Point const &left, Point const &right) {
                    return left.x < right.x || (left.x == right.x && left.y < right.y);
                });
            std::rotate(cycles[hole].points.begin(), hole_smallest, cycles[hole].points.end());
            component.holes.push_back(std::move(cycles[hole].points));
        }
        components.push_back(std::move(component));
    }
    std::sort(components.begin(), components.end(),
              [&](CollisionComponent const &left, CollisionComponent const &right) {
                  if (ring_less(left.outer, right.outer))
                      return true;
                  if (ring_less(right.outer, left.outer))
                      return false;
                  return left.holes.size() < right.holes.size();
              });
    return AlphaTraceResult{.components = std::move(components)};
}

/// The traced boundary for these pixels: from the shared cache, or traced now
/// and cached. Validation runs under its own constant-size budget so one
/// caller's exhausted part budget never poisons the shared entry.
std::shared_ptr<AlphaTrace const> trace_alpha_plane(AlphaPlane const &plane)
{
    AlphaTraceKey const key{plane.width, plane.height, plane.content_hash};
    if (auto cached = cached_alpha_trace(key))
        return cached;
    auto entry = std::make_shared<AlphaTrace>();
    bool too_complex = false;
    auto traced = trace_alpha_contours(plane, too_complex);
    if (!traced) {
        entry->too_complex = too_complex;
        entry->failure = too_complex ? "bitmap alpha boundary exceeds the raster edge budget"
                                     : "bitmap alpha boundary walk could not be traced";
    } else if (traced->components.empty()) {
        entry->failure = "bitmap is fully transparent";
    } else {
        // Validate in pixel space before storing reusable cache geometry.
        PartGeometry pixel_geometry{.components = traced->components};
        ValidationBudget pixel_budget;
        auto const pixel_validity = validate_part_components(pixel_geometry, pixel_budget);
        if (pixel_validity != GeometryValidity::Valid) {
            entry->too_complex = pixel_validity == GeometryValidity::TooComplex;
            entry->failure = pixel_validity == GeometryValidity::TooComplex
                                 ? "bitmap alpha boundary exceeds the validation budget"
                                 : "bitmap alpha boundary is self-touching or degenerate";
        } else {
            entry->traced = true;
            entry->components = std::move(traced->components);
        }
    }
    store_alpha_trace(key, entry);
    return entry;
}

PartGeometryResult prepare_bitmap_alpha_geometry(CapturedLeaf const &leaf, ValidationBudget &budget)
{
    if (!leaf.pixels)
        return {.error = "bitmap pixels are unavailable"};
    if (!leaf.pixel_to_document)
        return {.error = "bitmap pixel transform is singular"};
    static AlphaPlane const no_pixels;
    auto const entry = leaf.trace ? leaf.trace : trace_alpha_plane(leaf.alpha ? *leaf.alpha : no_pixels);
    if (!entry->traced) {
        // Cached failure: deterministic, never presented as a valid trace.
        return {.error = entry->failure.empty() ? std::string{"bitmap alpha boundary is unusable"} : entry->failure,
                .too_complex = entry->too_complex};
    }

    PartGeometry result;
    result.source = ContourSource::BitmapAlpha;
    result.components = entry->components;
    auto const &affine = *leaf.pixel_to_document;
    auto transform_points = [&](std::vector<Point> &points) {
        for (auto &point : points) {
            auto const transformed = Geom::Point(point.x, point.y) * affine;
            point = {transformed[Geom::X], transformed[Geom::Y]};
        }
    };
    for (auto &component : result.components) {
        transform_points(component.outer);
        for (auto &hole : component.holes)
            transform_points(hole);
    }
    // Revalidate after pixelToDocumentAffine(); a cached shape can become
    // invalid after a large translation or scaling. This failure is reported
    // for the current call only: the reusable pixel-space trace stays valid
    // and is never evicted, so correcting the transform restores the exact
    // trace instead of permanently downgrading the part.
    auto const document_validity = validate_part_components(result, budget);
    if (document_validity != GeometryValidity::Valid) {
        return {.error = document_validity == GeometryValidity::TooComplex
                            ? "transformed bitmap alpha boundary exceeds the validation budget"
                            : "transformed bitmap alpha boundary is self-touching or degenerate",
                .too_complex = document_validity == GeometryValidity::TooComplex};
    }
    return {.geometry = std::move(result)};
}

std::vector<Point> convex_hull(std::vector<Point> points, double tolerance)
{
    auto const near = [tolerance](Point const &left, Point const &right) {
        return squared_distance(left, right) <= tolerance * tolerance;
    };
    std::sort(points.begin(), points.end(), [](Point const &left, Point const &right) {
        return left.x < right.x || (left.x == right.x && left.y < right.y);
    });
    points.erase(std::unique(points.begin(), points.end(), near), points.end());
    if (points.size() < 3)
        return {};

    auto const cross = [](Point const &origin, Point const &a, Point const &b) {
        return (a.x - origin.x) * (b.y - origin.y) - (a.y - origin.y) * (b.x - origin.x);
    };
    std::vector<Point> hull;
    hull.reserve(points.size() * 2);
    for (auto const &point : points) {
        while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.back(), point) <= tolerance * tolerance) {
            hull.pop_back();
        }
        hull.emplace_back(point);
    }
    auto const lower_size = hull.size();
    for (auto iterator = points.rbegin() + 1; iterator != points.rend(); ++iterator) {
        while (hull.size() > lower_size &&
               cross(hull[hull.size() - 2], hull.back(), *iterator) <= tolerance * tolerance) {
            hull.pop_back();
        }
        hull.emplace_back(*iterator);
    }
    if (!hull.empty())
        hull.pop_back();
    remove_redundant_points(hull);
    orient_counterclockwise(hull);
    return hull;
}

bool point_inside_or_near_contour(Point const &point, std::span<Point const> contour, double tolerance,
                                  ValidationBudget *budget, bool &too_complex)
{
    auto const containment = point_in_polygon_bounded(point, contour, budget);
    if (containment == Containment::TooComplex) {
        too_complex = true;
        return false;
    }
    if (containment == Containment::Inside)
        return true;

    auto const tolerance_squared = tolerance * tolerance;
    for (std::size_t index = 0; index < contour.size(); ++index) {
        if (budget && !budget->charge_pair()) {
            too_complex = true;
            return false;
        }
        auto const &start = contour[index];
        auto const &end = contour[(index + 1) % contour.size()];
        auto const dx = end.x - start.x;
        auto const dy = end.y - start.y;
        auto const length_squared = dx * dx + dy * dy;
        auto const projection = length_squared > 0.0
                                    ? std::clamp(((point.x - start.x) * dx + (point.y - start.y) * dy) /
                                                     length_squared,
                                                 0.0, 1.0)
                                    : 0.0;
        auto const nearest = Point{start.x + projection * dx, start.y + projection * dy};
        if (squared_distance(point, nearest) <= tolerance_squared)
            return true;
    }
    return false;
}

GeometryResult contours_from_path(Geom::PathVector const &input, SPWindRule wind_rule, double tolerance, bool container,
                                 ValidationBudget *budget)
{
    if (input.empty())
        return {.error = "item has no path geometry"};

    Geom::PathVector resolved;
    try {
        resolved = flattened(input, wind_rule == SP_WIND_RULE_EVENODD ? fill_oddEven : fill_nonZero);
    } catch (...) {
        return {.error = "item path could not be normalized"};
    }

    // Ring peek happens before reserve/construction: the shared validity gate
    // charges the emitted rings exactly once, but an over-ring input must fail
    // before any collection is allocated for it.
    if (budget && !budget->rings_fit(resolved.size()))
        return {.error = "item path has too many flattened contours to validate safely", .too_complex = true};

    std::vector<Contour> contours;
    contours.reserve(resolved.size());
    std::vector<Point> degenerate_points;
    // Non-consuming aggregate vertex peek. The shared validity gate still
    // charges the emitted geometry exactly once; this local counter bounds the
    // temporary contour storage while flattening so a many-subpath part cannot
    // collect up to MAX_PART_VALIDATED_RINGS * MAX_CONTOUR_POINTS vertices
    // before validate_component_once can charge them. The per-path buffer is
    // already bounded by MAX_CONTOUR_POINTS inside flatten_contour().
    std::size_t const allowance =
        budget ? MAX_PART_VALIDATED_VERTICES - std::min(budget->vertices, MAX_PART_VALIDATED_VERTICES)
               : MAX_PART_VALIDATED_VERTICES;
    std::size_t aggregate_vertices = 0;
    for (auto const &path : resolved) {
        bool degenerate = false;
        auto const degenerate_begin = degenerate_points.size();
        auto contour = flatten_contour(path, tolerance, container ? nullptr : &degenerate,
                                       container ? nullptr : &degenerate_points);
        if (!contour) {
            if (!container && degenerate) {
                // Keep degenerate vertices bounded and available to the
                // conservative part fallback after the usable contours are classified.
                auto const degenerate_count = degenerate_points.size() - degenerate_begin;
                if (degenerate_count > allowance - aggregate_vertices) {
                    return {.error = "item path aggregate geometry exceeds the validation budget", .too_complex = true};
                }
                aggregate_vertices += degenerate_count;
                continue;
            }
            return {.error = "item path contains a contour that could not be flattened safely"};
        }
        // Reject before storing the next contour, not after: aggregate_vertices
        // never exceeds allowance, so the subtraction stays in range.
        if (contour->points.size() > allowance - aggregate_vertices) {
            return {.error = "item path aggregate geometry exceeds the validation budget", .too_complex = true};
        }
        aggregate_vertices += contour->points.size();
        contours.emplace_back(std::move(*contour));
    }
    if (contours.empty() && degenerate_points.empty())
        return {.error = "item has no closed non-zero-area contour"};
    bool classify_too_complex = false;
    if (!classify_contours(contours, tolerance, budget, classify_too_complex)) {
        return {.error = classify_too_complex ? "item contour nesting exceeds the validation budget"
                                              : "item contour nesting could not be classified",
                .too_complex = classify_too_complex};
    }

    std::vector<std::size_t> outer_indices;
    for (std::size_t index = 0; index < contours.size(); ++index) {
        if (contours[index].depth == 0)
            outer_indices.emplace_back(index);
    }

    bool include_degenerate_points = false;
    for (auto const &point : degenerate_points) {
        bool contained = false;
        for (auto const index : outer_indices) {
            bool too_complex = false;
            if (point_inside_or_near_contour(point, contours[index].points, tolerance, budget, too_complex)) {
                contained = true;
                break;
            }
            if (too_complex) {
                return {.error = "item contour nesting exceeds the validation budget", .too_complex = true};
            }
        }
        if (!contained) {
            include_degenerate_points = true;
            break;
        }
    }

    if (container) {
        if (outer_indices.size() != 1) {
            return {.error = "container must have exactly one connected outer boundary"};
        }
        auto const outer_index = outer_indices.front();
        PolygonGeometry result;
        result.outer = contours[outer_index].points;
        orient_counterclockwise(result.outer);
        for (std::size_t index = 0; index < contours.size(); ++index) {
            if (contours[index].depth > 1) {
                return {.error = "container has nested islands that the current solver cannot represent"};
            }
            if (contours[index].depth == 1 && contours[index].parent == static_cast<int>(outer_index)) {
                auto hole = contours[index].points;
                orient_counterclockwise(hole);
                result.holes.emplace_back(std::move(hole));
            }
        }
        result.source = ContourSource::ExactVector;
        return {.geometry = std::move(result)};
    }

    PolygonGeometry result;
    if (outer_indices.size() == 1 && !include_degenerate_points) {
        result.outer = contours[outer_indices.front()].points;
        orient_counterclockwise(result.outer);
        result.source = contours.size() == 1 ? ContourSource::ExactVector : ContourSource::ConservativeHull;
    } else {
        std::vector<Point> points;
        for (auto const index : outer_indices) {
            points.insert(points.end(), contours[index].points.begin(), contours[index].points.end());
        }
        // Degenerate fragments are ignored when enclosed by a usable outer;
        // otherwise hulling their vertices with every usable outer is conservative.
        if (include_degenerate_points)
            points.insert(points.end(), degenerate_points.begin(), degenerate_points.end());
        // Keep every degenerate vertex inside the recovery hull; tolerance-based
        // near-point merging could otherwise trim the small fragment itself.
        result.outer = convex_hull(std::move(points), include_degenerate_points ? 0.0 : tolerance);
        result.source = ContourSource::ConservativeHull;
    }
    if (result.outer.size() < 3)
        return {.error = "item outline degenerates after normalization"};
    return {.geometry = std::move(result)};
}

Geom::PathVector part_path(SPItem *item)
{
    if (is<SPGroup>(item) || (item->style && item->style->getFilter()))
        return {};

    if ((is<SPShape>(item) || is<SPText>(item)) && item->style && !item->style->stroke.isNone() &&
        item->style->stroke_width.computed > 0.0) {
        // item_to_outline() is a bounding-box helper: it returns only the
        // uncleaned stroke. Thin, overlapping stroke edges can contain repeated
        // vertices and are not a usable simple collision polygon (Calcifer).
        // Use the existing stroke-to-path cleanup and include the actual fill.
        // These are temporary paths; the SVG and group children stay untouched.
        Geom::PathVector fill, stroke;
        if (item_find_paths(item, fill, stroke)) {
            if (!item->style->fill.isNone() && !stroke.empty()) {
                auto const rule = item->style->fill_rule.computed == SP_WIND_RULE_EVENODD ?
                                  fill_oddEven : fill_nonZero;
                auto outline = sp_pathvector_boolop(fill, stroke, bool_op_union, rule, fill_nonZero);
                return outline * item->i2doc_affine();
            }
            if (item->style->fill.isNone() && !stroke.empty()) {
                auto closed_fill = fill;
                if (closed_fill.empty()) {
                    if (auto curve = curve_for_item(item))
                        closed_fill = std::move(*curve);
                }
                if (!closed_fill.empty() &&
                    std::all_of(closed_fill.begin(), closed_fill.end(),
                                [](Geom::Path const &path) { return path.closed(); })) {
                    auto const rule = item->style->fill_rule.computed == SP_WIND_RULE_EVENODD ?
                                      fill_oddEven : fill_nonZero;
                    try {
                        auto outline = sp_pathvector_boolop(closed_fill, stroke, bool_op_union, rule, fill_nonZero);
                        if (!outline.empty())
                            return outline * item->i2doc_affine();
                    } catch (...) {
                        // Fall back to the item's closed centerline below.
                    }
                    if (auto curve = curve_for_item(item))
                        return *curve * item->i2doc_affine();
                }
            }
            return (stroke.empty() ? fill : stroke) * item->i2doc_affine();
        }
    }
    if (auto curve = curve_for_item(item))
        return *curve * item->i2doc_affine();
    return {};
}

/// The container rules that need no contour extraction (sheetIneligibility).
std::optional<std::string> container_ineligibility(SPItem *item)
{
    if (!item)
        return "no nesting container was provided";
    if (!finite(item->i2doc_affine()))
        return "nesting container has a non-finite transform";
    if (is<SPGroup>(item))
        return "a group cannot be used as the nesting container";
    if (item->getClipObject() || item->getMaskObject())
        return "clipped or masked containers are not supported because their usable boundary is ambiguous";
    if (item->style && item->style->getFilter())
        return "filtered objects cannot be used as nesting containers";
    if (!curve_for_item(item))
        return "container has no convertible vector boundary";
    return std::nullopt;
}

GeometryResult prepare_container_geometry(SPItem *item, double tolerance, ValidationBudget *budget = nullptr)
{
    // The same rules as the tool's hover check, so the two cannot disagree.
    if (auto reason = container_ineligibility(item))
        return {.error = std::move(*reason)};
    auto curve = curve_for_item(item);
    if (!curve)
        return {.error = "container has no convertible vector boundary"};
    auto const transformed = *curve * item->i2doc_affine();
    auto const wind_rule = item->style ? item->style->fill_rule.computed : SP_WIND_RULE_NONZERO;
    return contours_from_path(transformed, wind_rule, tolerance, true, budget);
}

GeometryResult prepare_part_geometry(CapturedLeaf const &leaf, double tolerance, ValidationBudget *budget = nullptr)
{
    if (leaf.group) {
        std::vector<Point> all_points;
        for (auto const &child : leaf.children) {
            auto child_geometry = prepare_part_geometry(child, tolerance, budget);
            if (!child_geometry) {
                // A visible child must never be dropped while the remaining
                // children are treated as the whole payload.
                return {.error = "group child has no usable vector outline: " + child_geometry.error,
                        .too_complex = child_geometry.too_complex};
            }
            all_points.insert(all_points.end(), child_geometry.geometry->outer.begin(), child_geometry.geometry->outer.end());
        }
        auto hull = convex_hull(std::move(all_points), tolerance);
        if (hull.size() < 3)
            return {.error = "group has no usable visible child geometry"};
        return {.geometry = PolygonGeometry{
                    .outer = std::move(hull),
                    .source = ContourSource::ConservativeHull,
                }};
    }

    bool path_too_complex = false;
    std::string path_error;
    if (!leaf.path.empty()) {
        auto result = contours_from_path(leaf.path, static_cast<SPWindRule>(leaf.wind_rule), tolerance, false, budget);
        if (result) {
            if (leaf.clip_or_mask) {
                result.geometry->source = ContourSource::ConservativeHull;
            }
            return result;
        }
        path_too_complex = result.too_complex;
        path_error = result.error;
    }

    std::string reason;
    if (auto bounds = conservative_bounds_geometry(leaf.visual_bounds, reason)) {
        // Keep the diagnosed cause from the failed normalized contour when the
        // fallback outline succeeds: callers surface it instead of replacing it
        // with the generic bounds reason. The fallback outline itself is the
        // unchanged complete rendered extent.
        return {.geometry = std::move(*bounds), .error = std::move(path_error), .too_complex = path_too_complex};
    }
    return {.error = "item has no usable vector outline: " + reason, .too_complex = path_too_complex};
}

/**
 * Recovery for a whole payload (never a subset): the item's complete rendered
 * visual extent, rounded outward to f32 and validated. Part voids may be
 * filled because that reserves more material; container holes are never
 * touched because containers never use this path.
 */
PartGeometryResult conservative_part_result(CapturedLeaf const &leaf, RecoveryKind kind, std::string reason,
                                            bool too_complex)
{
    std::string bounds_reason;
    auto bounds = conservative_bounds_geometry(leaf.visual_bounds, bounds_reason);
    if (!bounds)
        return {.error = "conservative recovery unavailable: " + bounds_reason, .too_complex = too_complex};
    PartGeometry geometry;
    geometry.source = ContourSource::ConservativeBounds;
    geometry.recovery = kind;
    geometry.recovery_reason = std::move(reason);
    geometry.components.push_back({.outer = std::move(bounds->outer)});
    return {.geometry = std::move(geometry), .too_complex = too_complex};
}

/**
 * Recovery classification derived from the geometry actually emitted. A
 * ConservativeBounds component is never Clean: it is a diagnosed whole-part
 * fallback. A ConservativeHull is a recorded repair of the normalized outline.
 * Explicit reasons (invalid markers, trace failures) are preserved.
 */
void classify_recovery(PartGeometry &geometry)
{
    if (geometry.recovery != RecoveryKind::Clean)
        return;
    switch (geometry.source) {
    case ContourSource::ConservativeBounds:
        geometry.recovery = RecoveryKind::ConservativeFallback;
        geometry.recovery_reason = "conservative bounding-box fallback covers the complete rendered extent";
        break;
    case ContourSource::ConservativeHull:
        geometry.recovery = RecoveryKind::Repaired;
        geometry.recovery_reason = "repaired with a conservative hull of the normalized outline";
        break;
    default:
        break;
    }
}

/*
 * True when the item renders any non-bitmap content. A bitmap inside a group
 * that also holds vector artwork is printed content, not cut geometry: the
 * owner's rule is that nesting places and collides such a group by its vector
 * artwork only. The bitmap moves with the group and may extend beyond the
 * vectors (bleed) or overlap other parts' bitmaps. A part made only of
 * bitmaps keeps its alpha outline, because nothing else describes it.
 */
bool has_vector_content(SPItem *item)
{
    if (!item || item->isHidden() || is<SPImage>(item))
        return false;
    if (!is<SPGroup>(item))
        return true;
    for (auto &child_object : item->children) {
        if (has_vector_content(cast<SPItem>(&child_object)))
            return true;
    }
    return false;
}

/*
 * Document-space visual extent of the bitmap subtrees that a vector group's
 * collision outline excludes (see has_vector_content()).
 */
Geom::OptRect ignored_bitmap_bounds(SPItem *item)
{
    Geom::OptRect bounds;
    if (!item || item->isHidden())
        return bounds;
    if (!has_vector_content(item)) {
        bounds.unionWith(item->documentVisualBounds());
        return bounds;
    }
    if (is<SPGroup>(item)) {
        for (auto &child_object : item->children)
            bounds.unionWith(ignored_bitmap_bounds(cast<SPItem>(&child_object)));
    }
    return bounds;
}

// A cut line around a bitmap with bleed typically covers 70-95 % of the
// bitmap's extent; text or a small logo printed on a photo covers far less.
constexpr double SPARSE_VECTOR_AREA_RATIO = 0.5;

// Collision uses each component's filled outer ring (holes are not cut out),
// so a stroked cut line counts with its enclosed area, not its thin band.
double filled_footprint_area(std::span<CollisionComponent const> components)
{
    double area = 0.0;
    for (auto const &component : components)
        area += std::abs(signed_area(component.outer));
    return area;
}

PartGeometryResult prepare_part_components_inner(CapturedLeaf const &item, double tolerance, ValidationBudget &budget,
                                                 std::string &explicit_warning, bool &explicit_warning_too_complex,
                                                 bool ignore_bitmaps)
{
    std::vector<CapturedLeaf const *> explicit_contours;
    auto collect_explicit = [&](auto const &self, CapturedLeaf const &candidate) -> void {
        if (candidate.explicit_contour) {
            explicit_contours.emplace_back(&candidate);
            return;
        }
        if (candidate.group) {
            for (auto const &child : candidate.children)
                self(self, child);
        }
    };
    collect_explicit(collect_explicit, item);
    explicit_warning_too_complex = false;
    if (!explicit_contours.empty()) {
        PartGeometry result;
        result.source = ContourSource::ExplicitContour;
        bool explicit_valid = true;
        bool explicit_too_complex = false;
        std::string invalid_reason;
        for (auto *contour : explicit_contours) {
            auto geometry = prepare_part_geometry(*contour, tolerance, &budget);
            if (!geometry) {
                explicit_valid = false;
                explicit_too_complex = geometry.too_complex;
                invalid_reason = geometry.error;
                break;
            }
            CollisionComponent component{
                .outer = std::move(geometry.geometry->outer),
                .holes = std::move(geometry.geometry->holes),
            };
            auto const validity = validate_component(component, budget);
            if (validity != GeometryValidity::Valid) {
                explicit_valid = false;
                explicit_too_complex = validity == GeometryValidity::TooComplex;
                invalid_reason = validity == GeometryValidity::TooComplex
                                     ? "explicit nesting contour exceeds the validation budget"
                                     : "explicit nesting contour is self-intersecting or degenerate";
                break;
            }
            result.components.push_back(std::move(component));
        }
        if (explicit_valid && !result.components.empty())
            return {.geometry = std::move(result)};
        // Never accept a subset of the designated markers: warn and attempt
        // safe inferred geometry for the whole payload. The budget verdict is
        // retained so an unusable payload is reported as too complex.
        explicit_warning = invalid_reason.empty() ? "explicit nesting contour could not be prepared" : invalid_reason;
        explicit_warning_too_complex = explicit_too_complex;
    }

    if (item.clip) {
        auto const &path = item.clip_path; // empty when the clip is unusable
        if (!path.empty()) {
            auto geometry = contours_from_path(path, SP_WIND_RULE_NONZERO, tolerance, false, &budget);
            if (geometry) {
                CollisionComponent component{
                    .outer = std::move(geometry.geometry->outer),
                    .holes = std::move(geometry.geometry->holes),
                };
                if (validate_component(component, budget) == GeometryValidity::Valid) {
                    return {.geometry = PartGeometry{
                                .components = {std::move(component)},
                                .source = ContourSource::VectorClip,
                            }};
                }
            }
        }
        // A malformed or invalid clip falls through to conservative payload
        // geometry; it never invents cut contours.
    }

    if (item.image) {
        auto alpha = prepare_bitmap_alpha_geometry(item, budget);
        if (alpha)
            return alpha;
        // Fully transparent, unavailable or invalid bitmaps recover with the
        // complete rendered image extent, recording the trace failure.
        return conservative_part_result(item, RecoveryKind::ConservativeFallback, alpha.error, alpha.too_complex);
    }

    if (!item.group) {
        auto geometry = prepare_part_geometry(item, tolerance, &budget);
        if (!geometry)
            return conservative_part_result(item, RecoveryKind::ConservativeFallback, geometry.error,
                                            geometry.too_complex);
        if (geometry.too_complex && !geometry.error.empty()) {
            // The outline is already the complete rendered-extent fallback, but
            // it arrived with a bounded-complexity diagnosis. Rebuild it through
            // the diagnosed recovery helper so classify_recovery() cannot
            // replace the specific too-complex reason with the generic bounds
            // text. Same outline, same safety, better diagnosis.
            return conservative_part_result(item, RecoveryKind::ConservativeFallback, geometry.error, true);
        }
        CollisionComponent component{
            .outer = std::move(geometry.geometry->outer),
            .holes = std::move(geometry.geometry->holes),
        };
        auto const validity = validate_component(component, budget);
        if (validity != GeometryValidity::Valid) {
            auto const too_complex = validity == GeometryValidity::TooComplex;
            auto const reason = too_complex ? std::string{"inferred outline exceeds the validation budget"}
                                            : std::string{"inferred outline is self-intersecting or degenerate"};
            return conservative_part_result(item, RecoveryKind::ConservativeFallback, reason, too_complex);
        }
        PartGeometry part_geometry{
            .components = {std::move(component)},
            .source = geometry.geometry->source,
        };
        classify_recovery(part_geometry);
        return {.geometry = std::move(part_geometry)};
    }

    PartGeometry result;
    result.source = ContourSource::CompoundVector;
    std::size_t aggregate_vertices = 0;
    std::size_t aggregate_rings = 0;
    for (auto const &child : item.children) {
        // Bitmaps (and groups holding only bitmaps) never contribute collision
        // geometry once the part has vector artwork; see has_vector_content().
        if (ignore_bitmaps && !child.vector_content)
            continue;
        std::string child_warning;
        bool child_warning_too_complex = false;
        auto child_geometry = prepare_part_components_inner(child, tolerance, budget, child_warning,
                                                            child_warning_too_complex, ignore_bitmaps);
        if (!child_geometry) {
            // One bad visible child invalidates the whole group payload; the
            // group is never moved with that child silently omitted.
            auto reason = std::string{"group child could not yield safe geometry: "};
            if (!child_warning.empty())
                reason += child_warning + "; ";
            reason += child_geometry.error;
            return conservative_part_result(item, RecoveryKind::ConservativeFallback, std::move(reason),
                                            child_geometry.too_complex || child_warning_too_complex);
        }
        if (result.recovery == RecoveryKind::Clean && child_geometry.geometry->recovery != RecoveryKind::Clean) {
            result.recovery = child_geometry.geometry->recovery;
            result.recovery_reason = child_geometry.geometry->recovery_reason;
        }
        if (result.recovery == RecoveryKind::Clean && !child_warning.empty()) {
            result.recovery = RecoveryKind::Repaired;
            result.recovery_reason = "invalid explicit nesting contour ignored inside group: " + child_warning;
        }
        // Aggregate structural caps are checked before the child components
        // are inserted: an exhausted budget (including repeated child bounds
        // fallbacks) stops aggregate growth and falls back for the whole group
        // instead of building unbounded child rectangles.
        auto &components = child_geometry.geometry->components;
        for (auto const &component : components) {
            auto const count = structure_count(component);
            if (count.rings > MAX_PART_VALIDATED_RINGS - std::min(aggregate_rings, MAX_PART_VALIDATED_RINGS) ||
                count.vertices >
                    MAX_PART_VALIDATED_VERTICES - std::min(aggregate_vertices, MAX_PART_VALIDATED_VERTICES)) {
                return conservative_part_result(item, RecoveryKind::ConservativeFallback,
                                                "group aggregate geometry exceeds the validation budget", true);
            }
            aggregate_rings += count.rings;
            aggregate_vertices += count.vertices;
        }
        result.components.insert(result.components.end(), std::make_move_iterator(components.begin()),
                                 std::make_move_iterator(components.end()));
    }
    if (result.components.empty())
        return {.error = "group has no usable visible child geometry"};
    // Children were already validated and structurally charged once; only the
    // aggregate structural caps are re-checked here, not each sweep again.
    if (aggregate_rings > MAX_PART_VALIDATED_RINGS || aggregate_vertices > MAX_PART_VALIDATED_VERTICES) {
        return conservative_part_result(item, RecoveryKind::ConservativeFallback,
                                        "group aggregate geometry exceeds the validation budget", true);
    }
    return {.geometry = std::move(result)};
}

Geom::Rect ring_bounds(std::span<Point const> ring)
{
    Geom::Rect box(Geom::Point(ring.front().x, ring.front().y), Geom::Point(ring.front().x, ring.front().y));
    for (auto const &point : ring)
        box.expandTo(Geom::Point(point.x, point.y));
    return box;
}

/// True when no edge of `a` touches, crosses or overlaps an edge of `b`;
/// nullopt when the budget runs out. Edges are swept by minimum x, and every
/// cross-ring visit is charged, so the work is bounded by the budget.
std::optional<bool> rings_boundaries_apart(std::span<Point const> a, std::span<Point const> b, ValidationBudget &budget)
{
    struct Edge
    {
        double min_x, max_x, min_y, max_y;
        Point const *start, *end;
        bool second;
    };
    std::vector<Edge> edges;
    edges.reserve(a.size() + b.size());
    auto add = [&](std::span<Point const> ring, bool second) {
        for (std::size_t i = 0, pi = ring.size() - 1; i < ring.size(); pi = i++) {
            auto const &p0 = ring[pi];
            auto const &p1 = ring[i];
            edges.push_back({std::min(p0.x, p1.x), std::max(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.y, p1.y),
                             &p0, &p1, second});
        }
    };
    add(a, false);
    add(b, true);
    std::sort(edges.begin(), edges.end(), [](Edge const &l, Edge const &r) { return l.min_x < r.min_x; });
    // One active list per ring: only edges of different rings are compared.
    std::array<std::vector<Edge const *>, 2> active;
    auto behind = [](double x) { return [x](Edge const *other) { return other->max_x < x; }; };
    for (auto const &edge : edges) {
        auto &others = active[edge.second ? 0 : 1];
        std::erase_if(others, behind(edge.min_x));
        for (auto const *other : others) {
            if (!budget.charge_pair())
                return std::nullopt;
            if (other->max_y < edge.min_y || edge.max_y < other->min_y)
                continue;
            if (segments_conflict(*edge.start, *edge.end, *other->start, *other->end))
                return false;
        }
        // Stale edges leave a list when the other ring's edges scan it.
        active[edge.second ? 1 : 0].push_back(&edge);
    }
    return true;
}

/// True when component `inner` adds nothing to `outer`: its outer ring lies
/// strictly inside `outer`'s outer ring and clear of every hole of `outer`.
bool component_enclosed(CollisionComponent const &inner, Geom::Rect const &inner_box, CollisionComponent const &outer,
                        Geom::Rect const &outer_box, ValidationBudget &budget)
{
    if (!outer_box.contains(inner_box))
        return false;
    auto apart = rings_boundaries_apart(inner.outer, outer.outer, budget);
    if (!apart || !*apart || point_in_polygon_bounded(inner.outer.front(), outer.outer, &budget) != Containment::Inside)
        return false;
    for (auto const &hole : outer.holes) {
        // Boundaries apart: `inner` is inside the hole, around it, or clear of it.
        auto clear = rings_boundaries_apart(inner.outer, hole, budget);
        if (!clear || !*clear || point_in_polygon_bounded(inner.outer.front(), hole, &budget) != Containment::Outside ||
            point_in_polygon_bounded(hole.front(), inner.outer, &budget) != Containment::Outside)
            return false;
    }
    return true;
}

/**
 * Removes components lying wholly inside another component of the same part,
 * such as printed artwork inside its cut outline. Both engines collide
 * against the union of a part's components, so the union is unchanged; a
 * part that reduces to one component can also use the Sparrow lane. The
 * work has its own budget; when it runs out the remaining components stay.
 */
void drop_enclosed_components(PartGeometry &geometry)
{
    auto &components = geometry.components;
    if (components.size() < 2)
        return;
    std::vector<Geom::Rect> boxes;
    boxes.reserve(components.size());
    for (auto const &component : components)
        boxes.push_back(ring_bounds(component.outer));
    ValidationBudget budget; // separate: never charged to the part's validation
    std::vector<bool> dropped(components.size(), false);
    for (std::size_t i = 0; i < components.size() && !budget.exceeded; ++i) {
        for (std::size_t j = 0; j < components.size() && !dropped[i] && !budget.exceeded; ++j) {
            if (i != j && !dropped[j] && component_enclosed(components[i], boxes[i], components[j], boxes[j], budget))
                dropped[i] = true;
        }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (!dropped[i]) {
            if (kept != i)
                components[kept] = std::move(components[i]);
            ++kept;
        }
    }
    components.resize(kept);
}

PartGeometryResult prepare_part_components(CapturedItem const &captured, double tolerance, ValidationBudget &budget)
{
    std::string explicit_warning;
    bool explicit_warning_too_complex = false;
    auto const &item = captured.root;
    auto result = prepare_part_components_inner(item, tolerance, budget, explicit_warning, explicit_warning_too_complex,
                                                captured.ignore_bitmaps);
    if (result) {
        drop_enclosed_components(*result.geometry);
        if (!explicit_warning.empty() && result.geometry->recovery == RecoveryKind::Clean) {
            result.geometry->recovery = RecoveryKind::Repaired;
            result.geometry->recovery_reason = "invalid explicit nesting contour ignored: " + explicit_warning;
        }
        classify_recovery(*result.geometry);
        return result;
    }
    if (!explicit_warning.empty()) {
        // Preserve the marker warning in the whole-payload fallback reason too.
        auto fallback = conservative_part_result(
            item, RecoveryKind::ConservativeFallback,
            "invalid explicit nesting contour and no valid inferred geometry: " + explicit_warning,
            result.too_complex || explicit_warning_too_complex);
        if (fallback) {
            classify_recovery(*fallback.geometry);
            return fallback;
        }
        if (result.error.find(explicit_warning) == std::string::npos)
            result.error = "invalid explicit nesting contour: " + explicit_warning + "; " + result.error;
        result.too_complex = result.too_complex || explicit_warning_too_complex;
    }
    return result;
}

/// Per-capture state: pixel hashes, and one alpha plane per distinct bitmap.
struct CaptureContext
{
    PixelHashMemo pixel_hashes;
    std::map<AlphaTraceKey, std::shared_ptr<AlphaPlane const>> planes;
};

/**
 * Phase A for one object (R1): everything the decision tree reads, as plain
 * data. Children are captured for groups only (the tree never descends into
 * other items), in document order. Deviation from the synchronous code, which
 * computed outlines lazily: the stroke-to-path outline is computed for every
 * non-group leaf, also where the tree later uses an explicit contour or a
 * clip instead, so livarot's static TurnInside/PrevPos history can differ from
 * before (the golden parity run found no difference). Bitmap alpha is copied
 * only for bitmaps that can be traced (not ignored under S10) and whose trace
 * is not cached, once per distinct bitmap.
 */
CapturedLeaf capture_leaf(SPItem *item, bool ignore_bitmaps, CaptureContext &context)
{
    auto *memo = &context.pixel_hashes;
    CapturedLeaf leaf;
    leaf.label = object_label(item);
    leaf.group = is<SPGroup>(item);
    leaf.image = is<SPImage>(item);
    auto const *marker = item->getRepr()->attribute(NESTING_CONTOUR_ATTRIBUTE);
    leaf.explicit_contour = marker && std::string_view(marker) == "true";
    leaf.clip_or_mask = item->getClipObject() || item->getMaskObject();
    if (auto *clip = item->getClipObject()) {
        leaf.clip = true;
        auto clip_transform = item->i2doc_affine();
        bool clip_transform_valid = true;
        if (clip->clippath_units() == SP_CONTENT_UNITS_OBJECTBOUNDINGBOX) {
            auto const bounds = item->geometricBounds();
            if (bounds && !bounds->hasZeroArea()) {
                clip_transform = Geom::Scale(bounds->dimensions()) * Geom::Translate(bounds->min()) * clip_transform;
            } else {
                clip_transform_valid = false;
            }
        }
        if (clip_transform_valid)
            leaf.clip_path = clip->getPathVector(clip_transform);
    }
    if (!leaf.group)
        leaf.path = part_path(item); // stroke-to-path stays on this thread (livarot static state)
    leaf.wind_rule = item->style ? item->style->fill_rule.computed : SP_WIND_RULE_NONZERO;
    leaf.visual_bounds = item->documentVisualBounds();
    leaf.vector_content = has_vector_content(item);
    if (auto *image = cast<SPImage>(item); image && !ignore_bitmaps) {
        leaf.pixels = static_cast<bool>(image->pixbuf);
        leaf.pixel_to_document = image->pixelToDocumentAffine();
        if (leaf.pixels && leaf.pixel_to_document) {
            auto const &pixbuf = *image->pixbuf;
            auto const hash = pixel_hash(pixbuf, memo);
            AlphaTraceKey const key{pixbuf.width(), pixbuf.height(), hash};
            leaf.trace = cached_alpha_trace(key);
            if (auto shared = context.planes.find(key); !leaf.trace && shared != context.planes.end()) {
                leaf.alpha = shared->second;
            } else if (!leaf.trace && pixbuf.width() > 0 && pixbuf.height() > 0 && pixbuf.pixels()) {
                // Copy, never share with the document: the GTK thread may
                // convert the pixbuf's representation in place while a worker
                // traces.
                auto copy = std::make_shared<AlphaPlane>();
                auto &plane = *copy;
                plane.width = pixbuf.width();
                plane.height = pixbuf.height();
                plane.content_hash = hash;
                plane.alpha.resize(static_cast<std::size_t>(plane.width) * plane.height);
                auto const rowstride = pixbuf.rowstride();
                for (int y = 0; y < plane.height; ++y) {
                    auto const *row = pixbuf.pixels() + static_cast<std::ptrdiff_t>(y) * rowstride;
                    auto *out = plane.alpha.data() + static_cast<std::ptrdiff_t>(y) * plane.width;
                    for (int x = 0; x < plane.width; ++x)
                        out[x] = pixel_alpha(pixbuf, row, x);
                }
                leaf.alpha = copy;
                context.planes.emplace(key, std::move(copy));
            }
        }
    }
    if (leaf.group) {
        for (auto &child_object : item->children) {
            if (auto *child = cast<SPItem>(&child_object); child && !child->isHidden())
                leaf.children.push_back(capture_leaf(child, ignore_bitmaps, context));
        }
    }
    return leaf;
}

CapturedItem capture_item(SPItem *item, CaptureContext &context)
{
    CapturedItem captured;
    captured.label = object_label(item);
    captured.ignore_bitmaps = is<SPGroup>(item) && has_vector_content(item);
    captured.root = capture_leaf(item, captured.ignore_bitmaps, context);
    if (captured.ignore_bitmaps)
        captured.ignored_bitmap_bounds = ignored_bitmap_bounds(item);
    return captured;
}

/// Capture and prepare in one call on the GTK thread (apply-time checks).
PartGeometryResult prepare_part_components(SPItem *item, double tolerance, ValidationBudget &budget,
                                           PixelHashMemo *memo = nullptr)
{
    CaptureContext context;
    if (memo)
        context.pixel_hashes = *memo; // reuse hashes already computed in this call
    return prepare_part_components(capture_item(item, context), tolerance, budget);
}

void hash_word(std::uint64_t &hash, std::uint64_t word)
{
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash ^= (word >> (byte * 8)) & 0xffU;
        hash *= FNV_PRIME;
    }
}

std::uint64_t geometry_fingerprint(PolygonGeometry const &geometry)
{
    auto hash = FNV_OFFSET;
    hash_word(hash, static_cast<std::uint64_t>(geometry.source));
    hash_word(hash, geometry.outer.size());
    for (auto const &point : geometry.outer) {
        hash_word(hash, std::bit_cast<std::uint64_t>(point.x));
        hash_word(hash, std::bit_cast<std::uint64_t>(point.y));
    }
    hash_word(hash, geometry.holes.size());
    for (auto const &hole : geometry.holes) {
        hash_word(hash, hole.size());
        for (auto const &point : hole) {
            hash_word(hash, std::bit_cast<std::uint64_t>(point.x));
            hash_word(hash, std::bit_cast<std::uint64_t>(point.y));
        }
    }
    return hash;
}

std::uint64_t geometry_fingerprint(PartGeometry const &geometry)
{
    auto hash = FNV_OFFSET;
    hash_word(hash, static_cast<std::uint64_t>(geometry.source));
    hash_word(hash, geometry.components.size());
    for (auto const &component : geometry.components) {
        hash_word(hash, component.outer.size());
        for (auto const &point : component.outer) {
            hash_word(hash, std::bit_cast<std::uint64_t>(point.x));
            hash_word(hash, std::bit_cast<std::uint64_t>(point.y));
        }
        hash_word(hash, component.holes.size());
        for (auto const &hole : component.holes) {
            hash_word(hash, hole.size());
            for (auto const &point : hole) {
                hash_word(hash, std::bit_cast<std::uint64_t>(point.x));
                hash_word(hash, std::bit_cast<std::uint64_t>(point.y));
            }
        }
    }
    return hash;
}

// Referenced-resource and text hashing. All attributes are hashed (sorted by
// name) rather than a fragile whitelist. Every visited element follows its
// local references — url(#id) with optional quotes/whitespace, and href="#id" —
// through a shared visited set, so reference chains (marker -> use -> path,
// inheritance href chains, filter/mask chains) are captured, cycles terminate
// deterministically and repeated captures stay stable. External or otherwise
// uncapturable references are never fetched or speculated about: only targets
// that already exist in this document are traversed. There is deliberately no
// depth cap, so relevant XML content is never silently dropped.

void hash_node_attributes(std::uint64_t &hash, Inkscape::XML::Node const *node)
{
    if (!node)
        return;
    // Views, not copies: embedded images make values megabytes long. The node
    // is not modified while it is hashed.
    std::vector<std::pair<std::string_view, std::string_view>> attributes;
    for (auto const &record : node->attributeList()) {
        auto const *name = g_quark_to_string(record.key);
        auto const *value = static_cast<char const *>(record.value);
        attributes.emplace_back(name ? name : "", value ? value : "");
    }
    // attributeList order is an implementation detail; sort for determinism.
    std::sort(attributes.begin(), attributes.end());
    for (auto const &[name, value] : attributes) {
        hash_text(hash, name);
        hash_text(hash, value);
    }
}

void hash_xml_node(std::uint64_t &hash, Inkscape::XML::Node const *node)
{
    hash_text(hash, node->name());
    hash_node_attributes(hash, node);
    if (node->type() != Inkscape::XML::NodeType::ELEMENT_NODE) {
        if (auto const *content = node->content())
            hash_text(hash, content);
    }
}

std::string_view trim_fingerprint_space(std::string_view text)
{
    auto const is_space = [](char value) {
        return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f';
    };
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_space(text.back()))
        text.remove_suffix(1);
    return text;
}

/**
 * What a content fingerprint saw that watching the XML cannot vouch for (R2):
 * decoded bitmap pixels, text (its layout depends on installed fonts) and
 * references outside the document.
 */
struct FingerprintFacts
{
    std::vector<std::pair<SPWeakPtr<SPItem>, std::uint64_t>> images;
    bool has_text = false;
    bool has_external_reference = false;
};

bool is_text_element(char const *name)
{
    if (!name)
        return false;
    std::string_view const element{name};
    return element == "svg:text" || element == "svg:tspan" || element == "svg:tref" || element == "svg:textPath" ||
           element.starts_with("svg:flow");
}

void hash_local_references(std::uint64_t &hash, SPDocument *document, char const *name, char const *value,
                           std::vector<Inkscape::XML::Node const *> &pending, FingerprintFacts *facts)
{
    if (!document || !name || !value)
        return;

    auto const follow = [&](std::string_view raw) {
        raw = trim_fingerprint_space(raw);
        if (raw.size() >= 2 && ((raw.front() == '"' && raw.back() == '"') ||
                                (raw.front() == '\'' && raw.back() == '\''))) {
            raw = trim_fingerprint_space(raw.substr(1, raw.size() - 2));
        }
        // Only same-document references are followed. A file URL or a relative
        // external path is left alone: no network fetch, no resource guessing.
        if (raw.empty())
            return;
        if (raw.front() != '#') {
            if (facts && !raw.starts_with("data:"))
                facts->has_external_reference = true;
            return;
        }
        std::string const id{raw.substr(1)};
        auto *object = document->getObjectById(id);
        auto const *repr = object ? object->getRepr() : nullptr;
        if (!repr) {
            hash_text(hash, "<unresolved-reference>");
            hash_text(hash, id.c_str());
            return;
        }
        pending.push_back(repr);
    };

    std::string_view const text{value};
    std::size_t position = 0;
    while ((position = text.find("url(", position)) != std::string_view::npos) {
        auto const open = position + 4;
        auto const close = text.find(')', open);
        if (close == std::string_view::npos)
            break;
        follow(text.substr(open, close - open));
        position = close + 1;
    }
    std::string_view const attribute{name};
    if (attribute == "href" || attribute.ends_with(":href"))
        follow(text);
}

// Drains the referenced-node queue iteratively. References found inside a
// referenced resource are followed as well, so an edit in a marker's use->path
// target cannot leave the parent item's fingerprint unchanged.
void drain_referenced_nodes(std::uint64_t &hash, SPDocument *document,
                            std::unordered_set<Inkscape::XML::Node const *> &visited,
                            std::vector<Inkscape::XML::Node const *> &pending, FingerprintFacts *facts)
{
    while (!pending.empty()) {
        auto const *node = pending.back();
        pending.pop_back();
        if (!node)
            continue;
        if (!visited.insert(node).second) {
            hash_text(hash, "<shared-reference>");
            continue;
        }
        hash_xml_node(hash, node);
        if (node->type() != Inkscape::XML::NodeType::ELEMENT_NODE)
            continue;
        if (facts && is_text_element(node->name()))
            facts->has_text = true;
        for (auto const *child = node->firstChild(); child; child = child->next()) {
            if (child->type() == Inkscape::XML::NodeType::ELEMENT_NODE) {
                pending.push_back(child);
            } else if (auto const *content = child->content()) {
                hash_u64(hash, static_cast<std::uint64_t>(child->type()));
                hash_text(hash, content);
            }
        }
        for (auto const &record : node->attributeList()) {
            auto const *name = g_quark_to_string(record.key);
            auto const *value = static_cast<char const *>(record.value);
            hash_local_references(hash, document, name, value, pending, facts);
        }
    }
}

/**
 * Content-sensitive fingerprint over the visible item subtree: every XML
 * attribute (path data, style, clip/mask/marker/filter references, ...),
 * referenced resource definitions, text/comment content and decoded image
 * pixels. Geometric bounds alone can stay identical across such edits, so the
 * freshness check must compare this value as well.
 */
std::uint64_t item_content_fingerprint(SPItem *item, FingerprintFacts *facts = nullptr, PixelHashMemo *memo = nullptr)
{
    auto hash = FNV_OFFSET;
    std::unordered_set<Inkscape::XML::Node const *> visited;
    std::vector<Inkscape::XML::Node const *> pending_nodes;
    std::vector<SPItem *> pending_items;
    pending_items.push_back(item);
    while (!pending_items.empty()) {
        auto *current = pending_items.back();
        pending_items.pop_back();
        if (!current) {
            hash_u64(hash, 0);
            continue;
        }
        auto const *repr = current->getRepr();
        if (!repr) {
            hash_u64(hash, 0);
            continue;
        }
        if (!visited.insert(repr).second) {
            hash_text(hash, "<item-already-hashed>");
            continue;
        }
        hash_xml_node(hash, repr);
        if (facts && is_text_element(repr->name()))
            facts->has_text = true;
        for (auto const *child = repr->firstChild(); child; child = child->next()) {
            if (child->type() != Inkscape::XML::NodeType::ELEMENT_NODE) {
                if (auto const *content = child->content()) {
                    hash_u64(hash, static_cast<std::uint64_t>(child->type()));
                    hash_text(hash, content);
                }
            }
        }
        auto document = current->document;
        for (auto const &record : repr->attributeList()) {
            auto const *name = g_quark_to_string(record.key);
            auto const *value = static_cast<char const *>(record.value);
            hash_local_references(hash, document, name, value, pending_nodes, facts);
        }
        if (auto *image = cast<SPImage>(current); image && image->pixbuf) {
            auto const pixels = pixel_hash(*image->pixbuf, memo);
            hash_u64(hash, pixels);
            if (facts)
                facts->images.emplace_back(SPWeakPtr<SPItem>(image), pixels);
        }
        std::size_t visible_children = 0;
        for (auto &child_object : current->children) {
            auto *child = cast<SPItem>(&child_object);
            if (!child || child->isHidden())
                continue;
            ++visible_children;
            // Every visible item child is traversed, not only SPGroup: text
            // tspan descendants are items outside the group type and used to be
            // omitted, so their text edits could not invalidate a snapshot.
            pending_items.push_back(child);
        }
        hash_u64(hash, visible_children);
        drain_referenced_nodes(hash, document, visited, pending_nodes, facts);
    }
    return hash;
}

/**
 * R2: while the document's XML is unchanged (namedview edits aside), nothing
 * prepared from XML can have changed, so geometry is not re-prepared. The
 * checks XML cannot see always run: identity, visibility, lock state,
 * transforms and decoded bitmap pixels. Text and external references always
 * take the full path.
 */
bool geometry_still_matches(PreparedDocumentNesting const &snapshot, std::size_t &revalidated)
{
    bool const document_unchanged = snapshot.revision && !snapshot.revision->changed();
    PixelHashMemo pixel_hashes;
    auto *container = snapshot.container.get();
    if (snapshot.request_local_sheet && !document_unchanged) return false;
    if (!snapshot.request_local_sheet && (!container || container->document != snapshot.document || container->isHidden() || container->isLocked() ||
        !affine_near(container->i2doc_affine(), snapshot.container_item_to_document))) {
        return false;
    }
    if (!snapshot.request_local_sheet && (!document_unchanged || snapshot.container_always_revalidate)) {
        ValidationBudget container_geometry_budget;
        auto container_geometry =
            prepare_container_geometry(container, snapshot.flatten_tolerance, &container_geometry_budget);
        ++revalidated;
        if (!container_geometry ||
            geometry_fingerprint(*container_geometry.geometry) != snapshot.container_geometry_fingerprint) {
            return false;
        }
        {
            ValidationBudget container_budget;
            CollisionComponent container_component{.outer = container_geometry.geometry->outer,
                                                   .holes = container_geometry.geometry->holes};
            if (validate_component(container_component, container_budget) != GeometryValidity::Valid)
                return false;
        }
    }

    for (auto const &part : snapshot.parts) {
        auto *item = part.item.get();
        if (!item || item->document != snapshot.document || item->isHidden() || item->isLocked() ||
            !affine_near(item->i2doc_affine(), part.original_item_to_document)) {
            return false;
        }
        if (document_unchanged && !part.always_revalidate) {
            // An in-place pixel edit keeps the pixbuf address and writes no XML.
            for (auto const &[weak, hash] : part.image_alpha_hashes) {
                auto *image = cast<SPImage>(weak.get());
                if (!image || !image->pixbuf || pixel_hash(*image->pixbuf, &pixel_hashes) != hash)
                    return false;
            }
            continue;
        }
        if (item_content_fingerprint(item, nullptr, &pixel_hashes) != part.content_fingerprint)
            return false;
        ValidationBudget budget;
        auto geometry = prepare_part_components(item, snapshot.flatten_tolerance, budget, &pixel_hashes);
        ++revalidated;
        if (!geometry || geometry_fingerprint(*geometry.geometry) != part.geometry_fingerprint) {
            return false;
        }
        if (geometry.geometry->recovery != part.recovery ||
            geometry.geometry->recovery_reason != part.recovery_reason) {
            return false;
        }
    }
    for (auto const &obstacle : snapshot.obstacles) {
        // Amendment C2, following R2: identity and transform always (locked
        // obstacles are allowed; moved, hidden or deleted ones are stale),
        // bitmap pixels while the document is unchanged, and the content
        // fingerprint plus a re-measure when it changed or text/external
        // references are involved.
        auto *item = obstacle.item.get();
        if (!item || item->document != snapshot.document || item->isHidden() ||
            !affine_near(item->i2doc_affine(), obstacle.item_to_document)) {
            return false;
        }
        if (document_unchanged && !obstacle.always_revalidate) {
            for (auto const &[weak, hash] : obstacle.image_alpha_hashes) {
                auto *image = cast<SPImage>(weak.get());
                if (!image || !image->pixbuf || pixel_hash(*image->pixbuf, &pixel_hashes) != hash)
                    return false;
            }
            continue;
        }
        if (item_content_fingerprint(item, nullptr, &pixel_hashes) != obstacle.content_fingerprint)
            return false;
        // Re-measure too: inherited style can change the outline without an
        // edit of the obstacle's own XML (review of 5d78d0da1).
        ValidationBudget budget;
        auto geometry = prepare_part_components(item, snapshot.flatten_tolerance, budget, &pixel_hashes);
        ++revalidated;
        if (!geometry || geometry_fingerprint(*geometry.geometry) != obstacle.geometry_fingerprint)
            return false;
    }
    return true;
}

} // namespace

bool preparedNestingFresh(PreparedDocumentNesting const &snapshot) {
    std::size_t count=0; return geometry_still_matches(snapshot,count);
}

ContourBindingResult setNestingContour(SPDocument &document, SPItem *payload, SPItem *contour,
                                      bool dry_run, std::shared_ptr<void> const &owner_lease)
{
    using R = ContourBindingReason;
    using S = ContourBindingStatus;
    if (!payload || !contour || payload->document != &document || contour->document != &document ||
        !payload->getId() || !contour->getId())
        return {.reason = R::InvalidItem};
    if (payload == contour || payload->isAncestorOf(contour) || contour->isAncestorOf(payload))
        return {.reason = R::OverlappingRoles};
    if (payload->parent != contour->parent || payload->isHidden() || payload->isLocked() ||
        contour->isHidden() || contour->isLocked() || !finite(payload->i2doc_affine()) ||
        !finite(contour->i2doc_affine()) || payload->cloned || contour->cloned)
        return {.reason = R::UnsafeContext};
    if (!is<SPShape>(contour) || !prepare_container_geometry(contour, 0.05))
        return {.reason = R::UnsupportedContour};
    ContourBindingResult result;
    result.payload_ids.emplace_back(payload->getId());
    result.contour_ids.emplace_back(contour->getId());
    if (dry_run) { result.status = S::Prepared; return result; }
    auto transaction = owner_lease ? DocumentUndo::beginAtomicCommandInteraction(&document, owner_lease)
                                   : DocumentUndo::beginAtomicInteraction(&document);
    if (!transaction) return {.reason = R::Busy};
    std::vector<std::string> selected;
    for (auto *item : document.getSelection()->items()) if (item->getId()) selected.emplace_back(item->getId());
    auto restore = [&] {
        transaction->rollback();
        std::vector<SPItem *> items;
        for (auto const &id : selected)
            if (auto *item = cast<SPItem>(document.getObjectById(id))) items.push_back(item);
        document.getSelection()->setList(items);
    };
    try {
    ObjectSet targets(&document);
    targets.add(payload); targets.add(contour);
    contour->setAttribute("inkscape:nesting-contour", "true");
    auto *group = targets.group();
    if (!group) { restore(); return {.reason = R::PublicationFailed}; }
    group->setAttribute("inkscape:nesting-contour-version", "1");
    if (auto id = group->attribute("id")) result.binding_ids.emplace_back(id);
    if (!transaction->commitAtomically(Util::Internal::ContextString{"Set nesting contour"},
                                      INKSCAPE_ICON("object-group"), [] { return true; })) {
        restore(); return {.reason = R::PublicationFailed};
    }
    } catch (...) { restore(); return {.reason = R::PublicationFailed}; }
    result.status = S::Applied;
    return result;
}

ContourBindingResult releaseNestingContour(SPDocument &document, std::span<SPItem *const> roots,
                                          bool dry_run, std::shared_ptr<void> const &owner_lease)
{
    using R = ContourBindingReason;
    using S = ContourBindingStatus;
    ContourBindingResult result;
    std::vector<SPObject *> changes;
    std::unordered_set<SPObject *> seen;
    std::function<bool(SPObject *)> visit = [&](SPObject *object) {
        if (!object || object->cloned || !seen.emplace(object).second) return true;
        auto *repr = object->getRepr();
        bool binding = repr->attribute("inkscape:nesting-contour-version");
        bool contour = repr->attribute("inkscape:nesting-contour");
        if (binding || contour) {
            if (auto item = cast<SPItem>(object); item && (item->isHidden() || item->isLocked())) return false;
            changes.push_back(object);
            if (object->getId()) {
                if (binding) {
                    result.binding_ids.emplace_back(object->getId());
                    // Preserve payload-root identity, including a grouped payload.
                    // The binding wrapper itself is not artwork.
                    for (auto &child : object->children) {
                        if (is<SPItem>(&child) && child.getId() &&
                            !child.getRepr()->attribute("inkscape:nesting-contour") &&
                            !child.getRepr()->attribute("inkscape:nesting-contour-version"))
                            result.payload_ids.emplace_back(child.getId());
                    }
                }
                if (contour) result.contour_ids.emplace_back(object->getId());
            }
        }
        for (auto &child : object->children) if (!visit(&child)) return false;
        return true;
    };
    for (auto *root : roots) {
        if (!root || root->document != &document) return {.reason = R::InvalidItem};
        if (root->isHidden() || root->isLocked() || !visit(root)) return {.reason = R::UnsafeContext};
    }
    if (changes.empty()) { result.status = S::Unchanged; return result; }
    if (dry_run) { result.status = S::Prepared; return result; }
    auto transaction = owner_lease ? DocumentUndo::beginAtomicCommandInteraction(&document, owner_lease)
                                   : DocumentUndo::beginAtomicInteraction(&document);
    if (!transaction) return {.reason = R::Busy};
    try {
    for (auto *object : changes) {
        object->getRepr()->removeAttribute("inkscape:nesting-contour");
        object->getRepr()->removeAttribute("inkscape:nesting-contour-version");
    }
    if (!transaction->commitAtomically(Util::Internal::ContextString{"Release nesting contour"},
                                      INKSCAPE_ICON("object-group"), [] { return true; })) {
        transaction->rollback(); return {.reason = R::PublicationFailed};
    }
    } catch (...) { transaction->rollback(); return {.reason = R::PublicationFailed}; }
    result.status = S::Applied;
    return result;
}

SheetObstacles collectSheetObstacles(SPItem *sheet, unsigned dkey, std::span<SPItem *const> parts)
{
    SheetObstacles result;
    if (!sheet || !sheet->document)
        return result;
    auto const sheet_bounds = sheet->documentVisualBounds();
    if (!sheet_bounds)
        return result;

    std::unordered_set<SPItem const *> excluded(parts.begin(), parts.end());
    excluded.insert(sheet);
    auto const holds_excluded = [&](SPItem const *item) {
        if (item->isAncestorOf(sheet))
            return true;
        return std::any_of(parts.begin(), parts.end(),
                           [item](SPItem const *part) { return part && item->isAncestorOf(part); });
    };

    // An object that follows a part through a reference (a <use> clone, a text
    // path or tref, a flowed-text shape-inside/shape-subtract) moves or reflows
    // with the part: it is not a fixed obstacle. Paint servers, filters, clips
    // and markers are shared resources, not followers, and are not considered.
    auto const related_to_part = [&](SPObject const *object) {
        return std::any_of(parts.begin(), parts.end(), [object](SPItem const *part) {
            return part && (object == part || object->isAncestorOf(part) || part->isAncestorOf(object));
        });
    };
    auto const follow_ids = [](std::string_view name, std::string_view value, std::vector<std::string> &ids) {
        if ((name == "href" || name.ends_with(":href")) && value.starts_with('#')) {
            ids.emplace_back(value.substr(1));
            return;
        }
        if (name != "style" && name != "shape-inside" && name != "shape-subtract")
            return;
        // shape-inside / shape-subtract: url(#id), url('#id') or url("#id").
        for (auto const *property : {"shape-inside", "shape-subtract"}) {
            auto at = name == "style" ? value.find(property) : (name == property ? 0 : std::string_view::npos);
            while (at != std::string_view::npos) {
                auto const end = value.find(';', at);
                auto const declaration = value.substr(at, end == std::string_view::npos ? end : end - at);
                for (auto url = declaration.find("url("); url != std::string_view::npos;
                     url = declaration.find("url(", url + 4)) {
                    auto const close = declaration.find(')', url);
                    if (close == std::string_view::npos)
                        break;
                    auto target = declaration.substr(url + 4, close - url - 4);
                    while (!target.empty() && (target.front() == '"' || target.front() == '\'' || target.front() == ' '))
                        target.remove_prefix(1);
                    while (!target.empty() && (target.back() == '"' || target.back() == '\'' || target.back() == ' '))
                        target.remove_suffix(1);
                    if (target.starts_with('#'))
                        ids.emplace_back(target.substr(1));
                }
                at = name == "style" && end != std::string_view::npos ? value.find(property, end) : std::string_view::npos;
            }
        }
    };
    auto const references_part = [&](SPItem *item) {
        std::vector<Inkscape::XML::Node const *> pending{item->getRepr()};
        std::unordered_set<Inkscape::XML::Node const *> seen_nodes;
        auto *document = item->document;
        while (!pending.empty()) {
            auto const *node = pending.back();
            pending.pop_back();
            if (!node || !seen_nodes.insert(node).second)
                continue;
            for (auto const *child = node->firstChild(); child; child = child->next())
                pending.push_back(child);
            for (auto const &record : node->attributeList()) {
                auto const *raw_name = g_quark_to_string(record.key);
                auto const *raw_value = static_cast<char const *>(record.value);
                if (!raw_name || !raw_value)
                    continue;
                std::vector<std::string> ids;
                follow_ids(raw_name, raw_value, ids);
                for (auto const &id : ids) {
                    if (auto *target = document->getObjectById(id)) {
                        if (related_to_part(target))
                            return true;
                        pending.push_back(target->getRepr()); // chains: a clone of a clone
                    }
                }
            }
        }
        return false;
    };

    std::unordered_set<SPItem const *> visited;
    auto visit = [&](auto const &self, SPItem *item) -> void {
        if (!item || excluded.contains(item) || item->isHidden() || !visited.insert(item).second)
            return;
        auto const bounds = item->documentVisualBounds();
        if (!bounds || !bounds->intersects(*sheet_bounds))
            return;
        if (holds_excluded(item)) {
            // Take the other members of a group that holds the sheet or a part.
            for (auto &child : item->children)
                self(self, cast<SPItem>(&child));
            return;
        }
        if (bounds->contains(*sheet_bounds)) {
            result.ignored_backgrounds.push_back(item);
            return;
        }
        if (!parts.empty() && references_part(item)) {
            // A group that merely contains a follower keeps its other members.
            if (is<SPGroup>(item)) {
                for (auto &child : item->children)
                    self(self, cast<SPItem>(&child));
            }
            return;
        }
        result.items.push_back(item);
    };
    auto candidates = sheet->document->getItemsPartiallyInBox(dkey, *sheet_bounds, /*take_hidden=*/false,
                                                              /*take_insensitive=*/true, /*take_groups=*/true,
                                                              /*enter_groups=*/false, /*enter_layers=*/true);
    // A group holding the sheet is returned whole by the query; visiting it
    // enters it. The sheet itself is excluded.
    for (auto *item : candidates)
        visit(visit, item);
    return result;
}

std::optional<std::string> sheetIneligibility(SPItem *item)
{
    return container_ineligibility(item);
}

static CaptureResult capture_nesting(SPDocument *document, SPItem *container,
                                     std::span<SPItem *const> candidate_parts,
                                     double flatten_tolerance, std::span<SPItem *const> obstacles,
                                     std::optional<Geom::Rect> sheet = {})
{
    auto const started = std::chrono::steady_clock::now();
    if (!container && !sheet)
        return {.error = "no nesting container was provided"};
    if (!document)
        return {.error = "nesting container is detached from a document"};
    if (container && (container->isHidden() || container->isLocked())) {
        return {.error = "nesting container is hidden or locked"};
    }
    if (!std::isfinite(flatten_tolerance) || flatten_tolerance < MIN_ABSOLUTE_TOLERANCE) {
        return {.error = "flatten tolerance must be finite and at least 0.000001 document units"};
    }
    if (container && !finite(container->i2doc_affine())) {
        return {.error = "nesting container has a non-finite transform"};
    }
    // Watch the document from before anything is read (R2). Ignored: the
    // namedview's own attributes (view state) and its guides, pages and grids,
    // none of which nesting reads. Anything else placed under the namedview is a
    // real document object (it can be referenced or carry a stylesheet) and
    // still counts as a change.
    auto const code = [](char const *name) { return static_cast<int>(g_quark_from_static_string(name)); };
    auto revision = std::make_shared<Inkscape::XML::SubtreeRevision const>(
        *document->getReprRoot(), sheet ? 0 : code("sodipodi:namedview"),
        std::vector<int>{code("sodipodi:guide"), code("inkscape:page"), code("inkscape:grid")});

    // The same rules as the tool's hover check, so the two cannot disagree.
    if (auto reason = container ? container_ineligibility(container) : std::nullopt)
        return {.error = "invalid nesting container: " + *reason};

    auto input = std::make_shared<CapturedInput>();
    if (sheet) {
        Geom::Path path(sheet->corner(0));
        for (unsigned i = 1; i < 4; ++i) path.appendNew<Geom::LineSegment>(sheet->corner(i));
        path.close();
        input->container_path.push_back(path);
        input->container_wind_rule = SP_WIND_RULE_NONZERO;
    } else {
        input->container_path = *curve_for_item(container) * container->i2doc_affine();
        input->container_wind_rule = container->style ? container->style->fill_rule.computed : SP_WIND_RULE_NONZERO;
    }
    input->flatten_tolerance = flatten_tolerance;

    CaptureResult result;
    auto &skeleton = result.skeleton;
    skeleton.document = document;
    skeleton.request_local_sheet = sheet.has_value();
    skeleton.container = SPWeakPtr<SPItem>(container);
    if (container) skeleton.container_item_to_document = container->i2doc_affine();
    skeleton.flatten_tolerance = flatten_tolerance;
    skeleton.revision = std::move(revision);
    CaptureContext context;
    auto &pixel_hashes = context.pixel_hashes;
    if (container) {
        FingerprintFacts container_facts;
        (void)item_content_fingerprint(container, &container_facts, &pixel_hashes);
        // Container bitmaps are rare; keep them on the full path rather than
        // carrying a second pixel list.
        skeleton.container_always_revalidate = container_facts.has_text || container_facts.has_external_reference ||
                                               !container_facts.images.empty();
    }
    auto identity = [&](SPItem *item) {
        CaptureResult::Identity id;
        id.item = SPWeakPtr<SPItem>(item);
        if (item) {
            id.item_to_document = item->i2doc_affine();
            FingerprintFacts facts;
            id.content_fingerprint = item_content_fingerprint(item, &facts, &pixel_hashes);
            id.image_alpha_hashes = std::move(facts.images);
            id.always_revalidate = facts.has_text || facts.has_external_reference;
        }
        return id;
    };

    std::unordered_set<SPItem *> seen;
    for (auto *item : candidate_parts) {
        CapturedCandidate candidate;
        auto skip = [&](SkippedPartReason reason, std::string detail) {
            candidate.skipped_reason = static_cast<int>(reason);
            candidate.skipped_detail = std::move(detail);
        };
        bool capture = false;
        if (!item) {
            skip(SkippedPartReason::NullItem, "selection contains a null item");
        } else if (item == container) {
            skip(SkippedPartReason::ContainerSelectedAsPart, "container is not also nested as a part");
        } else if (!seen.emplace(item).second) {
            skip(SkippedPartReason::DuplicateItem, "duplicate selected item");
        } else if (item->document != skeleton.document) {
            skip(SkippedPartReason::DifferentDocument, "part belongs to another document");
        } else if (item->isHidden() || item->isLocked()) {
            skip(SkippedPartReason::HiddenOrLocked, "part is hidden or locked");
        } else if (!finite(item->i2doc_affine())) {
            skip(SkippedPartReason::NonFiniteGeometry, "part has a non-finite transform");
        } else {
            capture = true;
        }
        if (capture) {
            candidate.captured = true;
            candidate.item = capture_item(item, context);
            result.candidates.push_back(identity(item));
        } else {
            CaptureResult::Identity id;
            id.item = SPWeakPtr<SPItem>(item);
            result.candidates.push_back(std::move(id));
        }
        input->candidates.push_back(std::move(candidate));
    }

    for (auto *item : obstacles) {
        CapturedObstacle obstacle;
        // Parts win over obstacles; the sheet and foreign or hidden objects are
        // never obstacles (collectSheetObstacles already guarantees this).
        if (!item || item == container || seen.contains(item) || item->document != skeleton.document ||
            item->isHidden()) {
            obstacle.ignored = true;
            result.obstacles.emplace_back();
        } else if (!finite(item->i2doc_affine())) {
            obstacle.error = "an object on the sheet has a non-finite transform: " + object_label(item);
            result.obstacles.emplace_back();
        } else {
            obstacle.item = capture_item(item, context);
            result.obstacles.push_back(identity(item));
        }
        input->obstacles.push_back(std::move(obstacle));
    }

    result.input = std::move(input);
    result.capture_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

CaptureResult captureDocumentNesting(SPItem *container, std::span<SPItem *const> candidate_parts,
                                     double flatten_tolerance, std::span<SPItem *const> obstacles)
{
    return capture_nesting(container ? container->document : nullptr, container, candidate_parts,
                           flatten_tolerance, obstacles);
}

PreparationResult prepareRequestNesting(SPDocument &document, std::span<SPItem *const> candidates,
    RequestPreparationOptions const &request, double tolerance, std::span<SPItem *const> obstacles)
{
    if (request.sheet.has_value() == request.page.has_value())
        return {.error = "supply exactly one request-local sheet or page", .reason = PreparationReason::InvalidSheet};
    auto sheet = request.sheet;
    if (request.page) {
        if (!*request.page) return {.error = "page is one-based", .reason = PreparationReason::InvalidSheet};
        auto const &pages = document.getPageManager().getPages();
        if (pages.empty() && *request.page == 1) {
            sheet = Geom::Rect(Geom::Point(0, 0), document.getDimensions());
        } else if (*request.page <= pages.size()) {
            sheet = pages[*request.page - 1]->getDocumentRect();
        } else return {.error = "page not found", .reason = PreparationReason::InvalidSheet};
    }
    if (!std::isfinite(sheet->left()) || !std::isfinite(sheet->top()) ||
        !std::isfinite(sheet->right()) || !std::isfinite(sheet->bottom()) ||
        sheet->width() <= 0 || sheet->height() <= 0)
        return {.error = "invalid request-local sheet", .reason = PreparationReason::InvalidSheet};
    if (!request.copies.empty() && request.copies.size() != candidates.size())
        return {.error = "copy counts must match candidates", .reason = PreparationReason::InvalidCopies};
    std::size_t count = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto copies = request.copies.empty() ? 1u : request.copies[i];
        if (!copies || copies > 100000 || count + copies > 100000)
            return {.error = "expanded copies exceed 100000", .reason = PreparationReason::ResourceLimit};
        count += copies;
    }
    auto capture = capture_nesting(&document, nullptr, candidates, tolerance, obstacles, sheet);
    if (!capture) return {.error = capture.error};
    auto input = std::make_shared<CapturedInput>(*capture.input);
    input->reject_conservative = request.reject_conservative;
    capture.input = input;
    auto geometry = prepareCapturedGeometry(*capture.input);
    if (!geometry) {
        PreparationResult failed{.error = geometry.error,
            .reason = geometry.conservative_refused ? PreparationReason::ConservativeRejected : geometry.no_usable_parts ? PreparationReason::NoUsableParts : PreparationReason::None};
        for (auto const &skip : geometry.skipped)
            failed.skipped_parts.push_back({SPWeakPtr<SPItem>(candidates[skip.candidate]), skip.reason, skip.detail});
        return failed;
    }
    if (request.reject_conservative && geometry.metrics.fallback_count)
        return {.error = "conservative capture forbidden by fallback=reject", .reason = PreparationReason::ConservativeRejected};
    // Expand polygons and their source identities before assembly. The solver
    // sees distinct native IDs; only apply creates positive-index copies.
    auto originals = std::move(geometry.parts);
    geometry.parts.clear();
    std::vector<std::uint32_t> copy_indices;
    std::uint64_t next_id = 1;
    geometry.metrics.selected_contour_area = 0;
    for (auto const &part : originals) {
        auto copies = request.copies.empty() ? 1u : request.copies[part.candidate];
        for (std::uint32_t copy = 0; copy < copies; ++copy) {
            auto expanded = part;
            expanded.id = next_id++;
            geometry.metrics.selected_contour_area += part_area(expanded.components);
            geometry.parts.push_back(std::move(expanded));
            copy_indices.push_back(copy);
        }
    }
    geometry.metrics.prepared_count = geometry.parts.size();
    auto prepared = assemblePreparedNesting(std::move(capture), std::move(geometry));
    if (prepared) {
        if (prepared.snapshot->parts.size() != copy_indices.size())
            return {.error = "copy assembly cardinality mismatch"};
        for (std::size_t i = 0; i < copy_indices.size(); ++i) prepared.snapshot->parts[i].copy = copy_indices[i];
        prepared.snapshot->apply_validation_options = request.solver_options;
    }
    return prepared;
}

CapturedGeometry prepareCapturedGeometry(CapturedInput const &input, std::stop_token stop,
                                         PreparationProgress const &progress)
{
    auto const started = std::chrono::steady_clock::now();
    auto const tolerance = input.flatten_tolerance;
    CapturedGeometry result;
    auto fail = [&](std::string error) {
        CapturedGeometry failed;
        failed.error = std::move(error);
        return failed;
    };

    ValidationBudget container_geometry_budget;
    auto container_geometry = contours_from_path(input.container_path, static_cast<SPWindRule>(input.container_wind_rule),
                                                 tolerance, true, &container_geometry_budget);
    if (!container_geometry) {
        if (container_geometry.too_complex)
            return fail("nesting container boundary is too complex to validate safely");
        // Name the failing object role: a container failure is never replaced by
        // a bounds/hull substitute, so the reason must identify the container.
        return fail("invalid nesting container: " + container_geometry.error);
    }
    {
        // Container geometry is authoritative: invalid containers fail with an
        // actionable reason and never receive a box/hull substitute.
        ValidationBudget container_budget;
        CollisionComponent container_component{.outer = container_geometry.geometry->outer,
                                               .holes = container_geometry.geometry->holes};
        auto const validity = validate_component(container_component, container_budget);
        if (validity == GeometryValidity::TooComplex)
            return fail("nesting container boundary is too complex to validate safely");
        if (validity != GeometryValidity::Valid)
            return fail("nesting container boundary is self-intersecting, degenerate or has invalid holes");
    }
    result.container_outline = container_geometry.geometry->outer;
    result.container_holes = container_geometry.geometry->holes;
    result.container_geometry_fingerprint = geometry_fingerprint(*container_geometry.geometry);
    auto &metrics = result.metrics;
    metrics.selected_count = input.candidates.size();
    metrics.container_usable_area = polygon_area(result.container_outline, result.container_holes);

    std::uint64_t next_id = 1;
    auto const total = input.candidates.size();
    for (std::size_t index = 0; index < total; ++index) {
        if (stop.stop_requested())
            return fail(statusMessage(Status::Cancelled));
        if (progress)
            progress(index, total);
        auto const &candidate = input.candidates[index];
        if (!candidate.captured) {
            result.skipped.push_back({.candidate = index,
                                      .reason = static_cast<SkippedPartReason>(candidate.skipped_reason),
                                      .detail = candidate.skipped_detail});
            continue;
        }
        auto const &item = candidate.item;
        // One bounded validation budget per top-level part, shared by the
        // double pass and the f32 conversion pass.
        ValidationBudget budget;
        auto geometry = prepare_part_components(item, tolerance, budget);
        if (!geometry) {
            auto const reason = geometry.too_complex ? SkippedPartReason::GeometryTooComplex
                                                     : SkippedPartReason::UnsafeRecovery;
            if (metrics.skipped_details.size() < MAX_RECOVERY_DETAILS) {
                metrics.skipped_details.push_back({item.label, geometry.error});
            }
            result.skipped.push_back({.candidate = index, .reason = reason, .detail = std::move(geometry.error)});
            continue;
        }
        if (input.reject_conservative &&
            (geometry.geometry->source == ContourSource::ConservativeHull ||
             geometry.geometry->source == ContourSource::ConservativeBounds ||
             geometry.geometry->recovery == RecoveryKind::ConservativeFallback))
            return {.error="conservative capture forbidden by fallback=reject", .conservative_refused=true};
        auto const fingerprint = geometry_fingerprint(*geometry.geometry);
        result.parts.push_back({
            .candidate = index,
            .id = next_id++,
            .components = std::move(geometry.geometry->components),
            .source = geometry.geometry->source,
            .geometry_fingerprint = fingerprint,
            .recovery = geometry.geometry->recovery,
            .recovery_reason = geometry.geometry->recovery_reason,
        });
        auto const &prepared = result.parts.back();
        if (prepared.recovery == RecoveryKind::ConservativeFallback) {
            ++metrics.fallback_count;
        } else if (prepared.recovery == RecoveryKind::Repaired) {
            ++metrics.repaired_count;
        }
        if (prepared.recovery != RecoveryKind::Clean && metrics.recovery_details.size() < MAX_RECOVERY_DETAILS) {
            metrics.recovery_details.push_back({item.label, prepared.recovery_reason});
        }
        metrics.selected_contour_area += part_area(prepared.components);
        // An explicit contour or a clip is a deliberate cut outline; only
        // inferred vector outlines can be unexpectedly small.
        if (prepared.source != ContourSource::ExplicitContour && prepared.source != ContourSource::VectorClip &&
            item.ignore_bitmaps) {
            if (auto const &bitmaps = item.ignored_bitmap_bounds; bitmaps && bitmaps->area() > 0.0) {
                ++metrics.ignored_bitmap_part_count;
                if (filled_footprint_area(prepared.components) < SPARSE_VECTOR_AREA_RATIO * bitmaps->area()) {
                    ++metrics.sparse_vector_count;
                    if (metrics.sparse_vector_details.size() < MAX_RECOVERY_DETAILS) {
                        metrics.sparse_vector_details.push_back(
                            {item.label, "vector outline covers less than half of its bitmaps"});
                    }
                }
            }
        }
        auto const source_index = static_cast<std::size_t>(prepared.source);
        if (source_index < metrics.contour_source_counts.size()) {
            ++metrics.contour_source_counts[source_index];
        }
    }
    if (progress)
        progress(total, total);

    for (std::size_t index = 0; index < input.obstacles.size(); ++index) {
        if (stop.stop_requested())
            return fail(statusMessage(Status::Cancelled));
        auto const &obstacle = input.obstacles[index];
        if (obstacle.ignored)
            continue;
        if (obstacle.error)
            return fail(*obstacle.error);
        ValidationBudget budget;
        auto geometry = prepare_part_components(obstacle.item, tolerance, budget);
        if (!geometry) {
            return fail("an object on the sheet cannot be measured, so parts could overlap it: " +
                        obstacle.item.label + " (" + geometry.error + "). Move it off the sheet or hide it.");
        }
        if (input.reject_conservative &&
            (geometry.geometry->source == ContourSource::ConservativeHull ||
             geometry.geometry->source == ContourSource::ConservativeBounds ||
             geometry.geometry->recovery == RecoveryKind::ConservativeFallback))
            return {.error="conservative obstacle forbidden by fallback=reject", .conservative_refused=true};
        metrics.obstacle_area += filled_footprint_area(geometry.geometry->components);
        auto const obstacle_geometry = geometry_fingerprint(*geometry.geometry);
        result.obstacles.push_back({.index = index,
                                    .components = std::move(geometry.geometry->components),
                                    .geometry_fingerprint = obstacle_geometry});
    }
    metrics.obstacle_count = result.obstacles.size();

    if (result.parts.empty()) {
        result.error = "selection contains no nestable parts";
        result.no_usable_parts = true;
        return result;
    }
    metrics.prepared_count = result.parts.size();
    metrics.skipped_count = result.skipped.size();
    metrics.geometry_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

PreparationResult assemblePreparedNesting(CaptureResult &&capture, CapturedGeometry &&geometry)
{
    if (!capture)
        return {.error = std::move(capture.error)};
    if (!geometry)
        return {.error = std::move(geometry.error)};
    auto snapshot = std::move(capture.skeleton);
    snapshot.container_outline = std::move(geometry.container_outline);
    snapshot.container_holes = std::move(geometry.container_holes);
    snapshot.container_geometry_fingerprint = geometry.container_geometry_fingerprint;
    snapshot.metrics = std::move(geometry.metrics);
    snapshot.metrics.capture_seconds = capture.capture_seconds;
    snapshot.metrics.elapsed_seconds = snapshot.metrics.capture_seconds + snapshot.metrics.geometry_seconds;

    // Parts and skipped entries in selection order (both carry the candidate).
    auto part = geometry.parts.begin();
    auto skip = geometry.skipped.begin();
    for (std::size_t index = 0; index < capture.candidates.size(); ++index) {
        auto &id = capture.candidates[index];
        while (part != geometry.parts.end() && part->candidate == index) {
            snapshot.parts.push_back({
                .id = part->id,
                .item = id.item,
                .original_item_to_document = id.item_to_document,
                .components = std::move(part->components),
                .contour_source = part->source,
                .geometry_fingerprint = part->geometry_fingerprint,
                .content_fingerprint = id.content_fingerprint,
                .recovery = part->recovery,
                .recovery_reason = std::move(part->recovery_reason),
                .image_alpha_hashes = id.image_alpha_hashes,
                .always_revalidate = id.always_revalidate,
            });
            ++part;
        }
        if (skip != geometry.skipped.end() && skip->candidate == index) {
            snapshot.skipped_parts.push_back({id.item, skip->reason, std::move(skip->detail)});
            ++skip;
        }
    }
    for (auto &obstacle : geometry.obstacles) {
        auto &id = capture.obstacles[obstacle.index];
        snapshot.obstacles.push_back({
            .item = id.item,
            .item_to_document = id.item_to_document,
            .components = std::move(obstacle.components),
            .content_fingerprint = id.content_fingerprint,
            .geometry_fingerprint = obstacle.geometry_fingerprint,
            .image_alpha_hashes = std::move(id.image_alpha_hashes),
            .always_revalidate = id.always_revalidate,
        });
    }
    return {.snapshot = std::move(snapshot)};
}

PreparedDocumentNesting solvingSnapshot(SPDocument *document, CapturedInput const &input, CapturedGeometry const &geometry)
{
    PreparedDocumentNesting snapshot;
    snapshot.document = document;
    snapshot.container_outline = geometry.container_outline;
    snapshot.container_holes = geometry.container_holes;
    snapshot.container_geometry_fingerprint = geometry.container_geometry_fingerprint;
    snapshot.flatten_tolerance = input.flatten_tolerance;
    snapshot.metrics = geometry.metrics;
    for (auto const &part : geometry.parts) {
        PreparedPart prepared;
        prepared.id = part.id;
        prepared.components = part.components;
        prepared.contour_source = part.source;
        prepared.geometry_fingerprint = part.geometry_fingerprint;
        prepared.recovery = part.recovery;
        prepared.recovery_reason = part.recovery_reason;
        snapshot.parts.push_back(std::move(prepared));
    }
    for (auto const &obstacle : geometry.obstacles) {
        PreparedObstacle prepared;
        prepared.components = obstacle.components;
        prepared.geometry_fingerprint = obstacle.geometry_fingerprint;
        snapshot.obstacles.push_back(std::move(prepared));
    }
    return snapshot;
}

PreparationResult prepareDocumentNesting(SPItem *container, std::span<SPItem *const> candidate_parts,
                                         double flatten_tolerance, std::span<SPItem *const> obstacles)
{
    auto capture = captureDocumentNesting(container, candidate_parts, flatten_tolerance, obstacles);
    if (!capture)
        return {.error = std::move(capture.error)};
    auto geometry = prepareCapturedGeometry(*capture.input);
    return assemblePreparedNesting(std::move(capture), std::move(geometry));
}

static SolveResult solveNativeNesting(PreparedDocumentNesting const &snapshot, Options const &options,
                                 Job::ProgressCallback const &progress, std::stop_token cancellation,
                                 std::uint64_t work_limit = 0)
{
    auto const started = std::chrono::steady_clock::now();
    if (!snapshot.document || snapshot.container_outline.size() < 3 || snapshot.parts.empty()) {
        return {.status = Status::InvalidArgument, .error = "nesting snapshot is incomplete"};
    }
    if (cancellation.stop_requested()) {
        return {.status = Status::Cancelled, .error = statusMessage(Status::Cancelled),
                .terminal = TerminalResult{StopReason::Cancelled, 0}, .deterministic = work_limit != 0};
    }

    try {
        Job job(options);
        auto work_status = job.setWorkLimit(work_limit);
        if (work_status != Status::Ok) return {.status = work_status, .error = job.error()};
        std::stop_callback cancel_job(cancellation, [&job] { job.cancel(); });
        auto status = job.setContainer(snapshot.container_outline);
        if (status != Status::Ok)
            return {.status = status, .error = job.error()};
        for (auto const &hole : snapshot.container_holes) {
            status = job.addContainerHole(hole);
            if (status != Status::Ok)
                return {.status = status, .error = job.error()};
        }
        for (auto const &obstacle : snapshot.obstacles) {
            for (auto const &component : obstacle.components) {
                status = job.addObstacle(component.outer);
                if (status != Status::Ok)
                    return {.status = status, .error = job.error()};
            }
        }
        for (auto const &part : snapshot.parts) {
            status = job.addPart(part.id, part.components);
            if (status != Status::Ok)
                return {.status = status, .error = job.error()};
        }
        Progress latest_progress;
        std::optional<double> initial_solution_seconds;
        status = job.run([&](Progress const &value) {
            latest_progress = value;
            if (!value.placements.empty() && !initial_solution_seconds) {
                initial_solution_seconds = value.elapsed_seconds;
            }
            if (cancellation.stop_requested()) {
                job.cancel();
            } else if (progress) {
                progress(value);
            }
        });
        auto terminal = job.terminalResult();
        auto const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        SolveMetrics metrics{
            .iterations = work_limit && terminal ? terminal->completed_work : latest_progress.iteration,
            .placed_count = latest_progress.placed_count,
            .placed_contour_area = latest_progress.best_score,
            .utilization_percent = snapshot.metrics.container_usable_area > 0.0
                                       ? latest_progress.best_score / snapshot.metrics.container_usable_area * 100.0
                                       : 0.0,
            .initial_solution_seconds = initial_solution_seconds.value_or(elapsed),
            .refinement_seconds = initial_solution_seconds ? std::max(0.0, elapsed - *initial_solution_seconds) : 0.0,
            .elapsed_seconds = elapsed,
        };
        return {
            .status = status,
            .placements = status == Status::Ok ? job.results() : std::vector<Placement>{},
            .error = status == Status::Ok ? std::string{} : job.error(),
            .metrics = metrics,
            .terminal = terminal,
            .deterministic = work_limit != 0,
        };
    } catch (std::exception const &exception) {
        return {.status = Status::InternalError, .error = exception.what()};
    } catch (...) {
        return {.status = Status::InternalError, .error = "unknown exception while solving nesting job"};
    }
}

SolveResult solvePreparedNesting(PreparedDocumentNesting const &snapshot, Options const &options,
                                 EngineSelection engine, std::uint64_t work_limit,
                                 Job::ProgressCallback const &progress, std::stop_token cancellation)
{
    if (work_limit && (engine != EngineSelection::NativeOnly || options.worker_count != 1 || options.time_limit_ms))
        return {.status = Status::InvalidArgument, .error = "fixed work requires native-only, one worker and no deadline"};
    if (engine == EngineSelection::NativeOnly)
        return solveNativeNesting(snapshot, options, progress, cancellation, work_limit);
    return solvePreparedNesting(snapshot, options, progress, cancellation);
}

SolveResult solvePreparedNesting(PreparedDocumentNesting const &snapshot, Options const &options,
                                 Job::ProgressCallback const &progress, std::stop_token cancellation)
{
    if (cancellation.stop_requested()) return {.status=Status::Cancelled};
    if (!snapshot.document || !sparrowEligible(snapshot,options) || !sparrowAvailable())
        return solveNativeNesting(snapshot,options,progress,cancellation);
    auto const started=std::chrono::steady_clock::now();
    auto const deadline=started+std::chrono::milliseconds(options.time_limit_ms);
    try {
        // Validate options before starting either lane.
        Job option_check(options);
        auto sparrow=std::async(std::launch::async,[&] { return solveSparrow(snapshot,options,deadline,cancellation); });
        // Sparrow takes the cores the native lanes leave free (sparrowWorkers()).
        // With at least three hardware threads keep the native two-lane
        // portfolio (worker_count 0 = engine choice); otherwise use the single
        // baseline lane to avoid oversubscription.
        auto native_options=options;
        if (std::thread::hardware_concurrency()<3) native_options.worker_count=1;
        std::stop_source native_stop;
        std::stop_callback cancel_native(cancellation,[&] { native_stop.request_stop(); });
        std::vector<Placement> incumbent;
        std::atomic<bool> native_has_layout{false};
        SolveResult native;
        bool deadline_reached=false;
        {
            // Native preparation is cooperative, not a killable subprocess.
            // Independent cancellation reaches it even between progress reports.
            // The engine always completes its first layout, even after the
            // deadline; the limit bounds improvement only. Stop an overrunning
            // native lane only after it has published a complete layout, so slow
            // preparation can no longer end with nothing placed.
            std::jthread watchdog([&](std::stop_token stop) {
                std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
                auto wake_at=deadline+std::chrono::milliseconds(250);
                while (!wake.wait_until(lock,stop,wake_at,[]{return false;}) && !stop.stop_requested()) {
                    if (native_has_layout.load(std::memory_order_acquire)) {
                        deadline_reached=true; native_stop.request_stop(); return;
                    }
                    wake_at=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
                }
            });
            native=solveNativeNesting(snapshot,native_options,[&](Progress const &value) {
                if (!value.placements.empty()) {
                    incumbent=value.placements;
                    native_has_layout.store(true,std::memory_order_release);
                }
                if (progress && !cancellation.stop_requested()) progress(value);
            },native_stop.get_token());
        } // join before observing watchdog state or releasing the job/snapshot
        auto candidate=sparrow.get(); // always wait/reap, including after Cancel
        if (cancellation.stop_requested()) return {.status=Status::Cancelled};
        if (deadline_reached && !native && !incumbent.empty())
            native={.status=Status::Ok,.placements=std::move(incumbent)};

        auto valid=[&](SolveResult const &result) {
            if (!result || result.placements.size()!=snapshot.parts.size()) return false;
            auto validation_options=options; validation_options.worker_count=1;
            Job validator(validation_options);
            std::stop_callback cancel_validator(cancellation,[&] { validator.cancel(); });
            if (validator.setContainer(snapshot.container_outline)!=Status::Ok) return false;
            for (auto const &hole:snapshot.container_holes)
                if (validator.addContainerHole(hole)!=Status::Ok) return false;
            for (auto const &obstacle : snapshot.obstacles)
                for (auto const &component : obstacle.components)
                    if (validator.addObstacle(component.outer)!=Status::Ok) return false;
            for (auto const &part:snapshot.parts)
                if (validator.addPart(part.id,part.components)!=Status::Ok) return false;
            return validator.validate(result.placements)==Status::Ok;
        };
        bool native_valid=valid(native), sparrow_valid=valid(candidate);
        if (cancellation.stop_requested()) return {.status=Status::Cancelled};
        if (!native_valid && !sparrow_valid)
            return {.status=Status::InternalError,.error="Neither nesting candidate passed geometry validation. "+candidate.error};
        bool use_sparrow=sparrow_valid && (!native_valid || betterNestingCandidate(snapshot,candidate.placements,native.placements));
        // Read before `candidate` may be moved into the result.
        auto const sparrow_detail=candidate.backend_detail.empty() ? std::string{} : " ("+candidate.backend_detail+")";
        auto result=use_sparrow ? std::move(candidate) : std::move(native);
        result.backend=use_sparrow ? "sparrow" : "native";
        result.backend_detail=use_sparrow ? "Validated Sparrow candidate"+sparrow_detail :
            sparrow_valid ? "Native candidate wins or ties"+sparrow_detail : "Sparrow rejected or unavailable: "+candidate.error;
        result.metrics.placed_count=std::count_if(result.placements.begin(),result.placements.end(),[](auto const &p){return p.placed;});
        result.metrics.placed_contour_area=capturedNestingArea(snapshot,result.placements);
        result.metrics.utilization_percent=snapshot.metrics.container_usable_area>0 ?
            100*result.metrics.placed_contour_area/snapshot.metrics.container_usable_area : 0;
        result.metrics.elapsed_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        if (cancellation.stop_requested()) return {.status=Status::Cancelled};
        return result;
    } catch (std::exception const &e) {
        return {.status=cancellation.stop_requested()?Status::Cancelled:Status::InternalError,.error=e.what()};
    }
}

ApplyResult applyNestingPlacements(PreparedDocumentNesting const &snapshot, std::span<Placement const> placements,
                                   LeftoverPlacement const &leftovers,
                                   std::shared_ptr<void> const &owner_lease)
{
    ApplyResult result;
    result.skipped_count = snapshot.skipped_parts.size();
    if (!snapshot.document || placements.size() != snapshot.parts.size()) {
        result.error = "solver result does not contain exactly one entry per prepared part";
        return result;
    }

    std::unordered_map<std::uint64_t, Placement const *> by_id;
    by_id.reserve(placements.size());
    for (auto const &placement : placements) {
        if (!std::isfinite(placement.translation_x) || !std::isfinite(placement.translation_y) ||
            !std::isfinite(placement.rotation_degrees) || !by_id.emplace(placement.part_id, &placement).second) {
            result.error = "solver result contains non-finite values or duplicate part IDs";
            return result;
        }
    }
    for (auto const &part : snapshot.parts) {
        if (!by_id.contains(part.id)) {
            result.error = "solver result is missing a prepared part ID";
            return result;
        }
    }

    if (snapshot.apply_validation_options) {
        auto options = *snapshot.apply_validation_options;
        options.time_limit_ms = 0;
        options.worker_count = 1;
        Job validator(options);
        auto status = validator.setContainer(snapshot.container_outline);
        for (auto const &hole : snapshot.container_holes)
            if (status == Status::Ok) status = validator.addContainerHole(hole);
        for (auto const &obstacle : snapshot.obstacles)
            for (auto const &component : obstacle.components)
                if (status == Status::Ok) status = validator.addObstacle(component.outer);
        for (auto const &part : snapshot.parts)
            if (status == Status::Ok) status = validator.addPart(part.id, part.components);
        if (status != Status::Ok || validator.validate(placements) != Status::Ok) {
            result.error = "native placement validation failed: " + validator.error();
            return result;
        }
    }
    auto const validation_started = std::chrono::steady_clock::now();
    bool const fresh = geometry_still_matches(snapshot, result.revalidated_count);
    result.validation_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - validation_started).count();
    if (!fresh) {
        result.status = ApplyStatus::StaleSnapshot;
        result.error = "document changed while nesting was being solved";
        return result;
    }

    struct PendingTransform
    {
        SPItem *item = nullptr;
        Geom::Affine item_to_document;
        bool duplicate = false;
        std::uint64_t part_id = 0;
    };
    std::vector<PendingTransform> pending;
    pending.reserve(snapshot.parts.size());
    for (auto const &part : snapshot.parts) {
        auto const &placement = *by_id.at(part.id);
        if (!placement.placed) {
            ++result.unplaced_count;
            continue;
        }
        ++result.placed_count;
        auto const transform = Geom::Rotate::from_degrees(placement.rotation_degrees) *
                               Geom::Translate(placement.translation_x, placement.translation_y);
        auto const item_to_document = part.original_item_to_document * transform;
        if (part.copy || !affine_near(item_to_document, part.original_item_to_document)) {
            pending.push_back({part.item.get(), item_to_document, part.copy != 0, part.id});
        }
    }
    if (leftovers.move_beside_container) {
        // Columns to the right of the sheet, top-aligned with it, in the
        // snapshot's part order (the selection order at click time). Only the
        // top-level translation changes. The staging area is not checked for
        // other artwork (known limitation, like gap G1).
        // Visual bounds, like the parts': a leftover never sits on the sheet's
        // stroke. The freshness check above guarantees the container exists.
        Geom::OptRect sheet;
        if (snapshot.container) sheet = snapshot.container.get()->documentVisualBounds();
        if (!sheet) {
            for (auto const &point : snapshot.container_outline) {
                sheet.expandTo(Geom::Point(point.x, point.y));
            }
        }
        auto const gap = std::isfinite(leftovers.gap) ? std::max(leftovers.gap, 0.0) : 0.0;
        if (sheet) {
            double x = sheet->right() + gap;
            if (std::isfinite(leftovers.start_x))
                x = std::max(x, leftovers.start_x);
            double y = sheet->top();
            double column_width = 0.0;
            for (auto const &part : snapshot.parts) {
                if (part.copy || by_id.at(part.id)->placed)
                    continue;
                // Visual extent at the part's current (= original) transform,
                // in document coordinates like original_item_to_document.
                auto const bounds = part.item->documentVisualBounds();
                if (!bounds)
                    continue;
                // Start a new column when this part would pass the sheet's
                // bottom, unless it is the first part of the column.
                if (y > sheet->top() && y + bounds->height() > sheet->bottom()) {
                    x += column_width + gap;
                    y = sheet->top();
                    column_width = 0.0;
                }
                auto const item_to_document =
                    part.original_item_to_document * Geom::Translate(x - bounds->left(), y - bounds->top());
                // A huge gap can overflow; such a part stays where it is.
                if (finite(item_to_document) && !affine_near(item_to_document, part.original_item_to_document)) {
                    pending.push_back({part.item.get(), item_to_document});
                    ++result.leftover_moved_count;
                }
                y += bounds->height() + gap;
                column_width = std::max(column_width, bounds->width());
            }
        }
    }
    if (pending.empty()) {
        result.status = ApplyStatus::NoChange;
        return result;
    }

    auto interaction = snapshot.apply_validation_options
        ? (owner_lease ? DocumentUndo::beginAtomicCommandInteraction(snapshot.document, owner_lease)
                       : DocumentUndo::beginAtomicInteraction(snapshot.document))
        : DocumentUndo::beginRollbackableInteraction(snapshot.document);
    if (!interaction) {
        result.status = ApplyStatus::UndoUnavailable;
        result.error = "another rollbackable document interaction is active";
        return result;
    }

    try {
    // Duplicate before moving any source, so every copy starts with the
    // captured payload. Document attachment assigns unique subtree IDs.
    for (auto &change : pending) {
        if (!change.duplicate) continue;
        auto *source = change.item->getRepr();
        // Reuse import's reference-aware ID remapping in a disposable document.
        // Attaching raw duplicates alone renames IDs but leaves internal clones
        // and cutline references pointing back into the original source.
        constexpr std::string_view empty_svg =
            R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"/>)svg";
        auto fragment = SPDocument::createNewDocFromMem(std::span<char const>(empty_svg.data(), empty_svg.size()));
        if (!fragment) {
            interaction->rollback();
            result.status = ApplyStatus::PublicationFailed;
            result.error = "could not stage nesting copy";
            return result;
        }
        auto *staged = source->duplicate(fragment->getReprDoc());
        fragment->getReprRoot()->appendChild(staged);
        Inkscape::GC::release(staged);
        prevent_id_clashes(fragment.get(), snapshot.document, IdClashPolicy::RenameAllCollisions);
        auto *copy = staged->duplicate(snapshot.document->getReprDoc());
        source->parent()->addChild(copy, source);
        auto *created = cast<SPItem>(snapshot.document->getObjectByRepr(copy));
        Inkscape::GC::release(copy);
        if (!created) {
            interaction->rollback();
            result.status = ApplyStatus::PublicationFailed;
            result.error = "could not publish nesting copy";
            return result;
        }
        change.item = created;
    }
    for (auto const &change : pending) {
        auto *item = change.item;
        // Keep the solved placement in the item's SVG transform attribute.
        // doWriteTransform() lets optimized items such as SPPath call
        // set_transform(), which multiplies the affine into the path data and
        // drops the attribute (see SPPath::set_transform); the nesting contract
        // allows only the top-level transform to change, with original
        // d/style/children preserved. set_i2d_affine() derives exactly the
        // parent-relative affine (see SPItem::set_i2d_affine), so serializing
        // that same value cannot double-apply it, and descendants, clip/mask
        // and referenced paint keep group-matrix semantics under the new
        // transform.
        Geom::Affine const previous_repr_matrix = sp_item_transform_repr(item);
        item->set_i2d_affine(change.item_to_document * snapshot.document->doc2dt());
        Geom::Affine const parent_relative = item->transform;
        // The transform attribute is written before the signal, so clone and
        // connector listeners observe the new parent-relative matrix. The
        // advertized delta is the relative local change from the previous repr
        // matrix to the new parent-relative matrix: exactly the delta
        // doWriteTransform() reports. updateRepr() is deliberately not called;
        // it rewrites unrelated authored attributes (for example it
        // canonicalizes path data), while the nesting contract preserves the
        // original d/style/children.
        Geom::Affine const relative = previous_repr_matrix.inverse() * parent_relative;
        item->getRepr()->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(parent_relative));
        item->_transformed_signal.emit(&relative, item);
    }
    if (snapshot.apply_validation_options) {
        bool committed = interaction->commitAtomically(Util::Internal::ContextString{"Nest objects"},
            INKSCAPE_ICON("tool-nesting"), [&] {
                return std::all_of(pending.begin(), pending.end(), [&](auto const &change) {
                    return change.item && change.item->document == snapshot.document &&
                           affine_near(change.item->i2doc_affine(), change.item_to_document);
                });
            });
        if (!committed) {
            interaction->rollback();
            result.status = ApplyStatus::PublicationFailed;
            result.error = "atomic nesting publication refused";
            return result;
        }
    } else {
        interaction->commit(Util::Internal::ContextString{"Nest objects"}, INKSCAPE_ICON("tool-nesting"));
    }
    } catch (...) {
        interaction->rollback();
        result.status = ApplyStatus::PublicationFailed;
            result.error = "atomic nesting publication refused";
        return result;
    }
    snapshot.document->ensureUpToDate();
    result.status = ApplyStatus::Applied;
    result.moved_count = pending.size();
    for (auto const &change : pending)
        if (change.duplicate && change.item->getId())
            result.created_copies.emplace_back(change.part_id, change.item->getId());
    return result;
}

} // namespace Inkscape::Nesting
