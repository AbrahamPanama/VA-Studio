// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace Inkscape::VACardsCli {
// Checker-only production surface. Arming is declared in the testing header only.
enum class CliFaultKind {
    ServiceException, Analysis, Contour, Encoding, Publication, Renderer,
    MemoryAdmission, StaleCapture, StaleDependency, CommitRefusal
};
bool cli_fault(std::string_view point, CliFaultKind kind) noexcept;
void cli_fault_throw(std::string_view point);
std::uint64_t cli_counted_limit(std::string_view name, std::uint64_t production_limit) noexcept;
namespace detail {
struct CliFaultEntry {
    std::string point;
    CliFaultKind kind;
    std::uint64_t occurrence = 1;
    std::uint64_t visits = 0;
};
struct CliLimitEntry { std::string name; std::uint64_t ceiling; };
struct CliFaultPlan { std::vector<CliFaultEntry> faults; std::vector<CliLimitEntry> limits; };
}
}
