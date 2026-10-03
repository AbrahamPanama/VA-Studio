// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_LIVE_EFFECTS_FILLET_CHAMFER_EDIT_H
#define INKSCAPE_LIVE_EFFECTS_FILLET_CHAMFER_EDIT_H

#include <compare>
#include <optional>
#include <vector>
#include "helper/geom-pathvector_nodesatellites.h"

namespace Inkscape::LivePathEffect::CornerEdit {

enum class Scope { Selected, All };
enum class Mode { Round, InverseRound };
enum class Status { Ready, NoChange, NoCorners, InvalidInput, EngineLimit };

struct Address {
    std::size_t path;
    std::size_t node;
    auto operator<=>(Address const &) const = default;
};

// Detached input, never a retained Node*, knot holder or desktop-coordinate
// proximity match. Addresses are indices into the effect's INPUT path.
// The Node adapter must additionally exclude nodes marked smooth/symmetric/auto.
struct Snapshot {
    Geom::PathVector path;
    NodeSatellites satellites;
    std::vector<Address> selected;
    std::vector<Address> smooth;
};

struct Request {
    Scope scope = Scope::Selected;
    std::optional<Mode> mode;
    // True radius in INPUT path coordinates, not satellite amount/knot distance.
    // UI converts a display-unit value before calling this API.
    std::optional<double> radius;
};

struct Summary {
    Status status = Status::InvalidInput;
    std::size_t count = 0;
    std::optional<Mode> mode;
    std::optional<double> radius;
    bool approximate = false;
};

struct Plan {
    Status status = Status::InvalidInput;
    NodeSatellites satellites;
    std::size_t count = 0;
    bool approximate = false;
};

// Hard admission bound, before geometry conversion or native radius solving.
// This is a complexity cap, not a measured main-thread latency guarantee.
inline constexpr std::size_t max_nodes = 4096;
bool bounded_shape(Geom::PathVector const &path, NodeSatellites const &satellites);
bool equal(NodeSatellites const &a, NodeSatellites const &b);
Summary inspect(Snapshot const &input, Scope scope);
Plan prepare(Snapshot const &input, Request const &request);

// A circular document-space radius exists only under a similarity transform.
// Reflection is allowed. Return no value for skew/nonuniform/singular transforms;
// callers must explain that boundary, never average scales or rewrite transforms.
std::optional<double> document_scale(Geom::Affine const &input_to_document);

} // namespace Inkscape::LivePathEffect::CornerEdit
#endif
