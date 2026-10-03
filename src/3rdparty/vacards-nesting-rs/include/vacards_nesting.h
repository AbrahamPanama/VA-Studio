// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VACARDS_NESTING_H
#define VACARDS_NESTING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VAC_NESTING_API_VERSION 3u
#define VAC_NESTING_JAGUA_REVISION "9a19409bd38f3643c3d6d2d7571cddfba548ee17"

/* Fixed-width integer constants keep the C ABI identical on every platform. */
typedef int32_t VacNestingStatus;
#define VAC_NESTING_STATUS_OK ((VacNestingStatus)0)
#define VAC_NESTING_STATUS_INVALID_ARGUMENT ((VacNestingStatus)1)
#define VAC_NESTING_STATUS_INVALID_STATE ((VacNestingStatus)2)
#define VAC_NESTING_STATUS_CANCELLED ((VacNestingStatus)3)
#define VAC_NESTING_STATUS_SOLVER_UNAVAILABLE ((VacNestingStatus)4)
#define VAC_NESTING_STATUS_OUT_OF_RANGE ((VacNestingStatus)5)
#define VAC_NESTING_STATUS_PANIC ((VacNestingStatus)6)
#define VAC_NESTING_STATUS_INTERNAL_ERROR ((VacNestingStatus)7)

typedef int32_t VacNestingJobState;
#define VAC_NESTING_JOB_STATE_INVALID ((VacNestingJobState) - 1)
#define VAC_NESTING_JOB_STATE_CONFIGURING ((VacNestingJobState)0)
#define VAC_NESTING_JOB_STATE_RUNNING ((VacNestingJobState)1)
#define VAC_NESTING_JOB_STATE_COMPLETED ((VacNestingJobState)2)
#define VAC_NESTING_JOB_STATE_CANCELLED ((VacNestingJobState)3)
#define VAC_NESTING_JOB_STATE_FAILED ((VacNestingJobState)4)

typedef int32_t VacNestingRotationMode;
#define VAC_NESTING_ROTATION_NONE ((VacNestingRotationMode)0)
#define VAC_NESTING_ROTATION_RIGHT_ANGLES ((VacNestingRotationMode)1)
#define VAC_NESTING_ROTATION_DISCRETE ((VacNestingRotationMode)2)
#define VAC_NESTING_ROTATION_FREE ((VacNestingRotationMode)3)

typedef int32_t VacNestingQuality;
#define VAC_NESTING_QUALITY_DRAFT ((VacNestingQuality)0)
#define VAC_NESTING_QUALITY_BALANCED ((VacNestingQuality)1)
#define VAC_NESTING_QUALITY_HIGH ((VacNestingQuality)2)

typedef struct VacNestingPoint
{
    double x;
    double y;
} VacNestingPoint;

typedef struct VacNestingOptions
{
    /* Set by vac_nesting_options_init(); callers must preserve both fields. */
    uint32_t struct_size;
    uint32_t api_version;
    /* Minimum document-coordinate clearance between parts. */
    double part_spacing;
    /* Minimum document-coordinate clearance from the container boundary and
     * its holes (a wall, independent of part_spacing). */
    double container_margin;
    /* Used only by VAC_NESTING_ROTATION_DISCRETE. */
    double rotation_step_degrees;
    /* Makes repeated runs deterministic for identical input and options. */
    uint64_t random_seed;
    /* Zero means no deadline. The first complete constructive layout is always
     * published, even after the limit; expiration stops further improvement and
     * publishes the best layout found. */
    uint64_t time_limit_ms;
    VacNestingRotationMode rotation_mode;
    VacNestingQuality quality;
    /* Execution hint; zero lets the engine choose and it may use fewer. */
    uint32_t worker_count;
    uint32_t reserved;
} VacNestingOptions;

typedef struct VacNestingPlacement
{
    uint64_t part_id;
    double translation_x;
    double translation_y;
    double rotation_degrees;
    uint8_t placed;
    uint8_t reserved[7];
} VacNestingPlacement;

typedef struct VacNestingProgress
{
    uint64_t iteration;
    double elapsed_seconds;
    double best_score;
    uint32_t placed_count;
    uint32_t total_count;
    uint32_t stage;
    uint32_t reserved;
    /*
     * Optional best-so-far snapshot. This memory is borrowed and remains valid
     * only until the progress callback returns; callers must copy it.
     */
    VacNestingPlacement const *placements;
    uint64_t placement_count;
} VacNestingProgress;

#define VAC_NESTING_PROGRESS_VALIDATING 0u
#define VAC_NESTING_PROGRESS_SOLVING 1u
#define VAC_NESTING_PROGRESS_FINALIZING 2u

typedef struct VacNestingJob VacNestingJob;
typedef void (*VacNestingProgressCallback)(void *user_data, VacNestingProgress const *progress);

uint32_t vac_nesting_api_version(void);
char const *vac_nesting_jagua_revision(void);
uint32_t vac_nesting_dependency_probe(void);
int32_t vac_nesting_run_self_test(void);

char const *vac_nesting_status_message(VacNestingStatus status);
/* Always initialize options; this stamps the ABI size and version fields. */
void vac_nesting_options_init(VacNestingOptions *options);
VacNestingStatus vac_nesting_options_validate(VacNestingOptions const *options);

/*
 * Job inputs are copied before each call returns. A job is single-use after
 * run() starts. All job calls except free() are internally synchronized;
 * free() must only be called after run() has returned on every thread. A raw C
 * progress callback must not throw or unwind across this boundary.
 */
VacNestingJob *vac_nesting_job_new(VacNestingOptions const *options);
VacNestingStatus vac_nesting_job_set_container(VacNestingJob *job, VacNestingPoint const *points, size_t point_count);
VacNestingStatus vac_nesting_job_add_container_hole(VacNestingJob *job, VacNestingPoint const *points,
                                                    size_t point_count);
/* Additive ABI-3 capability. A fixed obstacle already on the sheet (for
 * example a part nested earlier). Parts keep part_spacing from it; the
 * container margin does not apply. Obstacles are never moved and have no
 * result entry. Call after vac_nesting_job_set_container(). */
#define VAC_NESTING_HAS_OBSTACLES 1
VacNestingStatus vac_nesting_job_add_obstacle(VacNestingJob *job, VacNestingPoint const *points, size_t point_count);
VacNestingStatus vac_nesting_job_add_part(VacNestingJob *job, uint64_t part_id, VacNestingPoint const *points,
                                          size_t point_count);
/*
 * ABI v2 compound-part builder. Components are rigid islands of one payload.
 * Holes are retained for topology/diagnostics; the current Jagua backend uses
 * their outer component conservatively until native polygon-hole items exist.
 * Calls must be ordered begin, one or more component outers (with optional
 * holes), then end. A job cannot run while a part is being built.
 */
VacNestingStatus vac_nesting_job_begin_part(VacNestingJob *job, uint64_t part_id);
VacNestingStatus vac_nesting_job_add_part_component_outer(VacNestingJob *job,
                                                          VacNestingPoint const *points,
                                                          size_t point_count);
VacNestingStatus vac_nesting_job_add_part_component_hole(VacNestingJob *job, size_t component_index,
                                                         VacNestingPoint const *points, size_t point_count);
VacNestingStatus vac_nesting_job_end_part(VacNestingJob *job);
VacNestingStatus vac_nesting_job_run(VacNestingJob *job, VacNestingProgressCallback callback, void *user_data);
/* Additive ABI-3 capability. Consumes a configured job, validates external
 * transforms (input order) against the actual collision geometry and options.
 * No search or document writes. cancel() remains thread safe. */
#define VAC_NESTING_HAS_CANDIDATE_VALIDATION 1
VacNestingStatus vac_nesting_job_validate_candidate(VacNestingJob *job,
    VacNestingPlacement const *placements, size_t count);
/* Additive ABI-3 capability. Returns centred, verified collision proxies for
 * every part in a configured job. Counts are always written. Null or short
 * arrays return OUT_OF_RANGE without writing arrays. ring_offsets contains
 * out_groups + 1 prefix offsets into ring_points. The job remains configurable. */
#define VAC_NESTING_HAS_COLLISION_PROXIES 1
VacNestingStatus vac_nesting_job_collision_proxies(VacNestingJob *job, double extra,
    size_t *part_group, VacNestingPoint *part_center, size_t part_capacity,
    size_t *ring_offsets, size_t offsets_capacity, VacNestingPoint *ring_points,
    size_t points_capacity, size_t *out_parts, size_t *out_groups, size_t *out_points);
void vac_nesting_job_cancel(VacNestingJob *job);
VacNestingJobState vac_nesting_job_state(VacNestingJob const *job);
/*
 * After a successful run, there is one result per input part in insertion
 * order. A part that did not fit has placed == 0 and a zero transformation.
 */
size_t vac_nesting_job_result_count(VacNestingJob const *job);
VacNestingStatus vac_nesting_job_result_at(VacNestingJob const *job, size_t index, VacNestingPlacement *placement);

/* The returned error pointer remains valid until the next mutating job call. */
char const *vac_nesting_job_error(VacNestingJob const *job);
void vac_nesting_job_free(VacNestingJob *job);

#ifdef __cplusplus
}
#endif

#endif // VACARDS_NESTING_H
