# Explode Bitmap simulator oracle

Pinned, deterministic reference for native EB1 piece-mask comparisons (byte for byte, on the ORIGINAL
pixel grid, before gutter, bleed or encoding).

## Provenance and pins

- Source: `work/explode-bitmap/sim/explode-bitmap-sim.html` (outside Git; the VA Studio simulator).
  SHA-256 `8e5d7405fda19957adb55fa682523a6c852e70c11c5947dbd9304be619132e3a` (41962 bytes).
- `sim-engine.js` = the exact bytes between `<script id="engine">` and its `</script>`
  (12613 bytes), extracted by `extract-engine.cjs`, not by hand.
  SHA-256 `7a9ce306fee5e83241a672a082669a95a8213b21b76e93358a2f54f4012704b4`.
- Do not edit `sim-engine.js`. A new simulator version means a new pin, regenerated outputs and a
  review of every difference.

## Output format (`explode-bitmap-oracle-1`, compact JSON, fixed key order)

`format, width, height, T, S, dpi, lut_sha256, piece_count, pieces[], sha256`.
`lut_sha256` hashes the 256-byte alpha LUT (alpha 0..255 -> a'). Each piece:
`index` (0-based output order), `id` (engine label), `bbox` `[x,y,x2,y2]` inclusive on the original grid,
`area` (pixels), `rle`: alternating run lengths over the bbox, row-major, starting with a
NON-member run (may be 0), so the sum equals bbox width*height. `sha256` hashes the JSON text of
everything before it (the same object without `sha256`).

## Regenerate / verify

    node extract-engine.cjs <path>/explode-bitmap-sim.html          # rewrites sim-engine.js
    node export-oracle.cjs --T 128 --S 40 --dpi 300 --out out.json in.pam
    node ../fixtures/generate-fixtures.cjs                           # PAMs + expected JSON
    node check-oracle.cjs [<path>/explode-bitmap-sim.html]           # pins, determinism x2, CLI

Node only, no packages. Input is binary PAM (P7, DEPTH 4, MAXVAL 255, TUPLTYPE RGB_ALPHA).
The oracle calls `preview(raster, T, S, dpi/300)` of the engine: alpha LUT, then partition with
`T=0, D=0, enclosure, dots, gutter 1, conn 8, reach = (300/25.4)·(dpi/300) px, speck area <= 2*reach^2 px^2`.

Equal nearest distances use the discovery id of the source foreground region at the nearest cell,
including regions merged during enclosure. Frozen candidates include other specks; joined chains
may exceed 2 mm². For the native anisotropic extension, distances use x-pixel units:
`gapX² + (gapY * pixelsPerMmX / pixelsPerMmY)² <= pixelsPerMmX²`, with
`pixelsPerMmAxis = (300/25.4)·(dpiAxis/300)` and area limit `2*pixelsPerMmX*pixelsPerMmY`.
The pinned simulator oracle covers isotropic DPI; native hand-computed tests cover anisotropy.

## Licence notice: alpha formula

The alpha threshold/softness formula in the engine comes from the owner's MIT-licensed AlphaKiller
(`imageProcessing.js`):

    MIT License

    Copyright (c) 2026 Abraham Saenz

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Private data

The owner's flower sample is never stored here. Its output lives in
`work/explode-bitmap/oracle-private/` only.

## Piece counts (fixture x T/S/dpi)

25.4 dpi gives reach 1 px and speck limit 2 px2. The engine expression `300/25.4*(25.4/300)` evaluates to
exactly 1 in Node (checked: reach===1, 2*reach^2===2; `check-oracle.cjs` asserts it), so 25.4 is used as is.

| aa_edges | 2 | 2 | 1 | 1 | 2 | 2 |
| alpha_edges | 8 | 7 | 12 | 1 | 8 | 7 |
| alpha_edges_ring | 1 | 1 | 1 | 0 | 1 | 1 |
| alpha_ramp | 1 | 1 | 1 | 1 | 1 | 1 |
| checker_1px | 1 | 1 | 1 | 1 | 1 | 1 |
| checker_3px | 1 | 1 | 1 | 1 | 1 | 1 |
| diagonal_touch | 1 | 1 | 1 | 1 | 2 | 2 |
| diamond_hole | 1 | 1 | 1 | 1 | 1 | 1 |
| diamond_hole_block | 1 | 1 | 1 | 1 | 1 | 1 |
| empty | 0 | 0 | 0 | 0 | 0 | 0 |
| far_blocks | 5 | 5 | 5 | 5 | 6 | 6 |
| island_in_ring_far | 2 | 2 | 2 | 2 | 2 | 2 |
| labyrinth | 1 | 1 | 1 | 1 | 1 | 1 |
| lines_1px | 1 | 1 | 1 | 1 | 3 | 3 |
| one_pixel | 1 | 1 | 1 | 1 | 1 | 1 |
| opaque | 1 | 1 | 1 | 1 | 1 | 1 |
| rings | 1 | 1 | 1 | 1 | 1 | 1 |
| rings_round | 1 | 1 | 1 | 1 | 1 | 1 |
| rings_wide | 1 | 1 | 1 | 1 | 1 | 1 |
| serpentine | 1 | 1 | 1 | 1 | 1 | 1 |
| specks_boundary | 3 | 3 | 3 | 3 | 4 | 4 |
| specks_equidistant | 4 | 4 | 4 | 4 | 5 | 5 |
| specks_field | 13 | 13 | 13 | 13 | 40 | 40 |

Fixtures still at 1 piece at 25.4 dpi collapse by design, not by reach: nested/enclosed shapes (rings,
rings_round, rings_wide, diamond_hole*, labyrinth) merge through the enclosure rule; checker_*,
serpentine, opaque, one_pixel, alpha_ramp, alpha_edges_ring are a single 8-connected shape; empty has no
foreground (0). The oracle JSON lists only final pieces, so enclosure topology is discriminated by the
pieces that stay separate (island_in_ring_far, far_blocks) and by native intermediate-label tests.
At 25.4 dpi diagonal_touch, lines_1px, far_blocks, specks_equidistant and specks_field split further
because specks beyond 1 px reach or 2 px2 stay pieces.

## Enclosure intermediate mode

`node export-oracle.cjs --enclosure --T 128 --S 40 --dpi 300 in.pam`
exports `explode-bitmap-enclosure-oracle-1`: alpha LUT and 8-connected foreground
labeling, followed by enclosure only. Background uses four-connectivity; all
border-connected background is exterior. Each enclosed foreground descendant
joins the foreground ancestor two levels up, repeatedly to its top-level owner.
Transparent holes retain alpha and belong to that owner; proximity creates no
new hole or merge. IDs keep the earliest source pixel's discovery id (minimum
id on merge); output sorts by bbox `(y, x, id)`, independently of tie ordering.

`--enclosure-suite <fixtures/pam>` generates `enclosure-regions.json`, consumed
by the native enclosure test and independently regenerated by `check-oracle.cjs`.
Its 368 entries are **184 distinct fixture/T/S cases**, each stored at DPI 300
and 25.4 for historical compatibility. In this mode reach is fixed at zero:
DPI has no effect on enclosure membership and the second set adds no coverage.
The format and hashes include DPI metadata; native piece id/area/bbox/membership
must match the regenerated entries. The round-2 specks_boundary correction changes its masks
in label/enclosure outputs as well as final outputs; all three sets are regenerated together. Hole ownership is tested
natively because the simulator exports foreground masks only.
