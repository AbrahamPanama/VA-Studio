// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_EXPLODE_BITMAP_OVERLAY_H
#define INKSCAPE_UI_EXPLODE_BITMAP_OVERLAY_H
#include "ui/bitmap-preview-composer.h"
#include "ui/explode-bitmap-dependencies.h"
#include "ui/explode-bitmap-grid.h"
#include "util/bitmap-island-specks.h"
class SPDesktop;
namespace Inkscape { class CanvasItem; struct CanvasItemBuffer; }
namespace Inkscape::Bitmap {
constexpr std::uint64_t outlineByteLimit = 64 * MiB;
constexpr std::uint64_t outlineSegmentLimit = outlineByteLimit / 20;
constexpr std::uint64_t outlineDrawSegmentLimit = 150'000;
constexpr unsigned outlineTileSize = 64;
enum class OutlineVisibility { exact, tooDense, unavailable };
struct OutlineCamera {
    OutlineVisibility visibility = OutlineVisibility::unavailable;
    std::uint64_t visibleSegments = 0;
};
struct OverlayTestHooks {
    bool legacyStroke = false; // Force the general-affine coverage path in tests.
    AllocationFault *retirement = nullptr;
    void (*afterStroke)(void *) noexcept = nullptr;
    void *context = nullptr;
};
struct OutlineSegment { std::uint32_t x, y, endX, endY, piece; };
struct OutlineTile { std::uint64_t begin = 0, count = 0; };
struct OutlineStorage {
    // Budget must outlive this immutable storage, including retired canvas snapshots.
    PlainBuffer segments, pieces, tiles;
    unsigned columns = 0, rows = 0;
    std::uint64_t count = 0;
    std::uint32_t pieceCount = 0;
    GridTransform pixelToDocument{};
    // Exact source-coordinate summary; camera admission sums intersecting tiles.
    std::array<std::uint32_t,4> bounds{}; // min x/y, max x/y
    std::uint64_t horizontalLength = 0, verticalLength = 0;
    OutlineTile const *index() const { return reinterpret_cast<OutlineTile const *>(tiles.data()); }
    std::uint64_t bytes() const { return segments.size() + pieces.size() + tiles.size(); }
    OutlineSegment const *data() const { return reinterpret_cast<OutlineSegment const *>(segments.data()); }
};
struct ExactOutlines {
    std::shared_ptr<OutlineStorage const> storage;
    DependencyToken dependencies;
    AdmissionLimits limits;
    Generation generation = 0;
    bool approximate = false; // proxy results never authorize exact readiness
};
// Worker-only, two bounded run scans: foreground edges, including hole contours;
// transparent owned holes and encoder gutters do not acquire visible outlines.
Result<ExactOutlines> prepareOutlines(Partition const &, FinalGrid const &, Budget &,
                                     JobWork &, Stop = {}, AllocationFault * = nullptr,
                                     std::uint64_t segmentBudget = outlineSegmentLimit) noexcept;
OutlineCamera outlineCamera(OutlineStorage const &, Geom::Affine const &, Geom::Rect const &);
// Candidate thumbnail shares the bounded coverage renderer with the canvas.
Outcome renderOutlinePreview(ExactOutlines const &, CanvasItemBuffer &, Geom::Affine const &);
class BitmapOverlay final {
public:
    explicit BitmapOverlay(OverlayTestHooks * = nullptr);
    ~BitmapOverlay();
    BitmapOverlay(BitmapOverlay const &) = delete;
    BitmapOverlay &operator=(BitmapOverlay const &) = delete;
    // Main only. Call BEFORE dispatch on every recipe/tab/view change. New tickets
    // strictly increase; clear retires the ticket, including queued completions.
    Outcome beginGeneration(SPDesktop &, Generation);
    Outcome installOutlines(SPDesktop &, ExactOutlines const &, Generation);
    void clear() noexcept;
    Outcome zoomToPiece(std::uint32_t piece, Generation);
    bool ready() const;
    OutlineCamera camera() const;
    bool cameraCallbackRegistered() const; // read-only diagnostic, including a cleared retained item
    sigc::connection connectVisibility(sigc::slot<void (OutlineVisibility)> const &);
    CanvasItem *canvasItem() const; // read-only diagnostic, no document object
private:
    struct State;
    std::unique_ptr<State> _state;
};
// Fitted vector preview, independent of the pixel-outline implementation.
namespace PanelPreparation { struct ContourProduct; }
class ContourOverlay final {
public:
    ContourOverlay();
    ~ContourOverlay();
    Outcome beginGeneration(SPDesktop &, Generation);
    Outcome install(SPDesktop &, std::shared_ptr<PanelPreparation::ContourProduct const>,
                    GridTransform const &, DependencyToken, Generation, std::uint32_t rgba);
    void clear() noexcept;
    bool ready() const;
    CanvasItem *canvasItem() const;
    // Read-only test seam: draw one fitted piece through the live item's renderer.
    void renderPieceForTest(CanvasItemBuffer &, unsigned piece) const;
private:
    struct State;
    std::unique_ptr<State> _state;
};
} // namespace Inkscape::Bitmap
#endif
