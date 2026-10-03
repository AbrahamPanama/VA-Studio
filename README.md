# VA Studio

VA Studio is a vector graphics editor for Windows and macOS, developed by
VA Cards for print and cutting work. It is based on
[Inkscape](https://inkscape.org) (a 1.5 development snapshot) and, like
Inkscape, it is free software under the GNU General Public License.

VA Studio is not Inkscape and is not endorsed by the Inkscape project. See
[TRADEMARKS.md](TRADEMARKS.md).

## What VA Studio adds to Inkscape

- **CorelDRAW import**: modern `.cdr`, `.cdt`, `.ccx` and `.cmx` files, including
  CorelDRAW 2023 fills and bitmap transparency, through a maintained fork of
  libcdr. Clips that hold only bitmaps can be turned into plain images
  (Edit > Convert Clipped Bitmaps to Images...).
- **Nesting tool**: packs selected parts onto sheets, with spacing and rotation
  options, using the jagua-rs engine and the Sparrow optimizer.
- **Explode Bitmap** (Object > Explode Bitmap...): splits a bitmap into its
  separate pieces, with optional contours.
- **Bitmap work**: Bitmap Adjustments (Filters > Bitmap Adjustments...), the
  Bitmap Eraser tool, Make a Bitmap Copy (Edit), and Destructive Clip and
  Destructive Inverse Clip (Object > Clip).
- **Shape tools**: the Corners tool, the Offset Shapes tool and a Break Apart
  that works by object kind (Path > Break Apart).
- **Text**: a redesigned Text panel and live on-canvas font preview.
- **Artwork Library** (Object > Artwork Library...): reusable artwork in
  `.valib` libraries; LightBurn `.lbart` libraries can be read.
- **Export**: a configurable list of export formats and RGB TIFF export with an
  embedded ICC profile.
- **Desktop integration**: SVG thumbnails and preview pane in Windows File
  Explorer; on macOS, saved SVG files show their drawing as their Finder icon.

EMF and WMF import and export are not available in VA Studio.

## Performance

Measured on Windows and macOS development builds; results depend on hardware.
Methods, hardware and more results are in
[doc/public/PERFORMANCE.md](doc/public/PERFORMANCE.md).

- **Windows drawing** (novel: VA Studio's own patch to GTK): dragging an object
  at 1080p, the median paint time per frame fell from 11.7 to 7.7 ms; on a 4K
  display at 150% the dragged object updated on screen 24.1 instead of 16.8
  times per second.
- **Undo of many objects** (VA Studio fix to code inherited from Inkscape):
  Undo of 1,000 objects takes 9.4 ms instead of 6.9 s on a Windows laptop, and
  3.9 ms instead of 248 ms on a Mac.
- **Stroke width on many texts** (novel): one click on 2,000 texts takes 0.34 s
  instead of 3.7 s on a Mac.
- **Nesting** (novel): with 1 mm part spacing the engine places 12 of 12
  different 3000-vertex parts in 5 s, where it placed none after 110 s; on a Mac,
  applying a result for 200 cards takes 2 ms instead of 215 ms; on Windows, the
  80-circle test places all 80 parts in 15.5 s at its 15-second setting instead
  of timing out.
- **Explode Bitmap** (novel): 150 pieces from a 5000×5000 image are ready in
  2.4 s with a 391 MiB memory peak on an 8 GB Mac, where this size was refused
  before.

## Install

- Windows: [doc/public/INSTALL-windows.md](doc/public/INSTALL-windows.md)
- macOS: [doc/public/INSTALL-macos.md](doc/public/INSTALL-macos.md)
- First steps: [doc/public/QUICKSTART.md](doc/public/QUICKSTART.md)
- Known issues: [doc/public/KNOWN-ISSUES.md](doc/public/KNOWN-ISSUES.md)

Releases, with their installers, checksums and complete source archive, are
published on the Releases page of the VA Studio repository.

## Help, privacy and security

- [SUPPORT.md](SUPPORT.md): how to get help and report problems.
- [PRIVACY.md](PRIVACY.md): VA Studio has no telemetry; which optional
  features use the network.
- [SECURITY.md](SECURITY.md): how to report a vulnerability.

## Source code and licenses

VA Studio as a whole is distributed under the GNU General Public License,
version 3 or later; individual files carry their own SPDX license identifiers
(see [COPYING](COPYING) and [LICENSES/](LICENSES)). This repository is
self-contained: the components that Inkscape keeps in Git submodules, and the
VA Studio fork of libcdr in `third_party/libcdr-vacards`, are ordinary
directories here.

- [share/doc/SOURCE.md](share/doc/SOURCE.md): how to obtain the source of each release.
- [share/doc/THIRD-PARTY-NOTICES.md](share/doc/THIRD-PARTY-NOTICES.md): third-party components and licenses.
- [doc/public/BUILDING.md](doc/public/BUILDING.md): building VA Studio on Windows and macOS.
- [CONTRIBUTING.md](CONTRIBUTING.md): how to contribute.

## Based on Inkscape

Inkscape is a free and open source vector graphics editor developed by the
Inkscape community (<https://gitlab.com/inkscape/inkscape>). Its authors and
translators are credited in [AUTHORS](AUTHORS) and [TRANSLATORS](TRANSLATORS).
Inkscape's developer documentation starts at [doc/readme.md](doc/readme.md).
Problems found in VA Studio should be reported to VA Studio, not to the
Inkscape project.
