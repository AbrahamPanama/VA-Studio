// SPDX-License-Identifier: GPL-2.0-or-later
#include "fillet-chamfer-edit.h"
#include <algorithm>
#include <cmath>
#include <set>
#include "helper/geom.h"
#include "helper/geom-curves.h"

namespace Inkscape::LivePathEffect::CornerEdit {
namespace {
bool finite(Geom::Point const &p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]);
}

bool near(double a, double b)
{
    return std::abs(a - b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)});
}

bool equal_satellite(NodeSatellite const &a, NodeSatellite const &b)
{
    return a.nodesatellite_type == b.nodesatellite_type && a.is_time == b.is_time &&
           a.selected == b.selected && a.has_mirror == b.has_mirror && a.hidden == b.hidden &&
           a.amount == b.amount && a.angle == b.angle && a.steps == b.steps;
}

std::size_t previous(Geom::Path const &path, std::size_t node)
{
    return node ? node - 1 : count_path_curves(path) - 1;
}

bool valid_address(Snapshot const &s, Address a)
{
    return a.path < s.satellites.size() && a.node < s.satellites[a.path].size();
}

std::optional<std::vector<Address>> targets(Snapshot const &s, Scope scope)
{
    if (scope != Scope::Selected && scope != Scope::All) return {};
    if (!bounded_shape(s.path, s.satellites) || s.selected.size() > max_nodes || s.smooth.size() > max_nodes) return {};
    for (auto a : s.selected) if (!valid_address(s, a)) return {};
    for (auto a : s.smooth) if (!valid_address(s, a)) return {};
    std::set<Address> selected(s.selected.begin(), s.selected.end());
    std::set<Address> smooth(s.smooth.begin(), s.smooth.end());
    std::vector<Address> result;
    for (std::size_t p = 0; p < s.path.size(); ++p) {
        auto const &path = s.path[p];
        if (path.empty()) continue;
        auto n = count_path_curves(path);
        for (std::size_t j = path.closed() ? 0 : 1; j < n; ++j) {
            Address a{p, j};
            if ((scope == Scope::Selected && !selected.contains(a)) || smooth.contains(a)) continue;
            auto const &in = path[previous(path, j)];
            auto const &out = path[j];
            if (in.isDegenerate() || out.isDegenerate()) continue;
            auto u = in.unitTangentAt(1);
            auto v = out.unitTangentAt(0);
            if (!finite(u) || !finite(v)) return {};
            // Collinear straight/smooth joins and reversals are not corners.
            // Geometrically smooth joins are excluded even with a cusp XML tag.
            if (std::abs(Geom::cross(u, v)) <= 1e-9) continue;
            result.push_back(a);
        }
    }
    return result;
}

bool curved(Snapshot const &s, Address a)
{
    auto const &path = s.path[a.path];
    return !is_straight_curve(path[previous(path, a.node)]) || !is_straight_curve(path[a.node]);
}
} // namespace

bool bounded_shape(Geom::PathVector const &path, NodeSatellites const &satellites)
{
    if (path.size() > max_nodes || path.size() != satellites.size()) return false;
    std::size_t total = 0;
    for (std::size_t p = 0; p < path.size(); ++p) {
        auto n = path[p].empty() ? 0 : count_path_nodes(path[p]);
        if (n > max_nodes - total || n != satellites[p].size()) return false;
        total += n;
        for (auto const &curve : path[p]) {
            auto bounds = curve.boundsFast();
            if (!finite(bounds.min()) || !finite(bounds.max())) return false;
        }
        for (auto const &sat : satellites[p]) {
            if (!std::isfinite(sat.amount) || sat.amount < 0 || !std::isfinite(sat.angle) ||
                sat.nodesatellite_type < FILLET || sat.nodesatellite_type > INVALID_SATELLITE) return false;
            // Mixing absolute/time satellites is controlled by the LPE's global
            // flexible parameter; changing it here would alter untouched nodes.
            if (sat.is_time) return false;
        }
    }
    return true;
}

bool equal(NodeSatellites const &a, NodeSatellites const &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t p = 0; p < a.size(); ++p) {
        if (a[p].size() != b[p].size()) return false;
        for (std::size_t j = 0; j < a[p].size(); ++j) if (!equal_satellite(a[p][j], b[p][j])) return false;
    }
    return true;
}

Summary inspect(Snapshot const &s, Scope scope)
{
    Summary result;
    auto list = targets(s, scope);
    if (!list) return result;
    result.count = list->size();
    result.status = list->empty() ? Status::NoCorners : Status::Ready;
    bool first = true;
    for (auto a : *list) {
        auto const &path = s.path[a.path];
        auto const &sat = s.satellites[a.path][a.node];
        std::optional<Mode> mode;
        if (sat.nodesatellite_type == FILLET) mode = Mode::Round;
        if (sat.nodesatellite_type == INVERSE_FILLET) mode = Mode::InverseRound;
        auto radius = sat.amount == 0 ? 0 : sat.lenToRad(sat.amount, path[previous(path, a.node)],
                                  path[a.node], s.satellites[a.path][previous(path, a.node)]);
        if (!std::isfinite(radius) || radius < 0) return {};
        if (first) {
            result.mode = mode;
            result.radius = radius;
        } else {
            if (result.mode != mode) result.mode.reset();
            if (result.radius && !near(*result.radius, radius)) result.radius.reset();
        }
        first = false;
        result.approximate |= curved(s, a);
    }
    return result;
}

Plan prepare(Snapshot const &s, Request const &request)
{
    Plan result;
    if (request.radius && (!std::isfinite(*request.radius) || *request.radius < 0)) return result;
    if (request.mode && *request.mode != Mode::Round && *request.mode != Mode::InverseRound) return result;
    auto list = targets(s, request.scope);
    if (!list) return result;
    result.count = list->size();
    if (list->empty()) { result.status = Status::NoCorners; return result; }
    auto after = s.satellites;
    for (auto a : *list) {
        auto &sat = after[a.path][a.node];
        auto const &path = s.path[a.path];
        if (request.radius) {
            auto readback = sat.amount == 0 ? 0 : sat.lenToRad(sat.amount, path[previous(path, a.node)],
                                      path[a.node], s.satellites[a.path][previous(path, a.node)]);
            // Native approximate radius→distance→radius is not an exact inverse.
            // Applying the exact readout must preserve the stored distance.
            auto amount = *request.radius == 0 ? 0 : *request.radius == readback ? sat.amount :
                          sat.radToLen(*request.radius, path[previous(path, a.node)], path[a.node]);
            if (!std::isfinite(amount) || amount < 0) return result;
            // Native conversion reports an unattainable radius as zero. Do not
            // silently turn a nonzero request into an unrounded corner.
            if (*request.radius > 0 && amount == 0) { result.status = Status::EngineLimit; return result; }
            sat.amount = amount;
        }
        if (request.mode) sat.nodesatellite_type = *request.mode == Mode::Round ? FILLET : INVERSE_FILLET;
        result.approximate |= curved(s, a);
    }
    if (equal(s.satellites, after)) {
        result.status = Status::NoChange;
        result.satellites = std::move(after);
        return result;
    }
    // The native renderer clips overlapping satellite extents sequentially.
    // Refuse that edit instead of changing an untouched neighbour's appearance.
    // Validate only edges adjacent to a target, not unrelated legacy corners.
    std::set<Address> edges;
    for (auto a : *list) {
        edges.insert(a);
        edges.insert({a.path, previous(s.path[a.path], a.node)});
    }
    for (auto a : edges) {
        auto const &path = s.path[a.path];
        auto n = count_path_curves(path);
        auto next = a.node + 1;
        if (next == n && path.closed()) next = 0;
        auto length = path[a.node].length();
        if (!std::isfinite(length) || length <= 0) return result;
        auto sum = after[a.path][a.node].amount + after[a.path][next].amount;
        if (!std::isfinite(sum) || sum > length) { result.status = Status::EngineLimit; return result; }
    }
    result.status = equal(s.satellites, after) ? Status::NoChange : Status::Ready;
    result.satellites = std::move(after);
    return result;
}

std::optional<double> document_scale(Geom::Affine const &a)
{
    for (unsigned i = 0; i < 6; ++i) if (!std::isfinite(a[i])) return {};
    double x = std::hypot(a[0], a[1]);
    double y = std::hypot(a[2], a[3]);
    if (!(x > 0) || !(y > 0) || !std::isfinite(x) || !std::isfinite(y)) return {};
    double ratio = x / y;
    double dot = (a[0] / x) * (a[2] / y) + (a[1] / x) * (a[3] / y);
    if (std::abs(ratio - 1) > 1e-9 || std::abs(dot) > 1e-9) return {};
    return x;
}
} // namespace Inkscape::LivePathEffect::CornerEdit
