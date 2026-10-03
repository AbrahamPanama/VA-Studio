[Inkscape Developer Documentation](readme.md) /

# Text system architecture

This document describes how text moves through Inkscape: from GTK input and the SVG object tree,
through font discovery, shaping, layout, editing, canvas rendering, and export. It is a source-level
map for developers who need to modify text behavior without breaking Unicode, SVG positioning,
font fallback, round trips, or editing state.

The analysis is based on source revision `d2b9be1c534e1c742fb5bbcce6e4c30a0833232d` from
2026-08-26. Line references describe that checkout and will drift as the source changes. Existing
uncommitted feature work was left untouched.

## Executive summary

Inkscape does not hand an SVG `<text>` element to a single toolkit text widget or `PangoLayout`.
It owns the document model, editing model, SVG positioning rules, line wrapping, text-on-path
placement, drawing objects, and most export policy. Pango itemizes text and provides language,
script, bidi, break, cursor, font-fallback, and shaping data; HarfBuzz and FreeType are reached
through Pango and `FontInstance` for glyph IDs, OpenType data, metrics, and outlines.

The authoritative content is the SVG/XML tree. `TextTool` holds temporary interaction state, and
`Inkscape::Text::Layout` holds a derived, disposable layout. Every edit must therefore preserve two
mappings:

1. source XML characters and style/positioning nodes to shaped characters and glyphs; and
2. shaped cursor positions back to exact XML insertion and deletion locations.

The complete flow is:

```text
GTK pointer / keyboard / IME
          |
          v
CanvasEvent -> TextTool -> text-editing.cpp -> SVG/XML object tree
                                             |          |
                                             |          +-> DocumentUndo
                                             v
                                      SPText / SPStyle
                                             |
                                             v
                                  Layout-TNG input stream
                                             |
                       Pango itemization, fallback and shaping
                            (HarfBuzz + FreeType underneath)
                                             |
                                             v
                       paragraphs / lines / chunks / spans /
                              characters / positioned glyphs
                                  /                     \
                                 v                       v
                     DrawingText canvas          print/export paths
```

The most consequential maintenance facts are:

- layout iterators become stale whenever layout is rebuilt;
- characters and glyphs have a many-to-many relationship;
- IME preedit is disabled, so only committed strings enter the canvas editor;
- font enumeration uses Fontconfig and a Pango FT2 map, including on macOS;
- there are two different font-list UI/data paths with different refresh behavior;
- many text/style changes rebuild the complete layout and recreate drawing glyphs;
- screen rendering, Cairo export, and the newer PDF builder are distinct implementations;
- legacy flowed text and several interchange conversions are intentionally lossy; and
- direct tests of interactive text mutation are much weaker than shaping and pixel-rendering tests.

## Ownership by layer

| Layer | Primary responsibility | Main implementation |
|---|---|---|
| GTK/canvas | Pointer, keyboard, focus, and IME event capture | `Canvas`, `CanvasEvent`, `TextTool` |
| Interaction | Active text, caret, subselection, gestures, pending style | `src/ui/tools/text-tool.*` |
| Mutation | Insert, replace, delete, line split/join, per-character attributes | `src/text-editing.*` |
| Document model | SVG objects, XML strings, references, serialization | `SPText`, `SPTSpan`, `SPTextPath`, `SPFlowtext`, `SPString` |
| Style | CSS cascade and computed text properties | `SPStyle`, `style-text.cpp` |
| Layout | Source mapping, wrapping, line breaking, SVG positioning, cursor geometry | `src/libnrtype/Layout-TNG-*` |
| Shaping boundary | Itemization, bidi runs, language, fallback, clusters and glyphs | Pango, with HarfBuzz underneath |
| Font boundary | Fontconfig enumeration, face cache, glyph metrics/outlines/OpenType | `FontFactory`, `FontInstance` |
| Canvas output | Glyph drawing objects, paint order, decorations, picking | `DrawingText`, `DrawingGlyphs` |
| Interchange | SVG, PDF/PS/EPS, EMF/WMF, XAML and clipboard policy | extensions, render contexts, chemistry functions |

## Text tool activation and event routing

The default Text tool shortcuts are `T` and `F8` in
[`share/keys/inkscape.xml:111`](../share/keys/inkscape.xml#L111). The `win.tool-switch`
action reaches `SPDesktop::setTool()`, which replaces the active tool through `ToolFactory` and
emits a tool-change signal
([`src/actions/actions-tools.cpp:124`](../src/actions/actions-tools.cpp#L124),
[`src/desktop.cpp:324`](../src/desktop.cpp#L324),
[`src/ui/tool-factory.cpp:90`](../src/ui/tool-factory.cpp#L90)). The toolbar registry separately
associates `/tools/text` with `TextToolbar`
([`src/ui/toolbar/toolbars.cpp:78`](../src/ui/toolbar/toolbars.cpp#L78)).

The canvas owns GTK gesture, motion, focus, and key controllers. It converts GDK input into the
Inkscape `CanvasEvent` variant while retaining the original key event for input-method filtering
([`src/ui/widget/canvas.cpp:323`](../src/ui/widget/canvas.cpp#L323),
[`src/ui/widget/canvas.cpp:1139`](../src/ui/widget/canvas.cpp#L1139)). Events are routed through the
desktop and `ToolBase`; object hits reach `TextTool::item_handler()`, while empty-canvas events reach
`TextTool::root_handler()`
([`src/desktop.cpp:1527`](../src/desktop.cpp#L1527),
[`src/ui/tools/tool-base.cpp:1240`](../src/ui/tools/tool-base.cpp#L1240)).

### Interactive state

`TextTool` stores the selected text item, two `Layout::iterator` values for the caret or selected
range, nascent-object/drag state, canvas caret and highlight objects, text-on-path and text-in-shape
hover state, a pending insertion style, and a GTK input-method context
([`src/ui/tools/text-tool.h:42`](../src/ui/tools/text-tool.h#L42)).

The active text invariant is intentionally narrow: editing state is attached only when the desktop
has one selected `SPText` or `SPFlowtext`. A selection change clears pending style and resets the
two iterators to a valid location in the current layout
([`src/ui/tools/text-tool.cpp:1489`](../src/ui/tools/text-tool.cpp#L1489)).

Mouse behavior on existing text is implemented in
[`src/ui/tools/text-tool.cpp:179`](../src/ui/tools/text-tool.cpp#L179):

- single click places the caret, with Shift extending the selection;
- double click selects a word;
- triple click selects a line; and
- dragging extends by character, word, or line according to the initial click count.

Point text and text frames are initially *nascent*: the gesture stores geometry but does not yet add
an SVG object. The first insertion calls `_setupText()`, which chooses point text, inline-size text,
legacy flowed text, shape-inside text, or text-on-path, creates the appropriate XML, selects it, and
records creation in undo
([`src/ui/tools/text-tool.cpp:254`](../src/ui/tools/text-tool.cpp#L254)). This postpones empty text
objects until the user actually types.

### Keyboard and IME input

`TextTool` creates a `GtkIMMulticontext`, connects focus and the `commit` signal, and forwards raw
key events to `gtk_im_context_filter_keypress()` before normal shortcut handling
([`src/ui/tools/text-tool.cpp:97`](../src/ui/tools/text-tool.cpp#L97),
[`src/ui/tools/text-tool.cpp:700`](../src/ui/tools/text-tool.cpp#L700)).

IME preedit is explicitly disabled. Inkscape does not display a native composition string or
composition underline on the canvas; it receives the completed UTF-8 commit. `_commit()` creates a
nascent object if necessary, replaces the current range, applies pending caret style, updates the
caret, and records `Type text`
([`src/ui/tools/text-tool.cpp:1898`](../src/ui/tools/text-tool.cpp#L1898)). Candidate-window
placement is updated from the caret rectangle, with an acknowledged coordinate-conversion
uncertainty
([`src/ui/tools/text-tool.cpp:1689`](../src/ui/tools/text-tool.cpp#L1689)).

Other key paths include Unicode hex entry, line insertion, Backspace/Delete, word and visual bidi
navigation, Home/End/Page movement, manual kerning, rotation, spacing, and Escape cancellation
([`src/ui/tools/text-tool.cpp:725`](../src/ui/tools/text-tool.cpp#L725),
[`src/ui/tools/text-tool.cpp:932`](../src/ui/tools/text-tool.cpp#L932),
[`src/ui/tools/text-tool.cpp:957`](../src/ui/tools/text-tool.cpp#L957),
[`src/ui/tools/text-tool.cpp:1037`](../src/ui/tools/text-tool.cpp#L1037)). Valid cursor, word, sentence,
and grapheme/backspace boundaries come from Pango log attributes stored in the layout
([`src/libnrtype/Layout-TNG-OutIter.cpp:863`](../src/libnrtype/Layout-TNG-OutIter.cpp#L863)).

## Editing and XML mutation

The object tree, not `TextTool`, is the durable source of text. The public editing seam is declared
in [`src/text-editing.h:24`](../src/text-editing.h#L24). The central operations are:

- `sp_te_insert()` at [`src/text-editing.cpp:549`](../src/text-editing.cpp#L549);
- `sp_te_delete()` at [`src/text-editing.cpp:781`](../src/text-editing.cpp#L781);
- `sp_te_replace()` at [`src/text-editing.cpp:171`](../src/text-editing.cpp#L171);
- line insertion/splitting at [`src/text-editing.cpp:397`](../src/text-editing.cpp#L397); and
- range styling at [`src/text-editing.cpp:2063`](../src/text-editing.cpp#L2063).

An insertion maps the layout iterator back to an `SPString` and a UTF-8 source offset, mutates the
XML text node, and updates the ancestor character-position lists. Deletion can cross strings,
tspans, and line containers; it erases corresponding positional entries, joins structures, removes
empty nodes, and returns new iterators. Replacing is delete followed by insert.

`TextTagAttributes` is responsible for keeping `x`, `y`, `dx`, `dy`, and `rotate` aligned with source
characters during insert, erase, split, join, and transform operations
([`src/text-tag-attributes.h:31`](../src/text-tag-attributes.h#L31)). This is why a character edit is
not merely a string operation.

Mutation functions force an immediate layout update rather than waiting for the normal idle update
([`src/text-editing.cpp:55`](../src/text-editing.cpp#L55)). This is necessary because style
consolidation and tree cleanup can destroy an `SPString`, and any existing layout iterator would
otherwise refer to stale or freed source state
([`src/text-editing.cpp:2149`](../src/text-editing.cpp#L2149)).

### Undo

Text code changes XML first and then commits the accumulated XML transaction using
`DocumentUndo::done()` or `maybeDone()`. `maybeDone(key)` coalesces repeated adjustments such as
manual kerning and spacing
([`src/document-undo.cpp:16`](../src/document-undo.cpp#L16),
[`src/document-undo.cpp:154`](../src/document-undo.cpp#L154)). Because undo/redo can replace text
content and trigger layout reconstruction, `TextTool` validates both stored iterators before using
them
([`src/ui/tools/text-tool.cpp:1623`](../src/ui/tools/text-tool.cpp#L1623)).

### Toolbar, dialog, and spellcheck

`TextToolbar` listens to both object selection and text-cursor/subselection signals. A nonempty
subselection is styled through `TextTool::_styleSet()`; otherwise styling targets the selected outer
text object or the default `/tools/text/style`
([`src/ui/toolbar/text-toolbar.cpp:297`](../src/ui/toolbar/text-toolbar.cpp#L297),
[`src/ui/tools/text-tool.cpp:1532`](../src/ui/tools/text-tool.cpp#L1532)). This outer-object versus inner-
range distinction is a recurring source of special cases in line-height, font, and writing-mode
code.

The classic Text and Font dialog keeps a separate editable text buffer. Text content is only written
back on Apply, using `sp_te_set_repr_text_multiline()`, while font/style controls may apply
continuously
([`src/ui/dialog/text-edit.cpp:405`](../src/ui/dialog/text-edit.cpp#L405),
[`src/text-editing.cpp:958`](../src/text-editing.cpp#L958)). Rewriting the multiline representation
reconstructs line children and can flatten inline span structure or per-range formatting; it should
not be treated as an identity-preserving editor for complex text trees.

When compiled with libspelling, the spellcheck dialog traverses visible unlocked text, uses layout
word boundaries, moves the Text tool caret to the match, and corrects it with `sp_te_replace()`
([`src/ui/dialog/spellcheck.cpp:171`](../src/ui/dialog/spellcheck.cpp#L171),
[`src/ui/dialog/spellcheck.cpp:304`](../src/ui/dialog/spellcheck.cpp#L304),
[`src/ui/dialog/spellcheck.cpp:523`](../src/ui/dialog/spellcheck.cpp#L523)).

## SVG/XML object model

The regular text hierarchy is:

```text
svg:text -> SPText
  |- XML text node -> SPString
  |- svg:tspan -> SPTSpan
  `- svg:textPath -> SPTextPath
```

Legacy SVG 1.2 draft flow uses a separate hierarchy rooted in `SPFlowtext`, with `flowRegion`,
`flowRegionExclude`, `flowPara`, `flowDiv`, and `flowSpan`. The object factory registrations are in
[`src/object/sp-factory.cpp:163`](../src/object/sp-factory.cpp#L163) and
[`src/object/sp-factory.cpp:240`](../src/object/sp-factory.cpp#L240).

`SPString` keeps a normalized `Glib::ustring` for layout while the XML text node retains raw
character content
([`src/object/sp-string.h:22`](../src/object/sp-string.h#L22),
[`src/xml/text-node.h:21`](../src/xml/text-node.h#L21)). Whitespace normalization combines CSS
`white-space` and legacy `xml:space`, including state crossing adjacent text nodes
([`src/object/sp-string.cpp:54`](../src/object/sp-string.cpp#L54),
[`src/object/sp-string.cpp:104`](../src/object/sp-string.cpp#L104)).

### Style cascade

`SPStyle` registers font family/specification, size, weight, stretch, variants/features, spacing,
line height, alignment, direction, writing mode, text orientation, baseline, anchor, whitespace,
inline size, shape-inside, shape-padding, and shape-subtract properties
([`src/style.cpp:86`](../src/style.cpp#L86)). Style reading combines inline `style`, stylesheet rules,
presentation attributes, and inherited computed values
([`src/style.cpp:553`](../src/style.cpp#L553),
[`src/style.cpp:810`](../src/style.cpp#L810)).

`SPText::build()` reads the SVG position lists plus `textLength` and `lengthAdjust`
([`src/object/sp-text.cpp:79`](../src/object/sp-text.cpp#L79)). During recursive layout input
construction, `TextTagAttributes::mergeInto()` overlays child lists on the inherited parent lists at
the correct source-character offset. `textLength` intentionally does not merge in the same way.

## Layout-TNG

`Inkscape::Text::Layout` is the central seam between editable SVG text and rendered glyphs. Its
header explains the design and terminology in
[`src/libnrtype/Layout-TNG.h:52`](../src/libnrtype/Layout-TNG.h#L52). It accepts calls to
`appendText()` and `appendControlCode()`, optionally accepts wrap shapes, and produces a hierarchy:

```text
flow -> shape -> paragraph -> line -> chunk -> span
                                      |          |
                                      |          +-> glyphs
                                      `------------> characters
```

Characters and glyphs deliberately overlap rather than nest one-to-one. Ligatures can represent
multiple characters, combining sequences can produce multiple glyph relationships, and control or
hidden characters may have no glyph.

### Rebuild lifecycle

`SPText::rebuildLayout()` performs the main conversion
([`src/object/sp-text.cpp:870`](../src/object/sp-text.cpp#L870)):

1. clear old input and output;
2. initialize font metrics and wrap mode;
3. walk the text object tree and append styled source ranges/control codes;
4. calculate shaping, wrapping, and glyph positions;
5. fit text-path glyph clusters to the referenced path; and
6. update line-span positions used by editing and serialization.

`_buildLayoutInit()` chooses normal, whitespace, inline-size, or shape-inside wrapping and constructs
the necessary wrap geometry
([`src/object/sp-text.cpp:472`](../src/object/sp-text.cpp#L472)). `_buildLayoutInput()` recursively
adds strings with effective style, source identity, language, character offsets, and merged position
lists
([`src/object/sp-text.cpp:538`](../src/object/sp-text.cpp#L538),
[`src/object/sp-text.cpp:673`](../src/object/sp-text.cpp#L673)).

Calling `calculateFlow()` invalidates all existing `Layout::iterator` values
([`src/libnrtype/Layout-TNG.h:339`](../src/libnrtype/Layout-TNG.h#L339)). Code must either convert a
position back to source identity before rebuilding or validate/recreate its iterators afterward.

### Pango and HarfBuzz responsibilities

The layout engine concatenates each paragraph's source ranges, adds font, feature, language, and
direction attributes, and calls Pango itemization
([`src/libnrtype/Layout-TNG-Compute.cpp:1077`](../src/libnrtype/Layout-TNG-Compute.cpp#L1077)). Pango
returns runs split by script, bidi level, language, and resolved fallback font, plus log attributes
for line breaks, cursor stops, word boundaries, and backspace behavior.

Each run is then split at Inkscape source/style/SVG-position boundaries and shaped with
`pango_shape_full()`
([`src/libnrtype/Layout-TNG-Compute.cpp:1218`](../src/libnrtype/Layout-TNG-Compute.cpp#L1218),
[`src/libnrtype/Layout-TNG-Compute.cpp:1371`](../src/libnrtype/Layout-TNG-Compute.cpp#L1371)). Pango
uses HarfBuzz for shaping. Inkscape converts those runs into its own characters and glyphs, then
applies SVG `x/y/dx/dy/rotate`, spacing, anchoring, justification, line fitting, baselines,
`textLength`, vertical-writing corrections, and path placement.

CSS `direction` provides the Pango base direction, while visual left/right cursor movement is
implemented over the output layout rather than raw logical source order
([`src/libnrtype/Layout-TNG-Compute.cpp:1138`](../src/libnrtype/Layout-TNG-Compute.cpp#L1138),
[`src/libnrtype/Layout-TNG-OutIter.cpp:1004`](../src/libnrtype/Layout-TNG-OutIter.cpp#L1004)).
Vertical layout configures Pango gravity and text orientation before shaping. `unicode-bidi` is
listed among unimplemented style properties, so full CSS isolation/embedding behavior is not
available
([`src/style.cpp:254`](../src/style.cpp#L254)).

### Wrapping and special text modes

| Text mode | Representation and layout behavior |
|---|---|
| Point/free text | Normal `<text>` positioning with `x/y/dx/dy/rotate`. |
| Inkscape line text | `sodipodi:role="line"` tspans create explicit line/paragraph controls and special singleton `x/y` behavior. |
| Inline-size text | `<text>` plus CSS `inline-size`; a rectangular wrap region extends in block progression. |
| Shape-inside text | Referenced shapes become Livarot inclusion regions; padding and exclusion shapes modify scan runs. |
| Text on path | Text is shaped normally, then glyph clusters are mapped by distance and tangent. |
| Legacy flow | `flowRoot` and flow region/paragraph objects feed the same general layout engine through separate object code. |

Shape-based flow uses `ShapeScanlineMaker` to compute available intervals for each line and a greedy
line-fitting algorithm. The source labels this exact scanline path as slow
([`src/libnrtype/Layout-TNG-Scanline-Maker.h:124`](../src/libnrtype/Layout-TNG-Scanline-Maker.h#L124)).

Text-on-path mapping happens after normal layout. Cluster midpoints are mapped to path distance and
tangent; closed paths wrap offsets while clusters outside an open path become hidden
([`src/libnrtype/Layout-TNG-Output.cpp:670`](../src/libnrtype/Layout-TNG-Output.cpp#L670)).

`textLength` is modeled at complete-layout scope rather than independently for every nested tspan
([`src/libnrtype/Layout-TNG.h:296`](../src/libnrtype/Layout-TNG.h#L296)). Any work in this area needs
tests for both `spacing` and `spacingAndGlyphs`, zero-length content, nesting, bidi, and vertical
text.

## Font discovery and management

The effective font stack is:

```text
font directories -> Fontconfig -> Pango FT2 map -> FontFactory
    -> Pango itemization/fallback -> FontInstance -> HarfBuzz/FreeType
```

In GUI mode Inkscape requests the Fontconfig PangoCairo backend, while `FontFactory` creates a
private `PangoFT2FontMap`, one shared Pango context, and a Fontconfig configuration. It fixes the map
at 72 DPI and filters for outline fonts
([`src/inkscape.cpp:209`](../src/inkscape.cpp#L209),
[`src/libnrtype/font-factory.cpp:59`](../src/libnrtype/font-factory.cpp#L59)).

This means the application does not directly enumerate or match through CoreText on macOS. The
packaged Fontconfig configuration scans `/System/Library/Fonts`, `/Library/Fonts`, and
`~/Library/Fonts`, and stores its cache under Application Support
([`packaging/macos/res/fonts.conf:17`](../packaging/macos/res/fonts.conf#L17)). Inkscape additionally
registers shared, user, and preference-defined font directories at startup
([`src/inkscape.cpp:214`](../src/inkscape.cpp#L214)).

### Enumeration, matching, and fallback

`FontFactory` lists Pango families/faces, rejects missing or invalid UTF-8 names, normalizes generic
families (`Sans` to `sans-serif`, for example), hides unsuitable synthesized duplicates, and recovers
variable-font named instances
([`src/libnrtype/font-factory.cpp:111`](../src/libnrtype/font-factory.cpp#L111),
[`src/libnrtype/font-factory.cpp:232`](../src/libnrtype/font-factory.cpp#L232),
[`src/libnrtype/font-factory.cpp:288`](../src/libnrtype/font-factory.cpp#L288)).

`SPStyle` becomes a `PangoFontDescription` containing family, weight, style, stretch, variant, and
variation settings
([`src/style-text.cpp:12`](../src/style-text.cpp#L12)). `FontFactory::Face()` loads and caches a
`FontInstance`; when permitted, a failed explicit face falls back to `sans-serif`
([`src/libnrtype/font-factory.cpp:472`](../src/libnrtype/font-factory.cpp#L472)). Pango performs
additional per-character fallback during itemization, and Inkscape stores the actual resolved font
on each output span.

`FontInstance` is the low-level bridge. It obtains Pango's HarfBuzz font, makes a sub-font that can
expose a FreeType face, reads OpenType tables, and lazily caches glyph advances, extents, outlines,
metrics, and SVG-in-OpenType data
([`src/libnrtype/font-instance.cpp:144`](../src/libnrtype/font-instance.cpp#L144),
[`src/libnrtype/font-instance.cpp:367`](../src/libnrtype/font-instance.cpp#L367)).

### Variable fonts and OpenType features

HarfBuzz reads `fvar` axes and named instances; hidden axes are omitted from the UI
([`src/libnrtype/OpenTypeUtil.cpp:301`](../src/libnrtype/OpenTypeUtil.cpp#L301)). The variation UI
serializes coordinates using Pango's `@tag=value` syntax. `wght` and `ital` may be promoted to CSS
`font-weight` and `font-style`; other axes remain in `font-variation-settings`
([`src/ui/widget/font-variations.cpp:356`](../src/ui/widget/font-variations.cpp#L356),
[`src/libnrtype/font-utils.cpp:19`](../src/libnrtype/font-utils.cpp#L19)).

OpenType GSUB tables are inspected through HarfBuzz to enable relevant feature controls. CSS
`font-variant-*` and raw `font-feature-settings` are converted into a Pango feature string before
shaping
([`src/libnrtype/OpenTypeUtil.cpp:129`](../src/libnrtype/OpenTypeUtil.cpp#L129),
[`src/style.cpp:1021`](../src/style.cpp#L1021)).

### Two font-list paths

The codebase currently contains two font-list implementations:

1. `FontLister`, used by the Text toolbar and classic Text and Font dialog, keeps global GTK list
   stores, lazily loads styles, places document fonts above system fonts, and marks missing families;
   and
2. `FontDiscovery`/`FontList`, used by the newer Font Browser, asynchronously enumerates faces,
   caches metadata, and supports search, grouping, collections, previews, variations, and missing-
   font injection.

See [`src/libnrtype/font-lister.cpp:410`](../src/libnrtype/font-lister.cpp#L410),
[`src/util/font-discovery.cpp:343`](../src/util/font-discovery.cpp#L343), and
[`src/ui/widget/font-list.cpp:618`](../src/ui/widget/font-list.cpp#L618).

Document font tracking records names/styles found in object styles; it does not embed font binaries.
The font-substitution dialog compares requested families with fonts actually used by layout runs and
can select affected objects, but it does not rewrite their CSS
([`src/ui/dialog/font-substitution.cpp:88`](../src/ui/dialog/font-substitution.cpp#L88)). User font
collections and recent-font lists persist family names in the profile; unavailable entries are
dropped when reloaded
([`src/util/font-collections.cpp:39`](../src/util/font-collections.cpp#L39)).

### Caching and refresh

Important caches are Fontconfig's disk cache, the finite `FontFactory::loaded` face cache, lazy
per-font glyph/OpenType data, the Font Browser's `font-cache.ini`, and the canvas's SVG color-glyph
pixbuf cache. The legacy list watches GTK's `gtk-fontconfig-timestamp`, tells Pango the configuration
changed, and rebuilds its family list
([`src/libnrtype/font-lister.cpp:79`](../src/libnrtype/font-lister.cpp#L79)).

The refresh paths are not unified:

- `FontFactory::refreshConfig()` does not explicitly clear loaded `FontInstance` values;
- `FontDiscovery` keeps its completed result and does not observe the legacy refresh signal;
- metadata-cache keys do not include a font-file timestamp or fingerprint; and
- `FontFactory` and its shared Pango context expose no obvious operation-level mutex while the new
  discovery path performs work asynchronously.

These are architectural risks, not proof of a current race. Relevant code is
[`src/libnrtype/font-factory.cpp:106`](../src/libnrtype/font-factory.cpp#L106),
[`src/util/font-discovery.cpp:233`](../src/util/font-discovery.cpp#L233), and
[`src/util/font-discovery.cpp:459`](../src/util/font-discovery.cpp#L459).

Relative TTF/OTF references in `@font-face` can be registered as application fonts, but the source
explicitly notes that font UI refresh is incomplete
([`src/object/sp-style-elem.cpp:320`](../src/object/sp-style-elem.cpp#L320)).

## Canvas rendering and hit testing

The interactive canvas path is:

```text
SPText::show()
  -> Layout::show()
    -> DrawingText per styled span
      -> DrawingGlyphs per positioned glyph
        -> cached outline/bounds or SVG-glyph pixbuf
```

`Layout::show()` creates drawing objects and attaches style/paint information
([`src/libnrtype/Layout-TNG-Output.cpp:144`](../src/libnrtype/Layout-TNG-Output.cpp#L144)).
`DrawingGlyphs::setGlyph()` loads the outline, exact/pick/draw bounds, and optional OpenType SVG
glyph image
([`src/display/drawing-text.cpp:78`](../src/display/drawing-text.cpp#L78)).
`DrawingText::_renderItem()` implements fill/stroke paint order, decorations, vector-effect handling,
and SVG color-glyph pixbuf drawing
([`src/display/drawing-text.cpp:451`](../src/display/drawing-text.cpp#L451)).

Object bounds come from glyph bounds transformed through layout, with optional stroke expansion
([`src/libnrtype/Layout-TNG-Output.cpp:233`](../src/libnrtype/Layout-TNG-Output.cpp#L233)). Text picking
uses expanded character/glyph bounding boxes rather than exact filled outlines
([`src/display/drawing-text.cpp:175`](../src/display/drawing-text.cpp#L175)). Cursor geometry,
selection quads, source mapping, and visual navigation all operate on the derived layout
([`src/libnrtype/Layout-TNG-OutIter.cpp:384`](../src/libnrtype/Layout-TNG-OutIter.cpp#L384)).

OpenType SVG glyph images use a process-wide 1024-entry pixbuf cache
([`src/display/drawing-text.cpp:46`](../src/display/drawing-text.cpp#L46)).

## Rendering for print and export

The shared Cairo path batches positioned glyphs in `Layout::showGlyphs()` and passes them to
`CairoRenderContext`, which creates a Cairo FT font face from the resolved Pango/Fontconfig pattern.
It can call `cairo_show_glyphs()` for live text or `cairo_glyph_path()` for outlines, clipping, or
stroked text
([`src/libnrtype/Layout-TNG-Output.cpp:442`](../src/libnrtype/Layout-TNG-Output.cpp#L442),
[`src/extension/internal/cairo-render-context.cpp:1715`](../src/extension/internal/cairo-render-context.cpp#L1715)).

This is not the same implementation as the interactive canvas. Color-font handling, filters,
clipping, text-to-path policy, and font embedding can therefore diverge. The newer native PDF
builder is another path; it supports ligature mappings and glyph positioning but currently does not
support vertical text, and an unavailable font can cause a span to be skipped
([`src/extension/internal/pdfoutput/build-text.cpp:146`](../src/extension/internal/pdfoutput/build-text.cpp#L146),
[`src/extension/internal/pdfoutput/build-text.cpp:251`](../src/extension/internal/pdfoutput/build-text.cpp#L251)).

Text on a path is outlined by the shared print path even when general text-to-path is disabled
([`src/libnrtype/Layout-TNG-Output.cpp:272`](../src/libnrtype/Layout-TNG-Output.cpp#L272)).

## Persistence and interchange

### Native and plain SVG

`SPText::write()` serializes the text children and position attributes, while `SPItem` writes shared
style and transform data
([`src/object/sp-text.cpp:252`](../src/object/sp-text.cpp#L252)). The XML writer suppresses formatting
whitespace inside text/flow content so save formatting does not change visible text
([`src/xml/repr-io.cpp:983`](../src/xml/repr-io.cpp#L983)).

Inkscape SVG is treated as the editable, non-lossy format. Plain SVG may prune Inkscape/Sodipodi
metadata and optionally generate SVG 1.1 fallbacks
([`src/extension/internal/svg.cpp:76`](../src/extension/internal/svg.cpp#L76)). A fallback that replaces
flow with explicitly positioned tspans preserves current appearance, not the original semantic flow
relationship.

### Flow conversion

Legacy SVG 1.2 flow and SVG2 `shape-inside`/`inline-size` are separate document models. Conversion to
legacy flow, unflowing legacy content, putting multiline flow on a path, and converting newlines to
Inkscape line tspans all have acknowledged structure or formatting losses
([`src/text-chemistry.cpp:90`](../src/text-chemistry.cpp#L90),
[`src/text-chemistry.cpp:357`](../src/text-chemistry.cpp#L357),
[`src/text-chemistry.cpp:449`](../src/text-chemistry.cpp#L449),
[`src/object/sp-text.cpp:1035`](../src/object/sp-text.cpp#L1035)).

### PDF, PS, and EPS

PDF import reconstructs SVG text from positioned glyph runs. A per-font policy can keep text,
substitute a family, convert glyphs to paths, or delete the text
([`src/extension/internal/pdfinput/pdf-input.h:29`](../src/extension/internal/pdfinput/pdf-input.h#L29)).
Editable import infers lines and spans; absent or poor PDF `ToUnicode` mappings make Unicode and
reading order unreliable. Outline mode retains recovered wording only as `aria-label`
([`src/extension/internal/pdfinput/svg-builder.cpp:1501`](../src/extension/internal/pdfinput/svg-builder.cpp#L1501),
[`src/extension/internal/pdfinput/svg-builder.cpp:1687`](../src/extension/internal/pdfinput/svg-builder.cpp#L1687)).

PDF/PS/EPS export can preserve font-backed text, convert to paths, or use LaTeX separation depending
on format/options. Font substitution can alter glyph metrics and therefore line breaks and
positions.

### EMF, WMF, and XAML

EMF and WMF default to text-to-path. Retained WMF text is reduced to Latin-1 after special symbol-
font conversions, and its integer font-size constraints can drift
([`src/extension/internal/wmf-print.cpp:1357`](../src/extension/internal/wmf-print.cpp#L1357)).

XAML low-level/Avalonia modes convert text to paths. High-level WPF output uses `TextBlock`/`Span`,
but does not preserve text stroke or alternate writing modes
([`share/extensions/other/extension-xaml/inkxaml/export/text.py:50`](../share/extensions/other/extension-xaml/inkxaml/export/text.py#L50)).

### Text to path, text to glyphs, clipboard, and clones

Native text-to-path converts layout glyphs into path geometry, preserves some outer metadata and an
`aria-label`, and permanently removes editability, source character structure, font metadata,
text-path semantics, and flow relationships
([`src/path-chemistry.cpp:484`](../src/path-chemistry.cpp#L484)). `Convert to Glyphs` is different: it
creates independently transformed one-glyph text objects, keeping characters/fonts but destroying
paragraph and shaping relationships
([`src/text-chemistry.cpp:558`](../src/text-chemistry.cpp#L558)).

Copying a text subselection exports plain characters and cursor style, not the original nested tspan
structure. Object copy uses an internal SVG document and copies text-path/flow dependencies; external
paste chooses an available MIME importer and may therefore substitute or flatten text
([`src/ui/clipboard.cpp:297`](../src/ui/clipboard.cpp#L297),
[`src/ui/clipboard.cpp:1068`](../src/ui/clipboard.cpp#L1068)).

`<use>` clones remain references and relayout when their source changes. Unlinking materializes an
independent object and ends future propagation.

## Invalidation and performance

A layout-sensitive change generally follows:

```text
XML/style/reference change
  -> requestDisplayUpdate(flags)
  -> updateDisplay()
  -> SPText::update()
  -> full Layout rebuild
  -> old DrawingText children cleared
  -> glyph drawing objects recreated
```

`SPText::update()` performs this work
([`src/object/sp-text.cpp:142`](../src/object/sp-text.cpp#L142)). The style-modification path is
described in the source as wasteful because drawing objects are destroyed and recreated rather than
updated in place
([`src/object/sp-text.cpp:211`](../src/object/sp-text.cpp#L211)). Shape references and text paths also
invalidate their dependent text when geometry changes.

Existing optimizations include cached `FontInstance` values, lazy per-font glyph/OpenType caches,
the SVG glyph pixbuf cache, and ordinary drawing surface/transform caches. There is no incremental
paragraph or span relayout seam in `SPText`; most edits reconstruct the whole text layout.

## Critical invariants and known gaps

1. **Never retain a layout iterator across `calculateFlow()`.** Recreate or validate it from source
   state after rebuilding.
2. **Never assume one character equals one glyph.** Ligatures, combining marks, bidi, control data,
   and fallback make that invalid.
3. **Update position vectors with source edits.** `x/y/dx/dy/rotate` are indexed alongside text.
4. **Treat styling as structural mutation.** Span consolidation can remove source objects.
5. **Do not confuse visual and logical order.** Cursor movement is visual; XML edits are logical.
6. **IME support is commit-only.** Preedit/composition UI is not represented on canvas.
7. **`unicode-bidi` is incomplete.** Full CSS bidi isolation/embedding cannot be assumed.
8. **Nested `textLength` is incomplete.** Current state is whole-layout oriented.
9. **Text picking is approximate.** Bounding boxes, not exact glyph fill, choose hits.
10. **Flow and text-path conversions can be lossy.** Preserve originals where semantic editing matters.
11. **Font refresh is split.** Legacy and new font browsers can disagree until restarted/refreshed.
12. **Font state is session-global.** `FontLister` and document-font helpers are not cleanly per-document.
13. **Canvas and export are separate renderers.** Appearance parity must be tested, not inferred.
14. **Color glyph support is asymmetric.** Canvas has explicit OpenType SVG pixbuf handling.
15. **Platform text output varies.** Pango, HarfBuzz, FreeType, Fontconfig, and installed fonts are part
    of the result.

## Existing tests and missing coverage

Direct unit coverage exists for layout/shaping/cursor cases, language itemization, font enumeration,
font-instance glyph data, and font-specification/variation normalization
([`testfiles/src/libnrtype/Layout-TNG-Compute.cpp:20`](../testfiles/src/libnrtype/Layout-TNG-Compute.cpp#L20),
[`testfiles/src/libnrtype/font-factory-test.cpp:30`](../testfiles/src/libnrtype/font-factory-test.cpp#L30)).

Rendering tests cover flow, vertical text, combining marks, bidi, SVG-in-OpenType glyphs, and a large
real document. They use isolated bundled fonts, but several shaping goldens are disabled because
Pango/HarfBuzz/FreeType versions change pixels
([`testfiles/rendering_tests/CMakeLists.txt:32`](../testfiles/rendering_tests/CMakeLists.txt#L32),
[`testfiles/rendering_tests/fonts/isolated.conf:3`](../testfiles/rendering_tests/fonts/isolated.conf#L3)).

CLI tests exercise text preservation versus outlines across formats, paint order, multiline anchors,
glyph conversion, PDF font spacing/styles/ligatures, and decorations
([`testfiles/cli_tests/CMakeLists.txt:522`](../testfiles/cli_tests/CMakeLists.txt#L522),
[`testfiles/cli_tests/CMakeLists.txt:1037`](../testfiles/cli_tests/CMakeLists.txt#L1037)).

The largest gaps are direct tests for:

- `sp_te_insert/delete/replace`, line splitting, and positional-list maintenance;
- TextTool mouse, keyboard, IME, clipboard, selection, and undo state;
- toolbar/dialog outer-object versus inner-subselection styling;
- live font refresh, async discovery cancellation, cache invalidation, and concurrent access;
- semantic SVG/flow/text-path save-open-save round trips;
- PDF Unicode extraction, selectable text, embedding, and reimport;
- structural `DrawingText` behavior before pixel comparison; and
- Unicode property/fuzz combinations across graphemes, bidi, ligatures, combining marks, and malformed
  input.

### Recommended test order

1. Add model tests for mutation across nested tspans, text paths, flow objects, multibyte text, and all
   per-character position arrays.
2. Add a non-pixel TextTool state-machine harness for key, pointer, IME commit, selection, and undo.
3. Replace disabled shaping goldens where possible with semantic glyph/cluster/cursor assertions.
4. Add controlled Fontconfig tests for refresh, missing fonts, variables, duplicates, and async cache
   behavior.
5. Add round-trip matrices that separately assert XML structure, Unicode, references, computed bounds,
   and rendered appearance.
6. Keep a small cross-platform UI smoke suite and record Pango, HarfBuzz, FreeType, Fontconfig, and test-
   font versions on failure.

## Practical debugging entry points

| Symptom | Start here |
|---|---|
| Typed characters or IME problems | `TextTool::root_handler()`, `TextTool::_commit()` |
| Wrong caret/selection or bidi movement | `Layout-TNG-OutIter.cpp`, `sp_te_get_position_by_coords()` |
| Edit corrupts tspans or positions | `sp_te_insert/delete/apply_style()`, `TextTagAttributes` |
| Wrong wrapping or overflow | `SPText::_buildLayoutInit()`, `Layout-TNG-Compute.cpp`, scanline makers |
| Wrong glyph or fallback font | `FontFactory::Face()`, Pango itemization, `FontInstance` |
| Font missing from UI | `FontLister`, `FontDiscovery`, Fontconfig configuration/cache |
| Canvas-only rendering bug | `Layout::show()`, `DrawingText::_renderItem()` |
| Export-only rendering bug | `Layout::showGlyphs()`, `CairoRenderContext`, format-specific writer |
| SVG round-trip change | `SPText::write()`, `SPTSpan::write()`, `repr-io.cpp` |
| Text/flow/path conversion loss | `text-chemistry.cpp`, `path-chemistry.cpp` |

## Recommended architectural seams for future work

- Keep Pango/HarfBuzz as the shaping authority; avoid implementing script-specific shaping in UI or
  SVG object code.
- Put source-preserving character/tree edits in `text-editing.cpp`, with focused model tests.
- Put SVG semantics and wrap-input construction in `SPText`/`SPFlowtext`.
- Put output-only geometry and cursor queries in `Layout-TNG` rather than mutating source XML.
- Unify font enumeration/refresh behind one service before extending either font browser.
- Add explicit renderer capability tests before sharing or replacing canvas/Cairo/PDF text paths.
- Introduce incremental layout only with a stable source-position identity that can survive partial
  rebuilds; current raw iterators are not suitable as persistent anchors.

## Source map

| Area | Files |
|---|---|
| Canvas input and Text tool | `src/ui/widget/canvas.cpp`, `src/ui/tools/text-tool.*` |
| Character/tree mutation | `src/text-editing.*`, `src/text-tag-attributes.*` |
| Creation and conversions | `src/text-chemistry.*`, `src/path-chemistry.cpp` |
| SVG text objects | `src/object/sp-text.*`, `sp-tspan.*`, `sp-string.*`, `sp-flowtext.*` |
| Style | `src/style.*`, `src/style-text.*` |
| Layout and cursor model | `src/libnrtype/Layout-TNG-*` |
| Font loading and glyph data | `src/libnrtype/font-factory.*`, `font-instance.*`, `OpenTypeUtil.*` |
| Font UI/data | `src/libnrtype/font-lister.*`, `src/util/font-discovery.*`, `src/ui/widget/font-*` |
| Canvas rendering | `src/display/drawing-text.*` |
| Cairo export | `src/extension/internal/cairo-render-context.*` |
| PDF import/export | `src/extension/internal/pdfinput/`, `src/extension/internal/pdfoutput/` |
| Tests | `testfiles/src/libnrtype/`, `testfiles/rendering_tests/`, `testfiles/cli_tests/` |

## Glossary

- **Source character:** a Unicode code point in an XML text node after whitespace normalization.
- **Layout character:** a character/control record in derived layout; it may be hidden or synthetic.
- **Glyph:** a font rendering primitive. It is not equivalent to one Unicode character.
- **Cluster:** the shaping unit relating one or more source characters to one or more glyphs.
- **Span:** a layout run sharing font, style, direction, block progression, and source stream.
- **Chunk:** a line fragment; complex wrap shapes can produce several chunks on one baseline.
- **Subselection:** the range between `TextTool`'s two layout iterators inside one selected text object.
- **Flow text:** either legacy SVG 1.2 `flowRoot` objects or SVG2-style wrapping through `inline-size` or
  `shape-inside`; the representations are not interchangeable.

