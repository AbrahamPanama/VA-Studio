// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "actions/vacards-cli-edit-services.h"
#include "nesting-document.h"
namespace Inkscape::VACardsCli {
struct NestCliContext {
    EditServices &edits;
    TokenStore &tokens;
    std::function<bool()> cancelled;
};
// Capture/apply on the owning thread; worker receives immutable native polygons
// only. No portfolio/preferences, native apply alone owns atomic settlement.
Record execute_nest(Request const &, DispatchContext &, NestCliContext &);
// Native fixed-work contract; transport dispatch remains owned by the nest adapter.
using NestStopReason = Nesting::StopReason;
// Exactly one positive budget is required. Fixed work has no clock cutoff;
// the explicit timeout alternative reports deterministic=false. Seed is exact uint64.
// Stop strings: completed, work-limit, cancelled, time-limit. Failures use Status.
struct NestWorkOptions {
    std::uint64_t seed = 0;
    std::optional<std::uint64_t> iterations;
    std::optional<std::uint64_t> timeout_ms;
    unsigned workers = 1;
};
[[nodiscard]] Nesting::SolveResult solve_nest_native(Nesting::PreparedDocumentNesting const &,
    Nesting::Options, NestWorkOptions const &, Nesting::Job::ProgressCallback const & = {},
    std::stop_token = {});
inline constexpr bool native_fixed_work_contract_frozen = true;
}

namespace Inkscape::Nesting {
// Owner-thread capability. TokenPayload never stores this or document pointers.
class CliSession {
public:
    struct Capture { PreparedDocumentNesting snapshot; };
    CliSession();
    ~CliSession();
    void retire() noexcept;
    void prune(Inkscape::VACardsCli::TokenSnapshot const &);
    void retain(std::string const &, std::shared_ptr<Capture>);
    std::shared_ptr<Capture> lookup(std::string const &) const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
