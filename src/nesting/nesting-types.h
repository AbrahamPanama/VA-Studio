// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_NESTING_TYPES_H
#define INKSCAPE_NESTING_TYPES_H

#include <cstdint>
#include <vector>

namespace Inkscape::Nesting {

// Safe finite default for direct engine use. The application replaces this
// with the selected preset; an explicit zero means no deadline.
constexpr std::uint64_t DEFAULT_ENGINE_TIME_LIMIT_MS = 1'000;

enum class Status : std::int32_t
{
    Ok = 0,
    InvalidArgument = 1,
    InvalidState = 2,
    Cancelled = 3,
    SolverUnavailable = 4,
    OutOfRange = 5,
    Panic = 6,
    InternalError = 7,
};

enum class JobState : std::int32_t
{
    Invalid = -1,
    Configuring = 0,
    Running = 1,
    Completed = 2,
    Cancelled = 3,
    Failed = 4,
};

enum class StopReason : std::int32_t { Completed = 0, WorkLimit = 1, Cancelled = 2, TimeLimit = 3 };
struct TerminalResult {
    StopReason stop_reason = StopReason::Completed;
    std::uint64_t completed_work = 0;
};

enum class RotationMode : std::int32_t
{
    None = 0,
    RightAngles = 1,
    Discrete = 2,
    Free = 3,
};

enum class Quality : std::int32_t
{
    Draft = 0,
    Balanced = 1,
    High = 2,
};

struct Point
{
    double x = 0.0;
    double y = 0.0;
};

struct CollisionComponent
{
    std::vector<Point> outer;
    std::vector<std::vector<Point>> holes;
};

struct Options
{
    double part_spacing = 0.0;
    double container_margin = 0.0;
    double rotation_step_degrees = 15.0;
    std::uint64_t random_seed = 0;
    std::uint64_t time_limit_ms = DEFAULT_ENGINE_TIME_LIMIT_MS;
    RotationMode rotation_mode = RotationMode::Free;
    Quality quality = Quality::Balanced;
    std::uint32_t worker_count = 0;
};

struct Placement
{
    std::uint64_t part_id = 0;
    double translation_x = 0.0;
    double translation_y = 0.0;
    double rotation_degrees = 0.0;
    bool placed = false;
};

struct Progress
{
    std::uint64_t iteration = 0;
    double elapsed_seconds = 0.0;
    double best_score = 0.0;
    std::uint32_t placed_count = 0;
    std::uint32_t total_count = 0;
    std::uint32_t stage = 0;
    std::vector<Placement> placements;
};

} // namespace Inkscape::Nesting

#endif // INKSCAPE_NESTING_TYPES_H
