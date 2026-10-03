# Modern text panel GUI regression battery

This directory contains the deterministic SVG fixture used to exercise the
modern text panel from a real Inkscape window. The battery is intentionally
broader than a widget smoke test: it covers selection synchronization,
transient previews, XML persistence, Undo/Redo, Layout-TNG rendering, imported
CorelDRAW text, and application shutdown.

## Environment

- Date: 2026-08-29
- Platform: macOS
- Build: `a79fd77954` plus the changes under test
- Build type: `RelWithDebInfo`
- Isolated build: `build-github-integration`
- Isolated profile: `/tmp/inkscape-current-gui-battery-profile`
- Isolated app id: `org.inkscape.Inkscape.current-gui-battery`
- Fixture: `complete-text-panel.svg`

The application, build, profile, logs, and temporary documents were isolated
from the integration checkout and from other running Inkscape tasks.

## Acceptance criteria

1. Opening or synchronizing the panel must not modify XML, mark the document
   dirty, change new-text defaults, or add Undo history.
2. A transient font/style preview must affect only the initiating canvas and
   must disappear on cancel, selection change, panel close, or application
   shutdown.
3. Every explicit commit must create at most one Undo step. A no-op must create
   none, and one Undo must restore every affected object.
4. Partial selections must preserve unrelated runs and OpenType features.
5. Paragraph, list, drop-cap, and frame operations must preserve source text,
   semantic paragraph wrappers, and base font sizes.
6. Imported CDR text must retain its imported size when selected or edited.
7. Save/reopen and PNG/PDF rendering paths must preserve the committed visual
   result.
8. Closing a panel, document, or application with a preview pending must not
   invoke a late callback or crash.

## GUI coverage and result

| Area | Cases exercised from the GUI | Result |
| --- | --- | --- |
| Startup and targeting | No selection, text selection, mixed objects, multiple text objects, selection change | Pass |
| Font browser | Open/focus search, filter without mutation, keyboard navigation, transient preview, cancel, commit, no-op commit | Pass |
| Font properties | Family, face, size, unit synchronization, mixed values, imported non-12-pt text | Pass |
| Inline styles | Bold, italic, underline, superscript, subscript, mixed runs, partial range | Pass |
| Capitalization | None, uppercase, small caps, Unicode source preservation, unrelated feature preservation | Pass |
| Ligatures | Standard-ligature toggle while preserving unrelated `font-feature-settings` | Pass |
| Spacing | Character, word, and Latin/Asian spacing on the same row; limits and Undo | Pass |
| Paint | Fill/Stroke solid and none, linear gradient, document pattern, document swatch, Undo/Redo | Pass |
| Advanced paint | Empty mesh resource list is a safe no-op and does not dirty the document | Pass |
| Paragraph | Alignment, line-height value/unit, direction, writing mode, orientation, first-line indent, before/after spacing | Pass |
| Paragraph tools | Bulleted/numbered list, list start, list removal, hyphenation, drop cap, drop-cap line count | Pass |
| Text frame | Width, height, columns, gap, one-column restoration, top/middle/bottom alignment | Pass |
| Complex layout | Long wrapped text, newline insertion, combining/Unicode text, RTL, vertical text, text on path | Pass |
| Persistence | Save/reopen of paragraph metadata, drop caps, frames, Unicode, and paint | Pass |
| Undo/Redo | Partial style, paint, text edit, list, and frame operations | Pass |
| CDR 1.7 MB | Customer CDR file (not in the repository): import, canvas inspection, text selection, text edit, Undo | Pass |
| CDR transforms | `testcoreldrawfile.cdr` import and canvas inspection | Pass |
| Shutdown | Quit with a real font preview visible and uncommitted | Pass |
| Export UI | Export panel opens and renders a preview of the complete fixture | Pass |

The shutdown case completed in 1.45 seconds. The source fixture compared byte
for byte equal after the pending-preview shutdown, and no new Inkscape crash
report was generated.

### CDR size regression

An imported 11-character text in that 1.7 MB customer CDR file reported `30.375 pt` in
the modern panel. Appending a character kept it at `30.375 pt`; Undo restored
the original 11-character text and returned the document to a clean state.
The selection did not write a 12-pt style.

### Large CDR performance gate

A 154.9 MB customer CDR file (not in the repository) was used. Its import stayed non-interactive for more
than 306 seconds while the process continued reading the CDR and increasing
CPU/RSS; it did not crash. The run was stopped with `SIGTERM` after the
five-minute acceptance limit.

This is a failed performance gate, not a text-panel correctness failure. The
host simultaneously had an unrelated rendering process consuming roughly
155-236% CPU and 1.1-2.5 GB RSS, so the result is not suitable as a clean
before/after benchmark. Repeat this case on an otherwise idle host before
assigning a performance regression to the current changes.

## Regressions found and fixed

### Mixed font-size synchronization changed drop caps to 0.001 pt

Changing a drop cap from three to four lines creates a legitimately mixed font
size while the panel synchronizes. Focus traversal allowed the spin button's
internal minimum to emit as a user edit.

The font-size selector now exposes its mixed state, and the panel ignores
font-size callbacks until a real user edit clears that state. The regression is
covered by `ChangingDropCapLinesPreservesTheBaseFontSize` and by the GUI case.

### Editing frame width also committed a stale 1 px height

The frame controls previously assembled one patch from every displayed field.
Editing only width therefore sent an uninitialized/stale height and clipped the
text.

Each geometry signal now commits only its own property. Missing display values
are derived from the geometric bounds, and switching to `shape-inside` removes
the conflicting presentation `inline-size`. The GUI case verified independent
width/height edits, columns, gap, and vertical alignment.

### Gradient paint needed an object-aware server

The compact paint popover could change its gradient mode without creating a
paint server appropriate for the selected text target. Gradient creation now
uses the same object-aware chemistry path as the existing paint UI, then
commits the resulting paint URL through `TextStyleController`.

### Semantic paragraph wrappers were vulnerable during persistence

Inline-size/shape-inside updates and SVG 1.1 fallback could flatten a
`sodipodi:role="paragraph"` wrapper or lose list/hyphen/drop-cap metadata.
Paragraph wrappers and managed metadata are now preserved. New object-model,
save/reopen, and SVG 1.1 fallback tests cover the behavior.

## Automated regression suites

Run from the worktree root:

```sh
cmake --build build-github-integration \
  --target test_text-font-preview test_text-paragraph-target -- -j2

ctest --test-dir build-github-integration --output-on-failure -j2 \
  -R '^(test_text-font-preview|test_text-paragraph-target|test_libnrtype_Layout-TNG-Compute(-lang)?|font-feature-utils-test|libnrtype-font-(factory|instance|utils)-test)$'

ctest --test-dir build-github-integration --output-on-failure -j1 \
  -R '^cli_import_cdr2_'

ctest --test-dir build-github-integration --output-on-failure -j1 \
  -R '^(cli_preserve-text_|cli_export-text-to-path|cli_export-text-paintorder_|cli_convert-text-paintorder_|cli_pdfoutput-(09-text-layout|10-text-paint|11-text-decoration)-)'

ctest --test-dir build-github-integration --output-on-failure -j1 \
  -R '^render_text-flow$'
```

Observed result:

- 9 font-preview/typography cases passed.
- 22 paragraph/frame persistence cases passed.
- 8 focused CTest entries passed.
- 3 CDR CLI import entries passed.
- 37 of 39 text/export entries passed; two paint-order comparison entries
  were explicitly skipped by their existing test harness.
- `render_text-flow` passed.
- `git diff --check` passed.

## Manual rerun notes

- Always use an isolated `INKSCAPE_PROFILE_DIR` and `--app-id-tag`.
- Reset the fixture before a run; do not reuse a document modified by a prior
  paint or text-edit case.
- For CDR files whose objects are outside the page viewport, use Zoom Drawing
  before judging an import as blank.
- Font preview must be tested without pressing Enter: keyboard navigation or a
  stable pointer dwell changes the canvas, while the window title stays clean.
- For shutdown testing, quit while that preview is still visible and verify
  both the serialized document and `~/Library/Logs/DiagnosticReports`.
