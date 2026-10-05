// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UTIL_BITMAP_ISLAND_ENCLOSURE_H
#define INKSCAPE_UTIL_BITMAP_ISLAND_ENCLOSURE_H
#include "util/bitmap-islands.h"
namespace Inkscape::Bitmap {
constexpr std::uint32_t exteriorPiece = UINT32_MAX;
struct Piece {
    // Tie rule: earliest top-left SOURCE foreground pixel = smallest piece id;
    // sorted output index is not a tie key. Preserve minimum id through merges.
    std::uint32_t id, x, y, endX, endY; // half-open bbox, original discovery id
    std::uint64_t area;                 // foreground only, never hole pixels
};
struct PieceRun {
    std::uint32_t x, end, y, piece;     // zero-based sorted piece, or exteriorPiece
    bool foreground;                   // false = owned transparent hole/exterior
};
// Full-grid row-major RLE, including transparent holes. Every foreground sample
// and non-exterior hole has exactly one owner; exterior has the explicit sentinel.
// Hole ownership does not fill alpha. Runs may remain split at original boundaries.
// Borrows immutable Regions runs: Regions and Budget MUST outlive Partition and
// attached descendants. Moving Regions invalidates this view. Only owner/index
// arrays are copied; coordinates remain on the original grid.
struct OrthogonalMetric;
bool pieceLess(Piece const &, Piece const &) noexcept;
// Iterative (y,x,id) sort, charged to the caller's shared JobWork; polls Stop.
Outcome sortPieces(Piece *, std::uint32_t, JobWork &, Stop = {}) noexcept;
class Partition {
public:
    Partition() noexcept = default;
    Partition(Partition &&other) noexcept { *this = std::move(other); }
    Partition &operator=(Partition &&) noexcept;
    std::uint32_t foregroundCount = 0, enclosureMerges = 0;
    // Joins are eliminated input pieces; isolated counts frozen eligible pieces
    // with no candidate within reach (not the area of the resulting chain).
    std::uint32_t speckJoins = 0, retainedIsolatedSpecks = 0;
    std::uint32_t width = 0, height = 0, pieceCount = 0, runCount = 0, regionCount = 0;
    Piece const *pieces() const noexcept { return reinterpret_cast<Piece const *>(_pieces.data()); }
    Run const *sourceRuns() const noexcept { return _sourceRuns; }
    PieceRun run(std::uint32_t i) const noexcept {
        auto a = _sourceRuns[i]; return {a.x, a.end, a.y, runOwners()[i], a.region < foregroundCount};
    }
    std::uint32_t const *runOwners() const noexcept { return reinterpret_cast<std::uint32_t const *>(_runOwners.data()); }
    // Row y occupies [rows()[y], rows()[y+1]); rows has height+1 entries.
    std::uint32_t const *rows() const noexcept { return reinterpret_cast<std::uint32_t const *>(_rows.data()); }
    // Piece i's owned runs (foreground AND holes) are run indices in
    // [pieceOffsets()[i], pieceOffsets()[i+1]); exterior runs are excluded.
    std::uint32_t const *pieceOffsets() const noexcept { return reinterpret_cast<std::uint32_t const *>(_pieceOffsets.data()); }
    std::uint32_t const *pieceRuns() const noexcept { return reinterpret_cast<std::uint32_t const *>(_pieceRuns.data()); }
    std::uint32_t const *owners() const noexcept { return reinterpret_cast<std::uint32_t const *>(_owners.data()); }
    // owners()[i] maps an input region to a piece (holes included), or exterior.
    std::uint64_t boundaries = 0, graphEdges = 0, topologyPeak = 0;
private:
    Run const *_sourceRuns = nullptr;
    PlainBuffer _pieces, _runOwners, _owners, _rows, _pieceOffsets, _pieceRuns;
    // Supported attachment construction: allocate through Budget, charge every
    // pass to JobWork, preserve the borrowed-run lifetime and rebuild indexes.
    friend Result<Partition> attach(Partition const &, OrthogonalMetric const &,
                                    Budget &, JobWork &, Stop) noexcept;
    friend Result<Partition> enclose(Regions const &, Budget &, JobWork &, Stop, struct EnclosureOptions) noexcept;
};
enum class EnclosurePhase { graph, traversal, partition, sorting };
struct EnclosureOptions {
    // Lower-only ceilings. Nodes include the virtual border. edges/boundaries
    // are aliases for RAW undirected run contacts (including virtual border),
    // NOT pixel perimeter; graphEdges reports unique region pairs after dedup.
    // Spend at most 50M visits here AND leave at least 20M for attachment.
    // Final reserve also accounts for actual runs, frozen areas and reach rows.
    // If labeling already exhausted that share, refuse without further visits.
    std::uint32_t nodes = 16000001, edges = 64000000, queue = 16000001;
    std::uint64_t boundaries = 64000000, topologyBytes = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t visits = 50000000;
    // Pixel-axis DPI for the following attachment; attach rechecks its metric.
    double speckDpiX = 300, speckDpiY = 300;
    AllocationFault *fault = nullptr;
    void (*observe)(EnclosurePhase, void *) noexcept = nullptr;
    void *observerData = nullptr;
};
Result<Partition> enclose(Regions const &, Budget &, JobWork &, Stop = {}, EnclosureOptions = {}) noexcept;
} // namespace Inkscape::Bitmap
#endif
