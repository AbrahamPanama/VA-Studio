// SPDX-License-Identifier: GPL-2.0-or-later

mod backend;
mod job;
mod rank;
mod solver;

struct ExternalCandidate(Vec<job::VacNestingPlacement>);
impl job::Solver for ExternalCandidate {
    fn solve(&self, input: &job::SolverInput, control: &job::RunControl<'_>)
        -> Result<Vec<job::VacNestingPlacement>, job::SolverError>
    {
        job::validate_solver_results(input, &self.0)?;
        if solver::validate_external_candidate(input, &self.0, control)? {
            Ok(self.0.clone())
        } else {
            Err(job::SolverError::Failed("candidate violates nesting geometry constraints".into()))
        }
    }
}

use jagua_rs::geometry::convex_hull::convex_hull_from_points;
use jagua_rs::geometry::primitives::Point;
use job::*;
use std::ffi::{c_char, c_void};
use std::mem::{align_of, size_of};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;

const JAGUA_REVISION: &[u8] = b"9a19409bd38f3643c3d6d2d7571cddfba548ee17\0";
const NULL_JOB_ERROR: &[u8] = b"nesting job pointer is null\0";

fn ffi_guard<T, F>(fallback: T, operation: F) -> T
where
    F: FnOnce() -> T,
{
    catch_unwind(AssertUnwindSafe(operation)).unwrap_or(fallback)
}

fn dependency_probe() -> u32 {
    let points = vec![
        Point(0.0, 0.0),
        Point(4.0, 0.0),
        Point(4.0, 3.0),
        Point(0.0, 3.0),
        Point(2.0, 1.0),
    ];

    convex_hull_from_points(points).len() as u32
}

fn status_message(status: i32) -> &'static [u8] {
    match status {
        STATUS_OK => b"ok\0",
        STATUS_INVALID_ARGUMENT => b"invalid argument\0",
        STATUS_INVALID_STATE => b"invalid job state\0",
        STATUS_CANCELLED => b"cancelled\0",
        STATUS_SOLVER_UNAVAILABLE => b"solver unavailable\0",
        STATUS_OUT_OF_RANGE => b"result index out of range\0",
        STATUS_PANIC => b"Rust panic contained\0",
        STATUS_INTERNAL_ERROR => b"internal solver error\0",
        _ => b"unknown nesting status\0",
    }
}

unsafe fn job_ref<'a>(job: *const VacNestingJob) -> Option<&'a VacNestingJob> {
    // SAFETY: The public contract requires non-null pointers returned by
    // vac_nesting_job_new and prohibits free() racing with any other call.
    unsafe { job.as_ref() }
}

fn guarded_job_status<F>(job: *mut VacNestingJob, operation: F) -> i32
where
    F: FnOnce(&VacNestingJob) -> i32,
{
    // SAFETY: This helper is only entered through the public job API contract.
    let Some(job_ref) = (unsafe { job_ref(job) }) else {
        return STATUS_INVALID_ARGUMENT;
    };
    match catch_unwind(AssertUnwindSafe(|| operation(job_ref))) {
        Ok(status) => status,
        Err(_) => {
            job_ref.record_panic();
            STATUS_PANIC
        }
    }
}

unsafe fn copy_polygon(
    points: *const VacNestingPoint,
    point_count: usize,
) -> Result<Polygon, &'static str> {
    if point_count < 3 {
        return Err("a polygon needs at least three points");
    }
    if points.is_null() {
        return Err("polygon point pointer is null");
    }
    if !(points as usize).is_multiple_of(align_of::<VacNestingPoint>()) {
        return Err("polygon point pointer is not correctly aligned");
    }
    if point_count > isize::MAX as usize / size_of::<VacNestingPoint>() {
        return Err("polygon point count exceeds addressable memory");
    }

    // SAFETY: Null, alignment and maximum length were validated above. The C
    // caller guarantees that this readable range is valid for this call.
    let source = unsafe { std::slice::from_raw_parts(points, point_count) };
    normalize_polygon(source.to_vec())
}

unsafe fn read_options(
    options: *const VacNestingOptions,
) -> Result<VacNestingOptions, &'static str> {
    if options.is_null() {
        return Err("options pointer is null");
    }
    // SAFETY: The C contract guarantees at least the leading struct_size field.
    let reported_size = unsafe { ptr::read_unaligned(options.cast::<u32>()) };
    if reported_size as usize != size_of::<VacNestingOptions>() {
        return Err("options struct_size does not match this nesting ABI");
    }
    // SAFETY: Matching struct_size establishes readable storage for this ABI's
    // complete options value under the public C contract.
    let options = unsafe { ptr::read_unaligned(options) };
    options.validate().map(|()| options)
}

#[unsafe(no_mangle)]
pub extern "C" fn vac_nesting_api_version() -> u32 {
    ffi_guard(0, || API_VERSION)
}

#[unsafe(no_mangle)]
/// Additive ABI-3 entry point; no structure/layout changes. Consumes a configured
/// job just like run(), but replays caller-owned external poses without optimizing.
/// # Safety
/// Job must be live and placements must reference count readable records.
pub unsafe extern "C" fn vac_nesting_job_validate_candidate(
    job: *mut VacNestingJob, placements: *const VacNestingPlacement, count: usize,
) -> i32 {
    guarded_job_status(job, |job| {
        if count == 0 || count > 2048 || placements.is_null()
            || (placements as usize) % align_of::<VacNestingPlacement>() != 0 {
            return STATUS_INVALID_ARGUMENT;
        }
        // SAFETY: checked null/alignment/bounds; readability is the C contract.
        let records = unsafe { std::slice::from_raw_parts(placements, count) }.to_vec();
        job.run_with_solver(&ExternalCandidate(records), None, ptr::null_mut())
    })
}

#[unsafe(no_mangle)]
/// Additive ABI-3 entry point; no structure/layout changes. Returns one
/// group per identical collision envelope, part indices and centred origins.
/// Required counts are always written; short or null buffers return OUT_OF_RANGE
/// without writing arrays. Offsets have groups + 1 entries and index ring_points.
/// The configured job remains usable for run or validation.
/// # Safety
/// The job must be live; counters must be writable, and non-null arrays must
/// have their stated writable capacities. Calls must not race job mutation.
pub unsafe extern "C" fn vac_nesting_job_collision_proxies(
    job: *mut VacNestingJob,
    extra: f64,
    part_group: *mut usize,
    part_center: *mut VacNestingPoint,
    part_capacity: usize,
    ring_offsets: *mut usize,
    offsets_capacity: usize,
    ring_points: *mut VacNestingPoint,
    points_capacity: usize,
    out_parts: *mut usize,
    out_groups: *mut usize,
    out_points: *mut usize,
) -> i32 {
    guarded_job_status(job, |job| {
        for pointer in [out_parts, out_groups, out_points] {
            if pointer.is_null() || !(pointer as usize).is_multiple_of(align_of::<usize>()) {
                return STATUS_INVALID_ARGUMENT;
            }
        }
        unsafe {
            *out_parts = 0;
            *out_groups = 0;
            *out_points = 0;
        }
        if (!part_group.is_null() && !(part_group as usize).is_multiple_of(align_of::<usize>()))
            || (!part_center.is_null() && !(part_center as usize).is_multiple_of(align_of::<VacNestingPoint>()))
            || (!ring_offsets.is_null() && !(ring_offsets as usize).is_multiple_of(align_of::<usize>()))
            || (!ring_points.is_null() && !(ring_points as usize).is_multiple_of(align_of::<VacNestingPoint>()))
        {
            return STATUS_INVALID_ARGUMENT;
        }
        if !extra.is_finite() || extra <= 0.0 {
            return STATUS_INVALID_ARGUMENT;
        }
        let input = match job.collision_proxy_input() {
            Ok(input) => input,
            Err(status) => return status,
        };
        let control = job.collision_proxy_control(input.parts.len());
        let proxies = match solver::collision_proxies(&input, extra, &control) {
            Ok(proxies) => proxies,
            Err(SolverError::InvalidInput(_)) => return STATUS_INVALID_ARGUMENT,
            Err(SolverError::Cancelled | SolverError::DeadlineReached) => return STATUS_CANCELLED,
            Err(SolverError::Failed(_)) => return STATUS_SOLVER_UNAVAILABLE,
            Err(_) => return STATUS_INTERNAL_ERROR,
        };
        let parts = proxies.part_group.len();
        let groups = proxies.rings.len();
        let Some(offsets) = groups.checked_add(1) else { return STATUS_OUT_OF_RANGE };
        let Some(points) = proxies.rings.iter().try_fold(0usize, |sum, ring| sum.checked_add(ring.len()))
            else { return STATUS_OUT_OF_RANGE };
        unsafe {
            *out_parts = parts;
            *out_groups = groups;
            *out_points = points;
        }
        if part_group.is_null() || part_center.is_null() || ring_offsets.is_null() || ring_points.is_null()
            || part_capacity < parts || offsets_capacity < offsets || points_capacity < points
        {
            return STATUS_OUT_OF_RANGE;
        }
        for (index, &group) in proxies.part_group.iter().enumerate() {
            unsafe { ptr::write(part_group.add(index), group) };
        }
        for (index, &(x, y)) in proxies.part_center.iter().enumerate() {
            unsafe { ptr::write(part_center.add(index), VacNestingPoint { x, y }) };
        }
        let mut offset = 0;
        for (group, ring) in proxies.rings.iter().enumerate() {
            unsafe { ptr::write(ring_offsets.add(group), offset) };
            for &(x, y) in ring {
                unsafe { ptr::write(ring_points.add(offset), VacNestingPoint { x, y }) };
                offset += 1;
            }
        }
        unsafe { ptr::write(ring_offsets.add(groups), offset) };
        STATUS_OK
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn vac_nesting_jagua_revision() -> *const c_char {
    ffi_guard(ptr::null(), || JAGUA_REVISION.as_ptr().cast())
}

#[unsafe(no_mangle)]
pub extern "C" fn vac_nesting_dependency_probe() -> u32 {
    ffi_guard(0, dependency_probe)
}

#[unsafe(no_mangle)]
pub extern "C" fn vac_nesting_run_self_test() -> i32 {
    ffi_guard(-1, || {
        let revision_is_terminated = JAGUA_REVISION.last() == Some(&0);
        let layouts_match = size_of::<VacNestingPoint>() == 16
            && size_of::<VacNestingOptions>() == 64
            && size_of::<VacNestingProgress>() == 56
            && size_of::<VacNestingPlacement>() == 40;
        if API_VERSION == 3 && revision_is_terminated && dependency_probe() == 4 && layouts_match {
            0
        } else {
            1
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn vac_nesting_status_message(status: i32) -> *const c_char {
    ffi_guard(ptr::null(), || status_message(status).as_ptr().cast())
}

#[unsafe(no_mangle)]
/// # Safety
/// `options` must be null or writable for one `VacNestingOptions` value.
pub unsafe extern "C" fn vac_nesting_options_init(options: *mut VacNestingOptions) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !options.is_null() {
            // SAFETY: The C caller supplied writable storage for one options
            // value. write_unaligned also tolerates non-native alignment.
            unsafe { ptr::write_unaligned(options, VacNestingOptions::default()) };
        }
    }));
}

#[unsafe(no_mangle)]
/// # Safety
/// `options` must be null or readable for one `VacNestingOptions` value.
pub unsafe extern "C" fn vac_nesting_options_validate(options: *const VacNestingOptions) -> i32 {
    ffi_guard(STATUS_PANIC, || {
        if options.is_null() {
            return STATUS_INVALID_ARGUMENT;
        }
        // SAFETY: read_options validates the leading size before reading the
        // complete value.
        unsafe { read_options(options) }.map_or(STATUS_INVALID_ARGUMENT, |_| STATUS_OK)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `options` must be null or readable for one `VacNestingOptions` value.
pub unsafe extern "C" fn vac_nesting_job_new(
    options: *const VacNestingOptions,
) -> *mut VacNestingJob {
    ffi_guard(ptr::null_mut(), || {
        let options = if options.is_null() {
            VacNestingOptions::default()
        } else {
            // SAFETY: read_options validates the leading size before reading
            // the complete value.
            let Ok(options) = (unsafe { read_options(options) }) else {
                return ptr::null_mut();
            };
            options
        };
        Box::into_raw(Box::new(VacNestingJob::new(options)))
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live pointer returned by `vac_nesting_job_new`.
/// `points` must reference `point_count` readable points for this call.
pub unsafe extern "C" fn vac_nesting_job_set_container(
    job: *mut VacNestingJob,
    points: *const VacNestingPoint,
    point_count: usize,
) -> i32 {
    guarded_job_status(job, |job| {
        // SAFETY: copy_polygon validates the raw slice representation.
        match unsafe { copy_polygon(points, point_count) } {
            Ok(polygon) => job.set_container(polygon),
            Err(message) => job.record_error(STATUS_INVALID_ARGUMENT, message),
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live pointer returned by `vac_nesting_job_new`.
/// `points` must reference `point_count` readable points for this call.
pub unsafe extern "C" fn vac_nesting_job_add_container_hole(
    job: *mut VacNestingJob,
    points: *const VacNestingPoint,
    point_count: usize,
) -> i32 {
    guarded_job_status(job, |job| {
        // SAFETY: copy_polygon validates the raw slice representation.
        match unsafe { copy_polygon(points, point_count) } {
            Ok(polygon) => job.add_hole(polygon),
            Err(message) => job.record_error(STATUS_INVALID_ARGUMENT, message),
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live pointer returned by `vac_nesting_job_new`.
/// `points` must reference `point_count` readable points for this call.
pub unsafe extern "C" fn vac_nesting_job_add_obstacle(
    job: *mut VacNestingJob,
    points: *const VacNestingPoint,
    point_count: usize,
) -> i32 {
    guarded_job_status(job, |job| {
        // SAFETY: copy_polygon validates the raw slice representation.
        match unsafe { copy_polygon(points, point_count) } {
            Ok(polygon) => job.add_obstacle(polygon),
            Err(message) => job.record_error(STATUS_INVALID_ARGUMENT, message),
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live pointer returned by `vac_nesting_job_new`.
/// `points` must reference `point_count` readable points for this call.
pub unsafe extern "C" fn vac_nesting_job_add_part(
    job: *mut VacNestingJob,
    part_id: u64,
    points: *const VacNestingPoint,
    point_count: usize,
) -> i32 {
    guarded_job_status(job, |job| {
        // SAFETY: copy_polygon validates the raw slice representation.
        match unsafe { copy_polygon(points, point_count) } {
            Ok(polygon) => job.add_part(part_id, polygon),
            Err(message) => job.record_error(STATUS_INVALID_ARGUMENT, message),
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live pointer returned by `vac_nesting_job_new`.
pub unsafe extern "C" fn vac_nesting_job_begin_part(job: *mut VacNestingJob, part_id: u64) -> i32 {
    guarded_job_status(job, |job| job.begin_part(part_id))
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be live and `points` must reference `point_count` readable
/// points for this call.
pub unsafe extern "C" fn vac_nesting_job_add_part_component_outer(
    job: *mut VacNestingJob,
    points: *const VacNestingPoint,
    point_count: usize,
) -> i32 {
    guarded_job_status(job, |job| {
        // SAFETY: copy_polygon validates the raw slice representation.
        match unsafe { copy_polygon(points, point_count) } {
            Ok(polygon) => job.add_part_component_outer(polygon),
            Err(message) => job.record_error(STATUS_INVALID_ARGUMENT, message),
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be live and `points` must reference `point_count` readable
/// points for this call.
pub unsafe extern "C" fn vac_nesting_job_add_part_component_hole(
    job: *mut VacNestingJob,
    component_index: usize,
    points: *const VacNestingPoint,
    point_count: usize,
) -> i32 {
    guarded_job_status(job, |job| {
        // SAFETY: copy_polygon validates the raw slice representation.
        match unsafe { copy_polygon(points, point_count) } {
            Ok(polygon) => job.add_part_component_hole(component_index, polygon),
            Err(message) => job.record_error(STATUS_INVALID_ARGUMENT, message),
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live pointer returned by `vac_nesting_job_new`.
pub unsafe extern "C" fn vac_nesting_job_end_part(job: *mut VacNestingJob) -> i32 {
    guarded_job_status(job, VacNestingJob::end_part)
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or live for the entire synchronous call. Callback data
/// must remain valid until this function returns and must not free the job.
pub unsafe extern "C" fn vac_nesting_job_run(
    job: *mut VacNestingJob,
    callback: ProgressCallback,
    user_data: *mut c_void,
) -> i32 {
    guarded_job_status(job, |job| job.run(callback, user_data))
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live pointer returned by `vac_nesting_job_new`.
pub unsafe extern "C" fn vac_nesting_job_cancel(job: *mut VacNestingJob) {
    // SAFETY: This helper is only entered through the public job API contract.
    let Some(job) = (unsafe { job_ref(job) }) else {
        return;
    };
    if catch_unwind(AssertUnwindSafe(|| job.cancel())).is_err() {
        job.record_panic();
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live pointer returned by `vac_nesting_job_new`.
pub unsafe extern "C" fn vac_nesting_job_state(job: *const VacNestingJob) -> i32 {
    // SAFETY: This helper is only entered through the public job API contract.
    let Some(job) = (unsafe { job_ref(job) }) else {
        return STATE_INVALID;
    };
    ffi_guard(STATE_FAILED, || job.state())
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live pointer returned by `vac_nesting_job_new`.
pub unsafe extern "C" fn vac_nesting_job_result_count(job: *const VacNestingJob) -> usize {
    // SAFETY: This helper is only entered through the public job API contract.
    let Some(job) = (unsafe { job_ref(job) }) else {
        return 0;
    };
    ffi_guard(0, || job.result_count())
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or live. `placement` must be null or writable for one
/// `VacNestingPlacement` value.
pub unsafe extern "C" fn vac_nesting_job_result_at(
    job: *const VacNestingJob,
    index: usize,
    placement: *mut VacNestingPlacement,
) -> i32 {
    // SAFETY: This helper is only entered through the public job API contract.
    let Some(job) = (unsafe { job_ref(job) }) else {
        return STATUS_INVALID_ARGUMENT;
    };
    if placement.is_null() {
        return job.record_error(STATUS_INVALID_ARGUMENT, "placement pointer is null");
    }
    match catch_unwind(AssertUnwindSafe(|| job.result_at(index))) {
        Ok(Ok(result)) => {
            // SAFETY: The C caller supplied writable storage for one result.
            unsafe { ptr::write_unaligned(placement, result) };
            STATUS_OK
        }
        Ok(Err(status)) => job.record_error(status, "result index is out of range"),
        Err(_) => {
            job.record_panic();
            STATUS_PANIC
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or live. The returned pointer must not be retained after
/// the next mutating job call.
pub unsafe extern "C" fn vac_nesting_job_error(job: *const VacNestingJob) -> *const c_char {
    // SAFETY: This helper is only entered through the public job API contract.
    let Some(job) = (unsafe { job_ref(job) }) else {
        return NULL_JOB_ERROR.as_ptr().cast();
    };
    ffi_guard(NULL_JOB_ERROR.as_ptr().cast(), || job.error_ptr())
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or an owned pointer returned by `vac_nesting_job_new`.
/// It must be freed exactly once and only after every concurrent call returns.
pub unsafe extern "C" fn vac_nesting_job_free(job: *mut VacNestingJob) {
    if job.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Ownership was returned by vac_nesting_job_new, and the API
        // contract requires exactly one free after all concurrent calls finish.
        unsafe { drop(Box::from_raw(job)) };
    }));
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    #[test]
    fn dependency_probe_executes_pinned_jagua_geometry() {
        assert_eq!(vac_nesting_dependency_probe(), 4);
    }

    #[test]
    fn public_contract_reports_expected_identity_and_layout() {
        assert_eq!(vac_nesting_api_version(), API_VERSION);
        assert_eq!(vac_nesting_run_self_test(), 0);
        assert_eq!(size_of::<VacNestingPoint>(), 16);
        assert_eq!(size_of::<VacNestingOptions>(), 64);
        assert_eq!(size_of::<VacNestingProgress>(), 56);
        assert_eq!(size_of::<VacNestingPlacement>(), 40);

        let revision = unsafe { CStr::from_ptr(vac_nesting_jagua_revision()) };
        assert_eq!(
            revision.to_bytes(),
            &JAGUA_REVISION[..JAGUA_REVISION.len() - 1]
        );
    }

    #[test]
    fn ffi_copies_input_and_publishes_solver_results() {
        // SAFETY: Every pointer in this test is created by the API or backed by
        // live local arrays for the complete duration of each call.
        unsafe {
            let job = vac_nesting_job_new(ptr::null());
            assert!(!job.is_null());
            let mut container = [
                VacNestingPoint { x: 0.0, y: 0.0 },
                VacNestingPoint { x: 100.0, y: 0.0 },
                VacNestingPoint { x: 100.0, y: 100.0 },
                VacNestingPoint { x: 0.0, y: 100.0 },
            ];
            let mut part = [
                VacNestingPoint { x: 0.0, y: 0.0 },
                VacNestingPoint { x: 10.0, y: 0.0 },
                VacNestingPoint { x: 10.0, y: 10.0 },
                VacNestingPoint { x: 0.0, y: 10.0 },
            ];
            assert_eq!(
                vac_nesting_job_set_container(job, container.as_ptr(), container.len()),
                STATUS_OK
            );
            assert_eq!(
                vac_nesting_job_add_part(job, 42, part.as_ptr(), part.len()),
                STATUS_OK
            );
            container.fill(VacNestingPoint {
                x: f64::NAN,
                y: f64::NAN,
            });
            part.fill(VacNestingPoint {
                x: f64::NAN,
                y: f64::NAN,
            });
            assert_eq!(vac_nesting_job_run(job, None, ptr::null_mut()), STATUS_OK);
            assert_eq!(vac_nesting_job_state(job), STATE_COMPLETED);
            assert_eq!(vac_nesting_job_result_count(job), 1);
            let mut placement = VacNestingPlacement::default();
            assert_eq!(
                vac_nesting_job_result_at(job, 0, &raw mut placement),
                STATUS_OK
            );
            assert_eq!(placement.part_id, 42);
            assert_eq!(placement.placed, 1);
            let error = CStr::from_ptr(vac_nesting_job_error(job));
            assert!(error.to_bytes().is_empty());
            vac_nesting_job_free(job);
        }
    }

    unsafe extern "C" fn cancel_from_callback(
        user_data: *mut c_void,
        progress: *const VacNestingProgress,
    ) {
        // SAFETY: progress and user_data remain live for the synchronous run.
        if let Some(progress) = unsafe { progress.as_ref() }
            && progress.stage == PROGRESS_SOLVING
            // Draft evaluates 512 candidates per part in one constructive
            // pass. Crossing 512 proves that a zero deadline started another
            // refinement pass instead of retaining the old one-pass behavior.
            && progress.iteration >= 600
        {
            unsafe { vac_nesting_job_cancel(user_data.cast()) };
        }
    }

    #[test]
    fn callback_can_cancel_synchronous_run_without_deadlock() {
        // SAFETY: Every pointer in this test is created by the API or backed by
        // a live local array for the complete duration of each call.
        unsafe {
            let mut options = VacNestingOptions::default();
            options.time_limit_ms = 0;
            let job = vac_nesting_job_new(&raw const options);
            let container = [
                VacNestingPoint { x: 0.0, y: 0.0 },
                VacNestingPoint { x: 100.0, y: 0.0 },
                VacNestingPoint { x: 100.0, y: 100.0 },
                VacNestingPoint { x: 0.0, y: 100.0 },
            ];
            let part = [
                VacNestingPoint { x: 0.0, y: 0.0 },
                VacNestingPoint { x: 10.0, y: 0.0 },
                VacNestingPoint { x: 10.0, y: 10.0 },
                VacNestingPoint { x: 0.0, y: 10.0 },
            ];
            assert_eq!(
                vac_nesting_job_set_container(job, container.as_ptr(), container.len()),
                STATUS_OK
            );
            assert_eq!(
                vac_nesting_job_add_part(job, 1, part.as_ptr(), part.len()),
                STATUS_OK
            );
            assert_eq!(
                vac_nesting_job_run(job, Some(cancel_from_callback), job.cast()),
                STATUS_CANCELLED
            );
            assert_eq!(vac_nesting_job_state(job), STATE_CANCELLED);
            assert_eq!(vac_nesting_job_result_count(job), 0);
            vac_nesting_job_free(job);
        }
    }

    #[test]
    fn nulls_and_bounds_are_fail_closed() {
        // SAFETY: Null is an explicitly supported sentinel for these queries.
        unsafe {
            assert_eq!(vac_nesting_job_state(ptr::null()), STATE_INVALID);
            assert_eq!(vac_nesting_job_result_count(ptr::null()), 0);
            assert_eq!(
                vac_nesting_job_result_at(ptr::null(), 0, ptr::null_mut()),
                STATUS_INVALID_ARGUMENT
            );
            let null_error = CStr::from_ptr(vac_nesting_job_error(ptr::null()));
            assert!(null_error.to_string_lossy().contains("null"));
        }
    }

    #[test]
    fn panic_guard_marks_the_job_failed_without_unwinding_across_ffi() {
        // SAFETY: The job pointer remains live until the final free call.
        unsafe {
            let job = vac_nesting_job_new(ptr::null());
            let status = guarded_job_status(job, |_| panic!("controlled bridge panic"));
            assert_eq!(status, STATUS_PANIC);
            assert_eq!(vac_nesting_job_state(job), STATE_FAILED);
            let error = CStr::from_ptr(vac_nesting_job_error(job));
            assert!(error.to_string_lossy().contains("panic"));
            vac_nesting_job_free(job);
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or a live job; configuration must not race with run.
pub unsafe extern "C" fn vac_nesting_job_set_work_limit(job: *mut VacNestingJob, limit: u64) -> i32 {
    guarded_job_status(job, |job| job.set_work_limit(limit))
}

#[unsafe(no_mangle)]
/// # Safety
/// `job` must be null or live; `terminal` must be null or writable.
/// On failure the output is untouched. Failed jobs have no successful terminal result.
pub unsafe extern "C" fn vac_nesting_job_get_terminal(
    job: *const VacNestingJob, terminal: *mut VacNestingTerminal,
) -> i32 {
    let Some(job) = (unsafe { job_ref(job) }) else { return STATUS_INVALID_ARGUMENT; };
    if terminal.is_null() { return STATUS_INVALID_ARGUMENT; }
    ffi_guard(STATUS_PANIC, || match job.terminal() {
        Ok(value) => { unsafe { terminal.write(value); } STATUS_OK }
        Err(status) => status,
    })
}
