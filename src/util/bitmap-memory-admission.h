// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap engine: RAM probes and whole-operation admission (EB4-memory).
 * Plan: internal note EXPLODE_BITMAP_IMPLEMENTATION_PLAN, internal note EXPLODE_BITMAP_PLAN v3.2.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UTIL_BITMAP_MEMORY_ADMISSION_H
#define INKSCAPE_UTIL_BITMAP_MEMORY_ADMISSION_H

#include <array>
#include <cstdint>
#include <limits>

#include "util/bitmap-island-budget.h"

namespace Inkscape::Bitmap {

/// A conservative probe result: physical R, available A, process footprint E (baseline).
using MemorySample = Memory;

/// Raw numbers from one probe source. Zero or inconsistent values mean the sample is refused.
struct RawMemory {
    std::uint64_t physical = 0, available = 0, footprint = 0;
};
/// Injectable source. Returns false when any native call fails. Must not throw.
class MemoryProbe {
public:
    virtual ~MemoryProbe() = default;
    virtual bool read(RawMemory &) const noexcept = 0;
};
/// The platform probe (macOS, Windows); always fails elsewhere.
MemoryProbe const &nativeMemoryProbe() noexcept;
/// Raw macOS counters (pure input for fromVmStats; compiled and tested on every platform).
struct VmStats {
    std::uint64_t memsize = 0, freePages = 0, inactivePages = 0, externalPages = 0, pageSize = 0;
    std::uint64_t footprint = 0;
    int pressureLevel = 0; ///< kern.memorystatus_vm_pressure_level: 1 normal, 2 warn, 4 critical
    std::uint64_t speculativePages = 0; ///< already inside free_count and (in xnu) external
};
/// A = free + min(inactive, external - speculative). This is an UPPER BOUND on reclaimable memory, not an
/// exact figure: external counts file-backed pages on every queue (active ones too), so it only limits
/// how much inactive memory may count; dirty anonymous inactive pages (compress/swap) are excluded when
/// external is small. Speculative pages are already in free_count and are not counted twice. Pressure
/// above normal halves A; critical or unknown pressure refuses. False means refused.
bool fromVmStats(VmStats const &, RawMemory &) noexcept;
/// Raw Windows counters (pure input for fromMemoryStatus). A = min(phys, pagefile[, virtual if 32-bit]).
struct WinStatus {
    std::uint64_t totalPhys = 0, availPhys = 0, availPageFile = 0, availVirtual = 0, privateUsage = 0;
    bool is32Bit = false;
};
bool fromMemoryStatus(WinStatus const &, RawMemory &) noexcept;
/// Validate a probe: any failure or inconsistency (A > R, E > R, zero) gives measured=false and
/// Status::unavailable. Never guesses.
Result<MemorySample> sampleMemory(MemoryProbe const &) noexcept;
Result<MemorySample> sampleMemory() noexcept;

/// Reservation terms. Several terms may map to one budget Stage.
enum class Term : unsigned {
    decode, canonical, preview, topology, crop, encoder, png, href, prepared, nodes,
    history,    ///< Undo payload
    redo,       ///< Redo payload
    recovery,   ///< rollback storage and original href copy
    generation, ///< superseded old generation kept during new-generation build
    queued,     ///< results queued or canceled but not yet reaped
    cache,      ///< filter/pattern/render and new SPImage caches
    postCommit, ///< lower bound that stays live after commit (4B + Q + H)
    count
};
constexpr unsigned termCount = static_cast<unsigned>(Term::count);
Stage termStage(Term) noexcept;
char const *termName(Term) noexcept;

/// 4B + Q + H, checked (T04). B is the crop PIXEL count (4 bytes each); Q and H are bytes.
bool postCommitLowerBound(std::uint64_t cropPixelsB, std::uint64_t pngBytesQ, std::uint64_t hrefBytesH,
                          std::uint64_t &out) noexcept;

/// One simultaneous reservation: live from phase `first` through `last` inclusive.
struct Reservation {
    Term term = Term::decode;
    std::uint64_t bytes = 0;
    unsigned first = 0, last = 0;
};
constexpr unsigned maxReservations = 64, maxPhases = 16;
/// Fixed storage: building a plan never allocates. Overlapping lifetimes add, sequential ones do not.
struct ResourcePlan {
    std::array<Reservation, maxReservations> items{};
    unsigned count = 0, phases = 1;
    bool add(Term t, std::uint64_t bytes, unsigned first, unsigned last) noexcept;
};
struct Admission {
    std::uint64_t limit = 0;    ///< J from admissionLimit
    std::uint64_t peak = 0;     ///< largest simultaneous total over all phases
    unsigned peakPhase = 0;
    Term binding = Term::decode; ///< largest term live at the peak (the one to shrink)
    char const *jBinding = "";   ///< which J term set the limit
    std::array<std::uint64_t, static_cast<unsigned>(Stage::count)> stageCaps{}; ///< per-stage peak
    std::array<std::uint64_t, termCount> termCaps{}; ///< per-term peak (Undo/Redo/cache stay separate)
    /// Operation-start A0/E0 as Budget::recheck would record it; pass back as `start` for later
    /// admissions of the same operation.
    Memory effective{};
};
/// Mandatory terms: admit() derives presence from the plan items and requires recovery, history, redo and
/// queued to be live at the peak phase (an explicit 0 is allowed).
constexpr unsigned mandatoryTerms = (1u << static_cast<unsigned>(Term::recovery)) |
    (1u << static_cast<unsigned>(Term::history)) | (1u << static_cast<unsigned>(Term::redo)) |
    (1u << static_cast<unsigned>(Term::queued));
/// J term that set the limit; requires a successful admissionLimit for the same Memory.
char const *bindingJTerm(Memory const &) noexcept;
/// The Budget side of an operation: its construction ceiling and the live ledger total.
struct AdmitBudget {
    std::uint64_t ceiling = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t ledgerTotal = 0; ///< live reservation bytes; `resident` may not exceed it
};
/// Mirrors Budget::recheck's arithmetic: `now.resident` (live job bytes already inside RSS and gone from
/// available) is restored and must not exceed budget.ledgerTotal; the limit is capped by budget.ceiling;
/// `start` (a previous Admission::effective of the same operation) anchors A* = min(A0, A+r),
/// E* = max(E0, E-r). Without `start` this is the first call of an operation and resident must be 0.
/// Given the same probe sample, ceiling, ledger and anchor, Budget::recheck is expected to reach this
/// limit; the Budget remains the authority. Admission::effective is filled once admissionLimit succeeds,
/// even if the plan is then refused: a retry with a smaller plan must keep passing that same anchor.
Result<Admission> admit(ResourcePlan const &, MemorySample now, Memory const *start = nullptr,
                        AdmitBudget const &budget = {}) noexcept;

} // namespace Inkscape::Bitmap

#endif // INKSCAPE_UTIL_BITMAP_MEMORY_ADMISSION_H
