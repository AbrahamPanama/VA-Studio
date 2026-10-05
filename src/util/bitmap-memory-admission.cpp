// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap engine: RAM probes and whole-operation admission (EB4-memory).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glib/gi18n.h>
#include "util/bitmap-memory-admission.h"

#include <algorithm>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // explicit: the budget header no longer includes it
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // K32GetProcessMemoryInfo in kernel32: no extra import library
#endif
#include <psapi.h>
#endif

namespace Inkscape::Bitmap {
namespace {
Outcome unavailable(char const *why) noexcept { return {Status::unavailable, why}; }

#if defined(__APPLE__)
class NativeProbe final : public MemoryProbe {
public:
    bool read(RawMemory &out) const noexcept override
    {
        VmStats v;
        std::size_t len = sizeof v.memsize;
        if (sysctlbyname("hw.memsize", &v.memsize, &len, nullptr, 0) != 0 || len != sizeof v.memsize) return false;
        int level = 0;
        len = sizeof level;
        if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &len, nullptr, 0) != 0 || len != sizeof level)
            return false;
        v.pressureLevel = level;
        task_vm_info_data_t info{};
        mach_msg_type_number_t m = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &m) != KERN_SUCCESS)
            return false;
        v.footprint = info.phys_footprint; // includes compressed pages: never under-reports E
        return fromVmStats(v, out);
    }
};
#elif defined(_WIN32)
class NativeProbe final : public MemoryProbe {
public:
    bool read(RawMemory &out) const noexcept override
    {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof status;
        if (!GlobalMemoryStatusEx(&status)) return false;
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof counters;
        // PrivateUsage (commit charge) also covers private pages that were trimmed from the working
        // set, so it cannot under-report E the way WorkingSetSize can; shared DLL pages are excluded.
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                  sizeof counters)) return false;
        WinStatus w{status.ullTotalPhys, status.ullAvailPhys, status.ullAvailPageFile, status.ullAvailVirtual,
                    counters.PrivateUsage, sizeof(void *) == 4};
        return fromMemoryStatus(w, out);
    }
};
#else
class NativeProbe final : public MemoryProbe {
public:
    bool read(RawMemory &) const noexcept override { return false; }
};
#endif

char const *const termLabel[termCount + 1] = {
    "decode", "canonical cache", "preview", "topology", "crops", "encoder", "PNG output", "href text",
    "prepared payload", "nodes", "Undo history", "Redo history", "recovery", "old generation",
    "queued results", "caches", "post-commit bound", "invalid term"};
char const *const missing[termCount] = {
    "", "", "", "", "", "", "", "", "", "", "Plan lacks mandatory term at peak: Undo history",
    "Plan lacks mandatory term at peak: Redo history", "Plan lacks mandatory term at peak: recovery", "", "Plan lacks mandatory term at peak: queued results",
    "", ""};
} // namespace

bool fromVmStats(VmStats const &v, RawMemory &out) noexcept
{
    if (v.pressureLevel == 4) { out.refusal = N_("critical macOS memory pressure"); return false; }
    if (!v.memsize || !v.footprint ||
        (v.pressureLevel != 1 && v.pressureLevel != 2)) return false;
    out.physical = v.memsize;
    out.available = v.footprint < v.memsize ? v.memsize - v.footprint : 0;
    out.footprint = v.footprint;
    return true;
}
bool fromMemoryStatus(WinStatus const &w, RawMemory &out) noexcept
{
    // Allocations fail on commit charge, so the page-file headroom also bounds A (and the address
    // space of a 32-bit build).
    auto a = w.availPageFile;
    if (w.is32Bit) a = std::min(a, w.availVirtual);
    out.physical = w.totalPhys;
    out.available = a;
    out.footprint = w.privateUsage;
    return true;
}

char const *bindingJTerm(Memory const &) noexcept
{
    return N_("OS commit/footprint headroom - recovery");
}

MemoryProbe const &nativeMemoryProbe() noexcept
{
    static NativeProbe const probe;
    return probe;
}

Result<MemorySample> sampleMemory(MemoryProbe const &probe) noexcept
{
    Result<MemorySample> r;
    RawMemory raw;
    if (!probe.read(raw)) { r.outcome = raw.refusal ? memoryFailure(raw.refusal, 256 * MiB, 0) : unavailable("RAM probe failed"); return r; }
    if (!raw.physical || !raw.footprint) { r.outcome = unavailable("RAM probe returned zero"); return r; }
    r.value = {raw.physical, raw.available, raw.footprint, true, 0};
    r.outcome = {Status::changed, ""};
    return r;
}
Result<MemorySample> sampleMemory() noexcept { return sampleMemory(nativeMemoryProbe()); }

Stage termStage(Term t) noexcept
{
    switch (t) {
        case Term::decode: return Stage::decode;
        case Term::canonical: return Stage::canonical;
        case Term::preview: return Stage::preview;
        case Term::topology: return Stage::topology;
        case Term::crop: return Stage::crop;
        case Term::encoder: return Stage::encoder;
        case Term::png: case Term::queued: return Stage::png;
        case Term::href: return Stage::href;
        case Term::prepared: return Stage::prepared;
        case Term::nodes: case Term::postCommit: return Stage::nodes;
        case Term::history: case Term::redo: return Stage::history;
        case Term::recovery: return Stage::rollback;
        case Term::generation: case Term::cache: default: return Stage::composition;
    }
}
char const *termName(Term t) noexcept { return termLabel[t < Term::count ? static_cast<unsigned>(t) : termCount]; }

bool postCommitLowerBound(std::uint64_t b, std::uint64_t q, std::uint64_t h, std::uint64_t &out) noexcept
{
    std::uint64_t b4, s;
    if (!checkedMul(b, 4, b4) || !checkedAdd(b4, q, s) || !checkedAdd(s, h, s)) return false;
    out = s;
    return true;
}

bool ResourcePlan::add(Term t, std::uint64_t bytes, unsigned f, unsigned l) noexcept
{
    if (count >= maxReservations || t >= Term::count || f > l || l >= maxPhases) return false;
    items[count++] = {t, bytes, f, l};
    phases = std::max(phases, l + 1);
    return true;
}

Result<Admission> admit(ResourcePlan const &plan, MemorySample now, Memory const *start, AdmitBudget const &budget) noexcept
{
    Result<Admission> r;
    auto fail = [&](Status s, char const *why) { r.outcome = {s, why}; return r; };
    if (plan.count > maxReservations || plan.phases == 0 || plan.phases > maxPhases)
        return fail(Status::failed, "Invalid resource plan");
    // Same reconstruction as Budget::recheck: restore resident job bytes, anchor at operation start.
    std::uint64_t recovered;
    if (!now.measured || !now.baseline) return fail(Status::unavailable, "RAM measurements unavailable");
    if (now.resident >= now.baseline || !checkedAdd(now.available, now.resident, recovered))
        return fail(Status::failed, "Invalid resident RAM measurement");
    bool anchored = start && start->measured;
    if (now.resident && !anchored) return fail(Status::failed, "Resident RAM needs an operation-start anchor");
    if (now.resident > budget.ledgerTotal) return fail(Status::failed, "Invalid resident RAM measurement");
    Memory eff = now;
    eff.resident = 0;
    eff.available = anchored ? std::min(start->available, recovered) : recovered;
    eff.baseline = anchored ? std::max(start->baseline, now.baseline - now.resident) : now.baseline - now.resident;
    std::uint64_t limit = 0;
    Outcome o = admissionLimit(eff, limit);
    if (!o.ok()) { r.outcome = o; return r; }
    r.value.effective = eff;
    r.value.jBinding = bindingJTerm(eff);
    if (budget.ceiling < limit) { limit = budget.ceiling; r.value.jBinding = "Budget ceiling"; }
    r.value.limit = limit;
    for (unsigned p = 0; p < plan.phases; ++p) {
        std::uint64_t total = 0;
        std::array<std::uint64_t, termCount> byTerm{};
        std::array<std::uint64_t, static_cast<unsigned>(Stage::count)> byStage{};
        for (unsigned i = 0; i < plan.count; ++i) {
            auto const &it = plan.items[i];
            if (it.term >= Term::count || it.first > it.last || it.last >= maxPhases)
                return fail(Status::failed, "Invalid resource plan");
            if (it.first > p || it.last < p) continue;
            auto &t = byTerm[static_cast<unsigned>(it.term)];
            auto &s = byStage[static_cast<unsigned>(termStage(it.term))];
            if (!checkedAdd(total, it.bytes, total) || !checkedAdd(t, it.bytes, t) || !checkedAdd(s, it.bytes, s))
                return fail(Status::failed, "Plan arithmetic overflow");
        }
        for (unsigned s = 0; s < byStage.size(); ++s)
            r.value.stageCaps[s] = std::max(r.value.stageCaps[s], byStage[s]);
        for (unsigned t = 0; t < termCount; ++t) r.value.termCaps[t] = std::max(r.value.termCaps[t], byTerm[t]);
        if (total > r.value.peak) {
            r.value.peak = total;
            r.value.peakPhase = p;
            r.value.binding = static_cast<Term>(std::max_element(byTerm.begin(), byTerm.end()) - byTerm.begin());
        }
    }
    // Recovery, Undo, Redo and queued results must be live (even as an explicit 0) at the peak phase.
    for (Term t : {Term::recovery, Term::history, Term::redo, Term::queued}) {
        bool live = false;
        for (unsigned i = 0; i < plan.count; ++i)
            live = live || (plan.items[i].term == t && plan.items[i].first <= r.value.peakPhase &&
                            plan.items[i].last >= r.value.peakPhase);
        if (!live) return fail(Status::failed, missing[static_cast<unsigned>(t)]);
    }
    if (r.value.peak > limit) { r.outcome = memoryFailure(r.value.jBinding, r.value.peak, limit); return r; }
    r.outcome = {Status::changed, ""};
    return r;
}

} // namespace Inkscape::Bitmap
