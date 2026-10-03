# Modern text panel v1

## Scope

This phase turns the placeholders in the modern Text panel into working paragraph and text-frame controls while keeping Inkscape's existing text pipeline authoritative. It does not introduce a second shaper, renderer, font database, style store, or undo mechanism.

The implementation reuses:

- `SPStyle` and `sp_desktop_query_style()` for computed style state;
- `sp_te_apply_style()` and the existing recursive style route for text-range and object commits;
- Layout-TNG and Pango for line breaking, shaping, vertical text, and shape-inside flow;
- `DocumentUndo::done()` / `DocumentUndo::maybeDone()` for atomic and continuous edits;
- the existing desktop-local transient preview controller for font/style previews;
- SVG `shape-inside` references for frames and columns.

## Shared architecture

`TextStyleController` is the panel-facing API. It captures logical character ranges, ignores non-text objects, reports uniform or mixed values, and centralizes commits. A nonempty Text-tool selection targets that range; otherwise the selected top-level text objects are targeted. No text target means no style or preference changes.

`text-target-utils` maps canonical Layout-TNG character indices to paragraph ranges. It never stores `Layout::iterator` across rebuilds.

`text-style-units` owns line-height conversions so switching between percent, line multipliers, points, and pixels preserves the rendered value.

## Implemented controls

### Paragraph

- Start, center, end, and justified alignment.
- Line height with authored-unit preservation.
- First-line indent.
- Space before and after paragraphs.
- Left-to-right and right-to-left paragraph direction.
- Horizontal, vertical right-to-left, and vertical left-to-right writing modes.
- Mixed, upright, and sideways text orientation.

The two Inkscape paragraph-spacing properties are registered in the normal style and attribute tables:

- `-inkscape-paragraph-spacing-before`
- `-inkscape-paragraph-spacing-after`

Layout-TNG adds them on the block axis without rewriting source text.

### Paragraph tools

- No list, bulleted list, and numbered list.
- Configurable numbered-list start.
- Dictionary-based automatic hyphenation.
- Drop caps with a configurable line count.

List markers are tagged structural tspans and are reversible. Automatic soft hyphens are likewise tagged, so disabling automatic hyphenation removes only generated markers and preserves soft hyphens authored by the user.

Hyphenation reads TeX/LibreOffice Liang-pattern dictionaries. `INKSCAPE_HYPHENATION_PATH` can provide an isolated or custom dictionary root. The control is disabled when no dictionary matching the effective language is available.

Drop caps wrap the first logical character in a tagged tspan and reserve its measured post-layout width beside subsequent lines. They preserve source character count and are currently offered only for horizontal writing mode.

### Text frames and columns

- Width and height.
- One or more columns.
- Column gap.
- Top, middle, and bottom vertical alignment for a single column.

Generated column rectangles live in `<defs>` and are referenced through standard `shape-inside` URLs. The source characters are not duplicated. Multiple-column vertical alignment remains top-aligned because independent column balancing is not yet implemented.

## Persistence and interoperability

Generated structures are owner-tagged with Inkscape attributes so that they can be updated and removed without touching unrelated user content. The visible flow remains based on standard SVG shape-inside references. Save/reopen tests verify that list state, drop caps, frame dimensions, column count, gap, hrefs, and character counts survive serialization.

Inkscape-specific paragraph spacing, generated marker metadata, drop-cap metadata, and frame ownership metadata are preserved by Inkscape. Other SVG renderers may ignore those metadata properties while still reading ordinary text and shape-inside content according to their SVG support.

## Undo and synchronization contract

- Button and selector actions create at most one undo entry.
- Spin-button changes coalesce under a stable undo key.
- A no-op does not create undo or discard redo.
- Programmatic panel synchronization does not write XML.
- Selection, document, tool, target-lifetime, or external-style changes invalidate transient previews.
- Controller callbacks are disconnected before desktop teardown.

## Verification record

All commands used an isolated worktree, build directory, and profile.

- Focused style/layout/controller suite: 9/9 passed.
- Paragraph target and persistence suite: 18/18 passed.
- Font-feature merge helper: 4/4 passed.
- PDF text layout, paint, and decoration paths: 6/6 passed.
- Full sequential CTest run: 860/874 active tests passed before the attribute-table correction; the one attributable failure was fixed and its test now passes.
- Real CDR imports rendered successfully:
  - a 148 MB customer CDR file: 34.55 s cold, 30.84 s warm;
  - a 1.7 MB customer CDR file: 1.25 s;
  - `testcoreldrawfile.cdr`: 0.81 s.

The remaining full-suite failures are environment or upstream baselines outside this change:

- CMS numeric differences, geometry/LPE baselines, and platform-dependent PDF-import comparisons;
- BSD `sed` incompatibility in several PDF-import comparison scripts;
- `render_test-peppercarrot-text` differs by 2.005%; the earlier pre-paragraph application produces exactly the same difference on this machine.

Automated macOS GUI interaction was only partially available because the test application did not expose an accessibility tree to the automation driver. Native engine, persistence, export, rendering, CDR, and teardown tests were completed; final pointer/keyboard accessibility acceptance remains a manual release check.

## Known v1 boundaries

- Drop caps are horizontal-only.
- Vertical alignment is single-column-only.
- Generated automatic-hyphen markers are refreshed when the hyphenation command is applied; fully automatic dictionary regeneration after every subsequent text edit is deferred to avoid hidden XML mutation during ordinary typing.
- The panel exposes the existing Font Browser separately and does not duplicate its font database or preview infrastructure.

