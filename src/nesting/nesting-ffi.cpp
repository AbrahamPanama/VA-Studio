// SPDX-License-Identifier: GPL-2.0-or-later

#include "nesting-ffi.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "vacards_nesting.h"

namespace Inkscape::Nesting {
namespace {

static_assert(std::is_standard_layout_v<Point>);
static_assert(sizeof(VacNestingPoint) == 16);
static_assert(sizeof(VacNestingOptions) == 64);
static_assert(sizeof(VacNestingProgress) == 56);
static_assert(sizeof(VacNestingPlacement) == 40);
static_assert(static_cast<std::int32_t>(Status::Ok) == VAC_NESTING_STATUS_OK);
static_assert(static_cast<std::int32_t>(Status::InvalidArgument) == VAC_NESTING_STATUS_INVALID_ARGUMENT);
static_assert(static_cast<std::int32_t>(Status::InvalidState) == VAC_NESTING_STATUS_INVALID_STATE);
static_assert(static_cast<std::int32_t>(Status::Cancelled) == VAC_NESTING_STATUS_CANCELLED);
static_assert(static_cast<std::int32_t>(Status::SolverUnavailable) == VAC_NESTING_STATUS_SOLVER_UNAVAILABLE);
static_assert(static_cast<std::int32_t>(Status::OutOfRange) == VAC_NESTING_STATUS_OUT_OF_RANGE);
static_assert(static_cast<std::int32_t>(Status::Panic) == VAC_NESTING_STATUS_PANIC);
static_assert(static_cast<std::int32_t>(Status::InternalError) == VAC_NESTING_STATUS_INTERNAL_ERROR);
static_assert(static_cast<std::int32_t>(JobState::Invalid) == VAC_NESTING_JOB_STATE_INVALID);
static_assert(static_cast<std::int32_t>(JobState::Configuring) == VAC_NESTING_JOB_STATE_CONFIGURING);
static_assert(static_cast<std::int32_t>(JobState::Running) == VAC_NESTING_JOB_STATE_RUNNING);
static_assert(static_cast<std::int32_t>(JobState::Completed) == VAC_NESTING_JOB_STATE_COMPLETED);
static_assert(static_cast<std::int32_t>(JobState::Cancelled) == VAC_NESTING_JOB_STATE_CANCELLED);
static_assert(static_cast<std::int32_t>(JobState::Failed) == VAC_NESTING_JOB_STATE_FAILED);

auto make_c_points(std::span<Point const> points) -> std::vector<VacNestingPoint>
{
    std::vector<VacNestingPoint> result;
    result.reserve(points.size());
    for (auto const &point : points) {
        result.push_back({point.x, point.y});
    }
    return result;
}

auto make_c_options(Options const &options) -> VacNestingOptions
{
    VacNestingOptions result;
    vac_nesting_options_init(&result);
    result.part_spacing = options.part_spacing;
    result.container_margin = options.container_margin;
    result.rotation_step_degrees = options.rotation_step_degrees;
    result.random_seed = options.random_seed;
    result.time_limit_ms = options.time_limit_ms;
    result.rotation_mode = static_cast<VacNestingRotationMode>(options.rotation_mode);
    result.quality = static_cast<VacNestingQuality>(options.quality);
    result.worker_count = options.worker_count;
    return result;
}

auto from_c(VacNestingPlacement const &placement) -> Placement
{
    return {
        .part_id = placement.part_id,
        .translation_x = placement.translation_x,
        .translation_y = placement.translation_y,
        .rotation_degrees = placement.rotation_degrees,
        .placed = placement.placed != 0,
    };
}

auto from_c(VacNestingProgress const &progress) -> Progress
{
    if (progress.placement_count > 0 && !progress.placements) {
        throw std::runtime_error("nesting progress contains a null placement snapshot");
    }
    if (progress.placement_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("nesting progress placement count is out of range");
    }

    std::vector<Placement> placements;
    placements.reserve(static_cast<std::size_t>(progress.placement_count));
    for (std::uint64_t index = 0; index < progress.placement_count; ++index) {
        placements.emplace_back(from_c(progress.placements[index]));
    }
    return {
        .iteration = progress.iteration,
        .elapsed_seconds = progress.elapsed_seconds,
        .best_score = progress.best_score,
        .placed_count = progress.placed_count,
        .total_count = progress.total_count,
        .stage = progress.stage,
        .placements = std::move(placements),
    };
}

struct CallbackContext
{
    Job::ProgressCallback const *callback = nullptr;
    VacNestingJob *job = nullptr;
    std::exception_ptr exception{};
};

static_assert(std::is_aggregate_v<CallbackContext>);

extern "C" void progress_trampoline(void *user_data, VacNestingProgress const *progress) noexcept
{
    auto &context = *static_cast<CallbackContext *>(user_data);
    if (!context.callback || !progress || context.exception) {
        return;
    }
    try {
        (*context.callback)(from_c(*progress));
    } catch (...) {
        context.exception = std::current_exception();
        vac_nesting_job_cancel(context.job);
    }
}

auto as_status(VacNestingStatus status) -> Status
{
    return static_cast<Status>(status);
}

} // namespace

Job::Job(Options const &options)
{
    auto c_options = make_c_options(options);
    if (vac_nesting_options_validate(&c_options) != VAC_NESTING_STATUS_OK) {
        throw std::invalid_argument("invalid nesting options");
    }
    _job = vac_nesting_job_new(&c_options);
    if (!_job) {
        throw std::runtime_error("unable to allocate a nesting job");
    }
}

Job::~Job()
{
    vac_nesting_job_free(_job);
}

Job::Job(Job &&other) noexcept
    : _job(std::exchange(other._job, nullptr)), _part_count(std::exchange(other._part_count, 0))
{}

Job &Job::operator=(Job &&other) noexcept
{
    if (this != &other) {
        vac_nesting_job_free(_job);
        _job = std::exchange(other._job, nullptr);
        _part_count = std::exchange(other._part_count, 0);
    }
    return *this;
}

Status Job::setContainer(std::span<Point const> points)
{
    if (!_job) {
        return Status::InvalidState;
    }
    auto const c_points = make_c_points(points);
    return as_status(vac_nesting_job_set_container(_job, c_points.data(), c_points.size()));
}

Status Job::addContainerHole(std::span<Point const> points)
{
    if (!_job) {
        return Status::InvalidState;
    }
    auto const c_points = make_c_points(points);
    return as_status(vac_nesting_job_add_container_hole(_job, c_points.data(), c_points.size()));
}

Status Job::addObstacle(std::span<Point const> points)
{
    if (!_job) {
        return Status::InvalidState;
    }
    auto const c_points = make_c_points(points);
    return as_status(vac_nesting_job_add_obstacle(_job, c_points.data(), c_points.size()));
}

Status Job::addPart(std::uint64_t part_id, std::span<Point const> points)
{
    if (!_job) {
        return Status::InvalidState;
    }
    auto const c_points = make_c_points(points);
    auto const status = as_status(vac_nesting_job_add_part(_job, part_id, c_points.data(), c_points.size()));
    if (status == Status::Ok) ++_part_count;
    return status;
}

Status Job::addPart(std::uint64_t part_id, std::span<CollisionComponent const> components)
{
    if (!_job) {
        return Status::InvalidState;
    }
    if (components.empty()) {
        return Status::InvalidArgument;
    }
    auto status = as_status(vac_nesting_job_begin_part(_job, part_id));
    if (status != Status::Ok) {
        return status;
    }
    for (std::size_t component_index = 0; component_index < components.size(); ++component_index) {
        auto const outer = make_c_points(components[component_index].outer);
        status = as_status(vac_nesting_job_add_part_component_outer(_job, outer.data(), outer.size()));
        if (status != Status::Ok) {
            return status;
        }
        for (auto const &hole_points : components[component_index].holes) {
            auto const hole = make_c_points(hole_points);
            status = as_status(
                vac_nesting_job_add_part_component_hole(_job, component_index, hole.data(), hole.size()));
            if (status != Status::Ok) {
                return status;
            }
        }
    }
    status = as_status(vac_nesting_job_end_part(_job));
    if (status == Status::Ok) ++_part_count;
    return status;
}

std::optional<CollisionProxies> Job::collisionProxies(double extra, Status *status_out, std::size_t max_points)
{
    auto report = [status_out](Status value) {
        if (status_out)
            *status_out = value;
    };
    report(Status::InvalidState);
    if (!_job)
        return std::nullopt;
    std::vector<std::size_t> groups(_part_count);
    std::vector<VacNestingPoint> centers(_part_count);
    std::vector<std::size_t> offsets(_part_count + 1);
    // A proxy has at most about 1600 vertices, and copies share one: room for
    // 64 full-size distinct shapes avoids recomputing in all but rare jobs.
    std::vector<VacNestingPoint> points(std::min<std::size_t>(_part_count, 64) * 2048);
    std::size_t part_count = 0, group_count = 0, point_count = 0;
    auto call = [&] {
        return vac_nesting_job_collision_proxies(_job, extra, groups.data(), centers.data(), groups.size(),
            offsets.data(), offsets.size(), points.data(), points.size(),
            &part_count, &group_count, &point_count);
    };
    auto status = call();
    if (status == VAC_NESTING_STATUS_OUT_OF_RANGE && point_count > max_points) {
        report(Status::OutOfRange); // the caller would reject it: do not compute it again
        return std::nullopt;
    }
    if (status == VAC_NESTING_STATUS_OUT_OF_RANGE) {
        groups.resize(part_count);
        centers.resize(part_count);
        offsets.resize(group_count + 1);
        points.resize(point_count);
        status = call();
    }
    report(as_status(status));
    if (status != VAC_NESTING_STATUS_OK || part_count > groups.size() ||
        group_count + 1 > offsets.size() || point_count > points.size()) {
        if (status == VAC_NESTING_STATUS_OK)
            report(Status::InternalError);
        return std::nullopt;
    }
    CollisionProxies result;
    result.part_group.assign(groups.begin(), groups.begin() + part_count);
    for (std::size_t i = 0; i < part_count; ++i)
        result.part_center.push_back({centers[i].x, centers[i].y});
    for (std::size_t group = 0; group < group_count; ++group) {
        if (offsets[group] > offsets[group + 1] || offsets[group + 1] > point_count) {
            report(Status::InternalError);
            return std::nullopt;
        }
        auto &ring = result.rings.emplace_back();
        for (auto i = offsets[group]; i < offsets[group + 1]; ++i)
            ring.push_back({points[i].x, points[i].y});
    }
    return result;
}

Status Job::setWorkLimit(std::uint64_t limit)
{
    return static_cast<Status>(vac_nesting_job_set_work_limit(_job, limit));
}

std::optional<TerminalResult> Job::terminalResult() const
{
    VacNestingTerminal result{};
    if (vac_nesting_job_get_terminal(_job, &result) != VAC_NESTING_STATUS_OK) return std::nullopt;
    return TerminalResult{static_cast<StopReason>(result.stop_reason), result.completed_work};
}

Status Job::run(ProgressCallback const &callback)
{
    if (!_job) {
        return Status::InvalidState;
    }
    CallbackContext context{.callback = callback ? &callback : nullptr, .job = _job};
    auto const status =
        as_status(vac_nesting_job_run(_job, callback ? progress_trampoline : nullptr, callback ? &context : nullptr));
    if (context.exception) {
        std::rethrow_exception(context.exception);
    }
    return status;
}

Status Job::validate(std::span<Placement const> placements)
{
    std::vector<VacNestingPlacement> records;
    records.reserve(placements.size());
    for (auto const &p : placements) {
        VacNestingPlacement record{};
        record.part_id = p.part_id;
        record.translation_x = p.translation_x;
        record.translation_y = p.translation_y;
        record.rotation_degrees = p.rotation_degrees;
        record.placed = p.placed ? 1 : 0;
        records.push_back(record);
    }
    return as_status(vac_nesting_job_validate_candidate(_job, records.data(), records.size()));
}

void Job::cancel() noexcept
{
    vac_nesting_job_cancel(_job);
}

JobState Job::state() const noexcept
{
    return static_cast<JobState>(vac_nesting_job_state(_job));
}

std::vector<Placement> Job::results() const
{
    std::vector<Placement> result;
    if (!_job) {
        return result;
    }
    auto const count = vac_nesting_job_result_count(_job);
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        VacNestingPlacement placement{};
        auto const status = vac_nesting_job_result_at(_job, index, &placement);
        if (status != VAC_NESTING_STATUS_OK) {
            throw std::runtime_error(error());
        }
        result.emplace_back(from_c(placement));
    }
    return result;
}

std::string Job::error() const
{
    auto const *message = vac_nesting_job_error(_job);
    return message ? message : "nesting bridge returned a null error message";
}

char const *statusMessage(Status status) noexcept
{
    auto const *message = vac_nesting_status_message(static_cast<VacNestingStatus>(status));
    return message ? message : "unknown nesting status";
}

} // namespace Inkscape::Nesting
