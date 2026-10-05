// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_NESTING_FFI_H
#define INKSCAPE_NESTING_FFI_H

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "nesting-types.h"

struct VacNestingJob;

namespace Inkscape::Nesting {

struct CollisionProxies {
    std::vector<std::size_t> part_group;
    std::vector<Point> part_center;
    std::vector<std::vector<Point>> rings;
};

class Job final
{
public:
    using ProgressCallback = std::function<void(Progress const &)>;

    explicit Job(Options const &options = {});
    ~Job();

    Job(Job const &) = delete;
    Job &operator=(Job const &) = delete;
    Job(Job &&other) noexcept;
    Job &operator=(Job &&other) noexcept;

    [[nodiscard]] Status setContainer(std::span<Point const> points);
    [[nodiscard]] Status addContainerHole(std::span<Point const> points);
    /// A fixed object already on the sheet; parts keep part_spacing from it.
    [[nodiscard]] Status addObstacle(std::span<Point const> points);
    [[nodiscard]] Status addPart(std::uint64_t part_id, std::span<Point const> points);
    [[nodiscard]] Status addPart(std::uint64_t part_id, std::span<CollisionComponent const> components);

    // run() is synchronous and single-use. cancel() may be called from a
    // progress callback or another thread; destruction must wait for run().
    [[nodiscard]] Status setWorkLimit(std::uint64_t limit);
    [[nodiscard]] std::optional<TerminalResult> terminalResult() const;
    [[nodiscard]] Status run(ProgressCallback const &callback = {});
    [[nodiscard]] Status validate(std::span<Placement const> placements);
    /// `status` (optional) tells a stop (Cancelled: cancel or the job's time
    /// limit) from a missing proxy; results above `max_points` vertices are
    /// not fetched (OutOfRange).
    [[nodiscard]] std::optional<CollisionProxies> collisionProxies(double extra, Status *status = nullptr,
                                                                   std::size_t max_points = SIZE_MAX);
    void cancel() noexcept;

    [[nodiscard]] JobState state() const noexcept;
    [[nodiscard]] std::vector<Placement> results() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] explicit operator bool() const noexcept { return _job != nullptr; }

private:
    VacNestingJob *_job = nullptr;
    std::size_t _part_count = 0;
};

[[nodiscard]] char const *statusMessage(Status status) noexcept;

} // namespace Inkscape::Nesting

#endif // INKSCAPE_NESTING_FFI_H
