// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_NESTING_DOCUMENT_H
#define INKSCAPE_NESTING_DOCUMENT_H

#include <array>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include <2geom/affine.h>

#include "nesting-capture.h"
#include "nesting-ffi.h"
#include "object/weakptr.h"

class SPDocument;
class SPItem;

namespace Inkscape::XML {
class SubtreeRevision;
} // namespace Inkscape::XML

namespace Inkscape::Nesting {

enum class ContourSource
{
    ExplicitContour,
    VectorClip,
    BitmapAlpha,
    ExactVector,
    CompoundVector,
    ConservativeHull,
    ConservativeBounds,
};

inline constexpr std::size_t CONTOUR_SOURCE_COUNT = 7;

/**
 * How the stored collision geometry was obtained. Appended enum: existing
 * values are never renumbered. Clean means the inferred/exact geometry passed
 * the shared validity gate unchanged.
 */
enum class RecoveryKind
{
    Clean,
    Repaired,
    ConservativeFallback,
    Skipped,
};

/** Bounded user-facing preparation diagnostic: object label plus short cause. */
struct RecoveryDetail
{
    std::string label;
    std::string reason;
};

struct PreparationMetrics
{
    std::size_t selected_count = 0;
    std::size_t prepared_count = 0;
    std::size_t skipped_count = 0;
    double selected_contour_area = 0.0;
    double container_usable_area = 0.0;
    double elapsed_seconds = 0.0;
    std::array<std::size_t, CONTOUR_SOURCE_COUNT> contour_source_counts{};
    // P2 diagnostics. Counts are complete; detail lists are capped.
    std::size_t repaired_count = 0;
    std::size_t fallback_count = 0;
    std::vector<RecoveryDetail> recovery_details;
    std::vector<RecoveryDetail> skipped_details;
    // Groups whose bitmaps were ignored because they contain vector artwork,
    // and the subset whose vector outline covers less than half of the
    // ignored bitmaps' extent (their bitmaps may overlap after nesting).
    std::size_t ignored_bitmap_part_count = 0;
    std::size_t sparse_vector_count = 0;
    std::vector<RecoveryDetail> sparse_vector_details;
    // Fixed objects on the sheet (obstacles) and their filled outline area,
    // plus objects ignored because they cover the whole sheet (backgrounds).
    std::size_t obstacle_count = 0;
    double obstacle_area = 0.0;
    std::size_t ignored_background_count = 0;
    // R1: capture on the GTK thread, geometry on a worker; elapsed is their sum.
    double capture_seconds = 0.0;
    double geometry_seconds = 0.0;
};

struct SolveMetrics
{
    std::uint64_t iterations = 0;
    std::size_t placed_count = 0;
    double placed_contour_area = 0.0;
    double utilization_percent = 0.0;
    double initial_solution_seconds = 0.0;
    double refinement_seconds = 0.0;
    double elapsed_seconds = 0.0;
};

enum class SkippedPartReason
{
    NullItem,
    ContainerSelectedAsPart,
    DuplicateItem,
    DifferentDocument,
    HiddenOrLocked,
    EmptyGeometry,
    GeometryTooComplex,
    // Appended in P2; existing ordinals unchanged.
    UnsafeRecovery,
    NonFiniteGeometry,
};

struct SkippedPart
{
    SPWeakPtr<SPItem> item;
    SkippedPartReason reason = SkippedPartReason::EmptyGeometry;
    std::string detail;
};

struct PreparedPart
{
    std::uint64_t id = 0;
    SPWeakPtr<SPItem> item;
    Geom::Affine original_item_to_document;
    std::vector<CollisionComponent> components;
    ContourSource contour_source = ContourSource::ExactVector;
    std::uint64_t geometry_fingerprint = 0;
    // Content-sensitive fingerprint of the item subtree (path/style/clip/
    // marker/image data). Kept separate from geometry_fingerprint so recovery
    // metadata never perturbs the geometry identity used by previews.
    std::uint64_t content_fingerprint = 0;
    RecoveryKind recovery = RecoveryKind::Clean;
    std::string recovery_reason;
    /// Pixel hashes of every visible bitmap (SPImage) in the subtree at
    /// preparation. Stored as items so this header needs no SPImage definition.
    std::vector<std::pair<SPWeakPtr<SPItem>, std::uint64_t>> image_alpha_hashes;
    /// Text layout or an external reference makes the cheap check unsafe.
    bool always_revalidate = false;
};

/// A fixed object on the sheet, captured with its collision outline.
struct PreparedObstacle
{
    SPWeakPtr<SPItem> item;
    Geom::Affine item_to_document;
    std::vector<CollisionComponent> components;
    std::uint64_t content_fingerprint = 0;
    /// The prepared collision outline, re-measured when the document changed:
    /// inherited style (a layer's stroke width, a CSS rule) can change it
    /// without touching the obstacle's own XML.
    std::uint64_t geometry_fingerprint = 0;
    /// As for parts (amendment C2): bitmap pixel hashes, and whether text or an
    /// external reference makes the cheap apply-time check unsafe.
    std::vector<std::pair<SPWeakPtr<SPItem>, std::uint64_t>> image_alpha_hashes;
    bool always_revalidate = false;
};

/**
 * Immutable geometry plus weak document references captured before a solve.
 *
 * The solver consumes only the copied polygons. applyNestingPlacements()
 * verifies the weak references, transforms, and geometry fingerprints before
 * touching XML, so a stale asynchronous result can never partially move a
 * document.
 */
struct PreparedDocumentNesting
{
    SPDocument *document = nullptr;
    SPWeakPtr<SPItem> container;
    Geom::Affine container_item_to_document;
    std::vector<Point> container_outline;
    std::vector<std::vector<Point>> container_holes;
    std::uint64_t container_geometry_fingerprint = 0;
    std::vector<PreparedPart> parts;
    std::vector<SkippedPart> skipped_parts;
    std::vector<PreparedObstacle> obstacles;
    double flatten_tolerance = 0.05;
    PreparationMetrics metrics;
    /// Created by prepareDocumentNesting on the GTK thread; destroyed there too.
    /// While it reports no XML change (the namedview excepted), apply skips
    /// re-preparing geometry and keeps only the checks XML cannot see.
    std::shared_ptr<XML::SubtreeRevision const> revision;
    /// The container is text, has an external reference or holds bitmaps.
    bool container_always_revalidate = false;
};

struct PreparationResult
{
    std::optional<PreparedDocumentNesting> snapshot;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return snapshot.has_value(); }
};

struct SolveResult
{
    Status status = Status::InvalidState;
    std::vector<Placement> placements;
    std::string error;
    SolveMetrics metrics;
    std::string backend = "native";
    std::string backend_detail;

    [[nodiscard]] explicit operator bool() const noexcept { return status == Status::Ok; }
};

enum class ApplyStatus
{
    Applied,
    NoChange,
    StaleSnapshot,
    InvalidResult,
    UndoUnavailable,
};

/// What applyNestingPlacements() does with parts that did not fit.
struct LeftoverPlacement
{
    /// Move unplaced parts into columns to the right of the container,
    /// top-aligned with it. Off by default: callers opt in.
    bool move_beside_container = false;
    /// Gap between the container and the first column, between columns and
    /// between stacked parts, in document units.
    double gap = 0.0;
    /// First column starts at max(container right + gap, start_x). NaN = unused.
    double start_x = std::numeric_limits<double>::quiet_NaN();
};

struct ApplyResult
{
    ApplyStatus status = ApplyStatus::InvalidResult;
    std::size_t moved_count = 0;
    std::size_t placed_count = 0;
    std::size_t unplaced_count = 0;
    std::size_t skipped_count = 0;
    std::size_t leftover_moved_count = 0; // unplaced parts actually moved beside the container
    std::string error;
    /// Time spent in the apply-time freshness check, and how many objects
    /// (container and parts) it fully re-prepared (R0; 0 when R2 found the
    /// document unchanged).
    double validation_seconds = 0.0;
    std::size_t revalidated_count = 0;

    [[nodiscard]] bool changed() const noexcept { return status == ApplyStatus::Applied; }
};

/// Objects on a sheet that must stay where they are, and whole-sheet backgrounds.
struct SheetObstacles
{
    std::vector<SPItem *> items;
    std::vector<SPItem *> ignored_backgrounds;
};

/**
 * Collect the objects on sheet that parts must avoid (section 3.2 of the
 * add-to-sheet work order): visible objects overlapping the sheet, locked
 * ones included, excluding the sheet, the parts of this run and whole-sheet
 * backgrounds, and entering any group that contains the sheet or a part.
 */
[[nodiscard]] SheetObstacles collectSheetObstacles(SPItem *sheet, unsigned dkey,
                                                   std::span<SPItem *const> parts);

/// Why item cannot be a nesting sheet, or std::nullopt if it can. Cheap: no
/// contour extraction. Container preparation applies the same rules first, so
/// hover feedback and preparation give the same message. Contour-level
/// problems (islands, self-intersection) are still reported by preparation.
[[nodiscard]] std::optional<std::string> sheetIneligibility(SPItem *item);

/**
 * Capture a target container and candidate parts in document coordinates.
 * Unsupported candidate parts are retained in skipped_parts and left in place;
 * an unsupported container makes the whole preparation fail.
 */
[[nodiscard]] PreparationResult prepareDocumentNesting(SPItem *container, std::span<SPItem *const> candidate_parts,
                                                       double flatten_tolerance = 0.05,
                                                       std::span<SPItem *const> obstacles = {});

// --- R1: preparation in three phases (consolidated work order 8.7) --------------
// prepareDocumentNesting() is assemble(capture(...), prepareCapturedGeometry(...))
// run synchronously, so every caller exercises the same code.

/// Phase A result: plain input for any thread, plus GTK-thread identities.
struct CaptureResult
{
    struct Identity
    {
        SPWeakPtr<SPItem> item;
        Geom::Affine item_to_document;
        std::uint64_t content_fingerprint = 0;
        std::vector<std::pair<SPWeakPtr<SPItem>, std::uint64_t>> image_alpha_hashes;
        bool always_revalidate = false;
    };

    std::string error;
    std::shared_ptr<CapturedInput const> input;
    /// GTK thread only: document, container, revision observer and flags.
    PreparedDocumentNesting skeleton;
    std::vector<Identity> candidates; // aligned with input->candidates
    std::vector<Identity> obstacles;  // aligned with input->obstacles
    double capture_seconds = 0.0;

    [[nodiscard]] explicit operator bool() const noexcept { return input != nullptr; }
};

/// Phase B result: geometry only, no document references.
struct CapturedGeometry
{
    struct Part
    {
        std::size_t candidate = 0;
        std::uint64_t id = 0;
        std::vector<CollisionComponent> components;
        ContourSource source = ContourSource::ExactVector;
        std::uint64_t geometry_fingerprint = 0;
        RecoveryKind recovery = RecoveryKind::Clean;
        std::string recovery_reason;
    };
    struct Skip
    {
        std::size_t candidate = 0;
        SkippedPartReason reason = SkippedPartReason::EmptyGeometry;
        std::string detail;
    };
    struct Obstacle
    {
        std::size_t index = 0;
        std::vector<CollisionComponent> components;
        std::uint64_t geometry_fingerprint = 0;
    };

    std::string error;
    std::vector<Point> container_outline;
    std::vector<std::vector<Point>> container_holes;
    std::uint64_t container_geometry_fingerprint = 0;
    std::vector<Part> parts;
    std::vector<Skip> skipped;
    std::vector<Obstacle> obstacles;
    PreparationMetrics metrics; // everything but capture_seconds

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

/// Called as candidates are prepared: (done, total).
using PreparationProgress = std::function<void(std::size_t, std::size_t)>;

/// Phase A (GTK thread): read the document into plain data.
[[nodiscard]] CaptureResult captureDocumentNesting(SPItem *container, std::span<SPItem *const> candidate_parts,
                                                   double flatten_tolerance = 0.05,
                                                   std::span<SPItem *const> obstacles = {});
/// Phase B (any thread): the geometry. A requested stop returns an error.
[[nodiscard]] CapturedGeometry prepareCapturedGeometry(CapturedInput const &input, std::stop_token stop = {},
                                                       PreparationProgress const &progress = {});
/// Phase C (GTK thread): join the identities with the geometry.
[[nodiscard]] PreparationResult assemblePreparedNesting(CaptureResult &&capture, CapturedGeometry &&geometry);
/// For a worker: the geometry as a snapshot to solve, without document
/// references (no SPWeakPtr is created or copied; `document` is only compared).
[[nodiscard]] PreparedDocumentNesting solvingSnapshot(SPDocument *document, CapturedInput const &input,
                                                      CapturedGeometry const &geometry);

/** Run the native solver without reading or mutating the SVG document. */
[[nodiscard]] SolveResult solvePreparedNesting(PreparedDocumentNesting const &snapshot, Options const &options = {},
                                               Job::ProgressCallback const &progress = {},
                                               std::stop_token cancellation = {});

/**
 * Apply a complete successful result as one rollbackable Undo transaction.
 * Every reference and geometry fingerprint is validated before the first XML
 * write. Unplaced and skipped objects are never transformed.
 */
[[nodiscard]] ApplyResult applyNestingPlacements(PreparedDocumentNesting const &snapshot,
                                                 std::span<Placement const> placements,
                                                 LeftoverPlacement const &leftovers = {});

} // namespace Inkscape::Nesting

#endif // INKSCAPE_NESTING_DOCUMENT_H
