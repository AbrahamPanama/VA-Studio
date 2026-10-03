# VA Studio performance

VA Studio's changes include measured speed and memory improvements on Windows
and on macOS. This page lists only results with a measured value before and
after the change, together with the workload, the hardware and the method.

The Origin column says whose work each item is:

- **VA Studio (novel):** VA Studio's own code or patch. It is not part of
  upstream Inkscape or of the library it changes.
- **VA Studio fix to inherited code:** a VA Studio change to code that VA
  Studio inherited from Inkscape. The "before" value is an earlier VA Studio
  build; upstream Inkscape itself was not measured.

**Results depend on hardware, drivers, display settings and documents.** They
come from development builds on the machines listed under Hardware. They are
not a formal benchmark suite.

## Windows

| Improvement | Before → after | Workload | How measured | Origin |
| --- | --- | --- | --- | --- |
| Faster Windows drawing path, object drag at 1080p | GTK paint time per frame, median 11.708 → 7.692 ms (−34.3%), 95th percentile 20.795 → 15.359 ms; process CPU time per second of input 0.660 → 0.609 s (−7.8%) | Dragging one rectangle; 1920×1080 display, 744×555 canvas | The same application with the new drawing buffer off and on; 8 runs (4 per setting) in alternating order; 1,141 injected mouse events per run; GTK's own paint timing. Windows laptop | VA Studio (novel): VA patch to GTK 4.22.4 for Windows |
| Faster Windows drawing path, panning at 1080p | Paint time per frame, median 9.650 → 5.762 ms (−40.3%), 95th percentile 19.076 → 14.270 ms; process CPU time per second of input −19.1% | Middle-button panning; 1920×1080 display, 766×555 canvas | Same application, buffer off and on; 8 runs in alternating order; 1,221 injected events per run. Windows 11 PC | VA Studio (novel): VA patch to GTK 4.22.4 |
| Faster Windows drawing path, 4K display at 150% | Updates of the dragged object seen on screen: 16.754 → 24.093 per second (+43.8%); 95th percentile gap between updates 100.08 → 66.65 ms | Dragging an object in a maximized 3840×2088 window on an external 4K monitor | Screen-capture observer on the composed desktop; 2 observed runs per setting, so the result is descriptive. Windows laptop | VA Studio (novel): VA patch to GTK 4.22.4 |
| Faster Windows drawing path, 4K display (internal counters) | Paint time per frame, median 24.45 → 12.738 ms; canvas paints per second 2.15903 → 20.30221; process CPU time per second of input rose 14.5% (more frames were drawn) | Dragging one rectangle; 4K monitor at 150%, 3332×1872 canvas | 8 runs in alternating order; GTK and canvas counters, not display frame rate. Windows laptop | VA Studio (novel): VA patch to GTK 4.22.4 |
| No OpenGL canvas under the Cairo renderer | Frame paint time, median, dragging: 8.89 → 5.75 ms (laptop), 7.55 → 4.98 ms (mini desktop); panning: 8.62 → 4.26 ms and 7.05 → 3.60 ms. Frames per second unchanged (46 to 48) | Three objects; 1280×800 window, 744×555 canvas | Both canvas settings measured on the same build, 5 runs per setting (3 to 5 valid). "Before" is the canvas with the "Enable OpenGL" preference on; VA Studio now always uses the Cairo canvas with the Cairo window renderer | VA Studio (novel) |
| Nesting helper start-up allowance | 80-circle test at the 15-second setting: timed out on all three seeds → 80 of 80 placed in 15.520 / 15.514 / 15.441 s, including validation | 80 circles, three random seeds | Development build test suite (21 checks pass after the change). Windows laptop | VA Studio (novel) |
| Undo of many objects | Undo of 100 pieces: 83 ms → 0.75 ms; 1,000 pieces: 6.9 s → 9.4 ms | Undo of one Explode Bitmap result with all pieces selected | Opt-in timing test; the "before" values come from one calibration run. Windows laptop | VA Studio fix to inherited code |

## macOS

| Improvement | Before → after | Workload | How measured | Origin |
| --- | --- | --- | --- | --- |
| Undo of many objects | Undo of 1,000 pieces: 248.499 → 3.935 ms; 100 pieces: 3.984 → 0.383 ms; 1,000 plain objects without Explode: 250.351 → 5.019 ms | Undo of one Explode Bitmap result, and of the same number of plain objects | Median of 3 fresh documents per size; setup excluded | VA Studio fix to inherited code |
| Stroke width on many texts | One click on 2,000 short texts: 3.7 s → 0.34 s; one text with 100 differently styled runs: apply step 3,183 → 45 ms | Synthetic documents with short texts, as imported from CorelDRAW | Timing of the four steps of the stroke-width command for one click, Release build, one run per case; screen refresh not included | VA Studio (novel) |
| Nesting: applying a result | 200 cards: 215 → 2 ms; 240 detailed curves: 503 → 0.8 ms; 100 stroked paths: 400 → 0.4 ms | Generated sheets of 3000×3000 px | Nesting responsiveness test, median of 3 runs per sheet | VA Studio (novel) |
| Nesting: preparation off the user-interface thread | Time the window is busy preparing parts: 222 → 30 ms (200 cards), 504 → 24 ms (240 curves), 443 → 203 ms (100 stroked paths) | Same sheets | Same test; the remaining work runs on a background thread | VA Studio (novel) |
| Nesting: validating Sparrow layouts | 80-circle test at the 15-second setting: 29 s → 15.3 s, still 80 of 80 placed | 80 circles | In-application nesting test; the fix stopped validation from rebuilding unused search geometry | VA Studio (novel) |
| Explode Bitmap at its 5000×5000 / 150-piece limit | Refused (work and memory limits) → completes in 2,422.63 ms, peak process memory 390.78 MiB; publishing the 150 pieces as one Undo step: 183.52 ms | Dense 5000×5000 image cut into 150 pieces | Calibration harness; time includes a 250 ms input debounce; peak memory of the whole process; publication is the longest of 3 runs | VA Studio (novel) |
| Bitmap Eraser memory | Peak memory while committing a stroke: about 313 to 370 MB → about 249 MB | 3000×3000 px stroke on a 4000×4000 px image (61 MiB) | Replay of the commit sequence in a fresh process per run; peak resident memory. The "before" value is a lower bound | VA Studio (novel) |
| Artwork Library thumbnails while scrolling | Blank thumbnails in 20 of 20 scroll samples → 0 of 20; settling after scrolling 34 ms → 3 ms | Library of 48 entries, all viewed once, then 20 rapid scroll samples | Native GTK regression test, one run before and one after | VA Studio (novel) |

## Nesting engine

| Improvement | Before → after | Workload | How measured | Origin |
| --- | --- | --- | --- | --- |
| Part spacing on detailed outlines | 12 different 3000-vertex parts, 1 mm spacing: 0 of 12 placed after 110.8 s → 12 of 12 in 5.0 s. 5 identical 8000-vertex parts, 2 mm spacing: no result after 400 s → 5 of 5 in 5.0 s. 16 identical 800-vertex parts, 1 mm spacing: 0 of 16 → 16 of 16 | A4 sheet, Balanced quality, 5-second setting, one worker | The nesting engine on its own, through its C interface, release build; every layout was checked again in a fresh job. Linux x86-64, 4 cores (the same Rust code is built into VA Studio on Windows and macOS) | VA Studio (novel): engine built on jagua-rs |

## Hardware

- **Windows laptop:** Windows 11, Intel Core i7, Intel Iris Xe graphics,
  16 GB RAM; built-in 1920×1080 display at 100% and an external 3840×2160
  monitor at 150%.
- **Windows mini desktop:** Windows 11 Pro, Intel Core i7-12650H, Intel UHD
  graphics.
- **Windows 11 PC:** the panning measurement does not record which of the
  two Windows machines it used; its display was 1920×1080.
- **Mac:** laptop with Apple M2, 8 GB RAM, macOS 26.
- **Linux:** x86-64 machine with 4 cores (nesting engine only).

## Not listed

Changes whose speed effect has not been measured are not listed, even where
they were made for speed. Examples are the Selector's "Fast preview when moving"
option and the nesting outline cache. Absolute timings without a "before" value
are not listed either.
