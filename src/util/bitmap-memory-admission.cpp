// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap engine: RAM probes and whole-operation admission (EB4-memory).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

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
        static mach_port_t const host = mach_host_self(); // one send right for the process lifetime
        VmStats v;
        std::size_t len = sizeof v.memsize;
        if (sysctlbyname("hw.memsize", &v.memsize, &len, nullptr, 0) != 0 || len != sizeof v.memsize) return false;
        int level = 0;
        len = sizeof level;
        if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &len, nullptr, 0) != 0 || len != sizeof level)
            return false;
        v.pressureLevel = level;
        v.pageSize = vm_kernel_page_size; // the unit of vm_statistics64 counters (Rosetta safe)
        vm_statistics64_data_t vm{};
        mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
        if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &n) != KERN_SUCCESS)
            return false;
        v.freePages = vm.free_count; // includes speculative pages
        v.inactivePages = vm.inactive_count;
        v.externalPages = vm.external_page_count;
        v.speculativePages = vm.speculative_count;
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
char const *const ramRefusal[termCount] = {
    "Plan exceeds RAM limit: decode", "Plan exceeds RAM limit: canonical cache",
    "Plan exceeds RAM limit: preview", "Plan exceeds RAM limit: topology",
    "Plan exceeds RAM limit: crops", "Plan exceeds RAM limit: encoder",
    "Plan exceeds RAM limit: PNG output", "Plan exceeds RAM limit: href text",
    "Plan exceeds RAM limit: prepared payload", "Plan exceeds RAM limit: nodes",
    "Plan exceeds RAM limit: Undo history", "Plan exceeds RAM limit: Redo history",
    "Plan exceeds RAM limit: recovery", "Plan exceeds RAM limit: old generation",
    "Plan exceeds RAM limit: queued results", "Plan exceeds RAM limit: caches",
    "Plan exceeds RAM limit: post-commit bound"};
char const *refusal(Term t) noexcept { return ramRefusal[static_cast<unsigned>(t)]; }
char const *const missing[termCount] = {
    "", "", "", "", "", "", "", "", "", "", "Plan lacks mandatory term at peak: Undo history",
    "Plan lacks mandatory term at peak: Redo history", "Plan lacks mandatory term at peak: recovery", "", "Plan lacks mandatory term at peak: queued results",
    "", ""};
} // namespace

bool fromVmStats(VmStats const &v, RawMemory &out) noexcept
{
    std::uint64_t pages, bytes;
    if (!v.pageSize || !v.memsize || v.pressureLevel < 1 || v.pressureLevel >= 4) return false; // critical/unknown
    if (!checkedAdd(v.freePages, std::min(v.inactivePages, v.externalPages > v.speculativePages ? v.externalPages - v.speculativePages : 0), pages) ||
        !checkedMul(pages, v.pageSize, bytes)) return false;
    if (v.pressureLevel > 1) bytes /= 2; // warn: the system is already reclaiming
    out.physical = v.memsize;
    out.available = bytes;
    out.footprint = v.footprint;
    return true;
}
bool fromMemoryStatus(WinStatus const &w, RawMemory &out) noexcept
{
    // Allocations fail on commit charge, so the page-file headroom also bounds A (and the address
    // space of a 32-bit build).
    auto a = std::min(w.availPhys, w.availPageFile);
    if (w.is32Bit) a = std::min(a, w.availVirtual);
    out.physical = w.totalPhys;
    out.available = a;
    out.footprint = w.privateUsage;
    return true;
}

char const *bindingJTerm(Memory const &m) noexcept
{
    constexpr auto recovery = 256 * MiB, process = 3072 * MiB;
    if (m.baseline > process - recovery || m.available < recovery) return "none";
    struct JTerm { char const *name; std::uint64_t value; };
    JTerm const terms[] = {{"1.5 GiB ceiling", 1536 * MiB}, {"physical RAM / 4", m.physical / 4},
                           {"process cap (3 GiB - E - recovery)", process - recovery - m.baseline},
                           {"available RAM - recovery", m.available - recovery}};
    auto best = &terms[0];
    for (auto &t : terms) if (t.value < best->value) best = &t;
    return best->name;
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
    if (!probe.read(raw)) { r.outcome = unavailable("RAM probe failed"); return r; }
    if (!raw.physical || !raw.available || !raw.footprint) { r.outcome = unavailable("RAM probe returned zero"); return r; }
    if (raw.available > raw.physical || raw.footprint > raw.physical) {
        r.outcome = unavailable("RAM probe values inconsistent");
        return r;
    }
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
    if (!now.measured || !now.baseline || !now.available) return fail(Status::unavailable, "RAM measurements unavailable");
    if (now.resident >= now.baseline || !checkedAdd(now.available, now.resident, recovered) || recovered > now.physical)
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
    // bindingJTerm duplicates the EB1 constants; refuse rather than mislabel if they ever drift.
    {
        constexpr auto recovery = 256 * MiB, process = 3072 * MiB;
        auto expect = std::min({1536 * MiB, eff.physical / 4, process - recovery - eff.baseline,
                                eff.available - recovery});
        if (expect != limit) return fail(Status::failed, "J binding term out of sync with admissionLimit");
    }
    if (budget.ceiling < limit) { limit = budget.ceiling; r.value.jBinding = "Budget ceiling"; }
    r.value.limit = limit;
    std::uint64_t combined = 0; // crop + encoder alive together
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
        combined = std::max(combined, byStage[static_cast<unsigned>(Stage::crop)] +
                                      byStage[static_cast<unsigned>(Stage::encoder)]);
        for (unsigned s = 0; s < byStage.size(); ++s)
            r.value.stageCaps[s] = std::max(r.value.stageCaps[s], byStage[s]);
        for (unsigned t = 0; t < termCount; ++t) r.value.termCaps[t] = std::max(r.value.termCaps[t], byTerm[t]);
        if (total > r.value.peak) {
            r.value.peak = total;
            r.value.peakPhase = p;
            r.value.binding = static_cast<Term>(std::max_element(byTerm.begin(), byTerm.end()) - byTerm.begin());
        }
    }
    auto cap = [&](Stage s) { return r.value.stageCaps[static_cast<unsigned>(s)]; };
    // Fixed ceilings are not RAM dependent; they refuse even on a roomy machine.
    if (cap(Stage::topology) > 256 * MiB) { r.value.binding = Term::topology; return fail(Status::failed, "Plan exceeds fixed cap: topology 256 MiB"); }
    if (cap(Stage::preview) > 4 * MiB) { r.value.binding = Term::preview; return fail(Status::failed, "Plan exceeds fixed cap: preview 4 MiB"); }
    if (cap(Stage::href) > 512 * MiB) { r.value.binding = Term::href; return fail(Status::failed, "Plan exceeds fixed cap: href 512 MiB"); }
    if (combined > 128 * MiB) {
        r.value.binding = cap(Stage::crop) >= cap(Stage::encoder) ? Term::crop : Term::encoder;
        return fail(Status::failed, "Plan exceeds fixed cap: crops plus encoder 128 MiB");
    }
    // Recovery, Undo, Redo and queued results must be live (even as an explicit 0) at the peak phase.
    for (Term t : {Term::recovery, Term::history, Term::redo, Term::queued}) {
        bool live = false;
        for (unsigned i = 0; i < plan.count; ++i)
            live = live || (plan.items[i].term == t && plan.items[i].first <= r.value.peakPhase &&
                            plan.items[i].last >= r.value.peakPhase);
        if (!live) return fail(Status::failed, missing[static_cast<unsigned>(t)]);
    }
    if (r.value.peak > limit) return fail(Status::failed, refusal(r.value.binding));
    r.outcome = {Status::changed, ""};
    return r;
}

} // namespace Inkscape::Bitmap
