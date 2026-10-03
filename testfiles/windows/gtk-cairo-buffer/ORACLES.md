# Oracles — Windows GTK Cairo buffer regression (defined before implementation)

These are outcome-based pass criteria for the default Windows GTK4/GDK Cairo
window-buffer path. They are stated against *presented HWND client pixels*
(after `GdiFlush`), not against an internal buffer. A candidate buffer
algorithm must not be re-implemented by the test.

Cases O1-O9 drive the real patched GDK Win32 Cairo context through the public
`gdk_surface_create_cairo_context` / `gdk_draw_context_begin_frame` /
`gdk_cairo_context_cairo_create` / `gdk_draw_context_end_frame` sequence on
real toplevel/popup HWNDs, so the caller supplies the damage region exactly and
the buffer under test is the one inside GDK. Case O10 drives the full
GtkWindow/GtkDrawingArea/GskCairoRenderer entry point for the same fixture.

## Renderer/backend configuration (run is INVALID if not observed)

- `GSK_RENDERER=cairo` — force `GskCairoRenderer`; O10 fails the run if the
  native renderer is not a `GskCairoRenderer`.
- `GDK_DISABLE=dcomp` — disable `GDK_FEATURE_DCOMP`. GTK registers `dcomp`
  under `GDK_DISABLE` (`gdk/gdk.c: gdk_feature_keys`, parsed at gdk.c:345),
  **not** under `GDK_DEBUG`; `GDK_DEBUG=dcomp` is not a GDK flag and must not be
  used. `GDK_DEBUG` is cleared for the child. With the DComp device absent,
  `gdk_win32_cairo_context_surface_attach()` leaves `self->swap_chain == NULL`,
  so `begin_frame`/`end_frame` dispatch to the GDI vfuncs this harness targets.
- `GDK_BACKEND=win32`.
- `GDK_WIN32_FORCE_DCOMP` is unset (not referenced by GTK 4.22.4; the MSYS2
  package defaults DComp off). `GDK_WIN32_CAIRO_GDI_BUFFER` is unset so the
  candidate runs its new default buffer path.
- Loaded `libgtk-4-1.dll` must come from the GTK prefix under test by explicit
  path and SHA-256.
- Loaded `libcairo-2.dll` must come from the arm's own loader path, checked by
  explicit path and by the single pinned parity-Cairo SHA-256
  `7f634c034e2e3c320b8f15b5eca8194a44ba14b03ab3668c1efd1ce9c9d6ea96`. The
  driver resolves that path from the same fixed locations the child loader
  searches, never from the path the process reports: (1) the arm's
  `<GtkPrefix>\bin\libcairo-2.dll` when it exists (the packaged candidate
  bundles the pinned Cairo app-locally), else (2) the pinned Cairo prefix
  `<CairoPrefix>\bin\libcairo-2.dll`. A `libcairo-2.dll` found in the first
  location that does not hash to the pinned value is INVALID; the driver does
  not fall back to the other location, because the loader would not use it. The
  pinned dependency and its `-CairoPrefix` are unchanged.
- A required module that is missing, resolves elsewhere, or hashes differently
  makes the run INVALID, never a pass. `libgdk-4-1.dll` is optional (it is
  inside `libgtk-4-1.dll` on MSYS2) but must match the GTK prefix when loaded.

The harness records `SESSION`, `ENV` and `LOADED`; the driver checks them.

## Coordinate space (public contract)

`gdk_draw_context_begin_frame()` takes a damage region in logical surface
("application") pixels; `gdk_draw_context_begin_frame_full()` scales it with
`gdk_cairo_region_scale_grow(region, scale, scale)` before the backend vfunc
(`gdk/gdkdrawcontext.c`). The backend therefore receives physical pixels, and
the candidate `BitBlt`s physical rectangles. This harness passes logical damage
rects and samples physical pixels at `round(logical * scale)`. Only the public
frame API is used; no GTK3 `queue_draw_area` API and no private hooks.

`gdk_surface_get_scale()` returns a `double`. The harness never truncates or
rounds the scale; it converts coordinates with explicit rounding
(`round(logical*scale)`) and compares requested vs actual scale with tolerance.
If a requested scale (e.g. 1.5) is not exposed by the runtime monitor it is
reported `SKIP requested-scale-unavailable`; it is never measured at 1 or 2.
Dump names include the scale (`<case>-s<scale>.bgra`) so multi-scale runs do not
overwrite each other.

## Alpha

Presented pixels are read back as top-down BGRA with GDI `BitBlt` from the real
HWND. A window DIB has no meaningful alpha channel, so the alpha byte is
undefined. Before any dump is written or hashed, `capture_dump()` normalizes
**only** `p[3] = 255`; RGB is never altered and every RGB oracle below is
strict. All in-process comparisons compare RGB only.

## Fixture

Window content is a fixed opaque quadrant grid in logical coordinates:

| quadrant | logical rect | RGBA |
|---|---|---|
| top-left | (0,0,w/2,h/2) | 255,0,0,255 |
| top-right | (w/2,0,w/2,h/2) | 0,255,0,255 |
| bottom-left | (0,h/2,w/2,h/2) | 0,0,255,255 |
| bottom-right | (w/2,h/2,w/2,h/2) | 255,255,255,255 |

Sample points are quadrant centres, away from half-pixel seams.

## O1 full update (exact)

One full-window frame. Each quadrant centre must equal the table RGB with
tolerance 0 per channel. Captured width/height must equal
`round(logical*scale)`.

## O2 partial update preserves untouched pixels (exact)

Frame 1 paints the grid full-window. Frame 2 queues damage = rect R and paints
an opaque overlay (0,255,255) inside R only. After frame 2:
- centre of R = 0,255,255 tolerance 0;
- every pixel outside R is RGB-identical to frame 1 (full-frame byte compare of
  RGB, not a sample);
- no sampled point becomes 0,0,0 or transparent.

## O3 multiple dirty rectangles (exact)

Frame 2 queues two disjoint rects R1,R2. Centres of R1,R2 equal their overlay
colours tolerance 0; the gap between them and the far quadrant equal the O1
grid values tolerance 0.

## O4 alpha boundaries on an opaque window (exact)

Overlay `rgba(0,255,0,0)` over the grid must leave every sampled pixel equal to
the grid value (tolerance 0). Overlay `rgba(255,0,255,1)` at full damage must
equal 255,0,255 tolerance 0. The window is opaque; presented RGB must never be
transparent.

## O5 translucent content (cross-build byte identity + monotonic sanity)

Overlay `rgba(255,0,0,0.5)` over the white quadrant. The exact 8-bit rounding
of Cairo compositing is environment-defined, so this case is **not** a strict
single-build pixel oracle. Pass requires:
- candidate dump byte-identical to baseline dump for this case; and
- red channel >= 250, green and blue each in [120,136]; and
- result differs from both the white background and the pure red source.
Any byte difference from baseline FAILS and is escalated, not tolerated.

## O6 grow / shrink (exact)

Resize the same window (grow then shrink). The requested client size is never
assumed: after `SetWindowPos` the harness reads the real `GetClientRect` and the
GDK surface metrics and FAILS if they do not match the requested physical size.
After each resize: client size equals the new `round(logical*scale)`; quadrant
centres equal the O1 grid values tolerance 0; no stale band, no zeroed band.

## WR-A2a proof cases (physical Windows console)

T1 pins the rebuilt candidate DLL SHA-256 and checks expected and loaded
paths. Identical `retention` children run with buffer switch unset and `0`;
only the on child receives `--expect-retained`. Both wait for the requested
physical client size and sync GDK metrics before recording the initial GDI
count and warming up. The second frame has small damage. On must keep its GDI
count unchanged after warm-up, preserve the seeded pixel outside damage, and
release its retained GDI object at destroy. Its initial-to-drawn GDI rise must
exceed the off child's rise. Off allows one lazy GDI object after warm-up.
Both load pinned GTK/Cairo.
Fatal warning, timeout, crash or missing data fails (`G_DEBUG=fatal-warnings`).

The driver always runs T2, T3 and T6, even with a narrowed `-Cases` list.
T6 runs separately at scale 2; an unavailable or unexecuted T6 is INCOMPLETE.
The candidate must PASS while stock GTK must produce `EXPECTED-FAIL` with
`proof-oracle-mismatch`; setup failures, timeouts and SKIPs are INCOMPLETE.
The driver does not compare missing baseline proof dumps.

T2 `first_partial` seeds HWND RGB(19,23,29) before small logical damage after
create, grow and shrink. Extent equals `GetClientRect`; every RGB pixel matches
the opaque grid exactly, including edges and seams.

## WR-A2b T4 fault oracles (candidate, physical console)

Each named fault runs in its own candidate child with `GDK_DISABLE=dcomp` and
`VACARDS_TEST_GDK_GDI_FAULT` set only for that child. Unset or invalid hook
values preserve production behavior. All other children clear the hook.
Only fault children clear `G_DEBUG=fatal-warnings`; each `.err` must contain
exactly one expected GDK warning and no other warning or critical. GTK/Cairo
module pins, timeout, crash, missing `CASE`, and physical-session gates apply.
With a candidate, all four T4 cases run by default. Narrowing `-FaultCases`
marks each omitted case `INCOMPLETE T4 <case> not run` and exits nonzero.
Fault children use the last requested scale. Once identity, session and CASE
are valid, a wrong warning count or text is FAIL. Allocation warning dimensions
come from the harness's runtime physical buffer-size report.

| Case | Hook | Required result |
| --- | --- | --- |
| `alloc_latch` | `alloc-once` | First small frame falls back; same-size frame has no retained-DIB rise or repeated warning. Real resize permits a retained-DIB rise and exact whole-client RGB. Warning gives the runtime buffer width × height and `out of memory`. |
| `first_transfer` | `bitblt-once` | Seeded HWND stays incomplete after failed first partial transfer; next successful frame presents exact whole-client grid from retained full pending damage. |
| `partial_transfer` | `bitblt-after:1:2` | Established DIB, failed small transfer, then actual invalidation-driven retry frame with bounded logical `appclip` enclosing pending damage only by outward scale rounding. Failed pixels remain old; a natural frame before the next timer repairs exact affected RGB while every pixel outside remains unchanged; no later retry frame. |
| `retry_backoff` | `bitblt-after:1:5` | Actual render callbacks expose failed retries and eventual recovery. Initial-to-first and successive intervals are at least 210/460/960/1960 ms; late scheduling is reported, not rejected. Pending RGB is exact after recovery and no retry frame occurs for 2000 ms. Timeout or absent callback is incomplete. |

The expected transfer warning is `GDK-WIN32: BitBlt failed; retaining pending
presentation`. A retry observation requires a `GdkSurface::render` callback;
manual frame submission alone cannot prove timer cadence. `TIMING`/`RETRY`
records include appclip and monotonic timestamps; counts and pixel checks are
assertions, not only diagnostics.

## O7 minimize / restore (T3)

Wait three seconds for observed minimize; retained-path GDI must fall.
Restore maps without minimized state within three seconds. Seed then draw small
damage; require exact whole-client RGB and pre-minimize identity. T1 off controls.

## O8 drawing lifecycle (T5)

`--rounds` below 26 is usage error. Every round creates, draws partial, resizes,
draws partial and destroys. Report per-round GDI/USER/private bytes plus
baseline/midpoint/end; end-minus-midpoint caps are 4 GDI, 4 USER, 2 MiB.
Missing midpoint or incomplete round fails; a client-size wait failure is
`SKIP`/INCOMPLETE, not a rendering FAIL.

## T6 scale-2 odd client

`odd_client` requires observed scale 2 and actual 201x161 then 203x163 clients.
Wait for `GetClientRect`; record logical metrics and scaled bound at most one
pixel beyond actual extent. Sentinel then small damage must yield exact RGB
through final edges. Unavailable scale/odd size is unexecuted; scale 1 cannot
qualify. GDI counts do not establish allocation byte dimensions.

## O9 popup / dialog repaint (exact, best effort)

A GDK popup surface (own HWND) painted through the GDK context must read back
its quadrant grid exactly (tolerance 0); a transient dialog toplevel must read
back its grid, and the parent must be unchanged after the dialog is destroyed.
If a popup HWND cannot be captured on a host, record SKIP with the reason —
never PASS.

## O10 GTK renderer entry point (exact)

A `GtkWindow` + `GtkDrawingArea` full frame rendered by `GskCairoRenderer` must
present the same quadrant grid (tolerance 0). The case FAILS if the native
renderer is not a `GskCairoRenderer`, if the captured size is not the requested
physical size, or if any quadrant check fails. A bounded wait uses non-blocking
main-context iteration so it cannot sleep past its deadline.

## O11 visual_artwork (real GSK path; cross-build byte identity + content)

One `GtkWindow` + `GtkDrawingArea` full frame rendered by the real
`GskCairoRenderer` (same bounded window and GDI HWND readback mechanism as O10).
The drawing is a deterministic composite in logical widget coordinates of:

- a horizontal linear gradient band;
- thin 0.6 px antialiased lines;
- an antialiased cubic Bezier stroke;
- two translucent (alpha 0.65) rectangles that overlap;
- a black/white checkerboard bitmap scaled and rotated, drawn through a
  transformed surface pattern;
- the text `VA Studio 0123` drawn with the common `DejaVu Sans` family inside a
  clip band; the actual family Pango resolves is reported.

Pass criteria:

- The renderer is a `GskCairoRenderer` and the captured width/height equal
  `round(logical*scale)`.
- Content is nonempty and nonuniform: `>= 64` distinct RGB triples, `>= 5%` of
  pixels differ from the first pixel, and at least two channels span `>= 32`
  levels. A blank/uniform capture FAILS even when it is byte-stable, so a
  frozen/blank buffer cannot pass by matching a blank baseline.
- `LAYOUT visual_artwork width=.. height=.. scale=.. text=VA_Studio_0123
  font=..` is byte-identical between the baseline and candidate arms (fixed
  dimensions, layout and actual font report). A difference FAILS.
- The normalized `.bgra` dump is byte-identical between baseline and candidate:
  the driver compares SHA-256 of the whole normalized image and no screenshot
  tolerance is relaxed.
- A normalized `.png` (`visual_artwork-s<scale>.png`, written with the CairoPNG
  API) is produced alongside the `.bgra` for human review and is required by
  the driver, so the case and its dumps cannot be omitted silently.
- A capture failure/SKIP is incomplete, never a pass; session 0 cannot capture
  and therefore cannot pass O11.

## Case outcomes and exit codes

The harness prints one `CASE <name> RESULT PASS|FAIL|SKIP <detail>` line per
requested case. Exit codes: `0` all requested cases PASSed; `1` at least one
requested case FAILed; `3` no case failed but a requested case was SKIPped or
produced no CASE line (incomplete); `2` usage. A FAIL is always nonzero; a
requested case that is missing, skipped or unexecuted is never a qualified pass.
The driver additionally fails a run whose required dumps are absent. A child
that terminates with a negative Windows exit code (an NTSTATUS/exception, e.g.
`-1073741819` = `0xC0000005`) is reported as `ERROR`/crash with that exit code
and counted incomplete; it is never reported as `UNEXECUTED` and can never pass.

## Physical-session classification (run INVALID if physical is claimed otherwise)

A nonzero `ProcessIdToSessionId` session id is **not** proof of a physical
console: an RDP session also has a nonzero session id, and session 0 has been
observed to report `WTSClientProtocolType = 0` (Console). The harness therefore
records richer, reliable signals:

```
SESSION id=<pid-session> physical_requested=<0|1> active_console=<id>
        protocol=<n|unknown> remote=<0|1> physical_ok=<0|1>
```

where `active_console = WTSGetActiveConsoleSessionId()`, `protocol` is
`WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, WTSClientProtocolType)`
(0 = Console, 1 = legacy/ICA, 2 = RDP, `unknown` if the query fails), and
`remote = GetSystemMetrics(SM_REMOTESESSION)`. Physical is claimed only when all
of the following hold:

```
id != 0 && id == active_console && protocol == 0 && remote == 0
```

A requested `--physical` run that is not physical is refused by the harness
(`HARNESS FAIL physical-requested-not-active-console`, exit 3) and the driver
marks the run INVALID/incomplete with `physical_claim=false`; RDP, legacy/ICA
and unknown protocol are all rejected as physical. Session 0 fails the
active-console match and can never claim physical or pass a pixel oracle. The
actual monitors GDK exposes are recorded for the run (`MONITORS count=` plus
`MONITOR i scale=.. x=.. y=.. w=.. h=..`). No user desktop, service or
RDP configuration is changed.

## Host limitation

Session 0 (SSH service window station) on the dev host can load GTK and create
GDK surfaces but `GetClientRect`/`GetDC` on those windows yields no usable
client, so capture fails; `GtkWindow` creation segfaults. All pixel oracles and
any performance claim require an interactive console session. Session 0 results
are labelled non-physical/UNEXECUTED and cannot pass a pixel oracle, and a
requested `-Physical` run is refused there.

## Timing (informational, physical session only)

The `TIMING ... draw_us=.. present_us=.. capture_us=..` fields are elapsed
monotonic microseconds for one frame. They are diagnostics, not a display-FPS
or throughput claim, and no per-second rate is derived from them.

- `draw_us` is Cairo scene construction.
- `present_us` is the elapsed submission-to-after-paint latency: for
  `gtk_renderer`/`visual_artwork` it runs from the `gtk_widget_queue_draw`
  submission to the frame clock's `after-paint` (the wait loop pumps events
  until that signal). For the GDK-context cases it times
  `draw_surface_frame()`, which after
  `gdk_draw_context_begin_frame`/`end_frame` also performs a non-blocking
  main-context drain
  (`while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE)`),
  so it includes that event drain and is not solely the `end_frame` call. It is
  never an absolute timestamp and never a frame rate.
- `capture_us` times the whole `capture_hwnd()` call, not `BitBlt` alone:
  `GetClientRect`/`GetDC`, the `CreateDIBSection` buffer allocation, the
  compatible DC and bitmap selection, `BitBlt` plus `GdiFlush`, the `g_malloc`
  and `memcpy` of the DIB pixels into the capture, and the DC/bitmap cleanup.
  It is excluded from `present_us`; `capture_dump()` (CairoPNG encoding and the
  `.bgra`/`.meta` output writes) runs after the timed region and is not
  included.

Session 0 / headless runs are labelled non-physical and cannot make any
physical-4K performance claim.

## Benchmark mode (`--benchmark`)

`--benchmark` is a separate, opt-in mode that does not run the correctness
cases. The verified pixel oracle is scale `1` only, so a surface that exposes
any other scale FAILs (`bench-scale-must-be-1`) and is never measured at 1 or
2. Like `--physical`, benchmark mode requires the physical-console
classification above unconditionally: `physical_ok` must be 1 or the harness
exits 3 (`physical-requested-not-active-console`) before GTK starts.

### Invocation

Run each arm in its own directory with the pinned runtime and the same
renderer environment as the pixel oracles. Use a fresh process per trial:

```bat
set "GDK_BACKEND=win32"
set "GSK_RENDERER=cairo"
set "GDK_DISABLE=dcomp"
rem GDK_DEBUG must stay empty; GDK_DEBUG=dcomp is not a GDK flag.

gtk-cairo-buffer-test.exe --benchmark --bench-mode full ^
  --bench-width 1280 --bench-height 720 --bench-frames 120 --bench-warmup 20 ^
  --out <evidence-dir>\trial-full-01

gtk-cairo-buffer-test.exe --benchmark --bench-mode partial ^
  --bench-width 1280 --bench-height 720 --bench-frames 120 --bench-warmup 20 ^
  --out <evidence-dir>\trial-partial-01
```

Defaults are width 1280, height 720, frames 120, warmup 20 and mode `full`.
Every numeric `--bench-*` value is a strict full-string bounded integer
(width/height 1..16384, frames 1..100000, warmup 0..100000); a bad value is
usage exit 2 and creates no window. `--bench-csv <path>` overrides the CSV
path, otherwise `<out>/bench-<mode>-w<cw>x<ch>-s<scale>.csv` is written.

### What the timed interval is

- `full` damage is the whole viewport; `partial` damage is the centred
  `width/4 x height/4` rect, exactly 1/16 of the area. An opaque cyan/magenta
  overlay alternates by frame index so the final pixels still prove the path.
- The timer starts immediately before `draw_surface_frame()` and stops after a
  successful `GdiFlush()` on the same thread. The interval therefore covers
  Cairo scene construction, `gdk_draw_context_end_frame()` and the queued GDI
  work. A failed draw or `GdiFlush()` is a nonzero invalid trial
  (`timed-frames-invalid`/`gdi-flush-failed`), never a skipped sample.
- Capture/readback, PNG encoding, the per-frame CSV write and all `--out`
  dumps happen outside the timed interval, after the timed frames. They are
  never folded into a per-frame duration.

### Interpretation

`BENCH samples` reports `p50_us`, `p95_us`, `min_us`, `max_us`, `mean_us` and
`requested_damage_pixels`; `BENCH totals` reports `elapsed_us`,
`completed_gdi_frame_cycles_per_s` and `requested_damage_mpix_per_s`; and
`BENCH labels` states the two boundary labels explicitly.

- `completed_gdi_frame_cycles_per_s` is the number of completed
  draw-and-`GdiFlush` cycles divided by their summed elapsed time. It is a
  completed-GDI-cycle rate, **not** monitor/display FPS and not an application
  FPS claim.
- `requested_damage_mpix_per_s` is the requested physical damage area per
  second (`full` = whole client, `partial` = 1/16). It is **not** the unique
  damaged-region union and **not** the pixels actually copied or displayed.
- The per-frame CSV columns are
  `width,height,scale,mode,index,elapsed_us,requested_damage_pixels`, one row
  per timed frame.

### Repeated comparison protocol

Compare two arms (for example baseline and candidate) with identical window
size, scale, mode, frames and warmup. Run **four trials per arm** in **ABBA**
order: one balanced pass is A, B, B, A (two trials each); repeat the pass as
A, B, B, A, A, B, B, A to reach four trials per arm, using one fresh process
per trial. Run `full` and `partial` as separate series. Every trial still
performs the 20 warmup frames before its 120 timed frames. Retain each trial's
raw stdout, per-frame CSV, `SESSION`/`LOADED` lines and the exact executable
and DLL identities.

### Numerical acceptance

A performance number is comparable only when the run itself is valid. Require,
per trial:

- the final capture passes the quadrant oracle and, for `partial`, the
  untouched-outside-damage oracle — correct pixels, not timing alone;
- the per-frame CSV exists with the header above, exactly `frames` data rows
  and a successful `fclose`; an open/write/close failure is a nonzero FAIL;
- all `frames` timed frames completed with a successful `GdiFlush()`;
- the process exits 0 with `CASE benchmark RESULT PASS
  numeric-benchmark-complete`; and
- `SESSION physical_ok=1` and `LOADED` shows the pinned `libgtk-4-1.dll` and
  the parity `libcairo-2.dll` SHA-256.

This protocol defines no performance pass/fail threshold or expected speedup.
Do not report a pass or a speedup from raw
`BENCH` numbers alone; the repeated protocol above is documented so results can
be collected and reported separately.
