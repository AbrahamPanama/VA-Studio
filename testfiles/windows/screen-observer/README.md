# screen-observer durable tests

Focused tests for the screen observer, native-drag setup, diagnostic admission,
and visual comparison. They exercise the actual helper functions in
`packaging/windows/vacards/` and a standalone screen-cadence reducer.

## Scope and limits

- **Pure / no-GUI by design.** Every PS 5.1 file is a *pure* test: it parses the
  real product script, extracts the actual helper functions by AST, dot-sources
  or `Invoke-Expression`s only those helpers, and drives them with synthetic
  fixtures. None launches the app, GUI, observer, input or display. The Python
  file builds synthetic Pan/observer fixtures and runs the reducer as a
  subprocess. That is why no GUI is needed and why none of this is a GUI or
  runtime-qualification claim.
- **Observer is not physical FPS.** The reducer reports observed screen-update
  cadence from observer `frames.csv` + `observer-metadata.json`, and fails
  closed on incomplete/invalid coverage. It is not a physical-FPS,
  photon-latency or refresh-rate measurement, and no such claim is made.
- **Native outcome gate is separate.** These tests do not replace the native
  SVG/visual outcome oracle or the root-owned GUI pilot gate. Passing here does
  not admit a build; the native outcome gate remains independent.
- **Incomplete coverage is invalid.** The admission suites require the observer's
  own QPC lifetime to fully cover the measured input window. Missing, malformed,
  mismatched, capped, fatal, or window-misordered metadata fails closed (exit
  non-zero); it is never treated as a pass or as a 0 FPS result.
- **No global PATH dependency.** Commands pass explicit source-helper paths and
  explicit relative/interpreter paths; nothing relies on `PATH` resolving the
  product scripts or the observer. The observer exe needs its three adjacent
  UCRT DLLs next to it at runtime.

## Local Python (portable, runs on macOS/Linux)

Run from this directory (`testfiles/windows/screen-observer/`):

```sh
python3 test_reduce_observer.py
```

The test locates `reduce_observer.py` relative to itself, so no path argument is
needed. Expected: `Ran 16 tests` ... `OK`, exit `0`.

## Windows PowerShell 5.1 (pure tests, source-helper paths passed explicitly)

Run from this directory in a normal PS 5.1 session. Set the checkout root once:

```powershell
$Root = "<checkout-root>\packaging\windows\vacards"
$Work = Join-Path $env:TEMP ("screen-observer-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Work -Force | Out-Null

powershell.exe -NoProfile -File .\Test-ArmPressOffset.ps1 `
  -PanPath   (Join-Path $Root "Invoke-AppPan.ps1") `
  -TrialPath (Join-Path $Root "Invoke-AppTrial.ps1")

powershell.exe -NoProfile -File .\Test-ObserverErrors.ps1 `
  -PanPath   (Join-Path $Root "Invoke-AppPan.ps1") `
  -TrialPath (Join-Path $Root "Invoke-AppTrial.ps1")

powershell.exe -NoProfile -File .\Test-ObserverCoverageAdmission.ps1 `
  -PanPath (Join-Path $Root "Invoke-AppPan.ps1") -WorkDir $Work

powershell.exe -NoProfile -File .\Test-TrialObserverCoverage.ps1 `
  -TrialPath (Join-Path $Root "Invoke-AppTrial.ps1") -WorkDir $Work

powershell.exe -NoProfile -File .\Test-BandAwareImage.ps1 `
  -VisualPath (Join-Path $Root "Invoke-AppVisualRegression.ps1") -WorkDir $Work
```

Each script exits `0` only when every check passes (`1` = check failed,
`2` = unreadable/unparseable input for the two AST suites).

## Observer exe build command

From `next4-observer-implementation/BUILD-EVIDENCE.md` (remote build dir
`next4-observer-build`), source `packaging/windows/vacards/app-screen-observer.cpp`:

```sh
g++ -O2 -std=c++17 -Wall -Wextra app-screen-observer.cpp -o app-screen-observer.exe \
    -ld3d11 -ldxgi -ldxguid -lshcore -luser32 -lkernel32 -ladvapi32 -lole32 -lgdi32
```

Reference identities: source sha256
`de02002862fb2fd99b124179028ffbe83bbc1b6b9421fdf25b79fb41ddf10e7a`; exe sha256
`c58957cc1ea49fd6cb8b387cbefdf3f79eefe654a883827b4b4f164b821f5fb5` (exe hashes
vary per rebuild; the source hash is the stable identity).

## Required adjacent UCRT runtime DLLs

Keep these next to `app-screen-observer.exe` (non-system runtime closure only;
no OS/system DLLs). Hashes are from
`next4-observer-runtime-bundle/runtime-bundle-manifest.json`:

| DLL | sha256 |
|---|---|
| `libstdc++-6.dll` | `69200d6d96b903dac4c873031813485a0455828be8101479efc56008862420cc` |
| `libgcc_s_seh-1.dll` | `80940372431cc76224dfda06e2d33f01e49af3b4e7c499c535be856ebcadd273` |
| `libwinpthread-1.dll` | `cd5fc7573f1ecf9a157bb62fbc35f8b97862dde6057ffabd6cebb3fc9fe6ff1d` |

## Evidence status of these copies

- The Python 16-case suite was run locally at promotion time (see
  `.dsh-report/next4-durable-observer-tests/reducer-test.log`).
- The Windows PS 5.1 suites were **not rerun** during this promotion. The
  byte-identical copies inherit the recorded remote run evidence from
  `next4-dual-arm-and-errors` (ArmPressOffset 38/38, ObserverErrors 29/29 including end-to-end serialization, ObserverCoverageAdmission 34/34, TrialObserverCoverage 10/10) and
  `next4-diagnostic-admission-repair` (BandAwareImage 16/16). "Inherited, not
  rerun" is distinct from "copied files": the bytes match, but no new execution
  is claimed here.
