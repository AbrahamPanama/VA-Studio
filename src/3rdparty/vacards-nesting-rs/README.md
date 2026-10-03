# VACards native nesting bridge

This directory owns the Rust static library used by VACards native nesting.
The application-facing boundary is the C header in `include/`; Rust or C++
types must never cross that boundary.

Phase 0 established the pinned toolchain and dependency probes. Phase 2 adds a
stable, fixed-width C job ABI and the C++20 RAII wrapper in `src/nesting/`.
The boundary copies all polygon input, contains Rust panics, publishes results
atomically, and supports synchronous progress callbacks plus lock-free cancel
requests. No Rust or C++ container crosses the ABI.

The production `run()` entry point dispatches through a private portfolio
solver beneath the unchanged ABI. `worker_count == 1` uses the frozen
deterministic constructive `JaguaSolver`; the automatic default and explicit
`worker_count >= 2` may add exactly one guarded experimental lane. Results
contain one entry per input part; parts that could not be fitted remain
explicitly marked as unplaced. The search is intentionally separate from
Jagua's educational LBF example, which upstream does not recommend for
real-world optimization.

The solver places larger-diameter parts first, tests deterministic boundary and
already-placed-item anchors, then fills the remaining quality-dependent budget
with a low-discrepancy sequence. Later passes deterministically move each item
to a different sample region, so additional wall-clock time performs new work
instead of replaying the first pass. It supports fixed, right-angle, discrete and
continuous rotation; arbitrary simple-polygon containers; container holes;
part spacing; extra container margin; deterministic seeds; cancellation; and a
wall-clock deadline. Deadline expiry is a successful partial result, while
explicit cancellation is terminal and publishes no results. Finite limits are
actual elapsed-time budgets rather than pass-count aliases. The deadline is
best-effort: it is checked between search candidates, so a single
geometry-preparation or collision operation may overrun it briefly.

The guarded portfolio gives both lanes one start instant, absolute deadline,
and cancellation token. The exact `JaguaSolver` remains on the calling thread
for the full budget and is the only callback owner. A scoped, silent Rust thread
builds its own Jagua container/items/layout, creates a quick stable-ID incumbent,
then runs adaptive insertion, bounded ejection, transactional ruin/recreate,
constructive exploration, and relocation. It transfers at most one complete
vector through `join`; the coordinator accepts it only when canonical rank is
strictly better and a fresh authoritative Jagua replay succeeds. Experimental
failure or panic preserves the baseline, while cancellation discards both.

The staged sequential wrapper normally exhausts a finite deadline in the
production baseline and therefore remains test-only rather than an activation
design. The hard-corpus activation gates governed by `dea0848` and `394344b`
enable automatic two-lane selection. Automatic and explicit two-or-more
requests are capped at two lanes and fall back to baseline unless at least two
logical processors are available, live resident-memory telemetry is available,
and the conservative incremental estimate passes both the 768 MiB lane limit
and 5.6 GiB total-process limit with a 1.5 safety multiplier. Callback pointers
never enter the worker thread. Final post-deadline validation is instrumented;
Jagua 0.8 still cannot interrupt one collision or shape-construction call in
flight. This policy is original VACards code informed by Sparrow's optimizer
architecture; the associated MIT attribution is in `LICENSES/`.

## Job lifecycle

1. Initialize `VacNestingOptions` with `vac_nesting_options_init()`; never
   aggregate-initialize it, because the function stamps its ABI size/version.
2. Create one opaque job and set its container.
3. Add zero or more container holes and uniquely identified parts. ABI v3
   accepts either the legacy one-outer call or a `begin_part`, one-or-more
   component-outers (with optional component holes), `end_part` sequence.
4. Call `run()` once. Progress callbacks execute synchronously on that thread.
5. Read placements only after an `OK` result, then free the job.

Successful results preserve input-part order and always have the same length as
the input. An item that cannot be fitted, or was not reached before the
deadline, has `placed == 0` and a zero transformation. Compound-part
components are rigid islands that retain one shared transform, so grouped
payloads remain intact. Part holes are accepted and retained by ABI v3; the
current Jagua 0.8 adapter conservatively uses each component outer as collision
geometry and therefore does not treat a part hole as space available to another
part.

Every job function except `free()` is internally synchronized. Another thread
may call `cancel()` while `run()` is active. The owner must join `run()` before
calling `free()`. Input arrays may be modified or released as soon as their API
call returns. The pointer returned by `job_error()` is invalidated by the next
mutating call.

## Reproducibility

- Rust 1.88.0 is pinned by `rust-toolchain.toml`.
- `jagua-rs` is pinned to an immutable commit in both `Cargo.toml` and
  `VACARDS-DEPENDENCIES.env`.
- `Cargo.lock` fixes every transitive version.
- `.cargo/config.toml` redirects all registries and Git sources to `vendor/`.
- Build and test commands use `--locked --offline`.

From this directory, configure and run the standalone ABI gate with:

```sh
cmake -S . -B build
cmake --build build --target check-vacards-nesting-phase0
cmake --build build --target check-vacards-nesting-phase2
```

The build directory must be unique to the current task or worktree.
