// SPDX-License-Identifier: GPL-2.0-or-later

use crate::solver::PortfolioSolver;
use std::cell::{Cell, RefCell};
use std::collections::HashSet;
use std::ffi::{CString, c_void};
#[cfg(test)]
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Condvar, Mutex, MutexGuard};
use std::thread::ThreadId;
use std::time::{Duration, Instant};
use std::{panic::AssertUnwindSafe, panic::catch_unwind};

pub const STATUS_OK: i32 = 0;
pub const STATUS_INVALID_ARGUMENT: i32 = 1;
pub const STATUS_INVALID_STATE: i32 = 2;
pub const STATUS_CANCELLED: i32 = 3;
pub const STATUS_SOLVER_UNAVAILABLE: i32 = 4;
pub const STATUS_OUT_OF_RANGE: i32 = 5;
pub const STATUS_PANIC: i32 = 6;
pub const STATUS_INTERNAL_ERROR: i32 = 7;

pub const API_VERSION: u32 = 3;

pub const STOP_COMPLETED: i32 = 0;
pub const STOP_WORK_LIMIT: i32 = 1;
pub const STOP_CANCELLED: i32 = 2;
pub const STOP_TIME_LIMIT: i32 = 3;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct VacNestingTerminal {
    pub stop_reason: i32,
    pub reserved: u32,
    pub completed_work: u64,
}

pub const STATE_INVALID: i32 = -1;
pub const STATE_CONFIGURING: i32 = 0;
pub const STATE_RUNNING: i32 = 1;
pub const STATE_COMPLETED: i32 = 2;
pub const STATE_CANCELLED: i32 = 3;
pub const STATE_FAILED: i32 = 4;

pub const ROTATION_NONE: i32 = 0;
pub const ROTATION_RIGHT_ANGLES: i32 = 1;
pub const ROTATION_DISCRETE: i32 = 2;
pub const ROTATION_FREE: i32 = 3;

pub const QUALITY_DRAFT: i32 = 0;
pub const QUALITY_BALANCED: i32 = 1;
pub const QUALITY_HIGH: i32 = 2;

pub const PROGRESS_VALIDATING: u32 = 0;
pub const PROGRESS_SOLVING: u32 = 1;
pub const PROGRESS_FINALIZING: u32 = 2;

// The engine API has a deliberately short finite default so callers that use
// null/default options cannot accidentally start an unbounded synchronous run.
// An explicit zero remains the public no-deadline value used by the Unlimited
// application preference.
pub const DEFAULT_ENGINE_TIME_LIMIT_MS: u64 = 1_000;

// Jagua performs several squared-distance calculations in f32. Keeping both
// coordinates and offsets below this bound leaves ample headroom for opposite
// extremes, inflation, and intermediate transforms without overflowing f32.
pub(crate) const MAX_ENGINE_MAGNITUDE: f64 = 1.0e18;

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct VacNestingPoint {
    pub x: f64,
    pub y: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct VacNestingOptions {
    pub struct_size: u32,
    pub api_version: u32,
    pub part_spacing: f64,
    pub container_margin: f64,
    pub rotation_step_degrees: f64,
    pub random_seed: u64,
    pub time_limit_ms: u64,
    pub rotation_mode: i32,
    pub quality: i32,
    pub worker_count: u32,
    pub reserved: u32,
}

impl Default for VacNestingOptions {
    fn default() -> Self {
        Self {
            struct_size: std::mem::size_of::<Self>() as u32,
            api_version: API_VERSION,
            part_spacing: 0.0,
            container_margin: 0.0,
            rotation_step_degrees: 15.0,
            random_seed: 0,
            time_limit_ms: DEFAULT_ENGINE_TIME_LIMIT_MS,
            rotation_mode: ROTATION_FREE,
            quality: QUALITY_BALANCED,
            worker_count: 0,
            reserved: 0,
        }
    }
}

impl VacNestingOptions {
    pub fn validate(&self) -> Result<(), &'static str> {
        if self.struct_size as usize != std::mem::size_of::<Self>() {
            return Err("options struct_size does not match this nesting ABI");
        }
        if self.api_version != API_VERSION {
            return Err("options api_version does not match this nesting ABI");
        }
        if !self.part_spacing.is_finite() || self.part_spacing < 0.0 {
            return Err("part_spacing must be finite and non-negative");
        }
        if !self.container_margin.is_finite() || self.container_margin < 0.0 {
            return Err("container_margin must be finite and non-negative");
        }
        if self.part_spacing > MAX_ENGINE_MAGNITUDE
            || self.container_margin > MAX_ENGINE_MAGNITUDE
            || self.container_margin + self.part_spacing / 2.0 > MAX_ENGINE_MAGNITUDE
        {
            return Err("spacing and margin exceed the nesting engine's numeric range");
        }
        if !self.rotation_step_degrees.is_finite()
            || self.rotation_step_degrees <= 0.0
            || self.rotation_step_degrees > 360.0
        {
            return Err("rotation_step_degrees must be in (0, 360]");
        }
        if !matches!(
            self.rotation_mode,
            ROTATION_NONE | ROTATION_RIGHT_ANGLES | ROTATION_DISCRETE | ROTATION_FREE
        ) {
            return Err("rotation_mode is not recognized");
        }
        if self.rotation_mode == ROTATION_DISCRETE
            && (360.0 / self.rotation_step_degrees).ceil() > 3600.0
        {
            return Err("rotation_step_degrees creates more than 3600 orientations");
        }
        if !(QUALITY_DRAFT..=QUALITY_HIGH).contains(&self.quality) {
            return Err("quality is not recognized");
        }
        if self.worker_count > 1024 {
            return Err("worker_count exceeds the supported limit");
        }
        if self.reserved != 0 {
            return Err("reserved options fields must be zero");
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct VacNestingProgress {
    pub iteration: u64,
    pub elapsed_seconds: f64,
    pub best_score: f64,
    pub placed_count: u32,
    pub total_count: u32,
    pub stage: u32,
    pub reserved: u32,
    pub placements: *const VacNestingPlacement,
    pub placement_count: u64,
}

impl Default for VacNestingProgress {
    fn default() -> Self {
        Self {
            iteration: 0,
            elapsed_seconds: 0.0,
            best_score: 0.0,
            placed_count: 0,
            total_count: 0,
            stage: 0,
            reserved: 0,
            placements: std::ptr::null(),
            placement_count: 0,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct VacNestingPlacement {
    pub part_id: u64,
    pub translation_x: f64,
    pub translation_y: f64,
    pub rotation_degrees: f64,
    pub placed: u8,
    pub reserved: [u8; 7],
}

pub type ProgressCallback = Option<unsafe extern "C" fn(*mut c_void, *const VacNestingProgress)>;

#[derive(Clone, Debug)]
pub(crate) struct Polygon(pub(crate) Vec<VacNestingPoint>);

#[derive(Clone, Debug)]
pub(crate) struct Part {
    pub(crate) id: u64,
    pub(crate) components: Vec<PartComponent>,
}

#[derive(Clone, Debug)]
pub(crate) struct PartComponent {
    pub(crate) outer: Polygon,
    pub(crate) holes: Vec<Polygon>,
}

#[derive(Clone, Debug)]
struct PartBuilder {
    id: u64,
    components: Vec<PartComponent>,
}

#[derive(Clone)]
pub(crate) struct SolverInput {
    pub(crate) options: VacNestingOptions,
    pub(crate) container: Polygon,
    pub(crate) holes: Vec<Polygon>,
    /// Fixed artwork already on the sheet. Parts keep `part_spacing` from
    /// every obstacle (not the container margin) and obstacles never move.
    pub(crate) obstacles: Vec<Polygon>,
    pub(crate) parts: Vec<Part>,
}

#[derive(Default)]
struct LifecycleState {
    cancel_requested: bool,
    run_active: bool,
    terminal: bool,
    terminal_status: i32,
    callbacks_in_flight: usize,
    callback_thread: Option<ThreadId>,
}

#[derive(Default)]
struct LifecycleGate {
    state: Mutex<LifecycleState>,
    callbacks_drained: Condvar,
    #[cfg(test)]
    foreign_cancel_waiters: AtomicUsize,
}

impl LifecycleGate {
    fn lock(&self) -> MutexGuard<'_, LifecycleState> {
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn begin_callback(&self) -> Option<CallbackPermit<'_>> {
        let mut state = self.lock();
        if state.cancel_requested || state.terminal {
            return None;
        }
        // Solver progress is coordinator-only, so at most one callback can be
        // live. Keeping the thread identity makes cancellation from inside the
        // callback non-blocking while foreign cancellation still waits for the
        // callback to drain.
        debug_assert_eq!(state.callbacks_in_flight, 0);
        state.callbacks_in_flight = state.callbacks_in_flight.saturating_add(1);
        state.callback_thread = Some(std::thread::current().id());
        Some(CallbackPermit { gate: self })
    }

    fn drain_foreign_callbacks<'a>(
        &'a self,
        mut state: MutexGuard<'a, LifecycleState>,
        caller_is_callback: bool,
    ) -> MutexGuard<'a, LifecycleState> {
        #[cfg(test)]
        let is_waiting = state.callbacks_in_flight != 0 && !caller_is_callback;
        #[cfg(test)]
        if is_waiting {
            self.foreign_cancel_waiters.fetch_add(1, Ordering::AcqRel);
        }
        while state.callbacks_in_flight != 0 && !caller_is_callback {
            state = self
                .callbacks_drained
                .wait(state)
                .unwrap_or_else(|poisoned| poisoned.into_inner());
        }
        #[cfg(test)]
        if is_waiting {
            self.foreign_cancel_waiters.fetch_sub(1, Ordering::AcqRel);
        }
        state
    }
}

struct CallbackPermit<'a> {
    gate: &'a LifecycleGate,
}

impl Drop for CallbackPermit<'_> {
    fn drop(&mut self) {
        let mut state = self.gate.lock();
        state.callbacks_in_flight = state.callbacks_in_flight.saturating_sub(1);
        if state.callbacks_in_flight == 0 {
            state.callback_thread = None;
            self.gate.callbacks_drained.notify_all();
        }
    }
}

pub(crate) struct RunControl<'a> {
    cancelled: &'a AtomicBool,
    local_stop: Option<&'a AtomicBool>,
    lifecycle: Option<&'a LifecycleGate>,
    callback: ProgressCallback,
    user_data: *mut c_void,
    started: Instant,
    total_count: u32,
    best_iteration: Cell<u64>,
    best_score: Cell<f64>,
    best_placed_count: Cell<u32>,
    best_placements: RefCell<Vec<VacNestingPlacement>>,
    deadline_suspended: Cell<bool>,
    work_limit: u64,
    completed_work: Cell<u64>,
    limit_reason: Cell<i32>,
}

/// The only state a scoped solver lane may inherit from the callback-owning
/// coordinator. This type deliberately cannot carry a callback or user-data
/// pointer across the thread boundary.
#[derive(Clone, Copy)]
pub(crate) struct SharedRunClock<'a> {
    cancelled: &'a AtomicBool,
    started: Instant,
    total_count: u32,
}

impl<'a> SharedRunClock<'a> {
    pub(crate) fn started_at(self) -> Instant {
        self.started
    }

    pub(crate) fn deadline(self, time_limit_ms: u64) -> Option<Instant> {
        (time_limit_ms != 0)
            .then(|| {
                self.started
                    .checked_add(Duration::from_millis(time_limit_ms))
            })
            .flatten()
    }

    /// Build the worker-local control on the worker thread itself. The null
    /// callback state is not transported from the coordinator.
    pub(crate) fn silent_control(self, local_stop: &'a AtomicBool) -> RunControl<'a> {
        RunControl {
            cancelled: self.cancelled,
            local_stop: Some(local_stop),
            lifecycle: None,
            callback: None,
            user_data: std::ptr::null_mut(),
            started: self.started,
            total_count: self.total_count,
            best_iteration: Cell::new(0),
            best_score: Cell::new(0.0),
            best_placed_count: Cell::new(0),
            best_placements: RefCell::new(Vec::new()),
            deadline_suspended: Cell::new(false),
            work_limit: 0,
            completed_work: Cell::new(0),
            limit_reason: Cell::new(STOP_COMPLETED),
        }
    }
}

impl RunControl<'_> {
    pub(crate) fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }

    pub(crate) fn local_stop_requested(&self) -> bool {
        self.local_stop
            .is_some_and(|stop| stop.load(Ordering::Acquire))
    }

    pub(crate) fn stop_requested(&self) -> bool {
        self.is_cancelled() || self.local_stop_requested()
    }

    pub(crate) fn stop_error(&self) -> Option<SolverError> {
        if self.is_cancelled() {
            Some(SolverError::Cancelled)
        } else if self.local_stop_requested() {
            Some(SolverError::DeadlineReached)
        } else {
            None
        }
    }

    fn emit(&self, mut progress: VacNestingProgress, placements: &[VacNestingPlacement]) -> bool {
        // A cancellation observed before publication is terminal for progress
        // too. The second check narrows the race around progress construction;
        // one foreign callback already executing when another thread cancels
        // remains the only unavoidable callback-granularity latency.
        if self.is_cancelled() {
            return false;
        }
        progress.elapsed_seconds = self.started.elapsed().as_secs_f64();
        progress.total_count = self.total_count;
        progress.placements = placements.as_ptr();
        progress.placement_count = placements.len().min(u64::MAX as usize) as u64;
        if self.is_cancelled() {
            return false;
        }
        if let Some(callback) = self.callback {
            let _permit = if let Some(lifecycle) = self.lifecycle {
                let Some(permit) = lifecycle.begin_callback() else {
                    return false;
                };
                Some(permit)
            } else {
                None
            };
            if self.is_cancelled() {
                return false;
            }
            // SAFETY: The callback and user data are supplied by the caller and
            // are valid for the duration of this synchronous run call.
            unsafe { callback(self.user_data, &raw const progress) };
        }
        true
    }

    pub(crate) fn report(&self, progress: VacNestingProgress) -> bool {
        self.emit(progress, &[])
    }

    pub(crate) fn report_best(
        &self,
        progress: VacNestingProgress,
        placements: &[VacNestingPlacement],
    ) -> bool {
        if self.is_cancelled() {
            return false;
        }
        self.best_iteration.set(progress.iteration);
        self.best_score.set(progress.best_score);
        self.best_placed_count.set(progress.placed_count);
        if !placements.is_empty() {
            self.best_placements.replace(placements.to_vec());
        }
        self.emit(progress, placements)
    }

    pub(crate) fn report_heartbeat(&self, iteration: u64) -> bool {
        self.emit(
            VacNestingProgress {
                iteration: iteration.max(self.best_iteration.get()),
                best_score: self.best_score.get(),
                placed_count: self.best_placed_count.get(),
                stage: PROGRESS_SOLVING,
                ..VacNestingProgress::default()
            },
            &[],
        )
    }

    /// Adopt an already validated portfolio winner without publishing an
    /// intermediate callback. `VacNestingJob` remains the sole owner of the
    /// finalizing callback, which will then report this winner's score and
    /// complete placement vector rather than stale baseline state.
    pub(crate) fn adopt_final_incumbent(
        &self,
        best_score: f64,
        placements: &[VacNestingPlacement],
    ) -> bool {
        if self.is_cancelled() {
            return false;
        }
        self.best_score.set(best_score);
        self.best_placed_count.set(
            placements
                .iter()
                .filter(|placement| placement.placed != 0)
                .count()
                .min(u32::MAX as usize) as u32,
        );
        self.best_placements.replace(placements.to_vec());
        !self.is_cancelled()
    }

    // Called at each increment of the existing solver iteration counter,
    // including increments between heartbeat publications. Preparation is free.
    pub(crate) fn record_work(&self, iteration: u64) {
        self.completed_work.set(iteration);
    }

    pub(crate) fn time_limit_reached(&self, time_limit_ms: u64) -> bool {
        // A work boundary is never suspended by the first-layout time exception.
        if self.work_limit != 0 && self.completed_work.get() >= self.work_limit {
            self.limit_reason.set(STOP_WORK_LIMIT);
            return true;
        }
        if !self.deadline_suspended.get() && time_limit_ms != 0
            && self.started.elapsed() >= Duration::from_millis(time_limit_ms) {
            self.limit_reason.set(STOP_TIME_LIMIT);
            return true;
        }
        false
    }

    /// Runs `operation` with the wall-clock limit ignored. Cancellation and
    /// lane-local stops still apply. Used only for the first constructive
    /// pass, so an expired budget cannot turn into an empty layout.
    pub(crate) fn with_deadline_suspended<T>(&self, operation: impl FnOnce() -> T) -> T {
        let previous = self.deadline_suspended.replace(true);
        let result = operation();
        self.deadline_suspended.set(previous);
        result
    }

    pub(crate) fn best_iteration(&self) -> u64 {
        self.best_iteration.get()
    }

    pub(crate) fn shared_clock(&self) -> SharedRunClock<'_> {
        SharedRunClock {
            cancelled: self.cancelled,
            started: self.started,
            total_count: self.total_count,
        }
    }

    pub(crate) fn started_at(&self) -> Instant {
        self.started
    }

    pub(crate) fn deadline(&self, time_limit_ms: u64) -> Option<Instant> {
        self.shared_clock().deadline(time_limit_ms)
    }

    fn recoverable_incumbent(&self) -> Vec<VacNestingPlacement> {
        self.best_placements.borrow().clone()
    }
}

#[cfg(test)]
impl<'a> RunControl<'a> {
    pub(crate) fn for_tests(cancelled: &'a AtomicBool, total_count: usize) -> Self {
        Self::for_tests_with_callback(cancelled, total_count, None, std::ptr::null_mut())
    }

    pub(crate) fn for_tests_with_callback(
        cancelled: &'a AtomicBool,
        total_count: usize,
        callback: ProgressCallback,
        user_data: *mut c_void,
    ) -> Self {
        Self {
            cancelled,
            local_stop: None,
            lifecycle: None,
            callback,
            user_data,
            started: Instant::now(),
            total_count: total_count.min(u32::MAX as usize) as u32,
            best_iteration: Cell::new(0),
            best_score: Cell::new(0.0),
            best_placed_count: Cell::new(0),
            best_placements: RefCell::new(Vec::new()),
            deadline_suspended: Cell::new(false),
            work_limit: 0,
            completed_work: Cell::new(0),
            limit_reason: Cell::new(STOP_COMPLETED),
        }
    }

    pub(crate) fn for_tests_after_elapsed(
        cancelled: &'a AtomicBool,
        total_count: usize,
        elapsed: Duration,
    ) -> Self {
        let mut control = Self::for_tests(cancelled, total_count);
        control.started = control
            .started
            .checked_sub(elapsed)
            .unwrap_or_else(Instant::now);
        control
    }
}

#[derive(Debug)]
pub(crate) enum SolverError {
    Cancelled,
    DeadlineReached,
    Panicked,
    InvalidInput(String),
    Failed(String),
}

pub(crate) trait Solver {
    fn solve(
        &self,
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError>;

    /// Opt in only when every non-empty placement snapshot supplied to
    /// `report_best` is complete and geometry-safe. The job may retain the
    /// latest structurally valid snapshot if this solver later unwinds.
    fn reported_incumbents_are_recoverable(&self) -> bool {
        false
    }
}

pub(crate) fn validate_solver_results(
    input: &SolverInput,
    results: &[VacNestingPlacement],
) -> Result<(), SolverError> {
    if results.len() != input.parts.len() {
        return Err(SolverError::Failed(
            "nesting solver returned the wrong number of results".to_owned(),
        ));
    }

    for (part, placement) in input.parts.iter().zip(results) {
        if placement.part_id != part.id {
            return Err(SolverError::Failed(
                "nesting solver returned results out of input order".to_owned(),
            ));
        }
        if placement.placed > 1 {
            return Err(SolverError::Failed(
                "nesting solver returned an invalid placed flag".to_owned(),
            ));
        }
        if !placement.translation_x.is_finite()
            || !placement.translation_y.is_finite()
            || !placement.rotation_degrees.is_finite()
        {
            return Err(SolverError::Failed(
                "nesting solver returned a non-finite transformation".to_owned(),
            ));
        }
        if placement.reserved != [0; 7] {
            return Err(SolverError::Failed(
                "nesting solver returned non-zero reserved fields".to_owned(),
            ));
        }
        if placement.placed == 0
            && (placement.translation_x != 0.0
                || placement.translation_y != 0.0
                || placement.rotation_degrees != 0.0)
        {
            return Err(SolverError::Failed(
                "an unplaced nesting result contained a transformation".to_owned(),
            ));
        }
    }

    Ok(())
}

struct JobData {
    work_limit: u64,
    terminal: Option<VacNestingTerminal>,
    options: VacNestingOptions,
    state: i32,
    container: Option<Polygon>,
    holes: Vec<Polygon>,
    obstacles: Vec<Polygon>,
    parts: Vec<Part>,
    part_ids: HashSet<u64>,
    building_part: Option<PartBuilder>,
    results: Vec<VacNestingPlacement>,
    error: CString,
}

#[repr(C)]
pub struct VacNestingJob {
    cancelled: AtomicBool,
    lifecycle: LifecycleGate,
    data: Mutex<JobData>,
}

impl VacNestingJob {
    pub(crate) fn new(options: VacNestingOptions) -> Self {
        Self {
            cancelled: AtomicBool::new(false),
            lifecycle: LifecycleGate::default(),
            data: Mutex::new(JobData {
                work_limit: 0,
                terminal: None,
                options,
                state: STATE_CONFIGURING,
                container: None,
                holes: Vec::new(),
                obstacles: Vec::new(),
                parts: Vec::new(),
                part_ids: HashSet::new(),
                building_part: None,
                results: Vec::new(),
                error: empty_c_string(),
            }),
        }
    }

    fn lock(&self) -> MutexGuard<'_, JobData> {
        self.data
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    pub(crate) fn set_work_limit(&self, limit: u64) -> i32 {
        let mut data = self.lock();
        if data.state != STATE_CONFIGURING { return STATUS_INVALID_STATE; }
        // Fixed work has a single native lane. Reject ambiguous clock/lane
        // combinations instead of silently altering options supplied by callers.
        if limit != 0 && (data.options.worker_count != 1 || data.options.time_limit_ms != 0) {
            return set_error(&mut data, STATUS_INVALID_ARGUMENT,
                "fixed work requires worker_count=1 and time_limit_ms=0");
        }
        data.work_limit = limit;
        STATUS_OK
    }

    pub(crate) fn terminal(&self) -> Result<VacNestingTerminal, i32> {
        self.lock().terminal.ok_or(STATUS_INVALID_STATE)
    }

    pub(crate) fn set_container(&self, polygon: Polygon) -> i32 {
        let mut data = self.lock();
        if data.state != STATE_CONFIGURING {
            return set_error(&mut data, STATUS_INVALID_STATE, "job is not configurable");
        }
        data.container = Some(polygon);
        data.error = empty_c_string();
        STATUS_OK
    }

    pub(crate) fn add_hole(&self, polygon: Polygon) -> i32 {
        let mut data = self.lock();
        if data.state != STATE_CONFIGURING {
            return set_error(&mut data, STATUS_INVALID_STATE, "job is not configurable");
        }
        if data.container.is_none() {
            return set_error(
                &mut data,
                STATUS_INVALID_STATE,
                "set the container before adding a hole",
            );
        }
        data.holes.push(polygon);
        data.error = empty_c_string();
        STATUS_OK
    }

    pub(crate) fn add_obstacle(&self, polygon: Polygon) -> i32 {
        let mut data = self.lock();
        if data.state != STATE_CONFIGURING {
            return set_error(&mut data, STATUS_INVALID_STATE, "job is not configurable");
        }
        if data.container.is_none() {
            return set_error(
                &mut data,
                STATUS_INVALID_STATE,
                "set the container before adding an obstacle",
            );
        }
        data.obstacles.push(polygon);
        data.error = empty_c_string();
        STATUS_OK
    }

    pub(crate) fn add_part(&self, id: u64, polygon: Polygon) -> i32 {
        let mut data = self.lock();
        if data.state != STATE_CONFIGURING {
            return set_error(&mut data, STATUS_INVALID_STATE, "job is not configurable");
        }
        if data.building_part.is_some() {
            return set_error(
                &mut data,
                STATUS_INVALID_STATE,
                "finish the active compound part before adding another part",
            );
        }
        if !data.part_ids.insert(id) {
            return set_error(
                &mut data,
                STATUS_INVALID_ARGUMENT,
                "part_id must be unique within a job",
            );
        }
        data.parts.push(Part {
            id,
            components: vec![PartComponent {
                outer: polygon,
                holes: Vec::new(),
            }],
        });
        data.error = empty_c_string();
        STATUS_OK
    }

    pub(crate) fn begin_part(&self, id: u64) -> i32 {
        let mut data = self.lock();
        if data.state != STATE_CONFIGURING {
            return set_error(&mut data, STATUS_INVALID_STATE, "job is not configurable");
        }
        if data.building_part.is_some() {
            return set_error(
                &mut data,
                STATUS_INVALID_STATE,
                "a compound part is already active",
            );
        }
        if data.part_ids.contains(&id) {
            return set_error(
                &mut data,
                STATUS_INVALID_ARGUMENT,
                "part_id must be unique within a job",
            );
        }
        data.building_part = Some(PartBuilder {
            id,
            components: Vec::new(),
        });
        data.error = empty_c_string();
        STATUS_OK
    }

    pub(crate) fn add_part_component_outer(&self, polygon: Polygon) -> i32 {
        let mut data = self.lock();
        if data.state != STATE_CONFIGURING {
            return set_error(&mut data, STATUS_INVALID_STATE, "job is not configurable");
        }
        let Some(builder) = data.building_part.as_mut() else {
            return set_error(
                &mut data,
                STATUS_INVALID_STATE,
                "begin a compound part first",
            );
        };
        builder.components.push(PartComponent {
            outer: polygon,
            holes: Vec::new(),
        });
        data.error = empty_c_string();
        STATUS_OK
    }

    pub(crate) fn add_part_component_hole(&self, component_index: usize, polygon: Polygon) -> i32 {
        let mut data = self.lock();
        if data.state != STATE_CONFIGURING {
            return set_error(&mut data, STATUS_INVALID_STATE, "job is not configurable");
        }
        let Some(builder) = data.building_part.as_mut() else {
            return set_error(
                &mut data,
                STATUS_INVALID_STATE,
                "begin a compound part first",
            );
        };
        let Some(component) = builder.components.get_mut(component_index) else {
            return set_error(
                &mut data,
                STATUS_INVALID_ARGUMENT,
                "compound part component index is out of range",
            );
        };
        component.holes.push(polygon);
        data.error = empty_c_string();
        STATUS_OK
    }

    pub(crate) fn end_part(&self) -> i32 {
        let mut data = self.lock();
        if data.state != STATE_CONFIGURING {
            return set_error(&mut data, STATUS_INVALID_STATE, "job is not configurable");
        }
        let Some(builder) = data.building_part.take() else {
            return set_error(
                &mut data,
                STATUS_INVALID_STATE,
                "no compound part is active",
            );
        };
        if builder.components.is_empty() {
            data.building_part = Some(builder);
            return set_error(
                &mut data,
                STATUS_INVALID_ARGUMENT,
                "a compound part needs at least one component",
            );
        }
        data.part_ids.insert(builder.id);
        data.parts.push(Part {
            id: builder.id,
            components: builder.components,
        });
        data.error = empty_c_string();
        STATUS_OK
    }

    pub(crate) fn record_error(&self, status: i32, message: &str) -> i32 {
        set_error(&mut self.lock(), status, message)
    }

    pub(crate) fn record_panic(&self) {
        let mut lifecycle = self.lifecycle.lock();
        if lifecycle.terminal {
            return;
        }
        let mut data = self.lock();
        data.state = STATE_FAILED;
        data.results.clear();
        data.error = c_string("panic was contained inside the Rust nesting bridge");
        lifecycle.run_active = false;
        lifecycle.terminal = true;
        lifecycle.terminal_status = STATUS_PANIC;
    }

    pub(crate) fn cancel(&self) {
        let caller = std::thread::current().id();
        let mut lifecycle = self.lifecycle.lock();
        let cancelling_from_callback = lifecycle.callback_thread == Some(caller);
        if lifecycle.terminal {
            let _lifecycle = self
                .lifecycle
                .drain_foreign_callbacks(lifecycle, cancelling_from_callback);
            return;
        }
        lifecycle.cancel_requested = true;
        self.cancelled.store(true, Ordering::Release);

        lifecycle = self
            .lifecycle
            .drain_foreign_callbacks(lifecycle, cancelling_from_callback);
        if lifecycle.terminal {
            return;
        }

        // A running solver owns terminal publication. In the portfolio case it
        // cannot reach that publication point until the scoped worker has been
        // joined. Self-cancellation from a callback must return here as well,
        // because waiting for the callback (or the run that owns it) would
        // deadlock the coordinator thread.
        if lifecycle.run_active {
            return;
        }

        let mut data = self.lock();
        data.results.clear();
        data.state = STATE_CANCELLED;
        data.terminal = Some(VacNestingTerminal { stop_reason: STOP_CANCELLED, ..Default::default() });
        data.error = c_string("nesting job was cancelled");
        lifecycle.terminal = true;
        lifecycle.terminal_status = STATUS_CANCELLED;
    }

    pub(crate) fn state(&self) -> i32 {
        self.lock().state
    }

    pub(crate) fn result_count(&self) -> usize {
        self.lock().results.len()
    }

    pub(crate) fn result_at(&self, index: usize) -> Result<VacNestingPlacement, i32> {
        let data = self.lock();
        data.results.get(index).copied().ok_or(STATUS_OUT_OF_RANGE)
    }

    pub(crate) fn error_ptr(&self) -> *const std::ffi::c_char {
        self.lock().error.as_ptr()
    }

    pub(crate) fn collision_proxy_input(&self) -> Result<SolverInput, i32> {
        let mut data = self.lock();
        if data.state == STATE_CANCELLED || self.cancelled.load(Ordering::Acquire) {
            return Err(STATUS_CANCELLED);
        }
        if data.state != STATE_CONFIGURING || data.building_part.is_some() {
            return Err(STATUS_INVALID_STATE);
        }
        let Some(container) = data.container.clone() else {
            return Err(STATUS_INVALID_ARGUMENT);
        };
        if data.parts.is_empty() {
            return Err(STATUS_INVALID_ARGUMENT);
        }
        Ok(SolverInput {
            options: data.options,
            container,
            holes: data.holes.clone(),
            obstacles: data.obstacles.clone(),
            parts: data.parts.clone(),
        })
    }

    pub(crate) fn collision_proxy_control(&self, total_count: usize) -> RunControl<'_> {
        RunControl {
            cancelled: &self.cancelled,
            local_stop: None,
            lifecycle: None,
            callback: None,
            user_data: std::ptr::null_mut(),
            started: Instant::now(),
            total_count: total_count.min(u32::MAX as usize) as u32,
            best_iteration: Cell::new(0),
            best_score: Cell::new(0.0),
            best_placed_count: Cell::new(0),
            best_placements: RefCell::new(Vec::new()),
            deadline_suspended: Cell::new(false),
            work_limit: 0,
            completed_work: Cell::new(0),
            limit_reason: Cell::new(STOP_COMPLETED),
        }
    }

    pub(crate) fn run(&self, callback: ProgressCallback, user_data: *mut c_void) -> i32 {
        self.run_with_solver(&PortfolioSolver::production(), callback, user_data)
    }

    pub(crate) fn run_with_solver<S: Solver>(
        &self,
        solver: &S,
        callback: ProgressCallback,
        user_data: *mut c_void,
    ) -> i32 {
        let input = {
            // Starting and cancellation are linearized through the lifecycle
            // gate. The data lock is always acquired second throughout this
            // type, avoiding a lifecycle/data lock inversion.
            let mut lifecycle = self.lifecycle.lock();
            let mut data = self.lock();
            if data.state == STATE_CANCELLED {
                return set_error(&mut data, STATUS_CANCELLED, "nesting job was cancelled");
            }
            if data.state != STATE_CONFIGURING {
                return set_error(
                    &mut data,
                    STATUS_INVALID_STATE,
                    "nesting job can only be run once",
                );
            }
            if data.building_part.is_some() {
                return set_error(
                    &mut data,
                    STATUS_INVALID_STATE,
                    "finish the active compound part before running the job",
                );
            }
            if self.cancelled.load(Ordering::Acquire) {
                data.state = STATE_CANCELLED;
                return set_error(&mut data, STATUS_CANCELLED, "nesting job was cancelled");
            }
            let Some(container) = data.container.clone() else {
                return set_error(
                    &mut data,
                    STATUS_INVALID_ARGUMENT,
                    "nesting job has no container",
                );
            };
            if data.parts.is_empty() {
                return set_error(
                    &mut data,
                    STATUS_INVALID_ARGUMENT,
                    "nesting job has no parts",
                );
            }
            data.state = STATE_RUNNING;
            data.results.clear();
            data.error = empty_c_string();
            lifecycle.run_active = true;
            SolverInput {
                options: data.options,
                container,
                holes: data.holes.clone(),
                obstacles: data.obstacles.clone(),
                parts: data.parts.clone(),
            }
        };

        let control = RunControl {
            cancelled: &self.cancelled,
            local_stop: None,
            lifecycle: Some(&self.lifecycle),
            callback,
            user_data,
            started: Instant::now(),
            total_count: input.parts.len().min(u32::MAX as usize) as u32,
            best_iteration: Cell::new(0),
            best_score: Cell::new(0.0),
            best_placed_count: Cell::new(0),
            best_placements: RefCell::new(Vec::new()),
            deadline_suspended: Cell::new(false),
            work_limit: self.lock().work_limit,
            completed_work: Cell::new(0),
            limit_reason: Cell::new(STOP_COMPLETED),
        };
        control.report(VacNestingProgress {
            stage: PROGRESS_VALIDATING,
            ..VacNestingProgress::default()
        });

        let outcome = if control.is_cancelled() {
            Err(SolverError::Cancelled)
        } else {
            match catch_unwind(AssertUnwindSafe(|| solver.solve(&input, &control))) {
                Ok(result) => result.and_then(|results| {
                    validate_solver_results(&input, &results)?;
                    Ok(results)
                }),
                Err(_) if control.is_cancelled() => Err(SolverError::Cancelled),
                Err(_) if solver.reported_incumbents_are_recoverable() => {
                    let recovered = control.recoverable_incumbent();
                    if recovered.is_empty() {
                        Err(SolverError::Panicked)
                    } else {
                        validate_solver_results(&input, &recovered).map(|()| recovered)
                    }
                }
                Err(_) => Err(SolverError::Panicked),
            }
        };

        if let Ok(results) = &outcome
            && !control.is_cancelled()
        {
            control.report_best(
                VacNestingProgress {
                    iteration: control.best_iteration.get(),
                    best_score: control.best_score.get(),
                    placed_count: results
                        .iter()
                        .filter(|placement| placement.placed != 0)
                        .count()
                        .min(u32::MAX as usize) as u32,
                    stage: PROGRESS_FINALIZING,
                    ..VacNestingProgress::default()
                },
                results,
            );
        }

        let mut lifecycle = self.lifecycle.lock();
        if lifecycle.terminal {
            return lifecycle.terminal_status;
        }
        let mut data = self.lock();
        let status = match outcome {
            Ok(_) if lifecycle.cancel_requested || self.cancelled.load(Ordering::Acquire) => {
                data.results.clear();
                data.state = STATE_CANCELLED;
                set_error(&mut data, STATUS_CANCELLED, "nesting job was cancelled")
            }
            Ok(results) => {
                data.results = results;
                data.state = STATE_COMPLETED;
                data.error = empty_c_string();
                STATUS_OK
            }
            Err(SolverError::Cancelled) => {
                data.results.clear();
                data.state = STATE_CANCELLED;
                set_error(&mut data, STATUS_CANCELLED, "nesting job was cancelled")
            }
            Err(SolverError::DeadlineReached) => {
                data.results.clear();
                data.state = STATE_FAILED;
                set_error(
                    &mut data,
                    STATUS_INTERNAL_ERROR,
                    "an internal nesting deadline escaped its recovery boundary",
                )
            }
            Err(SolverError::Panicked) => {
                data.results.clear();
                data.state = STATE_FAILED;
                set_error(
                    &mut data,
                    STATUS_PANIC,
                    "panic occurred before a recoverable nesting incumbent existed",
                )
            }
            Err(SolverError::InvalidInput(message)) => {
                data.results.clear();
                data.state = STATE_FAILED;
                set_error(&mut data, STATUS_INVALID_ARGUMENT, &message)
            }
            Err(SolverError::Failed(message)) => {
                data.results.clear();
                data.state = STATE_FAILED;
                set_error(&mut data, STATUS_INTERNAL_ERROR, &message)
            }
        };
        if matches!(data.state, STATE_COMPLETED | STATE_CANCELLED) {
            data.terminal = Some(VacNestingTerminal {
                stop_reason: if data.state == STATE_CANCELLED { STOP_CANCELLED }
                             else { control.limit_reason.get() },
                reserved: 0,
                completed_work: control.completed_work.get(),
            });
        }
        lifecycle.run_active = false;
        lifecycle.terminal = true;
        lifecycle.terminal_status = status;
        status
    }
}

pub(crate) fn normalize_polygon(mut points: Vec<VacNestingPoint>) -> Result<Polygon, &'static str> {
    if points.len() > 1 && points.first() == points.last() {
        points.pop();
    }
    points.dedup();
    if points.len() < 3 {
        return Err("a polygon needs at least three distinct consecutive points");
    }
    if points
        .iter()
        .any(|point| !point.x.is_finite() || !point.y.is_finite())
    {
        return Err("polygon coordinates must be finite");
    }
    if points
        .iter()
        .any(|point| point.x.abs() > MAX_ENGINE_MAGNITUDE || point.y.abs() > MAX_ENGINE_MAGNITUDE)
    {
        return Err("polygon coordinates exceed the nesting engine's numeric range");
    }

    let twice_area = points
        .iter()
        .zip(points.iter().cycle().skip(1))
        .take(points.len())
        .fold(0.0, |area, (left, right)| {
            area + left.x * right.y - right.x * left.y
        });
    if !twice_area.is_finite() || twice_area.abs() <= f64::EPSILON {
        return Err("polygon area must be non-zero");
    }
    Ok(Polygon(points))
}

fn set_error(data: &mut JobData, status: i32, message: &str) -> i32 {
    data.error = c_string(message);
    status
}

fn empty_c_string() -> CString {
    CString::default()
}

fn c_string(message: &str) -> CString {
    CString::new(message.replace('\0', " ")).expect("NUL bytes were removed")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering as AtomicOrdering};
    use std::sync::{Arc, Barrier, mpsc};
    use std::thread;

    fn square(size: f64) -> Polygon {
        normalize_polygon(vec![
            VacNestingPoint { x: 0.0, y: 0.0 },
            VacNestingPoint { x: size, y: 0.0 },
            VacNestingPoint { x: size, y: size },
            VacNestingPoint { x: 0.0, y: size },
        ])
        .unwrap()
    }

    fn configured_job() -> Arc<VacNestingJob> {
        let job = Arc::new(VacNestingJob::new(VacNestingOptions::default()));
        assert_eq!(job.set_container(square(100.0)), STATUS_OK);
        assert_eq!(job.add_part(7, square(10.0)), STATUS_OK);
        job
    }

    #[test]
    fn options_validation_is_fail_closed() {
        let mut options = VacNestingOptions::default();
        assert_eq!(options.validate(), Ok(()));
        options.struct_size = 0;
        assert!(options.validate().is_err());
        options = VacNestingOptions::default();
        options.api_version += 1;
        assert!(options.validate().is_err());
        options = VacNestingOptions::default();
        options.part_spacing = f64::NAN;
        assert!(options.validate().is_err());
        options = VacNestingOptions::default();
        options.rotation_mode = 99;
        assert!(options.validate().is_err());
        options = VacNestingOptions::default();
        options.rotation_mode = ROTATION_DISCRETE;
        options.rotation_step_degrees = 0.01;
        assert!(options.validate().is_err());
        options = VacNestingOptions::default();
        options.worker_count = 1025;
        assert!(options.validate().is_err());
        options = VacNestingOptions::default();
        options.container_margin = MAX_ENGINE_MAGNITUDE * 2.0;
        assert!(options.validate().is_err());
        options = VacNestingOptions::default();
        options.reserved = 1;
        assert!(options.validate().is_err());
    }

    #[test]
    fn polygon_normalization_accepts_closure_and_rejects_bad_geometry() {
        let mut closed = square(10.0).0;
        closed.push(closed[0]);
        assert_eq!(normalize_polygon(closed).unwrap().0.len(), 4);
        assert!(normalize_polygon(vec![]).is_err());
        assert!(
            normalize_polygon(vec![
                VacNestingPoint { x: 0.0, y: 0.0 },
                VacNestingPoint { x: 1.0, y: 1.0 },
                VacNestingPoint { x: 2.0, y: 2.0 },
            ])
            .is_err()
        );
        assert!(
            normalize_polygon(vec![
                VacNestingPoint {
                    x: MAX_ENGINE_MAGNITUDE * 2.0,
                    y: 0.0,
                },
                VacNestingPoint { x: 0.0, y: 1.0 },
                VacNestingPoint { x: 0.0, y: 0.0 },
            ])
            .is_err()
        );
        assert!(
            normalize_polygon(vec![
                VacNestingPoint { x: 0.0, y: 0.0 },
                VacNestingPoint {
                    x: f64::MAX,
                    y: 0.0,
                },
                VacNestingPoint { x: 0.0, y: 1.0 },
            ])
            .is_err()
        );
    }

    #[test]
    fn production_backend_places_a_simple_part() {
        let job = configured_job();
        assert_eq!(job.run(None, std::ptr::null_mut()), STATUS_OK);
        assert_eq!(job.state(), STATE_COMPLETED);
        assert_eq!(job.result_count(), 1);
        assert_eq!(job.result_at(0).unwrap().part_id, 7);
        assert_eq!(job.result_at(0).unwrap().placed, 1);
    }

    #[test]
    fn configuration_is_single_use_and_part_ids_are_unique() {
        let job = configured_job();
        assert_eq!(job.add_part(7, square(5.0)), STATUS_INVALID_ARGUMENT);
        assert_eq!(job.run(None, std::ptr::null_mut()), STATUS_OK);
        assert_eq!(job.add_part(8, square(5.0)), STATUS_INVALID_STATE);
        assert_eq!(job.run(None, std::ptr::null_mut()), STATUS_INVALID_STATE);
    }

    #[test]
    fn large_part_sets_keep_unique_id_validation_linear() {
        let job = VacNestingJob::new(VacNestingOptions::default());
        assert_eq!(job.set_container(square(1000.0)), STATUS_OK);
        for id in 0..10_000 {
            assert_eq!(job.add_part(id, square(1.0)), STATUS_OK);
        }
        assert_eq!(job.add_part(9_999, square(1.0)), STATUS_INVALID_ARGUMENT);
    }

    #[test]
    fn cancellation_before_run_remains_a_cancelled_terminal_state() {
        let job = configured_job();
        job.cancel();
        assert_eq!(job.state(), STATE_CANCELLED);
        assert_eq!(job.run(None, std::ptr::null_mut()), STATUS_CANCELLED);
        assert_eq!(job.state(), STATE_CANCELLED);
        assert_eq!(job.result_count(), 0);
    }

    struct BlockingSolver {
        entered: Arc<Barrier>,
    }

    impl Solver for BlockingSolver {
        fn solve(
            &self,
            _input: &SolverInput,
            control: &RunControl<'_>,
        ) -> Result<Vec<VacNestingPlacement>, SolverError> {
            self.entered.wait();
            while !control.is_cancelled() {
                thread::yield_now();
            }
            Ok(vec![VacNestingPlacement {
                part_id: 7,
                placed: 1,
                ..VacNestingPlacement::default()
            }])
        }
    }

    #[test]
    fn cancellation_is_visible_across_threads_and_discards_results() {
        let job = configured_job();
        let entered = Arc::new(Barrier::new(2));
        let runner_job = Arc::clone(&job);
        let runner_barrier = Arc::clone(&entered);
        let runner = thread::spawn(move || {
            runner_job.run_with_solver(
                &BlockingSolver {
                    entered: runner_barrier,
                },
                None,
                std::ptr::null_mut(),
            )
        });

        entered.wait();
        assert_eq!(job.state(), STATE_RUNNING);
        job.cancel();
        assert_eq!(runner.join().unwrap(), STATUS_CANCELLED);
        assert_eq!(job.state(), STATE_CANCELLED);
        assert_eq!(job.result_count(), 0);
    }

    #[derive(Default)]
    struct CallbackCapture {
        stages: Vec<u32>,
    }

    unsafe extern "C" fn capture_progress(
        user_data: *mut c_void,
        progress: *const VacNestingProgress,
    ) {
        if user_data.is_null() || progress.is_null() {
            return;
        }
        // SAFETY: Each test passes live stack storage for the complete
        // synchronous run and RunControl supplies a live progress value.
        let capture = unsafe { &mut *user_data.cast::<CallbackCapture>() };
        // SAFETY: Null was rejected and RunControl owns this value for the call.
        capture.stages.push(unsafe { (*progress).stage });
    }

    struct CancellingSuccessfulSolver {
        job: Arc<VacNestingJob>,
    }

    impl Solver for CancellingSuccessfulSolver {
        fn solve(
            &self,
            input: &SolverInput,
            control: &RunControl<'_>,
        ) -> Result<Vec<VacNestingPlacement>, SolverError> {
            self.job.cancel();
            assert!(!control.report_best(
                VacNestingProgress {
                    stage: PROGRESS_SOLVING,
                    placed_count: 1,
                    ..VacNestingProgress::default()
                },
                &[VacNestingPlacement {
                    part_id: input.parts[0].id,
                    placed: 1,
                    ..VacNestingPlacement::default()
                }],
            ));
            Ok(vec![VacNestingPlacement {
                part_id: input.parts[0].id,
                placed: 1,
                ..VacNestingPlacement::default()
            }])
        }
    }

    #[test]
    fn cancellation_suppresses_late_progress_and_finalizing_callbacks() {
        let job = configured_job();
        let mut capture = CallbackCapture::default();
        let status = job.run_with_solver(
            &CancellingSuccessfulSolver {
                job: Arc::clone(&job),
            },
            Some(capture_progress),
            (&mut capture as *mut CallbackCapture).cast(),
        );

        assert_eq!(status, STATUS_CANCELLED);
        assert_eq!(capture.stages, vec![PROGRESS_VALIDATING]);
        assert_eq!(job.state(), STATE_CANCELLED);
        assert_eq!(job.result_count(), 0);
    }

    struct BlockingCallbackCapture {
        entered: Barrier,
        release: Barrier,
        calls: AtomicUsize,
    }

    unsafe extern "C" fn block_progress_callback(
        user_data: *mut c_void,
        progress: *const VacNestingProgress,
    ) {
        if user_data.is_null() || progress.is_null() {
            return;
        }
        // SAFETY: The Arc allocation is retained by the runner for the whole
        // synchronous call and RunControl supplies a live progress value.
        let capture = unsafe { &*user_data.cast::<BlockingCallbackCapture>() };
        capture.calls.fetch_add(1, AtomicOrdering::AcqRel);
        capture.entered.wait();
        capture.release.wait();
    }

    #[test]
    fn foreign_cancel_waits_for_callback_and_no_callback_can_start_after_return() {
        let job = configured_job();
        let capture = Arc::new(BlockingCallbackCapture {
            entered: Barrier::new(2),
            release: Barrier::new(2),
            calls: AtomicUsize::new(0),
        });
        let runner_job = Arc::clone(&job);
        let runner_capture = Arc::clone(&capture);
        let runner = thread::spawn(move || {
            runner_job.run_with_solver(
                &SuccessfulSolver,
                Some(block_progress_callback),
                Arc::as_ptr(&runner_capture).cast_mut().cast(),
            )
        });

        capture.entered.wait();
        let (cancelled_tx, cancelled_rx) = mpsc::channel();
        let cancelling_job = Arc::clone(&job);
        let canceller = thread::spawn(move || {
            cancelling_job.cancel();
            cancelled_tx.send(()).unwrap();
        });
        let observation_deadline = Instant::now() + Duration::from_secs(1);
        while !job.cancelled.load(Ordering::Acquire) {
            assert!(
                Instant::now() < observation_deadline,
                "cancelling thread did not enter the lifecycle gate"
            );
            thread::yield_now();
        }
        assert!(
            matches!(cancelled_rx.try_recv(), Err(mpsc::TryRecvError::Empty)),
            "cancel must not return while a foreign callback is executing"
        );
        capture.release.wait();
        cancelled_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        canceller.join().unwrap();
        let calls_at_cancel_return = capture.calls.load(AtomicOrdering::Acquire);

        assert_eq!(runner.join().unwrap(), STATUS_CANCELLED);
        assert_eq!(
            capture.calls.load(AtomicOrdering::Acquire),
            calls_at_cancel_return
        );
        assert_eq!(calls_at_cancel_return, 1);
        assert_eq!(job.state(), STATE_CANCELLED);
        assert_eq!(job.result_count(), 0);
    }

    struct SelfCancellingCallback {
        job: Arc<VacNestingJob>,
        calls: AtomicUsize,
    }

    unsafe extern "C" fn cancel_from_progress_callback(
        user_data: *mut c_void,
        progress: *const VacNestingProgress,
    ) {
        if user_data.is_null() || progress.is_null() {
            return;
        }
        // SAFETY: The synchronous test retains this stack value until run
        // returns and RunControl supplied a non-null progress pointer.
        let capture = unsafe { &*user_data.cast::<SelfCancellingCallback>() };
        capture.calls.fetch_add(1, AtomicOrdering::AcqRel);
        capture.job.cancel();
    }

    #[test]
    fn callback_can_cancel_its_own_job_without_deadlock() {
        let job = configured_job();
        let capture = SelfCancellingCallback {
            job: Arc::clone(&job),
            calls: AtomicUsize::new(0),
        };
        assert_eq!(
            job.run_with_solver(
                &SuccessfulSolver,
                Some(cancel_from_progress_callback),
                (&capture as *const SelfCancellingCallback)
                    .cast_mut()
                    .cast(),
            ),
            STATUS_CANCELLED
        );
        assert_eq!(capture.calls.load(AtomicOrdering::Acquire), 1);
        assert_eq!(job.state(), STATE_CANCELLED);
        assert_eq!(job.result_count(), 0);
    }

    struct JoinVisibilitySolver {
        worker_observed_cancel: Arc<Barrier>,
        release_worker: Arc<AtomicBool>,
        worker_finished: Arc<AtomicBool>,
    }

    impl Solver for JoinVisibilitySolver {
        fn solve(
            &self,
            _input: &SolverInput,
            control: &RunControl<'_>,
        ) -> Result<Vec<VacNestingPlacement>, SolverError> {
            let shared_clock = control.shared_clock();
            let local_stop = AtomicBool::new(false);
            thread::scope(|scope| {
                scope.spawn(|| {
                    let worker_control = shared_clock.silent_control(&local_stop);
                    while !worker_control.is_cancelled() {
                        thread::yield_now();
                    }
                    self.worker_observed_cancel.wait();
                    while !self.release_worker.load(Ordering::Acquire) {
                        thread::yield_now();
                    }
                    self.worker_finished.store(true, Ordering::Release);
                });
                control.report(VacNestingProgress {
                    stage: PROGRESS_SOLVING,
                    ..VacNestingProgress::default()
                });
            });
            Err(SolverError::Cancelled)
        }
    }

    struct JoinVisibilityCallback {
        job: Arc<VacNestingJob>,
        self_cancel_returned: Barrier,
    }

    unsafe extern "C" fn self_cancel_during_scoped_worker(
        user_data: *mut c_void,
        progress: *const VacNestingProgress,
    ) {
        if user_data.is_null() || progress.is_null() {
            return;
        }
        // SAFETY: The runner retains the Arc allocation for the complete
        // synchronous run and RunControl supplies a live progress record.
        let capture = unsafe { &*user_data.cast::<JoinVisibilityCallback>() };
        // SAFETY: Null was rejected above.
        if unsafe { (*progress).stage } == PROGRESS_SOLVING {
            capture.job.cancel();
            capture.self_cancel_returned.wait();
        }
    }

    #[test]
    fn cancellation_stays_running_until_scoped_worker_is_joined() {
        let job = configured_job();
        let worker_observed_cancel = Arc::new(Barrier::new(2));
        let release_worker = Arc::new(AtomicBool::new(false));
        let worker_finished = Arc::new(AtomicBool::new(false));
        let capture = Arc::new(JoinVisibilityCallback {
            job: Arc::clone(&job),
            self_cancel_returned: Barrier::new(2),
        });
        let runner_job = Arc::clone(&job);
        let runner_capture = Arc::clone(&capture);
        let runner_worker_observed_cancel = Arc::clone(&worker_observed_cancel);
        let runner_release_worker = Arc::clone(&release_worker);
        let runner_worker_finished = Arc::clone(&worker_finished);
        let (returned_tx, returned_rx) = mpsc::channel();
        let runner = thread::spawn(move || {
            let status = runner_job.run_with_solver(
                &JoinVisibilitySolver {
                    worker_observed_cancel: runner_worker_observed_cancel,
                    release_worker: runner_release_worker,
                    worker_finished: runner_worker_finished,
                },
                Some(self_cancel_during_scoped_worker),
                Arc::as_ptr(&runner_capture).cast_mut().cast(),
            );
            returned_tx.send(status).unwrap();
        });

        capture.self_cancel_returned.wait();
        worker_observed_cancel.wait();
        assert_eq!(job.state(), STATE_RUNNING);
        assert!(!worker_finished.load(Ordering::Acquire));
        assert!(matches!(
            returned_rx.try_recv(),
            Err(mpsc::TryRecvError::Empty)
        ));

        release_worker.store(true, Ordering::Release);
        assert_eq!(
            returned_rx.recv_timeout(Duration::from_secs(1)).unwrap(),
            STATUS_CANCELLED
        );
        runner.join().unwrap();
        assert!(worker_finished.load(Ordering::Acquire));
        assert_eq!(job.state(), STATE_CANCELLED);
        assert_eq!(job.result_count(), 0);
    }

    struct BlockingSelfCancellingCallback {
        job: Arc<VacNestingJob>,
        cancelled: Barrier,
        release: Barrier,
    }

    unsafe extern "C" fn self_cancel_then_block(
        user_data: *mut c_void,
        progress: *const VacNestingProgress,
    ) {
        if user_data.is_null() || progress.is_null() {
            return;
        }
        // SAFETY: The runner retains this Arc allocation until the
        // synchronous run returns.
        let capture = unsafe { &*user_data.cast::<BlockingSelfCancellingCallback>() };
        capture.job.cancel();
        capture.cancelled.wait();
        capture.release.wait();
    }

    #[test]
    fn foreign_cancel_still_drains_a_callback_that_already_self_cancelled() {
        let job = configured_job();
        let capture = Arc::new(BlockingSelfCancellingCallback {
            job: Arc::clone(&job),
            cancelled: Barrier::new(2),
            release: Barrier::new(2),
        });
        let runner_job = Arc::clone(&job);
        let runner_capture = Arc::clone(&capture);
        let runner = thread::spawn(move || {
            runner_job.run_with_solver(
                &SuccessfulSolver,
                Some(self_cancel_then_block),
                Arc::as_ptr(&runner_capture).cast_mut().cast(),
            )
        });

        capture.cancelled.wait();
        let (returned_tx, returned_rx) = mpsc::channel();
        let cancelling_job = Arc::clone(&job);
        let canceller = thread::spawn(move || {
            cancelling_job.cancel();
            returned_tx.send(()).unwrap();
        });
        let observation_deadline = Instant::now() + Duration::from_secs(1);
        let observed_waiter = loop {
            if job.lifecycle.foreign_cancel_waiters.load(Ordering::Acquire) != 0 {
                break true;
            }
            if returned_rx.try_recv().is_ok() || Instant::now() >= observation_deadline {
                break false;
            }
            thread::yield_now();
        };
        capture.release.wait();
        if observed_waiter {
            returned_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        }

        canceller.join().unwrap();
        assert_eq!(runner.join().unwrap(), STATUS_CANCELLED);
        assert!(
            observed_waiter,
            "foreign cancellation never entered the callback-drain wait"
        );
        assert_eq!(job.state(), STATE_CANCELLED);
        assert_eq!(job.result_count(), 0);
    }

    #[test]
    fn completed_result_wins_over_a_later_cancel() {
        let job = configured_job();
        assert_eq!(
            job.run_with_solver(&SuccessfulSolver, None, std::ptr::null_mut()),
            STATUS_OK
        );
        let completed = job.result_at(0).unwrap();
        job.cancel();
        assert_eq!(job.state(), STATE_COMPLETED);
        assert_eq!(job.result_count(), 1);
        assert_eq!(job.result_at(0).unwrap(), completed);
    }

    struct PanicAfterIncumbentSolver;

    impl Solver for PanicAfterIncumbentSolver {
        fn solve(
            &self,
            input: &SolverInput,
            control: &RunControl<'_>,
        ) -> Result<Vec<VacNestingPlacement>, SolverError> {
            let incumbent = vec![VacNestingPlacement {
                part_id: input.parts[0].id,
                placed: 1,
                ..VacNestingPlacement::default()
            }];
            assert!(control.report_best(
                VacNestingProgress {
                    iteration: 7,
                    best_score: 100.0,
                    placed_count: 1,
                    stage: PROGRESS_SOLVING,
                    ..VacNestingProgress::default()
                },
                &incumbent,
            ));
            panic!("panic after a complete recoverable incumbent")
        }

        fn reported_incumbents_are_recoverable(&self) -> bool {
            true
        }
    }

    #[test]
    fn panic_after_valid_incumbent_recovers_atomically() {
        let job = configured_job();
        assert_eq!(
            job.run_with_solver(&PanicAfterIncumbentSolver, None, std::ptr::null_mut()),
            STATUS_OK
        );
        assert_eq!(job.state(), STATE_COMPLETED);
        assert_eq!(job.result_count(), 1);
        assert_eq!(job.result_at(0).unwrap().part_id, 7);
        assert_eq!(job.result_at(0).unwrap().placed, 1);
    }

    struct SuccessfulSolver;

    impl Solver for SuccessfulSolver {
        fn solve(
            &self,
            input: &SolverInput,
            _control: &RunControl<'_>,
        ) -> Result<Vec<VacNestingPlacement>, SolverError> {
            Ok(input
                .parts
                .iter()
                .map(|part| VacNestingPlacement {
                    part_id: part.id,
                    translation_x: 12.5,
                    translation_y: -2.0,
                    rotation_degrees: 90.0,
                    placed: 1,
                    ..VacNestingPlacement::default()
                })
                .collect())
        }
    }

    #[test]
    fn successful_backend_results_are_published_atomically() {
        let job = configured_job();
        assert_eq!(
            job.run_with_solver(&SuccessfulSolver, None, std::ptr::null_mut()),
            STATUS_OK
        );
        assert_eq!(job.state(), STATE_COMPLETED);
        assert_eq!(job.result_count(), 1);
        assert_eq!(
            job.result_at(0).unwrap(),
            VacNestingPlacement {
                part_id: 7,
                translation_x: 12.5,
                translation_y: -2.0,
                rotation_degrees: 90.0,
                placed: 1,
                ..VacNestingPlacement::default()
            }
        );
        assert_eq!(job.result_at(1), Err(STATUS_OUT_OF_RANGE));
    }

    struct FailingSolver;

    impl Solver for FailingSolver {
        fn solve(
            &self,
            _input: &SolverInput,
            _control: &RunControl<'_>,
        ) -> Result<Vec<VacNestingPlacement>, SolverError> {
            Err(SolverError::Failed("controlled solver failure".to_owned()))
        }
    }

    #[test]
    fn solver_failures_are_terminal_and_have_no_partial_results() {
        let job = configured_job();
        assert_eq!(
            job.run_with_solver(&FailingSolver, None, std::ptr::null_mut()),
            STATUS_INTERNAL_ERROR
        );
        assert_eq!(job.state(), STATE_FAILED);
        assert_eq!(job.result_count(), 0);
    }

    struct MalformedSolver;

    impl Solver for MalformedSolver {
        fn solve(
            &self,
            _input: &SolverInput,
            _control: &RunControl<'_>,
        ) -> Result<Vec<VacNestingPlacement>, SolverError> {
            Ok(vec![VacNestingPlacement {
                part_id: 999,
                translation_x: f64::NAN,
                placed: 2,
                ..VacNestingPlacement::default()
            }])
        }
    }

    #[test]
    fn malformed_solver_output_is_rejected_before_publication() {
        let job = configured_job();
        assert_eq!(
            job.run_with_solver(&MalformedSolver, None, std::ptr::null_mut()),
            STATUS_INTERNAL_ERROR
        );
        assert_eq!(job.state(), STATE_FAILED);
        assert_eq!(job.result_count(), 0);
    }
}

#[cfg(test)]
mod fixed_work_tests {
    use super::*;
    fn job(limit: u64) -> VacNestingJob {
        let job = VacNestingJob::new(VacNestingOptions {
            random_seed: 0xfeed_beef_1234_5678, time_limit_ms: 0, worker_count: 1,
            quality: QUALITY_DRAFT, rotation_mode: ROTATION_NONE, ..Default::default()
        });
        let square = |size| Polygon(vec![VacNestingPoint{x:0.0,y:0.0}, VacNestingPoint{x:size,y:0.0},
            VacNestingPoint{x:size,y:size}, VacNestingPoint{x:0.0,y:size}]);
        assert_eq!(job.set_container(square(100.0)), STATUS_OK);
        assert_eq!(job.add_part(1, square(10.0)), STATUS_OK);
        assert_eq!(job.add_part(2, square(12.0)), STATUS_OK);
        assert_eq!(job.set_work_limit(limit), STATUS_OK);
        job
    }
    #[test]
    fn fixed_work_repeatable_exact_limit_between_heartbeats() {
        for limit in [1, 17, 511, 1027] {
            let a = job(limit); let b = job(limit);
            assert_eq!(a.terminal(), Err(STATUS_INVALID_STATE));
            assert_eq!(a.run(None, std::ptr::null_mut()), STATUS_OK);
            assert_eq!(b.run(None, std::ptr::null_mut()), STATUS_OK);
            assert_eq!(a.terminal(), b.terminal());
            assert_eq!(a.terminal().unwrap(), VacNestingTerminal {
                stop_reason: STOP_WORK_LIMIT, reserved: 0, completed_work: limit });
            for i in 0..a.result_count() { assert_eq!(a.result_at(i), b.result_at(i)); }
            assert_eq!(a.set_work_limit(0), STATUS_INVALID_STATE);
        }
    }
    #[test]
    fn natural_finite_solver_completion_before_limit() {
        let job = job(100_000);
        assert_eq!(job.run_with_solver(&crate::solver::SinglePassJaguaSolver, None, std::ptr::null_mut()), STATUS_OK);
        let terminal = job.terminal().unwrap();
        assert_eq!(terminal.stop_reason, STOP_COMPLETED);
        assert!(terminal.completed_work > 0 && terminal.completed_work < 100_000);
    }
    #[test]
    fn fixed_work_cancel_during_search_and_before_run() {
        unsafe extern "C" fn cancel(data: *mut c_void, progress: *const VacNestingProgress) {
            if unsafe { (*progress).iteration } > 0 { unsafe { (&*(data as *const VacNestingJob)).cancel(); } }
        }
        let job = job(100_000);
        let start = Instant::now();
        assert_eq!(job.run(Some(cancel), &job as *const _ as *mut c_void), STATUS_CANCELLED);
        assert!(start.elapsed() < Duration::from_secs(2));
        let terminal = job.terminal().unwrap();
        assert_eq!(terminal.stop_reason, STOP_CANCELLED);
        assert!(terminal.completed_work > 0 && terminal.completed_work < 100_000);
        assert_eq!(job.result_count(), 0);
        let before = self::job(17); before.cancel();
        assert_eq!(before.terminal().unwrap().stop_reason, STOP_CANCELLED);
        assert_eq!(before.terminal().unwrap().completed_work, 0);
    }
    #[test]
    fn work_limit_rejects_clock_or_multiple_workers_and_zero_preserves_options() {
        let legacy = VacNestingJob::new(VacNestingOptions::default());
        assert_eq!(legacy.set_work_limit(1), STATUS_INVALID_ARGUMENT);
        assert_eq!(legacy.set_work_limit(0), STATUS_OK);
        let multiple = VacNestingJob::new(VacNestingOptions { time_limit_ms: 0, worker_count: 2, ..Default::default() });
        assert_eq!(multiple.set_work_limit(1), STATUS_INVALID_ARGUMENT);
    }
}
