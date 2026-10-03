# Multi-object bitmap adjustments GUI regression battery

This directory contains the deterministic SVG fixture used to exercise tone
adjustments on bitmap, vector, text, group, clone, mask, clip and transparency
targets from a real Inkscape window.

## Environment

- Date: 2026-08-29
- Platform: macOS 26.6 on Apple Silicon
- Build: `d9044ea17d` plus this fixture and report
- Build type: `RelWithDebInfo`
- Isolated build: `build-bitmap-adjustments-v1`
- Isolated profile: `test-profile-multiobject-gui`
- Application ID: `org.inkscape.Inkscape.bitmap-adjustments-v1-full`
- Runtime: isolated application bundle with the pinned Cairo clipping fix
- Fixture: `multi-object-tone.svg`

The source fixture is never saved during the battery. The test finishes by
undoing all document changes and checking that the title returns to its clean
state before the application is closed.

## Acceptance criteria

1. Bitmap and vector targets participate in one atomic adjustment without
   converting vector source objects to images.
2. Different values display only an em dash. Multi-object selection hides the
   histogram and adds no count or status label.
3. Editing one property preserves every other property independently on every
   target.
4. Preview affects the initiating canvas only; Escape and selection changes
   restore the canonical renderer without changing XML or Undo.
5. One adjustment gesture creates one Undo command for every target. Redo
   restores the complete result.
6. Transparent pixels, masks, clips, existing filter chains and clone/source
   normalization remain valid.
7. Copy/paste and transforms preserve independent filters and original object
   types.
8. Rapid changes, zoom, pan, panel close and application shutdown do not crash,
   leak a preview callback or display a stale candidate.

## GUI execution result

| Area | Real-interface exercise | Result |
| --- | --- | --- |
| Mixed state | Selected every target in the unlocked layer. Brightness, contrast and intensity displayed `—`; uniform properties displayed `0`. No histogram or extra selection label appeared. | Pass |
| Atomic mixed commit | Set Brightness to 60 for bitmap, vector, text, group and clone targets. Existing masks, clips and unrelated filter primitives remained visible. | Pass |
| Undo/Redo | One document Undo restored all original mixed values and one Redo restored Brightness 60 on every target. | Pass |
| Before/Preview | Keyboard activation changed the whole selection between neutral comparison rendering and the committed preview. | Pass |
| Copy/Paste | Copied and pasted the 16-object adjusted selection. The pasted alpha bitmaps, mask, vectors, text and filters remained valid and independently selectable. | Pass |
| Rapid controls | Sent 305 consecutive keyboard changes to Brightness. Only the latest value painted, the UI stayed responsive, and one Undo restored the pre-gesture value of 60. | Pass |
| Cancel | Entered a transient value of 80 and pressed Escape before commit. Canvas and control returned to 60 with no stale flash. | Pass |
| Transparency | Embedded RGBA images retained checkerboard-visible transparency; the gradient-masked image produced no black frame before, during or after adjustment. | Pass |
| Zoom/Pan | Completed 60 selection/page zoom transitions and 140 directional pan events with filters active. | Pass |
| Reset/cleanup | Undid paste and tone changes. The source returned to its original clean title and original rendering. | Pass |
| Shutdown | Closed the clean application with the panel loaded. The process exited with status 0 and no Inkscape crash report. | Pass |

The fixture also records an existing blur/component-transfer/blend chain on the
star, a clipped vector group and a clone of the gradient rectangle. Dedicated
automated tests verify primitive position/slot preservation, group/descendant
normalization and source/clone normalization exactly; the GUI confirms their
rendering remains stable.

## Automated result paired with this run

```text
Dedicated bitmap gate: 12/12 CTest entries passed
Actionable platform gate: 805 executed tests passed, 80 skipped, 4 disabled, 0 failures
Raw complete suite: 805 passed, 80 skipped, 4 disabled, 3 known ARM64 failures
Known failures: test_geom-pathstroke, test_lpe, test_lpe64
git diff --check: passed
```

The dedicated cases cover sparse patches, all six controls, mixed aggregates,
bitmap/vector/text/group/clone targets, masks, clips, exact alpha, one-Undo
commits, no-op redo preservation, deletion and selection cancellation, 500
coalesced candidates, stale-generation rejection, preview/commit pixel parity,
SVG persistence and PNG export.

## Automation limitation

One coordinate click terminated `SkyComputerUseService` before the event
reached Inkscape. Inkscape remained running, responsive and crash-free. The
diagnostic report is:

```text
~/Library/Logs/DiagnosticReports/SkyComputerUseService-2026-08-29-151218.ips
```

The remainder of the battery used accessibility menu items and keyboard input.
Pointer drag, wheel pan and direct resize-handle manipulation therefore remain
blocked by the automation service in this run. The prior single-bitmap torture
battery already passed resize, masks, copy/paste and persistence; controller
and rendering tests cover those invariants for multi-object changes without
depending on pointer synthesis.

## Manual rerun

1. Launch an isolated, patched-Cairo bundle with this fixture, a unique profile
   and a unique `--app-id-tag`.
2. Select the unlocked Targets layer with Command+A.
3. Open Filters > Bitmap Adjustments.
4. Confirm the mixed dashes and hidden histogram before changing a value.
5. Change exactly one row, then exercise Undo and Redo before touching another
   row.
6. Repeat with only the masked bitmap, only a vector, a text object, the clipped
   group and the source/clone pair.
7. Do not save over the fixture. Undo to a clean title before closing.
