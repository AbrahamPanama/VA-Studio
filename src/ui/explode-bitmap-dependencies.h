// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_EXPLODE_BITMAP_DEPENDENCIES_H
#define INKSCAPE_UI_EXPLODE_BITMAP_DEPENDENCIES_H
#include <limits>
#include <memory>
#include <2geom/rect.h>
#include <optional>
#include <vector>
#include "document-undo.h"
#include "ui/explode-bitmap-jobs.h"
#include "ui/explode-bitmap-target.h"
namespace Inkscape::Bitmap {
constexpr std::uint64_t UnlimitedExplodeObservationUnits = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t MaxExplodePieces = 150;
constexpr unsigned MaxExplodeSourceAxis = 5000;
struct ExplodeSourceSize { unsigned width = 0, height = 0; };
// Downscale only; floor each axis, keeping positive input axes at least one pixel.
ExplodeSourceSize fitExplodeSourceSize(unsigned width, unsigned height);
// Document-space bounds in SVG px (96 px/in). Zero means no integer DPI fits.
// Mirrors SingleImageResize: ceil(dpi / 96 * exact extent) on each axis,
// without ordinary Bitmap Copy's outward rounding at 96 DPI.
int maxResizeDpi(Geom::Rect const &bounds);
inline constexpr char ExplodeSourceSizeMessage[] =
    "This image is %1 × %2 px. Explode Bitmap works with images up to %3 × %3 px.";
std::string explodeSourceSizeMessage(std::uint64_t width, std::uint64_t height);
// Plain worker-safe identities; lookup never dereferences addresses from a token.
struct DependencyToken {
    std::uint64_t lease = 0, generation = 0, capture = 0;
    explicit operator bool() const { return lease && capture; }
};
struct PlatformEvidence;
struct PublicationEpoch { std::uint64_t lease = 0, value = 0; };
// Construct on a LIVE desktop before resolve/capture. One owner per request;
// destroy on main after closing delivery. Leases neither pin views nor documents.
class DependencyLease {
public:
    explicit DependencyLease(SPDesktop &);
    DependencyLease(SPDesktop &, PlatformEvidence const &);
    ~DependencyLease();
    DependencyLease(DependencyLease const &) = delete;
    DependencyLease &operator=(DependencyLease const &) = delete;
    void invalidate(); // recipe/session/composer client changes, before requesting new work
    std::size_t nativeWatchCount() const; // read-only retained observation/admission diagnostic
private:
    struct State;
    std::unique_ptr<State> _state;
    friend class DependencyPublication;
    friend class DependencyRequest;
    friend DependencyToken capture(TargetSnapshot const &);
    friend bool valid(DependencyToken const &, PublicationEpoch);
};
DependencyToken capture(TargetSnapshot const &); // absent lease/stale/refused -> empty
bool valid(DependencyToken const &, PublicationEpoch = {});
enum class DependencyCheck { Dispatch, Delivery, Admission, Settlement, Confirmation };
// Idempotent invalidation accounting per captured result. Success at intermediate
// checks does not reset consecutive failures; completed work or explicit retry does.
class DependencyRequest {
public:
    Outcome check(DependencyToken, DependencyCheck, PublicationEpoch = {});
    Outcome confirm(Ticket, DependencyToken); // single use, latch before revalidation
    void retry() { _invalidated.clear(); _paused = false; }
    void completed() { retry(); }
    bool paused() const { return _paused; }
private:
    std::vector<DependencyToken> _invalidated; // bounded to three distinct results
    bool _paused = false;
    Ticket _confirmed = 0;
    DependencyToken _activation;
};
// Exact ordered publication script, prepared before mutation. Null vs empty values
// remain distinct; event addresses are compared without dereferencing.
// Selection notifications remain exact. Native updates are diagnostic only.
// Native node is capture-time SPObject identity; inserted nodes are never watched.
// Add child must be live, fully staged XML in the target document at construction;
// its anchored XML subtree must still equal those values when the script completes.
// An exactly consumed Remove retires captured bitmap watches in its subtree.
// An owned href Attribute on a captured bitmap also retires that watch; its
// staged XML must equal the original with only that href replaced at settlement.
// Other stamps/checks still apply; Native entries are not accepted in scripts.
enum class MutationKind { Add, Remove, Order, Attribute, Content, Name, Native, Selection };
struct ExpectedMutation {
    MutationKind kind;
    std::uintptr_t node = 0, child = 0, previous = 0, oldPrevious = 0;
    unsigned key = 0;
    std::optional<std::string> before, after;
    std::vector<std::uintptr_t> selection;
    // Mutually exclusive with native selection. Identities must belong to an
    // anchored Add subtree in this script; bindings are checked at delivery.
    std::optional<std::vector<std::uintptr_t>> stagedSelection;
};
// Synchronous only, nested epochs refuse. The live atomic guard must outlive this
// lease. EB5 additionally proves staged ids/objects and its reserved rollback in
// ready(); valid(token, epoch()) checks each callback prefix; request.check(...,
// Settlement, epoch()) additionally requires the entire script to be consumed.
class DependencyPublication {
public:
    DependencyPublication(DependencyToken, DocumentUndo::RollbackableInteraction &,
                          std::vector<ExpectedMutation>);
    ~DependencyPublication();
    DependencyPublication(DependencyPublication const &) = delete;
    DependencyPublication &operator=(DependencyPublication const &) = delete;
    PublicationEpoch epoch() const { return _epoch; }
private:
    PublicationEpoch _epoch;
};
enum class EvidencePlatform { Missing, Mac, Windows };
struct ExplodeStepMaxima {
    std::uint64_t publicationNs = 0, rollbackNs = 0, selectNs = 0, undoNs = 0, redoNs = 0;
};
struct ExplodePromotionEvidence {
    // Historical full-call p100 records; diagnostic only. MP is decimal.
    unsigned repetitions = 0;
    std::uint64_t pieces = 0, sourcePixels = 0, piecePixels = 0;
    ExplodeStepMaxima small, ceiling;
    std::uint64_t outlineDrawNs = 0, captureNs = 0, workerCompletionNs = 0;
    std::uint64_t pngDecodeNs = 0, pngStopNs = 0, jpegDecodeNs = 0, jpegStopNs = 0;
    std::uint64_t webpDecodeNs = 0, webpStopNs = 0;
    bool webpUnmeasuredAccepted = false; // Supervisor D5, explicit and reported.
    // Graphical objects in the historical full capture fixture (diagnostic only).
    std::uint64_t captureObjects = 0;
};
struct PlatformEvidence {
    EvidencePlatform platform = EvidencePlatform::Missing;
    // Optional diagnostic records. Costs never determine production admission.
    std::uint64_t observationNs = 0, clipNs = 0, renderNs = 0, publicationNs = 0,
                  rollbackNs = 0, historyNs = 0, outlineNs = 0;
    std::uint64_t workerPixelsPerSecond = 0, workerBytesPerSecond = 0;
    std::uint64_t stopAckNs = 0, closeNs = 0;
    ExplodePromotionEvidence promotion;
};
struct AdmissionLimits {
    std::uint64_t observationUnits = 0, clipUnits = 0, renderUnits = 0,
                  publicationUnits = 0, rollbackUnits = 0, historyUnits = 0, outlineUnits = 0;
    std::uint64_t stopPixels = 0, stopBytes = 0, stopNs = 10'000'000;
    bool workerQualified = false;
    unsigned sourceAxis = MaxExplodeSourceAxis;
    bool extremeStressForTest = false; // Explicit harness-only N=300; never set by platform defaults.
};
AdmissionLimits measuredLimits(PlatformEvidence const &);
PlatformEvidence macExplodePromotionEvidence(); // Mac record, independent of the compiling host
PlatformEvidence windowsExplodePromotionEvidence(); // Optional independent Windows diagnostics
PlatformEvidence diagnosticDependencyEvidence(); // Known platform identity for diagnostic fixtures
PlatformEvidence macDependencyEvidence(); // Historical name: returns this host's platform defaults
struct LatencyWork {
    std::uint64_t observation = 0, clip = 0, render = 0, publication = 0,
                  rollback = 0, history = 0, outlines = 0;
    std::uint64_t sourcePixels = 0, piecePixels = 0; // P and exact B where known
    unsigned width = 0, height = 0; // Final source grid, checked independently on both axes
};
// Fixed product admission AFTER header/memory reservation, BEFORE native work.
// Historical API name; measuredLimits reads only the platform, never evidence costs.
// Publication/rollback/history units count output pieces. Paired conversion
// separately reserves all source/temporary nodes, recovery and both Undo entries.
Outcome admitLatency(LatencyWork const &, AdmissionLimits const &);
// All three stop bounds enforced together with an injectable clock, no UI state.
class StopCadence {
public:
    explicit StopCadence(AdmissionLimits, JobNow = JobClock::now);
    Outcome advance(std::uint64_t pixels, std::uint64_t bytes, Stop = {});
    void checked(); // only after an actual stop check
private:
    AdmissionLimits _limits;
    JobNow _now;
    JobClock::time_point _last;
    std::uint64_t _pixels = 0, _bytes = 0;
};
} // namespace Inkscape::Bitmap
#endif
