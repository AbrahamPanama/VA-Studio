# VA Studio quick start

VA Studio works like Inkscape, with additional tools for print and cutting
work. This page covers the first steps and where the VA Studio additions are.
Keyboard shortcuts use Ctrl on Windows and Cmd on macOS.

## Open, import and save

- **File > Open** opens SVG, PDF, CorelDRAW (`.cdr`, `.cdt`, `.ccx`, `.cmx`),
  Affinity Designer files (through the bundled extension) and bitmap formats.
- When a CorelDRAW file contains clips that hold only bitmaps, VA Studio offers
  to turn each into a plain image. You can do the same later in any document
  with **Edit > Convert Clipped Bitmaps to Images...**.
- **File > Import...** adds a file to the current drawing.
- **File > Save** saves VA Studio SVG; plain SVG is available in Save As.
- **File > Export...** exports PNG, TIFF, JPEG, PDF and more. Choose which
  formats the Export dialog lists, and their order, in
  **Edit > Preferences > Input/Output > Export formats**. TIFF export writes RGB
  with an embedded ICC profile.

## VA Studio tools

| Tool or command | Where | What it does |
| --- | --- | --- |
| Nesting tool | toolbox, Shift+N | Select the parts, then Ctrl/Cmd-click a sheet to nest them into it. |
| Offset Shapes tool | toolbox, Alt+O | Creates interactive offsets of the selected shapes. |
| Corners tool | toolbox | Rounds or inverse-rounds corners interactively. |
| Bitmap Eraser tool | toolbox | Erases bitmap pixels with a raster brush. |
| Break Apart | Path > Break Apart, Ctrl+K | Breaks text one level down (lines, then words, then letters) into separate editable texts that keep their fonts, colors and positions, and paths into their subpaths; other objects stay as they are. |
| Explode Bitmap | Object > Explode Bitmap... | Splits a bitmap into its separate pieces, with optional contours. |
| Bitmap Adjustments | Filters > Bitmap Adjustments... | Adjusts bitmaps in the drawing. |
| Make a Bitmap Copy | Edit > Make a Bitmap Copy... | Renders the selection as a bitmap with the chosen options. |
| Destructive Clip | Object > Clip > Destructive Clip | Cuts a bitmap to a vector shape; Destructive Inverse Clip keeps the outside. |
| Artwork Library | Object > Artwork Library... | Keeps reusable artwork in `.valib` libraries; reads LightBurn `.lbart` libraries. |
| Text and Font | Text > Text and Font... | The redesigned Text panel, with live font preview on the canvas. |

When you move objects with the Selector, VA Studio drags a picture of them and
places them on release, which keeps moving large drawings fast. Turn this off
with the Selector's "Fast preview when moving" option.

## Undo

VA Studio's commands treat a group or a multiple selection as one action, with
one step in Edit > Undo History. One exception: converting the clipped bitmaps
of a CorelDRAW file while it opens cannot be undone (the command in the Edit
menu can).

## Online features

Two optional features use the internet, only when you start them: **File >
Import Web Image...** and **Extensions > Manage Extensions...**. See
[PRIVACY.md](../../PRIVACY.md).

## Learn more

- The Help menu links to the Inkscape manual and tutorials. They describe
  Inkscape, which VA Studio is based on; VA Studio's own tools are listed above.
- [KNOWN-ISSUES.md](KNOWN-ISSUES.md) and [SUPPORT.md](../../SUPPORT.md).
