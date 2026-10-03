// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: read-only Undo capacity preflight (EB5-undo).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/explode-bitmap-undo.h"

#include <algorithm>
#include <limits>
#include <new>

#include "document.h"
#include "event-log.h"
#include "event.h"
#include "preferences.h"
#include "util/bitmap-memory-admission.h"

namespace Inkscape::Bitmap {

namespace {

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

bool addU64(std::uint64_t a, std::uint64_t b, std::uint64_t &out) noexcept
{
    if (a > kMax - b) return false;
    out = a + b;
    return true;
}

UndoAdmission refuse(UndoAdmission a, UndoRefusal why, char const *text) noexcept
{
    a.admitted = false;
    a.reason = why;
    a.diagnostic = text;
    return a;
}

} // namespace

char const *undoRefusalName(UndoRefusal r) noexcept
{
    switch (r) {
        case UndoRefusal::none: return "none";
        case UndoRefusal::historyUnreadable: return "history unreadable";
        case UndoRefusal::arithmetic: return "arithmetic overflow";
        case UndoRefusal::countCapInvalid: return "history count cap invalid";
        case UndoRefusal::countCap: return "history count cap too small";
        case UndoRefusal::payloadTooLarge: return "new payload above 40% of the Undo budget";
        case UndoRefusal::settlementUnavailable: return "history settlement unavailable";
    }
    return "unknown";
}

bool DocumentHistorySource::readUsage(HistoryUsage &out) const noexcept
{
    try {
        // get_event_log() is a non-const accessor but this walk only reads.
        auto *log = const_cast<SPDocument &>(_doc).get_event_log();
        if (!log) return false;
        auto const store = log->getEventListStore();
        if (!store) return false;
        auto const &columns = EventLog::getColumns();
        auto const current = log->getCurrEvent();

        HistoryUsage usage;
        bool past_current = false; // rows after the current one are Redo
        bool ok = true;
        bool first = true;
        auto visit = [&](auto it) {
            if (first) { // start pseudo row: neither Undo nor Redo; when current, every later row is Redo
                first = false;
                if (it == current) past_current = true;
                return;
            }
            Event *event = (*it)[columns.event];
            if (!event) { ok = false; return; }
            auto &bytes = past_current ? usage.redoBytes : usage.undoBytes;
            auto &count = past_current ? usage.redoCount : usage.undoCount;
            if (!addU64(bytes, event->payload_bytes, bytes) || !addU64(count, 1, count)) ok = false;
            if (it == current) past_current = true;
        };
        for (auto parent = store->children().begin(); parent != store->children().end(); ++parent) {
            visit(parent);
            for (auto child = parent->children().begin(); child != parent->children().end(); ++child) {
                visit(child);
            }
            if (!ok) return false;
        }
        out = usage;
        return true;
    } catch (...) {
        return false;
    }
}

bool DocumentHistorySource::readLimits(UndoLimits &out) const noexcept
{
    try {
        auto *prefs = Preferences::get();
        if (!prefs) return false;
        out.limitUndo = prefs->getBool("/options/undo/limit", true);
        out.countCap = prefs->getInt("/options/undo/size", 200);
        out.maxMb = prefs->getInt("/options/undo/max-mb", 1024);
        return true;
    } catch (...) {
        return false;
    }
}

bool DocumentHistorySource::reserveSettlement(std::uint64_t entries) const noexcept
{
    if (entries == 0 || entries > (1u << 16)) return false;
    // Probe the storage a settlement needs (an Event and a log row per entry), then give it back.
    auto *probe = new (std::nothrow) unsigned char[entries * (sizeof(Event) + 1024)];
    if (!probe) return false;
    delete[] probe;
    return true;
}

UndoAdmission preflightUndo(UndoHistorySource const &source, EventPayloads const &payloads) noexcept
{
    UndoAdmission a;
    a.entries = payloads.conversion ? 2 : 1;
    if (!addU64(payloads.explodeBytes, payloads.conversion ? payloads.conversionBytes : 0, a.newBytes)) {
        return refuse(a, UndoRefusal::arithmetic, "new payload sum overflows");
    }
    UndoLimits limits;
    if (!source.readLimits(limits)) return refuse(a, UndoRefusal::historyUnreadable, "undo limits unreadable");
    if (!source.readUsage(a.usage)) return refuse(a, UndoRefusal::historyUnreadable, "history unreadable");

    // U: the configured positive budget capped at 1,024 MiB; disabled/nonpositive uses the feature cap
    // without touching the preference.
    a.budgetBytes = undoFeatureCapBytes;
    if (limits.limitUndo && limits.maxMb > 0) {
        auto const mb = static_cast<std::uint64_t>(limits.maxMb);
        a.budgetBytes = mb >= undoFeatureCapBytes / (1024 * 1024) ? undoFeatureCapBytes : mb * 1024 * 1024;
    }
    a.maxNewBytes = a.budgetBytes * 2 / 5; // floor(2U/5); U <= 1 GiB so no overflow
    if (limits.limitUndo) {
        if (limits.countCap <= 0) return refuse(a, UndoRefusal::countCapInvalid, "undo size is not positive");
        a.countCap = static_cast<std::uint64_t>(limits.countCap);
        if (a.entries > a.countCap) return refuse(a, UndoRefusal::countCap, "undo size cannot hold every new entry");
    }
    if (a.newBytes > a.maxNewBytes) return refuse(a, UndoRefusal::payloadTooLarge, "new payload above 40% of U");
    std::uint64_t retainedPlusNew = 0;
    if (!addU64(a.usage.undoBytes, a.newBytes, retainedPlusNew)) {
        return refuse(a, UndoRefusal::arithmetic, "retained plus new overflows");
    }
    // Native trimming expires the oldest entries while the total is above U (keeping the newest), so the
    // history term cannot stay above U.
    a.historyTermBytes = std::min(retainedPlusNew, a.budgetBytes);
    std::uint64_t peak = 0;
    if (!addU64(a.historyTermBytes, a.usage.redoBytes, peak)) {
        return refuse(a, UndoRefusal::arithmetic, "history plus Redo overflows");
    }
    a.redoTermBytes = a.usage.redoBytes;
    if (!source.reserveSettlement(a.entries)) {
        return refuse(a, UndoRefusal::settlementUnavailable, "settlement reservation failed");
    }
    a.admitted = true;
    a.reason = UndoRefusal::none;
    return a;
}

UndoAdmission preflightUndo(SPDocument const &doc, EventPayloads const &payloads) noexcept
{
    return preflightUndo(DocumentHistorySource{doc}, payloads);
}

bool addUndoTerms(ResourcePlan &plan, UndoAdmission const &a, unsigned first, unsigned last) noexcept
{
    return a.admitted && plan.add(Term::history, a.historyTermBytes, first, last) &&
           plan.add(Term::redo, a.redoTermBytes, first, last);
}

} // namespace Inkscape::Bitmap
