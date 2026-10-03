# Groups and selections editing contract

Owner-approved product rule, 2026-09-09. Applies to new and modified editing
operations across UI, keyboard actions, command-line actions and extensions.
Existing exceptions are migration gaps, not precedent for new features.

## One user target, explicit operation semantics

A group or selection is one editing target and one undoable user action.
Every compatible part must receive the intended result without requiring the
user to ungroup. This does not mean every operation recursively edits leaves.

| Mode | Required semantics | Examples |
| --- | --- | --- |
| Collective geometry | One document-space transform/pivot/bounds; preserve relative layout, hierarchy and clip/mask relationships | move, scale, rotate, skew |
| Compatible members | Discover eligible descendants through nested groups; patch only the requested property per member, retaining unrelated values | font family, per-image brightness |
| Collective compositing | Evaluate the combined appearance once; preserve editable structure | one shadow/blur around the entire visual target |

Do not represent a multi-root collective effect by attaching separate copies to
its members: overlaps and filter bounds can make those results different. Do
not reparent across layers, clipping contexts or blend/isolation boundaries just
to manufacture a group. A persistent selection-effect representation and its
save/export semantics require implementation before that default is supported.
Explicit 'apply separately' remains a valid alternative.

## Target and result rules

- Normalize parent/child overlap and duplicates. Respect operation-specific
  clone inheritance; editing an instance alone must not rewrite its source.
- Compatible-member traversal visits nested groups but preserves text spans,
  clones, masks and clips according to their own editing model. Referenced defs
  are not selected editable members. A hidden/locked target stays protected.
- Evaluate availability/compatibility for the operation, not just object type.
  Missing bitmap sources, unsafe recursive filters, and unavailable drawing
  objects require explicit reasons.
- Count an item covered by an ancestor/source as covered, not incompatible.
- Apply to compatible targets and report skipped targets. If none are eligible,
  disable the control with an explanation or give an explicit action result.
- Distinguish changed, unchanged, incompatible, unavailable, canceled and failed.
  Existing equal values, an identity transform or a visually insensitive image
  are legitimate no-ops; do not promise every numeric edit changes visible pixels.
- Incompatibility must not delete an old effect, change defaults, or mutate the
  document. Defaults change only through an explicit default-setting action.
- One action gives one Undo and Redo. Unexpected failure must not leave a
  partially committed document. Intentional incompatible exclusions are reported
  separately from runtime failure. No-op and canceled preview preserve Redo.
- Preview uses the same targets and intended semantics as commit, remains
  transient, and cancels safely on selection/document change or target deletion.
- Save/reopen/export preserve the result, transparency and editability.

## Shared implementation and current boundaries

`Util::resolve_composite_targets` resolves independent composited roots and
reports unavailable/missing versus covered items. The tone controller uses it.
It does not traverse group leaves or aggregate unrelated selected roots into a
single visual surface. Those capabilities are explicitly pending.

`assign_filter_preserving_incompatible` centralizes safe filter assignment and
explicit outcomes. The filter dialog uses it; older assignment paths still need
audit/migration. Transaction ownership remains with the caller.

Legacy tone adjustment is temporarily **per selected composite root**: a group
gets a group filter and separately selected bitmaps get independent filters.
This preserves existing documents but is not yet equivalent to the desired
compatible-member model for groups with pre-existing child adjustments. Do not
flatten or stack these settings silently. Migration must define how stored group
and child adjustments interact, then prove appearance preservation on old files.

### Explode Bitmap: direct embedded bitmap only

Mode: **single-bitmap compositing**. The dedicated panel accepts exactly one
selected usable embedded SPImage. Groups, mixed/multiple selections, vectors and
clones are rejected without changing XML, selection or Undo. Vectors receive:
“Explode Bitmap works on embedded images. For vectors, use Path > Break Apart.”
There is no panel conversion-candidate exception. Shared Bitmap Copy and candidate
engine behavior remain separate and unchanged. Ctrl+K remains vector Break Apart.

Opening, mapping and selection notifications perform only cheap eligibility and
identity checks. Analyze alone authorizes full target resolution/admission from
Idle or Stale. A session binds document incarnation, logical bitmap identity and
session generation. Identical selection notifications preserve it; selecting
another object ends it. Recipe edits in a valid session use the existing 250 ms
debounce. Document/native/view invalidation (including Undo/Redo) clears results
and requires Analyze again. Compatible non-text tools preserve the session;
entering text editing invalidates it.

After Analyze, `Bitmap::resolve` snapshots the direct image, ancestors, placement,
transforms and protection/resource context. Refuse hidden/locked objects, unsafe
effects, linked/missing/unsupported raster dependencies, references and CSS/id
ambiguity without mutation. Direct images retain qualified nonzero ancestor
opacity/transforms at the original parent. Computed visibility:hidden/collapse
also protects targets and ancestors. Cropping preserveAspectRatio slice is refused.
Only canonical managed tone with validated filter region/units and simple own
geometric clips is admitted; masks and other filters remain protected.

A supported direct bitmap with one final piece can publish that refined/cropped
bitmap with one Undo/Redo transaction; zero pieces remains a no-op. Optional
outline refusals must not discard an exact count or disable admissible Explode.
Preview and Cancel never change persisted XML or history. Delivery, confirmation
and publication validate the original capture; publication remains atomic with
rollback and one Undo. A queued selection change goes Idle; a queued same-target
invalidation goes Stale. Successful Apply retains the same bitmap session and
re-analyzes with the baked marker, preventing double refinement (supervisor P1b
override). Explode ends the session with a completion acknowledgment; Resize
returns Idle. Neither operation automatically analyzes its new output selection.

### Build 26: structural operations and unavailable targets

Ungroup with nested clips (N1, `cd5f5aea7`) is collective geometry/structure
on selected non-layer groups; Ungroup All dissolves compatible descendants once.
Wrappers preserve parent/child clip intersections and multi-shape clip geometry.
Unsafe clipped/masked or otherwise incompatible groups remain selected and are
reported, rather than losing their compositing. One native Undo/Redo action;
no preview. Plain Ungroup of an empty clipPath remains an uncovered boundary.

Absolute skew with flat members (G4, `40515f585`) refuses dimensions <= 1e-12
and reports the exclusion when a desktop exists. Applied separately, compatible
members still skew and flat members stay unchanged; collectively, a degenerate
target refuses the whole operation. Refusal writes no non-finite transform and
adds no Undo step; an applied action retains one native Undo step.

App actions (D2, `8007b096f`) that require selection are disabled when there is
no active selection or its document is absent, and re-enabled when available.
This means missing app selection state, not an available selection with zero
items; operation-specific empty-target rules still apply. Closing a background
tab preserves the foreground target (D1); no fallback to a different document.

Move to Layer (N3, `cd5f5aea7`) refuses a destination equal to, or inside, the
selected structure. Refusal preserves XML and history; legitimate
moves retain native behavior. These notes do not broaden compatible-member
resolution or authorize flattening clip/mask contexts.

### Destructive bitmap clip: narrow two-role pair exception

`object-destructive-clip` and its inverse are a paired compositing operation:
exactly one bitmap branch and one closed vector cutter. This is a two-role
resolution, not the independent-composited-roots policy of
`Util::resolve_composite_targets`, so the operation deliberately does not reuse
that helper. `DestructiveBitmapClip::resolve_targets` implements the narrow
policy:

- The selection is exactly two distinct item roots that are neither equal nor
  ancestor/descendant; duplicate or parent/child roots are rejected atomically.
- One root must contain exactly one image and the other none. Two image-bearing
  roots, or none, are rejected. A selected wrapper may contain only its single
  image and an exclusively owned chain of hidden or single-child groups; visible
  non-image content, a clone/`use` boundary, a filter or a non-normal blend on
  that chain is rejected. A directly selected bitmap keeps the existing
  trim-conditional resource policy.
- The cutter root must supply one closed region through the existing
  `OffsetShapes::prepare` policy; open subpaths, unsupported content or unusable
  image data are rejected. Every locked/hidden/masked/filtered member of the
  cutter subtree is rejected before preparation, because `OffsetShapes` would
  otherwise drop a hidden/masked/filtered member silently or include a locked
  member the operation does not own.
- A compositing context (mask, clip, filter, opacity or blend) above the
  selected wrapper is outside the conversion boundary and stays shared with the
  rest of the document, so it is rejected rather than silently crossed. Walking
  an exclusively owned ancestor context into the conversion boundary is not yet
  implemented; that remains a B04 transaction concern.
- Resolution is read-only: no document, selection or undo-history change. Every
  rejection is an atomic no-op, never a partial destructive edit.
- Hidden or locked branch/cutter members and missing image sources are reported
  with explicit reasons rather than silently skipped.
- The cutter is kept (CLIP-1, owner 2026-09-27): only the bitmap is changed;
  the cutter keeps its position, layer, stacking order and attributes, and
  after a commit only the bitmap is selected (a no-op changes nothing,
  selection included). Clip and Inverse Clip behave the same.
  A cutter referenced or cloned elsewhere is therefore allowed. When the result
  leaves the pixels unchanged and needs no trim (for example a cutter that
  contains the whole image), the operation is a no-op with no Undo step.
  Evidence: `testfiles/src/destructive-bitmap-clip-test.cpp` (the clip and
  inverse clip cases compare the cutter's parent, position and attributes;
  `CutterReferencedElsewhereIsKeptAndStillReferenced`;
  `InvalidSelectionAndNoOpDoNotCreateUndo`, `InverseClipWithDisjointCutterIsANoOp`,
  `FullCoverageOwnClipWithContainingCutterIsANoOpAndKeepsTheClip`) and the `vacards-agent/clip` CLI
  cases (k01, k02, k06, k11, k12, k15, k16, k18).

- Oblique bitmap pixel axes (rotation other than multiples of 90 degrees or
  skew, measured in document space) are automatically straightened. Own
  mask/clip/opacity is first baked in the source grid. The baked extent is
  resampled into a document-axis-aligned grid, intersected with the cutter's
  bounds for KeepInside; KeepOutside uses the full baked visual box. The grid
  starts at the region's exact minimum corner, with Dx=ceil(w*D)/w and
  Dy=ceil(h*D)/h. Both densities are at least D and add less than one pixel
  per axis; an interior rectangular cutter gives its exact box (within 1e-6).
  Slice viewports clip the baked extent and Cairo resample in source pixels,
  so previously hidden pixels are never revealed.
- Density is the larger reciprocal length of the two pixel-to-document linear
  columns, preserving at least the source density in both directions. Cairo
  uses EXTEND_NONE and NEAREST for optimizeSpeed, pixelated and crisp-edges,
  GOOD otherwise. ICC metadata is retained and both PNG dpi options describe
  new grid using Dx for both options. Resampling's premultiplied-alpha round
  trip can quantize low-alpha RGB. Only oblique bitmaps are resampled; axis-aligned
  bitmaps, including 90/180/270-degree rotations and flips, retain the exact
  existing source-pixel crop path. The 1e-9 alignment test or off-axis drift
  of at most 0.05 target px across the bitmap qualifies for this exact path.
- Oblique plus disjoint refuses; axis-aligned plus disjoint keeps today's
  fully transparent commit.
- Empty/all-transparent baked regions and straight grids over 16384 pixels per
  side or 100 megapixels are explicitly refused before publication. A nonempty
  grid whose cutter result is fully transparent retains that grid's extent.
  Worst-case peak is about 1.6 GB for four 400 MB target buffers (resample,
  apply copy, crop, PNG), plus decoded source, coverage bake and Cairo source
  copies; Cairo painting is synchronous and cannot be interrupted mid-paint.
  A straightened result commits only when cutter pixels differ from the plain
  resample or a frame side shrinks by more than one target pixel; otherwise
  NoChange preserves Redo. Publication updates the same image id, parent and
  stacking position, detaches baked coverage, keeps the cutter and records one
  Undo step. The image transform cancels its parent's document affine.
  Evidence: `DestructiveBitmapClipStraightenTest` T1–T8 in the existing suite.

This exception is limited to this paired destructive operation and does not
authorize first-descendant or nearest-object selection elsewhere.

### Offset shapes: per-selected-root exception

`vacards-offset` and the Offset Shapes tool are **per selected root**, not
compatible-members. Each selected root is one source: a selected group becomes
one combined silhouette of its eligible closed subpaths, and its members are not
offset individually. Open subpaths are skipped and counted, and unsupported,
hidden, locked, masked or filtered members are excluded by the same
`OffsetShapes::prepare` policy the tool uses. With `delete-originals`, the whole
selected root is removed, including members that produced no offset. Originals
are deleted with the delete signal as in Edit > Delete, so unselected clones of a
deleted original follow the orphaned-clone preference (unlinked into independent
copies that keep their ids by default) instead of being left dangling. Unlike
Edit > Delete, selected clones are deleted before other sources and the deepest
clone of a chain first, so no selected object survives as an unlinked copy. Created paths are inserted after their source in the same parent, one
Undo/Redo step covers the whole action, and a refusal is an atomic no-op.

Both entry points share one `prepare`/`build`/`commit`; the exception exists
because "offset" is defined on a shape, not on each descendant. A
compatible-members mode (one offset per eligible leaf, discovering nested
members) would be a different operation and needs an owner decision; it is not
implemented and must not be inferred from this exception.

### Corner rounding: single-object exception

`vacards-corners` and the Node tool's corner controls are **single-object**:
exactly one selected, visible, unlocked native shape (rect, ellipse, polygon or
star). A multi-selection, a group or nested group, text, a clone or a hidden or
locked shape is refused with `requires-single-shape` or `unsupported-shape`
and a reason; nothing is changed and no member is chosen for the user. An
`SPPath` is refused with `path-not-supported` until its node types can be
resolved without the Node tool (AC-8b), and a nonuniform scale or skew with
`non-similarity-transform`. Both entry points share one capture
(`capture_corner_rounding_document`), one plan check (`check_corner_plan`, also
used by dry runs) and one commit (`apply_corner_plan`); a commit is one
Undo/Redo step and a refusal is an atomic no-op.

The exception exists because corner identity (which node is which corner) is
defined on one shape's own geometry. Rounding the corners of every eligible
member of a group would be a compatible-members operation needing an owner
decision; it is not implemented and must not be inferred from this exception.

### Boolean Assist: groups as one combined shape

Boolean Assist (`apply_boolean_assist`, shared by the GUI flyout and the
`vacards-boolean` command line) is **collective geometry per selected root**.
Each selected group root is one operand: the union of its leaf paths, shapes and
texts, nested groups included. The chosen operation then runs on those root
operands exactly as the Boolean engine accepts them; a group is never combined
leaf-by-leaf against the other operands.

- A group root is refused, atomically, when any member is hidden or locked, when
  any member is not a path, shape or text, or when any group in it (the root
  included) carries a clip, mask, filter, group opacity below 1 or a non-normal
  mix-blend mode. The refusal names the offending object and, for a group
  effect, the reason; a combined silhouette cannot reproduce that effect.
- A selection without a group root behaves exactly as before and removes
  nothing. Only the selected group roots and the groups nested inside them are
  cleanup candidates: a selected group the operation emptied is removed, while a
  layer is never removed and an unselected group elsewhere in the document is
  never touched.
- The whole action is one Undo/Redo step. When a group root was combined, the
  GUI and the CLI record the same label (`boolean_assist_undo_label`); the CLI
  also reports the removed group ids. An operation that does not leave exactly
  one path is reverted — the GUI shows a notice and changes nothing, and the CLI
  records a failure after restoring the document — rather than committing the
  deletion of every operand.
- A refused or failed boolean cancels its changes with `DocumentUndo::cancel`;
  as with any cancel, that also clears the Redo history, so the previous edit is
  no longer redoable. An empty result (for example the intersection of disjoint
  shapes, or a bottom-minus-rest whose anchor is fully covered) is reverted and
  reported as a failure, unlike stock Path > Intersection, which commits an
  empty path.
- The result path takes the style and the parent of the engine's source operand:
  the bottom object for union, intersection and exclusion, and the retained
  operand for the two differences. A group's inherited fill or stroke of the
  other operands is therefore not kept. Stock boolean operations copy the source
  operand's style, clip and mask onto the result (copy_object_properties), so for a
  clip, mask, filter or opacity set on an individual leaf inside a group, the
  pre-union keeps it from its source leaf and drops it from the other leaves; the
  preview does not show leaf-level effects. The group-level counterparts are
  refused above.

This is a per-root collective-geometry exception, not a compatible-member
operation over group leaves, and not authorization to flatten groups elsewhere.
Modes and object transforms remain those of the underlying Boolean engine.

### Nesting: collective geometry of the selected top-level objects

The nesting tool (NEST-010/011) declares **collective geometry**: each selected
top-level object (a group included) is one rigid part, moved once by its
top-level transform; its children, styles and resources are never edited.
Targets are the selection in selection order, plus the sheet's tracked parts
for a re-nest (tracked first). Unsupported selected objects are skipped and
reported (hidden, duplicate, unmeasurable); locked ones stay fixed. Objects
already on the sheet are fixed obstacles and are never modified; a part the
preparation skips while it sits on the sheet stops the run instead of being
left unprotected. Placements and the leftover move beside the sheet are one
rollbackable Undo step; the selection afterwards (the leftovers) is not part
of Undo. The live preview, labels and outlines are canvas items only;
cancelling (Esc, tool switch, selection change) never writes the document.
Evidence: `testfiles/src/nesting-document-test.cpp` and
`testfiles/src/nesting-tool-test.cpp` (for example
`LeftoversMoveBesideSheetInOneUndoStep`, `AddKeepsExistingPartsInPlace`,
`LockedTrackedPartStaysFixed`, `SelectionChangeWhileSolvingCancels`,
`EscapeDuringSparrowRunLeavesDocumentAndUndoUntouched`), plus
`check-selection-contract` on every tool milestone.

### Ungroup All: recursive group handling

Mode: group handling (owner request GRP-1, 2026-09-26). Targets: the
selected groups, then the groups those contain, level by level, in one
Undo step.

- The selected level behaves exactly as Ungroup (`ObjectSet::ungroup`).
  Neither command ever ungroups a layer: selected layers are held out and
  stay selected. As with Ungroup, dissolving a selected group unlinks
  clones of that group.
- Below the selected level only plain groups are dissolved. Kept, and
  counted in the status bar message: links (`<a>`), switches, 3D boxes,
  locked groups (their own or an ancestor's `sodipodi:insensitive`),
  groups whose dissolution would change the artwork (path effects, a
  filter, a blend mode other than normal, opacity below 100%), and groups
  used by a clone anywhere in the document (dissolving them would unlink
  that clone, an object the user did not select). Nested layers are also
  kept (not counted).
- Paths are never broken apart; clip and mask handling is Ungroup's.
- Non-group items in the selection are left as they are and stay
  selected. A selection whose only groups are layers makes the command
  unavailable (greyed out), like a selection without groups.
- A command-line call on a selection without an ungroupable group is
  refused as a disabled action.

Evidence: `testfiles/src/object-set-test.cpp` (`UngroupAll*` and
`UngroupHoldsOutSelectedLayers`: every nesting level, one Undo step,
document positions, compound paths, mixed selections, clones, locked
groups, groups with effects, links and switches, layers held out of both
commands).

### Make a Bitmap Copy: collective compositing

Mode: collective compositing (owner request BMP-1, 2026-09-26). Target:
the whole selection, rendered as it is seen on screen into one bitmap.

- The bitmap looks identical to the selection (owner decision
  2026-09-27). The render includes the effects of the selected objects'
  ancestors (opacity, filter, clip, mask, path effects), so the bitmap is
  placed where none of its own ancestors has such an effect: directly above the topmost selected object in that object's
  parent when no ancestor has one (the usual case), otherwise directly
  after the highest effect-carrying ancestor, in that ancestor's parent.
  Unselected objects that were above the topmost selected object inside
  that ancestor are then below the bitmap. With Keep original off, every
  selected object is deleted once (clones before the objects they use);
  with it on, nothing selected changes. OK is one Undo step; Cancel
  changes nothing. (The legacy dialog-free command still inserts inside
  the topmost object's parent.)
- A blend mode on an ancestor does not move the bitmap: the render hides
  everything unselected, so the blend is not in it and must still apply.
- Known limitations: a filter on an effect ancestor that spreads beyond
  the selection (blur, drop shadow) is cropped to the selection's visual
  bounds; opacity or a filter on the document root is applied twice (the
  bitmap cannot leave the root; as before); lifting out of a top-level
  layer puts the bitmap at the root, outside every layer; a sublayer
  inside a locked or hidden layer can receive the bitmap there.
- The command-line action `selection-make-bitmap-copy` keeps the previous
  dialog-free behaviour; the dialog is `selection-make-bitmap-copy-dialog`
  (Edit menu, Alt+B); `selection-make-bitmap-copy-repeat` reuses the
  dialog's last options.

Evidence: `testfiles/src/object-set-test.cpp` (`BitmapCopy*`: resolution,
replace mode, a shape with its clone, selections across layers, white
background, hard edges, one Undo step, preferences, size readout,
placement outside a semi-transparent layer or group, blend-mode layers).

### Selector moves: picture preview

Mode: collective geometry, unchanged; only the preview differs. When the
Selector moves the selection (a drag, not a handle) with "When transforming,
show: Objects" and "Fast preview when moving" on (default), a picture of the
selection rendered as seen (with its ancestors' effects, as Make a Bitmap
Copy) follows the pointer while the selected objects are hidden on that
canvas only; nothing in the document changes until release, when the
selection moves once through `ObjectSet::applyAffine` in one Undo step
(the Box outline path). Escape shows the objects again and changes nothing.
Scaling, rotating, skewing, sticky transforms, selections that something
references (clones, text on a path, also of a child) and the outline,
grayscale, split and clip-to-page display modes keep the live preview.
Known preview differences, none in the result: while dragging, the picture
is drawn above all artwork (an object under others appears on top until
release) and a blend mode is shown against transparency; parts of a very
large selection farther than a quarter view outside the window are not in
the picture; the picture is at most 16 megapixels (a full window at 2x
stays at screen resolution); a style change during the drag shows the
object at its old place until release.

Evidence: `testfiles/src/selector-interaction-test.cpp` (`MovePicture*`:
move and single Undo, a group and an object in another layer moved once
with one Undo, Escape, duplicate-drag cancel, rendered pixels of only the
selection, scaling, clones of the selection or of a child, outline mode
and the preference off stay live); the other selector tests run with the
preference off and cover the live preview.

### Convert Clipped Bitmaps: document-wide, one image per clip

Mode: per-object replacement over the whole document (owner request
CDR-1, 2026-09-27); the selection is not a target and is not read. Target
policy: each outermost clipped or masked item in visible, unlocked layer
content (never `<defs>`, never a layer itself) whose visible content is only
bitmaps. Each target is replaced once, on its own, by Make a Bitmap Copy in
replace mode (the placement and rendering rules above), topmost first; clip
paths and masks nothing uses afterwards are removed in the same step.
Excluded, left unchanged and reported by count: clips that also hold
vectors or text, and bitmap clips under a group with transparency, a
filter, a clip, a mask or a path effect (their image would be placed
outside that group, above its other content). Pure vector clips are
ignored. On opening a CorelDRAW file the conversion is recorded as one step
and the history is then cleared (owner decision: it cannot be undone; the
history then holds only automatic fixes made while opening); the Edit menu
command is one Undo step. Neither runs while a live preview or drag owns
the history. The dialog is modal; Cancel / Keep as They Are changes nothing.
The conversion then runs one item per main-loop turn behind a modal
progress window (images done / total, owner request 2026-09-29), inside a
rollbackable interaction: meanwhile no live preview or drag can start and
Undo is refused; the result is still one Undo step (menu) or folded into
the opened state (on open), and a conversion stopped before its end (the
document closed) is rolled back, never left half done. The interaction
does not stop another action from recording a history step between two
images (only a menu the platform leaves active could, as the window is
modal and cannot be closed); that step then holds the images made so far,
the conversion stops, reports that count and removes their unused clips.
Known exceptions on that path: from the menu, undoing the other action
also undoes those images, and the clip clean-up is a second Undo step;
on opening, clearing the history also drops the other action's step,
because the images are inside it.

Evidence: `testfiles/src/clipped-bitmaps-test.cpp` (scan cases, effect
groups, clipped layers and invisible images excluded, exclusions unchanged,
no-Undo conversion followed by an ordinary edit and Undo, one-Undo
conversion with Redo, unused clip paths and masks removed and restored by
Undo, pending edits kept, refusal during an interaction, rendered pixels,
stacking order inside an effect group, step-by-step progress as one Undo
step, a conversion stopped halfway leaving the document as it was, another
Undo step between two images stopping it with the real count).

## Required behavior matrix

### Stroke-width query: compatible members, before writer integration

`UI::query_stroke_widths` is a read-only compatible-member query. It visits
ordinary groups, deduplicates eligible owners, excludes bitmaps and protected
members with reasons, and never changes selection, XML, defaults or history.
Layers and other unsupported containers are explicit exclusions. Only eligible
runs contribute to uniform/mixed values and display flags. Ordinary stroke
widths use document CSS pixels; native non-scaling strokes and hairlines retain
their distinct conventions.

A valid explicit character range overrides object roots. A caret uses the
whole text owner while preserving the raw caret index; stale, foreign or
out-of-bounds ranges reject without broadening the edit scope. Text-path runs
belong to their selected text owner; referenced geometry is not an edit target.
Whole-owner queries exclude `tref` runs; explicit ranges touching them reject.

The clone query remains deliberately narrow: a plain shape instance whose
source width is unset or inherited. Explicit numeric source widths and complex
clone children are unavailable. Source and clone can be independent query
records, but never independent write outcomes. The writer preflights shared
sources and follows the selected-source exception below; excluded instances
are never detached. Query, writer/UI, Undo/Redo, rendering and save/reopen
remain separate evidence gates.

For the stroke-width writer, an explicit character subrange and separately
selected compatible shapes form one compatible-member edit and one Undo/Redo
action. The text owner keeps the explicit range; selected ordinary groups
traverse eligible members once and report bitmap, protected and unsupported
exclusions. Deliberate Hairline uses native hairline representation; Remove
Stroke changes only eligible stroke paint to none. Numeric absolute width
converts a hairline; relative percent and additive width exclude hairline
members. Numeric zero is a width, never an implicit Remove Stroke.

AdditiveCssPx is a finite, nonzero signed delta in document CSS px, with
|delta| <= 1e6. Each compatible member or text run uses its own pre-edit
effective width w: ordinary stroke is local width × i2doc_affine().descrim(),
including viewBox and ancestors; non-scaling stroke uses local width.
The target is w + delta, converted back to that member's local convention.
Reflections do not negate widths; non-uniform scaling uses the descriminant,
without promising direction-independent thickness. Unsafe local results
exclude that candidate. Canonically unrepresentable changes are Unchanged,
with no write, history entry or modified flag.

For a decrement of step s, the shared floor predicate is
w - s >= s - 1e-9 * max(1, w). A member failing it is Unchanged;
− never produces a width below one step. Positive deltas apply from zero.
Typing 0 and Remove Stroke remain deliberate ways to reach zero/no stroke.
Dash scaling snapshots /options/dash/scale and uses new local / old local;
old width zero or scaling disabled produces no dash patch. Unsafe dashes
exclude the candidate; negative offsets and independent priorities survive.

Text eligibility follows layout source provenance: structural controls retain
logical indices but receive no stroke patch; authored whitespace, soft hyphens
and overflow characters remain targets. Empty/missing-layout text is excluded.
Whole-owner/caret edits write each proven rendering source directly, preserving
source identity and raw whitespace, with no native fallback or threshold.
Flat text/tspan/flowSpan/flowPara/textPath sources and bounded one-level rendering
text/tspan/flowSpan/flowPara/flowDiv/textPath parents qualify; every flat child must receive its own changing patch,
including every changed inherited dash property. Unproved coverage is excluded
before mutation while compatible members apply. Partial ranges retain native
splitting and split at structural breaks. Classes/important, protected descendants,
source declarations and exact whole-owner glyph geometry remain verified.
A caret operation creates no text and changes no defaults.
F1b also preserves complete flat leaves under classed role=line wrappers in
partial ranges through the existing local CSS writer; partial leaves retain
native splitting. Exact remaining whole-owner exclusions are listed in L-ST-1;
round-two tests/timings are in `work/stroke-6800/F1b-report.md`.

After verified application, notes report, in order: distinct changed owners
with stroke paint none; linked clones following their changed original;
incompatible/protected items or skipped text runs. Skipped runs are counted
separately from excluded owners; following clones are not also reported as
skipped. "Stroke width applied" prefixes only a nonempty report. An ordinary
all-Unchanged action is silent; floor/exclusion no-ops explain why. Failed or
Rejected results produce no applied note or accepted-change count.

WP2b (7bd2c858e) implements the shared command and Fill and Stroke row.
The command uses the supplied desktop, never SP_ACTIVE_DESKTOP. It captures
scope before refreshing layout, then refuses a changed scope before read-only
preparation. Scope includes desktop/document lifetime, live generation,
ordered roots, display unit, tool identity and text owner/layout presence,
raw caret or range indices and direction. A no-change plan returns without
settling updates, acquiring a token, writing, touching defaults or consuming Redo.
Only a planned change permits quiescent pending automatic updates to be
settled as their own "Automatic update" Undo step, followed by layout currency,
re-capture, re-prepare and strict scope/plan comparison. A difference refuses.
`stroke_width_plans_match` compares intent, counts, text/run scope, frozen
style and every member/run patch, retaining provenance: parent, exact affine,
inline style text and every authored attribute. The only permitted difference
is an applied LPE's regenerated `d` with identical `inkscape:path-effect` and
`inkscape:original-d`; unrelated attribute or provenance changes refuse.

A live-scope predicate guards every write before and after it; a selection,
document, generation, unit, tool or raw text-scope change stops remaining
writes. Lifetime checks follow every callback boundary before dereferencing
or reporting through the desktop/document. Atomic commit re-captures the full
scope and verifies output; failure after writing rolls back, never publishing
partial changes. IME composition refuses with a message; composition starting
during application fails the live-scope check and rolls back.

Fill and Stroke uses [−] [value][▾] [+] [unit]. Uniform stepping uses an
absolute target from the exact queried value, not rounded display text;
Mixed ("—") uses a per-member additive delta. Uniformity/check marks use
1e-3 px tolerance; tolerance-uniform stepping may flatten smaller differences.
Steps are px/pt 0.1, pc 0.01, mm 0.05, cm 0.005, in 0.001,
m 0.00005 and ft 0.0001; digits are 3 except m (5) and ft (6).
Sensitivity follows writable members, not query eligibility alone.
− requires a decreasable member. Uniform hairline in a numeric unit permits
typing to convert and + to apply the smallest numeric preset; − is disabled.
In % mode ± are disabled; an applied preset switches to the last linear
unit's list unit after the command closes. An unchanged/failed preset keeps %.
The Hairline unit disables the field and ±; numeric presets leave that mode.

The shared lists are mm for mm/cm/m, otherwise pt:

- mm: Hairline, 0.1, 0.2, 0.25, 0.35, 0.5, 0.75, 1, 1.5, 2, 2.5, 3.
- pt: Hairline, 0.25, 0.5, 0.75, 1, 1.5, 2, 3, 4, 6, 8, 10, 12.
Hairline has the existing translated unit-menu label. Numeric descriptors
convert once from their own unit; the controller decides no-ops.

The field has no value drag, wheel stepping, hover arrows or held spinning.
Unmodified Up/Down act as ± once per physical press; modifiers and navigation
keys do not step. Holding a button's activation key does not repeat.
Merely opening/leaving the field never commits rounded text; unchanged blank
Mixed input is not pending. Invalid, negative or over-range pending text
remains open with a warning and no write. Both decimal separators are accepted;
thousands-like comma input such as 1,000 is rejected as ambiguous.
Queried widths are not clamped; typed widths above converted 1e6 px reject.
Before ±, valid pending text commits as its own action; only Applied or
Unchanged permits a fresh query and step. Thus a changed typed commit followed
by a changed step has two Undo steps. Selection/document/desktop changes
discard pending text. Field Undo/Redo discards it and explicitly invokes
document actions, never restores/recommits a focus-in value or uses GtkText Undo.

Pointer activation preserves canvas/text focus. Preset openings capture scope
and refuse stale activation; selection/style changes (including Undo/Redo),
document/desktop/unit changes, hiding/page switching or lost sensitivity close
the popover. Refresh and unit-display changes never write.
Object Properties retains its separate row: a documented divergence.

WP3 (877671af6) removes status-bar width drag, drag preview and wheel.
Those gestures acquire no token, claim no sequence, capture no shortcut and
produce no message, mutation, history, Redo or save-state change.
Left click still opens Stroke Style; width-indicator middle click is inert.
Colour-swatch gestures remain; stroke-swatch removal uses the shared command,
while none-to-last-colour restoration remains a colour operation.
The status menu retains unit radios and uses the shared per-unit descriptor
lists, Hairline and Remove Stroke through the command. Each opening captures
desktop/document, scope and opening generations, ordered roots, unit, tool,
text owner/layout presence and raw caret/range indices and direction.
Stale activation refuses; closing consumes the opening before dispatch,
including synchronous reentry. Unit/display refresh never writes.
BUG-012 is fixed: last-used stroke colour targets stroke, and the stroke
opacity action records "Make stroke opaque".

Paint order (WP0, 54900d9d3): Fill and Stroke's Order row writes through the
panel's current desktop under an update guard and records one "Set paint order"
Undo step. The query counts unstroked objects with explicit paint-order,
including group/text owners. Writing still uses the existing recursive style
path; its writes to hidden/locked descendants remain a documented migration
gap, not compatible-member stroke-width policy.

### Break Apart (BRK-1): each object by its kind

`app.break-apart` (Ctrl+K, owner decision 2026-09-29) is compatible-member
editing over the selection: each selected text goes one level down (lines,
then words, then letters) into separate editable texts; each selected path
with several subpaths is broken with the existing path Break Apart (unchanged,
still `app.path-break-apart`, Ctrl+Shift+K). Texts on a path, vertical texts
or texts with right-to-left runs, texts flowed into a shape, texts with a
clip or mask, texts that clones or other objects refer to, texts with rotate
or textLength, texts with nothing to break, groups, images and other objects
are left untouched and reported in the status bar. The first piece keeps the
text's id; inline-size (wrapped) text becomes point text at the same place. Each object is
affected once; the pieces keep every glyph's exact position (layout anchors;
text-anchor rewritten to start; explicit positions when the text uses
kerning lists or a piece spans several positioned chunks, per cluster when a
ligature or combining mark shares a glyph), fonts, colours and strokes (run
styles, including presentation attributes, written as the difference from
the piece's root). Every piece is verified against the original drawing
(0.01 px) in a trial outside the history before anything is recorded; a text
whose pieces cannot match is left unchanged and reported. One Undo step for
the whole selection; a selection with nothing to break is a no-op with no
history entry. The pieces
and the untouched objects stay selected so Ctrl+K goes to the next level.
Tests: `test_text-break-apart`.

Text structure is preserved by a width write: a tspan positioned by its own
x or y (CorelDRAW imports position lines that way) is never merged into its
predecessor or deleted when empty by the writer's tidy pass; spans merge or
dissolve only when their presentation-attribute styles agree, so line
positions, blank lines and the colours of flat runs survive (BUG-009).

Selected source and clone (owner decision 2026-09-28, narrow exception to the
earlier whole-action clone guard): when a selected clone's source graph holds
a planned change, the source and other eligible members
change in one Undo step; the clone is excluded, never written or detached,
and reported as following its original. This includes clones reached only
inside selected locked/unsupported groups. Clone scaling can change the visible
result by its own scale; protected clones can visibly follow a changed source.
A missing, cyclic or foreign source graph rejects the action when changes are
planned. Pre-write/output/commit checks require the same following clones and
live source relationships; retargeting refuses or rolls back.

WP1c (a39c00cf3): an initially unset source width becomes explicit after
writing, so a following clone's query class can become CloneSourceOverrides.
The output checks expect exactly that post-write class, and only for clones
that follow a changed source. Covered duplicates of such a clone are accounted
for. A flipped clone must keep its original source. Excluded clones keep a
preserved binding, affine, attribute and inline-style snapshot, which the
plan comparators and output checks require unchanged. Every other record is
compared strictly.

### Selected vector export: temporary document projection

Export pruning operates on an independent document copy, rather than applying
the editing Delete command. Retain the selected roots, their descendants and
ancestor structure, and the forward references and live-effect sources needed
by that retained structure. Inspecting an ancestor's dependencies does not
select its unrelated children. Preserve retained object identity, order,
transforms, styles and payloads; do not flatten effects or reparent artwork to
avoid a crash. Normal editing Delete and its notifications remain unchanged.

Both single-object command-line export and the Export panel use this policy.
Cropping must not mutate the original document, its selection or its undo
history. The native crop itself remains reversible when deliberately exercised
inside a test undo transaction. Empty plural crop input is a no-op; the legacy
singular null argument removes renderable children, although the export callers
use a resolved non-null object. Retained dependencies may be outside the fitted
export page; preserving those references does not expand the selected bounds.

### Text formatting scope for the native-operation reuse repair

For ordinary multiline SVG text, alignment is an object-level compatible-member
operation: native toolbar and text panel align each eligible text object as a
whole, including when a caret or character subrange is active. Changing only one
legacy `role="line"` span is not equivalent to native object alignment. Preserve
the native alignment-point adjustment and the text object's position contract.
SVG2 wrapped text, shape-inside text and explicitly authored paragraph structures
retain paragraph-local panel alignment where that is their documented operation;
do not flatten authored paragraphs to mimic ordinary SVG text serialization.

Ordinary-text line height uses native whole-object semantics for a caret or whole
object selection, and native inner-range semantics for an explicit partial text
selection. Preserve unselected run styles when normalizing the root line-height
minimum. Wrapped paragraphs require their own scope and preservation tests.
Character/word spacing and other run formatting retain their existing character
selection semantics. UI unit differences must convert to equivalent results.

These rules define the intended repair, not verified current behavior. Tests must
exercise actual UI entry points as well as shared helpers and compare rendered
positions from equivalent starting documents. An exception in paragraph scope
must be named in the repair evidence, not reported as universal toolbar parity.

For every changed operation exercise: single bitmap/vector/text as applicable;
multiple same-type items; mixed compatible/incompatible items; unsupported-only;
nested groups; parent plus child; clone alone and clone plus source; missing
source; locked/hidden members; existing distinct property values; existing
filters/clips/masks; preview/cancel; one Undo/Redo; save/reopen and rendered result.
Test differences that matter to that operation; explain non-applicable cells.
Never count source-pattern checks as rendered behavior or skipped GUI fixtures
as passes. Add the report's real failing fixture before claiming that regression
fixed. A controller-only test is insufficient to prove sliders reach it.

## Local enforcement and review

Run the configured application's `check-selection-contract` target and
affected feature/query suites. The intended application inventory includes
resolver, tone chemistry/controller, destructive bitmap clip, stroke-width
query/controller and the suites below. Require `selection-contract` and
`vacards-critical` labels, inclusion in the local critical build/test path,
and guards rejecting SKIPPED, disabled or zero-test runs. GUI suites require
`INKSCAPE_TEST_GUI=1` and `G_DEBUG=fatal-criticals`.

Intended inventory:

- `test_paint-order-query` (headless): explicit unstroked orders.
- `test_paint-order-widget` (GUI): current-desktop writes, recursive outcomes,
  refresh and exact Undo/Redo.
- `test_spin-button-policies` (GUI): opt-ins, locale validation, pending
  edits/Undo and unchanged default behavior.
- `test_stroke-width-row` (GUI): real row/command entry points, scope, Mixed,
  floor, units, pending commits, presets, focus and document Undo/Redo.
- `test_stroke-width-statusbar` (GUI, rewritten in WP3): real-event preservation
  for inert width drag/wheel/middle click, actual menu presets, removal,
  text-range and LPE transactions, stale-menu refusal and BUG-012.

Preserve JUnit, executed case inventory, source revision/diff and runtime
identities; native event/platform evidence remains separate.
WP1c (a39c00cf3) covers unset-source clone transitions and text-write
complexity with deterministic work counters: pending-owner checks, native
and direct writes, layout rebuilds and source lookups. These are asserted as
bounds, not wall-clock. Nine native-versus-direct differential tests cover
the bulk text path. Controller timings on the Mac (plan §8b): 2,000 short
texts about 0.28 s, and a 300-run text about 0.14 s, per click. Controller-only
timings do not qualify click-through-refresh performance.

WP4 native smoke (2026-09-30): the a6afe4cd4 payload ran unpacked, with no
install, on the Windows printer PC while printing was idle. It used an isolated
profile and injected mouse and keyboard input on the maximized window. On a mixed
selection (group, shapes, a two-span text, unstroked members) these passed:
- `+` adds one step to each stroked member and reports the unstroked ones;
- the mm presets, including the status-bar menu, each apply in one Undo step,
  and two Undos restore the unmodified file;
- a decimal comma is accepted;
- `−` stops at one step and then disables;
- a 2 s press-and-hold on `+` applies once, and so does a held Enter on the
  focused `+`;
- wheel and drag over the field and the width indicator change nothing;
- Remove Stroke writes `stroke:none`, keeps the width, and its Undo restores
  the strokes.

The smoke found one wording defect, fixed afterwards: Remove Stroke reported
its own none paint as "N objects have no stroke colour". Not exercised on
Windows: precision touchpad, touch, IME, the floating dock and display scaling.

For diagnosis without GTK, configure `testfiles/selection-contract` separately.
That runs only resolver semantics, not the application gate. macOS evidence is
not Windows qualification. Missing app tests block readiness, not source work.

The AGENTS.md rule and local targets are implemented safeguards, not protected
server-side merge enforcement. There is no new cloud workflow, scheduler or
permission to bypass existing release gates. Reviewers must check evidence and
reject undocumented exceptions. An agent cannot self-certify unrun tests.

### Explode Bitmap 1.5 contour publication (C2)

A supported single bitmap is one compatible-target replacement and one Undo/Redo
operation. With contours, Explode selects one group per piece: the image keeps
its ID and shares one pixel-to-parent transform with the cut path above it.
Contours are closed compound paths with no fill and a stroke; noContour pieces
remain image-only groups and are counted. Without contours the image-only output
is unchanged. Create contour only replaces the bitmap at its existing sibling
position with a group containing an exact copy of its element and one compound
path above it; the bitmap is not split. Preview never publishes these groups;
failed publication restores the original XML, selection and history.
