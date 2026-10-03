// SPDX-License-Identifier: GPL-2.0-or-later
// ARTIFACT DRAFT: implementation/tests supplied separately; unbuilt/unqualified.
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_SVG_PREFLIGHT_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_SVG_PREFLIGHT_H

#include "artwork-library-archive.h"
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Inkscape::IO::ArtworkLibrary {

struct SvgPreflightExpectation {
    std::string asset_id; // Canonical manifest asset UUID.
    std::string sha256;   // Required digest of these exact bytes, not authenticity.
    double width_mm = 0;
    double height_mm = 0;
};

// Draft policy-v3 budgets. The implementation rejects attempts to
// raise these hard defaults; callers may lower them. No deadline guarantee.
struct SvgPreflightLimits {
    std::size_t xml_bytes = 32u * 1024 * 1024;
    std::size_t nodes = 100000;
    std::size_t depth = 64;
    std::size_t attributes_per_node = 128;
    std::size_t references = 100000;
    std::size_t expanded_nodes = 100000;
    std::size_t geometry_commands = 1000000;
    std::size_t image_pixels = 16u * 1024 * 1024;
    std::size_t total_image_pixels = 64u * 1024 * 1024;
    std::size_t filter_primitives = 1024; // Aggregate; each chain <= 64 primitives.
    double coordinate_magnitude = 1e9;
    double dimension_tolerance_mm = 0.01;
};

enum class SvgPreflightFailure {
    MalformedXml, ForbiddenContent, Unsupported, LimitExceeded,
    Integrity, DimensionMismatch, Cancelled
};

class SvgPreflightError final : public std::runtime_error {
public:
    SvgPreflightError(SvgPreflightFailure failure, std::string message);
    SvgPreflightFailure failure() const noexcept { return _failure; }
private:
    SvgPreflightFailure _failure;
};

struct SvgViewBox {
    double x, y, width, height;
};
struct SvgPreflightStats {
    // nodes includes bounded SAX text/CDATA events, not a native DOM node count.
    // geometry_commands and expanded_* include conservative reference/tile reuse.
    std::size_t nodes = 0, depth = 0, references = 0, expanded_nodes = 0;
    std::size_t geometry_commands = 0, image_pixels = 0, expanded_image_pixels = 0;
    std::size_t filter_primitives = 0;
};

class ValidatedSvg;

// Copies admitted input into fresh immutable storage BEFORE the first caller
// callback. Neither moving a caller's vector nor borrowing its storage suffices:
// a retained caller pointer must not be able to change an admitted payload.
// Caller keeps input readable and unchanged until that synchronous copy finishes;
// concurrent writes to input are not supported. Expected metadata/limits are
// value-pinned. Cancellation produces no token and performs no document/disk IO.
ValidatedSvg preflight_svg(std::span<unsigned char const> input,
                          SvgPreflightExpectation expected,
                          SvgPreflightLimits limits = {},
                          Cancelled cancelled = {});

// Non-forgeable through the public API; no default/bytes-taking constructor,
// mutable bytes, public state, setter or serialized "trusted" receipt.
// Ordinary copies (including from rvalues) share immutable state and remain valid.
// Does not certify arbitrary SVG, font availability, renderer safety, destination
// CSS isolation/fidelity, document eligibility or successful native insertion.
class ValidatedSvg final {
public:
    ValidatedSvg(ValidatedSvg const &) = default;
    ValidatedSvg &operator=(ValidatedSvg const &) = default;
    ~ValidatedSvg() = default;

    // An owning handle to an actually const string, allocated privately by the
    // validator. It outlives this wrapper. No alias to caller-mutable bytes is
    // retained. Native consumers hold this owner across createNewDocFromMem.
    std::shared_ptr<std::string const> svg_bytes() const;
    std::string asset_id() const;
    std::string sha256() const;
    double width_mm() const;  // Verified physical ROOT VIEWPORT, not painted bounds.
    double height_mm() const;
    SvgViewBox view_box() const;
    SvgPreflightStats stats() const;
    std::vector<std::string> warnings() const;
    std::vector<std::string> requested_font_families() const;
    unsigned policy_version() const; // Draft value: 3. Cache hint, not a receipt.

private:
    struct State;
    explicit ValidatedSvg(std::shared_ptr<State const> state);
    std::shared_ptr<State const> _state;
    friend ValidatedSvg preflight_svg(std::span<unsigned char const>,
                                     SvgPreflightExpectation, SvgPreflightLimits, Cancelled);
};

// Policy-v3 insertion boundary:
// * Namespace-aware parsed rejection of EVERY svg:style element, anywhere in
//   the tree, plus xml-stylesheet PI, scripting/events, external references,
//   xml:base, DTD/entities and active/unsupported content. No textual grep gate.
// * Supported per-element inline styles/presentation attributes are preserved,
//   checked and locally resolved. No payload stylesheet reaches importDefs or
//   SPDocument::import. We never silently strip CSS and count that as fidelity.
// * Known editable/vector/metadata content is retained byte-for-byte. Unsupported
//   constructs reject with an explicit reason; they are not flattened or dropped.
// * The result is the only proposed input type to main's native insertion API.
//   Keep SPDocument creation/import/Undo on their owning thread, after admission.
//   No SPDocument, GTK, filesystem or global parser-setting API appears here.
// * Nested svg and the inert native namedview scaffold remain byte-identical.
// * Common native filter chains are admitted with input/graph bounds; this does
//   not impose a device-space allocation cap on native renderers. See the v2
//   FILTER-RENDER-PROTOCOL.md before enabling arbitrary filtered-input rendering.
// * Native tone-v1 parameters and bounded editable text/paragraph/frame XML are
//   retained. Text metric/graph checks do not certify font rasterizer behavior,
//   exact glyph bounds, layout time, or destination-specific rendering fidelity.

} // namespace Inkscape::IO::ArtworkLibrary
#endif
