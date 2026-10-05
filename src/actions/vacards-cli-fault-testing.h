// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "vacards-cli-fault.h"
#include "vacards-cli-session.h"
#include "vacards-cli-tokens.h"
#include <utility>
namespace Inkscape::VACardsCli {
namespace detail {
CliFaultPlan *exchange_cli_fault_plan_for_testing(CliFaultPlan *) noexcept;
}
// Synchronous owner-thread scope: nested plans replace, then restore the outer
// plan including its counters. No inheritance to other threads, no wire/env route.
class ScopedCliFaultPlanForTesting final {
public:
    explicit ScopedCliFaultPlanForTesting(detail::CliFaultPlan plan)
        : plan_(std::move(plan)), previous_(detail::exchange_cli_fault_plan_for_testing(&plan_)) {}
    ~ScopedCliFaultPlanForTesting() { detail::exchange_cli_fault_plan_for_testing(previous_); }
    ScopedCliFaultPlanForTesting(ScopedCliFaultPlanForTesting const &) = delete;
    ScopedCliFaultPlanForTesting &operator=(ScopedCliFaultPlanForTesting const &) = delete;
    detail::CliFaultPlan const &plan() const noexcept { return plan_; }
private:
    detail::CliFaultPlan plan_;
    detail::CliFaultPlan *previous_;
};
// Borrowed synchronous access to a real Engine for native preservation oracles.
// No replacement dispatch or result callback; execute is Engine::execute itself.
void with_cli_engine_for_testing(SessionOptions,
    std::function<void(DispatchContext &, FileState &, TokenStore &,
                       std::function<Record(Request const &)> const &)> const &);
}
