// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_BITMAP_COPY_OUTCOME_H
#define INKSCAPE_BITMAP_COPY_OUTCOME_H
#include <memory>
#include <variant>
#include "ui/explode-bitmap-jobs.h"
#include <string>
#include "document-undo.h"
#include "ui/explode-bitmap-grid.h"
#include "util/bitmap-island-budget.h"
#include "ui/explode-bitmap-target.h"
struct _cairo_surface;
namespace Inkscape {
class ObjectSet;
class Pixbuf;
struct BitmapCopyOptions;
namespace Bitmap {
class MemoryProbe;
struct AdmissionLimits;
// NativeCopy preserves the existing command, including effect-ancestor lift.
// ContiguousReplacement is the explicit collective conversion policy.
// SingleImageResize is opt-in: caller DPI, exact document extent, one selected
// bitmap, metered transparent replacement in the original parent/stacking slot.
enum class PlacementPolicy { NativeCopy, ContiguousReplacement, SingleImageResize };
class DocumentPublicationContext;
struct Dpi { double value; };
struct ExactPixels { std::uint32_t width, height; };
enum class CopyBounds { Visual, Geometric };
struct BitmapCopyRequestOptions {
    std::variant<Dpi, ExactPixels> sizing = Dpi{96};
    CopyBounds bounds = CopyBounds::Visual;
    std::array<double, 4> background{0, 0, 0, 0};
    bool keep_original = true;
    PlacementPolicy placement = PlacementPolicy::NativeCopy;
};
struct CandidateHooks {
    MemoryProbe const *memory = nullptr; // null = native probe; must outlive candidate
    _cairo_surface *(*createSurface)(int, int) = nullptr;
    void (*hideProbe)() = nullptr;
    void (*afterInsert)() = nullptr; // failure-injection seam, inside caller's atomic token
    AdmissionLimits const *renderLimits = nullptr; // opt-in; borrowed only during prepare, before render/reservation
};
struct BitmapCopyMetadata {
    unsigned width = 0, height = 0;
    double widthMm = 0, heightMm = 0, requestedDpi = 0, renderDpi = 0, dpiX = 0, dpiY = 0;
    std::uintptr_t parent = 0, layer = 0;
    unsigned slot = 0;
    bool clamped = false;
    char const *resolutionReason = "";
    TargetSnapshot token;
    std::uint64_t candidateIdentity = 0;
};
struct BitmapCopyOutcome;
// Main-thread-only, move-only immutable candidate. Holds pixels and values, never XML nodes.
// Budget and injected probe outlive this object. Publication consumes it even on failure.
class PreparedBitmapCopy {
public:
    PreparedBitmapCopy();
    ~PreparedBitmapCopy();
    PreparedBitmapCopy(PreparedBitmapCopy &&) noexcept;
    PreparedBitmapCopy &operator=(PreparedBitmapCopy &&) noexcept;
    PreparedBitmapCopy(PreparedBitmapCopy const &) = delete;
    PreparedBitmapCopy &operator=(PreparedBitmapCopy const &) = delete;
    BitmapCopyMetadata const &metadata() const;
    Pixbuf const *pixels() const;
    Result<CandidateGridInput> gridInput(Budget &, Stop = {}) const noexcept;
    RgbaView rgba() const; // safe candidates: canonical straight RGBA, immutable borrowed view
private:
    struct State;
    std::unique_ptr<State> _state;
    friend BitmapCopyOutcome prepareBitmapCopyCore(ObjectSet &, BitmapCopyOptions const &, PlacementPolicy,
        Budget &, CandidateHooks, BitmapCopyRequestOptions const *, DocumentPublicationContext *);
    friend BitmapCopyOutcome prepareBitmapCopy(ObjectSet &, BitmapCopyOptions const &, PlacementPolicy,
                                               Budget &, CandidateHooks);
    friend BitmapCopyOutcome publishBitmapCopy(ObjectSet &, PreparedBitmapCopy const &,
                                               DocumentUndo::RollbackableInteraction *);
};
struct BitmapCopyOutcome {
    Outcome outcome;
    std::optional<CliBitmapFailure> failure;
    bool rolledBack = false;
    PreparedBitmapCopy candidate;
    std::vector<TargetReason> refusals; // preserve every excluded object and reason
    std::uintptr_t image = 0; // live SPImage incarnation, only after successful publication
    std::string imageId;
    unsigned refusedWidth = 0, refusedHeight = 0; // exact grid for a caller-domain refusal
    bool ok() const noexcept { return outcome.ok(); }
};
BitmapCopyOutcome prepareBitmapCopy(DocumentPublicationContext &, BitmapCopyRequestOptions const &, Budget &,
                                    CandidateHooks = {});
BitmapCopyOutcome prepareBitmapCopy(ObjectSet &, BitmapCopyOptions const &, PlacementPolicy, Budget &,
                                    CandidateHooks = {});
// Safe conversion/resize requires a valid caller-owned atomic token. Does not settle
// history: the caller commits its operation once, or rolls back before returning.
// Null guard is accepted only for the legacy NativeCopy policy.
BitmapCopyOutcome publishBitmapCopy(ObjectSet &, PreparedBitmapCopy const &,
                                    DocumentUndo::RollbackableInteraction *guard = nullptr);
} // namespace Bitmap
} // namespace Inkscape
#endif
