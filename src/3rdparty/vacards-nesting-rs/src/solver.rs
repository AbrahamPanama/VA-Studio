// SPDX-License-Identifier: GPL-2.0-or-later

//! Deterministic constructive nesting on top of jagua-rs collision detection.
//!
//! jagua-rs deliberately separates geometry from optimization. Its repository
//! includes an LBF teaching example, but explicitly advises against using that
//! example for production work. This solver therefore owns its search policy
//! while delegating every placement feasibility decision to jagua-rs.

use crate::backend::{CanonicalIncumbent, NestingBackend, RankedSolution, SolutionPool};
use crate::job::{
    PROGRESS_SOLVING, Part, Polygon, QUALITY_BALANCED, QUALITY_DRAFT, QUALITY_HIGH,
    ROTATION_DISCRETE, ROTATION_FREE, ROTATION_NONE, ROTATION_RIGHT_ANGLES, RunControl, Solver,
    SolverError, SolverInput, VacNestingPlacement, VacNestingProgress, validate_solver_results,
};
use crate::rank::CanonicalRank;
use jagua_rs::collision_detection::hazards::filter::NoFilter;
use jagua_rs::collision_detection::{CDEConfig, CDEngine};
use jagua_rs::entities::{Container, InferiorQualityZone, Item, Layout};
use jagua_rs::geometry::fail_fast::SPSurrogateConfig;
use jagua_rs::geometry::geo_enums::RotationRange;
use jagua_rs::geometry::geo_traits::{Transformable, TransformableFrom};
use jagua_rs::geometry::primitives::{Point, Rect, SPolygon};
use jagua_rs::geometry::convex_hull::convex_hull_from_points;
use jagua_rs::geometry::geo_traits::{CollidesWith, DistanceTo};
use jagua_rs::geometry::primitives::Edge;
use jagua_rs::geometry::shape_modification::{
    ShapeModifyConfig, ShapeModifyMode, offset_shape, shape_modification_valid, simplify_shape,
};
use jagua_rs::geometry::{DTransformation, OriginalShape};
use jagua_rs::io::export::int_to_ext_transformation;
use jagua_rs::io::import::ext_to_int_transformation;
use std::f32::consts::{FRAC_PI_2, PI, TAU};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};
use std::sync::{Arc, Mutex, OnceLock};
use std::collections::HashMap;

const MAX_ANCHOR_ROTATIONS: usize = 64;
const MAX_AXIS_ANCHORS: usize = 32;
const PROGRESS_QUERY_INTERVAL: u64 = 256;
const SOLUTION_POOL_CAPACITY: usize = 8;
const MAX_EJECTION_WIDTH: usize = 3;
const MAX_RUIN_WIDTH: usize = 6;
const EXPERIMENTAL_FOOTPRINT_LIMIT_BYTES: u64 = 768 * 1024 * 1024;
const TOTAL_PROCESS_LIMIT_BYTES: u64 = 56 * 1024 * 1024 * 1024 / 10;
const MEMORY_SAFETY_NUMERATOR: u64 = 3;
const MEMORY_SAFETY_DENOMINATOR: u64 = 2;
const EXPERIMENTAL_BASE_FOOTPRINT_BYTES: u64 = 64 * 1024 * 1024;
const EXPERIMENTAL_BYTES_PER_PART: u64 = 512 * 1024;
const EXPERIMENTAL_BYTES_PER_COMPONENT: u64 = 256 * 1024;
const EXPERIMENTAL_BYTES_PER_POINT: u64 = 8 * 1024;

// The 100 ms, 1 s, 5 s, and 60 s differential hard-corpus gates governed by
// dea0848/394344b passed the monotonicity, feasibility, deadline, and strict-
// improvement policy. Keep this private constant as the one-cycle emergency
// kill switch; worker_count == 1 remains the same constructive fallback used
// by the portfolio's baseline lane.
const AUTOMATIC_PORTFOLIO_ENABLED: bool = true;

type ExperimentalLane =
    for<'input, 'control, 'cancel> fn(
        &'input SolverInput,
        &'control RunControl<'cancel>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError>;
type BaselineLane = ExperimentalLane;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PortfolioPlan {
    BaselineOnly,
    TwoLanes,
}

#[derive(Clone, Copy)]
struct PortfolioEnvironment {
    available_parallelism: Option<usize>,
    estimated_footprint: Option<u64>,
    resident_bytes: Option<Option<u64>>,
    automatic_enabled: bool,
    baseline_lane: BaselineLane,
    experimental_lane: ExperimentalLane,
}

/// Private execution portfolio beneath ABI v3. The public job contract and
/// result representation remain unchanged.
pub(crate) struct PortfolioSolver {
    environment: PortfolioEnvironment,
}

impl PortfolioSolver {
    pub(crate) const fn production() -> Self {
        Self {
            environment: PortfolioEnvironment {
                available_parallelism: None,
                estimated_footprint: None,
                resident_bytes: None,
                automatic_enabled: AUTOMATIC_PORTFOLIO_ENABLED,
                baseline_lane: solve_baseline_lane,
                experimental_lane: solve_experimental_lane,
            },
        }
    }

    #[cfg(test)]
    fn for_tests(experimental_lane: ExperimentalLane) -> Self {
        Self::for_tests_with_baseline(solve_baseline_lane, experimental_lane)
    }

    #[cfg(test)]
    fn for_tests_with_baseline(
        baseline_lane: BaselineLane,
        experimental_lane: ExperimentalLane,
    ) -> Self {
        Self {
            environment: PortfolioEnvironment {
                available_parallelism: Some(2),
                estimated_footprint: Some(0),
                resident_bytes: Some(Some(0)),
                automatic_enabled: false,
                baseline_lane,
                experimental_lane,
            },
        }
    }

    fn plan(&self, input: &SolverInput) -> PortfolioPlan {
        if input.options.worker_count == 1
            || (input.options.worker_count == 0 && !self.environment.automatic_enabled)
        {
            return PortfolioPlan::BaselineOnly;
        }
        let available_parallelism = self.environment.available_parallelism.unwrap_or_else(|| {
            thread::available_parallelism()
                .map(|value| value.get())
                .unwrap_or(1)
        });
        let estimated_footprint = self
            .environment
            .estimated_footprint
            .unwrap_or_else(|| estimate_experimental_footprint(input));
        let resident_bytes = self
            .environment
            .resident_bytes
            .unwrap_or_else(current_process_resident_bytes);
        select_portfolio_plan(
            input.options.worker_count,
            available_parallelism,
            estimated_footprint,
            resident_bytes,
            self.environment.automatic_enabled,
        )
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug)]
struct PortfolioTiming {
    common_started: Instant,
    common_deadline: Option<Instant>,
    baseline_started: Instant,
    baseline_finished: Instant,
    experimental_clock_started: Option<Instant>,
    experimental_deadline: Option<Instant>,
    experimental_finished: Option<Instant>,
    final_validation_started: Option<Instant>,
    final_validation_elapsed: Duration,
    completed_at: Instant,
    terminal_deadline_overrun: Duration,
}

impl PortfolioTiming {
    fn complete_at(&mut self, completed_at: Instant) {
        self.completed_at = completed_at;
        self.terminal_deadline_overrun = self
            .common_deadline
            .map(|deadline| completed_at.saturating_duration_since(deadline))
            .unwrap_or(Duration::ZERO);
    }
}

#[allow(dead_code)]
struct PortfolioExecution {
    placements: Vec<VacNestingPlacement>,
    timing: PortfolioTiming,
}

struct ExperimentalLaneReport {
    placements: Option<Vec<VacNestingPlacement>>,
    clock_started: Instant,
    deadline: Option<Instant>,
    finished: Instant,
}

fn select_portfolio_plan(
    worker_count: u32,
    available_parallelism: usize,
    estimated_experimental_footprint: u64,
    resident_bytes: Option<u64>,
    automatic_enabled: bool,
) -> PortfolioPlan {
    if worker_count == 1
        || available_parallelism < 2
        || !portfolio_memory_is_safe(estimated_experimental_footprint, resident_bytes)
    {
        return PortfolioPlan::BaselineOnly;
    }

    if worker_count == 0 && !automatic_enabled {
        PortfolioPlan::BaselineOnly
    } else {
        // Explicit values above two are deliberately capped at the same
        // baseline-plus-one portfolio during stabilization.
        PortfolioPlan::TwoLanes
    }
}

fn safety_adjusted_bytes(bytes: u64) -> Option<u64> {
    bytes
        .checked_mul(MEMORY_SAFETY_NUMERATOR)?
        .checked_add(MEMORY_SAFETY_DENOMINATOR - 1)
        .map(|value| value / MEMORY_SAFETY_DENOMINATOR)
}

fn portfolio_memory_is_safe(
    estimated_experimental_footprint: u64,
    resident_bytes: Option<u64>,
) -> bool {
    let Some(adjusted_lane) = safety_adjusted_bytes(estimated_experimental_footprint) else {
        return false;
    };
    // Strictly below the limit: equality has no safety headroom.
    if adjusted_lane >= EXPERIMENTAL_FOOTPRINT_LIMIT_BYTES {
        return false;
    }
    // Admission happens before either lane prepares its private Jagua state.
    // Current RSS therefore needs headroom for both the baseline and the
    // experimental allocation, even though the per-lane cap only applies to
    // the additional experimental lane.
    let Some(new_lane_bytes) = estimated_experimental_footprint.checked_mul(2) else {
        return false;
    };
    let Some(total) = resident_bytes
        .and_then(|resident| resident.checked_add(new_lane_bytes))
        .and_then(safety_adjusted_bytes)
    else {
        return false;
    };
    total < TOTAL_PROCESS_LIMIT_BYTES
}

#[cfg(target_os = "macos")]
fn current_process_resident_bytes() -> Option<u64> {
    use std::ffi::{c_int, c_void};
    use std::mem::{MaybeUninit, size_of};

    const PROC_PIDTASKINFO: c_int = 4;

    #[allow(dead_code)]
    #[repr(C)]
    struct ProcTaskInfo {
        virtual_size: u64,
        resident_size: u64,
        total_user: u64,
        total_system: u64,
        threads_user: u64,
        threads_system: u64,
        policy: i32,
        faults: i32,
        pageins: i32,
        cow_faults: i32,
        messages_sent: i32,
        messages_received: i32,
        syscalls_mach: i32,
        syscalls_unix: i32,
        context_switches: i32,
        thread_count: i32,
        running_threads: i32,
        priority: i32,
    }

    unsafe extern "C" {
        fn getpid() -> c_int;
        fn proc_pidinfo(
            pid: c_int,
            flavor: c_int,
            arg: u64,
            buffer: *mut c_void,
            buffer_size: c_int,
        ) -> c_int;
    }

    let buffer_size = c_int::try_from(size_of::<ProcTaskInfo>()).ok()?;
    let mut info = MaybeUninit::<ProcTaskInfo>::zeroed();
    // SAFETY: `info` is writable for exactly `buffer_size` bytes and both
    // functions are stable macOS libproc/libSystem interfaces.
    let written = unsafe {
        proc_pidinfo(
            getpid(),
            PROC_PIDTASKINFO,
            0,
            info.as_mut_ptr().cast(),
            buffer_size,
        )
    };
    if written != buffer_size {
        return None;
    }
    // SAFETY: proc_pidinfo reported that it initialized the complete struct.
    Some(unsafe { info.assume_init() }.resident_size)
}

#[cfg(target_os = "linux")]
fn current_process_resident_bytes() -> Option<u64> {
    let status = std::fs::read_to_string("/proc/self/status").ok()?;
    let value_kib = status.lines().find_map(|line| {
        line.strip_prefix("VmRSS:")?
            .split_ascii_whitespace()
            .next()?
            .parse::<u64>()
            .ok()
    })?;
    value_kib.checked_mul(1024)
}

#[cfg(target_os = "windows")]
fn current_process_resident_bytes() -> Option<u64> {
    use std::ffi::c_void;
    use std::mem::{MaybeUninit, size_of};

    type Handle = *mut c_void;

    #[allow(non_snake_case)]
    #[repr(C)]
    struct ProcessMemoryCounters {
        cb: u32,
        PageFaultCount: u32,
        PeakWorkingSetSize: usize,
        WorkingSetSize: usize,
        QuotaPeakPagedPoolUsage: usize,
        QuotaPagedPoolUsage: usize,
        QuotaPeakNonPagedPoolUsage: usize,
        QuotaNonPagedPoolUsage: usize,
        PagefileUsage: usize,
        PeakPagefileUsage: usize,
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentProcess() -> Handle;
    }
    #[link(name = "psapi")]
    unsafe extern "system" {
        fn GetProcessMemoryInfo(
            process: Handle,
            counters: *mut ProcessMemoryCounters,
            size: u32,
        ) -> i32;
    }

    let size = u32::try_from(size_of::<ProcessMemoryCounters>()).ok()?;
    let mut counters = MaybeUninit::<ProcessMemoryCounters>::zeroed();
    // SAFETY: The pseudo-handle remains valid for this process and counters is
    // writable for the exact size passed to the operating system.
    let succeeded = unsafe {
        (*counters.as_mut_ptr()).cb = size;
        GetProcessMemoryInfo(GetCurrentProcess(), counters.as_mut_ptr(), size)
    };
    if succeeded == 0 {
        return None;
    }
    // SAFETY: a successful call initialized the complete counter structure.
    u64::try_from(unsafe { counters.assume_init() }.WorkingSetSize).ok()
}

#[cfg(not(any(target_os = "macos", target_os = "linux", target_os = "windows")))]
fn current_process_resident_bytes() -> Option<u64> {
    None
}

fn estimate_experimental_footprint(input: &SolverInput) -> u64 {
    let mut part_count = 0_u64;
    let mut component_count = 0_u64;
    let mut point_count = polygon_point_count(&input.container);
    point_count = input
        .holes
        .iter()
        .chain(&input.obstacles)
        .fold(point_count, |total, polygon| {
            total.saturating_add(polygon_point_count(polygon))
        });

    for part in &input.parts {
        part_count = part_count.saturating_add(1);
        for component in &part.components {
            component_count = component_count.saturating_add(1);
            point_count = point_count.saturating_add(polygon_point_count(&component.outer));
            point_count = component.holes.iter().fold(point_count, |total, polygon| {
                total.saturating_add(polygon_point_count(polygon))
            });
        }
    }

    EXPERIMENTAL_BASE_FOOTPRINT_BYTES
        .saturating_add(part_count.saturating_mul(EXPERIMENTAL_BYTES_PER_PART))
        .saturating_add(component_count.saturating_mul(EXPERIMENTAL_BYTES_PER_COMPONENT))
        .saturating_add(point_count.saturating_mul(EXPERIMENTAL_BYTES_PER_POINT))
}

fn polygon_point_count(polygon: &Polygon) -> u64 {
    u64::try_from(polygon.0.len()).unwrap_or(u64::MAX)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u64)]
enum OptimizerPhase {
    Exploration = 0x6578_706c_6f72_6501,
    Insertion = 0x696e_7365_7274_0002,
    Ejection = 0x656a_6563_7400_0003,
    RuinRecreate = 0x7275_696e_7265_0004,
    Refinement = 0x7265_6669_6e65_0005,
}

pub(crate) struct JaguaSolver;

#[cfg(test)]
struct SinglePassJaguaSolver;

/// Legacy staged NEST-009 wrapper retained for bounded primitive tests. The
/// public job uses `PortfolioSolver`; this sequential baseline-then-refinement
/// path remains inactive because a wall-clock baseline consumes its budget.
#[allow(dead_code)]
pub(crate) struct FixedContainerOptimizer {
    exploration_passes: u64,
    refinement_moves: usize,
    baseline_limit: RefinementLimit,
}

#[allow(dead_code)]
impl FixedContainerOptimizer {
    pub(crate) const fn bounded(exploration_passes: u64, refinement_moves: usize) -> Self {
        Self {
            exploration_passes,
            refinement_moves,
            baseline_limit: RefinementLimit::WallClock,
        }
    }

    #[cfg(test)]
    const fn bounded_after_baseline_passes(
        baseline_passes: u64,
        exploration_passes: u64,
        refinement_moves: usize,
    ) -> Self {
        Self {
            exploration_passes,
            refinement_moves,
            baseline_limit: RefinementLimit::CompletedPasses(baseline_passes),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RefinementLimit {
    WallClock,
    #[cfg(test)]
    CompletedPasses(u64),
}

#[derive(Clone, Copy)]
struct SolverProfile {
    sample_budget: usize,
    simplify_tolerance: Option<f32>,
    quadtree_depth: u8,
    search_surrogates: bool,
}

impl SolverProfile {
    fn from_quality(quality: i32) -> Self {
        match quality {
            QUALITY_DRAFT => Self {
                sample_budget: 512,
                simplify_tolerance: Some(0.001),
                quadtree_depth: 4,
                search_surrogates: true,
            },
            QUALITY_HIGH => Self {
                sample_budget: 8192,
                simplify_tolerance: None,
                quadtree_depth: 7,
                search_surrogates: true,
            },
            QUALITY_BALANCED => Self {
                sample_budget: 2048,
                simplify_tolerance: Some(0.0001),
                quadtree_depth: 5,
                search_surrogates: true,
            },
            _ => unreachable!("options are validated before a solver is created"),
        }
    }

    fn cde_config(self) -> CDEConfig {
        CDEConfig {
            quadtree_depth: self.quadtree_depth,
            cd_threshold: 96,
            item_surrogate_config: SPSurrogateConfig {
                n_pole_limits: [(100, 0.0), (20, 0.75), (10, 0.90)],
                ff_pole_area_ratio: 0.5,
                n_ff_piers: 0,
            },
        }
    }
}

#[derive(Clone, Copy)]
struct SearchCandidate {
    transform: DTransformation,
    score: f64,
}

struct SearchOutcome {
    best: Option<SearchCandidate>,
    timed_out: bool,
}

struct PassOutcome {
    solution: RankedSolution,
    timed_out: bool,
}

struct NeighborhoodOutcome {
    candidate: Option<RankedSolution>,
    timed_out: bool,
}

struct LayoutBuildOutcome {
    layout: Option<Layout>,
    timed_out: bool,
}

struct ValidationOutcome {
    feasible: bool,
    timed_out: bool,
}

/// Jagua geometry prepared once from the immutable job snapshot. Backends
/// share this representation so refinement never reinterprets source input or
/// drifts from the constructive fallback's clearance policy.
struct PreparedProblem {
    profile: SolverProfile,
    container: Container,
    items: Vec<CompoundItem>,
}

/// One document payload represented by one or more rigid collision islands.
/// Every component uses the same pre-transform, so one solver transform maps
/// back to one top-level SVG transform for the complete payload.
struct CompoundItem {
    part_id: u64,
    components: Vec<Item>,
    allowed_rotation: RotationRange,
    diameter: f32,
    area: f32,
}

#[derive(Clone, Copy)]
struct TranslationBounds {
    x_min: f32,
    x_max: f32,
    y_min: f32,
    y_max: f32,
    rotated_bbox: Rect,
    epsilon: f32,
}

impl Solver for PortfolioSolver {
    fn solve(
        &self,
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        if self.plan(input) == PortfolioPlan::BaselineOnly {
            return JaguaSolver.solve(input, control);
        }

        solve_portfolio_execution_with_lanes(
            input,
            control,
            self.environment.baseline_lane,
            self.environment.experimental_lane,
        )
        .map(|execution| execution.placements)
    }

    fn reported_incumbents_are_recoverable(&self) -> bool {
        true
    }
}

impl Solver for JaguaSolver {
    fn solve(
        &self,
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        self.solve_backend(input, control)
    }

    fn reported_incumbents_are_recoverable(&self) -> bool {
        true
    }
}

impl NestingBackend for JaguaSolver {
    fn solve_backend(
        &self,
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        solve_with_limit(input, control, RefinementLimit::WallClock)
    }
}

impl Solver for FixedContainerOptimizer {
    fn solve(
        &self,
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        self.solve_backend(input, control)
    }

    fn reported_incumbents_are_recoverable(&self) -> bool {
        true
    }
}

impl NestingBackend for FixedContainerOptimizer {
    fn solve_backend(
        &self,
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        solve_fixed_container(
            input,
            control,
            self.exploration_passes,
            self.refinement_moves,
            self.baseline_limit,
        )
    }
}

#[cfg(test)]
impl Solver for SinglePassJaguaSolver {
    fn solve(
        &self,
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        solve_with_limit(input, control, RefinementLimit::CompletedPasses(1))
    }

    fn reported_incumbents_are_recoverable(&self) -> bool {
        true
    }
}

fn solve_with_limit(
    input: &SolverInput,
    control: &RunControl<'_>,
    _refinement_limit: RefinementLimit,
) -> Result<Vec<VacNestingPlacement>, SolverError> {
    let problem = prepare_problem(input, control)?;
    let mut iteration = 0_u64;
    let mut pass = 0_u64;
    let mut incumbent: Option<CanonicalIncumbent> =
        identical_part_grid_seed(input, &problem, control)?.map(CanonicalIncumbent::new);

    loop {
        if let Some(error) = control.stop_error() {
            return Err(error);
        }
        #[cfg(test)]
        if pass > 0
            && matches!(
                _refinement_limit,
                RefinementLimit::CompletedPasses(limit) if pass >= limit
            )
        {
            break;
        }
        if pass > 0 && control.time_limit_reached(input.options.time_limit_ms) {
            break;
        }
        // The time limit bounds improvement, not the first layout: when no
        // seed exists, the first pass always completes (cancellation still
        // stops it), so slow preparation can no longer publish an empty result.
        let outcome = if pass == 0 && incumbent.is_none() {
            control.with_deadline_suspended(|| {
                construct_pass(input, &problem, control, &mut iteration, pass)
            })?
        } else {
            construct_pass(input, &problem, control, &mut iteration, pass)?
        };
        let timed_out = outcome.timed_out;
        if let Some(current) = incumbent.as_mut() {
            current.consider(outcome.solution);
        } else {
            incumbent = Some(CanonicalIncumbent::new(outcome.solution));
        }
        let best = incumbent
            .as_ref()
            .expect("the first pass always publishes a result");
        if let Some(error) = control.stop_error() {
            return Err(error);
        }
        control.report_best(
            VacNestingProgress {
                iteration,
                best_score: best.best().rank().canonical_area_score(),
                placed_count: best.best().rank().placed_count(),
                stage: PROGRESS_SOLVING,
                ..VacNestingProgress::default()
            },
            best.best().placements(),
        );
        if let Some(error) = control.stop_error() {
            return Err(error);
        }
        if timed_out || control.time_limit_reached(input.options.time_limit_ms) {
            break;
        }
        pass = pass.saturating_add(1);
    }

    Ok(incumbent
        .map(CanonicalIncumbent::into_best)
        .unwrap_or_else(|| empty_results(input)))
}

#[cfg(test)]
fn solve_portfolio_execution(
    input: &SolverInput,
    control: &RunControl<'_>,
    experimental_lane: ExperimentalLane,
) -> Result<PortfolioExecution, SolverError> {
    solve_portfolio_execution_with_lanes(input, control, solve_baseline_lane, experimental_lane)
}

fn solve_baseline_lane(
    input: &SolverInput,
    control: &RunControl<'_>,
) -> Result<Vec<VacNestingPlacement>, SolverError> {
    JaguaSolver.solve(input, control)
}

fn solve_portfolio_execution_with_lanes(
    input: &SolverInput,
    control: &RunControl<'_>,
    baseline_lane: BaselineLane,
    experimental_lane: ExperimentalLane,
) -> Result<PortfolioExecution, SolverError> {
    solve_portfolio_execution_with_lanes_and_spawn_mode(
        input,
        control,
        baseline_lane,
        experimental_lane,
        WorkerSpawnMode::Create,
    )
}

#[derive(Clone, Copy)]
enum WorkerSpawnMode {
    Create,
    #[cfg(test)]
    InjectFailure,
}

fn solve_portfolio_execution_with_lanes_and_spawn_mode(
    input: &SolverInput,
    control: &RunControl<'_>,
    baseline_lane: BaselineLane,
    experimental_lane: ExperimentalLane,
    spawn_mode: WorkerSpawnMode,
) -> Result<PortfolioExecution, SolverError> {
    let shared_clock = control.shared_clock();
    let common_started = shared_clock.started_at();
    let common_deadline = shared_clock.deadline(input.options.time_limit_ms);
    let local_stop = AtomicBool::new(false);

    thread::scope(|scope| {
        // Only SharedRunClock and immutable input enter this closure. In
        // particular, callback and user_data remain on the coordinator.
        let worker_stop = &local_stop;
        let worker = match spawn_mode {
            WorkerSpawnMode::Create => thread::Builder::new()
                .name("vacards-nesting-experimental".to_owned())
                .spawn_scoped(scope, move || {
                    let silent_control = shared_clock.silent_control(worker_stop);
                    let placements = match catch_unwind(AssertUnwindSafe(|| {
                        experimental_lane(input, &silent_control)
                    })) {
                        Ok(Ok(placements)) if !silent_control.stop_requested() => Some(placements),
                        Ok(Ok(_)) | Ok(Err(_)) | Err(_) => None,
                    };
                    ExperimentalLaneReport {
                        placements,
                        clock_started: silent_control.started_at(),
                        deadline: silent_control.deadline(input.options.time_limit_ms),
                        finished: Instant::now(),
                    }
                })
                .ok(),
            #[cfg(test)]
            WorkerSpawnMode::InjectFailure => None,
        };

        // Frozen production behavior runs on this caller/coordinator thread
        // for its complete configured wall-clock budget and owns all progress.
        // A failed worker creation deliberately reaches this exact same path.
        let baseline_started = Instant::now();
        let baseline_result = catch_unwind(AssertUnwindSafe(|| baseline_lane(input, control)));
        let baseline_finished = Instant::now();
        let baseline_is_structurally_valid = matches!(
            &baseline_result,
            Ok(Ok(placements)) if validate_solver_results(input, placements).is_ok()
        );
        if !baseline_is_structurally_valid
            || input.options.time_limit_ms == 0
            || control.is_cancelled()
        {
            // A lane-local stop is distinct from public cancellation. It lets
            // an unlimited experimental lane leave its checkpoints before the
            // scoped join when the baseline fails or unwinds.
            local_stop.store(true, Ordering::Release);
        }
        let experimental = worker.and_then(|worker| worker.join().ok());

        if control.is_cancelled() {
            return Err(SolverError::Cancelled);
        }
        let baseline = match baseline_result {
            Ok(result) => result?,
            Err(payload) => std::panic::resume_unwind(payload),
        };
        validate_solver_results(input, &baseline)?;
        let mut timing = PortfolioTiming {
            common_started,
            common_deadline,
            baseline_started,
            baseline_finished,
            experimental_clock_started: experimental.as_ref().map(|lane| lane.clock_started),
            experimental_deadline: experimental.as_ref().and_then(|lane| lane.deadline),
            experimental_finished: experimental.as_ref().map(|lane| lane.finished),
            final_validation_started: None,
            final_validation_elapsed: Duration::ZERO,
            completed_at: Instant::now(),
            terminal_deadline_overrun: Duration::ZERO,
        };
        let Some(experimental) = experimental.and_then(|lane| lane.placements) else {
            timing.complete_at(Instant::now());
            return Ok(PortfolioExecution {
                placements: baseline,
                timing,
            });
        };

        let Some(baseline_ranked) = RankedSolution::new(input, baseline, true) else {
            return Err(SolverError::Failed(
                "production portfolio lane returned malformed results".to_owned(),
            ));
        };
        let Some(experimental_ranked) = RankedSolution::new(input, experimental, true) else {
            timing.complete_at(Instant::now());
            return Ok(PortfolioExecution {
                placements: baseline_ranked.into_placements(),
                timing,
            });
        };
        if !experimental_ranked
            .rank()
            .is_better_than(baseline_ranked.rank())
        {
            timing.complete_at(Instant::now());
            return Ok(PortfolioExecution {
                placements: baseline_ranked.into_placements(),
                timing,
            });
        }

        // This replay is intentionally allowed after the shared deadline. It
        // is bounded to one complete vector, remains cancellation-aware between
        // Jagua calls, and is measured so differential gates can enforce the
        // p95 overrun contract. Jagua 0.8 cannot interrupt one call in flight.
        let validation_started = Instant::now();
        timing.final_validation_started = Some(validation_started);
        let validation =
            authoritative_portfolio_validation(input, experimental_ranked.placements(), control);
        let validation_finished = Instant::now();
        timing.final_validation_elapsed = validation_finished.duration_since(validation_started);
        timing.complete_at(validation_finished);

        match validation {
            Err(SolverError::Cancelled) => Err(SolverError::Cancelled),
            Ok(true) => {
                if !control.adopt_final_incumbent(
                    experimental_ranked.rank().canonical_area_score(),
                    experimental_ranked.placements(),
                ) {
                    return Err(SolverError::Cancelled);
                }
                Ok(PortfolioExecution {
                    placements: experimental_ranked.into_placements(),
                    timing,
                })
            }
            Ok(false) | Err(_) => Ok(PortfolioExecution {
                placements: baseline_ranked.into_placements(),
                timing,
            }),
        }
    })
}

pub(crate) fn authoritative_portfolio_validation(
    input: &SolverInput,
    placements: &[VacNestingPlacement],
    control: &RunControl<'_>,
) -> Result<bool, SolverError> {
    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    let problem = prepare_problem(input, control)?;
    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    solution_is_feasible(input, &problem, placements, control)
}

/// External candidates need exact polygon replay, not the expensive pole
/// surrogates used to screen thousands of search poses. Keep the same contours,
/// offsets, simplification and collision engine as the normal validator.
pub(crate) fn validate_external_candidate(
    input: &SolverInput,
    placements: &[VacNestingPlacement],
    control: &RunControl<'_>,
) -> Result<bool, SolverError> {
    preparation_checkpoint(input, control, false)?;
    let mut profile = SolverProfile::from_quality(input.options.quality);
    profile.search_surrogates = false;
    let container = build_container(input, profile, control, false)?;
    let items = build_items(input, profile, control, false)?;
    let problem = PreparedProblem { profile, container, items };
    solution_is_feasible(input, &problem, placements, control)
}

#[cfg(test)]
fn permitted_deadline_overrun(time_limit_ms: u64) -> Duration {
    Duration::from_millis(250).max(Duration::from_millis(time_limit_ms / 20))
}

#[cfg(test)]
fn deadline_overrun_within_gate(time_limit_ms: u64, overrun: Duration) -> bool {
    time_limit_ms == 0 || overrun <= permitted_deadline_overrun(time_limit_ms)
}

fn solve_experimental_lane(
    input: &SolverInput,
    control: &RunControl<'_>,
) -> Result<Vec<VacNestingPlacement>, SolverError> {
    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    let Some(problem) = prepare_problem_for_optimizer(input, control)? else {
        return Ok(empty_results(input));
    };
    let mut iteration = 0_u64;
    let initial_pass =
        derive_phase_seed(input.options.random_seed, OptimizerPhase::Exploration, 0, 0);
    let initial = construct_pass(
        input,
        &problem,
        control,
        &mut iteration,
        initial_pass,
    )?;
    let mut pool = SolutionPool::new(SOLUTION_POOL_CAPACITY);
    let retained = pool.insert(initial.solution.clone());
    debug_assert!(retained);
    let mut incumbent = CanonicalIncumbent::new(initial.solution);
    if initial.timed_out || phase_checkpoint(input, control)? {
        return Ok(incumbent.into_best());
    }

    let exploration_cutoff = experimental_exploration_cutoff(input, control);
    let mut step = 0_u64;
    let mut refinement_move = 0_usize;
    let mut stagnation = 0_usize;

    loop {
        if phase_checkpoint(input, control)? {
            break;
        }
        let exploring = if input.options.time_limit_ms == 0 {
            // Unlimited mode has no temporal quarter boundary. Keep the same
            // 3:1 exploration/refinement allocation until cancellation.
            step % 4 != 3
        } else {
            exploration_cutoff.is_none_or(|cutoff| Instant::now() < cutoff)
        };
        let outcome = if exploring {
            adaptive_exploration_step(
                input,
                &problem,
                &pool,
                control,
                &mut iteration,
                step,
                stagnation,
            )?
        } else {
            let outcome = adaptive_refinement_step(
                input,
                &problem,
                incumbent.best(),
                control,
                &mut iteration,
                refinement_move,
            )?;
            refinement_move = refinement_move.saturating_add(1);
            outcome
        };
        if outcome.timed_out {
            break;
        }

        let improved = if let Some(candidate) = outcome.candidate {
            admit_candidate(candidate, &mut pool, &mut incumbent, control, iteration)?
        } else {
            control.report_heartbeat(iteration);
            false
        };
        stagnation = if improved {
            0
        } else {
            stagnation.saturating_add(1)
        };
        step = step.wrapping_add(1);
    }

    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    Ok(incumbent.into_best())
}

fn experimental_exploration_cutoff(
    input: &SolverInput,
    control: &RunControl<'_>,
) -> Option<Instant> {
    if input.options.time_limit_ms == 0 {
        return None;
    }
    let exploration_ms = input.options.time_limit_ms.saturating_mul(3) / 4;
    control
        .started_at()
        .checked_add(Duration::from_millis(exploration_ms))
}

#[allow(clippy::too_many_arguments)]
fn adaptive_exploration_step(
    input: &SolverInput,
    problem: &PreparedProblem,
    pool: &SolutionPool,
    control: &RunControl<'_>,
    iteration: &mut u64,
    step: u64,
    stagnation: usize,
) -> Result<NeighborhoodOutcome, SolverError> {
    if phase_checkpoint(input, control)? {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: true,
        });
    }

    // Cycle every operator deterministically; stagnation adapts neighborhood
    // width and source selection without starving an operator family.
    let operator = (step % 4) as u8;
    if operator == 0 {
        let pass = derive_phase_seed(
            input.options.random_seed,
            OptimizerPhase::Exploration,
            step.saturating_add(1),
            0,
        );
        let outcome = construct_pass(
            input,
            problem,
            control,
            iteration,
            pass,
        )?;
        if outcome.timed_out || phase_checkpoint(input, control)? {
            return Ok(NeighborhoodOutcome {
                candidate: None,
                timed_out: true,
            });
        }
        let validation = solution_is_feasible_with_deadline(
            input,
            problem,
            outcome.solution.placements(),
            control,
        )?;
        return Ok(NeighborhoodOutcome {
            candidate: validation.feasible.then_some(outcome.solution),
            timed_out: validation.timed_out,
        });
    }

    let sources = pool.entries().cloned().collect::<Vec<_>>();
    if sources.is_empty() {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: false,
        });
    }
    let source_index = (derive_phase_seed(
        input.options.random_seed,
        OptimizerPhase::Exploration,
        step,
        stagnation as u64,
    ) % sources.len() as u64) as usize;
    let source = &sources[source_index];

    match operator {
        1 => insert_unplaced_parts(input, problem, source, control, iteration, step),
        2 => {
            let target = ordered_unplaced_indices(
                input,
                problem,
                source.placements(),
                OptimizerPhase::Ejection,
                step,
            )
            .into_iter()
            .next();
            let Some(target) = target else {
                return Ok(NeighborhoodOutcome {
                    candidate: None,
                    timed_out: false,
                });
            };
            let ejection_width = 1 + stagnation % MAX_EJECTION_WIDTH;
            bounded_ejection_insert(
                input,
                problem,
                source,
                target,
                ejection_width,
                control,
                iteration,
                step,
            )
        }
        _ => {
            let placed_count = source.rank().placed_count() as usize;
            let ruin_width = placed_count
                .min(MAX_RUIN_WIDTH)
                .min(1 + stagnation % MAX_RUIN_WIDTH);
            ruin_and_recreate(input, problem, source, ruin_width, control, iteration, step)
        }
    }
}

fn adaptive_refinement_step(
    input: &SolverInput,
    problem: &PreparedProblem,
    source: &RankedSolution,
    control: &RunControl<'_>,
    iteration: &mut u64,
    move_index: usize,
) -> Result<NeighborhoodOutcome, SolverError> {
    let Some(part_index) =
        refinement_part_index(source.placements(), input.options.random_seed, move_index)
    else {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: phase_checkpoint(input, control)?,
        });
    };
    let candidate = relocate_part(
        input,
        problem,
        source,
        part_index,
        control,
        iteration,
        derive_phase_seed(
            input.options.random_seed,
            OptimizerPhase::Refinement,
            move_index as u64,
            input.parts[part_index].id,
        ),
    )?;
    Ok(NeighborhoodOutcome {
        candidate,
        timed_out: phase_checkpoint(input, control)?,
    })
}

fn solve_fixed_container(
    input: &SolverInput,
    control: &RunControl<'_>,
    exploration_passes: u64,
    refinement_moves: usize,
    baseline_limit: RefinementLimit,
) -> Result<Vec<VacNestingPlacement>, SolverError> {
    // This is deliberately the exact current production path, not a similar
    // reconstruction or a single-pass approximation. It provides a clean
    // differential baseline and a result that the staged optimizer can never
    // worsen. With the wall-clock limit the current solver normally consumes
    // the configured deadline, so activation remains gated until integration
    // can schedule both equal-budget backends without extending that deadline.
    let baseline = solve_with_limit(input, control, baseline_limit)?;
    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    if baseline_limit == RefinementLimit::WallClock
        && control.time_limit_reached(input.options.time_limit_ms)
    {
        return Ok(baseline);
    }

    optimize_fixed_container_from_baseline(
        input,
        control,
        baseline,
        exploration_passes,
        refinement_moves,
        true,
    )
}

fn optimize_fixed_container_from_baseline(
    input: &SolverInput,
    control: &RunControl<'_>,
    baseline: Vec<VacNestingPlacement>,
    exploration_passes: u64,
    refinement_moves: usize,
    baseline_was_reported: bool,
) -> Result<Vec<VacNestingPlacement>, SolverError> {
    let initial = RankedSolution::new(input, baseline, true).ok_or_else(|| {
        SolverError::Failed("production baseline produced malformed results".to_owned())
    })?;
    if phase_checkpoint(input, control)? {
        return Ok(initial.into_placements());
    }
    let Some(problem) = prepare_problem_for_optimizer(input, control)? else {
        return Ok(initial.into_placements());
    };
    let validation =
        solution_is_feasible_with_deadline(input, &problem, initial.placements(), control)?;
    if validation.timed_out {
        return Ok(initial.into_placements());
    }
    if !validation.feasible {
        return Err(SolverError::Failed(
            "production baseline failed authoritative Jagua validation".to_owned(),
        ));
    }
    let mut iteration = control.best_iteration();
    let mut pool = SolutionPool::new(SOLUTION_POOL_CAPACITY);
    let initial_retained = pool.insert(initial.clone());
    debug_assert!(initial_retained);
    let mut incumbent = CanonicalIncumbent::new(initial);
    if !baseline_was_reported {
        report_incumbent(control, iteration, &incumbent)?;
    }
    let mut timed_out = phase_checkpoint(input, control)?;
    let recovered_panic = contain_optimizer_panic(control, || {
        if !timed_out {
            for pass in 1..=exploration_passes {
                if phase_checkpoint(input, control)? {
                    timed_out = true;
                    break;
                }

                let search_pass = derive_phase_seed(
                    input.options.random_seed,
                    OptimizerPhase::Exploration,
                    pass,
                    0,
                );
                let candidate = construct_pass(
                    input,
                    &problem,
                    control,
                    &mut iteration,
                    search_pass,
                )?;
                if candidate.timed_out || phase_checkpoint(input, control)? {
                    timed_out = true;
                    break;
                }
                let validation = solution_is_feasible_with_deadline(
                    input,
                    &problem,
                    candidate.solution.placements(),
                    control,
                )?;
                if validation.timed_out {
                    timed_out = true;
                    break;
                }
                if validation.feasible {
                    admit_candidate(
                        candidate.solution,
                        &mut pool,
                        &mut incumbent,
                        control,
                        iteration,
                    )?;
                } else {
                    control.report_heartbeat(iteration);
                }
            }
        }

        if exploration_passes > 0 && !timed_out {
            let sources = pool.entries().cloned().collect::<Vec<_>>();
            for (source_index, source) in sources.iter().enumerate() {
                if phase_checkpoint(input, control)? {
                    timed_out = true;
                    break;
                }
                let outcome = insert_unplaced_parts(
                    input,
                    &problem,
                    source,
                    control,
                    &mut iteration,
                    source_index as u64,
                )?;
                if outcome.timed_out {
                    timed_out = true;
                    break;
                }
                if let Some(candidate) = outcome.candidate {
                    admit_candidate(candidate, &mut pool, &mut incumbent, control, iteration)?;
                } else {
                    control.report_heartbeat(iteration);
                }
            }
        }

        if exploration_passes > 0 && !timed_out {
            for neighborhood in 0..exploration_passes {
                if phase_checkpoint(input, control)? {
                    timed_out = true;
                    break;
                }
                let sources = pool.entries().cloned().collect::<Vec<_>>();
                if sources.is_empty() {
                    break;
                }
                let source_index = (derive_phase_seed(
                    input.options.random_seed,
                    OptimizerPhase::Ejection,
                    neighborhood,
                    0,
                ) % sources.len() as u64) as usize;
                let source = &sources[source_index];
                let target = ordered_unplaced_indices(
                    input,
                    &problem,
                    source.placements(),
                    OptimizerPhase::Ejection,
                    neighborhood,
                )
                .into_iter()
                .next();
                if let Some(target_index) = target {
                    let outcome = bounded_ejection_insert(
                        input,
                        &problem,
                        source,
                        target_index,
                        MAX_EJECTION_WIDTH,
                        control,
                        &mut iteration,
                        neighborhood,
                    )?;
                    if outcome.timed_out {
                        timed_out = true;
                        break;
                    }
                    if let Some(candidate) = outcome.candidate {
                        admit_candidate(candidate, &mut pool, &mut incumbent, control, iteration)?;
                    } else {
                        control.report_heartbeat(iteration);
                    }
                }

                if phase_checkpoint(input, control)? {
                    timed_out = true;
                    break;
                }
                let sources = pool.entries().cloned().collect::<Vec<_>>();
                let source_index = (derive_phase_seed(
                    input.options.random_seed,
                    OptimizerPhase::RuinRecreate,
                    neighborhood,
                    0,
                ) % sources.len() as u64) as usize;
                let source = &sources[source_index];
                let placed_count = source.rank().placed_count() as usize;
                let ruin_width = placed_count
                    .min(MAX_RUIN_WIDTH)
                    .max(1)
                    .min(1 + (neighborhood % MAX_RUIN_WIDTH as u64) as usize);
                let outcome = ruin_and_recreate(
                    input,
                    &problem,
                    source,
                    ruin_width,
                    control,
                    &mut iteration,
                    neighborhood,
                )?;
                if outcome.timed_out {
                    timed_out = true;
                    break;
                }
                if let Some(candidate) = outcome.candidate {
                    admit_candidate(candidate, &mut pool, &mut incumbent, control, iteration)?;
                } else {
                    control.report_heartbeat(iteration);
                }
            }
        }

        if !timed_out {
            for move_index in 0..refinement_moves {
                if phase_checkpoint(input, control)? {
                    break;
                }
                let Some(part_index) = refinement_part_index(
                    incumbent.best().placements(),
                    input.options.random_seed,
                    move_index,
                ) else {
                    break;
                };
                if let Some(candidate) = relocate_part(
                    input,
                    &problem,
                    incumbent.best(),
                    part_index,
                    control,
                    &mut iteration,
                    derive_phase_seed(
                        input.options.random_seed,
                        OptimizerPhase::Refinement,
                        move_index as u64,
                        input.parts[part_index].id,
                    ),
                )? {
                    if phase_checkpoint(input, control)? {
                        break;
                    }
                    admit_candidate(candidate, &mut pool, &mut incumbent, control, iteration)?;
                } else {
                    control.report_heartbeat(iteration);
                }
            }
        }

        Ok(())
    })?;

    debug_assert!(
        !incumbent
            .initial()
            .rank()
            .is_better_than(incumbent.best().rank())
    );
    if !recovered_panic {
        debug_assert_eq!(
            pool.best()
                .map(RankedSolution::placements)
                .unwrap_or_default(),
            incumbent.best().placements()
        );
    }
    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    Ok(incumbent.into_best())
}

fn contain_optimizer_panic<F>(control: &RunControl<'_>, operation: F) -> Result<bool, SolverError>
where
    F: FnOnce() -> Result<(), SolverError>,
{
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(result) => {
            result?;
            Ok(false)
        }
        Err(_) => match control.stop_error() {
            Some(error) => Err(error),
            None => Ok(true),
        },
    }
}

fn admit_candidate(
    candidate: RankedSolution,
    pool: &mut SolutionPool,
    incumbent: &mut CanonicalIncumbent,
    control: &RunControl<'_>,
    iteration: u64,
) -> Result<bool, SolverError> {
    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    pool.insert(candidate.clone());
    let improved = incumbent.consider(candidate);
    if improved {
        report_incumbent(control, iteration, incumbent)?;
    } else {
        control.report_heartbeat(iteration);
    }
    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    Ok(improved)
}

fn report_incumbent(
    control: &RunControl<'_>,
    iteration: u64,
    incumbent: &CanonicalIncumbent,
) -> Result<(), SolverError> {
    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    control.report_best(
        VacNestingProgress {
            iteration,
            best_score: incumbent.best().rank().canonical_area_score(),
            placed_count: incumbent.best().rank().placed_count(),
            stage: PROGRESS_SOLVING,
            ..VacNestingProgress::default()
        },
        incumbent.best().placements(),
    );
    if let Some(error) = control.stop_error() {
        return Err(error);
    }
    Ok(())
}

fn empty_results(input: &SolverInput) -> Vec<VacNestingPlacement> {
    input
        .parts
        .iter()
        .map(|part| VacNestingPlacement {
            part_id: part.id,
            ..VacNestingPlacement::default()
        })
        .collect()
}

/// Clearance added between grid cells so float32 contact is never reported
/// as a collision, in document units.
const GRID_SEED_GAP: f32 = 1.0e-3;

/// Deterministic seed for the common card-sheet case: every part shares one
/// collision shape and the usable container is an axis-aligned rectangle
/// without holes. Lays out the inflated bounding boxes in one or two
/// orthogonal blocks and keeps the densest arrangement. The seed is only a
/// candidate: it must pass the authoritative feasibility check and then
/// competes with every constructive pass under the canonical rank.
fn identical_part_grid_seed(
    input: &SolverInput,
    problem: &PreparedProblem,
    control: &RunControl<'_>,
) -> Result<Option<RankedSolution>, SolverError> {
    let Some(first) = problem.items.first() else {
        return Ok(None);
    };
    // Identical parts share every collision shape (NEST-S2), component by
    // component, so pointer identity also proves the same relative layout.
    if !input.holes.is_empty()
        || !input.obstacles.is_empty()
        || problem.items.iter().any(|item| {
            item.components.len() != first.components.len()
                || item
                    .components
                    .iter()
                    .zip(&first.components)
                    .any(|(left, right)| !std::sync::Arc::ptr_eq(&left.shape_cd, &right.shape_cd))
        })
    {
        return Ok(None);
    }
    let outer = &problem.container.outer_cd;
    let area = outer.bbox;
    if outer.n_vertices() != 4
        || outer.vertices.iter().any(|point| {
            let on_x =
                (point.0 - area.x_min).abs() < 1.0e-4 || (point.0 - area.x_max).abs() < 1.0e-4;
            let on_y =
                (point.1 - area.y_min).abs() < 1.0e-4 || (point.1 - area.y_max).abs() < 1.0e-4;
            !(on_x && on_y)
        })
    {
        return Ok(None);
    }
    let right_angle = |angle: f32| angle.abs() < 1.0e-6 || (angle - FRAC_PI_2).abs() < 1.0e-6;
    let angles = match &first.allowed_rotation {
        RotationRange::None => vec![0.0],
        RotationRange::Continuous => vec![0.0, FRAC_PI_2],
        RotationRange::Discrete(values) => {
            values.iter().copied().filter(|a| right_angle(*a)).collect()
        }
    };
    // (angle, cell width, cell height, rotated bbox) per usable orientation.
    let cells = angles
        .into_iter()
        .map(|angle| {
            let rotation = DTransformation::new(angle, (0.0, 0.0)).compose();
            let bbox = first
                .components
                .iter()
                .map(|component| component.shape_cd.transform_clone(&rotation).bbox)
                .reduce(union_rect)
                .expect("compound item has at least one component");
            (
                angle,
                bbox.width() + GRID_SEED_GAP,
                bbox.height() + GRID_SEED_GAP,
                bbox,
            )
        })
        .collect::<Vec<_>>();
    // n cells need n * (cell + gap) <= extent - gap: a gap on every side,
    // including the container boundary, keeps float32 replay from reporting
    // exact contact as a collision.
    let (width, height) = (area.width() - GRID_SEED_GAP, area.height() - GRID_SEED_GAP);
    let fit = |extent: f32, cell: f32| (extent / cell).floor().max(0.0) as usize;

    // Each block is (orientation index, x0, y0, columns, rows).
    let mut best: Vec<(usize, f32, f32, usize, usize)> = Vec::new();
    let count = |blocks: &[(usize, f32, f32, usize, usize)]| {
        blocks.iter().map(|block| block.3 * block.4).sum::<usize>()
    };
    for (a, &(_, wa, ha, _)) in cells.iter().enumerate() {
        let single = vec![(a, 0.0, 0.0, fit(width, wa), fit(height, ha))];
        if count(&single) > count(&best) {
            best = single;
        }
        for (b, &(_, wb, hb, _)) in cells.iter().enumerate() {
            if a == b {
                continue;
            }
            for columns in 0..=fit(width, wa) {
                let used = columns as f32 * wa;
                let split = vec![
                    (a, 0.0, 0.0, columns, fit(height, ha)),
                    (b, used, 0.0, fit(width - used, wb), fit(height, hb)),
                ];
                if count(&split) > count(&best) {
                    best = split;
                }
            }
            for rows in 0..=fit(height, ha) {
                let used = rows as f32 * ha;
                let split = vec![
                    (a, 0.0, 0.0, fit(width, wa), rows),
                    (b, 0.0, used, fit(width, wb), fit(height - used, hb)),
                ];
                if count(&split) > count(&best) {
                    best = split;
                }
            }
        }
    }
    if count(&best) == 0 {
        return Ok(None);
    }

    let mut results = empty_results(input);
    let mut next = 0_usize;
    'blocks: for &(orientation, x0, y0, columns, rows) in &best {
        let (angle, cell_width, cell_height, bbox) = cells[orientation];
        for column in 0..columns {
            for row in 0..rows {
                if next == results.len() {
                    break 'blocks;
                }
                let x = area.x_min + GRID_SEED_GAP + x0 + column as f32 * cell_width;
                let y = area.y_min + GRID_SEED_GAP + y0 + row as f32 * cell_height;
                let transform = DTransformation::new(angle, (x - bbox.x_min, y - bbox.y_min));
                results[next] =
                    placement_from_internal(input.parts[next].id, &problem.items[next], transform)?;
                next += 1;
            }
        }
    }
    if !solution_is_feasible(input, problem, &results, control)? {
        return Ok(None);
    }
    Ok(RankedSolution::new(input, results, true))
}

fn construct_pass(
    input: &SolverInput,
    problem: &PreparedProblem,
    control: &RunControl<'_>,
    iteration: &mut u64,
    pass: u64,
) -> Result<PassOutcome, SolverError> {
    let mut layout = Layout::new(problem.container.clone());
    let mut results = empty_results(input);
    let order = order_for_pass(&problem.items, input.options.random_seed, pass);
    let mut placed_count = 0_u32;
    let mut timed_out = false;

    for index in order {
        if let Some(error) = control.stop_error() {
            return Err(error);
        }
        if control.time_limit_reached(input.options.time_limit_ms) {
            timed_out = true;
            break;
        }

        let outcome = search_item(
            &layout,
            &problem.items[index],
            input,
            problem.profile,
            control,
            iteration,
            pass,
        )?;

        if let Some(candidate) = outcome.best {
            results[index] = placement_from_internal(
                input.parts[index].id,
                &problem.items[index],
                candidate.transform,
            )?;
            for component in &problem.items[index].components {
                layout.place_item(component, candidate.transform);
            }
            placed_count = placed_count.saturating_add(1);
        }

        control.report_heartbeat(*iteration);
        if outcome.timed_out {
            timed_out = true;
            break;
        }
    }

    let solution = RankedSolution::new(input, results, true).ok_or_else(|| {
        SolverError::Failed("constructive backend produced malformed results".to_owned())
    })?;
    Ok(PassOutcome {
        solution,
        timed_out,
    })
}

fn prepare_problem(
    input: &SolverInput,
    control: &RunControl<'_>,
) -> Result<PreparedProblem, SolverError> {
    prepare_problem_impl(input, control, false)
}

fn prepare_problem_for_optimizer(
    input: &SolverInput,
    control: &RunControl<'_>,
) -> Result<Option<PreparedProblem>, SolverError> {
    match prepare_problem_impl(input, control, true) {
        Ok(problem) => Ok(Some(problem)),
        Err(SolverError::DeadlineReached) => Ok(None),
        Err(error) => Err(error),
    }
}

fn prepare_problem_impl(
    input: &SolverInput,
    control: &RunControl<'_>,
    enforce_deadline: bool,
) -> Result<PreparedProblem, SolverError> {
    preparation_checkpoint(input, control, enforce_deadline)?;
    let profile = SolverProfile::from_quality(input.options.quality);
    let container = build_container(input, profile, control, enforce_deadline)?;
    preparation_checkpoint(input, control, enforce_deadline)?;
    let items = build_items(input, profile, control, enforce_deadline)?;
    preparation_checkpoint(input, control, enforce_deadline)?;
    Ok(PreparedProblem {
        profile,
        container,
        items,
    })
}

fn preparation_checkpoint(
    input: &SolverInput,
    control: &RunControl<'_>,
    enforce_deadline: bool,
) -> Result<(), SolverError> {
    if let Some(error) = control.stop_error() {
        Err(error)
    } else if enforce_deadline && control.time_limit_reached(input.options.time_limit_ms) {
        Err(SolverError::DeadlineReached)
    } else {
        Ok(())
    }
}

fn placement_from_internal(
    part_id: u64,
    item: &CompoundItem,
    transform: DTransformation,
) -> Result<VacNestingPlacement, SolverError> {
    let external =
        int_to_ext_transformation(&transform, &item.components[0].shape_orig.pre_transform);
    let (translation_x, translation_y) = external.translation();
    let mut rotation_degrees = external.rotation().to_degrees().rem_euclid(360.0);
    if !translation_x.is_finite() || !translation_y.is_finite() || !rotation_degrees.is_finite() {
        return Err(SolverError::Failed(
            "nesting engine produced a non-finite placement".to_owned(),
        ));
    }
    if rotation_degrees.abs() < 1.0e-5 || (360.0 - rotation_degrees).abs() < 1.0e-5 {
        rotation_degrees = 0.0;
    }
    Ok(VacNestingPlacement {
        part_id,
        translation_x: f64::from(translation_x),
        translation_y: f64::from(translation_y),
        rotation_degrees: f64::from(rotation_degrees),
        placed: 1,
        ..VacNestingPlacement::default()
    })
}

fn refinement_part_index(
    placements: &[VacNestingPlacement],
    seed: u64,
    move_index: usize,
) -> Option<usize> {
    let mut placed = placements
        .iter()
        .enumerate()
        .filter_map(|(index, placement)| (placement.placed != 0).then_some(index))
        .collect::<Vec<_>>();
    if placed.is_empty() {
        return None;
    }
    placed.sort_unstable_by_key(|index| placements[*index].part_id);
    let start = (splitmix64(seed ^ 0x7265_6669_6e65_0001) % placed.len() as u64) as usize;
    Some(placed[(start + move_index % placed.len()) % placed.len()])
}

#[allow(clippy::too_many_arguments)]
fn relocate_part(
    input: &SolverInput,
    problem: &PreparedProblem,
    source: &RankedSolution,
    part_index: usize,
    control: &RunControl<'_>,
    iteration: &mut u64,
    pass: u64,
) -> Result<Option<RankedSolution>, SolverError> {
    if source.placements()[part_index].placed == 0 {
        return Ok(None);
    }

    let mut layout = Layout::new(problem.container.clone());
    for (index, placement) in source.placements().iter().enumerate() {
        if index == part_index || placement.placed == 0 {
            continue;
        }
        if phase_checkpoint(input, control)? {
            return Ok(None);
        }
        let Some(transform) = external_to_internal(&problem.items[index], placement) else {
            return Ok(None);
        };
        if !place_compound_exact(&mut layout, &problem.items[index], transform) {
            return Ok(None);
        }
        if phase_checkpoint(input, control)? {
            return Ok(None);
        }
    }

    let outcome = search_item(
        &layout,
        &problem.items[part_index],
        input,
        problem.profile,
        control,
        iteration,
        pass,
    )?;
    let Some(candidate) = outcome.best else {
        return Ok(None);
    };
    let mut results = source.placements().to_vec();
    results[part_index] = placement_from_internal(
        input.parts[part_index].id,
        &problem.items[part_index],
        candidate.transform,
    )?;
    let validation = solution_is_feasible_with_deadline(input, problem, &results, control)?;
    if validation.timed_out {
        return Ok(None);
    }
    Ok(RankedSolution::new(input, results, validation.feasible))
}

fn insert_unplaced_parts(
    input: &SolverInput,
    problem: &PreparedProblem,
    source: &RankedSolution,
    control: &RunControl<'_>,
    iteration: &mut u64,
    neighborhood: u64,
) -> Result<NeighborhoodOutcome, SolverError> {
    if phase_checkpoint(input, control)? {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: true,
        });
    }
    let build = rebuild_layout_from_placements(input, problem, source.placements(), control)?;
    let Some(mut layout) = build.layout else {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: build.timed_out,
        });
    };
    let mut results = source.placements().to_vec();
    let order = ordered_unplaced_indices(
        input,
        problem,
        &results,
        OptimizerPhase::Insertion,
        neighborhood,
    );
    if order.is_empty() {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: false,
        });
    }
    let pass = derive_phase_seed(
        input.options.random_seed,
        OptimizerPhase::Insertion,
        neighborhood,
        0,
    );
    if insert_indices_into_layout(
        input,
        problem,
        &mut layout,
        &mut results,
        &order,
        control,
        iteration,
        pass,
    )? {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: true,
        });
    }
    finalize_transaction(input, problem, source, results, control)
}

#[allow(clippy::too_many_arguments)]
fn bounded_ejection_insert(
    input: &SolverInput,
    problem: &PreparedProblem,
    source: &RankedSolution,
    target_index: usize,
    max_ejections: usize,
    control: &RunControl<'_>,
    iteration: &mut u64,
    neighborhood: u64,
) -> Result<NeighborhoodOutcome, SolverError> {
    if target_index >= input.parts.len()
        || source.placements()[target_index].placed != 0
        || max_ejections == 0
    {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: phase_checkpoint(input, control)?,
        });
    }
    if phase_checkpoint(input, control)? {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: true,
        });
    }

    let mut ejection_order = source
        .placements()
        .iter()
        .enumerate()
        .filter_map(|(index, placement)| {
            (index != target_index && placement.placed != 0).then_some(index)
        })
        .collect::<Vec<_>>();
    ejection_order.sort_by(|left, right| {
        problem.items[*left]
            .area
            .total_cmp(&problem.items[*right].area)
            .then_with(|| {
                derive_phase_seed(
                    input.options.random_seed,
                    OptimizerPhase::Ejection,
                    neighborhood,
                    input.parts[*left].id,
                )
                .cmp(&derive_phase_seed(
                    input.options.random_seed,
                    OptimizerPhase::Ejection,
                    neighborhood,
                    input.parts[*right].id,
                ))
            })
            .then_with(|| input.parts[*left].id.cmp(&input.parts[*right].id))
    });

    let mut best = None;
    for width in 1..=max_ejections
        .min(MAX_EJECTION_WIDTH)
        .min(ejection_order.len())
    {
        if phase_checkpoint(input, control)? {
            return Ok(NeighborhoodOutcome {
                candidate: None,
                timed_out: true,
            });
        }
        let ejected = &ejection_order[..width];
        let mut results = source.placements().to_vec();
        for &index in ejected {
            mark_unplaced(input, &mut results, index);
        }

        let build = rebuild_layout_from_placements(input, problem, &results, control)?;
        let Some(mut layout) = build.layout else {
            if build.timed_out {
                return Ok(NeighborhoodOutcome {
                    candidate: None,
                    timed_out: true,
                });
            }
            continue;
        };
        let pass = derive_phase_seed(
            input.options.random_seed,
            OptimizerPhase::Ejection,
            neighborhood,
            width as u64,
        );
        if insert_indices_into_layout(
            input,
            problem,
            &mut layout,
            &mut results,
            &[target_index],
            control,
            iteration,
            pass,
        )? {
            return Ok(NeighborhoodOutcome {
                candidate: None,
                timed_out: true,
            });
        }
        if results[target_index].placed == 0 {
            continue;
        }

        let mut recreate = ejected.to_vec();
        recreate.reverse();
        if insert_indices_into_layout(
            input,
            problem,
            &mut layout,
            &mut results,
            &recreate,
            control,
            iteration,
            pass.rotate_left(23),
        )? {
            return Ok(NeighborhoodOutcome {
                candidate: None,
                timed_out: true,
            });
        }
        let outcome = finalize_transaction(input, problem, source, results, control)?;
        if outcome.timed_out {
            return Ok(NeighborhoodOutcome {
                candidate: None,
                timed_out: true,
            });
        }
        if let Some(candidate) = outcome.candidate
            && best.as_ref().is_none_or(|current: &RankedSolution| {
                candidate.rank().is_better_than(current.rank())
            })
        {
            best = Some(candidate);
        }
    }

    Ok(NeighborhoodOutcome {
        candidate: best,
        timed_out: false,
    })
}

#[allow(clippy::too_many_arguments)]
fn ruin_and_recreate(
    input: &SolverInput,
    problem: &PreparedProblem,
    source: &RankedSolution,
    ruin_width: usize,
    control: &RunControl<'_>,
    iteration: &mut u64,
    neighborhood: u64,
) -> Result<NeighborhoodOutcome, SolverError> {
    if phase_checkpoint(input, control)? {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: true,
        });
    }
    let mut ruined = source
        .placements()
        .iter()
        .enumerate()
        .filter_map(|(index, placement)| (placement.placed != 0).then_some(index))
        .collect::<Vec<_>>();
    ruined.sort_by_key(|index| {
        (
            derive_phase_seed(
                input.options.random_seed,
                OptimizerPhase::RuinRecreate,
                neighborhood,
                input.parts[*index].id,
            ),
            input.parts[*index].id,
        )
    });
    ruined.truncate(ruin_width.min(MAX_RUIN_WIDTH).min(ruined.len()));
    if ruined.is_empty() {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: false,
        });
    }

    let mut results = source.placements().to_vec();
    for index in ruined {
        mark_unplaced(input, &mut results, index);
    }
    let build = rebuild_layout_from_placements(input, problem, &results, control)?;
    let Some(mut layout) = build.layout else {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: build.timed_out,
        });
    };
    let recreate = ordered_unplaced_indices(
        input,
        problem,
        &results,
        OptimizerPhase::RuinRecreate,
        neighborhood,
    );
    let pass = derive_phase_seed(
        input.options.random_seed,
        OptimizerPhase::RuinRecreate,
        neighborhood,
        0,
    );
    if insert_indices_into_layout(
        input,
        problem,
        &mut layout,
        &mut results,
        &recreate,
        control,
        iteration,
        pass,
    )? {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: true,
        });
    }
    finalize_transaction(input, problem, source, results, control)
}

fn rebuild_layout_from_placements(
    input: &SolverInput,
    problem: &PreparedProblem,
    placements: &[VacNestingPlacement],
    control: &RunControl<'_>,
) -> Result<LayoutBuildOutcome, SolverError> {
    if !CanonicalRank::evaluate(input, placements, true).is_valid() {
        return Ok(LayoutBuildOutcome {
            layout: None,
            timed_out: false,
        });
    }
    let mut layout = Layout::new(problem.container.clone());
    for (index, placement) in placements.iter().enumerate() {
        if phase_checkpoint(input, control)? {
            return Ok(LayoutBuildOutcome {
                layout: None,
                timed_out: true,
            });
        }
        if placement.placed == 0 {
            continue;
        }
        if !rotation_is_allowed(
            &problem.items[index].allowed_rotation,
            placement.rotation_degrees,
        ) {
            return Ok(LayoutBuildOutcome {
                layout: None,
                timed_out: false,
            });
        }
        let Some(transform) = external_to_internal(&problem.items[index], placement) else {
            return Ok(LayoutBuildOutcome {
                layout: None,
                timed_out: false,
            });
        };
        if !place_compound_exact(&mut layout, &problem.items[index], transform) {
            return Ok(LayoutBuildOutcome {
                layout: None,
                timed_out: false,
            });
        }
        if phase_checkpoint(input, control)? {
            return Ok(LayoutBuildOutcome {
                layout: None,
                timed_out: true,
            });
        }
    }
    Ok(LayoutBuildOutcome {
        layout: Some(layout),
        timed_out: false,
    })
}

#[allow(clippy::too_many_arguments)]
fn insert_indices_into_layout(
    input: &SolverInput,
    problem: &PreparedProblem,
    layout: &mut Layout,
    results: &mut [VacNestingPlacement],
    indices: &[usize],
    control: &RunControl<'_>,
    iteration: &mut u64,
    pass: u64,
) -> Result<bool, SolverError> {
    for &index in indices {
        if results[index].placed != 0 {
            continue;
        }
        if phase_checkpoint(input, control)? {
            return Ok(true);
        }
        let outcome = search_item(
            layout,
            &problem.items[index],
            input,
            problem.profile,
            control,
            iteration,
            pass,
        )?;
        if outcome.timed_out {
            return Ok(true);
        }
        if let Some(candidate) = outcome.best {
            results[index] = placement_from_internal(
                input.parts[index].id,
                &problem.items[index],
                candidate.transform,
            )?;
            for component in &problem.items[index].components {
                layout.place_item(component, candidate.transform);
            }
        }
        control.report_heartbeat(*iteration);
    }
    Ok(false)
}

fn finalize_transaction(
    input: &SolverInput,
    problem: &PreparedProblem,
    source: &RankedSolution,
    results: Vec<VacNestingPlacement>,
    control: &RunControl<'_>,
) -> Result<NeighborhoodOutcome, SolverError> {
    if results == source.placements() {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: false,
        });
    }
    if phase_checkpoint(input, control)? {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: true,
        });
    }
    let validation = solution_is_feasible_with_deadline(input, problem, &results, control)?;
    if validation.timed_out || phase_checkpoint(input, control)? {
        return Ok(NeighborhoodOutcome {
            candidate: None,
            timed_out: true,
        });
    }
    let candidate = RankedSolution::new(input, results, validation.feasible)
        .filter(|candidate| candidate.rank().is_better_than(source.rank()));
    Ok(NeighborhoodOutcome {
        candidate,
        timed_out: false,
    })
}

fn mark_unplaced(input: &SolverInput, results: &mut [VacNestingPlacement], index: usize) {
    results[index] = VacNestingPlacement {
        part_id: input.parts[index].id,
        ..VacNestingPlacement::default()
    };
}

fn solution_is_feasible(
    input: &SolverInput,
    problem: &PreparedProblem,
    results: &[VacNestingPlacement],
    control: &RunControl<'_>,
) -> Result<bool, SolverError> {
    let outcome = solution_is_feasible_impl(input, problem, results, control, false)?;
    debug_assert!(!outcome.timed_out);
    Ok(outcome.feasible)
}

fn solution_is_feasible_with_deadline(
    input: &SolverInput,
    problem: &PreparedProblem,
    results: &[VacNestingPlacement],
    control: &RunControl<'_>,
) -> Result<ValidationOutcome, SolverError> {
    solution_is_feasible_impl(input, problem, results, control, true)
}

fn solution_is_feasible_impl(
    input: &SolverInput,
    problem: &PreparedProblem,
    results: &[VacNestingPlacement],
    control: &RunControl<'_>,
    enforce_deadline: bool,
) -> Result<ValidationOutcome, SolverError> {
    if validation_checkpoint(input, control, enforce_deadline)? {
        return Ok(ValidationOutcome {
            feasible: false,
            timed_out: true,
        });
    }
    if !CanonicalRank::evaluate(input, results, true).is_valid() {
        return Ok(ValidationOutcome {
            feasible: false,
            timed_out: false,
        });
    }
    if validation_checkpoint(input, control, enforce_deadline)? {
        return Ok(ValidationOutcome {
            feasible: false,
            timed_out: true,
        });
    }
    let mut layout = Layout::new(problem.container.clone());
    for (index, placement) in results.iter().enumerate() {
        if validation_checkpoint(input, control, enforce_deadline)? {
            return Ok(ValidationOutcome {
                feasible: false,
                timed_out: true,
            });
        }
        if placement.placed == 0 {
            continue;
        }
        if !rotation_is_allowed(
            &problem.items[index].allowed_rotation,
            placement.rotation_degrees,
        ) {
            return Ok(ValidationOutcome {
                feasible: false,
                timed_out: false,
            });
        }
        let Some(transform) = external_to_internal(&problem.items[index], placement) else {
            return Ok(ValidationOutcome {
                feasible: false,
                timed_out: false,
            });
        };
        if validation_checkpoint(input, control, enforce_deadline)? {
            return Ok(ValidationOutcome {
                feasible: false,
                timed_out: true,
            });
        }
        // Jagua 0.8 does not expose a cancellable collision primitive. These
        // checkpoints bracket each rigid-compound replay; cancellation/deadline
        // latency can therefore include at most one in-flight Jagua call.
        if !place_compound_exact(&mut layout, &problem.items[index], transform) {
            return Ok(ValidationOutcome {
                feasible: false,
                timed_out: false,
            });
        }
        if validation_checkpoint(input, control, enforce_deadline)? {
            return Ok(ValidationOutcome {
                feasible: false,
                timed_out: true,
            });
        }
    }
    Ok(ValidationOutcome {
        feasible: true,
        timed_out: false,
    })
}

fn validation_checkpoint(
    input: &SolverInput,
    control: &RunControl<'_>,
    enforce_deadline: bool,
) -> Result<bool, SolverError> {
    if let Some(error) = control.stop_error() {
        Err(error)
    } else {
        Ok(enforce_deadline && control.time_limit_reached(input.options.time_limit_ms))
    }
}

fn external_to_internal(
    item: &CompoundItem,
    placement: &VacNestingPlacement,
) -> Option<DTransformation> {
    let rotation = placement.rotation_degrees.to_radians() as f32;
    let translation = (
        placement.translation_x as f32,
        placement.translation_y as f32,
    );
    if !rotation.is_finite() || !translation.0.is_finite() || !translation.1.is_finite() {
        return None;
    }
    let external = DTransformation::new(rotation, translation);
    let internal =
        ext_to_int_transformation(&external, &item.components[0].shape_orig.pre_transform);
    let internal_translation = internal.translation();
    (internal.rotation().is_finite()
        && internal_translation.0.is_finite()
        && internal_translation.1.is_finite())
    .then_some(internal)
}

fn rotation_is_allowed(range: &RotationRange, degrees: f64) -> bool {
    let angle = degrees.to_radians().rem_euclid(f64::from(TAU));
    let tolerance = 1.0e-4_f64;
    match range {
        RotationRange::None => circular_distance(angle, 0.0) <= tolerance,
        RotationRange::Continuous => true,
        RotationRange::Discrete(rotations) => rotations
            .iter()
            .any(|allowed| circular_distance(angle, f64::from(*allowed)) <= tolerance),
    }
}

fn circular_distance(left: f64, right: f64) -> f64 {
    let distance = (left - right).abs().rem_euclid(f64::from(TAU));
    distance.min(f64::from(TAU) - distance)
}

fn place_compound_exact(
    layout: &mut Layout,
    item: &CompoundItem,
    transform: DTransformation,
) -> bool {
    let composed = transform.compose();
    let transformed = item
        .components
        .iter()
        .map(|component| component.shape_cd.transform_clone(&composed))
        .collect::<Vec<_>>();
    if transformed
        .iter()
        .any(|shape| layout.cde().detect_poly_collision(shape, &NoFilter))
    {
        return false;
    }
    for component in &item.components {
        layout.place_item(component, transform);
    }
    true
}

fn order_for_pass(items: &[CompoundItem], seed: u64, pass: u64) -> Vec<usize> {
    let mut order = (0..items.len()).collect::<Vec<_>>();
    // Input positions identify result slots, never search identity. Both the
    // standalone fallback and portfolio must follow the same stable-ID path.
    let stable_tie = |left: &usize, right: &usize| items[*left].part_id.cmp(&items[*right].part_id);
    match pass % 4 {
        0 => order.sort_by(|left, right| {
            items[*right]
                .diameter
                .total_cmp(&items[*left].diameter)
                .then_with(|| items[*right].area.total_cmp(&items[*left].area))
                .then_with(|| stable_tie(left, right))
        }),
        1 => order.sort_by(|left, right| {
            items[*right]
                .area
                .total_cmp(&items[*left].area)
                .then_with(|| items[*right].diameter.total_cmp(&items[*left].diameter))
                .then_with(|| stable_tie(left, right))
        }),
        2 => {
            order.sort_by_key(|index| items[*index].part_id);
            if !order.is_empty() {
                let length = order.len();
                let offset = ((seed.wrapping_add(pass) % length as u64) as usize).max(1);
                order.rotate_left(offset % length);
            }
        }
        _ => order.sort_by_key(|index| {
            let identity = items[*index].part_id;
            (splitmix64(seed ^ pass.rotate_left(17) ^ identity), identity)
        }),
    }
    order
}

fn splitmix64(mut value: u64) -> u64 {
    value = value.wrapping_add(0x9e37_79b9_7f4a_7c15);
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

fn derive_phase_seed(
    master_seed: u64,
    phase: OptimizerPhase,
    neighborhood: u64,
    stable_part_id: u64,
) -> u64 {
    splitmix64(
        master_seed
            ^ phase as u64
            ^ splitmix64(neighborhood.rotate_left(17))
            ^ splitmix64(stable_part_id.rotate_left(31)),
    )
}

fn phase_checkpoint(input: &SolverInput, control: &RunControl<'_>) -> Result<bool, SolverError> {
    if let Some(error) = control.stop_error() {
        Err(error)
    } else {
        Ok(control.time_limit_reached(input.options.time_limit_ms))
    }
}

fn ordered_unplaced_indices(
    input: &SolverInput,
    problem: &PreparedProblem,
    placements: &[VacNestingPlacement],
    phase: OptimizerPhase,
    neighborhood: u64,
) -> Vec<usize> {
    let mut indices = placements
        .iter()
        .enumerate()
        .filter_map(|(index, placement)| (placement.placed == 0).then_some(index))
        .collect::<Vec<_>>();
    indices.sort_by(|left, right| {
        problem.items[*right]
            .area
            .total_cmp(&problem.items[*left].area)
            .then_with(|| {
                derive_phase_seed(
                    input.options.random_seed,
                    phase,
                    neighborhood,
                    input.parts[*left].id,
                )
                .cmp(&derive_phase_seed(
                    input.options.random_seed,
                    phase,
                    neighborhood,
                    input.parts[*right].id,
                ))
            })
            .then_with(|| input.parts[*left].id.cmp(&input.parts[*right].id))
    });
    indices
}

fn build_container(
    input: &SolverInput,
    profile: SolverProfile,
    control: &RunControl<'_>,
    enforce_deadline: bool,
) -> Result<Container, SolverError> {
    preparation_checkpoint(input, control, enforce_deadline)?;
    let cde_config = profile.cde_config();
    // The sheet margin is the only clearance to the container and its holes;
    // part spacing applies between parts. Every part envelope is already
    // inflated by spacing / 2, so the hazards move by margin - spacing / 2.
    let clearance = input.options.container_margin - input.options.part_spacing / 2.0;
    let outer = polygon_to_simple(&input.container, "container")?;
    preparation_checkpoint(input, control, enforce_deadline)?;
    let original_outer = if clearance >= 0.0 {
        OriginalShape {
            shape: outer,
            pre_transform: DTransformation::empty(),
            modify_mode: ShapeModifyMode::Deflate,
            modify_config: modify_config(clearance, profile.simplify_tolerance),
        }
    } else {
        // Growing the container is exact only for convex outlines, where the
        // mitered expansion eroded by spacing / 2 is the outline eroded by the
        // margin. A grown concave outline would close notches narrower than
        // the spacing, so it keeps its exact shape: the wall is then
        // spacing / 2, still never closer than the margin.
        let shape = expand_convex(&outer, -clearance).unwrap_or(outer);
        OriginalShape {
            shape,
            pre_transform: DTransformation::empty(),
            modify_mode: ShapeModifyMode::Deflate,
            modify_config: modify_config(0.0, None),
        }
    };
    // Holes are never eroded: an eroded hole can lose thin features.
    let hole_clearance = clearance.max(0.0);

    let quality_zones = if input.holes.is_empty() && input.obstacles.is_empty() {
        Vec::new()
    } else {
        let mut hazards = input
            .holes
            .iter()
            .enumerate()
            .map(|(index, polygon)| {
                preparation_checkpoint(input, control, enforce_deadline)?;
                let shape = polygon_to_simple(polygon, &format!("container hole {index}"))?;
                preparation_checkpoint(input, control, enforce_deadline)?;
                Ok(OriginalShape {
                    shape,
                    pre_transform: DTransformation::empty(),
                    modify_mode: ShapeModifyMode::Inflate,
                    modify_config: modify_config(hole_clearance, profile.simplify_tolerance),
                })
            })
            .collect::<Result<Vec<_>, SolverError>>()?;
        for (index, polygon) in input.obstacles.iter().enumerate() {
            preparation_checkpoint(input, control, enforce_deadline)?;
            hazards.push(obstacle_hazard(polygon, index, input, profile)?);
            preparation_checkpoint(input, control, enforce_deadline)?;
        }
        preparation_checkpoint(input, control, enforce_deadline)?;
        let zone = InferiorQualityZone::new(0, hazards).map_err(|error| {
            SolverError::InvalidInput(format!(
                "container holes or obstacles are invalid: {error:#}"
            ))
        })?;
        preparation_checkpoint(input, control, enforce_deadline)?;
        vec![zone]
    };

    preparation_checkpoint(input, control, enforce_deadline)?;
    let container =
        Container::new(0, original_outer, quality_zones, cde_config).map_err(|error| {
            SolverError::InvalidInput(format!(
                "container cannot be prepared for nesting: {error:#}"
            ))
        })?;
    preparation_checkpoint(input, control, enforce_deadline)?;
    Ok(container)
}

/// A fixed obstacle as a container hazard. Parts are inflated by
/// `part_spacing / 2`, so the obstacle gets the same envelope as a placed
/// part (`part_spacing / 2`, through the same bounded-cost conservative
/// offset as parts). The hazard is handed over already inflated and is not
/// modified again, so the margin never applies to obstacles.
fn obstacle_hazard(
    polygon: &Polygon,
    index: usize,
    input: &SolverInput,
    profile: SolverProfile,
) -> Result<OriginalShape, SolverError> {
    let shape = polygon_to_simple(polygon, &format!("obstacle {index}"))?;
    let envelope = OriginalShape {
        shape,
        pre_transform: DTransformation::empty(),
        modify_mode: ShapeModifyMode::Inflate,
        modify_config: modify_config(input.options.part_spacing / 2.0, profile.simplify_tolerance),
    };
    let inflated = part_collision_shape(&envelope).map_err(|error| {
        SolverError::InvalidInput(format!(
            "obstacle {index} cannot be prepared for nesting: {error:#}"
        ))
    })?;
    Ok(OriginalShape {
        shape: inflated,
        pre_transform: DTransformation::empty(),
        modify_mode: ShapeModifyMode::Inflate,
        modify_config: modify_config(0.0, None),
    })
}

/// Mitered outward offset of a convex polygon by `distance`; `None` when the
/// polygon is not strictly convex after dropping collinear vertices.
fn expand_convex(shape: &SPolygon, distance: f64) -> Option<SPolygon> {
    let points = shape
        .vertices
        .iter()
        .map(|point| (f64::from(point.0), f64::from(point.1)))
        .collect::<Vec<_>>();
    let count = points.len();
    let cross = |a: (f64, f64), b: (f64, f64), c: (f64, f64)| {
        (b.0 - a.0) * (c.1 - b.1) - (b.1 - a.1) * (c.0 - b.0)
    };
    let scale = f64::from(shape.diameter).max(1.0);
    let mut corners = Vec::with_capacity(count);
    for index in 0..count {
        let turn = cross(
            points[(index + count - 1) % count],
            points[index],
            points[(index + 1) % count],
        );
        if turn < -1.0e-9 * scale * scale {
            return None;
        }
        if turn > 1.0e-9 * scale * scale {
            corners.push(points[index]);
        }
    }
    if corners.len() < 3 {
        return None;
    }
    // Vertices are counter-clockwise, so the outward normal of edge a->b is
    // (dy, -dx) / length. Each new vertex is the intersection of the two
    // offset edge lines meeting at the original corner.
    let offset_line = |a: (f64, f64), b: (f64, f64)| {
        let (dx, dy) = (b.0 - a.0, b.1 - a.1);
        let length = dx.hypot(dy);
        let normal = (dy / length, -dx / length);
        (
            (a.0 + normal.0 * distance, a.1 + normal.1 * distance),
            (dx, dy),
        )
    };
    let count = corners.len();
    let expanded = (0..count)
        .map(|index| {
            let previous = corners[(index + count - 1) % count];
            let current = corners[index];
            let next = corners[(index + 1) % count];
            let (p, r) = offset_line(previous, current);
            let (q, s) = offset_line(current, next);
            let denominator = r.0 * s.1 - r.1 * s.0;
            let t = ((q.0 - p.0) * s.1 - (q.1 - p.1) * s.0) / denominator;
            Point((p.0 + t * r.0) as f32, (p.1 + t * r.1) as f32)
        })
        .collect::<Vec<_>>();
    SPolygon::new(expanded).ok()
}

fn build_items(
    input: &SolverInput,
    profile: SolverProfile,
    control: &RunControl<'_>,
    enforce_deadline: bool,
) -> Result<Vec<CompoundItem>, SolverError> {
    preparation_checkpoint(input, control, enforce_deadline)?;
    let rotations = rotation_range(input)?;
    preparation_checkpoint(input, control, enforce_deadline)?;
    let item_offset = input.options.part_spacing / 2.0;
    let mut items = Vec::with_capacity(input.parts.len());
    let mut collision_cache = CollisionShapeCache::new();
    for (index, part) in input.parts.iter().enumerate() {
        preparation_checkpoint(input, control, enforce_deadline)?;
        items.push(build_item(
            index,
            part,
            &rotations,
            item_offset,
            profile,
            input,
            control,
            enforce_deadline,
            &mut collision_cache,
        )?);
        preparation_checkpoint(input, control, enforce_deadline)?;
    }
    Ok(items)
}

fn build_item(
    index: usize,
    part: &Part,
    rotations: &RotationRange,
    item_offset: f64,
    profile: SolverProfile,
    input: &SolverInput,
    control: &RunControl<'_>,
    enforce_deadline: bool,
    collision_cache: &mut CollisionShapeCache,
) -> Result<CompoundItem, SolverError> {
    let shapes = part
        .components
        .iter()
        .enumerate()
        .map(|(component, value)| {
            preparation_checkpoint(input, control, enforce_deadline)?;
            let shape = polygon_to_simple(
                &value.outer,
                &format!("part {} component {component}", part.id),
            )?;
            preparation_checkpoint(input, control, enforce_deadline)?;
            Ok(shape)
        })
        .collect::<Result<Vec<_>, SolverError>>()?;
    if shapes.is_empty() {
        return Err(SolverError::InvalidInput(format!(
            "part {} has no collision components",
            part.id
        )));
    }

    let original_bbox = shapes
        .iter()
        .map(|shape| shape.bbox)
        .reduce(union_rect)
        .expect("non-empty compound shape");
    let center = (
        (original_bbox.x_min + original_bbox.x_max) * 0.5,
        (original_bbox.y_min + original_bbox.y_max) * 0.5,
    );
    let pre_transform = DTransformation::new(0.0, (-center.0, -center.1));
    let exact_center = exact_component_center(part);
    let retained = uncovered_components(&shapes);
    let mut components = Vec::with_capacity(retained.len());
    for (component_index, shape) in shapes.into_iter().enumerate() {
        if !retained.contains(&component_index) {
            continue;
        }
        preparation_checkpoint(input, control, enforce_deadline)?;
        let original_shape = OriginalShape {
            shape,
            pre_transform,
            modify_mode: ShapeModifyMode::Inflate,
            modify_config: modify_config(item_offset, profile.simplify_tolerance),
        };
        let shape_orig = std::sync::Arc::new(original_shape);
        let surrogate_config = if profile.search_surrogates {
            profile.cde_config().item_surrogate_config
        } else {
            SPSurrogateConfig::none()
        };
        // Include the centred contour and all modification and surrogate inputs.
        let cache_key = (
            collision_cache_key(&part.components[component_index].outer, exact_center),
            item_offset.to_bits(),
            profile.simplify_tolerance.map(f32::to_bits),
            profile.search_surrogates,
            if profile.search_surrogates { format!("{surrogate_config:?}") } else { String::new() },
        );
        let shape_cd = if let Some(shared) = collision_cache.get(&cache_key) {
            Arc::clone(shared)
        } else {
            let global = shape_cache().lock().ok().and_then(|mut cache| cache.get(&cache_key));
            let shape_cd = if let Some(shared) = global {
                shared
            } else {
            let mut shape_cd = part_collision_shape(&shape_orig).map_err(|error| {
                SolverError::InvalidInput(format!(
                    "part {} cannot be prepared for nesting: {error:#}",
                    part.id
                ))
            })?;
            if profile.search_surrogates {
                shape_cd
                    .generate_surrogate(surrogate_config)
                    .map_err(|error| {
                        SolverError::InvalidInput(format!(
                            "part {} surrogate failed: {error:#}",
                            part.id
                        ))
                    })?;
            }
            let computed = Arc::new(shape_cd);
            shape_cache().lock().ok()
                // Charge the key too: it holds every input outline vertex.
                .map(|mut cache| {
                    let charge = computed.vertices.len() + cache_key.0.len() / 2;
                    cache.insert(cache_key.clone(), Arc::clone(&computed), charge)
                })
                .unwrap_or(computed)
            };
            collision_cache.insert(cache_key, Arc::clone(&shape_cd));
            shape_cd
        };
        let component = Item {
            id: index,
            shape_orig,
            shape_cd,
            allowed_rotation: rotations.clone(),
            min_quality: None,
            surrogate_config,
        };
        preparation_checkpoint(input, control, enforce_deadline)?;
        components.push(component);
    }
    let internal_bbox = components
        .iter()
        .map(|component| component.shape_cd.bbox)
        .reduce(union_rect)
        .expect("non-empty compound item");
    let diameter = internal_bbox.width().hypot(internal_bbox.height());
    let area = components.iter().map(Item::area).sum();
    Ok(CompoundItem {
        part_id: part.id,
        components,
        allowed_rotation: rotations.clone(),
        diameter,
        area,
    })
}

type ShapeKey = (Vec<u32>, u64, Option<u32>, bool, String);
type CollisionShapeCache = HashMap<ShapeKey, Arc<SPolygon>>;
type ProxyKey = (Vec<u32>, u64);

const CACHE_MAX_ENTRIES: usize = 256;
const CACHE_MAX_VERTICES: usize = 500_000;

struct BoundedCache<K, V> {
    entries: HashMap<K, (Arc<V>, usize, u64)>,
    vertices: usize,
    tick: u64,
}

impl<K: Eq + std::hash::Hash + Clone, V> BoundedCache<K, V> {
    fn new() -> Self { Self { entries: HashMap::new(), vertices: 0, tick: 0 } }
    fn get(&mut self, key: &K) -> Option<Arc<V>> {
        self.tick = self.tick.wrapping_add(1);
        self.entries.get_mut(key).map(|(value, _, used)| {
            *used = self.tick;
            Arc::clone(value)
        })
    }
    fn insert(&mut self, key: K, value: Arc<V>, vertices: usize) -> Arc<V> {
        if let Some(existing) = self.get(&key) { return existing; }
        if vertices > CACHE_MAX_VERTICES { return value; }
        while self.entries.len() >= CACHE_MAX_ENTRIES || self.vertices + vertices > CACHE_MAX_VERTICES {
            let oldest = self.entries.iter().min_by_key(|(_, (_, _, used))| *used)
                .map(|(key, _)| key.clone()).expect("cache has an entry");
            let (_, removed, _) = self.entries.remove(&oldest).unwrap();
            self.vertices -= removed;
        }
        self.tick = self.tick.wrapping_add(1);
        self.vertices += vertices;
        self.entries.insert(key, (Arc::clone(&value), vertices, self.tick));
        value
    }
}

static SHAPE_CACHE: OnceLock<Mutex<BoundedCache<ShapeKey, SPolygon>>> = OnceLock::new();
static PROXY_CACHE: OnceLock<Mutex<BoundedCache<ProxyKey, Vec<(f64, f64)>>>> = OnceLock::new();
#[cfg(test)]
thread_local! { static PROXY_COMPUTATIONS: std::cell::Cell<usize> = const { std::cell::Cell::new(0) }; }
fn shape_cache() -> &'static Mutex<BoundedCache<ShapeKey, SPolygon>> {
    SHAPE_CACHE.get_or_init(|| Mutex::new(BoundedCache::new()))
}
fn proxy_cache() -> &'static Mutex<BoundedCache<ProxyKey, Vec<(f64, f64)>>> {
    PROXY_CACHE.get_or_init(|| Mutex::new(BoundedCache::new()))
}

/// Translation-invariant key: the float64 input contour relative to the
/// float64 centre of the whole part, rounded once to float32. Copies that
/// differ only by a translation share one collision shape. Reuse may differ
/// from a per-copy rebuild by one float32 rounding step of the centred
/// coordinates, the same magnitude as the existing float64-to-float32 input
/// conversion in `polygon_to_simple`.
fn collision_cache_key(outer: &Polygon, center: (f64, f64)) -> Vec<u32> {
    outer
        .0
        .iter()
        .flat_map(|point| {
            [
                ((point.x - center.0) as f32).to_bits(),
                ((point.y - center.1) as f32).to_bits(),
            ]
        })
        .collect()
}

fn exact_component_center(part: &Part) -> (f64, f64) {
    let (mut x_min, mut y_min) = (f64::INFINITY, f64::INFINITY);
    let (mut x_max, mut y_max) = (f64::NEG_INFINITY, f64::NEG_INFINITY);
    for point in part
        .components
        .iter()
        .flat_map(|component| component.outer.0.iter())
    {
        x_min = x_min.min(point.x);
        y_min = y_min.min(point.y);
        x_max = x_max.max(point.x);
        y_max = y_max.max(point.y);
    }
    ((x_min + x_max) * 0.5, (y_min + y_max) * 0.5)
}

/// Indices of the components not strictly inside another component of the
/// same part. Collision uses each component's filled outer region, and
/// inflation preserves containment, so a covered component (text on a card
/// background, artwork inside its cut line) never changes the part's
/// footprint or spacing envelope; it only costs preparation and collision
/// checks. Touching or crossing components are always kept. Among
/// identical components the lowest index is kept.
fn uncovered_components(shapes: &[SPolygon]) -> Vec<usize> {
    let strictly_inside = |inner: &SPolygon, outer: &SPolygon| {
        let (a, b) = (inner.bbox, outer.bbox);
        if a.x_min < b.x_min || a.y_min < b.y_min || a.x_max > b.x_max || a.y_max > b.y_max {
            return false;
        }
        let clearance = 1.0e-6 * f64::from(outer.diameter.max(1.0));
        let outer_edges = outer.edge_iter().collect::<Vec<_>>();
        inner
            .vertices
            .iter()
            .all(|point| outer.collides_with(point))
            && inner.edge_iter().all(|edge| {
                outer_edges
                    .iter()
                    .all(|other| segment_distance(&edge, other) > clearance)
            })
    };
    let identical = |left: &SPolygon, right: &SPolygon| left.vertices == right.vertices;
    (0..shapes.len())
        .filter(|&index| {
            !(0..shapes.len()).any(|other| {
                other != index
                    && (strictly_inside(&shapes[index], &shapes[other])
                        || (other < index && identical(&shapes[index], &shapes[other])))
            })
        })
        .collect()
}

fn union_rect(left: Rect, right: Rect) -> Rect {
    Rect {
        x_min: left.x_min.min(right.x_min),
        y_min: left.y_min.min(right.y_min),
        x_max: left.x_max.max(right.x_max),
        y_max: left.y_max.max(right.y_max),
    }
}

/// Retain the original geometry/transform for ranking and document placement.
/// Only when the spacing offset fails, remove microscopic contour noise with
/// Jagua's existing conservative simplifier, then retry that same offset.
/// Both search and authoritative validation use this identical envelope.
fn part_collision_shape(original: &OriginalShape) -> Result<SPolygon, String> {
    if let Some(shape) = bounded_inflate(original) {
        return Ok(shape);
    }
    exact_collision_shape(original).or_else(|error| hull_envelope(original).ok_or(error))
}

/// Last resort when every offset of the real contour fails (for example a
/// pixel-staircase contour from a bitmap-only part or a traced bitmap, which
/// geo-buffer can turn into a self-intersecting polygon): the mitered
/// expansion of the convex hull. It contains the hull
/// dilated by the spacing radius, so it is always conservative; the part keeps
/// nesting with a looser outline instead of failing the whole job.
fn hull_envelope(original: &OriginalShape) -> Option<SPolygon> {
    let offset = f64::from(original.modify_config.offset?);
    if original.modify_mode != ShapeModifyMode::Inflate || !(offset > 0.0) {
        return None;
    }
    let source = original
        .shape
        .transform_clone(&original.pre_transform.compose());
    let hull = SPolygon::new(convex_hull_from_points(source.vertices.clone())).ok()?;
    let envelope = expand_convex(&hull, offset)?;
    inflation_is_conservative(&source, &envelope, offset).then_some(envelope)
}

fn exact_collision_shape(original: &OriginalShape) -> Result<SPolygon, String> {
    match original.convert_to_internal() {
        Ok(shape) => Ok(shape),
        Err(error) => {
            if original.modify_mode != ShapeModifyMode::Inflate
                || !original.modify_config.offset.is_some_and(|v| v > 0.0)
            {
                return Err(error.to_string());
            }
            // At most 0.01% extra collision area, never a shrink or a hull.
            let simplified = simplify_shape(&original.shape, ShapeModifyMode::Inflate, 0.0001);
            if !shape_modification_valid(&original.shape, &simplified, ShapeModifyMode::Inflate) {
                return Err(error.to_string());
            }
            let mut retry = original.clone();
            retry.shape = simplified;
            retry.convert_to_internal().map_err(|retry_error| {
                retry_error.to_string()
            })
        }
    }
}

/// Contours with fewer vertices keep the exact offset path unchanged.
const BOUNDED_OFFSET_MIN_VERTICES: usize = 64;
/// Preferred vertex count after conservative pre-simplification.
const BOUNDED_OFFSET_TARGET_VERTICES: usize = 400;
/// Upper bound, in document units, of the extra clearance simplification may add.
const BOUNDED_OFFSET_MAX_EXTRA: f64 = 1.0;
/// geo-buffer approximates round joins with 0.1 rad steps; dividing the
/// radius by cos(0.05) puts every chord on or outside the requested arc.
const ROUND_JOIN_CHORD_FACTOR: f64 = 0.998_750_260_394_966; // cos(0.05)
/// Accepted float32 shortfall when verifying a clearance, in document units.
const CLEARANCE_VERIFY_EPSILON: f64 = 1.0e-3;

/// Geo-buffer's straight-skeleton offset grows super-linearly with vertex
/// count (about 0.2 s at 800 and 2.6 s at 2000 vertices), which consumed the
/// complete time budget whenever part spacing was non-zero. Offset a
/// Douglas-Peucker copy (maximum deviation `delta`) by `offset + delta`
/// instead. The result is accepted only after verifying that it contains the
/// original contour with at least `offset` clearance; otherwise the caller
/// keeps the exact path. Clearance is never reduced, only locally enlarged by
/// at most `2 * delta`.
fn bounded_inflate(original: &OriginalShape) -> Option<SPolygon> {
    let offset = f64::from(original.modify_config.offset?);
    if original.modify_mode != ShapeModifyMode::Inflate
        || !(offset > 0.0)
        || original.modify_config.narrow_concavity_cutoff.is_some()
        || original.shape.n_vertices() < BOUNDED_OFFSET_MIN_VERTICES
    {
        return None;
    }
    let source = original
        .shape
        .transform_clone(&original.pre_transform.compose());
    let points = source
        .vertices
        .iter()
        .map(|point| (f64::from(point.0), f64::from(point.1)))
        .collect::<Vec<_>>();
    let deltas = [
        (0.25 * offset).min(0.5 * BOUNDED_OFFSET_MAX_EXTRA),
        offset.min(BOUNDED_OFFSET_MAX_EXTRA),
    ];
    let mut accepted = None;
    for delta in deltas {
        let kept = douglas_peucker_ring(&points, delta);
        if kept.len() < 3 || kept.len() >= points.len() {
            continue;
        }
        let Ok(simplified) = SPolygon::new(
            kept.iter()
                .map(|&(x, y)| Point(x as f32, y as f32))
                .collect(),
        ) else {
            continue;
        };
        let radius = (offset + delta) / ROUND_JOIN_CHORD_FACTOR;
        let Ok(mut inflated) = offset_shape(&simplified, ShapeModifyMode::Inflate, radius as f32)
        else {
            continue;
        };
        if let Some(tolerance) = original.modify_config.simplify_tolerance {
            inflated = simplify_shape(&inflated, ShapeModifyMode::Inflate, tolerance);
        }
        if !inflation_is_conservative(&source, &inflated, offset) {
            continue;
        }
        let small_enough = kept.len() <= BOUNDED_OFFSET_TARGET_VERTICES;
        accepted = Some(inflated);
        if small_enough {
            break;
        }
    }
    accepted
}

/// Returns a simple polygon that contains `points` with at least `offset`
/// clearance (verified by `inflation_is_conservative`), simplified where
/// possible; None when no verified proxy exists.
/// Most vertices conservative_inflated_ring offsets (about 2.6 s at 2000).
const PROXY_MAX_OFFSET_VERTICES: usize = 1500;

pub(crate) fn conservative_inflated_ring(
    points: &[(f64, f64)],
    offset: f64,
) -> Option<Vec<(f64, f64)>> {
    conservative_inflated_ring_until(points, offset, &|| false)
}

/// conservative_inflated_ring that also returns None as soon as `stop()`
/// holds, checked before each simplification attempt.
pub(crate) fn conservative_inflated_ring_until(
    points: &[(f64, f64)],
    offset: f64,
    stop: &dyn Fn() -> bool,
) -> Option<Vec<(f64, f64)>> {
    if !offset.is_finite() || offset <= 0.0 || points.len() < 3 {
        return None;
    }
    let source = SPolygon::new(points.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect()).ok()?;
    // The simplification allowance also scales with the ring's size: with a
    // tiny `offset` (spacing 0) an offset-sized allowance keeps nearly every
    // vertex, and the straight-skeleton offset below grows super-linearly
    // (measured 9-27 s at 3000 vertices). The coarse allowance (0.2% of the
    // diagonal, adding at most twice that to the proxy) is tried first so the
    // offset stays cheap; the fine one only if the coarse proxy fails.
    // A ring still too detailed at the coarse allowance (large traced
    // outlines) gets wider ones, doubling up to 0.5% of its diagonal.
    let diagonal = f64::from(source.bbox.width()).hypot(f64::from(source.bbox.height()));
    let coarse = offset.max(0.002 * diagonal).min(BOUNDED_OFFSET_MAX_EXTRA);
    let mut deltas = vec![coarse, (0.25 * offset).max(0.0005 * diagonal).min(0.5 * BOUNDED_OFFSET_MAX_EXTRA)];
    let mut wider = 2.0 * coarse;
    while wider <= (0.005 * diagonal).max(coarse) {
        deltas.push(wider);
        wider *= 2.0;
    }
    let mut accepted = None;
    for delta in deltas {
        if stop() {
            return None;
        }
        let kept = douglas_peucker_ring(points, delta);
        // Never offset a ring too detailed to offset quickly.
        if kept.len() < 3 || kept.len() >= points.len() || kept.len() > PROXY_MAX_OFFSET_VERTICES {
            continue;
        }
        let Ok(simplified) = SPolygon::new(
            kept.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect(),
        ) else {
            continue;
        };
        let radius = (offset + delta) / ROUND_JOIN_CHORD_FACTOR;
        let Ok(inflated) = offset_shape(&simplified, ShapeModifyMode::Inflate, radius as f32) else {
            continue;
        };
        let inflated = simplify_shape(&inflated, ShapeModifyMode::Inflate, 0.001);
        if !inflation_is_conservative(&source, &inflated, offset) {
            continue;
        }
        // The first verified proxy is kept: later allowances are finer and
        // only cost more.
        accepted = Some(inflated);
        break;
    }
    if accepted.is_none() && points.len() <= BOUNDED_OFFSET_TARGET_VERTICES {
        let radius = offset / ROUND_JOIN_CHORD_FACTOR;
        if let Ok(inflated) = offset_shape(&source, ShapeModifyMode::Inflate, radius as f32) {
            let inflated = simplify_shape(&inflated, ShapeModifyMode::Inflate, 0.001);
            if inflation_is_conservative(&source, &inflated, offset) {
                accepted = Some(inflated);
            }
        }
    }
    accepted.map(|polygon| {
        polygon.vertices.iter().map(|point| (f64::from(point.0), f64::from(point.1))).collect()
    })
}

pub(crate) struct CollisionProxies {
    pub(crate) part_group: Vec<usize>,
    pub(crate) part_center: Vec<(f64, f64)>,
    pub(crate) rings: Vec<Vec<(f64, f64)>>,
}

/// Proxies that do not overlap after the same rotation and translation keep
/// their parts valid under validate_external_candidate. Rings are centred:
/// a part's document point `p` maps to `p - part_center`.
pub(crate) fn collision_proxies(
    input: &SolverInput,
    extra: f64,
    control: &RunControl<'_>,
) -> Result<CollisionProxies, SolverError> {
    if !extra.is_finite() || extra <= 0.0 {
        return Err(SolverError::InvalidInput("proxy margin must be finite and positive".into()));
    }
    let mut profile = SolverProfile::from_quality(input.options.quality);
    profile.search_surrogates = false;
    // The caller's time limit bounds this preparation (build_items and every
    // proxy); cancellation stops it between steps.
    let items = build_items(input, profile, control, true)?;
    let mut groups = std::collections::HashMap::new();
    let mut result = CollisionProxies {
        part_group: Vec::with_capacity(items.len()),
        part_center: Vec::with_capacity(items.len()),
        rings: Vec::new(),
    };
    for item in &items {
        if item.components.len() != 1 {
            return Err(SolverError::InvalidInput("collision proxies need single-component parts".into()));
        }
        let component = &item.components[0];
        let key = std::sync::Arc::as_ptr(&component.shape_cd);
        preparation_checkpoint(input, control, true)?;
        let group = if let Some(&group) = groups.get(&key) {
            group
        } else {
            let ring = component.shape_cd.vertices.iter()
                .map(|p| (f64::from(p.0), f64::from(p.1))).collect::<Vec<_>>();
            let proxy_key = (component.shape_cd.vertices.iter()
                .flat_map(|p| [p.0.to_bits(), p.1.to_bits()]).collect(), extra.to_bits());
            let cached = proxy_cache().lock().ok().and_then(|mut cache| cache.get(&proxy_key));
            let proxy = if let Some(cached) = cached { Some(cached) } else {
                #[cfg(test)]
                PROXY_COMPUTATIONS.with(|count| count.set(count.get() + 1));
                let stop = || preparation_checkpoint(input, control, true).is_err();
                conservative_inflated_ring_until(&ring, extra, &stop).map(|computed| {
                    let computed = Arc::new(computed);
                    proxy_cache().lock().ok()
                        .map(|mut cache| {
                            let charge = computed.len() + proxy_key.0.len() / 2;
                            cache.insert(proxy_key, Arc::clone(&computed), charge)
                        })
                        .unwrap_or(computed)
                })
            };
            let Some(proxy) = proxy else {
                // A stop reports as such; otherwise no verified proxy exists.
                preparation_checkpoint(input, control, true)?;
                return Err(SolverError::Failed("no verified collision proxy".into()));
            };
            let group = result.rings.len();
            result.rings.push((*proxy).clone());
            groups.insert(key, group);
            group
        };
        result.part_group.push(group);
        let (tx, ty) = component.shape_orig.pre_transform.translation();
        result.part_center.push((-f64::from(tx), -f64::from(ty)));
    }
    Ok(result)
}

/// Closed-ring Douglas-Peucker. Every removed vertex lies within `tolerance`
/// of the chord that replaces it, so the boundaries are within `tolerance`
/// of each other. Kept vertices are original vertices in original order.
fn douglas_peucker_ring(points: &[(f64, f64)], tolerance: f64) -> Vec<(f64, f64)> {
    let count = points.len();
    if count < 4 || !(tolerance > 0.0) {
        return points.to_vec();
    }
    let squared = |a: (f64, f64), b: (f64, f64)| (a.0 - b.0).powi(2) + (a.1 - b.1).powi(2);
    let far = (1..count)
        .max_by(|&left, &right| {
            squared(points[left], points[0]).total_cmp(&squared(points[right], points[0]))
        })
        .expect("ring has at least four vertices");
    let mut keep = vec![false; count + 1];
    keep[0] = true;
    keep[far] = true;
    keep[count] = true;
    let at = |index: usize| points[index % count];
    let mut stack = vec![(0, far), (far, count)];
    while let Some((low, high)) = stack.pop() {
        if high <= low + 1 {
            continue;
        }
        let (mut worst, mut worst_index) = (0.0_f64, low);
        for index in low + 1..high {
            let distance = point_segment_distance(at(index), at(low), at(high));
            if distance > worst {
                worst = distance;
                worst_index = index;
            }
        }
        if worst > tolerance {
            keep[worst_index] = true;
            stack.push((low, worst_index));
            stack.push((worst_index, high));
        }
    }
    (0..count)
        .filter(|&index| keep[index])
        .map(|index| points[index])
        .collect()
}

fn point_segment_distance(point: (f64, f64), start: (f64, f64), end: (f64, f64)) -> f64 {
    let (dx, dy) = (end.0 - start.0, end.1 - start.1);
    let length_squared = dx * dx + dy * dy;
    let t = if length_squared > 0.0 {
        (((point.0 - start.0) * dx + (point.1 - start.1) * dy) / length_squared).clamp(0.0, 1.0)
    } else {
        0.0
    };
    (point.0 - start.0 - t * dx).hypot(point.1 - start.1 - t * dy)
}

/// True when `inflated` contains `source` and no boundary point of `source`
/// is closer than `clearance` to the boundary of `inflated`. Because both are
/// simple polygons, this implies `source` dilated by `clearance` lies inside
/// `inflated`.
fn inflation_is_conservative(source: &SPolygon, inflated: &SPolygon, clearance: f64) -> bool {
    let required = clearance - CLEARANCE_VERIFY_EPSILON;
    if !source
        .vertices
        .iter()
        .all(|point| inflated.collides_with(point))
    {
        return false;
    }
    let outer = inflated.edge_iter().collect::<Vec<_>>();
    source.edge_iter().all(|edge| {
        outer
            .iter()
            .all(|other| segment_distance(&edge, other) >= required)
    })
}

fn segment_distance(left: &Edge, right: &Edge) -> f64 {
    if left.collides_with(right) {
        return 0.0;
    }
    [
        left.distance_to(&right.start),
        left.distance_to(&right.end),
        right.distance_to(&left.start),
        right.distance_to(&left.end),
    ]
    .into_iter()
    .map(f64::from)
    .fold(f64::INFINITY, f64::min)
}

fn polygon_to_simple(polygon: &Polygon, label: &str) -> Result<SPolygon, SolverError> {
    let points = polygon
        .0
        .iter()
        .map(|point| Point(point.x as f32, point.y as f32))
        .collect();
    let shape = SPolygon::new(points).map_err(|error| {
        SolverError::InvalidInput(format!("{label} is not a simple polygon: {error:#}"))
    })?;
    if !shape.area.is_finite()
        || !shape.diameter.is_finite()
        || !shape.bbox.x_min.is_finite()
        || !shape.bbox.x_max.is_finite()
        || !shape.bbox.y_min.is_finite()
        || !shape.bbox.y_max.is_finite()
    {
        return Err(SolverError::InvalidInput(format!(
            "{label} exceeds the nesting engine's numeric range"
        )));
    }
    Ok(shape)
}

fn modify_config(offset: f64, simplify_tolerance: Option<f32>) -> ShapeModifyConfig {
    ShapeModifyConfig {
        simplify_tolerance,
        offset: (offset > 0.0).then_some(offset as f32),
        narrow_concavity_cutoff: None,
    }
}

fn rotation_range(input: &SolverInput) -> Result<RotationRange, SolverError> {
    match input.options.rotation_mode {
        ROTATION_NONE => Ok(RotationRange::None),
        ROTATION_RIGHT_ANGLES => Ok(RotationRange::Discrete(vec![
            0.0,
            FRAC_PI_2,
            PI,
            3.0 * FRAC_PI_2,
        ])),
        ROTATION_FREE => Ok(RotationRange::Continuous),
        ROTATION_DISCRETE => {
            let step = input.options.rotation_step_degrees as f32;
            let count = (360.0 / step).ceil() as usize;
            if count > 3600 {
                return Err(SolverError::InvalidInput(
                    "rotation_step_degrees creates more than 3600 orientations".to_owned(),
                ));
            }
            let rotations = (0..count)
                .map(|index| (index as f32 * step).to_radians())
                .take_while(|angle| *angle < TAU)
                .collect::<Vec<_>>();
            if rotations.is_empty() {
                return Err(SolverError::InvalidInput(
                    "rotation_step_degrees produced no orientations".to_owned(),
                ));
            }
            Ok(RotationRange::Discrete(rotations))
        }
        _ => unreachable!("options are validated before a solver is created"),
    }
}

fn search_item(
    layout: &Layout,
    item: &CompoundItem,
    input: &SolverInput,
    profile: SolverProfile,
    control: &RunControl<'_>,
    iteration: &mut u64,
    pass: u64,
) -> Result<SearchOutcome, SolverError> {
    let mut buffers = item
        .components
        .iter()
        .map(|component| {
            let mut shape = (*component.shape_cd).clone();
            shape.surrogate = None;
            shape
        })
        .collect::<Vec<_>>();
    let mut best = None;
    let mut tested = 0_usize;
    let anchor_rotations = anchor_rotations(&item.allowed_rotation);
    let per_rotation_anchor_budget =
        (profile.sample_budget / (2 * anchor_rotations.len().max(1))).clamp(4, 256);

    for angle in anchor_rotations {
        let Some(bounds) =
            translation_bounds(layout.container.outer_cd.bbox, item, angle, &mut buffers)
        else {
            continue;
        };
        let anchors = translation_anchors(layout, bounds);
        for (translation_x, translation_y) in anchors.into_iter().take(per_rotation_anchor_budget) {
            if tested >= profile.sample_budget {
                return Ok(SearchOutcome {
                    best,
                    timed_out: false,
                });
            }
            if let Some(error) = control.stop_error() {
                return Err(error);
            }
            if control.time_limit_reached(input.options.time_limit_ms) {
                return Ok(SearchOutcome {
                    best,
                    timed_out: true,
                });
            }
            let candidate = DTransformation::new(angle, (translation_x, translation_y));
            consider_candidate(layout.cde(), item, candidate, &mut buffers, &mut best);
            tested += 1;
            report_queries(control, iteration);
        }
    }

    let sequence_offset =
        sample_sequence_offset(input.options.random_seed, pass, item.part_id);
    while tested < profile.sample_budget {
        if let Some(error) = control.stop_error() {
            return Err(error);
        }
        if control.time_limit_reached(input.options.time_limit_ms) {
            return Ok(SearchOutcome {
                best,
                timed_out: true,
            });
        }

        let sequence_index = sequence_offset.saturating_add(tested).saturating_add(1);
        let angle = sampled_rotation(&item.allowed_rotation, sequence_index);
        if let Some(bounds) =
            translation_bounds(layout.container.outer_cd.bbox, item, angle, &mut buffers)
        {
            let translation_x = interpolate(
                bounds.x_min,
                bounds.x_max,
                radical_inverse(sequence_index, 2),
            );
            let translation_y = interpolate(
                bounds.y_min,
                bounds.y_max,
                radical_inverse(sequence_index, 3),
            );
            let candidate = DTransformation::new(angle, (translation_x, translation_y));
            consider_candidate(layout.cde(), item, candidate, &mut buffers, &mut best);
        }
        tested += 1;
        report_queries(control, iteration);
    }

    Ok(SearchOutcome {
        best,
        timed_out: false,
    })
}

/// Keep the first constructive pass backward-compatible, then move every
/// later pass and item to a deterministic, disjoint-looking region of the
/// low-discrepancy sequence. Without the pass and item inputs, a longer time
/// budget mostly repeats the same candidate transforms.
fn sample_sequence_offset(seed: u64, pass: u64, item_identity: u64) -> usize {
    if pass == 0 {
        return (seed % 1_000_003) as usize;
    }

    let mixed = splitmix64(seed ^ pass.rotate_left(17) ^ item_identity.rotate_left(31));
    (mixed % 1_000_003) as usize
}

fn translation_bounds(
    container_bbox: Rect,
    item: &CompoundItem,
    angle: f32,
    buffers: &mut [SPolygon],
) -> Option<TranslationBounds> {
    let rotation = DTransformation::new(angle, (0.0, 0.0)).compose();
    let rotated_bbox = item
        .components
        .iter()
        .zip(buffers)
        .map(|(component, buffer)| {
            buffer.transform_from(&component.shape_cd, &rotation);
            buffer.bbox
        })
        .reduce(union_rect)?;
    let scale = container_bbox
        .width()
        .max(container_bbox.height())
        .max(item.diameter)
        .max(f32::MIN_POSITIVE);
    let epsilon = scale * 64.0 * f32::EPSILON;
    let x_min = container_bbox.x_min - rotated_bbox.x_min + epsilon;
    let x_max = container_bbox.x_max - rotated_bbox.x_max - epsilon;
    let y_min = container_bbox.y_min - rotated_bbox.y_min + epsilon;
    let y_max = container_bbox.y_max - rotated_bbox.y_max - epsilon;
    (x_min <= x_max && y_min <= y_max).then_some(TranslationBounds {
        x_min,
        x_max,
        y_min,
        y_max,
        rotated_bbox,
        epsilon,
    })
}

fn translation_anchors(layout: &Layout, bounds: TranslationBounds) -> Vec<(f32, f32)> {
    let mut x_values = vec![
        bounds.x_min,
        bounds.x_max,
        midpoint(bounds.x_min, bounds.x_max),
    ];
    let mut y_values = vec![
        bounds.y_min,
        bounds.y_max,
        midpoint(bounds.y_min, bounds.y_max),
    ];

    for placed in layout.placed_items.values() {
        let placed_bbox = placed.shape.bbox;
        push_in_range(
            &mut x_values,
            placed_bbox.x_max + bounds.epsilon - bounds.rotated_bbox.x_min,
            bounds.x_min,
            bounds.x_max,
        );
        push_in_range(
            &mut x_values,
            placed_bbox.x_min - bounds.epsilon - bounds.rotated_bbox.x_max,
            bounds.x_min,
            bounds.x_max,
        );
        push_in_range(
            &mut x_values,
            placed_bbox.x_min - bounds.rotated_bbox.x_min,
            bounds.x_min,
            bounds.x_max,
        );
        push_in_range(
            &mut x_values,
            placed_bbox.x_max - bounds.rotated_bbox.x_max,
            bounds.x_min,
            bounds.x_max,
        );
        push_in_range(
            &mut y_values,
            placed_bbox.y_max + bounds.epsilon - bounds.rotated_bbox.y_min,
            bounds.y_min,
            bounds.y_max,
        );
        push_in_range(
            &mut y_values,
            placed_bbox.y_min - bounds.epsilon - bounds.rotated_bbox.y_max,
            bounds.y_min,
            bounds.y_max,
        );
        push_in_range(
            &mut y_values,
            placed_bbox.y_min - bounds.rotated_bbox.y_min,
            bounds.y_min,
            bounds.y_max,
        );
        push_in_range(
            &mut y_values,
            placed_bbox.y_max - bounds.rotated_bbox.y_max,
            bounds.y_min,
            bounds.y_max,
        );
    }

    normalize_axis(&mut x_values, bounds.epsilon);
    normalize_axis(&mut y_values, bounds.epsilon);

    let mut candidates = x_values
        .into_iter()
        .flat_map(|x| y_values.iter().copied().map(move |y| (x, y)))
        .collect::<Vec<_>>();
    candidates.sort_by(|left, right| {
        translation_score(*left, bounds.rotated_bbox)
            .total_cmp(&translation_score(*right, bounds.rotated_bbox))
    });
    candidates
}

fn normalize_axis(values: &mut Vec<f32>, epsilon: f32) {
    values.sort_by(f32::total_cmp);
    values.dedup_by(|left, right| (*left - *right).abs() <= epsilon);
    if values.len() > MAX_AXIS_ANCHORS {
        let tail = values.split_off(values.len() - 8);
        values.truncate(MAX_AXIS_ANCHORS - tail.len());
        values.extend(tail);
        values.sort_by(f32::total_cmp);
    }
}

fn push_in_range(values: &mut Vec<f32>, value: f32, minimum: f32, maximum: f32) {
    if value >= minimum && value <= maximum {
        values.push(value);
    }
}

fn translation_score(translation: (f32, f32), rotated_bbox: Rect) -> f32 {
    let x_max = rotated_bbox.x_max + translation.0;
    let y_max = rotated_bbox.y_max + translation.1;
    x_max * 10.0 + y_max
}

fn consider_candidate(
    cde: &CDEngine,
    item: &CompoundItem,
    candidate: DTransformation,
    buffers: &mut [SPolygon],
    best: &mut Option<SearchCandidate>,
) {
    let transform = candidate.compose();
    if item.components.iter().any(|component| {
        cde.detect_surrogate_collision(component.shape_cd.surrogate(), &transform, &NoFilter)
    }) {
        return;
    }
    let transformed_bbox = item
        .components
        .iter()
        .zip(buffers.iter_mut())
        .map(|(component, buffer)| {
            buffer.transform_from(&component.shape_cd, &transform);
            buffer.bbox
        })
        .reduce(union_rect)
        .expect("compound item has at least one component");
    let score = f64::from(translation_score((0.0, 0.0), transformed_bbox));
    if best.is_some_and(|current| current.score <= score) {
        return;
    }
    if buffers
        .iter()
        .all(|buffer| !cde.detect_poly_collision(buffer, &NoFilter))
    {
        *best = Some(SearchCandidate {
            transform: candidate,
            score,
        });
    }
}

fn anchor_rotations(rotation_range: &RotationRange) -> Vec<f32> {
    match rotation_range {
        RotationRange::None => vec![0.0],
        RotationRange::Continuous => vec![0.0, FRAC_PI_2, PI, 3.0 * FRAC_PI_2],
        RotationRange::Discrete(rotations) if rotations.len() <= MAX_ANCHOR_ROTATIONS => {
            rotations.clone()
        }
        RotationRange::Discrete(rotations) => (0..MAX_ANCHOR_ROTATIONS)
            .map(|index| rotations[index * rotations.len() / MAX_ANCHOR_ROTATIONS])
            .collect(),
    }
}

fn sampled_rotation(rotation_range: &RotationRange, sequence_index: usize) -> f32 {
    match rotation_range {
        RotationRange::None => 0.0,
        RotationRange::Continuous => radical_inverse(sequence_index, 5) * TAU,
        RotationRange::Discrete(rotations) => rotations[sequence_index % rotations.len()],
    }
}

fn radical_inverse(mut index: usize, base: usize) -> f32 {
    let inverse_base = 1.0 / base as f64;
    let mut factor = inverse_base;
    let mut result = 0.0_f64;
    while index > 0 {
        result += (index % base) as f64 * factor;
        index /= base;
        factor *= inverse_base;
    }
    result as f32
}

fn interpolate(minimum: f32, maximum: f32, fraction: f32) -> f32 {
    minimum + (maximum - minimum) * fraction
}

fn midpoint(left: f32, right: f32) -> f32 {
    left + (right - left) / 2.0
}

fn report_queries(control: &RunControl<'_>, iteration: &mut u64) {
    *iteration = iteration.saturating_add(1);
    if (*iteration).is_multiple_of(PROGRESS_QUERY_INTERVAL) {
        control.report_heartbeat(*iteration);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::job::{
        STATE_CANCELLED, STATE_COMPLETED, STATE_FAILED, STATUS_CANCELLED, STATUS_INVALID_ARGUMENT,
        STATUS_OK, VacNestingJob, VacNestingOptions, VacNestingPoint,
    };
    use std::ffi::{CStr, c_void};
    use std::ptr;
    use std::sync::atomic::{AtomicBool, Ordering as AtomicOrdering};
    use std::sync::{Arc, Mutex, mpsc};
    use std::thread::ThreadId;

    // Several search tests intentionally use real wall-clock budgets. Keep
    // CPU-intensive solver checks from starving each other when libtest runs
    // them in parallel; this does not relax any production deadline assertion.
    static CPU_INTENSIVE_SOLVER_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
    static EXPERIMENTAL_TEST_STARTED: AtomicBool = AtomicBool::new(false);
    static EXPERIMENTAL_TEST_SAW_CANCEL: AtomicBool = AtomicBool::new(false);
    static EXPERIMENTAL_TEST_SAW_LOCAL_STOP: AtomicBool = AtomicBool::new(false);
    const EXPERIMENTAL_SENTINEL_ITERATION: u64 = u64::MAX - 17;

    #[derive(Clone, Copy, Debug)]
    struct BBox {
        x_min: f64,
        y_min: f64,
        x_max: f64,
        y_max: f64,
    }

    #[derive(Default)]
    struct ProgressCapture {
        snapshots: Vec<Vec<VacNestingPlacement>>,
        malformed_publication: bool,
        cancel_on_first_snapshot: *const AtomicBool,
    }

    #[derive(Default)]
    struct PortfolioCallbackCapture {
        thread_ids: Mutex<Vec<ThreadId>>,
        stages: Mutex<Vec<u32>>,
        progress_values: Mutex<Vec<(u32, f64, u32)>>,
        baseline_snapshots: Mutex<Vec<Vec<VacNestingPlacement>>>,
        experimental_sentinel_seen: AtomicBool,
        cancel_on_solving: AtomicBool,
        cancellation_sent: AtomicBool,
        job_to_cancel: Option<Arc<VacNestingJob>>,
    }

    unsafe extern "C" fn capture_portfolio_progress(
        user_data: *mut c_void,
        progress: *const VacNestingProgress,
    ) {
        if user_data.is_null() || progress.is_null() {
            return;
        }
        // SAFETY: The capture outlives run_with_solver and all scoped workers
        // are joined before that synchronous call returns.
        let capture = unsafe { &*user_data.cast::<PortfolioCallbackCapture>() };
        // SAFETY: RunControl passes a live progress record synchronously.
        let progress = unsafe { &*progress };
        capture
            .thread_ids
            .lock()
            .unwrap()
            .push(std::thread::current().id());
        capture.stages.lock().unwrap().push(progress.stage);
        capture.progress_values.lock().unwrap().push((
            progress.stage,
            progress.best_score,
            progress.placed_count,
        ));
        if progress.iteration == EXPERIMENTAL_SENTINEL_ITERATION {
            capture
                .experimental_sentinel_seen
                .store(true, AtomicOrdering::Release);
        }
        if progress.placement_count > 0 && !progress.placements.is_null() {
            let placement_count = usize::try_from(progress.placement_count).unwrap();
            // SAFETY: placement_count describes the exact live slice supplied
            // by RunControl for this callback invocation.
            let placements = unsafe {
                std::slice::from_raw_parts(progress.placements, placement_count).to_vec()
            };
            capture.baseline_snapshots.lock().unwrap().push(placements);
        }
        if progress.stage == PROGRESS_SOLVING
            && capture.cancel_on_solving.load(AtomicOrdering::Acquire)
            && !capture.cancellation_sent.swap(true, AtomicOrdering::AcqRel)
        {
            capture
                .job_to_cancel
                .as_ref()
                .expect("cancelling capture has a job")
                .cancel();
        }
    }

    fn reporting_experimental_lane(
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        control.report(VacNestingProgress {
            iteration: EXPERIMENTAL_SENTINEL_ITERATION,
            stage: PROGRESS_SOLVING,
            ..VacNestingProgress::default()
        });
        solve_experimental_lane(input, control)
    }

    fn panicking_experimental_lane(
        _input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        panic!("injected portfolio worker panic")
    }

    fn must_not_spawn_experimental_lane(
        _input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        EXPERIMENTAL_TEST_STARTED.store(true, AtomicOrdering::Release);
        panic!("spawn-failure fallback unexpectedly ran the experimental lane")
    }

    fn panicking_baseline_lane(
        _input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        panic!("injected portfolio baseline panic")
    }

    fn failing_baseline_lane(
        _input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        Err(SolverError::Failed(
            "injected portfolio baseline failure".to_owned(),
        ))
    }

    fn malformed_baseline_lane(
        input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        Ok(input
            .parts
            .iter()
            .map(|_| VacNestingPlacement {
                part_id: u64::MAX,
                ..VacNestingPlacement::default()
            })
            .collect())
    }

    fn immediate_empty_baseline_lane(
        input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        Ok(empty_results(input))
    }

    fn local_stop_observing_experimental_lane(
        _input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        EXPERIMENTAL_TEST_STARTED.store(true, AtomicOrdering::Release);
        while !control.stop_requested() {
            std::thread::yield_now();
        }
        EXPERIMENTAL_TEST_SAW_LOCAL_STOP
            .store(control.local_stop_requested(), AtomicOrdering::Release);
        Err(SolverError::DeadlineReached)
    }

    fn delayed_nonwinner_experimental_lane(
        input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        std::thread::sleep(Duration::from_millis(20));
        Ok(empty_results(input))
    }

    fn delayed_error_experimental_lane(
        _input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        std::thread::sleep(Duration::from_millis(20));
        Err(SolverError::Failed(
            "delayed experimental failure".to_owned(),
        ))
    }

    fn delayed_malformed_experimental_lane(
        input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        std::thread::sleep(Duration::from_millis(20));
        Ok(input
            .parts
            .iter()
            .rev()
            .map(|part| VacNestingPlacement {
                part_id: part.id,
                ..VacNestingPlacement::default()
            })
            .collect())
    }

    fn same_bucket_baseline_lane(
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        let placements = explicit_placements(input, &[(0, 0.0, 0.0)]);
        let rank = CanonicalRank::evaluate(input, &placements, true);
        control.report_best(
            VacNestingProgress {
                iteration: 1,
                best_score: rank.canonical_area_score(),
                placed_count: rank.placed_count(),
                stage: PROGRESS_SOLVING,
                ..VacNestingProgress::default()
            },
            &placements,
        );
        Ok(placements)
    }

    fn same_bucket_higher_count_experimental_lane(
        input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        Ok(explicit_placements(input, &[(1, 0.1, 0.1), (2, 1.0, 0.1)]))
    }

    struct InjectedPortfolioSolver {
        baseline_lane: BaselineLane,
        experimental_lane: ExperimentalLane,
    }

    struct SpawnFailurePortfolioSolver;

    fn run_unlimited_portfolio_with_watchdog(
        baseline_lane: BaselineLane,
    ) -> std::thread::Result<Result<(), SolverError>> {
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 10.0, 10.0),
            vec![(7, rectangle(0.0, 0.0, 1.0, 1.0))],
        );
        input.options.worker_count = 2;
        input.options.time_limit_ms = 0;
        let cancelled = Arc::new(AtomicBool::new(false));
        let runner_cancelled = Arc::clone(&cancelled);
        let (outcome_tx, outcome_rx) = mpsc::channel();
        let runner = std::thread::spawn(move || {
            let control = RunControl::for_tests(&runner_cancelled, input.parts.len());
            let outcome = catch_unwind(AssertUnwindSafe(|| {
                solve_portfolio_execution_with_lanes(
                    &input,
                    &control,
                    baseline_lane,
                    local_stop_observing_experimental_lane,
                )
                .map(|_| ())
            }));
            outcome_tx.send(outcome).unwrap();
        });

        let outcome = match outcome_rx.recv_timeout(Duration::from_secs(1)) {
            Ok(outcome) => outcome,
            Err(error) => {
                // A real watchdog must also clean up a regressed worker rather
                // than hanging the test process forever. Public cancellation
                // remains an independent escape hatch for the injected lane.
                cancelled.store(true, AtomicOrdering::Release);
                let _ = outcome_rx.recv_timeout(Duration::from_secs(1));
                runner.join().unwrap();
                panic!("unlimited portfolio did not terminate before watchdog: {error}");
            }
        };
        runner.join().unwrap();
        outcome
    }

    impl Solver for InjectedPortfolioSolver {
        fn solve(
            &self,
            input: &SolverInput,
            control: &RunControl<'_>,
        ) -> Result<Vec<VacNestingPlacement>, SolverError> {
            solve_portfolio_execution_with_lanes(
                input,
                control,
                self.baseline_lane,
                self.experimental_lane,
            )
            .map(|execution| execution.placements)
        }

        fn reported_incumbents_are_recoverable(&self) -> bool {
            true
        }
    }

    impl Solver for SpawnFailurePortfolioSolver {
        fn solve(
            &self,
            input: &SolverInput,
            control: &RunControl<'_>,
        ) -> Result<Vec<VacNestingPlacement>, SolverError> {
            solve_portfolio_execution_with_lanes_and_spawn_mode(
                input,
                control,
                solve_baseline_lane,
                must_not_spawn_experimental_lane,
                WorkerSpawnMode::InjectFailure,
            )
            .map(|execution| execution.placements)
        }

        fn reported_incumbents_are_recoverable(&self) -> bool {
            true
        }
    }

    fn cancellation_observing_experimental_lane(
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        EXPERIMENTAL_TEST_STARTED.store(true, AtomicOrdering::Release);
        while !control.stop_requested() {
            if control.time_limit_reached(input.options.time_limit_ms) {
                return Ok(empty_results(input));
            }
            std::thread::yield_now();
        }
        if control.is_cancelled() {
            EXPERIMENTAL_TEST_SAW_CANCEL.store(true, AtomicOrdering::Release);
            Err(SolverError::Cancelled)
        } else {
            Err(SolverError::DeadlineReached)
        }
    }

    fn overlapping_experimental_lane(
        input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        Ok(input
            .parts
            .iter()
            .map(|part| VacNestingPlacement {
                part_id: part.id,
                placed: 1,
                translation_x: 1.0,
                translation_y: 1.0,
                ..VacNestingPlacement::default()
            })
            .collect())
    }

    fn improving_experimental_lane(
        input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        Ok(explicit_placements(input, &[(1, 0.1, 0.1), (2, 6.3, 0.1)]))
    }

    fn reordered_experimental_lane(
        input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        Ok(input
            .parts
            .iter()
            .rev()
            .map(|part| VacNestingPlacement {
                part_id: part.id,
                placed: 1,
                ..VacNestingPlacement::default()
            })
            .collect())
    }

    fn failing_experimental_lane(
        _input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        Err(SolverError::Failed(
            "injected experimental failure".to_owned(),
        ))
    }

    fn deadline_experimental_lane(
        _input: &SolverInput,
        _control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        Err(SolverError::DeadlineReached)
    }

    unsafe extern "C" fn capture_optimizer_progress(
        user_data: *mut c_void,
        progress: *const VacNestingProgress,
    ) {
        if user_data.is_null() || progress.is_null() {
            return;
        }
        // SAFETY: Tests pass a live ProgressCapture and RunControl invokes the
        // callback synchronously before either the capture or progress value
        // leaves scope.
        let capture = unsafe { &mut *user_data.cast::<ProgressCapture>() };
        // SAFETY: Null was rejected above and RunControl owns this progress
        // value for the complete callback invocation.
        let progress = unsafe { &*progress };
        if progress.stage != PROGRESS_SOLVING || progress.placement_count == 0 {
            return;
        }
        let Ok(placement_count) = usize::try_from(progress.placement_count) else {
            capture.malformed_publication = true;
            return;
        };
        if progress.placements.is_null() {
            capture.malformed_publication = true;
            return;
        }
        // SAFETY: RunControl sets placement_count from the exact live slice it
        // passes to its synchronous callback.
        let placements =
            unsafe { std::slice::from_raw_parts(progress.placements, placement_count).to_vec() };
        capture.snapshots.push(placements);

        if capture.snapshots.len() == 1 && !capture.cancel_on_first_snapshot.is_null() {
            // SAFETY: The cancellation test stores a pointer to an AtomicBool
            // that outlives this synchronous solve invocation.
            unsafe { &*capture.cancel_on_first_snapshot }.store(true, AtomicOrdering::Release);
        }
    }

    /// Geometry tests need one deterministic constructive pass rather than a
    /// wall-clock benchmark. The dedicated deadline test below deliberately
    /// bypasses this wrapper and exercises the production refinement loop.
    struct SolverTestJob(VacNestingJob);

    impl std::ops::Deref for SolverTestJob {
        type Target = VacNestingJob;

        fn deref(&self) -> &Self::Target {
            &self.0
        }
    }

    impl SolverTestJob {
        fn run(
            &self,
            callback: crate::job::ProgressCallback,
            user_data: *mut std::ffi::c_void,
        ) -> i32 {
            self.0
                .run_with_solver(&SinglePassJaguaSolver, callback, user_data)
        }

        fn run_with_wall_clock_deadline(&self) -> i32 {
            self.0.run(None, ptr::null_mut())
        }
    }

    #[test]
    fn radical_inverse_is_stable_and_inside_unit_interval() {
        assert_eq!(radical_inverse(1, 2), 0.5);
        assert_eq!(radical_inverse(2, 2), 0.25);
        assert_eq!(radical_inverse(3, 2), 0.75);
        for index in 1..10_000 {
            let sample = radical_inverse(index, 3);
            assert!(sample > 0.0 && sample < 1.0);
        }
    }

    #[test]
    fn later_passes_and_items_use_distinct_deterministic_sample_regions() {
        let seed = 91_337;
        assert_eq!(
            sample_sequence_offset(seed, 0, 0),
            (seed % 1_000_003) as usize
        );
        assert_eq!(
            sample_sequence_offset(seed, 17, 4),
            sample_sequence_offset(seed, 17, 4)
        );
        assert_ne!(
            sample_sequence_offset(seed, 1, 0),
            sample_sequence_offset(seed, 2, 0)
        );
        assert_ne!(
            sample_sequence_offset(seed, 1, 0),
            sample_sequence_offset(seed, 1, 1)
        );
    }

    #[test]
    fn refinement_visits_every_placed_part_in_a_seeded_cycle() {
        let placements = [
            VacNestingPlacement {
                part_id: 1,
                placed: 1,
                ..VacNestingPlacement::default()
            },
            VacNestingPlacement {
                part_id: 2,
                ..VacNestingPlacement::default()
            },
            VacNestingPlacement {
                part_id: 3,
                placed: 1,
                ..VacNestingPlacement::default()
            },
            VacNestingPlacement {
                part_id: 4,
                placed: 1,
                ..VacNestingPlacement::default()
            },
        ];
        let first_cycle = (0..3)
            .map(|index| refinement_part_index(&placements, 81, index).unwrap())
            .collect::<Vec<_>>();
        let second_cycle = (3..6)
            .map(|index| refinement_part_index(&placements, 81, index).unwrap())
            .collect::<Vec<_>>();
        let mut sorted = first_cycle.clone();
        sorted.sort_unstable();
        assert_eq!(sorted, vec![0, 2, 3]);
        assert_eq!(first_cycle, second_cycle);
        assert_eq!(refinement_part_index(&[], 81, 0), None);
    }

    #[test]
    fn external_validation_matches_exact_replay_without_search_surrogates() {
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 40.0, 30.0),
            vec![(1, rectangle(0.0, 0.0, 5.0, 5.0)),
                 (2, rectangle(0.0, 0.0, 5.0, 5.0))],
        );
        input.holes.push(rectangle(25.0, 10.0, 8.0, 14.0));
        input.options.part_spacing = 1.0;
        input.options.container_margin = 1.0;
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let problem = prepare_problem(&input, &control).unwrap();
        for (poses, expected) in [
            ([(0, 3.0, 3.0), (1, 11.0, 3.0)], true),
            ([(0, 3.0, 3.0), (1, 4.0, 4.0)], false),
            ([(0, 3.0, 3.0), (1, 8.5, 3.0)], false),
            ([(0, 3.0, 3.0), (1, 26.0, 12.0)], false),
            ([(0, -2.0, 3.0), (1, 11.0, 3.0)], false),
        ] {
            let placements = explicit_placements(&input, &poses);
            assert_eq!(solution_is_feasible(&input, &problem, &placements, &control).unwrap(), expected);
            assert_eq!(validate_external_candidate(&input, &placements, &control).unwrap(), expected);
        }
        cancelled.store(true, Ordering::Relaxed);
        assert!(validate_external_candidate(&input, &empty_results(&input), &control).is_err());
    }

    #[test]
    fn authoritative_validation_rejects_overlap_and_container_holes() {
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 40.0, 30.0),
            vec![
                (1, rectangle(0.0, 0.0, 8.0, 8.0)),
                (2, rectangle(0.0, 0.0, 8.0, 8.0)),
            ],
        );
        input.holes.push(rectangle(14.0, 8.0, 12.0, 14.0));
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let problem = prepare_problem(&input, &control).unwrap();

        let valid = explicit_placements(&input, &[(0, 2.0, 2.0), (1, 30.0, 20.0)]);
        assert!(solution_is_feasible(&input, &problem, &valid, &control).unwrap());

        let overlapping = explicit_placements(&input, &[(0, 2.0, 2.0), (1, 5.0, 5.0)]);
        assert!(!solution_is_feasible(&input, &problem, &overlapping, &control).unwrap());

        let in_hole = explicit_placements(&input, &[(0, 2.0, 2.0), (1, 16.0, 10.0)]);
        assert!(!solution_is_feasible(&input, &problem, &in_hole, &control).unwrap());
    }

    #[test]
    fn bounded_relocation_compacts_a_shifted_valid_solution() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = rectangular_input(
            rectangle(0.0, 0.0, 100.0, 100.0),
            vec![(7, rectangle(0.0, 0.0, 10.0, 10.0))],
        );
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let problem = prepare_problem(&input, &control).unwrap();
        let source_results = explicit_placements(&input, &[(0, 50.0, 50.0)]);
        assert!(solution_is_feasible(&input, &problem, &source_results, &control).unwrap());
        let source = RankedSolution::new(&input, source_results, true).unwrap();
        let mut iteration = 0;
        let refined = relocate_part(&input, &problem, &source, 0, &control, &mut iteration, 1)
            .unwrap()
            .unwrap();

        assert!(refined.rank().is_better_than(source.rank()));
        assert!(solution_is_feasible(&input, &problem, refined.placements(), &control).unwrap());
        assert!(refined.placements()[0].translation_x < 50.0);
        assert!(refined.placements()[0].translation_y < 50.0);
    }

    #[test]
    fn phase_seed_derivation_is_repeatable_and_domain_separated() {
        let phases = [
            OptimizerPhase::Exploration,
            OptimizerPhase::Insertion,
            OptimizerPhase::Ejection,
            OptimizerPhase::RuinRecreate,
            OptimizerPhase::Refinement,
        ];
        let mut seen = std::collections::HashSet::new();
        for phase in phases {
            for neighborhood in 0..32 {
                for stable_part_id in [1, 2, 17, u64::MAX] {
                    let seed = derive_phase_seed(
                        0x5eed_f00d_dead_beef,
                        phase,
                        neighborhood,
                        stable_part_id,
                    );
                    assert_eq!(
                        seed,
                        derive_phase_seed(
                            0x5eed_f00d_dead_beef,
                            phase,
                            neighborhood,
                            stable_part_id,
                        )
                    );
                    assert!(
                        seen.insert(seed),
                        "seed collision for {phase:?}/{neighborhood}/{stable_part_id}"
                    );
                }
            }
        }
    }

    #[test]
    fn stable_search_order_and_refinement_are_input_permutation_invariant() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = rectangular_input(
            rectangle(0.0, 0.0, 40.0, 30.0),
            vec![
                (40, rectangle(0.0, 0.0, 8.0, 7.0)),
                (10, rectangle(0.0, 0.0, 8.0, 7.0)),
                (30, rectangle(0.0, 0.0, 8.0, 7.0)),
                (20, rectangle(0.0, 0.0, 8.0, 7.0)),
            ],
        );
        let permutation = [2, 0, 3, 1];
        let permuted_input = permute_solver_input(&input, &permutation);
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let original_problem = prepare_problem(&input, &control).unwrap();
        let permuted_problem = prepare_problem(&permuted_input, &control).unwrap();

        for pass in 0..4 {
            let original_ids = order_for_pass(
                &original_problem.items,
                0x5eed,
                pass,
            )
            .into_iter()
            .map(|index| input.parts[index].id)
            .collect::<Vec<_>>();
            let permuted_ids = order_for_pass(
                &permuted_problem.items,
                0x5eed,
                pass,
            )
            .into_iter()
            .map(|index| permuted_input.parts[index].id)
            .collect::<Vec<_>>();
            assert_eq!(original_ids, permuted_ids, "pass family {pass}");
        }

        // Exercise production too: equal-size parts must receive the same
        // transforms by ID, while each vector stays in its caller's order.
        let original =
            solve_with_limit(&input, &control, RefinementLimit::CompletedPasses(1)).unwrap();
        let permuted_control = RunControl::for_tests(&cancelled, input.parts.len());
        let permuted = solve_with_limit(
            &permuted_input,
            &permuted_control,
            RefinementLimit::CompletedPasses(1),
        )
        .unwrap();
        validate_solver_results(&input, &original).unwrap();
        validate_solver_results(&permuted_input, &permuted).unwrap();
        assert_eq!(placements_by_id(&original), placements_by_id(&permuted));

        let mut original_placements = empty_results(&input);
        for placement in &mut original_placements {
            placement.placed = 1;
        }
        let permuted_placements = permutation
            .iter()
            .map(|&index| original_placements[index])
            .collect::<Vec<_>>();
        for move_index in 0..16 {
            let original_index =
                refinement_part_index(&original_placements, 0x5eed, move_index).unwrap();
            let permuted_index =
                refinement_part_index(&permuted_placements, 0x5eed, move_index).unwrap();
            assert_eq!(
                original_placements[original_index].part_id,
                permuted_placements[permuted_index].part_id
            );
        }
    }

    #[test]
    fn explicit_unplaced_insertion_adds_every_part_that_fits() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = rectangular_input(
            rectangle(0.0, 0.0, 22.0, 12.0),
            vec![
                (10, rectangle(0.0, 0.0, 8.0, 8.0)),
                (20, rectangle(0.0, 0.0, 8.0, 8.0)),
            ],
        );
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let problem = prepare_problem(&input, &control).unwrap();
        let source_results = explicit_placements(&input, &[(0, 1.0, 1.0)]);
        assert!(solution_is_feasible(&input, &problem, &source_results, &control).unwrap());
        let source = RankedSolution::new(&input, source_results.clone(), true).unwrap();
        let mut iteration = 0;

        let outcome =
            insert_unplaced_parts(&input, &problem, &source, &control, &mut iteration, 0).unwrap();
        assert!(!outcome.timed_out);
        let candidate = outcome.candidate.unwrap();
        assert_eq!(source.placements(), source_results);
        assert!(
            candidate
                .placements()
                .iter()
                .all(|placement| placement.placed == 1)
        );
        assert!(candidate.rank().is_better_than(source.rank()));
        assert!(solution_is_feasible(&input, &problem, candidate.placements(), &control).unwrap());
    }

    #[test]
    fn bounded_ejection_admits_a_larger_unplaced_part_and_preserves_source() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = replacement_input(5.0, 9.0);
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let problem = prepare_problem(&input, &control).unwrap();
        let source_results = explicit_placements(&input, &[(0, 1.0, 1.0)]);
        let source = RankedSolution::new(&input, source_results.clone(), true).unwrap();
        let mut iteration = 0;

        let outcome =
            bounded_ejection_insert(&input, &problem, &source, 1, 1, &control, &mut iteration, 0)
                .unwrap();
        assert!(!outcome.timed_out);
        let candidate = outcome.candidate.unwrap();
        assert_eq!(source.placements(), source_results);
        assert_eq!(candidate.placements()[1].placed, 1);
        assert!(candidate.rank().is_better_than(source.rank()));
        assert!(solution_is_feasible(&input, &problem, candidate.placements(), &control).unwrap());
    }

    #[test]
    fn bounded_ejection_rolls_back_when_replacement_would_regress_rank() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = replacement_input(9.0, 5.0);
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let problem = prepare_problem(&input, &control).unwrap();
        let source_results = explicit_placements(&input, &[(0, 1.0, 1.0)]);
        let source = RankedSolution::new(&input, source_results.clone(), true).unwrap();
        let mut iteration = 0;

        let outcome =
            bounded_ejection_insert(&input, &problem, &source, 1, 1, &control, &mut iteration, 0)
                .unwrap();
        assert!(!outcome.timed_out);
        assert!(outcome.candidate.is_none());
        assert_eq!(source.placements(), source_results);
        assert!(solution_is_feasible(&input, &problem, source.placements(), &control).unwrap());
    }

    #[test]
    fn ruin_recreate_improves_transactionally_and_rolls_back_a_worse_case() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        for (blocker_width, target_width, should_improve) in [(5.0, 9.0, true), (9.0, 5.0, false)] {
            let input = replacement_input(blocker_width, target_width);
            let cancelled = AtomicBool::new(false);
            let control = RunControl::for_tests(&cancelled, input.parts.len());
            let problem = prepare_problem(&input, &control).unwrap();
            // Start from the canonical origin so the non-improving case cannot
            // be admitted merely as an equal-content positional tie-break.
            let source_results = explicit_placements(&input, &[(0, 0.0, 0.0)]);
            let source = RankedSolution::new(&input, source_results.clone(), true).unwrap();
            let mut iteration = 0;
            let outcome =
                ruin_and_recreate(&input, &problem, &source, 1, &control, &mut iteration, 0)
                    .unwrap();

            assert!(!outcome.timed_out);
            assert_eq!(source.placements(), source_results);
            assert_eq!(outcome.candidate.is_some(), should_improve);
            if let Some(candidate) = outcome.candidate {
                assert!(candidate.rank().is_better_than(source.rank()));
                assert!(
                    solution_is_feasible(&input, &problem, candidate.placements(), &control)
                        .unwrap()
                );
            }
        }
    }

    #[test]
    fn every_transaction_phase_honors_cancellation_and_expired_deadline() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = replacement_input(5.0, 9.0);
        let preparation_cancelled = AtomicBool::new(false);
        let preparation_control = RunControl::for_tests(&preparation_cancelled, input.parts.len());
        let problem = prepare_problem(&input, &preparation_control).unwrap();
        let source_results = explicit_placements(&input, &[(0, 1.0, 1.0)]);
        let source = RankedSolution::new(&input, source_results.clone(), true).unwrap();

        let cancelled = AtomicBool::new(true);
        let cancelled_control = RunControl::for_tests(&cancelled, input.parts.len());
        let mut iteration = 0;
        assert!(matches!(
            insert_unplaced_parts(
                &input,
                &problem,
                &source,
                &cancelled_control,
                &mut iteration,
                0
            ),
            Err(SolverError::Cancelled)
        ));
        assert!(matches!(
            bounded_ejection_insert(
                &input,
                &problem,
                &source,
                1,
                1,
                &cancelled_control,
                &mut iteration,
                0
            ),
            Err(SolverError::Cancelled)
        ));
        assert!(matches!(
            ruin_and_recreate(
                &input,
                &problem,
                &source,
                1,
                &cancelled_control,
                &mut iteration,
                0
            ),
            Err(SolverError::Cancelled)
        ));

        for operation in 0..3 {
            let deadline_cancelled = AtomicBool::new(false);
            let deadline_control = RunControl::for_tests_after_elapsed(
                &deadline_cancelled,
                input.parts.len(),
                std::time::Duration::from_millis(input.options.time_limit_ms.saturating_add(1)),
            );
            let outcome = match operation {
                0 => insert_unplaced_parts(
                    &input,
                    &problem,
                    &source,
                    &deadline_control,
                    &mut iteration,
                    0,
                ),
                1 => bounded_ejection_insert(
                    &input,
                    &problem,
                    &source,
                    1,
                    1,
                    &deadline_control,
                    &mut iteration,
                    0,
                ),
                _ => ruin_and_recreate(
                    &input,
                    &problem,
                    &source,
                    1,
                    &deadline_control,
                    &mut iteration,
                    0,
                ),
            }
            .unwrap();
            assert!(outcome.timed_out);
            assert!(outcome.candidate.is_none());
            assert_eq!(source.placements(), source_results);
        }
    }

    #[test]
    fn optimizer_preparation_and_authoritative_replay_honor_expired_deadline() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = replacement_input(5.0, 9.0);
        let active_cancelled = AtomicBool::new(false);
        let active_control = RunControl::for_tests(&active_cancelled, input.parts.len());
        let problem = prepare_problem(&input, &active_control).unwrap();
        let baseline = explicit_placements(&input, &[(0, 1.0, 1.0)]);
        assert!(solution_is_feasible(&input, &problem, &baseline, &active_control).unwrap());

        let expired_cancelled = AtomicBool::new(false);
        let expired_control = RunControl::for_tests_after_elapsed(
            &expired_cancelled,
            input.parts.len(),
            std::time::Duration::from_millis(input.options.time_limit_ms.saturating_add(1)),
        );
        assert!(
            prepare_problem_for_optimizer(&input, &expired_control)
                .unwrap()
                .is_none()
        );
        let validation =
            solution_is_feasible_with_deadline(&input, &problem, &baseline, &expired_control)
                .unwrap();
        assert!(validation.timed_out);
        assert!(!validation.feasible);

        let retained = optimize_fixed_container_from_baseline(
            &input,
            &expired_control,
            baseline.clone(),
            100,
            100,
            false,
        )
        .unwrap();
        assert_eq!(retained, baseline);
    }

    #[test]
    fn panic_after_strict_improvement_recovers_latest_valid_incumbent() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = replacement_input(5.0, 9.0);
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let problem = prepare_problem(&input, &control).unwrap();
        let initial_placements = explicit_placements(&input, &[(0, 1.0, 1.0)]);
        let improved_placements = explicit_placements(&input, &[(1, 1.0, 1.0)]);
        assert!(solution_is_feasible(&input, &problem, &initial_placements, &control).unwrap());
        assert!(solution_is_feasible(&input, &problem, &improved_placements, &control).unwrap());
        let initial = RankedSolution::new(&input, initial_placements, true).unwrap();
        let improved = RankedSolution::new(&input, improved_placements.clone(), true).unwrap();
        let mut pool = SolutionPool::new(2);
        assert!(pool.insert(initial.clone()));
        let mut incumbent = CanonicalIncumbent::new(initial);

        let recovered = contain_optimizer_panic(&control, || -> Result<(), SolverError> {
            admit_candidate(improved, &mut pool, &mut incumbent, &control, 1)?;
            panic!("injected panic after a valid strict improvement")
        })
        .unwrap();

        assert!(recovered);
        assert_eq!(incumbent.best().placements(), improved_placements);
        assert_eq!(pool.best().unwrap().placements(), improved_placements);
    }

    #[test]
    fn fixed_optimizer_retains_the_full_current_production_baseline_without_extra_work() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = optimizer_test_input();
        let baseline_passes = 3;
        let fallback_cancelled = AtomicBool::new(false);
        let fallback_control = RunControl::for_tests(&fallback_cancelled, input.parts.len());
        let fallback = solve_with_limit(
            &input,
            &fallback_control,
            RefinementLimit::CompletedPasses(baseline_passes),
        )
        .unwrap();

        let optimizer_cancelled = AtomicBool::new(false);
        let optimizer_control = RunControl::for_tests(&optimizer_cancelled, input.parts.len());
        let retained =
            FixedContainerOptimizer::bounded_after_baseline_passes(baseline_passes, 0, 0)
                .solve(&input, &optimizer_control)
                .unwrap();

        assert_eq!(retained, fallback);
    }

    #[test]
    fn fixed_optimizer_publishes_only_feasible_monotonic_incumbents() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = optimizer_test_input();
        let baseline_passes = 3;
        let fallback_cancelled = AtomicBool::new(false);
        let fallback_control = RunControl::for_tests(&fallback_cancelled, input.parts.len());
        let fallback = solve_with_limit(
            &input,
            &fallback_control,
            RefinementLimit::CompletedPasses(baseline_passes),
        )
        .unwrap();

        let cancelled = AtomicBool::new(false);
        let mut capture = ProgressCapture::default();
        let control = RunControl::for_tests_with_callback(
            &cancelled,
            input.parts.len(),
            Some(capture_optimizer_progress),
            (&mut capture as *mut ProgressCapture).cast(),
        );
        let optimized =
            FixedContainerOptimizer::bounded_after_baseline_passes(baseline_passes, 1, 2)
                .solve(&input, &control)
                .unwrap();

        assert!(!capture.malformed_publication);
        assert!(!capture.snapshots.is_empty());
        assert!(
            capture
                .snapshots
                .iter()
                .any(|snapshot| snapshot == &fallback)
        );
        assert_eq!(capture.snapshots.last(), Some(&optimized));

        let validation_cancelled = AtomicBool::new(false);
        let validation_control = RunControl::for_tests(&validation_cancelled, input.parts.len());
        let problem = prepare_problem(&input, &validation_control).unwrap();
        let mut previous_rank: Option<CanonicalRank> = None;
        for (publication_index, snapshot) in capture.snapshots.iter().enumerate() {
            assert_eq!(snapshot.len(), input.parts.len());
            assert!(
                solution_is_feasible(&input, &problem, snapshot, &validation_control).unwrap(),
                "published incumbent {publication_index} failed Jagua validation"
            );
            let rank = CanonicalRank::evaluate(&input, snapshot, true);
            assert!(rank.is_valid());
            if let Some(previous) = previous_rank.as_ref() {
                assert!(
                    !previous.is_better_than(&rank),
                    "published incumbent {publication_index} regressed canonical rank"
                );
            }
            previous_rank = Some(rank);
        }
    }

    #[test]
    fn fixed_optimizer_honors_preflight_and_cooperative_cancellation() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = optimizer_test_input();

        let preflight_cancelled = AtomicBool::new(true);
        let preflight_control = RunControl::for_tests(&preflight_cancelled, input.parts.len());
        assert!(matches!(
            FixedContainerOptimizer::bounded(4, input.parts.len())
                .solve(&input, &preflight_control),
            Err(SolverError::Cancelled)
        ));

        let cancelled = AtomicBool::new(false);
        let mut capture = ProgressCapture {
            cancel_on_first_snapshot: &raw const cancelled,
            ..ProgressCapture::default()
        };
        let control = RunControl::for_tests_with_callback(
            &cancelled,
            input.parts.len(),
            Some(capture_optimizer_progress),
            (&mut capture as *mut ProgressCapture).cast(),
        );
        assert!(matches!(
            FixedContainerOptimizer::bounded(4, input.parts.len()).solve(&input, &control),
            Err(SolverError::Cancelled)
        ));
        assert_eq!(capture.snapshots.len(), 1);
        assert!(cancelled.load(AtomicOrdering::Acquire));
    }

    #[test]
    fn fixed_optimizer_honors_an_expired_deadline_with_complete_feasible_output() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = optimizer_test_input();
        input.options.time_limit_ms = 1;
        let baseline_cancelled = AtomicBool::new(false);
        let baseline_control = RunControl::for_tests_after_elapsed(
            &baseline_cancelled,
            input.parts.len(),
            std::time::Duration::from_millis(2),
        );
        let expected_production_fallback = JaguaSolver.solve(&input, &baseline_control).unwrap();
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests_after_elapsed(
            &cancelled,
            input.parts.len(),
            std::time::Duration::from_millis(2),
        );

        let started = std::time::Instant::now();
        let result = FixedContainerOptimizer::bounded(100, 100)
            .solve(&input, &control)
            .unwrap();
        assert_eq!(result, expected_production_fallback);
        assert!(started.elapsed() < std::time::Duration::from_secs(2));
        assert_eq!(result.len(), input.parts.len());
        // An expired limit stops improvement, never the first layout: the
        // first constructive pass completes and publishes its placements.
        assert!(result.iter().any(|placement| placement.placed != 0));
        assert!(result.iter().all(|placement| placement.reserved == [0; 7]));

        let validation_cancelled = AtomicBool::new(false);
        let validation_control = RunControl::for_tests(&validation_cancelled, input.parts.len());
        let problem = prepare_problem(&input, &validation_control).unwrap();
        assert!(solution_is_feasible(&input, &problem, &result, &validation_control).unwrap());
    }

    #[test]
    fn portfolio_worker_policy_is_pure_guarded_and_capped_at_two() {
        let safe_lane = EXPERIMENTAL_FOOTPRINT_LIMIT_BYTES
            .checked_mul(MEMORY_SAFETY_DENOMINATOR)
            .unwrap()
            / MEMORY_SAFETY_NUMERATOR
            - 1;
        let equality_lane = safe_lane + 1;
        assert_eq!(
            select_portfolio_plan(1, 64, 0, Some(0), true),
            PortfolioPlan::BaselineOnly
        );
        assert_eq!(
            select_portfolio_plan(0, 2, safe_lane, Some(0), false),
            PortfolioPlan::BaselineOnly
        );
        assert_eq!(
            select_portfolio_plan(0, 2, safe_lane, Some(0), true),
            PortfolioPlan::TwoLanes
        );
        assert_eq!(
            select_portfolio_plan(2, 1, 0, Some(0), false),
            PortfolioPlan::BaselineOnly
        );
        assert_eq!(
            select_portfolio_plan(2, 2, equality_lane, Some(0), false),
            PortfolioPlan::BaselineOnly
        );
        assert_eq!(
            select_portfolio_plan(2, 2, safe_lane, Some(0), false),
            PortfolioPlan::TwoLanes
        );
        assert_eq!(
            select_portfolio_plan(1_024, 128, 0, Some(0), false),
            PortfolioPlan::TwoLanes
        );
        assert_eq!(
            select_portfolio_plan(2, 2, 0, None, false),
            PortfolioPlan::BaselineOnly,
            "an unavailable RSS probe fails closed"
        );
        assert_eq!(
            select_portfolio_plan(2, 2, u64::MAX, Some(0), false),
            PortfolioPlan::BaselineOnly,
            "guard arithmetic overflow fails closed"
        );
        assert_eq!(
            select_portfolio_plan(2, 2, safe_lane, Some(TOTAL_PROCESS_LIMIT_BYTES), false,),
            PortfolioPlan::BaselineOnly,
            "the total-process ceiling is also strict"
        );

        assert!(AUTOMATIC_PORTFOLIO_ENABLED);
    }

    #[test]
    fn portfolio_memory_gate_brackets_total_boundary_and_addition_overflow() {
        let last_safe_total = TOTAL_PROCESS_LIMIT_BYTES
            .checked_mul(MEMORY_SAFETY_DENOMINATOR)
            .unwrap()
            / MEMORY_SAFETY_NUMERATOR;
        assert_eq!(
            safety_adjusted_bytes(last_safe_total),
            Some(TOTAL_PROCESS_LIMIT_BYTES - 1)
        );
        assert_eq!(
            safety_adjusted_bytes(last_safe_total + 1),
            Some(TOTAL_PROCESS_LIMIT_BYTES + 1)
        );
        assert!(portfolio_memory_is_safe(0, Some(last_safe_total)));
        assert!(!portfolio_memory_is_safe(0, Some(last_safe_total + 1)));
        let lane = 64 * 1024 * 1024;
        let resident_with_two_lane_headroom = last_safe_total - lane * 2;
        assert!(portfolio_memory_is_safe(
            lane,
            Some(resident_with_two_lane_headroom)
        ));
        assert!(!portfolio_memory_is_safe(
            lane,
            Some(resident_with_two_lane_headroom + 1)
        ));
        assert!(
            !portfolio_memory_is_safe(lane, Some(last_safe_total - lane)),
            "headroom for only one newly allocated lane must fail closed"
        );
        assert!(
            !portfolio_memory_is_safe(1, Some(u64::MAX)),
            "resident plus both lanes overflow must fail closed"
        );
        assert!(
            !portfolio_memory_is_safe(u64::MAX / 2 + 1, Some(0)),
            "doubling the lane estimate must fail closed on overflow"
        );
    }

    #[test]
    fn worker_spawn_failure_runs_and_returns_the_frozen_baseline() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        EXPERIMENTAL_TEST_STARTED.store(false, AtomicOrdering::Release);
        let mut options = draft_options(ROTATION_NONE);
        options.worker_count = 2;
        options.time_limit_ms = 25;
        let job = VacNestingJob::new(options);
        assert_eq!(
            job.set_container(rectangle(0.0, 0.0, 30.0, 30.0)),
            STATUS_OK
        );
        assert_eq!(job.add_part(7, rectangle(0.0, 0.0, 8.0, 6.0)), STATUS_OK);

        assert_eq!(
            job.run_with_solver(&SpawnFailurePortfolioSolver, None, std::ptr::null_mut()),
            STATUS_OK
        );
        assert_eq!(job.state(), STATE_COMPLETED);
        assert_eq!(job.result_count(), 1);
        assert_eq!(job.result_at(0).unwrap().part_id, 7);
        assert!(!EXPERIMENTAL_TEST_STARTED.load(AtomicOrdering::Acquire));
    }

    #[test]
    fn experimental_memory_estimate_is_conservative_and_monotonic() {
        let empty = sample_input();
        let one_part = rectangular_input(
            rectangle(0.0, 0.0, 100.0, 100.0),
            vec![(7, rectangle(0.0, 0.0, 10.0, 10.0))],
        );
        let two_parts = rectangular_input(
            rectangle(0.0, 0.0, 100.0, 100.0),
            vec![
                (7, rectangle(0.0, 0.0, 10.0, 10.0)),
                (8, rectangle(20.0, 0.0, 10.0, 10.0)),
            ],
        );

        assert_eq!(
            estimate_experimental_footprint(&empty),
            EXPERIMENTAL_BASE_FOOTPRINT_BYTES + 4 * EXPERIMENTAL_BYTES_PER_POINT
        );
        assert!(
            estimate_experimental_footprint(&one_part) > estimate_experimental_footprint(&empty)
        );
        assert!(
            estimate_experimental_footprint(&two_parts)
                > estimate_experimental_footprint(&one_part)
        );
    }

    #[cfg(any(target_os = "macos", target_os = "linux", target_os = "windows"))]
    #[test]
    fn supported_platform_resident_memory_probe_is_live() {
        assert!(current_process_resident_bytes().is_some_and(|bytes| bytes > 0));
    }

    #[test]
    fn shared_worker_controls_have_identical_required_deadlines() {
        let cancelled = AtomicBool::new(false);
        let coordinator = RunControl::for_tests(&cancelled, 3);
        let shared = coordinator.shared_clock();
        let local_stop = AtomicBool::new(false);
        let silent = shared.silent_control(&local_stop);

        assert_eq!(coordinator.started_at(), silent.started_at());
        for budget_ms in [100, 1_000, 5_000, 60_000] {
            assert_eq!(coordinator.deadline(budget_ms), silent.deadline(budget_ms));
        }
    }

    #[test]
    fn baseline_panic_stops_and_joins_an_unlimited_experimental_lane() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        EXPERIMENTAL_TEST_STARTED.store(false, AtomicOrdering::Release);
        EXPERIMENTAL_TEST_SAW_LOCAL_STOP.store(false, AtomicOrdering::Release);

        let result = run_unlimited_portfolio_with_watchdog(panicking_baseline_lane);

        assert!(result.is_err());
        assert!(EXPERIMENTAL_TEST_STARTED.load(AtomicOrdering::Acquire));
        assert!(EXPERIMENTAL_TEST_SAW_LOCAL_STOP.load(AtomicOrdering::Acquire));
    }

    #[test]
    fn baseline_error_stops_and_joins_an_unlimited_experimental_lane() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        EXPERIMENTAL_TEST_STARTED.store(false, AtomicOrdering::Release);
        EXPERIMENTAL_TEST_SAW_LOCAL_STOP.store(false, AtomicOrdering::Release);

        let result = run_unlimited_portfolio_with_watchdog(failing_baseline_lane);

        assert!(matches!(result, Ok(Err(SolverError::Failed(_)))));
        assert!(EXPERIMENTAL_TEST_STARTED.load(AtomicOrdering::Acquire));
        assert!(EXPERIMENTAL_TEST_SAW_LOCAL_STOP.load(AtomicOrdering::Acquire));
    }

    #[test]
    fn malformed_baseline_stops_and_joins_an_unlimited_experimental_lane() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        EXPERIMENTAL_TEST_STARTED.store(false, AtomicOrdering::Release);
        EXPERIMENTAL_TEST_SAW_LOCAL_STOP.store(false, AtomicOrdering::Release);

        let result = run_unlimited_portfolio_with_watchdog(malformed_baseline_lane);

        assert!(matches!(result, Ok(Err(SolverError::Failed(_)))));
        assert!(EXPERIMENTAL_TEST_STARTED.load(AtomicOrdering::Acquire));
        assert!(EXPERIMENTAL_TEST_SAW_LOCAL_STOP.load(AtomicOrdering::Acquire));
    }

    #[test]
    fn every_fallback_path_accounts_for_terminal_deadline_overrun() {
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 30.0, 30.0),
            vec![
                (7, rectangle(0.0, 0.0, 8.0, 6.0)),
                (8, rectangle(0.0, 0.0, 7.0, 5.0)),
            ],
        );
        input.options.worker_count = 2;
        input.options.time_limit_ms = 1;

        for experimental_lane in [
            delayed_nonwinner_experimental_lane as ExperimentalLane,
            delayed_error_experimental_lane as ExperimentalLane,
            delayed_malformed_experimental_lane as ExperimentalLane,
        ] {
            let cancelled = AtomicBool::new(false);
            let control = RunControl::for_tests(&cancelled, input.parts.len());
            let execution = solve_portfolio_execution_with_lanes(
                &input,
                &control,
                immediate_empty_baseline_lane,
                experimental_lane,
            )
            .unwrap();
            let timing = execution.timing;
            assert!(timing.experimental_finished.is_some());
            assert!(timing.completed_at >= timing.experimental_finished.unwrap());
            assert!(timing.terminal_deadline_overrun >= Duration::from_millis(10));
            assert!(deadline_overrun_within_gate(
                input.options.time_limit_ms,
                timing.terminal_deadline_overrun,
            ));
        }
    }

    #[test]
    fn portfolio_lanes_share_real_deadlines_at_100ms_1s_and_5s() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        for budget_ms in [100, 1_000, 5_000] {
            let mut input = rectangular_input(
                rectangle(0.0, 0.0, 30.0, 30.0),
                vec![(7, rectangle(10.0, 20.0, 8.0, 6.0))],
            );
            input.options.worker_count = 2;
            input.options.time_limit_ms = budget_ms;
            let cancelled = AtomicBool::new(false);
            let control = RunControl::for_tests(&cancelled, input.parts.len());
            let execution =
                solve_portfolio_execution(&input, &control, solve_experimental_lane).unwrap();
            let timing = execution.timing;
            let deadline = timing.common_deadline.expect("finite deadline");

            assert_eq!(
                timing.experimental_clock_started,
                Some(timing.common_started)
            );
            assert_eq!(timing.experimental_deadline, Some(deadline));
            assert!(timing.baseline_started >= timing.common_started);
            assert!(timing.baseline_finished >= deadline);
            assert!(
                timing
                    .experimental_finished
                    .is_some_and(|at| at >= deadline)
            );
            assert_eq!(execution.placements.len(), input.parts.len());
            assert!(deadline_overrun_within_gate(
                budget_ms,
                timing.terminal_deadline_overrun
            ));
        }
    }

    #[test]
    #[ignore = "extended 60-second equal-deadline portfolio gate"]
    fn portfolio_lanes_share_real_deadline_at_60s() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 30.0, 30.0),
            vec![(7, rectangle(10.0, 20.0, 8.0, 6.0))],
        );
        input.options.worker_count = 2;
        input.options.time_limit_ms = 60_000;
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let execution =
            solve_portfolio_execution(&input, &control, solve_experimental_lane).unwrap();
        assert_eq!(
            execution.timing.experimental_clock_started,
            Some(execution.timing.common_started)
        );
        assert_eq!(
            execution.timing.experimental_deadline,
            execution.timing.common_deadline
        );
        assert!(deadline_overrun_within_gate(
            input.options.time_limit_ms,
            execution.timing.terminal_deadline_overrun
        ));
    }

    #[test]
    fn portfolio_callback_is_coordinator_only_and_rank_never_worsens() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = optimizer_test_input();
        input.options.worker_count = 2;
        input.options.time_limit_ms = 100;
        let cancelled = AtomicBool::new(false);
        let capture = PortfolioCallbackCapture::default();
        let coordinator_thread = std::thread::current().id();
        let control = RunControl::for_tests_with_callback(
            &cancelled,
            input.parts.len(),
            Some(capture_portfolio_progress),
            (&capture as *const PortfolioCallbackCapture as *mut PortfolioCallbackCapture).cast(),
        );
        let result = PortfolioSolver::for_tests(reporting_experimental_lane)
            .solve(&input, &control)
            .unwrap();

        assert!(
            capture
                .thread_ids
                .lock()
                .unwrap()
                .iter()
                .all(|thread_id| thread_id == &coordinator_thread)
        );
        assert!(
            !capture
                .experimental_sentinel_seen
                .load(AtomicOrdering::Acquire)
        );
        let snapshots = capture.baseline_snapshots.lock().unwrap();
        let baseline = snapshots.last().expect("baseline publishes an incumbent");
        let baseline_rank = CanonicalRank::evaluate(&input, baseline, true);
        let result_rank = CanonicalRank::evaluate(&input, &result, true);
        assert!(!baseline_rank.is_better_than(&result_rank));
        drop(snapshots);
        let validation_control = RunControl::for_tests(&cancelled, input.parts.len());
        assert!(authoritative_portfolio_validation(&input, &result, &validation_control).unwrap());
    }

    fn single_pass_baseline_lane(
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError> {
        SinglePassJaguaSolver.solve(input, control)
    }

    #[test]
    fn finalizing_progress_reports_the_selected_portfolio_winner_score() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut options = draft_options(ROTATION_NONE);
        options.worker_count = 2;
        options.time_limit_ms = 100;
        let job = VacNestingJob::new(options);
        assert_eq!(
            job.set_container(rectangle(0.0, 0.0, 12.4, 10.2)),
            STATUS_OK
        );
        // The injected single-pass diameter-first baseline places the 7x10
        // part (area 70), which prevents either 6x10 part from fitting. The
        // real wall-clock baseline may find the 120 layout itself on a fast
        // machine, so it cannot serve as this test's losing lane. The
        // injected, valid experimental result places both smaller parts
        // (area 120).
        assert_eq!(job.add_part(10, rectangle(0.0, 0.0, 7.0, 10.0)), STATUS_OK);
        assert_eq!(job.add_part(20, rectangle(0.0, 0.0, 6.0, 10.0)), STATUS_OK);
        assert_eq!(job.add_part(30, rectangle(0.0, 0.0, 6.0, 10.0)), STATUS_OK);
        let capture = PortfolioCallbackCapture::default();

        assert_eq!(
            job.run_with_solver(
                &PortfolioSolver::for_tests_with_baseline(
                    single_pass_baseline_lane,
                    improving_experimental_lane,
                ),
                Some(capture_portfolio_progress),
                (&capture as *const PortfolioCallbackCapture as *mut PortfolioCallbackCapture)
                    .cast(),
            ),
            STATUS_OK
        );
        assert_eq!(job.state(), STATE_COMPLETED);
        assert_eq!(job.result_count(), 3);
        assert_eq!(job.result_at(0).unwrap().placed, 0);
        assert_eq!(job.result_at(1).unwrap().placed, 1);
        assert_eq!(job.result_at(2).unwrap().placed, 1);

        let progress = capture.progress_values.lock().unwrap();
        let finalizing = progress
            .iter()
            .filter(|(stage, _, _)| *stage == crate::job::PROGRESS_FINALIZING)
            .collect::<Vec<_>>();
        assert_eq!(finalizing.len(), 1);
        assert!((finalizing[0].1 - 120.0).abs() < 1.0e-9);
        assert_eq!(finalizing[0].2, 2);
        let baseline_score = progress
            .iter()
            .filter(|(stage, _, _)| *stage == PROGRESS_SOLVING)
            .map(|(_, score, _)| *score)
            .fold(0.0_f64, f64::max);
        assert!(baseline_score < finalizing[0].1);
    }

    #[test]
    fn higher_count_same_bucket_winner_never_regresses_public_progress() {
        let mut options = draft_options(ROTATION_NONE);
        options.time_limit_ms = 1_000;
        let job = VacNestingJob::new(options);
        assert_eq!(job.set_container(rectangle(0.0, 0.0, 3.0, 2.0)), STATUS_OK);
        // Keep both totals well inside the same rounded metric bucket instead
        // of placing the fixture on an IEEE-754 exponent boundary.
        let baseline_width = f64::from_bits(1.0_f64.to_bits() + 1_024);
        let half_width = baseline_width / 2.0;
        let slightly_below_half = f64::from_bits(half_width.to_bits() - 2);
        let ranking_input = rectangular_input(
            rectangle(0.0, 0.0, 3.0, 2.0),
            vec![
                (10, rectangle(0.0, 0.0, baseline_width, 1.0)),
                (20, rectangle(0.0, 0.0, half_width, 1.0)),
                (30, rectangle(0.0, 0.0, slightly_below_half, 1.0)),
            ],
        );
        let baseline_fixture = same_bucket_baseline_lane(
            &ranking_input,
            &RunControl::for_tests(&AtomicBool::new(false), 3),
        )
        .unwrap();
        let experimental_fixture = same_bucket_higher_count_experimental_lane(
            &ranking_input,
            &RunControl::for_tests(&AtomicBool::new(false), 3),
        )
        .unwrap();
        let baseline_rank = CanonicalRank::evaluate(&ranking_input, &baseline_fixture, true);
        let experimental_rank =
            CanonicalRank::evaluate(&ranking_input, &experimental_fixture, true);
        assert!(experimental_rank.placed_area() < baseline_rank.placed_area());
        assert_eq!(
            experimental_rank.canonical_area_score().to_bits(),
            baseline_rank.canonical_area_score().to_bits()
        );
        assert!(experimental_rank.is_better_than(&baseline_rank));
        let validation_cancelled = AtomicBool::new(false);
        let validation_control = RunControl::for_tests(&validation_cancelled, 3);
        assert!(
            authoritative_portfolio_validation(
                &ranking_input,
                &experimental_fixture,
                &validation_control,
            )
            .unwrap()
        );
        assert_eq!(
            job.add_part(10, rectangle(0.0, 0.0, baseline_width, 1.0)),
            STATUS_OK
        );
        assert_eq!(
            job.add_part(20, rectangle(0.0, 0.0, half_width, 1.0)),
            STATUS_OK
        );
        assert_eq!(
            job.add_part(30, rectangle(0.0, 0.0, slightly_below_half, 1.0)),
            STATUS_OK
        );
        let capture = PortfolioCallbackCapture::default();

        assert_eq!(
            job.run_with_solver(
                &InjectedPortfolioSolver {
                    baseline_lane: same_bucket_baseline_lane,
                    experimental_lane: same_bucket_higher_count_experimental_lane,
                },
                Some(capture_portfolio_progress),
                (&capture as *const PortfolioCallbackCapture as *mut PortfolioCallbackCapture)
                    .cast(),
            ),
            STATUS_OK
        );
        assert_eq!(job.state(), STATE_COMPLETED);
        assert_eq!(job.result_at(0).unwrap().placed, 0);
        assert_eq!(job.result_at(1).unwrap().placed, 1);
        assert_eq!(job.result_at(2).unwrap().placed, 1);

        let values = capture.progress_values.lock().unwrap();
        let solving_and_finalizing = values
            .iter()
            .filter(|(stage, _, _)| {
                matches!(*stage, PROGRESS_SOLVING | crate::job::PROGRESS_FINALIZING)
            })
            .collect::<Vec<_>>();
        assert_eq!(solving_and_finalizing.len(), 2);
        assert_eq!(solving_and_finalizing[0].2, 1);
        assert_eq!(solving_and_finalizing[1].2, 2);
        assert!(
            solving_and_finalizing[1].1 >= solving_and_finalizing[0].1,
            "a canonical winner must never publish a lower score"
        );
        assert_eq!(
            solving_and_finalizing[1].1.to_bits(),
            solving_and_finalizing[0].1.to_bits()
        );
    }

    #[test]
    fn cancellation_reaches_both_portfolio_lanes_and_suppresses_finalizing() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        EXPERIMENTAL_TEST_STARTED.store(false, AtomicOrdering::Release);
        EXPERIMENTAL_TEST_SAW_CANCEL.store(false, AtomicOrdering::Release);
        let mut options = draft_options(ROTATION_NONE);
        options.worker_count = 2;
        options.time_limit_ms = 1_000;
        let job = Arc::new(VacNestingJob::new(options));
        assert_eq!(
            job.set_container(rectangle(0.0, 0.0, 100.0, 100.0)),
            STATUS_OK
        );
        assert_eq!(job.add_part(7, rectangle(0.0, 0.0, 10.0, 10.0)), STATUS_OK);
        let capture = PortfolioCallbackCapture {
            cancel_on_solving: AtomicBool::new(true),
            job_to_cancel: Some(Arc::clone(&job)),
            ..PortfolioCallbackCapture::default()
        };
        let status = job.run_with_solver(
            &PortfolioSolver::for_tests(cancellation_observing_experimental_lane),
            Some(capture_portfolio_progress),
            (&capture as *const PortfolioCallbackCapture as *mut PortfolioCallbackCapture).cast(),
        );

        assert_eq!(status, STATUS_CANCELLED);
        assert_eq!(job.state(), STATE_CANCELLED);
        assert_eq!(job.result_count(), 0);
        assert!(EXPERIMENTAL_TEST_STARTED.load(AtomicOrdering::Acquire));
        assert!(EXPERIMENTAL_TEST_SAW_CANCEL.load(AtomicOrdering::Acquire));
        let stages = capture.stages.lock().unwrap();
        assert!(!stages.contains(&crate::job::PROGRESS_FINALIZING));
        let callback_count = stages.len();
        drop(stages);
        std::thread::yield_now();
        assert_eq!(capture.stages.lock().unwrap().len(), callback_count);
    }

    #[test]
    fn experimental_panic_retains_the_exact_reported_baseline() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 30.0, 30.0),
            vec![(7, rectangle(0.0, 0.0, 8.0, 6.0))],
        );
        input.options.worker_count = 2;
        input.options.time_limit_ms = 100;
        let cancelled = AtomicBool::new(false);
        let capture = PortfolioCallbackCapture::default();
        let control = RunControl::for_tests_with_callback(
            &cancelled,
            input.parts.len(),
            Some(capture_portfolio_progress),
            (&capture as *const PortfolioCallbackCapture as *mut PortfolioCallbackCapture).cast(),
        );
        let result = PortfolioSolver::for_tests(panicking_experimental_lane)
            .solve(&input, &control)
            .unwrap();
        assert_eq!(
            capture.baseline_snapshots.lock().unwrap().last(),
            Some(&result)
        );
    }

    #[test]
    fn worker_count_one_is_exact_frozen_baseline_parity() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 30.0, 30.0),
            vec![(7, rectangle(0.0, 0.0, 8.0, 6.0))],
        );
        input.options.worker_count = 1;
        input.options.time_limit_ms = 25;
        let portfolio_cancelled = AtomicBool::new(false);
        let portfolio_control = RunControl::for_tests(&portfolio_cancelled, input.parts.len());
        let portfolio = PortfolioSolver::for_tests(panicking_experimental_lane)
            .solve(&input, &portfolio_control)
            .unwrap();
        let baseline_cancelled = AtomicBool::new(false);
        let baseline_control = RunControl::for_tests(&baseline_cancelled, input.parts.len());
        let baseline = JaguaSolver.solve(&input, &baseline_control).unwrap();
        assert_eq!(portfolio, baseline);
    }

    #[test]
    fn automatic_worker_count_runs_the_guarded_two_lane_portfolio_when_admitted() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut options = draft_options(ROTATION_NONE);
        options.worker_count = 0;
        options.time_limit_ms = 100;
        let job = VacNestingJob::new(options);
        assert_eq!(
            job.set_container(rectangle(0.0, 0.0, 12.4, 10.2)),
            STATUS_OK
        );
        assert_eq!(job.add_part(10, rectangle(0.0, 0.0, 7.0, 10.0)), STATUS_OK);
        assert_eq!(job.add_part(20, rectangle(0.0, 0.0, 6.0, 10.0)), STATUS_OK);
        assert_eq!(job.add_part(30, rectangle(0.0, 0.0, 6.0, 10.0)), STATUS_OK);
        let automatic = PortfolioSolver {
            environment: PortfolioEnvironment {
                available_parallelism: Some(2),
                estimated_footprint: Some(0),
                resident_bytes: Some(Some(0)),
                automatic_enabled: true,
                baseline_lane: solve_baseline_lane,
                experimental_lane: improving_experimental_lane,
            },
        };

        assert_eq!(
            job.run_with_solver(&automatic, None, std::ptr::null_mut()),
            STATUS_OK
        );
        assert_eq!(job.result_at(0).unwrap().placed, 0);
        assert_eq!(job.result_at(1).unwrap().placed, 1);
        assert_eq!(job.result_at(2).unwrap().placed, 1);
    }

    #[test]
    fn automatic_worker_count_falls_back_when_any_admission_probe_fails() {
        let mut input = sample_input();
        input.options.worker_count = 0;
        for environment in [
            PortfolioEnvironment {
                available_parallelism: Some(1),
                estimated_footprint: Some(0),
                resident_bytes: Some(Some(0)),
                automatic_enabled: true,
                baseline_lane: solve_baseline_lane,
                experimental_lane: panicking_experimental_lane,
            },
            PortfolioEnvironment {
                available_parallelism: Some(2),
                estimated_footprint: Some(0),
                resident_bytes: Some(None),
                automatic_enabled: true,
                baseline_lane: solve_baseline_lane,
                experimental_lane: panicking_experimental_lane,
            },
            PortfolioEnvironment {
                available_parallelism: Some(2),
                estimated_footprint: Some(u64::MAX),
                resident_bytes: Some(Some(0)),
                automatic_enabled: true,
                baseline_lane: solve_baseline_lane,
                experimental_lane: panicking_experimental_lane,
            },
        ] {
            assert_eq!(
                PortfolioSolver { environment }.plan(&input),
                PortfolioPlan::BaselineOnly
            );
        }
    }

    #[test]
    fn experimental_error_deadline_or_reordered_records_retain_baseline() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = replacement_input(9.0, 9.0);
        input.options.worker_count = 2;
        input.options.time_limit_ms = 25;

        for lane in [
            failing_experimental_lane as ExperimentalLane,
            deadline_experimental_lane as ExperimentalLane,
            reordered_experimental_lane as ExperimentalLane,
        ] {
            let cancelled = AtomicBool::new(false);
            let capture = PortfolioCallbackCapture::default();
            let control = RunControl::for_tests_with_callback(
                &cancelled,
                input.parts.len(),
                Some(capture_portfolio_progress),
                (&capture as *const PortfolioCallbackCapture as *mut PortfolioCallbackCapture)
                    .cast(),
            );
            let result = PortfolioSolver::for_tests(lane)
                .solve(&input, &control)
                .unwrap();
            assert_eq!(
                capture.baseline_snapshots.lock().unwrap().last(),
                Some(&result)
            );
        }
    }

    #[test]
    fn authoritative_portfolio_validation_honors_preexisting_cancellation() {
        let input = rectangular_input(
            rectangle(0.0, 0.0, 30.0, 30.0),
            vec![(7, rectangle(0.0, 0.0, 8.0, 6.0))],
        );
        let cancelled = AtomicBool::new(true);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        assert!(matches!(
            authoritative_portfolio_validation(&input, &empty_results(&input), &control),
            Err(SolverError::Cancelled)
        ));
    }

    #[test]
    fn portfolio_reduction_is_stable_for_identical_seed_and_input() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 30.0, 30.0),
            vec![
                (30, rectangle(50.0, 10.0, 8.0, 6.0)),
                (10, rectangle(-20.0, 40.0, 8.0, 6.0)),
                (20, rectangle(90.0, -30.0, 8.0, 6.0)),
            ],
        );
        input.options.worker_count = 2;
        // Repeatability concerns fixed work, not the OS scheduling of two
        // 100-ms searches. Keep the real coordinator/reducer and exact geometry
        // validation, but have both lanes complete one seeded constructive pass.
        // Separate tests exercise the real 100-ms/1-s/5-s deadlines.
        input.options.time_limit_ms = 60_000;
        input.options.random_seed = 0xface_feed;
        fn completed_pass(input: &SolverInput, control: &RunControl<'_>)
            -> Result<Vec<VacNestingPlacement>, SolverError>
        {
            SinglePassJaguaSolver.solve(input, control)
        }
        let run = || {
            let cancelled = AtomicBool::new(false);
            let control = RunControl::for_tests(&cancelled, input.parts.len());
            solve_portfolio_execution_with_lanes(
                &input, &control, completed_pass, completed_pass)
                .unwrap()
                .placements
        };

        assert_eq!(run(), run());
    }

    #[test]
    fn coordinator_rejects_overlapping_experimental_winner_and_measures_overrun() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = replacement_input(9.0, 9.0);
        input.options.worker_count = 2;
        input.options.time_limit_ms = 100;
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        // The coordinator fixture needs a known feasible one-part baseline.
        // Keep it clear of the container boundary, and keep the real 100-ms
        // timing checks. The experimental lane overlaps both parts at this pose.
        let execution = solve_portfolio_execution_with_lanes(
            &input,
            &control,
            |input, _control| Ok(explicit_placements(input, &[(0, 1.0, 1.0)])),
            overlapping_experimental_lane,
        )
        .unwrap();

        assert!(execution.timing.final_validation_started.is_some());
        assert!(deadline_overrun_within_gate(
            input.options.time_limit_ms,
            execution.timing.terminal_deadline_overrun
        ));
        assert_eq!(
            execution
                .placements
                .iter()
                .filter(|placement| placement.placed != 0)
                .count(),
            1
        );
        let validation_control = RunControl::for_tests(&cancelled, input.parts.len());
        assert!(
            authoritative_portfolio_validation(&input, &execution.placements, &validation_control)
                .unwrap()
        );
    }

    #[test]
    fn deadline_overrun_gate_uses_250ms_or_five_percent() {
        assert_eq!(permitted_deadline_overrun(100), Duration::from_millis(250));
        assert_eq!(
            permitted_deadline_overrun(5_000),
            Duration::from_millis(250)
        );
        assert_eq!(permitted_deadline_overrun(60_000), Duration::from_secs(3));
        assert!(deadline_overrun_within_gate(
            100,
            Duration::from_millis(250)
        ));
        assert!(!deadline_overrun_within_gate(
            100,
            Duration::from_millis(251)
        ));
        assert!(deadline_overrun_within_gate(0, Duration::from_secs(86_400)));
    }

    #[test]
    fn staged_optimizer_is_seeded_repeatable_and_never_worse_than_fallback() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let input = optimizer_test_input();
        let baseline_passes = 3;

        let run_fallback = || {
            let cancelled = AtomicBool::new(false);
            let control = RunControl::for_tests(&cancelled, input.parts.len());
            solve_with_limit(
                &input,
                &control,
                RefinementLimit::CompletedPasses(baseline_passes),
            )
            .unwrap()
        };
        let run_optimizer = || {
            let cancelled = AtomicBool::new(false);
            let control = RunControl::for_tests(&cancelled, input.parts.len());
            FixedContainerOptimizer::bounded_after_baseline_passes(baseline_passes, 1, 2)
                .solve(&input, &control)
                .unwrap()
        };

        let fallback = run_fallback();
        let first = run_optimizer();
        let second = run_optimizer();
        assert_eq!(first, second);
        let fallback_rank = CanonicalRank::evaluate(&input, &fallback, true);
        let optimized_rank = CanonicalRank::evaluate(&input, &first, true);
        assert!(!fallback_rank.is_better_than(&optimized_rank));
    }

    #[test]
    fn production_concave_search_is_input_order_invariant_at_completed_passes() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        // Exact geometry and seed from the retained 100 ms differential failure.
        // Fixed work isolates input identity from deadline/scheduler variation;
        // the unchanged wall-clock portfolio gate remains separately required.
        let polygon = |points: &[(f64, f64)]| {
            Polygon(
                points.iter().map(|&(x, y)| VacNestingPoint { x, y }).collect(),
            )
        };
        let mut input = rectangular_input(
            polygon(&[
                (0., 0.), (90., 0.), (90., 24.), (38., 24.), (38., 82.), (0., 82.),
            ]),
            vec![
                (101, rectangle(120., 0., 30., 18.)),
                (102, polygon(&[
                    (170., 0.), (194., 0.), (194., 8.), (181., 8.), (181., 24.), (170., 24.),
                ])),
                (103, rectangle(215., 0., 16., 30.)),
            ],
        );
        input.options.random_seed = 0x5eed_1234_89ab_cdef;
        input.options.time_limit_ms = 0;
        let run = |candidate_input: &SolverInput, passes| {
            let cancelled = AtomicBool::new(false);
            let control = RunControl::for_tests(&cancelled, candidate_input.parts.len());
            let placements = solve_with_limit(
                candidate_input,
                &control,
                RefinementLimit::CompletedPasses(passes),
            )
            .unwrap();
            validate_solver_results(candidate_input, &placements).unwrap();
            assert!(
                authoritative_portfolio_validation(candidate_input, &placements, &control).unwrap()
            );
            assert!(placements.iter().all(|placement| placement.placed == 1));
            placements
        };
        // Include every pass family and later seeded sample regions. Checking
        // each prefix prevents a later improvement from hiding early divergence.
        for passes in [1, 2, 3, 4, 8, 16, 32, 64, 128, 256] {
            let original = run(&input, passes);
            assert_eq!(original, run(&input, passes), "same-order control, {passes} passes");
            for permutation in [[2, 1, 0], [1, 2, 0]] {
                let permuted_input = permute_solver_input(&input, &permutation);
                let permuted = run(&permuted_input, passes);
                assert_eq!(
                    CanonicalRank::evaluate(&input, &original, true).quality_cmp(
                        &CanonicalRank::evaluate(&permuted_input, &permuted, true),
                    ),
                    std::cmp::Ordering::Equal,
                    "canonical rank, {passes} passes, {permutation:?}",
                );
                assert_eq!(
                    placements_by_id(&original), placements_by_id(&permuted),
                    "stable-ID transforms, {passes} passes, {permutation:?}",
                );
            }
        }
    }

    #[test]
    fn optimizer_search_is_input_order_metamorphic_by_stable_part_id() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = rectangular_input(
            rectangle(-5.0, -7.0, 38.0, 28.0),
            vec![
                (40, rectangle(90.0, 10.0, 8.0, 7.0)),
                (10, rectangle(-30.0, 20.0, 8.0, 7.0)),
                (30, rectangle(15.0, 80.0, 8.0, 7.0)),
                (20, rectangle(120.0, -40.0, 8.0, 7.0)),
            ],
        );
        input.options.random_seed = 0xdead_beef;
        input.options.time_limit_ms = 60_000;
        input.options.quality = QUALITY_DRAFT;
        let permutation = [2, 0, 3, 1];
        let permuted_input = permute_solver_input(&input, &permutation);

        let run = |candidate_input: &SolverInput| {
            let cancelled = AtomicBool::new(false);
            let control = RunControl::for_tests(&cancelled, candidate_input.parts.len());
            optimize_fixed_container_from_baseline(
                candidate_input,
                &control,
                empty_results(candidate_input),
                3,
                4,
                false,
            )
            .unwrap()
        };

        let original = run(&input);
        let permuted = run(&permuted_input);
        assert!(original.iter().all(|placement| placement.placed == 1));
        assert_eq!(placements_by_id(&original), placements_by_id(&permuted));
        assert_eq!(
            CanonicalRank::evaluate(&input, &original, true).quality_cmp(&CanonicalRank::evaluate(
                &permuted_input,
                &permuted,
                true
            )),
            std::cmp::Ordering::Equal
        );
    }

    #[test]
    fn finite_time_limit_is_a_real_wall_clock_budget() {
        let mut options = draft_options(ROTATION_FREE);
        options.time_limit_ms = 25;
        let job = configured(options, rectangle(0.0, 0.0, 80.0, 55.0));
        for id in 0..6 {
            assert_eq!(
                job.add_part(id, rectangle(id as f64 * 17.0, 100.0, 14.0, 9.0)),
                STATUS_OK
            );
        }

        let started = std::time::Instant::now();
        assert_eq!(job.run_with_wall_clock_deadline(), STATUS_OK);
        let elapsed = started.elapsed();

        assert!(
            elapsed >= std::time::Duration::from_millis(20),
            "the 25 ms budget returned prematurely after {elapsed:?}"
        );
        assert!(
            elapsed < std::time::Duration::from_secs(2),
            "the 25 ms budget overran unexpectedly: {elapsed:?}"
        );
    }

    #[test]
    fn oversized_discrete_rotation_set_is_rejected() {
        let mut input = sample_input();
        input.options.rotation_mode = ROTATION_DISCRETE;
        input.options.rotation_step_degrees = 0.01;
        assert!(matches!(
            rotation_range(&input),
            Err(SolverError::InvalidInput(_))
        ));
    }

    #[test]
    fn packs_multiple_parts_without_overlap() {
        let options = draft_options(ROTATION_NONE);
        let job = configured(options, rectangle(0.0, 0.0, 50.0, 25.0));
        assert_eq!(
            job.add_part(10, rectangle(100.0, 40.0, 20.0, 20.0)),
            STATUS_OK
        );
        assert_eq!(
            job.add_part(11, rectangle(-50.0, -20.0, 20.0, 20.0)),
            STATUS_OK
        );

        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        assert_eq!(job.state(), STATE_COMPLETED);
        let first = job.result_at(0).unwrap();
        let second = job.result_at(1).unwrap();
        assert_eq!((first.placed, second.placed), (1, 1));

        let first_bbox = transformed_bbox(&rectangle(100.0, 40.0, 20.0, 20.0), first);
        let second_bbox = transformed_bbox(&rectangle(-50.0, -20.0, 20.0, 20.0), second);
        assert!(inside(first_bbox, BBox::new(0.0, 0.0, 50.0, 25.0), 1.0e-3));
        assert!(inside(second_bbox, BBox::new(0.0, 0.0, 50.0, 25.0), 1.0e-3));
        assert!(!overlaps(first_bbox, second_bbox, 1.0e-3));
    }

    #[test]
    fn marks_oversized_part_unplaced_and_continues_with_smaller_parts() {
        let job = configured(
            draft_options(ROTATION_NONE),
            rectangle(0.0, 0.0, 30.0, 30.0),
        );
        assert_eq!(job.add_part(1, rectangle(0.0, 0.0, 40.0, 40.0)), STATUS_OK);
        assert_eq!(job.add_part(2, rectangle(0.0, 0.0, 10.0, 10.0)), STATUS_OK);

        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        assert_eq!(job.result_count(), 2);
        assert_eq!(job.result_at(0).unwrap().placed, 0);
        assert_eq!(job.result_at(1).unwrap().placed, 1);
    }

    #[test]
    fn right_angle_and_free_rotation_enable_a_fit() {
        for rotation_mode in [ROTATION_NONE, ROTATION_RIGHT_ANGLES, ROTATION_FREE] {
            let job = configured(
                draft_options(rotation_mode),
                rectangle(0.0, 0.0, 12.0, 22.0),
            );
            let part = rectangle(30.0, 50.0, 20.0, 10.0);
            assert_eq!(job.add_part(7, part.clone()), STATUS_OK);
            assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
            let placement = job.result_at(0).unwrap();
            if rotation_mode == ROTATION_NONE {
                assert_eq!(placement.placed, 0);
            } else {
                assert_eq!(placement.placed, 1);
                let bbox = transformed_bbox(&part, placement);
                assert!(inside(bbox, BBox::new(0.0, 0.0, 12.0, 22.0), 1.0e-3));
                assert!(
                    (placement.rotation_degrees - 90.0).abs() < 1.0e-3
                        || (placement.rotation_degrees - 270.0).abs() < 1.0e-3
                );
            }
        }
    }

    #[test]
    fn translated_copies_share_one_collision_shape() {
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 2000.0, 2000.0),
            vec![
                (1, wavy_blob(100.1, 100.7, 60.0, 900)),
                (2, wavy_blob(1033.3, 97.9, 60.0, 900)),
                (3, wavy_blob(411.17, 1520.03, 60.0, 900)),
                (4, wavy_blob(600.0, 600.0, 61.0, 900)),
            ],
        );
        input.options.part_spacing = 3.78;
        input.options.quality = QUALITY_BALANCED;
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let problem = prepare_problem(&input, &control).unwrap();
        let shape = |index: usize| &problem.items[index].components[0].shape_cd;
        assert!(Arc::ptr_eq(shape(0), shape(1)));
        assert!(Arc::ptr_eq(shape(0), shape(2)));
        assert!(!Arc::ptr_eq(shape(0), shape(3)));
    }

    #[test]
    fn avoids_container_holes() {
        let job = configured(
            draft_options(ROTATION_NONE),
            rectangle(0.0, 0.0, 30.0, 30.0),
        );
        let hole = rectangle(0.0, 0.0, 20.0, 20.0);
        assert_eq!(job.add_hole(hole.clone()), STATUS_OK);
        let part = rectangle(0.0, 0.0, 8.0, 8.0);
        assert_eq!(job.add_part(8, part.clone()), STATUS_OK);

        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        let placement = job.result_at(0).unwrap();
        assert_eq!(placement.placed, 1);
        let placed_bbox = transformed_bbox(&part, placement);
        assert!(!overlaps(
            placed_bbox,
            BBox::new(0.0, 0.0, 20.0, 20.0),
            1.0e-3
        ));
    }

    #[test]
    fn exact_collision_rejects_the_missing_quadrant_of_a_concave_container() {
        let l_shaped_container = Polygon(vec![
            VacNestingPoint { x: 0.0, y: 0.0 },
            VacNestingPoint { x: 30.0, y: 0.0 },
            VacNestingPoint { x: 30.0, y: 10.0 },
            VacNestingPoint { x: 10.0, y: 10.0 },
            VacNestingPoint { x: 10.0, y: 30.0 },
            VacNestingPoint { x: 0.0, y: 30.0 },
        ]);
        let job = configured(draft_options(ROTATION_NONE), l_shaped_container);
        assert_eq!(job.add_part(1, rectangle(0.0, 0.0, 12.0, 12.0)), STATUS_OK);
        assert_eq!(job.add_part(2, rectangle(0.0, 0.0, 8.0, 8.0)), STATUS_OK);

        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        assert_eq!(job.result_at(0).unwrap().placed, 0);
        let small = job.result_at(1).unwrap();
        assert_eq!(small.placed, 1);
        let bbox = transformed_bbox(&rectangle(0.0, 0.0, 8.0, 8.0), small);
        assert!(bbox.x_max <= 10.001 || bbox.y_max <= 10.001);
    }

    #[test]
    fn packs_a_regular_twenty_five_part_stress_case_without_overlap() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let part = rectangle(0.0, 0.0, 8.0, 8.0);
        let mut options = draft_options(ROTATION_NONE);
        options.time_limit_ms = 1_000;
        let job = configured(options, rectangle(0.0, 0.0, 50.0, 50.0));
        for id in 0..25 {
            assert_eq!(job.add_part(id, part.clone()), STATUS_OK);
        }

        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        let placements = (0..job.result_count())
            .map(|index| job.result_at(index).unwrap())
            .collect::<Vec<_>>();
        assert_eq!(placements.len(), 25);
        assert!(placements.iter().all(|placement| placement.placed == 1));

        let bboxes = placements
            .into_iter()
            .map(|placement| transformed_bbox(&part, placement))
            .collect::<Vec<_>>();
        for (index, bbox) in bboxes.iter().copied().enumerate() {
            assert!(inside(bbox, BBox::new(0.0, 0.0, 50.0, 50.0), 1.0e-3));
            for other in bboxes.iter().copied().skip(index + 1) {
                assert!(!overlaps(bbox, other, 1.0e-3));
            }
        }
    }

    #[test]
    #[ignore = "extended 10,000-case release property suite"]
    fn ten_thousand_randomized_rectangle_cases_remain_feasible() {
        let mut random_state = 0x56ac_a4d5_13b7_9e21_u64;
        for case_index in 0_u64..10_000 {
            let container_x = random_between(&mut random_state, -500.0, 500.0);
            let container_y = random_between(&mut random_state, -500.0, 500.0);
            let container_width = random_between(&mut random_state, 12.0, 120.0);
            let container_height = random_between(&mut random_state, 12.0, 120.0);
            let container_bounds = BBox::new(
                container_x,
                container_y,
                container_x + container_width,
                container_y + container_height,
            );
            // The extended gate validates every placement, not optimization
            // duration. Keep each independent case bounded so 10,000 jobs do
            // not each consume the user-facing time budget.
            let mut options = draft_options(ROTATION_NONE);
            options.time_limit_ms = 1;
            let job = configured(
                options,
                rectangle(container_x, container_y, container_width, container_height),
            );
            let parts = [
                rectangle(
                    random_between(&mut random_state, -1_000.0, 1_000.0),
                    random_between(&mut random_state, -1_000.0, 1_000.0),
                    random_between(&mut random_state, 1.0, 90.0),
                    random_between(&mut random_state, 1.0, 90.0),
                ),
                rectangle(
                    random_between(&mut random_state, -1_000.0, 1_000.0),
                    random_between(&mut random_state, -1_000.0, 1_000.0),
                    random_between(&mut random_state, 1.0, 90.0),
                    random_between(&mut random_state, 1.0, 90.0),
                ),
            ];
            for (part_index, part) in parts.iter().enumerate() {
                assert_eq!(
                    job.add_part(case_index * 2 + part_index as u64, part.clone()),
                    STATUS_OK,
                    "case {case_index}, part {part_index}"
                );
            }

            assert_eq!(
                job.run(None, ptr::null_mut()),
                STATUS_OK,
                "case {case_index}"
            );
            assert_eq!(job.result_count(), parts.len(), "case {case_index}");

            let mut placed_bounds = Vec::new();
            for (part_index, part) in parts.iter().enumerate() {
                let placement = job.result_at(part_index).unwrap();
                if placement.placed == 0 {
                    continue;
                }
                let bounds = transformed_bbox(part, placement);
                assert!(
                    inside(bounds, container_bounds, 1.0e-3),
                    "case {case_index}, part {part_index}: {bounds:?} is outside {container_bounds:?}"
                );
                for prior in placed_bounds.iter().copied() {
                    assert!(
                        !overlaps(bounds, prior, 1.0e-3),
                        "case {case_index}, part {part_index}: {bounds:?} overlaps {prior:?}"
                    );
                }
                placed_bounds.push(bounds);
            }
        }
    }

    #[test]
    fn enforces_container_margin_and_part_spacing() {
        let mut options = draft_options(ROTATION_NONE);
        options.container_margin = 1.0;
        options.part_spacing = 2.0;
        let job = configured(options, rectangle(0.0, 0.0, 35.0, 18.0));
        let part = rectangle(0.0, 0.0, 10.0, 10.0);
        assert_eq!(job.add_part(1, part.clone()), STATUS_OK);
        assert_eq!(job.add_part(2, part.clone()), STATUS_OK);

        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        let first = transformed_bbox(&part, job.result_at(0).unwrap());
        let second = transformed_bbox(&part, job.result_at(1).unwrap());
        // The margin alone is the wall: parts may sit exactly 1 from the edge.
        let usable_area = BBox::new(0.999, 0.999, 34.001, 17.001);
        assert!(
            inside(first, usable_area, 1.0e-3),
            "first part {first:?} is outside {usable_area:?}"
        );
        assert!(
            inside(second, usable_area, 1.0e-3),
            "second part {second:?} is outside {usable_area:?}"
        );
        assert!(
            bbox_distance(first, second) >= 1.999,
            "part clearance is {}, expected at least 2",
            bbox_distance(first, second)
        );
    }

    // ---- NEST-S1: bounded-cost spacing offsets ----

    fn wavy_blob(center_x: f64, center_y: f64, radius: f64, vertices: usize) -> Polygon {
        Polygon(
            (0..vertices)
                .map(|index| {
                    let angle = std::f64::consts::TAU * index as f64 / vertices as f64;
                    let r = radius
                        * (1.0
                            + 0.15 * (5.0 * angle).sin()
                            + 0.04 * (23.0 * angle).cos()
                            + 0.01 * (71.0 * angle).sin());
                    VacNestingPoint {
                        x: center_x + r * angle.cos(),
                        y: center_y + r * angle.sin(),
                    }
                })
                .collect(),
        )
    }

    fn inflated_original(polygon: &Polygon, offset: f64, quality: i32) -> OriginalShape {
        OriginalShape {
            shape: polygon_to_simple(polygon, "test").unwrap(),
            pre_transform: DTransformation::empty(),
            modify_mode: ShapeModifyMode::Inflate,
            modify_config: modify_config(
                offset,
                SolverProfile::from_quality(quality).simplify_tolerance,
            ),
        }
    }

    #[test]
    fn bounded_inflate_keeps_full_clearance_with_far_fewer_vertices() {
        for (vertices, spacing) in [(200, 0.38), (800, 3.78), (2000, 7.56), (3000, 1.0)] {
            let polygon = wavy_blob(300.0, 300.0, 80.0, vertices);
            let original = inflated_original(&polygon, spacing / 2.0, QUALITY_BALANCED);
            let started = Instant::now();
            let bounded = bounded_inflate(&original).expect("bounded path applies");
            // Wall-clock limit for optimized builds only (owner decision 2026-09-26).
            assert!(
                cfg!(debug_assertions) || started.elapsed() < Duration::from_secs(1),
                "{vertices} vertices took {:?}",
                started.elapsed()
            );
            let source = original.shape.clone();
            assert!(inflation_is_conservative(&source, &bounded, spacing / 2.0));
            assert!(bounded.n_vertices() < 2 * vertices.max(400));
        }
    }

    #[test]
    fn bounded_inflate_leaves_simple_and_zero_offset_shapes_on_the_exact_path() {
        let rectangle = rectangle(0.0, 0.0, 336.0, 192.0);
        assert!(bounded_inflate(&inflated_original(&rectangle, 3.78, QUALITY_BALANCED)).is_none());
        let blob = wavy_blob(0.0, 0.0, 80.0, 800);
        assert!(bounded_inflate(&inflated_original(&blob, 0.0, QUALITY_BALANCED)).is_none());
    }

    #[test]
    fn conservative_inflated_ring_bounds_detailed_rings() {
        let points = (0..4000)
            .map(|index| {
                let angle = std::f64::consts::TAU * index as f64 / 4000.0;
                let radius = 50.0 * (1.0 + 0.25 * (7.0 * angle).sin() + 0.1 * (23.0 * angle).sin());
                (radius * angle.cos(), radius * angle.sin())
            })
            .collect::<Vec<_>>();
        let started = Instant::now();
        let result = conservative_inflated_ring(&points, 1.0).expect("verified proxy");
        let elapsed = started.elapsed();
        eprintln!("conservative_inflated_ring_bounds_detailed_rings: vertices={} elapsed={elapsed:?}", result.len());
        assert!(result.len() <= 1500);
        let source = SPolygon::new(points.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect()).unwrap();
        let inflated = SPolygon::new(result.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect()).unwrap();
        assert!(inflation_is_conservative(&source, &inflated, 1.0));
    }

    #[test]
    fn conservative_inflated_ring_simplifies_detailed_rings_at_a_tiny_offset() {
        let points = (0..4000)
            .map(|index| {
                let angle = std::f64::consts::TAU * index as f64 / 4000.0;
                let radius = 50.0 * (1.0 + 0.25 * (7.0 * angle).sin() + 0.1 * (23.0 * angle).sin());
                (radius * angle.cos(), radius * angle.sin())
            })
            .collect::<Vec<_>>();
        let result = conservative_inflated_ring(&points, 0.05).expect("verified proxy");
        eprintln!("tiny offset proxy vertices={}", result.len());
        assert!(result.len() <= 1000);
        let source = SPolygon::new(points.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect()).unwrap();
        let inflated = SPolygon::new(result.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect()).unwrap();
        assert!(inflation_is_conservative(&source, &inflated, 0.05));
    }

    #[test]
    fn conservative_inflated_ring_widens_for_large_detailed_rings() {
        // About 900 units across with fine 4-unit teeth: Douglas-Peucker at the
        // capped coarse allowance (1.0) keeps more than 1500 vertices; a wider
        // allowance (up to 0.5% of the diagonal, 6.4 here) flattens them.
        let points = (0..8000)
            .map(|index| {
                let angle = std::f64::consts::TAU * index as f64 / 8000.0;
                let radius = 450.0 + 2.0 * (1000.0 * angle).sin();
                (radius * angle.cos(), radius * angle.sin())
            })
            .collect::<Vec<_>>();
        assert!(douglas_peucker_ring(&points, 1.0).len() > PROXY_MAX_OFFSET_VERTICES);
        let result = conservative_inflated_ring(&points, 0.05).expect("verified proxy");
        eprintln!("large detailed proxy vertices={}", result.len());
        let source = SPolygon::new(points.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect()).unwrap();
        let inflated = SPolygon::new(result.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect()).unwrap();
        assert!(inflation_is_conservative(&source, &inflated, 0.05));
        assert!(conservative_inflated_ring_until(&points, 0.05, &|| true).is_none());
    }

    #[test]
    fn conservative_inflated_ring_keeps_small_rings_exact_enough() {
        let points = [(0.0, 0.0), (10.0, 0.0), (10.0, 10.0), (0.0, 10.0)];
        let result = conservative_inflated_ring(&points, 0.5).expect("verified proxy");
        let inflated = SPolygon::new(result.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect()).unwrap();
        for &(x, y) in &points {
            assert!(inflated.collides_with(&Point(x as f32, y as f32)));
        }
        for &(x, y) in &result {
            assert!((-0.55..=10.55).contains(&x));
            assert!((-0.55..=10.55).contains(&y));
        }
    }

    #[test]
    fn collision_proxies_match_the_validation_envelopes() {
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 1000.0, 1000.0),
            vec![
                (1, wavy_blob(200.0, 200.0, 40.0, 1500)),
                (2, wavy_blob(500.0, 300.0, 40.0, 1500)),
                (3, wavy_blob(700.0, 650.0, 40.0, 1500)),
                (4, wavy_blob(350.0, 700.0, 55.0, 1500)),
            ],
        );
        input.options.part_spacing = 2.0;
        input.options.quality = QUALITY_BALANCED;
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let proxies = collision_proxies(&input, 0.05, &control).unwrap();
        assert_eq!(proxies.rings.len(), 2);
        assert_eq!(proxies.part_group, [0, 0, 0, 1]);
        eprintln!("collision_proxies_match_the_validation_envelopes: ring vertices={:?}",
                  proxies.rings.iter().map(Vec::len).collect::<Vec<_>>());
        let mut profile = SolverProfile::from_quality(input.options.quality);
        profile.search_surrogates = false;
        let items = build_items(&input, profile, &control, false).unwrap();
        for (index, item) in items.iter().enumerate() {
            let component = &item.components[0];
            let proxy = &proxies.rings[proxies.part_group[index]];
            let inflated = SPolygon::new(proxy.iter().map(|&(x, y)| Point(x as f32, y as f32)).collect()).unwrap();
            assert!(inflation_is_conservative(&component.shape_cd, &inflated, 0.05 - 1e-3));
            let (tx, ty) = component.shape_orig.pre_transform.translation();
            assert_eq!(proxies.part_center[index], (-f64::from(tx), -f64::from(ty)));
            assert!(proxy.len() < 1000);
        }
    }

    #[test]
    fn global_collision_shapes_reuse_and_spacing_separates() {
        let mut input = rectangular_input(rectangle(0.0, 0.0, 100.0, 100.0),
            vec![(1, rectangle(0.0, 0.0, 10.0, 10.0))]);
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, 1);
        let profile = SolverProfile::from_quality(input.options.quality);
        let first = build_items(&input, profile, &control, false).unwrap();
        let second = build_items(&input, profile, &control, false).unwrap();
        let a = &first[0].components[0].shape_cd;
        let b = &second[0].components[0].shape_cd;
        assert!(Arc::ptr_eq(a, b));
        assert_eq!(a.vertices, b.vertices);
        input.options.part_spacing += 2.0;
        let third = build_items(&input, profile, &control, false).unwrap();
        assert!(!Arc::ptr_eq(a, &third[0].components[0].shape_cd));
    }

    #[test]
    fn global_collision_proxies_reuse_cached_arc() {
        let input = rectangular_input(rectangle(0.0, 0.0, 100.0, 100.0),
            vec![(1, rectangle(0.0, 0.0, 10.0, 10.0))]);
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, 1);
        let first = collision_proxies(&input, 0.137, &control).unwrap();
        let mut profile = SolverProfile::from_quality(input.options.quality);
        profile.search_surrogates = false;
        let items = build_items(&input, profile, &control, false).unwrap();
        let key = (items[0].components[0].shape_cd.vertices.iter()
            .flat_map(|p| [p.0.to_bits(), p.1.to_bits()]).collect(), 0.137f64.to_bits());
        let before = proxy_cache().lock().unwrap().get(&key).unwrap();
        let computations_before = PROXY_COMPUTATIONS.with(|count| count.get());
        let second = collision_proxies(&input, 0.137, &control).unwrap();
        assert_eq!(PROXY_COMPUTATIONS.with(|count| count.get()), computations_before);
        let after = proxy_cache().lock().unwrap().get(&key).unwrap();
        assert_eq!(first.rings, second.rings);
        assert!(Arc::ptr_eq(&before, &after));
    }

    #[test]
    fn bounded_cache_evicts_257th_small_entry() {
        let mut cache = BoundedCache::<usize, Vec<u8>>::new();
        for index in 0..257 { cache.insert(index, Arc::new(vec![0]), 1); }
        assert_eq!(cache.entries.len(), 256);
        assert!(cache.get(&0).is_none());
    }

    #[test]
    fn conservative_inflated_ring_rejects_bad_offsets() {
        let points = [(0.0, 0.0), (10.0, 0.0), (0.0, 10.0)];
        for offset in [0.0, -1.0, f64::NAN] {
            assert!(conservative_inflated_ring(&points, offset).is_none());
        }
    }

    #[test]
    fn conservative_check_rejects_an_envelope_that_is_too_tight() {
        let polygon = wavy_blob(0.0, 0.0, 80.0, 400);
        let source = polygon_to_simple(&polygon, "test").unwrap();
        let tight = offset_shape(&source, ShapeModifyMode::Inflate, 1.0).unwrap();
        assert!(!inflation_is_conservative(&source, &tight, 2.0));
        assert!(!inflation_is_conservative(&source, &source, 0.5));
    }

    #[test]
    fn spaced_high_vertex_parts_are_placed_within_the_time_limit() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut options = draft_options(ROTATION_FREE);
        options.quality = QUALITY_BALANCED;
        options.worker_count = 1;
        options.time_limit_ms = 5_000;
        options.part_spacing = 3.78;
        let job = configured(options, rectangle(0.0, 0.0, 793.7, 1122.5));
        for id in 0..6_u64 {
            let radius = 60.0 * (1.0 + 0.01 * id as f64);
            assert_eq!(
                job.add_part(id, wavy_blob(900.0 + 10.0 * id as f64, 100.0, radius, 3000)),
                STATUS_OK
            );
        }
        let started = Instant::now();
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        // Wall-clock limit for optimized builds only (owner decision 2026-09-26).
        assert!(
            cfg!(debug_assertions) || started.elapsed() < Duration::from_millis(6_000),
            "took {:?}",
            started.elapsed()
        );
        assert_eq!(
            (0..6)
                .filter(|&index| job.result_at(index).unwrap().placed != 0)
                .count(),
            6
        );
    }

    #[test]
    fn sheet_margin_is_the_only_edge_clearance() {
        for (margin, width, height) in [(1.0, 24.1, 12.1), (0.0, 22.1, 10.1)] {
            let mut options = draft_options(ROTATION_NONE);
            options.worker_count = 1;
            options.time_limit_ms = 500;
            options.part_spacing = 2.0;
            options.container_margin = margin;
            let job = configured(options, rectangle(0.0, 0.0, width, height));
            let part = rectangle(0.0, 0.0, 10.0, 10.0);
            assert_eq!(job.add_part(1, part.clone()), STATUS_OK);
            assert_eq!(job.add_part(2, part.clone()), STATUS_OK);
            assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
            let first = job.result_at(0).unwrap();
            let second = job.result_at(1).unwrap();
            assert!(
                first.placed != 0 && second.placed != 0,
                "margin {margin}: both must fit"
            );
            let wall = BBox::new(
                margin - 1.0e-3,
                margin - 1.0e-3,
                width - margin + 1.0e-3,
                height - margin + 1.0e-3,
            );
            let first = transformed_bbox(&part, first);
            let second = transformed_bbox(&part, second);
            assert!(inside(first, wall, 1.0e-3) && inside(second, wall, 1.0e-3));
            assert!(bbox_distance(first, second) >= 2.0 - 1.0e-3);
        }
    }

    #[test]
    fn container_growth_is_exact_for_convex_and_skipped_for_concave() {
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, 1);
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 30.0, 20.0),
            vec![(1, rectangle(0.0, 0.0, 5.0, 5.0))],
        );
        input.options.part_spacing = 2.0;
        let problem = prepare_problem(&input, &control).unwrap();
        let outer = &problem.container.outer_cd;
        assert_eq!(outer.n_vertices(), 4);
        assert!(
            (outer.bbox.x_min + 1.0).abs() < 1.0e-4 && (outer.bbox.x_max - 31.0).abs() < 1.0e-4
        );
        assert!(
            (outer.bbox.y_min + 1.0).abs() < 1.0e-4 && (outer.bbox.y_max - 21.0).abs() < 1.0e-4
        );

        input.container = Polygon(vec![
            VacNestingPoint { x: 0.0, y: 0.0 },
            VacNestingPoint { x: 30.0, y: 0.0 },
            VacNestingPoint { x: 30.0, y: 10.0 },
            VacNestingPoint { x: 10.0, y: 10.0 },
            VacNestingPoint { x: 10.0, y: 20.0 },
            VacNestingPoint { x: 0.0, y: 20.0 },
        ]);
        let problem = prepare_problem(&input, &control).unwrap();
        let outer = &problem.container.outer_cd;
        assert_eq!(outer.n_vertices(), 6);
        assert!((outer.area - 400.0).abs() < 1.0e-3);
    }

    #[test]
    fn identical_seed_and_input_produce_identical_results() {
        let solve = || {
            let mut options = draft_options(ROTATION_FREE);
            options.random_seed = 991;
            let job = configured(options, rectangle(-10.0, -20.0, 80.0, 55.0));
            for id in 0..6 {
                assert_eq!(
                    job.add_part(id, rectangle(id as f64 * 17.0, 100.0, 14.0, 9.0)),
                    STATUS_OK
                );
            }
            assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
            (0..job.result_count())
                .map(|index| job.result_at(index).unwrap())
                .collect::<Vec<_>>()
        };
        assert_eq!(solve(), solve());
    }

    #[test]
    fn reports_original_coordinate_transform_for_off_origin_input() {
        let job = configured(
            draft_options(ROTATION_NONE),
            rectangle(0.0, 0.0, 25.0, 25.0),
        );
        let part = rectangle(500.0, -300.0, 10.0, 10.0);
        assert_eq!(job.add_part(5, part.clone()), STATUS_OK);
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        let placement = job.result_at(0).unwrap();
        assert_eq!(placement.placed, 1);
        assert!(inside(
            transformed_bbox(&part, placement),
            BBox::new(0.0, 0.0, 25.0, 25.0),
            1.0e-3
        ));
    }

    #[test]
    fn rejects_self_intersecting_geometry_without_partial_results() {
        let job = configured(
            draft_options(ROTATION_NONE),
            rectangle(0.0, 0.0, 30.0, 30.0),
        );
        let crossing = Polygon(vec![
            VacNestingPoint { x: 0.0, y: 0.0 },
            VacNestingPoint { x: 10.0, y: 10.0 },
            VacNestingPoint { x: 0.0, y: 10.0 },
            VacNestingPoint { x: 8.0, y: 0.0 },
        ]);
        assert_eq!(job.add_part(6, crossing), STATUS_OK);
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_INVALID_ARGUMENT);
        assert_eq!(job.state(), STATE_FAILED);
        assert_eq!(job.result_count(), 0);
        // SAFETY: The job remains alive and is not mutated while this borrowed
        // error string is inspected.
        let error = unsafe { CStr::from_ptr(job.error_ptr()) };
        assert!(error.to_string_lossy().contains("simple polygon"));
    }

    #[test]
    fn expired_deadline_still_publishes_the_first_layout() {
        let mut options = draft_options(ROTATION_NONE);
        options.worker_count = 1;
        options.time_limit_ms = 1;
        let job = configured(options, rectangle(0.0, 0.0, 100.0, 100.0));
        for id in 0..8_u64 {
            assert_eq!(
                job.add_part(
                    id,
                    wavy_blob(500.0 + 40.0 * id as f64, 500.0, 10.0 + id as f64, 1500)
                ),
                STATUS_OK
            );
        }
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        assert!((0..8).any(|index| job.result_at(index).unwrap().placed != 0));
    }

    #[test]
    fn tiny_time_limit_returns_a_complete_explicit_result_vector() {
        let mut options = draft_options(ROTATION_FREE);
        options.time_limit_ms = 1;
        let job = configured(options, rectangle(0.0, 0.0, 100.0, 100.0));
        for id in 0..100 {
            assert_eq!(job.add_part(id, rectangle(0.0, 0.0, 7.0, 7.0)), STATUS_OK);
        }
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        assert_eq!(job.state(), STATE_COMPLETED);
        assert_eq!(job.result_count(), 100);
    }

    #[test]
    fn identical_cards_use_the_denser_two_block_grid() {
        let mut options = draft_options(ROTATION_RIGHT_ANGLES);
        options.worker_count = 1;
        options.time_limit_ms = 500;
        options.part_spacing = 7.56;
        options.container_margin = 18.9;
        let job = configured(options, rectangle(0.0, 0.0, 793.7, 1122.5));
        let card = rectangle(0.0, 0.0, 336.0, 192.0);
        for id in 0..12_u64 {
            assert_eq!(job.add_part(id, card.clone()), STATUS_OK);
        }
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        let placed = (0..12)
            .map(|index| job.result_at(index).unwrap())
            .filter(|placement| placement.placed != 0)
            .collect::<Vec<_>>();
        // One upright column of five plus two rotated columns of three.
        assert!(placed.len() >= 11, "placed {}", placed.len());
        // The sheet margin alone is the wall (spacing applies between parts).
        let edge = 18.9 - 1.0e-3;
        let usable = BBox::new(edge, edge, 793.7 - edge, 1122.5 - edge);
        let boxes = placed
            .iter()
            .map(|placement| transformed_bbox(&card, *placement))
            .collect::<Vec<_>>();
        for (index, bounds) in boxes.iter().enumerate() {
            assert!(inside(*bounds, usable, 1.0e-3), "card {index} {bounds:?}");
            for other in &boxes[index + 1..] {
                assert!(bbox_distance(*bounds, *other) >= 7.56 - 1.0e-3);
            }
        }
    }

    #[test]
    fn grid_seed_is_not_used_for_mixed_parts_or_containers_with_holes() {
        let cancelled = AtomicBool::new(false);
        let mut mixed = rectangular_input(
            rectangle(0.0, 0.0, 100.0, 100.0),
            vec![
                (1, rectangle(0.0, 0.0, 10.0, 10.0)),
                (2, rectangle(0.0, 0.0, 10.0, 11.0)),
            ],
        );
        mixed.options.rotation_mode = ROTATION_RIGHT_ANGLES;
        let control = RunControl::for_tests(&cancelled, 2);
        let problem = prepare_problem(&mixed, &control).unwrap();
        assert!(
            identical_part_grid_seed(&mixed, &problem, &control)
                .unwrap()
                .is_none()
        );

        let mut holed = rectangular_input(
            rectangle(0.0, 0.0, 100.0, 100.0),
            vec![
                (1, rectangle(0.0, 0.0, 10.0, 10.0)),
                (2, rectangle(0.0, 0.0, 10.0, 10.0)),
            ],
        );
        holed.holes.push(rectangle(40.0, 40.0, 10.0, 10.0));
        let problem = prepare_problem(&holed, &control).unwrap();
        assert!(
            identical_part_grid_seed(&holed, &problem, &control)
                .unwrap()
                .is_none()
        );
    }

    fn staircase_ring(radius: f64, pixel: f64) -> Polygon {
        // A hard-alpha trace: the boundary snapped to a pixel grid and joined
        // with axis-aligned steps, collinear points removed.
        let mut points: Vec<(f64, f64)> = Vec::new();
        let samples = (std::f64::consts::TAU * radius * 5.2 / pixel) as usize;
        for index in 0..samples {
            let angle = std::f64::consts::TAU * index as f64 / samples as f64;
            let r = radius * (1.0 + 0.15 * (5.0 * angle).sin() + 0.04 * (23.0 * angle).cos());
            let cell = (
                (r * angle.cos() / pixel).round(),
                (r * angle.sin() / pixel).round(),
            );
            if let Some(&last) = points.last() {
                if last == cell {
                    continue;
                }
                if last.0 != cell.0 && last.1 != cell.1 {
                    points.push((cell.0, last.1));
                }
            }
            points.push(cell);
        }
        while points.len() > 1 && points.last() == points.first() {
            points.pop();
        }
        let count = points.len();
        let kept = (0..count)
            .filter(|&index| {
                let (a, b, c) = (
                    points[(index + count - 1) % count],
                    points[index],
                    points[(index + 1) % count],
                );
                (b.0 - a.0) * (c.1 - b.1) - (b.1 - a.1) * (c.0 - b.0) != 0.0
            })
            .map(|index| VacNestingPoint {
                x: 500.0 + points[index].0 * pixel,
                y: 500.0 + points[index].1 * pixel,
            })
            .collect();
        Polygon(kept)
    }

    fn compound_part(id: u64, outers: Vec<Polygon>) -> Part {
        Part {
            id,
            components: outers
                .into_iter()
                .map(|outer| crate::job::PartComponent {
                    outer,
                    holes: Vec::new(),
                })
                .collect(),
        }
    }

    #[test]
    fn covered_components_are_dropped_and_touching_ones_kept() {
        let shapes = [
            rectangle(0.0, 0.0, 100.0, 60.0),  // bitmap
            rectangle(10.0, 10.0, 20.0, 20.0), // text inside
            rectangle(90.0, 10.0, 20.0, 20.0), // crosses the edge
            rectangle(0.0, 70.0, 10.0, 10.0),  // separate island
            rectangle(0.0, 0.0, 100.0, 60.0),  // duplicate of the bitmap
            rectangle(0.0, 20.0, 10.0, 10.0),  // touches the bitmap edge
        ]
        .iter()
        .map(|polygon| polygon_to_simple(polygon, "test").unwrap())
        .collect::<Vec<_>>();
        assert_eq!(uncovered_components(&shapes), vec![0, 2, 3, 5]);
    }

    #[test]
    fn spaced_staircase_contour_inside_its_cut_line_is_nested() {
        let _solver_test_guard = CPU_INTENSIVE_SOLVER_TEST_LOCK.lock().unwrap();
        let mut input = rectangular_input(rectangle(0.0, 0.0, 793.7, 1122.5), Vec::new());
        input.options.quality = QUALITY_BALANCED;
        input.options.rotation_mode = ROTATION_FREE;
        input.options.part_spacing = 3.78;
        input.options.container_margin = 18.9;
        input.options.time_limit_ms = 5_000;
        // A pixel-staircase contour (traced artwork) inside a smooth cut line.
        let traced = staircase_ring(90.0, 0.32);
        assert!(
            traced.0.len() > 1000,
            "trace has {} vertices",
            traced.0.len()
        );
        input.parts = (0..6)
            .map(|id| compound_part(id, vec![traced.clone(), wavy_blob(500.0, 500.0, 93.0, 600)]))
            .collect();
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, input.parts.len());
        let problem = prepare_problem(&input, &control).unwrap();
        // The traced contour lies inside its cut line; only the cut line remains.
        assert!(problem.items.iter().all(|item| item.components.len() == 1));
        let result = JaguaSolver.solve(&input, &control).unwrap();
        assert_eq!(
            result
                .iter()
                .filter(|placement| placement.placed != 0)
                .count(),
            6
        );
        assert!(solution_is_feasible(&input, &problem, &result, &control).unwrap());
    }

    #[test]
    fn hull_envelope_contains_the_spaced_contour() {
        let bitmap = staircase_ring(90.0, 0.32);
        let original = inflated_original(&bitmap, 1.89, QUALITY_BALANCED);
        let envelope = hull_envelope(&original).expect("hull envelope");
        assert!(inflation_is_conservative(&original.shape, &envelope, 1.89));
    }

    #[test]
    fn multi_component_cards_use_the_grid_seed() {
        let card = |x: f64| {
            let mut outers = vec![rectangle(x, 0.0, 336.0, 192.0)];
            for glyph in 0..30 {
                let gx = x + 20.0 + (glyph % 10) as f64 * 30.0;
                let gy = 40.0 + (glyph / 10) as f64 * 40.0;
                outers.push(wavy_blob(gx + 7.0, gy + 11.0, 6.0, 80));
            }
            outers
        };
        let mut options = draft_options(ROTATION_RIGHT_ANGLES);
        options.worker_count = 1;
        options.time_limit_ms = 500;
        options.part_spacing = 7.56;
        options.container_margin = 18.9;
        let job = configured(options, rectangle(0.0, 0.0, 793.7, 1122.5));
        for id in 0..12_u64 {
            assert_eq!(job.begin_part(id), STATUS_OK);
            for outer in card(1000.0 + id as f64 * 13.7) {
                assert_eq!(job.add_part_component_outer(outer), STATUS_OK);
            }
            assert_eq!(job.end_part(), STATUS_OK);
        }
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        let placed = (0..12)
            .filter(|&index| job.result_at(index).unwrap().placed != 0)
            .count();
        assert!(placed >= 11, "placed {placed}");
    }

    // ---- Fixed obstacles (artwork already on the sheet) ----

    fn obstacle_job(
        options: VacNestingOptions,
        container: Polygon,
        obstacles: &[Polygon],
    ) -> SolverTestJob {
        let job = configured(options, container);
        for obstacle in obstacles {
            assert_eq!(job.add_obstacle(obstacle.clone()), STATUS_OK);
        }
        job
    }

    #[test]
    fn obstacles_keep_part_spacing_and_ignore_the_margin() {
        // Sheet 0..62 x 0..22 with margin 1; an existing part fills x 1..21.
        // New 17 x 18 parts must keep the spacing (2) from it and only the
        // margin (1) from the sheet edge: x 23..61 holds 17 + 2 + 17 with
        // 2 units of slack, y 1..21 holds 18 with 2 units of slack.
        let mut options = draft_options(ROTATION_NONE);
        options.worker_count = 1;
        options.time_limit_ms = 1_000;
        options.part_spacing = 2.0;
        options.container_margin = 1.0;
        let fixed = rectangle(1.0, 1.0, 20.0, 18.0);
        let job = obstacle_job(options, rectangle(0.0, 0.0, 62.0, 22.0), &[fixed.clone()]);
        let part = rectangle(0.0, 0.0, 17.0, 18.0);
        assert_eq!(job.add_part(1, part.clone()), STATUS_OK);
        assert_eq!(job.add_part(2, part.clone()), STATUS_OK);
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        let fixed_box = transformed_bbox(
            &fixed,
            VacNestingPlacement {
                placed: 1,
                ..VacNestingPlacement::default()
            },
        );
        let usable = BBox::new(0.999, 0.999, 61.001, 21.001);
        let mut boxes = Vec::new();
        for index in 0..2 {
            let placement = job.result_at(index).unwrap();
            assert_ne!(
                placement.placed, 0,
                "part {index} must fit beside the obstacle"
            );
            let bounds = transformed_bbox(&part, placement);
            assert!(inside(bounds, usable, 1.0e-3), "part {index} {bounds:?}");
            assert!(
                bbox_distance(bounds, fixed_box) >= 2.0 - 1.0e-3,
                "part {index} too close to the obstacle"
            );
            boxes.push(bounds);
        }
        assert!(bbox_distance(boxes[0], boxes[1]) >= 2.0 - 1.0e-3);
    }

    #[test]
    fn obstacle_contact_is_allowed_without_spacing() {
        // 11 x 12 of free space beside the obstacle for a 10 x 10 part: it
        // fits only because no spacing is required next to the obstacle.
        let mut options = draft_options(ROTATION_NONE);
        options.worker_count = 1;
        options.time_limit_ms = 1_000;
        let job = obstacle_job(
            options,
            rectangle(0.0, 0.0, 31.0, 12.0),
            &[rectangle(0.0, 0.0, 20.0, 12.0)],
        );
        let part = rectangle(100.0, 100.0, 10.0, 10.0);
        assert_eq!(job.add_part(1, part.clone()), STATUS_OK);
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        let placement = job.result_at(0).unwrap();
        assert_ne!(
            placement.placed, 0,
            "the only free cell touches the obstacle"
        );
        let bounds = transformed_bbox(&part, placement);
        assert!(bounds.x_min >= 20.0 - 1.0e-3);
    }

    #[test]
    fn no_room_beside_obstacles_leaves_parts_unplaced() {
        let mut options = draft_options(ROTATION_FREE);
        options.worker_count = 1;
        options.time_limit_ms = 200;
        options.part_spacing = 1.0;
        let job = obstacle_job(
            options,
            rectangle(0.0, 0.0, 30.0, 10.0),
            &[rectangle(0.0, 0.0, 25.0, 10.0)],
        );
        assert_eq!(job.add_part(1, rectangle(0.0, 0.0, 6.0, 6.0)), STATUS_OK);
        assert_eq!(job.run(None, ptr::null_mut()), STATUS_OK);
        assert_eq!(job.result_at(0).unwrap().placed, 0);
    }

    #[test]
    fn external_validation_rejects_overlap_with_obstacles() {
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 40.0, 20.0),
            vec![(1, rectangle(0.0, 0.0, 5.0, 5.0))],
        );
        input.options.part_spacing = 1.0;
        input.obstacles.push(rectangle(10.0, 0.0, 10.0, 20.0));
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, 1);
        for (x, expected) in [
            (2.0, true),
            (25.0, true),
            (12.0, false),
            (5.5, false),
            (20.5, false),
        ] {
            let placements = explicit_placements(&input, &[(0, x, 5.0)]);
            assert_eq!(
                validate_external_candidate(&input, &placements, &control).unwrap(),
                expected,
                "x = {x}"
            );
        }
    }

    #[test]
    fn obstacles_disable_the_grid_seed() {
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 100.0, 100.0),
            vec![
                (1, rectangle(0.0, 0.0, 10.0, 10.0)),
                (2, rectangle(0.0, 0.0, 10.0, 10.0)),
            ],
        );
        input.obstacles.push(rectangle(40.0, 40.0, 10.0, 10.0));
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, 2);
        let problem = prepare_problem(&input, &control).unwrap();
        assert!(
            identical_part_grid_seed(&input, &problem, &control)
                .unwrap()
                .is_none()
        );
    }

    #[test]
    fn detailed_obstacles_with_spacing_prepare_quickly() {
        let mut input = rectangular_input(
            rectangle(0.0, 0.0, 800.0, 800.0),
            vec![(1, rectangle(0.0, 0.0, 50.0, 50.0))],
        );
        input.options.quality = QUALITY_BALANCED;
        input.options.part_spacing = 3.78;
        for index in 0..4 {
            input
                .obstacles
                .push(wavy_blob(150.0 + 170.0 * index as f64, 400.0, 70.0, 3000));
        }
        let cancelled = AtomicBool::new(false);
        let control = RunControl::for_tests(&cancelled, 1);
        let started = Instant::now();
        prepare_problem(&input, &control).unwrap();
        // Wall-clock limit for optimized builds only (owner decision 2026-09-26).
        assert!(
            cfg!(debug_assertions) || started.elapsed() < Duration::from_secs(3),
            "took {:?}",
            started.elapsed()
        );
    }

    #[test]
    fn obstacles_require_a_container_and_a_configurable_job() {
        let job = VacNestingJob::new(draft_options(ROTATION_NONE));
        assert_eq!(
            job.add_obstacle(rectangle(0.0, 0.0, 1.0, 1.0)),
            crate::job::STATUS_INVALID_STATE
        );
        assert_eq!(
            job.set_container(rectangle(0.0, 0.0, 10.0, 10.0)),
            STATUS_OK
        );
        assert_eq!(job.add_obstacle(rectangle(0.0, 0.0, 1.0, 1.0)), STATUS_OK);
    }

    #[test]
    fn splitmix_ordering_is_deterministic_and_seeded() {
        let sequence = |seed| {
            (0_u64..32)
                .map(|index| splitmix64(seed ^ index))
                .collect::<Vec<_>>()
        };
        assert_eq!(sequence(42), sequence(42));
        assert_ne!(sequence(42), sequence(43));
    }

    fn sample_input() -> SolverInput {
        SolverInput {
            options: crate::job::VacNestingOptions::default(),
            container: Polygon(vec![
                crate::job::VacNestingPoint { x: 0.0, y: 0.0 },
                crate::job::VacNestingPoint { x: 10.0, y: 0.0 },
                crate::job::VacNestingPoint { x: 10.0, y: 10.0 },
                crate::job::VacNestingPoint { x: 0.0, y: 10.0 },
            ]),
            holes: Vec::new(),
            obstacles: Vec::new(),
            parts: Vec::new(),
        }
    }

    fn rectangular_input(container: Polygon, parts: Vec<(u64, Polygon)>) -> SolverInput {
        SolverInput {
            options: draft_options(ROTATION_NONE),
            container,
            holes: Vec::new(),
            obstacles: Vec::new(),
            parts: parts
                .into_iter()
                .map(|(id, outer)| Part {
                    id,
                    components: vec![crate::job::PartComponent {
                        outer,
                        holes: Vec::new(),
                    }],
                })
                .collect(),
        }
    }

    fn optimizer_test_input() -> SolverInput {
        let mut input = rectangular_input(
            rectangle(-10.0, -20.0, 42.0, 26.0),
            (0..4)
                .map(|id| (id, rectangle(id as f64 * 11.0, 100.0, 9.0, 7.0)))
                .collect(),
        );
        input.options.random_seed = 0x5eed;
        input.options.time_limit_ms = 60_000;
        input.options.quality = QUALITY_DRAFT;
        input
    }

    fn permute_solver_input(input: &SolverInput, permutation: &[usize]) -> SolverInput {
        SolverInput {
            options: input.options,
            container: input.container.clone(),
            holes: input.holes.clone(),
            obstacles: input.obstacles.clone(),
            parts: permutation
                .iter()
                .map(|&index| input.parts[index].clone())
                .collect(),
        }
    }

    fn placements_by_id(
        placements: &[VacNestingPlacement],
    ) -> std::collections::BTreeMap<u64, (u8, u64, u64, u64, [u8; 7])> {
        placements
            .iter()
            .map(|placement| {
                (
                    placement.part_id,
                    (
                        placement.placed,
                        placement.translation_x.to_bits(),
                        placement.translation_y.to_bits(),
                        placement.rotation_degrees.to_bits(),
                        placement.reserved,
                    ),
                )
            })
            .collect()
    }

    fn replacement_input(blocker_width: f64, target_width: f64) -> SolverInput {
        rectangular_input(
            rectangle(0.0, 0.0, 12.0, 12.0),
            vec![
                (10, rectangle(0.0, 0.0, blocker_width, 10.0)),
                (20, rectangle(0.0, 0.0, target_width, 10.0)),
            ],
        )
    }

    fn explicit_placements(
        input: &SolverInput,
        placed: &[(usize, f64, f64)],
    ) -> Vec<VacNestingPlacement> {
        let mut results = empty_results(input);
        for &(index, translation_x, translation_y) in placed {
            results[index] = VacNestingPlacement {
                part_id: input.parts[index].id,
                translation_x,
                translation_y,
                placed: 1,
                ..VacNestingPlacement::default()
            };
        }
        results
    }

    fn draft_options(rotation_mode: i32) -> VacNestingOptions {
        VacNestingOptions {
            quality: QUALITY_DRAFT,
            rotation_mode,
            time_limit_ms: 60_000,
            ..VacNestingOptions::default()
        }
    }

    fn configured(options: VacNestingOptions, container: Polygon) -> SolverTestJob {
        let job = VacNestingJob::new(options);
        assert_eq!(job.set_container(container), STATUS_OK);
        SolverTestJob(job)
    }

    fn rectangle(x: f64, y: f64, width: f64, height: f64) -> Polygon {
        Polygon(vec![
            VacNestingPoint { x, y },
            VacNestingPoint { x: x + width, y },
            VacNestingPoint {
                x: x + width,
                y: y + height,
            },
            VacNestingPoint { x, y: y + height },
        ])
    }

    fn random_between(state: &mut u64, minimum: f64, maximum: f64) -> f64 {
        *state = splitmix64(*state);
        let fraction = ((*state >> 11) as f64) * (1.0 / ((1_u64 << 53) as f64));
        minimum + (maximum - minimum) * fraction
    }

    fn transformed_bbox(polygon: &Polygon, placement: VacNestingPlacement) -> BBox {
        let angle = placement.rotation_degrees.to_radians();
        let (sin, cos) = angle.sin_cos();
        polygon.0.iter().fold(
            BBox::new(
                f64::INFINITY,
                f64::INFINITY,
                f64::NEG_INFINITY,
                f64::NEG_INFINITY,
            ),
            |mut bbox, point| {
                let x = point.x * cos - point.y * sin + placement.translation_x;
                let y = point.x * sin + point.y * cos + placement.translation_y;
                bbox.x_min = bbox.x_min.min(x);
                bbox.y_min = bbox.y_min.min(y);
                bbox.x_max = bbox.x_max.max(x);
                bbox.y_max = bbox.y_max.max(y);
                bbox
            },
        )
    }

    fn inside(inner: BBox, outer: BBox, tolerance: f64) -> bool {
        inner.x_min + tolerance >= outer.x_min
            && inner.y_min + tolerance >= outer.y_min
            && inner.x_max - tolerance <= outer.x_max
            && inner.y_max - tolerance <= outer.y_max
    }

    fn overlaps(left: BBox, right: BBox, tolerance: f64) -> bool {
        left.x_min < right.x_max - tolerance
            && left.x_max > right.x_min + tolerance
            && left.y_min < right.y_max - tolerance
            && left.y_max > right.y_min + tolerance
    }

    fn bbox_distance(left: BBox, right: BBox) -> f64 {
        let dx = (left.x_min - right.x_max)
            .max(right.x_min - left.x_max)
            .max(0.0);
        let dy = (left.y_min - right.y_max)
            .max(right.y_min - left.y_max)
            .max(0.0);
        dx.hypot(dy)
    }

    impl BBox {
        const fn new(x_min: f64, y_min: f64, x_max: f64, y_max: f64) -> Self {
            Self {
                x_min,
                y_min,
                x_max,
                y_max,
            }
        }
    }
}
