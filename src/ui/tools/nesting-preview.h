// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_NESTING_PREVIEW_H
#define INKSCAPE_UI_TOOLS_NESTING_PREVIEW_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <2geom/pathvector.h>

#include "nesting/nesting-document.h"

namespace Inkscape::UI::Tools {

/// Which preview paths change when a new best layout arrives (R3). Headless:
/// the tool owns the canvas items and maps document to desktop coordinates.
class NestingPreviewModel
{
public:
    static constexpr std::size_t POINT_BUDGET = 20000; // above it: bounding boxes

    explicit NestingPreviewModel(Nesting::PreparedDocumentNesting const &snapshot);

    struct Change
    {
        std::size_t index;     // part index in the snapshot
        bool visible;
        Geom::PathVector path; // document coordinates; empty when !visible
    };
    /// Only parts whose placement changed, appeared or disappeared.
    std::vector<Change> update(std::span<Nesting::Placement const> placements);

    [[nodiscard]] bool uses_bounds() const noexcept { return _uses_bounds; }
    [[nodiscard]] std::size_t rebuilt_paths() const noexcept { return _rebuilt; }
    [[nodiscard]] std::size_t size() const noexcept { return _parts.size(); }

private:
    struct Part
    {
        std::uint64_t id;
        Geom::PathVector local; // outline (or bounds) at the original position
        std::optional<std::array<double, 3>> shown; // tx, ty, rotation last shown
    };
    std::vector<Part> _parts;
    bool _uses_bounds = false;
    std::size_t _rebuilt = 0;
};

/**
 * Progress throttling (R3): the engine attaches a layout only to the report
 * that improves it, so a throttled report must not lose it. Call for every
 * forwarded report with the latest layout-bearing report seen since the last
 * forward. A report without a layout takes the pending layout together with
 * its score and placed count (so the three always describe one layout); a
 * report with its own layout is newer and wins. `pending` is always cleared.
 */
void carry_pending_layout(Nesting::Progress &forwarded, std::optional<Nesting::Progress> &pending);

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_NESTING_PREVIEW_H
