// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_VACARDS_CLI_INTAKE_TESTING_H
#define INKSCAPE_IO_VACARDS_CLI_INTAKE_TESTING_H
#include <cstdint>
#include <functional>
#include "io/vacards-cli-resources.h"
#include <optional>

// Test-only declaration: production never includes this header. There is no
// environment, CLI option or request route to this thread-local memory override.
namespace Inkscape::VACardsCli {
// Synchronous per-call witness after the opened file's initial metadata/limit check.
// Production read_admitted always passes no observer. No global hooks.
AdmittedBytes read_admitted_after_stat_for_testing(ResourceAccess const &, std::uint64_t,
                                                  std::function<void()> const &after_stat);
namespace detail {
std::optional<std::uint64_t> exchange_intake_memory_for_testing(std::optional<std::uint64_t>) noexcept;
}
#ifndef INKSCAPE_VACARDS_CLI_INTAKE_MEMORY_TEST_CLASS
#define INKSCAPE_VACARDS_CLI_INTAKE_MEMORY_TEST_CLASS
class ScopedIntakeMemoryForTesting
{
public:
    explicit ScopedIntakeMemoryForTesting(std::uint64_t available_bytes)
        : previous(detail::exchange_intake_memory_for_testing(available_bytes)) {}
    ~ScopedIntakeMemoryForTesting() { detail::exchange_intake_memory_for_testing(previous); }
    ScopedIntakeMemoryForTesting(ScopedIntakeMemoryForTesting const &) = delete;
    ScopedIntakeMemoryForTesting &operator=(ScopedIntakeMemoryForTesting const &) = delete;
private:
    std::optional<std::uint64_t> previous;
};
#endif
} // namespace Inkscape::VACardsCli
#endif
