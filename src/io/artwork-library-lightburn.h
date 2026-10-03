// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_LIGHTBURN_DRAFT_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_LIGHTBURN_DRAFT_H

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Inkscape::IO::ArtworkLibrary {

enum class LightBurnDraftErrorCode { Xml, Limit, Unsupported, Geometry, Cancelled };

class LightBurnDraftError final : public std::runtime_error {
public:
    LightBurnDraftError(LightBurnDraftErrorCode code, std::string message);
    LightBurnDraftErrorCode code() const noexcept { return _code; }
private:
    LightBurnDraftErrorCode _code;
};

struct LightBurnDraftLimits {
    std::size_t xml_bytes = 32u * 1024 * 1024;
    std::size_t xml_nodes = 100000;
    std::size_t xml_depth = 64; // Hard ceiling also 64, including list/wrapper nodes.
    std::size_t text_bytes = 32u * 1024 * 1024;
    std::size_t shapes = 20000;
    std::size_t vertices = 500000;   // Aggregate across the entry, not per path.
    std::size_t primitives = 500000;
    std::size_t svg_bytes = 64u * 1024 * 1024;
    std::size_t tab_pairs = 10000; // Aggregate opaque numeric pairs, not interpreted geometry.
    std::size_t tab_metadata_bytes = 256u * 1024; // Aggregate retained decoded leaf text.
    double coordinate_abs = 1e9; // Also bounded by a hard ceiling of 1e9.
};

// Explicit per-shape diagnostic/provenance, not SVG content or laser settings.
// Retain with the import result and source record identity; never silently discard.
struct LightBurnUnappliedLaserTabs {
    std::size_t shape_preorder_1based = 0; // Includes Group nodes in source traversal.
    std::size_t pair_count = 0;
    std::string source_text; // Exact SAX-decoded leaf text, not original XML bytes.
    static constexpr char const *message =
        "Laser tabs not applied: full editable geometry retained; tab pair semantics are not decoded.";
};

struct LightBurnSvgDraft {
    std::string svg;
    // Exact emitted physical root viewport, including the one-mm margin.
    // Independent SVG preflight still verifies these against the emitted bytes.
    double width_mm = 0, height_mm = 0;
    std::size_t shapes = 0, vertices = 0, primitives = 0;
    std::vector<std::string> qualifications;
    std::vector<LightBurnUnappliedLaserTabs> unapplied_laser_tabs;
    static constexpr bool millimeter_mapping_source_backed = true;
    // These constants deliberately cannot certify success as shipping import.
    static constexpr bool lightburn_fidelity_qualified = false;
    static constexpr bool physical_fidelity_qualified = false;
    static constexpr bool requires_svg_preflight = true;
};

// Research adapter, not a shipping import API. See SEMANTICS.md in the packet.
// Requires UTF-8 LightBurnShapes FormatVersion="1", explicit MirrorX/MirrorY,
// and inline Group/Path/Rect geometry. No disk/network/document/UI access.
// The input and callback are owned copies before any callback can run.
// A whole entry either returns a draft or throws; no partial SVG is published.
// Unknown types/attributes/children and unsupported geometry are explicit errors.
// Output is editable SVG geometry: one numerical user unit = one millimeter,
// with explicit mm width/height and a matching viewBox. No raw extent inference.
// This is NOT certified SVG, validated physical fidelity or recovered print styling.
// Nonempty Tabs retain decoded metadata and explicit unapplied-laser diagnostics.
// C's independent SVG preflight must succeed before any SPDocument use.
LightBurnSvgDraft convert_lightburn_shapes_v1_draft(
    std::string xml, LightBurnDraftLimits limits = {},
    std::function<bool()> cancelled = {});

} // namespace Inkscape::IO::ArtworkLibrary
#endif
