// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_INSERT_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_INSERT_H
#include "artwork-library-svg-preflight.h"
#include <2geom/point.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

class SPDocument;
class SPObject;

namespace Inkscape::IO::ArtworkLibrary {
enum class InsertionFailure { Ineligible, StaleTarget, Busy, Cancelled, Unsupported, LimitExceeded, Interrupted };
class InsertionError final : public std::runtime_error {
public:
    InsertionError(InsertionFailure code, std::string message) : std::runtime_error(std::move(message)), _code(code) {}
    InsertionFailure failure() const noexcept { return _code; }
private:
    InsertionFailure _code;
};
struct InsertionLimits {
    std::size_t nodes = 100000, depth = 64;
    std::size_t materialized_style_bytes = 32u * 1024 * 1024;
    std::size_t target_snapshot_bytes = 2u * 1024 * 1024;
    double coordinate_magnitude = 1e9;
};
// An XML identity/ancestor snapshot, not a borrowed SPObject. Capture on the
// document's model-owning main thread, after the caller's normal model update.
// This read-only capture does not force an update/emit update callbacks.
// It may outlive that document, but cannot
// be applied to another document (serial + XML identity are both checked).
class InsertionTarget final {
public:
    InsertionTarget(InsertionTarget const &) = default;
    InsertionTarget &operator=(InsertionTarget const &) = default;
private:
    struct State;
    std::shared_ptr<State const> _state;
    explicit InsertionTarget(std::shared_ptr<State const> state) : _state(std::move(state)) {}
    friend InsertionTarget capture_insertion_target(SPDocument &, SPObject &, InsertionLimits);
    friend struct InsertionAccess;
};
struct InsertedArtwork {
    unsigned long document_serial;
    std::string asset_id, source_sha256;
    std::vector<std::string> inserted_ids; // One AlwaysGroup ID; resolve afresh.
    double width_mm, height_mm;
    Geom::Point viewport_origin; // Document coordinates, CSS px at 96 px/in.
    std::vector<std::string> warnings;
};
// Eligible means attached root/group, visible/unlocked through all ancestors,
// finite invertible transforms and neutral ancestor compositing. A clipping,
// masked, filtered, translucent or blending parent cannot preserve source appearance.
InsertionTarget capture_insertion_target(SPDocument &, SPObject &, InsertionLimits = {});
// Synchronous MAIN-THREAD operation; destination owner must remain alive for
// the entire call, including callbacks and return-value/capture destruction.
// Registered owners additionally defer close through DocumentUndo's lease.
// For private unique_ptr owners, keep the unique_ptr in the caller's outer scope.
// No caller callback may destroy that private owner directly.
//
// Places the source physical ROOT VIEWPORT origin at viewport_origin. Original
// viewBox/preserveAspectRatio and editable root metadata remain on a nested SVG.
// Uses the real v3 token by value; no path or alternate byte-taking overload.
// Success commits one Undo entry. Throws before history publication on refusal.
// Existing pending XML makes the operation Busy; it is not folded into this Undo.
// cancelled is cooperative and may run nested loops; no wall-time/OOM guarantee.
InsertedArtwork insert_artwork(SPDocument &, InsertionTarget target, ValidatedSvg asset,
                               Geom::Point viewport_origin, InsertionLimits = {}, Cancelled = {});
// UI contract: resolve returned IDs against the matching document after success.
// Change only view selection, suppress automatic page/layer XML writes, and do
// not call done() again. The helper never changes selection. Normal reconstruction
// removes selected deleted objects on Undo; restoring a prior UI selection on Undo
// needs the caller's existing selection/history adapter, not a second XML commit.
// Native clip/mask inline-important overrides are included in the appearance shield.
// A close/removal from post-publication history/atomic-retirement observers still means committed;
// IDs can then be absent. Do not report failure or automatically retry that case.
}
#endif
