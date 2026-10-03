# Known issues

This list covers the current release line. Each release's notes add the issues
specific to that release.

## Installation and platforms

- Windows builds are 64-bit x64 only; macOS builds run on Apple silicon with
  macOS 26 or later only.
- Until the installers are code-signed and the macOS app is notarized, Windows
  SmartScreen and macOS Gatekeeper ask for confirmation at the first start. The
  install guides explain how to check the download and continue.
- Several VA Studio versions can be installed side by side, but an installer
  does not upgrade or replace an existing installation of the same version.

## Windows File Explorer

- SVG thumbnails and the preview pane need the Microsoft Edge WebView2 Runtime.
- When Microsoft PowerToys shows SVG thumbnails or previews for your user, they
  take precedence over VA Studio's; turn off the PowerToys SVG add-ons to use
  VA Studio's (VA Studio explains how).
- Thumbnails at display scaling of 225% and above are not covered yet.

## File formats

- EMF and WMF import and export are not available. Use SVG or PDF for vector
  exchange and PNG or TIFF for bitmaps.
- CorelDRAW import: clips that contain vectors or text, and clips inside groups
  with transparency or effects, are kept as clips rather than converted to
  plain images, and are listed in the report.
- Converting the clipped bitmaps of a CorelDRAW file while it opens cannot be
  undone; use Edit > Convert Clipped Bitmaps to Images... when you want an
  Undo step.

## Editing

- Some stroke changes on parts of a text (for example a range whose exact
  positioning cannot be preserved after splitting it) are refused with a
  message instead of being approximated. A stroke cannot be set on a text clone
  alone; edit the original.
- The stroke width row in Object Properties keeps the older drag and mouse-wheel
  behavior; the Fill and Stroke dialog has the new controls.

## Explode Bitmap

- Cancelling the import of a large GIF or other highly compressible image takes
  effect only between internal write steps, so it can take a moment.
- On Windows, WebP images are decoded with premultiplied alpha: the color under
  fully transparent pixels is lost and partly transparent pixels can differ
  slightly from the file.
- After the target changes, piece outlines from the previous state can remain
  visible for one frame.

## Nesting

- The first nesting of a document prepares the outlines of its parts, which can
  take noticeably longer on slower computers; later runs reuse that work.

## Help and translations

- The Help menu's manual, tutorials, FAQ and keyboard reference are upstream
  Inkscape documentation and do not describe VA Studio's own tools.
- New VA Studio texts are written in English and, for many of them, translated
  to Spanish; other languages show them in English.
