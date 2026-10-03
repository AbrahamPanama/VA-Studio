// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: read-only Undo capacity preflight (EB5-undo).
 * Plan: doc/vacards/EXPLODE_BITMAP_IMPLEMENTATION_PLAN.md, doc/vacards/EXPLODE_BITMAP_PLAN.md v3.2 §13.4.
 *
 * The check runs before either publication (optional "convert to one bitmap" plus the explode) is admitted.
 * It never trims, expires, clears or otherwise touches history, Redo, selection or the document.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_EXPLODE_BITMAP_UNDO_H
#define INKSCAPE_UI_EXPLODE_BITMAP_UNDO_H

#include <cstdint>

class SPDocument;

namespace Inkscape::Bitmap {

struct ResourcePlan;

/// The feature never relies on more than this much Undo, whatever the user preference says (U <= 1,024 MiB).
constexpr std::uint64_t undoFeatureCapBytes = std::uint64_t{1024} * 1024 * 1024;

/// Complete new event payloads (old + new href, XML/style/structure, metadata), prepared before admission.
struct EventPayloads {
    bool conversion = false;          ///< the pair: conversion entry first, then Explode
    std::uint64_t conversionBytes = 0; ///< only read when `conversion`
    std::uint64_t explodeBytes = 0;
};

enum class UndoRefusal : unsigned {
    none,
    historyUnreadable,     ///< history or limits could not be read (fault, no event log, bad row)
    arithmetic,            ///< a checked sum overflowed
    countCapInvalid,       ///< limiting is on but the count cap is not positive
    countCap,              ///< the count cap cannot retain every new entry
    payloadTooLarge,       ///< new payload(s) above 40% of U
    settlementUnavailable, ///< the settlement reservation failed
};
char const *undoRefusalName(UndoRefusal) noexcept;

/// Retained Undo and Redo, as the document history reports them now.
struct HistoryUsage {
    std::uint64_t undoBytes = 0, undoCount = 0, redoBytes = 0, redoCount = 0;
};
/// Preferences: /options/undo/limit, /options/undo/size, /options/undo/max-mb.
struct UndoLimits {
    bool limitUndo = true;
    std::int64_t countCap = 200;
    std::int64_t maxMb = 1024;
};

/// Seam for tests and fault injection. Implementations must not throw and must not mutate anything.
class UndoHistorySource {
public:
    virtual ~UndoHistorySource() = default;
    virtual bool readUsage(HistoryUsage &) const noexcept = 0;
    virtual bool readLimits(UndoLimits &) const noexcept = 0;
    /// Probe that the allocations for settling `entries` new history rows are available; releases them again.
    virtual bool reserveSettlement(std::uint64_t entries) const noexcept = 0;
};

/// Reads the document's EventLog rows (Undo = start row .. current row, Redo = rows after it) and the
/// preferences. The document is only read.
class DocumentHistorySource final : public UndoHistorySource {
public:
    explicit DocumentHistorySource(SPDocument const &doc) noexcept : _doc(doc) {}
    bool readUsage(HistoryUsage &) const noexcept override;
    bool readLimits(UndoLimits &) const noexcept override;
    bool reserveSettlement(std::uint64_t entries) const noexcept override;
private:
    SPDocument const &_doc;
};

struct UndoAdmission {
    bool admitted = false;
    UndoRefusal reason = UndoRefusal::none;
    char const *diagnostic = "";
    std::uint64_t entries = 0;       ///< 1 (direct) or 2 (pair)
    std::uint64_t newBytes = 0;      ///< combined new payload
    std::uint64_t budgetBytes = 0;   ///< U = min(1,024 MiB, positive max-mb); 1,024 MiB when disabled
    std::uint64_t maxNewBytes = 0;   ///< floor(0.40 U)
    std::uint64_t countCap = 0;      ///< 0 = not limited
    HistoryUsage usage;              ///< what was read; refusals keep it for the message
    std::uint64_t historyTermBytes = 0; ///< min(U, retained Undo + new): Term::history (native trimming bounds it)
    std::uint64_t redoTermBytes = 0;    ///< Redo kept until settlement: Term::redo
};

/// Admit the pair (or the direct operation) only if the combined new payload is <= floor(2U/5) (so each
/// single entry is too) and the count cap can hold all new entries. Retained + new above U or the cap is NOT
/// a refusal: the oldest entries expire by the native byte (max-mb) and count policy, never the new entries.
/// Read-only; the preflight itself never trims history.
UndoAdmission preflightUndo(UndoHistorySource const &, EventPayloads const &) noexcept;
UndoAdmission preflightUndo(SPDocument const &, EventPayloads const &) noexcept;

/// Add the history and Redo reservations of an admitted preflight to an EB4 plan for phases first..last.
bool addUndoTerms(ResourcePlan &, UndoAdmission const &, unsigned first, unsigned last) noexcept;

} // namespace Inkscape::Bitmap

#endif // INKSCAPE_UI_EXPLODE_BITMAP_UNDO_H
