// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui/tools/nesting-preview.h"

#include <unordered_map>
#include <2geom/path.h>
#include <2geom/rect.h>
#include <2geom/transforms.h>

namespace Inkscape::UI::Tools {

namespace {

void append_contour(Geom::PathVector &pathvector, std::span<Nesting::Point const> points)
{
    if (points.size() < 3) {
        return;
    }
    Geom::Path path(Geom::Point(points.front().x, points.front().y));
    for (auto const &point : points.subspan(1)) {
        path.appendNew<Geom::LineSegment>(Geom::Point(point.x, point.y));
    }
    path.close(true);
    pathvector.push_back(std::move(path));
}

} // namespace

NestingPreviewModel::NestingPreviewModel(Nesting::PreparedDocumentNesting const &snapshot)
{
    std::size_t points = 0;
    for (auto const &part : snapshot.parts) {
        for (auto const &component : part.components) {
            points += component.outer.size();
            for (auto const &hole : component.holes) {
                points += hole.size();
            }
        }
    }
    _uses_bounds = points > POINT_BUDGET;

    _parts.reserve(snapshot.parts.size());
    for (auto const &part : snapshot.parts) {
        Geom::PathVector local;
        if (_uses_bounds) {
            // Transformed by the placement, the rectangle still contains the
            // transformed part: the preview stays conservative.
            Geom::OptRect bounds;
            for (auto const &component : part.components) {
                for (auto const &point : component.outer) {
                    bounds.expandTo(Geom::Point(point.x, point.y));
                }
            }
            if (bounds) {
                local.push_back(Geom::Path(*bounds));
            }
        } else {
            for (auto const &component : part.components) {
                append_contour(local, component.outer);
                for (auto const &hole : component.holes) {
                    append_contour(local, hole);
                }
            }
        }
        _parts.push_back({.id = part.id, .local = std::move(local), .shown = std::nullopt});
    }
}

std::vector<NestingPreviewModel::Change> NestingPreviewModel::update(std::span<Nesting::Placement const> placements)
{
    std::unordered_map<std::uint64_t, Nesting::Placement const *> by_id;
    by_id.reserve(placements.size());
    for (auto const &placement : placements) {
        by_id.emplace(placement.part_id, &placement);
    }

    std::vector<Change> changes;
    for (std::size_t index = 0; index < _parts.size(); ++index) {
        auto &part = _parts[index];
        auto const found = by_id.find(part.id);
        if (found == by_id.end() || !found->second->placed || part.local.empty()) {
            if (part.shown) {
                part.shown.reset();
                changes.push_back({.index = index, .visible = false, .path = {}});
            }
            continue;
        }
        auto const &placement = *found->second;
        std::array<double, 3> const key{placement.translation_x, placement.translation_y, placement.rotation_degrees};
        if (part.shown == key) {
            continue;
        }
        part.shown = key;
        auto const transform = Geom::Rotate::from_degrees(placement.rotation_degrees) *
                               Geom::Translate(placement.translation_x, placement.translation_y);
        changes.push_back({.index = index, .visible = true, .path = part.local * transform});
        ++_rebuilt;
    }
    return changes;
}

void carry_pending_layout(Nesting::Progress &forwarded, std::optional<Nesting::Progress> &pending)
{
    if (forwarded.placements.empty() && pending) {
        forwarded.placements = std::move(pending->placements);
        forwarded.best_score = pending->best_score;
        forwarded.placed_count = pending->placed_count;
    }
    pending.reset();
}

} // namespace Inkscape::UI::Tools
