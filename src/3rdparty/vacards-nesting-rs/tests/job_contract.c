// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "vacards_nesting.h"

static void cancel_run(void *user_data, VacNestingProgress const *progress)
{
    unsigned *callback_count = (unsigned *)user_data;
    if (progress && progress->total_count == 1) {
        ++*callback_count;
    }
}

static void cancel_job(void *user_data, VacNestingProgress const *progress)
{
    if (progress && progress->stage == VAC_NESTING_PROGRESS_SOLVING && progress->iteration >= 256) {
        vac_nesting_job_cancel((VacNestingJob *)user_data);
    }
}

int main(void)
{
    _Static_assert(sizeof(VacNestingPoint) == 16, "unexpected point ABI");
    _Static_assert(sizeof(VacNestingOptions) == 64, "unexpected options ABI");
    _Static_assert(sizeof(VacNestingProgress) == 56, "unexpected progress ABI");
    _Static_assert(sizeof(VacNestingPlacement) == 40, "unexpected placement ABI");

    VacNestingOptions options;
    vac_nesting_options_init(&options);
    if (vac_nesting_options_validate(&options) != VAC_NESTING_STATUS_OK) {
        fputs("default options were invalid\n", stderr);
        return 1;
    }
    options.struct_size = 0;
    if (vac_nesting_options_validate(&options) != VAC_NESTING_STATUS_INVALID_ARGUMENT ||
        vac_nesting_job_new(&options) != NULL) {
        fputs("mismatched options layout was accepted\n", stderr);
        return 2;
    }
    vac_nesting_options_init(&options);
    options.api_version += 1;
    if (vac_nesting_options_validate(&options) != VAC_NESTING_STATUS_INVALID_ARGUMENT) {
        fputs("mismatched options version was accepted\n", stderr);
        return 2;
    }
    vac_nesting_options_init(&options);
    options.rotation_step_degrees = 0.0;
    if (vac_nesting_options_validate(&options) != VAC_NESTING_STATUS_INVALID_ARGUMENT) {
        fputs("invalid options were accepted\n", stderr);
        return 2;
    }
    vac_nesting_options_init(&options);

    VacNestingPoint container[] = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    VacNestingPoint part[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    VacNestingJob *job = vac_nesting_job_new(&options);
    if (!job || vac_nesting_job_state(job) != VAC_NESTING_JOB_STATE_CONFIGURING) {
        fputs("job creation failed\n", stderr);
        return 3;
    }
    if (vac_nesting_job_set_container(job, container, 4) != VAC_NESTING_STATUS_OK ||
        vac_nesting_job_add_part(job, 1001, part, 4) != VAC_NESTING_STATUS_OK) {
        fputs(vac_nesting_job_error(job), stderr);
        vac_nesting_job_free(job);
        return 4;
    }
    container[0].x = NAN;
    part[0].x = NAN;

    unsigned callback_count = 0;
    VacNestingStatus status = vac_nesting_job_run(job, cancel_run, &callback_count);
    if (status != VAC_NESTING_STATUS_OK || callback_count == 0 ||
        vac_nesting_job_state(job) != VAC_NESTING_JOB_STATE_COMPLETED || vac_nesting_job_result_count(job) != 1 ||
        vac_nesting_job_error(job)[0] != '\0') {
        fputs("solver contract failed\n", stderr);
        vac_nesting_job_free(job);
        return 5;
    }
    VacNestingPlacement placement;
    if (vac_nesting_job_result_at(job, 0, &placement) != VAC_NESTING_STATUS_OK || placement.part_id != 1001 ||
        placement.placed == 0 || !isfinite(placement.translation_x) || !isfinite(placement.translation_y) ||
        !isfinite(placement.rotation_degrees)) {
        fputs("valid placement was not published\n", stderr);
        vac_nesting_job_free(job);
        return 6;
    }
    if (vac_nesting_job_result_at(job, 1, &placement) != VAC_NESTING_STATUS_OUT_OF_RANGE) {
        fputs("out-of-range result was accepted\n", stderr);
        vac_nesting_job_free(job);
        return 6;
    }
    vac_nesting_job_free(job);

    job = vac_nesting_job_new(NULL);
    if (vac_nesting_job_set_container(job, part, 4) != VAC_NESTING_STATUS_INVALID_ARGUMENT) {
        fputs("mutated source unexpectedly remained valid\n", stderr);
        vac_nesting_job_free(job);
        return 7;
    }
    part[0].x = 0.0;
    VacNestingPoint cancellation_container[] = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    if (vac_nesting_job_set_container(job, cancellation_container, 4) != VAC_NESTING_STATUS_OK ||
        vac_nesting_job_add_part(job, 3, part, 4) != VAC_NESTING_STATUS_OK) {
        fputs("cancellation setup failed\n", stderr);
        vac_nesting_job_free(job);
        return 8;
    }
    if (vac_nesting_job_run(job, cancel_job, job) != VAC_NESTING_STATUS_CANCELLED ||
        vac_nesting_job_state(job) != VAC_NESTING_JOB_STATE_CANCELLED || vac_nesting_job_result_count(job) != 0) {
        fputs("callback cancellation failed\n", stderr);
        vac_nesting_job_free(job);
        return 9;
    }
    vac_nesting_job_free(job);

#ifdef VAC_NESTING_HAS_OBSTACLES
    /* Obstacles: rejected before a container, never moved, and respected. */
    VacNestingPoint obstacle_sheet[] = {{0, 0}, {40, 0}, {40, 12}, {0, 12}};
    VacNestingPoint obstacle[] = {{0, 0}, {25, 0}, {25, 12}, {0, 12}};
    job = vac_nesting_job_new(NULL);
    if (vac_nesting_job_add_obstacle(job, obstacle, 4) != VAC_NESTING_STATUS_INVALID_STATE ||
        vac_nesting_job_add_obstacle(NULL, obstacle, 4) == VAC_NESTING_STATUS_OK) {
        fputs("obstacle before container or on a null job was accepted\n", stderr);
        vac_nesting_job_free(job);
        return 11;
    }
    if (vac_nesting_job_set_container(job, obstacle_sheet, 4) != VAC_NESTING_STATUS_OK ||
        vac_nesting_job_add_obstacle(job, obstacle, 4) != VAC_NESTING_STATUS_OK ||
        vac_nesting_job_add_obstacle(job, obstacle, 2) != VAC_NESTING_STATUS_INVALID_ARGUMENT ||
        vac_nesting_job_add_part(job, 7, part, 4) != VAC_NESTING_STATUS_OK) {
        fputs("obstacle setup failed\n", stderr);
        vac_nesting_job_free(job);
        return 12;
    }
    if (vac_nesting_job_run(job, NULL, NULL) != VAC_NESTING_STATUS_OK || vac_nesting_job_result_count(job) != 1 ||
        vac_nesting_job_result_at(job, 0, &placement) != VAC_NESTING_STATUS_OK || placement.placed == 0 ||
        placement.translation_x < 25.0 - 1.0e-6) {
        fputs("part was not placed beside the obstacle\n", stderr);
        vac_nesting_job_free(job);
        return 13;
    }
    vac_nesting_job_free(job);
#endif

    if (vac_nesting_job_state(NULL) != VAC_NESTING_JOB_STATE_INVALID || vac_nesting_job_result_count(NULL) != 0 ||
        strstr(vac_nesting_job_error(NULL), "null") == NULL) {
        fputs("null handling failed\n", stderr);
        return 10;
    }
    return 0;
}
