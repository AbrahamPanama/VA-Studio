# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
  One fixed default-Cairo VA Studio app trial: owned launch, identity gates,
  deterministic pan (delegated), normal close, and fail-closed schema-4 CSV
  validation. Diagnostic harness; it issues no PASS and computes no FPS.

.DESCRIPTION
  The trial runner does NOT synthesise input itself. It launches one owned copy of
  a prepared VA Studio payload through its canonical VACards-Test.exe resource
  launcher, verifies the pinned executable / GTK / Cairo / pan-helper identities,
  verifies the actual owned child process (command line + app-id tag + loaded
  module paths + hashes), verifies the active physical console (WTS protocol
  console, active console session, not RDP), waits for the owned main window,
  allows a settle interval, then invokes the separately delivered deterministic
  pan helper exactly once. After a second settle it requests a normal WM_CLOSE /
  CloseMainWindow and waits for the launcher/child to exit so the buffered CSV
  can flush. Only then does it validate the schema-4 CSV. A run that cannot close
  normally, or whose CSV is missing, truncated, heterogeneous or wrong-renderer,
  is reported invalid and never valid.

  Process-scoped environment only: values are saved and restored in finally. No
  user-profile write, no registry, no scheduler, no elevation, no shell pipeline.
  The launcher process handle is retained and its stdout/stderr are drained
  asynchronously (System.Diagnostics.Process + CopyToAsync).

  The pan helper runs as its own retained PowerShell child (stdout/stderr drained
  asynchronously, bounded by PanHelperTimeoutSeconds); it is never executed
  in-process, so its `exit` cannot terminate this runner. The runner reads the
  helper's input-summary.json and enforces PID/HWND/console/client/DPI/event/CPU
  invariants (exit code alone is insufficient). It passes the loaded app's exact
  libglib-2.0-0.dll path+hash so the helper can bracket-verify the GLib monotonic
  clock basis against QPC; it never invents a clock offset.

  Phase boundaries come from the pan helper's own input-summary.json (explicit
  input_start/end QPC + UTC + qpc_frequency). Every complete CSV interval
  [sample_us - interval_ms*1000, sample_us] is labelled against that proven
  window; startup/resize, boundary and post-input rows are retained, and only
  wholly-contained rows enter steady-pan.csv. The existing stderr frame timings
  are not timestamped, so they remain whole-trial only; this runner does not
  invent per-phase attribution.

.PARAMETER AppRoot
  Absolute path to a prepared payload/install root containing VACards-Test.exe,
  bin\inkscape.exe, bin\libgtk-4-1.dll and bin\libcairo-2.dll.

.PARAMETER OutputDirectory
  Absolute path to a NEW directory for this run. Receives the isolated profile,
  fixture copy, rendering CSV, app logs, pan-helper output and run-summary.json.

.PARAMETER PanHelperPath
  Absolute path to the separately delivered deterministic pan helper
  (Invoke-AppPan.ps1). Its interface is exactly
  TargetProcessId, OutputDirectory, ClientWidth, ClientHeight, Loops,
  StepsPerLeg, StepIntervalMs, GlibDllPath, ExpectedGlibSha256. This runner never
  implements input interop.

.PARAMETER FixtureSvg
  Absolute path to the fixture SVG opened in the app. For Workload=object-drag this
  must be the narrow drag fixture (900x600, 1 CSS px per viewBox unit, ids on every
  renderable node, lower-left rect id dragTarget).

.PARAMETER Workload
  'pan' (default) preserves the existing middle-button pan profile exactly.
  'object-drag' opens the drag fixture and asks the shared helper for the fixed
  continuous-v2 protocol (one select click, one out-of-timing armed left-button
  press whose first motion fixes the 250 anchor, then 9 out/back cycles + one
  final outbound leg and ONE release; expected 1141 measured events), then runs
  Test-AppDragOutcome.ps1 over the helper's native baseline/endpoint/returned SVG
  snapshots and folds its verdict into the run validity.

.PARAMETER ExpectedDragFixtureSha256
  Pinned SHA-256 of the isolated drag fixture source. Required (non-empty) when
  Workload=object-drag; the trial re-hashes its isolated copy and refuses to launch
  the GUI on mismatch or on missing known ids.

.PARAMETER DragOraclePath
  Optional absolute path to Test-AppDragOutcome.ps1. Defaults to the sibling script.
  Used only when Workload=object-drag.

.PARAMETER DragOracleTimeoutSeconds
  Bounded wait for the outside-GUI drag oracle child. Default 120.

.PARAMETER ExpectedExeSha256
  Pinned SHA-256 of bin\inkscape.exe.

.PARAMETER ExpectedGtkSha256
  Pinned SHA-256 of bin\libgtk-4-1.dll.

.PARAMETER ExpectedCairoSha256
  Pinned SHA-256 of bin\libcairo-2.dll.

.PARAMETER ExpectedPanHelperSha256
  Pinned SHA-256 of the supplied pan helper script.

.PARAMETER LaunchTimeoutSeconds
  Bound for the owned child process and its main window. Default 90.

.PARAMETER SettleSeconds
  Settle before and after the pan helper. Default 2.

.PARAMETER CloseTimeoutSeconds
  Bound for the normal close. Default 15.

.PARAMETER PanHelperWaitSeconds
  Optional bounded wait for PanHelperPath to appear. Default 0 (fail closed).

.PARAMETER PanHelperTimeoutSeconds
  Optional outer bound for the single pan-helper child process. Default 600.

.PARAMETER GdiBufferMode
  Bounded experimental control for the patched GTK Cairo GDI backing store
  (packaging/windows/vacards/gtk-4.22.4-win32-cairo-buffer.patch). 'default'
  clears GDK_WIN32_CAIRO_GDI_BUFFER; 'disabled' sets it to exactly '0'. These
  request an environment setting only: a stock GTK DLL ignores the variable, so
  the recorded effective env value alone is not evidence that the patched buffer
  path was used. Default 'default' preserves the pre-control environment
  contract. Any other value is rejected at parameter binding.

.EXAMPLE
  .\Invoke-AppTrial.ps1 -AppRoot C:\vacards\gtk-gdi-buffer-20260921\dev-en-test\baseline-app `
    -OutputDirectory C:\vacards\gtk-gdi-buffer-20260921\app-benchmark-prep\trial-01 `
    -PanHelperPath C:\vacards\gtk-gdi-buffer-20260921\app-benchmark-prep\Invoke-AppPan.ps1 `
    -FixtureSvg .\fixture-simple.svg `
    -ExpectedExeSha256 <64hex> -ExpectedGtkSha256 <64hex> `
    -ExpectedCairoSha256 <64hex> -ExpectedPanHelperSha256 <64hex>

.NOTES
  Diagnostic only. No PASS, no FPS, no benchmark claim. Failed/invalid trials are
  preserved with their partial logs and are never reported as valid. The pan
  helper bounds its own fixed-length trajectory (10 loops x 60+60 steps) and
  signals failure by throwing or by exiting non-zero; this runner invokes it once
  and records that outcome.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$AppRoot,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$PanHelperPath,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$FixtureSvg,
    [ValidateSet('pan', 'object-drag')][string]$Workload = 'pan',
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedDragFixtureSha256 = '',
    [string]$DragOraclePath = '',
    [ValidateRange(10, 1200)][int]$DragOracleTimeoutSeconds = 120,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedExeSha256,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedGtkSha256,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedCairoSha256,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedPanHelperSha256,
    [ValidateRange(10, 600)][int]$LaunchTimeoutSeconds = 90,
    [ValidateRange(0, 60)][int]$SettleSeconds = 2,
    [ValidateRange(5, 120)][int]$CloseTimeoutSeconds = 15,
    [ValidateRange(0, 600)][int]$PanHelperWaitSeconds = 0,
    [ValidateRange(30, 3600)][int]$PanHelperTimeoutSeconds = 600,
    [ValidateSet('default', 'disabled')][string]$GdiBufferMode = 'default',

    # Opt-in maximized-workarea calibration (object-drag only). Both must be
    # supplied together or both left empty. Supplying both validates the pinned
    # vacards-maximized-drag-calibration/1 JSON (hash, schema, every field/type/
    # bound) BEFORE launch, forwards it to the pan helper, validates the
    # maximized geometry and calibrated physical client/DPI/canvas scale, and
    # supplies the calibrated physical-pixels-per-logical-pixel factor to the
    # drag oracle. Defaults empty preserve the legacy normal-client trial.
    [string]$MaximizedCalibrationPath = '',

    [ValidatePattern('^([0-9a-fA-F]{64})?$')]
    [string]$ExpectedMaximizedCalibrationSha256 = '',

    # Optional screen-observer hook (object-drag maximized-workarea only). All
    # four must be supplied together or all left empty (fail closed otherwise).
    # Forwarded verbatim to the pan helper, which re-validates the exe hash and
    # the pinned vacards-screen-observer-roi/1 config before any input. Defaults
    # empty preserve the legacy trial and launch no observer.
    [string]$ObserverExePath = '',

    [ValidatePattern('^([0-9a-fA-F]{64})?$')]
    [string]$ExpectedObserverExeSha256 = '',

    [string]$ObserverRoiConfigPath = '',

    [ValidatePattern('^([0-9a-fA-F]{64})?$')]
    [string]$ExpectedObserverRoiConfigSha256 = ''
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Fixed default-Cairo trial contract (the helper owns sizing/focus/trajectory).
$TrialSchema             = 'vacards-app-trial/1'
# dev_en's interactive physical console is session 2 (recorded in
# .dsh-report/dev-en-metrics/METRICS.md:57-59); the gate also requires that this
# equals WTSGetActiveConsoleSessionId with protocol console and no RDP.
$RequiredConsoleSession  = 2
$PanClientWidth          = 1280
$PanClientHeight         = 800
$PanLoops                = 10
$PanStepsPerLeg          = 60
$PanStepIntervalMs       = 16
# Object-drag workload contract (the helper owns the fixed gesture trajectory:
# continuous-v2 = one select click, one out-of-timing armed press (246) whose
# first motion anchors exactly at 250, then 9 out/back cycles and 1 final
# outbound leg at 60 steps/leg with exactly ONE release. 1141 measured events =
# 9*120 + 60 + 1 (nine out/back cycles, one final outbound leg, one release);
# there is no measured initial position or press, and
# the separate return drag uses its own analogous arm (374 -> anchor 370).
$DragExpectedEventCount  = 1141
$DragProtocolIdentity    = 'continuous-v2'
$DragTargetId            = 'dragTarget'
# Simple opaque fixture per ROOT-NOTE/ROOT-REVIEW: white 900x600 background, one
# plain opaque dragTarget and one tiny stationary collateral rect. No gradient,
# text, image, group, filter or transparency.
$DragFixtureRequiredIds  = @('background', 'dragTarget', 'tinyControl')
$DragFixtureExpectedGeometry = [ordered]@{
    background  = [ordered]@{ x = 0.0;   y = 0.0;   width = 900.0; height = 600.0 }
    dragTarget  = [ordered]@{ x = 40.0;  y = 420.0; width = 400.0; height = 140.0 }
    tinyControl = [ordered]@{ x = 800.0; y = 500.0; width = 40.0;  height = 20.0 }
}
$StderrTimingLabels      = @('vacards_frame_paint_us', 'vacards_frame_wait_us', 'vacards_input_latency_us')
$CsvExpectedSchema       = 4
$CsvExpectedRenderer     = 'GskCairoRenderer'
$CsvExpectedBackend      = 'Cairo'
$CsvInputEventNames      = @('motion_received', 'button_motion', 'button_presses', 'button_releases')
$CsvBaseColumns          = @('sample_us', 'interval_ms', 'canvas_id', 'gtk_renderer', 'canvas_backend',
                             'logical_width', 'logical_height', 'scale', 'threads', 'gtk_cycles',
                             'schema_version', 'render_mode', 'split_mode', 'dragging')
$CsvEventNames           = @('tiles', 'pixels', 'redraws', 'timeouts', 'store_recreated', 'store_shifted',
                             'redraw_requests', 'coalesced_requests', 'idle_callbacks', 'instant_callbacks',
                             'motion_received', 'motion_ignored', 'button_motion', 'button_presses',
                             'button_releases', 'paint_launches', 'deadline_callbacks')
$CsvTimingNames          = @('raster', 'buffer_wait', 'commit', 'paint', 'queue', 'update', 'redraw',
                             'dispatch', 'snapshot', 'callback_wait', 'redraw_context', 'motion_handler',
                             'event_handler')
$CsvTimingSuffixes       = @('_count', '_total_us', '_lifetime_max_us')
$CsvExpectedHeader       = @($CsvBaseColumns + $CsvEventNames)
foreach ($timing in $CsvTimingNames) {
    foreach ($suffix in $CsvTimingSuffixes) { $CsvExpectedHeader += ($timing + $suffix) }
}

# ---------------------------------------------------------------------------
# Native interop: physical-console classification and WM_CLOSE. Kept minimal;
# the pan helper owns its own richer interop.
# ---------------------------------------------------------------------------
if (-not ('VACardsTrialNative' -as [type])) {
    $nativeSource = @'
using System;
using System.Runtime.InteropServices;
public static class VACardsTrialNative
{
    public const int SM_REMOTESESSION = 0x1000;
    public const int WTSClientProtocolType = 16;
    public const uint WM_CLOSE = 0x0010;
    [DllImport("kernel32.dll")] public static extern uint WTSGetActiveConsoleSessionId();
    [DllImport("wtsapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool WTSQuerySessionInformationW(IntPtr hServer, int sessionId, int infoClass,
        out IntPtr ppBuffer, out int pBytesReturned);
    [DllImport("wtsapi32.dll")] public static extern void WTSFreeMemory(IntPtr pMemory);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int nIndex);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
}
'@
    Add-Type -TypeDefinition $nativeSource -Language CSharp
}

# ---------------------------------------------------------------------------
# Run state. Initialised before the first validation throw so the finally block
# can always persist a structured record.
# ---------------------------------------------------------------------------
$state = [ordered]@{
    schema      = $TrialSchema
    trialStatus = 'initializing'
    passIssued  = $false
    failure     = $null
    capturedUtc = $null
    host        = [ordered]@{}
    session     = [ordered]@{}
    app         = [ordered]@{}
    panHelper   = [ordered]@{}
    observer    = [ordered]@{ requested = $false }
    workload    = [ordered]@{ mode = $Workload }
    dragOutcome = $null
    dragTelemetry = $null
    maximizedCalibration = [ordered]@{ provided = $false; requested = $false; mode = 'normal-client' }
    profile     = [ordered]@{}
    fixture     = [ordered]@{}
    environment = [ordered]@{}
    launch      = [ordered]@{}
    window      = [ordered]@{}
    timing      = [ordered]@{}
    exit        = [ordered]@{ normalExit = $null; killedOwnProcesses = @() }
    loadedModules = [ordered]@{}
    csv         = [ordered]@{}
    output      = [ordered]@{}
    warnings    = @()
    validationErrors = @()
}
$summaryPath = $null
$outputCreated = $false
$launcherProcess = $null
$appProcess = $null
$launcherPid = $null
$appPid = $null
$stdoutStream = $null
$stderrStream = $null
$stdoutTask = $null
$stderrTask = $null
$savedEnv = $null
$managedEnv = $null
$validationErrors = New-Object System.Collections.Generic.List[string]

function Resolve-FullPath([string]$Path) {
    return [IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($Path))
}
function Get-FileSha256([string]$Path) {
    # FileShare.ReadWrite so a currently loaded module can still be hashed.
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try { return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
}
function Get-JsonProperty($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
function Get-JsonPropertyRaw($Object, [string]$Name) {
    # Same lookup as Get-JsonProperty but WITHOUT PowerShell's return-value
    # array unrolling. `return $property.Value` enumerates arrays, so an empty
    # JSON array collapses to $null and a single-element JSON array collapses to
    # its scalar element. That is relied on by existing callers, so
    # Get-JsonProperty is deliberately not changed; this raw variant is used
    # only where the JSON array shape itself must be validated.
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return ,$property.Value
}
function Get-TrialCalValue($Calibration, [string]$Name) {
    if ($null -eq $Calibration) { return $null }
    if ($Calibration -is [System.Collections.IDictionary]) { return $Calibration[$Name] }
    $prop = $Calibration.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $null }
    return $prop.Value
}
function Test-TrialStrictInt($Value) {
    if ($null -eq $Value) { return $false }
    if ($Value -is [bool] -or $Value -is [string] -or $Value -is [System.Array]) { return $false }
    if ($Value -is [System.Management.Automation.PSCustomObject]) { return $false }
    if ($Value -is [System.Collections.IDictionary]) { return $false }
    try {
        $d = [double]$Value
        if ([double]::IsNaN($d) -or [double]::IsInfinity($d)) { return $false }
        return ($d -eq [Math]::Floor($d))
    } catch { return $false }
}
function Read-TrialCalibrationBounds($Object, [string]$Name, $Errors) {
    $v = Get-TrialCalValue $Object $Name
    if ($v -isnot [System.Array] -or $v.Count -ne 4) { [void]$Errors.Add($Name + ' must be an array of exactly 4 integers'); return $null }
    foreach ($e in $v) { if (-not (Test-TrialStrictInt $e)) { [void]$Errors.Add($Name + ' must contain only integers'); return $null } }
    return @([int]$v[0], [int]$v[1], [int]$v[2], [int]$v[3])
}
function Get-ArmPressOffset {
    # Mirrors Invoke-AppPan.ps1 Get-ArmPressOffset: legacy 4; maximized
    # ceil(4*physical_pixels_per_logical_pixel)+2 from the independent measured
    # scale; $null (fail closed) on missing/NaN/Infinity/out-of-range scale.
    param([bool]$MaximizedMode = $false, $Calibration = $null)
    if (-not $MaximizedMode) { return 4 }
    $pptRaw = Get-TrialCalValue $Calibration 'physical_pixels_per_logical_pixel'
    if ($null -eq $pptRaw -or $pptRaw -is [bool] -or $pptRaw -is [string] -or
        $pptRaw -is [System.Array] -or $pptRaw -is [System.Management.Automation.PSCustomObject]) {
        return $null
    }
    try {
        $ppt = [double]$pptRaw
        if ([double]::IsNaN($ppt) -or [double]::IsInfinity($ppt) -or $ppt -le 0.0 -or $ppt -gt 1000.0) { return $null }
        $offset = [int]([Math]::Ceiling(4.0 * $ppt) + 2)
        if ($offset -lt 1) { return $null }
        return $offset
    } catch { return $null }
}
function Read-TrialMaximizedCalibration([string]$Path, [string]$ExpectedSha256) {
    # Strict, pre-launch validation of the pinned calibration. Hash first, then
    # ConvertFrom-Json (never Invoke-Expression), then every field/type/bound and
    # the whole gesture path inside the physical client. The helper repeats this
    # before input; a fresh in-run measurement is never the expected calibration.
    $res = [ordered]@{ ok = $false; reason = $null; actualSha256 = $null; schema = $null; calibration = $null; errors = @() }
    $errs = New-Object System.Collections.Generic.List[string]
    $res.errors = @($errs)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        [void]$errs.Add('MaximizedCalibrationPath is missing or not a file'); $res.reason = $errs[0]; $res.errors = @($errs); return $res
    }
    $full = $null
    try { $full = (Resolve-Path -LiteralPath $Path).Path } catch { [void]$errs.Add('MaximizedCalibrationPath could not be resolved'); $res.reason = $errs[0]; $res.errors = @($errs); return $res }
    try { $res.actualSha256 = (Get-FileSha256 $full) } catch { [void]$errs.Add('calibration sha256 could not be computed'); $res.reason = $errs[0]; $res.errors = @($errs); return $res }
    if ($res.actualSha256 -ne ([string]$ExpectedSha256).ToLowerInvariant()) {
        [void]$errs.Add('calibration sha256 does not match ExpectedMaximizedCalibrationSha256'); $res.reason = $errs[0]; $res.errors = @($errs); return $res
    }
    $obj = $null
    try { $obj = ([IO.File]::ReadAllText($full) | ConvertFrom-Json) } catch { [void]$errs.Add('calibration is not valid JSON'); $res.reason = $errs[0]; $res.errors = @($errs); return $res }
    if ($null -eq $obj) { [void]$errs.Add('calibration JSON is empty'); $res.reason = $errs[0]; $res.errors = @($errs); return $res }
    $schema = [string](Get-TrialCalValue $obj 'schema'); $res.schema = $schema
    if ($schema -ne 'vacards-maximized-drag-calibration/1') { [void]$errs.Add('schema is not vacards-maximized-drag-calibration/1 (got ' + $schema + ')') }
    $calId = Get-TrialCalValue $obj 'calibration_id'
    if ($calId -isnot [string] -or [string]::IsNullOrWhiteSpace($calId)) { [void]$errs.Add('calibration_id must be a non-empty string') }
    $dev = Get-TrialCalValue $obj 'monitor_device'
    if ($dev -isnot [string] -or [string]::IsNullOrWhiteSpace($dev)) { [void]$errs.Add('monitor_device must be a non-empty string') }
    $mon = Read-TrialCalibrationBounds $obj 'monitor_bounds' $errs
    $work = Read-TrialCalibrationBounds $obj 'work_bounds' $errs
    if ($null -ne $mon -and ($mon[0] -ge $mon[2] -or $mon[1] -ge $mon[3])) { [void]$errs.Add('monitor_bounds must satisfy left<right and top<bottom') }
    if ($null -ne $work) {
        if ($work[0] -ge $work[2] -or $work[1] -ge $work[3]) { [void]$errs.Add('work_bounds must satisfy left<right and top<bottom') }
        if ($null -ne $mon -and ($work[0] -lt $mon[0] -or $work[1] -lt $mon[1] -or $work[2] -gt $mon[2] -or $work[3] -gt $mon[3])) { [void]$errs.Add('work_bounds must lie inside monitor_bounds') }
    }
    $dpiRaw = Get-TrialCalValue $obj 'dpi'
    if (-not (Test-TrialStrictInt $dpiRaw) -or [int]$dpiRaw -le 0 -or [int]$dpiRaw -gt 960) { [void]$errs.Add('dpi must be an integer in 1..960') }
    $cwRaw = Get-TrialCalValue $obj 'client_width'; $chRaw = Get-TrialCalValue $obj 'client_height'
    if (-not (Test-TrialStrictInt $cwRaw) -or [int]$cwRaw -le 0) { [void]$errs.Add('client_width must be a positive integer') }
    if (-not (Test-TrialStrictInt $chRaw) -or [int]$chRaw -le 0) { [void]$errs.Add('client_height must be a positive integer') }
    if ($null -ne $mon -and (Test-TrialStrictInt $cwRaw) -and (Test-TrialStrictInt $chRaw) -and
        ([int]$cwRaw -gt ($mon[2] - $mon[0]) -or [int]$chRaw -gt ($mon[3] - $mon[1]))) {
        [void]$errs.Add('client_width/client_height must not exceed the monitor bounds')
    }
    $sxRaw = Get-TrialCalValue $obj 'select_client_x'; $syRaw = Get-TrialCalValue $obj 'select_client_y'
    if (-not (Test-TrialStrictInt $sxRaw) -or [int]$sxRaw -lt 0) { [void]$errs.Add('select_client_x must be a non-negative integer') }
    if (-not (Test-TrialStrictInt $syRaw) -or [int]$syRaw -lt 0) { [void]$errs.Add('select_client_y must be a non-negative integer') }
    if ((Test-TrialStrictInt $cwRaw) -and (Test-TrialStrictInt $chRaw) -and (Test-TrialStrictInt $sxRaw) -and (Test-TrialStrictInt $syRaw)) {
        $cw = [int]$cwRaw; $ch = [int]$chRaw; $sx = [int]$sxRaw; $sy = [int]$syRaw
        if ($sx -ge $cw) { [void]$errs.Add('select_client_x must be inside [0, client_width)') }
        if ($sy -ge $ch) { [void]$errs.Add('select_client_y must be inside [0, client_height)') }
        if (($sx + 120) -gt ($cw - 1)) { [void]$errs.Add('drag endpoint (select_client_x+120) is outside the client') }
    }
    $pptRaw = Get-TrialCalValue $obj 'physical_pixels_per_logical_pixel'
    $ppt = $null
    if ($null -eq $pptRaw -or $pptRaw -is [bool] -or $pptRaw -is [string] -or $pptRaw -is [System.Array] -or $pptRaw -is [System.Management.Automation.PSCustomObject]) {
        [void]$errs.Add('physical_pixels_per_logical_pixel must be a finite positive number')
    } else {
        try {
            $ppt = [double]$pptRaw
            if ([double]::IsNaN($ppt) -or [double]::IsInfinity($ppt) -or $ppt -le 0.0 -or $ppt -gt 1000.0) { [void]$errs.Add('physical_pixels_per_logical_pixel must be finite in (0, 1000]'); $ppt = $null }
        } catch { [void]$errs.Add('physical_pixels_per_logical_pixel must be a finite positive number'); $ppt = $null }
    }
    # Whole injected gesture path must stay inside the physical client:
    # arm press sx-offset -> anchor sx -> endpoint sx+120 -> return press
    # sx+120+offset -> sx. The offset derives from the independent measured
    # physical scale; an invalid scale has already failed closed above.
    if ((Test-TrialStrictInt $cwRaw) -and (Test-TrialStrictInt $chRaw) -and (Test-TrialStrictInt $sxRaw) -and (Test-TrialStrictInt $syRaw)) {
        $armOffset = Get-ArmPressOffset -MaximizedMode:$true -Calibration $obj
        if ($null -ne $armOffset) {
            if (($sx - $armOffset) -lt 0) { [void]$errs.Add('arm press (select_client_x-' + $armOffset + ') is outside the client') }
            if (($sx + 120 + $armOffset) -gt ($cw - 1)) { [void]$errs.Add('return press (select_client_x+120+' + $armOffset + ') is outside the client') }
        }
    }
    $csRaw = Get-TrialCalValue $obj 'canvas_scale'
    if (-not (Test-TrialStrictInt $csRaw) -or [int]$csRaw -le 0) { [void]$errs.Add('canvas_scale must be a positive integer') }
    if ($errs.Count -gt 0) { $res.errors = @($errs); $res.reason = ($errs -join ' | '); return $res }
    $res.calibration = [ordered]@{
        schema = $schema; calibration_id = [string]$calId; monitor_device = [string]$dev
        monitor_bounds = $mon; work_bounds = $work; dpi = [int]$dpiRaw
        client_width = [int]$cwRaw; client_height = [int]$chRaw
        select_client_x = [int]$sxRaw; select_client_y = [int]$syRaw
        physical_pixels_per_logical_pixel = [double]$ppt; canvas_scale = [int]$csRaw
        source_path = $full; source_sha256 = $res.actualSha256
    }
    $res.ok = $true; $res.errors = @()
    return $res
}
function Get-GdiBufferControl([string]$Mode) {
    # Bounded experimental control for the patched GTK Cairo GDI backing store.
    # The patch (packaging/windows/vacards/gtk-4.22.4-win32-cairo-buffer.patch)
    # reads GDK_WIN32_CAIRO_GDI_BUFFER and disables its reusable backing store
    # only when the value is exactly '0'; unset/any other value keeps the buffered
    # default. 'default' preserves the pre-control contract (variable cleared);
    # 'disabled' sets exactly '0' (legacy per-frame escape).
    switch ($Mode) {
        'default' {
            return [ordered]@{
                mode = 'default'; controlLabel = 'gdi-buffer-default'
                envVariable = 'GDK_WIN32_CAIRO_GDI_BUFFER'; envValue = $null; envLiteral = '<cleared>'
            }
        }
        'disabled' {
            return [ordered]@{
                mode = 'disabled'; controlLabel = 'gdi-buffer-disabled'
                envVariable = 'GDK_WIN32_CAIRO_GDI_BUFFER'; envValue = '0'; envLiteral = '0'
            }
        }
        default { throw ('Unsupported GdiBufferMode: "' + $Mode + '" (valid: default, disabled).') }
    }
}
function Get-TrialAppliedEnvironment($GdiControl, [string]$ProfileDirectory, [string]$RenderStatsLog) {
    # Exact process/child environment contract. '<cleared>' means the variable is
    # removed for the owned child; an ambient value is never inherited.
    return [ordered]@{
        GSK_RENDERER = 'cairo'
        GDK_DISABLE = 'dcomp'
        GDK_BACKEND = 'win32'
        GDK_DEBUG = '<cleared>'
        GDK_WIN32_FORCE_DCOMP = '<cleared>'
        GDK_WIN32_CAIRO_GDI_BUFFER = $GdiControl.envLiteral
        VACARDS_REDRAW_DEADLINE = '<cleared>'
        VACARDS_DOCUMENT_DEADLINE = '<cleared>'
        VACARDS_TEST_PROFILE_DIR = $ProfileDirectory
        VACARDS_RENDER_STATS_LOG = $RenderStatsLog
        VACARDS_FRAME_TIMING = '1'
        VACARDS_INPUT_LATENCY = '1'
    }
}
function Get-TrialPreferencesXml([string]$Mode) {
    # Pan keeps the original minimal profile exactly. Object-drag adds the
    # verified native preferences: the fresh-profile live-content selector
    # (/tools/select/show="content") and snapping off
    # (/options/snapping/snap-global-toggle="0"). The fixture also carries the
    # native inkscape:snap-global="false"; the preference is the authoritative
    # control because source attributes.cpp comments out the document mapping.
    $snapping = ''
    $tools = ''
    if ($Mode -eq 'object-drag') {
        $snapping = '    <group id="snapping" snap-global-toggle="0"/>' + "`n"
        $tools = "  <group id=`"tools`">`n    <group id=`"select`" show=`"content`"/>`n  </group>`n"
    }
    return @"
<inkscape version="1">
  <group id="options">
    <group id="rendering" show_diagnostics="1" request_opengl="0" windows_accelerated="0"/>
$snapping    <group id="savewindowgeometry" value="2"/>
  </group>
$tools  <group id="desktop">
    <group id="geometry" width="1280" height="800" maximized="0" fullscreen="0"/>
  </group>
</inkscape>
"@
}
function Get-XmlAttr($Node, [string]$LocalName) {
    if ($null -eq $Node) { return $null }
    foreach ($attr in $Node.Attributes) {
        if ($attr.LocalName -eq $LocalName) { return $attr.Value }
    }
    return $null
}
function Get-SvgElementById($Node, [string]$Id) {
    if ($null -eq $Node) { return $null }
    foreach ($child in $Node.ChildNodes) {
        if ($child.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
        if ((Get-XmlAttr $child 'id') -eq $Id) { return $child }
        $found = Get-SvgElementById $child $Id
        if ($null -ne $found) { return $found }
    }
    return $null
}
function Add-SvgRenderableIds($Node, $Ids) {
    $skip = @('namedview', 'title', 'desc', 'metadata', 'script', 'style')
    foreach ($child in $Node.ChildNodes) {
        if ($child.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
        if ($skip -contains $child.LocalName) { continue }
        $id = Get-XmlAttr $child 'id'
        if ($null -ne $id) { $Ids.Add($id) | Out-Null }
        Add-SvgRenderableIds $child $Ids
    }
}
function Test-DragFixture([string]$Path, [string]$ExpectedSha256, [string]$Label) {
    # Pre-GUI narrow-fixture gate: pinned hash, well-formed XML, root units and
    # exactly three direct root rects (known ids, exact geometry) with no
    # gradient, text, image, group, filter or transparency.
    $res = [ordered]@{
        path = $Path; sha256 = $null; hashExpected = $ExpectedSha256; hashMatches = $false
        xmlValid = $false; ids = @(); missingIds = @(); rootOk = $false
        directRenderableCount = $null; rectGeometry = $null; targetGeometry = $null; errors = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $errs.Add($Label + ' fixture is missing'); $res.errors = @($errs); return $res
    }
    try { $res.sha256 = Get-FileSha256 $Path } catch { $errs.Add($Label + ' fixture hash unavailable') }
    $res.hashMatches = ($res.sha256 -eq ([string]$ExpectedSha256).ToLowerInvariant())
    if (-not $res.hashMatches) { $errs.Add($Label + ' fixture sha256 does not match the pinned ExpectedDragFixtureSha256') }
    $doc = $null
    try {
        $doc = New-Object System.Xml.XmlDocument
        $doc.XmlResolver = $null
        $doc.Load($Path)
        $res.xmlValid = ($null -ne $doc.DocumentElement -and $doc.DocumentElement.LocalName -eq 'svg')
        if (-not $res.xmlValid) { $errs.Add($Label + ' fixture root is not <svg>') }
    } catch { $errs.Add($Label + ' fixture is not well-formed XML: ' + $_.Exception.Message) }
    if ($res.xmlValid) {
        $root = $doc.DocumentElement
        $w = Get-XmlAttr $root 'width'; $h = Get-XmlAttr $root 'height'; $vb = Get-XmlAttr $root 'viewBox'
        $res.rootOk = ($w -eq '900' -and $h -eq '600' -and $vb -eq '0 0 900 600')
        if (-not $res.rootOk) { $errs.Add($Label + ' fixture root is not width=900 height=600 viewBox="0 0 900 600"') }
        if (-not [string]::IsNullOrWhiteSpace((Get-XmlAttr $root 'transform'))) {
            $errs.Add($Label + ' fixture root <svg> has a transform')
        }
        $skip = @('namedview', 'title', 'desc', 'metadata', 'script', 'style', 'defs')
        $direct = New-Object 'System.Collections.Generic.List[System.Xml.XmlElement]'
        foreach ($child in $root.ChildNodes) {
            if ($child.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
            if ($skip -contains $child.LocalName) { continue }
            $direct.Add($child) | Out-Null
        }
        $res.directRenderableCount = $direct.Count
        if ($direct.Count -ne 3) {
            $errs.Add($Label + ' fixture must have exactly 3 direct renderable rects, observed ' + $direct.Count)
        }
        $ids = New-Object System.Collections.Generic.List[string]
        $geo = [ordered]@{}
        foreach ($element in $direct) {
            if ($element.LocalName -ne 'rect') { $errs.Add($Label + ' fixture renderable <' + $element.LocalName + '> is not a rect') }
            $id = Get-XmlAttr $element 'id'
            if ([string]::IsNullOrWhiteSpace($id)) { $errs.Add($Label + ' fixture renderable rect has no id'); continue }
            if ($ids -contains $id) { $errs.Add($Label + ' fixture has duplicate id ' + $id); continue }
            $ids.Add($id) | Out-Null
            $geo[$id] = [ordered]@{
                tag = $element.LocalName; x = (Get-XmlAttr $element 'x'); y = (Get-XmlAttr $element 'y')
                width = (Get-XmlAttr $element 'width'); height = (Get-XmlAttr $element 'height')
            }
            foreach ($attr in $element.Attributes) {
                if ($attr.LocalName -eq 'id') { continue }
                if (@('fill-opacity', 'stroke-opacity', 'opacity') -contains $attr.LocalName) {
                    $errs.Add($Label + ' fixture id ' + $id + ' carries transparency attribute ' + $attr.LocalName)
                }
                if ($attr.Value -match 'url\(') {
                    $errs.Add($Label + ' fixture id ' + $id + ' references a paint server (url(...)); gradients/patterns are not allowed')
                }
            }
        }
        $res.ids = @($ids)
        $missing = @()
        foreach ($required in $DragFixtureRequiredIds) {
            if (-not ($res.ids -contains $required)) { $missing += $required }
        }
        $res.missingIds = @($missing)
        if ($missing.Count -gt 0) { $errs.Add($Label + ' fixture is missing required ids: ' + ($missing -join ', ')) }
        $rectGeometry = [ordered]@{}
        foreach ($known in $DragFixtureRequiredIds) {
            if (-not $geo.Contains($known)) { continue }
            $entry = $geo[$known]
            $rectGeometry[$known] = $entry
            if ($entry.tag -ne 'rect') { $errs.Add($Label + ' fixture ' + $known + ' is not a rect') }
            foreach ($key in @('x', 'y', 'width', 'height')) {
                $actual = 0.0
                $expected = [double]$DragFixtureExpectedGeometry[$known][$key]
                if (-not [double]::TryParse([string]$entry[$key], [ref]$actual) -or
                    [double]::IsNaN($actual) -or [double]::IsInfinity($actual) -or
                    [Math]::Abs($actual - $expected) -gt 0.001) {
                    $errs.Add($Label + ' fixture ' + $known + ' ' + $key + ' is not ' + $expected)
                }
            }
        }
        $res.rectGeometry = $rectGeometry
        if ($rectGeometry.Contains($DragTargetId)) { $res.targetGeometry = $rectGeometry[$DragTargetId] }
    }
    $res.errors = @($errs)
    return $res
}
function Set-ManagedProcessEnvironment($Values) {
    # Process-scoped only: $null/'<cleared>' removes the variable, anything else
    # is set. Shared by the parent process and mirrored into the child PSI.
    foreach ($name in $Values.Keys) {
        $value = $Values[$name]
        if ($null -eq $value -or [string]$value -eq '<cleared>') {
            [Environment]::SetEnvironmentVariable($name, $null, 'Process')
        } else {
            [Environment]::SetEnvironmentVariable($name, [string]$value, 'Process')
        }
    }
}
function Restore-ManagedProcessEnvironment([string[]]$Names, $Saved) {
    foreach ($name in $Names) {
        $previous = $Saved[$name]
        if ($null -eq $previous) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
        else { [Environment]::SetEnvironmentVariable($name, [string]$previous, 'Process') }
    }
}
function Get-ProcessExitInfo($Process) {
    $info = [ordered]@{ processId = $null; hasExited = $null; exitCode = $null; killUnavailable = $false }
    if ($null -eq $Process) { return $info }
    try { $info.processId = [int]$Process.Id } catch { return $info }
    try { $info.hasExited = [bool]$Process.HasExited } catch { $info.hasExited = 'unavailable'; return $info }
    if ($info.hasExited) {
        try { $info.exitCode = [int]$Process.ExitCode } catch { $info.exitCode = 'unavailable' }
    }
    return $info
}
function Test-ProcessAlive($Process) {
    if ($null -eq $Process) { return $false }
    # Fail-safe: an unreadable process is treated as alive so no validity gate
    # can pass and no owned-process cleanup is skipped because of the error.
    try { return -not $Process.HasExited } catch { return $true }
}
function Test-PathInside([string]$Path, [string]$Root) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::GetFullPath($Root)
    if (-not $fullRoot.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $fullRoot += [IO.Path]::DirectorySeparatorChar
    }
    return $fullPath.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)
}
function Get-PhysicalConsoleInfo {
    $info = [ordered]@{
        userInteractive        = [Environment]::UserInteractive
        sessionId              = $null
        sessionName            = [string]$env:SESSIONNAME
        activeConsoleSessionId = $null
        protocol               = $null
        protocolName           = 'unknown'
        remoteSystemMetrics    = $null
        isRemote               = $null
        physicalConsole        = $false
        requiredSessionId      = $RequiredConsoleSession
        errors                 = @()
    }
    $errors = New-Object System.Collections.Generic.List[string]
    try { $info.sessionId = [int](Get-Process -Id $PID).SessionId } catch { $errors.Add('current session id unavailable') }
    try { $info.activeConsoleSessionId = [uint32][VACardsTrialNative]::WTSGetActiveConsoleSessionId() } catch { $errors.Add('WTSGetActiveConsoleSessionId failed') }
    try {
        $buffer = [IntPtr]::Zero
        $bytes = 0
        $ok = [VACardsTrialNative]::WTSQuerySessionInformationW([IntPtr]::Zero, [int]$info.sessionId,
                [VACardsTrialNative]::WTSClientProtocolType, [ref]$buffer, [ref]$bytes)
        if ($ok -and $buffer -ne [IntPtr]::Zero) {
            try {
                if ($bytes -ge 2) {
                    $protocol = [int][Runtime.InteropServices.Marshal]::ReadInt16($buffer)
                    $info.protocol = $protocol
                    $info.protocolName = switch ($protocol) {
                        0 { 'console' }
                        1 { 'legacy' }
                        2 { 'rdp' }
                        default { 'unknown' }
                    }
                }
            } finally { [VACardsTrialNative]::WTSFreeMemory($buffer) }
        } else { $errors.Add('WTSClientProtocolType query failed') }
    } catch { $errors.Add('WTSClientProtocolType query threw') }
    try {
        $info.remoteSystemMetrics = [int][VACardsTrialNative]::GetSystemMetrics([VACardsTrialNative]::SM_REMOTESESSION)
    } catch { $errors.Add('GetSystemMetrics(SM_REMOTESESSION) failed') }
    if ($null -ne $info.remoteSystemMetrics) { $info.isRemote = ($info.remoteSystemMetrics -ne 0) }
    $info.errors = @($errors)
    $physical = $info.userInteractive -and
        ($null -ne $info.sessionId) -and ($info.sessionId -gt 0) -and
        ($info.sessionId -eq $RequiredConsoleSession) -and
        ($null -ne $info.activeConsoleSessionId) -and ([uint32]$info.sessionId -eq [uint32]$info.activeConsoleSessionId) -and
        ($info.protocol -eq 0) -and ($info.isRemote -eq $false)
    $info.physicalConsole = [bool]$physical
    return $info
}
function Write-PanJsonArtifacts([string]$Directory) {
    $result = [ordered]@{ directory = $Directory; present = $false; jsonFiles = @(); summaries = @(); phaseBoundaries = $null }
    if ([string]::IsNullOrWhiteSpace($Directory) -or -not (Test-Path -LiteralPath $Directory -PathType Container)) { return $result }
    $result.present = $true
    $files = @(Get-ChildItem -LiteralPath $Directory -Recurse -File -Filter '*.json' -ErrorAction SilentlyContinue | Sort-Object FullName)
    $result.jsonFiles = @($files | ForEach-Object { $_.FullName })
    foreach ($file in $files) {
        $entry = [ordered]@{ file = $file.FullName; sha256 = $null; parsed = $null; error = $null }
        try { $entry.sha256 = Get-FileSha256 $file.FullName } catch { $entry.error = 'hash unavailable' }
        try { $entry.parsed = (Get-Content -Raw -LiteralPath $file.FullName | ConvertFrom-Json) } catch { $entry.error = 'parse failed' }
        if ($null -ne $entry.parsed -and $null -eq $result.phaseBoundaries) {
            foreach ($name in @('phase_boundaries', 'phaseBoundaries', 'phases', 'boundaries', 'phase_timestamps')) {
                $value = Get-JsonProperty $entry.parsed $name
                if ($null -ne $value) { $result.phaseBoundaries = $value; break }
            }
        }
        $result.summaries += $entry
    }
    return $result
}
function Invoke-DragOutcomeOracle([string]$PanDir, [string]$OraclePath, [int]$TimeoutSeconds, [bool]$MaximizedMode = $false, [double]$PhysicalPixelsPerLogicalPixel = 1.0) {
    # Runs Test-AppDragOutcome.ps1 as its own retained PowerShell child OUTSIDE
    # the GUI/timing window and reads its JSON verdict. The oracle's own errors
    # are folded into the run validity by the caller.
    $res = [ordered]@{
        attempted = $false; oraclePath = $OraclePath; oracleSha256 = $null
        exitCode = $null; timedOut = $false; error = $null
        outputJsonPath = $null; outputJsonSha256 = $null
        status = $null; valid = $null; oracle = $null
        stdoutLog = $null; stderrLog = $null; errors = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    if ([string]::IsNullOrWhiteSpace($OraclePath) -or -not (Test-Path -LiteralPath $OraclePath -PathType Leaf)) {
        $errs.Add('drag oracle script is missing: ' + $OraclePath)
        $res['errors'] = @($errs); return $res
    }
    try { $res.oracleSha256 = Get-FileSha256 $OraclePath } catch { $errs.Add('drag oracle hash unavailable') }
    $baselinePath = Join-Path $PanDir 'baseline.svg'
    $endpointPath = Join-Path $PanDir 'endpoint.svg'
    $returnedPath = Join-Path $PanDir 'returned.svg'
    $outJsonPath  = Join-Path $PanDir 'drag-outcome.json'
    $res.outputJsonPath = $outJsonPath
    $stdoutPath = Join-Path $PanDir 'drag-oracle-stdout.log'
    $stderrPath = Join-Path $PanDir 'drag-oracle-stderr.log'
    $res.stdoutLog = $stdoutPath
    $res.stderrLog = $stderrPath
    $res.attempted = $true
    $psExe = Join-Path $PSHOME 'powershell.exe'
    if (-not (Test-Path -LiteralPath $psExe -PathType Leaf)) {
        $psExe = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    }
    $argList = @(
        '-NoProfile', '-NonInteractive', '-File', $OraclePath,
        '-BaselineSvg', $baselinePath, '-EndpointSvg', $endpointPath, '-ReturnedSvg', $returnedPath,
        '-TargetId', $DragTargetId,
        '-ExpectedScreenDelta', '120', '-ToleranceScreenPx', '1.5',
        '-OutputJson', $outJsonPath
    )
    if ($MaximizedMode) {
        # Calibrated physical pixels per logical pixel, supplied ONLY in the new
        # mode. Legacy omits the parameter entirely (oracle default 1.0).
        $argList += @('-PhysicalPixelsPerLogicalPixel', $PhysicalPixelsPerLogicalPixel.ToString([Globalization.CultureInfo]::InvariantCulture))
    }
    $quoted = @($argList | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } })
    $proc = $null; $outStream = $null; $errStream = $null; $outTask = $null; $errTask = $null
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $psExe
        $psi.Arguments = ($quoted -join ' ')
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.CreateNoWindow = $true
        $psi.WorkingDirectory = $PanDir
        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        if (-not $proc.Start()) { throw 'could not start the drag oracle PowerShell child' }
        $outStream = [IO.File]::Create($stdoutPath)
        $errStream = [IO.File]::Create($stderrPath)
        $outTask = $proc.StandardOutput.BaseStream.CopyToAsync($outStream)
        $errTask = $proc.StandardError.BaseStream.CopyToAsync($errStream)
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while (-not $proc.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 150 }
        if (-not $proc.HasExited) {
            $res.timedOut = $true
            $errs.Add('drag oracle child exceeded the ' + $TimeoutSeconds + 's bound')
            try { $proc.Kill() } catch {}
            try { [void]$proc.WaitForExit(5000) } catch {}
        } else {
            $res.exitCode = [int]$proc.ExitCode
        }
    } catch {
        $res.error = $_.Exception.Message
        $errs.Add('drag oracle child failed: ' + $_.Exception.Message)
        if ($null -eq $res.exitCode) { $res.exitCode = 99 }
    } finally {
        try { [System.Threading.Tasks.Task]::WaitAll(@($outTask, $errTask), 8000) | Out-Null } catch {}
        foreach ($stream in @($outStream, $errStream)) {
            if ($null -ne $stream) { try { $stream.Flush(); $stream.Dispose() } catch {} }
        }
    }
    if (Test-Path -LiteralPath $outJsonPath -PathType Leaf) {
        try {
            $res.outputJsonSha256 = Get-FileSha256 $outJsonPath
            $parsed = (Get-Content -Raw -LiteralPath $outJsonPath | ConvertFrom-Json)
            $res.oracle = $parsed
            $res.status = [string](Get-JsonProperty $parsed 'status')
            $res.valid = (Get-JsonProperty $parsed 'valid')
            foreach ($e in @(Get-JsonProperty $parsed 'errors')) { if ($null -ne $e) { $errs.Add('oracle: ' + [string]$e) } }
        } catch { $errs.Add('drag oracle JSON could not be parsed: ' + $_.Exception.Message) }
    } else {
        $errs.Add('drag oracle did not write drag-outcome.json')
    }
    if ($null -ne $res.exitCode -and [int]$res.exitCode -ne 0) { $errs.Add('drag oracle exit code was ' + $res.exitCode) }
    if ($res.status -ne 'valid') { $errs.Add('drag oracle status is not valid (' + $res.status + ')') }
    $res['errors'] = @($errs)
    return $res
}
function Test-DragSteadyTelemetry([string]$SteadyCsvPath) {
    # ROOT-NOTE: at least one wholly-steady row with dragging=1 and button_motion>0.
    # Boundary press/release intervals are legitimately excluded, so only the
    # presence of steady drag work is required here, never an exact per-row count.
    $res = [ordered]@{
        path = $SteadyCsvPath; present = $false; draggingRows = 0; buttonMotionSum = 0
        draggingColumn = $false; buttonMotionColumn = $false; errors = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $SteadyCsvPath -PathType Leaf)) {
        $errs.Add('steady-pan.csv is missing for the object-drag workload'); $res.errors = @($errs); return $res
    }
    $res.present = $true
    try { $lines = @(Get-Content -LiteralPath $SteadyCsvPath) } catch { $errs.Add('steady-pan.csv unreadable'); $res.errors = @($errs); return $res }
    if ($lines.Count -lt 2) { $errs.Add('steady-pan.csv has no data rows'); $res.errors = @($errs); return $res }
    $header = @($lines[0] -split ',')
    $dragIdx = [Array]::IndexOf($header, 'dragging')
    $bmIdx = [Array]::IndexOf($header, 'button_motion')
    $res.draggingColumn = ($dragIdx -ge 0); $res.buttonMotionColumn = ($bmIdx -ge 0)
    if ($dragIdx -lt 0) { $errs.Add('steady CSV is missing the dragging column') }
    if ($bmIdx -lt 0) { $errs.Add('steady CSV is missing the button_motion column') }
    if ($errs.Count -gt 0) { $res.errors = @($errs); return $res }
    for ($i = 1; $i -lt $lines.Count; $i++) {
        $fields = @($lines[$i] -split ',')
        if ($fields.Count -le [Math]::Max($dragIdx, $bmIdx)) { continue }
        $drag = 0; $bm = 0
        if ([int]::TryParse($fields[$dragIdx].Trim(), [ref]$drag) -and $drag -eq 1) {
            $res.draggingRows++
            if ([int]::TryParse($fields[$bmIdx].Trim(), [ref]$bm)) { $res.buttonMotionSum += $bm }
        }
    }
    if ($res.draggingRows -lt 1) { $errs.Add('no wholly-steady row has dragging=1') }
    if ($res.buttonMotionSum -le 0) { $errs.Add('steady rows have no positive button_motion events') }
    $res.errors = @($errs)
    return $res
}
function Get-TextSha256([string]$Text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}
function Get-PanPhaseRange($Summary) {
    # Convert the helper's proven QPC input window into the CSV microsecond
    # basis. No offset is guessed: the helper's own clock contract validated
    # g_get_monotonic_time against QPC for the exact loaded GLib DLL.
    $range = [ordered]@{
        available = $false; valid = $false; input_start_us = $null; input_end_us = $null
        qpc_frequency = $null; input_start_utc = $null; input_end_utc = $null
        source = 'pan helper input-summary.json phase_boundaries'
        errors = @()
    }
    $errors = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Summary) { $errors.Add('pan helper summary is missing'); $range.errors = @($errors); return $range }
    $pb = Get-JsonProperty $Summary 'phase_boundaries'
    if ($null -eq $pb) { $errors.Add('phase_boundaries missing from pan helper summary'); $range.errors = @($errors); return $range }
    $s = Get-JsonProperty $pb 'input_start_qpc'
    $e = Get-JsonProperty $pb 'input_end_qpc'
    $f = Get-JsonProperty $pb 'qpc_frequency'
    if ($null -eq $s -or $null -eq $e -or $null -eq $f) {
        $errors.Add('phase_boundaries input_start_qpc/input_end_qpc/qpc_frequency incomplete')
        $range.errors = @($errors); return $range
    }
    if ([long]$f -le 0) { $errors.Add('phase_boundaries qpc_frequency is not positive'); $range.errors = @($errors); return $range }
    $startUs = ([double][long]$s) * 1000000.0 / [double][long]$f
    $endUs = ([double][long]$e) * 1000000.0 / [double][long]$f
    if ($endUs -le $startUs) { $errors.Add('phase_boundaries input_end_qpc is not after input_start_qpc'); $range.errors = @($errors); return $range }
    $range.qpc_frequency = [long]$f
    $range.input_start_us = [double]$startUs
    $range.input_end_us = [double]$endUs
    $range.input_start_utc = Get-JsonProperty $pb 'input_start_utc'
    $range.input_end_utc = Get-JsonProperty $pb 'input_end_utc'
    $range.available = $true
    $range.valid = $true
    $range.errors = @($errors)
    return $range
}
function Test-PanSummary($Summary, [int]$ExpectedEventCount, [int]$AppPid, [long]$ExpectedHwnd,
                         [int]$ClientWidth, [int]$ClientHeight, [int]$ConsoleSession,
                         [string]$ExpectedProtocolIdentity = '',
                         [string]$ExpectedWindowMode = 'normal-client',
                         $MaximizedCalibration = $null,
                         [bool]$ObserverRequested = $false) {
    # Exit code alone is insufficient: every invariant the trial depends on is
    # re-read from the helper's own JSON. `injected_event_count` is injected
    # (SendInput==1), never claimed as delivered.
    $result = [ordered]@{
        present = ($null -ne $Summary); status = $null; exitCode = $null
        injectedEventCount = $null; injectedIsNotDelivered = $true
        targetProcessId = $null; hwnd = $null; hwndMatches = $false
        sessionOk = $false; clientAfter = @(); clientMatches = $false; dpi = $null
        windowMode = $null; expectedClient = @(); expectedDpi = $null
        maximizedProof = $null; maximizedProofOk = $null; measuredGeometryGuardCount = $null
        durationSeconds = $null; durationPositive = $false
        cpuBefore = $null; cpuAfter = $null; cpuDelta = $null; cpuAvailable = $false
        clockContractStatus = $null; clockContractSamples = $null
        phaseBoundariesPresent = $false
        protocolIdentity = $null; protocolIdentityOk = $null
        observerPresent = $null; observerReadySeen = $null; observerExitCode = $null
        observerFramesWritten = $null; observerValidFrames = $null; observerForcedKill = $null
        errors = @()
    }
    $errors = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Summary) { $errors.Add('pan helper input-summary.json is missing or unparsable'); $result.errors = @($errors); return $result }
    $result.status = [string](Get-JsonProperty $Summary 'status')
    $result.exitCode = Get-JsonProperty $Summary 'exit_code'
    $targetProcess = Get-JsonProperty $Summary 'target_process'
    if ($null -ne $targetProcess) { $result.targetProcessId = Get-JsonProperty $targetProcess 'id' }
    $result.hwnd = Get-JsonProperty $Summary 'hwnd'
    $clientAfter = Get-JsonProperty $Summary 'client_after'
    if ($null -ne $clientAfter) { $result.clientAfter = @($clientAfter) }
    $result.dpi = Get-JsonProperty $Summary 'dpi'
    $result.windowMode = [string](Get-JsonProperty $Summary 'window_mode')
    $result.injectedEventCount = Get-JsonProperty $Summary 'injected_event_count'
    # Continuous-v2 identity is fail-closed: when the caller expects it, a helper
    # summary without the exact identity is invalid (never silently accepted).
    $objectDrag = Get-JsonProperty $Summary 'object_drag'
    if ($null -ne $objectDrag) { $result.protocolIdentity = Get-JsonProperty $objectDrag 'protocol_identity' }
    if (-not [string]::IsNullOrEmpty($ExpectedProtocolIdentity)) {
        $result.protocolIdentityOk = ($result.protocolIdentity -eq $ExpectedProtocolIdentity)
        if (-not $result.protocolIdentityOk) {
            $errors.Add('pan helper object_drag.protocol_identity is not the expected ' + $ExpectedProtocolIdentity + ' (got ' + [string]$result.protocolIdentity + ')')
        }
    }
    $result.durationSeconds = Get-JsonProperty $Summary 'window_duration_seconds'
    $result.cpuBefore = Get-JsonProperty $Summary 'cpu_seconds_before'
    $result.cpuAfter = Get-JsonProperty $Summary 'cpu_seconds_after'
    $result.cpuDelta = Get-JsonProperty $Summary 'cpu_seconds_delta'

    if ($result.status -ne 'ok') { $errors.Add('pan helper status is not ok (' + $result.status + ')') }
    if ($null -eq $result.exitCode) { $errors.Add('pan helper exit_code is missing') }
    elseif ([long]$result.exitCode -ne 0) { $errors.Add('pan helper exit_code is not 0 (' + $result.exitCode + ')') }
    if ($null -eq $result.targetProcessId) { $errors.Add('pan helper target_process.id is missing') }
    elseif ([int]$result.targetProcessId -ne $AppPid) { $errors.Add('pan helper target_process.id does not match the owned app pid') }
    $hwndMatch = $false
    if ($null -ne $result.hwnd) {
        try {
            $hwndText = ([string]$result.hwnd).Trim() -replace '^0[xX]', ''
            $hwndValue = [Convert]::ToInt64($hwndText, 16)
            $hwndMatch = ($hwndValue -eq $ExpectedHwnd)
        } catch { $hwndMatch = $false }
    }
    $result.hwndMatches = $hwndMatch
    if (-not $hwndMatch) { $errors.Add('pan helper hwnd does not match the owned main HWND') }
    $sess = Get-JsonProperty $Summary 'session'
    $sessionOk = $false
    if ($null -ne $sess) {
        $targetSession = Get-JsonProperty $sess 'target_session'
        $activeConsole = Get-JsonProperty $sess 'active_console_session'
        $protocol = Get-JsonProperty $sess 'client_protocol'
        $remote = Get-JsonProperty $sess 'is_remote'
        $sessionOk = ($null -ne $targetSession -and [int]$targetSession -eq $ConsoleSession -and
                      $null -ne $activeConsole -and [int]$activeConsole -eq $ConsoleSession -and
                      $null -ne $protocol -and [int]$protocol -eq 0 -and
                      $null -ne $remote -and [int]$remote -eq 0)
    }
    $result.sessionOk = $sessionOk
    if (-not $sessionOk) { $errors.Add('pan helper session is not the required physical console session ' + $ConsoleSession) }
    # Expected geometry: legacy normal-client defaults, or the calibrated
    # physical client/DPI plus the maximized proof in maximized-workarea mode.
    $expectedClientW = $ClientWidth
    $expectedClientH = $ClientHeight
    $expectedDpi = 96
    if ($ExpectedWindowMode -eq 'maximized-workarea') {
        $expectedClientW = [int](Get-TrialCalValue $MaximizedCalibration 'client_width')
        $expectedClientH = [int](Get-TrialCalValue $MaximizedCalibration 'client_height')
        $expectedDpi = [int](Get-TrialCalValue $MaximizedCalibration 'dpi')
        $result.expectedClient = @($expectedClientW, $expectedClientH)
        $result.expectedDpi = $expectedDpi
        if ($result.windowMode -ne 'maximized-workarea') {
            $errors.Add('pan helper window_mode is not maximized-workarea (got ' + [string]$result.windowMode + ')')
        }
        $proof = Get-JsonProperty $Summary 'maximized_proof'
        $result.maximizedProof = $proof
        $maxOk = $false
        if ($null -eq $proof) {
            $errors.Add('pan helper maximized_proof is missing')
        } else {
            $maxOk = ((Get-JsonProperty $proof 'ok') -eq $true)
            if (-not $maxOk) { $errors.Add('pan helper maximized_proof.ok is not true (' + [string](Get-JsonProperty $proof 'reason') + ')') }
            if ((Get-JsonProperty $proof 'is_zoomed') -ne $true) { $errors.Add('pan helper maximized_proof.is_zoomed is not true') }
            if ((Get-JsonProperty $proof 'is_iconic') -ne $false) { $errors.Add('pan helper maximized_proof.is_iconic is not false') }
            if ([int](Get-JsonProperty $proof 'placement_show_cmd') -ne 3) { $errors.Add('pan helper maximized_proof.placement_show_cmd is not 3') }
            if ((Get-JsonProperty $proof 'exact_client') -ne $true) { $errors.Add('pan helper maximized_proof.exact_client is not true') }
            if ((Get-JsonProperty $proof 'monitor_match') -ne $true) { $errors.Add('pan helper maximized_proof.monitor_match is not true') }
            if ((Get-JsonProperty $proof 'work_match') -ne $true) { $errors.Add('pan helper maximized_proof.work_match is not true') }
            if ((Get-JsonProperty $proof 'dpi_match') -ne $true) { $errors.Add('pan helper maximized_proof.dpi_match is not true') }
            if ((Get-JsonProperty $proof 'coverage_ok') -ne $true) { $errors.Add('pan helper maximized_proof.coverage_ok is not true') }
            if ((Get-JsonProperty $proof 'window_covers_work') -ne $true) { $errors.Add('pan helper maximized_proof.window_covers_work is not true') }
            if ((Get-JsonProperty $proof 'work_overhang_ok') -ne $true) { $errors.Add('pan helper maximized_proof.work_overhang_ok is not true') }
            if ((Get-JsonProperty $proof 'frame_ok') -ne $true) { $errors.Add('pan helper maximized_proof.frame_ok is not true') }
        }
        $result.maximizedProofOk = $maxOk
        $postProof = Get-JsonProperty $Summary 'post_settle_maximized_proof'
        if ($null -eq $postProof) {
            $errors.Add('pan helper post_settle_maximized_proof is missing')
        } elseif ((Get-JsonProperty $postProof 'ok') -ne $true) {
            $errors.Add('pan helper post_settle_maximized_proof.ok is not true')
        }
        # Runtime guard evidence: the helper must have re-proved the calibrated
        # maximized geometry during the measured drag and the return gesture, not
        # only before timing.
        $guardsRaw = Get-JsonProperty $Summary 'measured_geometry_guards'
        $guards = @()
        if ($null -ne $guardsRaw) { $guards = @($guardsRaw) }
        $result.measuredGeometryGuardCount = $guards.Count
        $expectedGuards = 2 * ($PanLoops - 1) + 2 + 3
        if ($guards.Count -lt $expectedGuards) {
            $errors.Add('pan helper measured_geometry_guards count is ' + $guards.Count + ', expected at least ' + $expectedGuards)
        }
        $whereList = New-Object System.Collections.Generic.List[string]
        foreach ($gd in $guards) {
            if ((Get-JsonProperty $gd 'ok') -ne $true) { $errors.Add('pan helper measured geometry guard failed at ' + [string](Get-JsonProperty $gd 'where')) }
            [void]$whereList.Add([string](Get-JsonProperty $gd 'where'))
        }
        foreach ($need in @('final-out', 'post-release', 'return-arm', 'return-leg', 'returned')) {
            if (-not $whereList.Contains($need)) { $errors.Add('pan helper measured geometry guard evidence is missing: ' + $need) }
        }
    }
    $ca = @($result.clientAfter)
    $clientMatches = ($ca.Count -eq 2 -and [int]$ca[0] -eq $expectedClientW -and [int]$ca[1] -eq $expectedClientH)
    $result.clientMatches = $clientMatches
    if (-not $clientMatches) { $errors.Add('pan helper client_after is not ' + $expectedClientW + 'x' + $expectedClientH) }
    if ($null -eq $result.dpi -or [int]$result.dpi -ne $expectedDpi) { $errors.Add('pan helper dpi is not ' + $expectedDpi) }
    if ($null -eq $result.injectedEventCount) { $errors.Add('pan helper injected_event_count is missing') }
    elseif ([long]$result.injectedEventCount -ne [long]$ExpectedEventCount) { $errors.Add('pan helper injected_event_count is not the expected ' + $ExpectedEventCount) }
    if ($null -ne $result.durationSeconds -and [double]$result.durationSeconds -gt 0) { $result.durationPositive = $true }
    else { $errors.Add('pan helper window_duration_seconds is not positive') }
    if ($null -ne $result.cpuBefore -and $null -ne $result.cpuAfter -and $null -ne $result.cpuDelta) { $result.cpuAvailable = $true }
    else { $errors.Add('pan helper CPU before/after/delta is not available') }
    $clockContract = Get-JsonProperty $Summary 'clock_contract'
    if ($null -eq $clockContract) {
        $errors.Add('pan helper clock_contract is missing')
    } else {
        $result.clockContractStatus = [string](Get-JsonProperty $clockContract 'status')
        $result.clockContractSamples = Get-JsonProperty $clockContract 'sample_count'
        if ($result.clockContractStatus -ne 'ok') { $errors.Add('pan helper clock_contract.status is not ok (' + $result.clockContractStatus + ')') }
        if ($null -eq $result.clockContractSamples -or [int]$result.clockContractSamples -lt 10) { $errors.Add('pan helper clock_contract has fewer than 10 QPC samples') }
        if ((Get-JsonProperty $clockContract 'all_in_bracket') -ne $true) { $errors.Add('pan helper clock_contract.all_in_bracket is not true') }
    }
    $pb = Get-JsonProperty $Summary 'phase_boundaries'
    $result.phaseBoundariesPresent = ($null -ne $pb)
    if ($null -eq $pb) { $errors.Add('pan helper phase_boundaries is missing') }
    else {
        if ((Get-JsonProperty $pb 'complete') -ne $true) { $errors.Add('pan helper phase_boundaries.complete is not true') }
        foreach ($k in @('input_start_qpc', 'input_end_qpc', 'input_start_utc', 'input_end_utc', 'qpc_frequency')) {
            if ($null -eq (Get-JsonProperty $pb $k)) { $errors.Add('pan helper phase_boundaries.' + $k + ' is missing') }
        }
    }
    # Optional observer hook: fail closed when it was requested. The helper
    # already invalidates the trial on a nonzero exit / forced kill / no frames,
    # but the trial re-reads its own summary as the acceptance authority.
    if ($ObserverRequested) {
        $obs = Get-JsonProperty $Summary 'observer'
        if ($null -eq $obs) {
            $result.observerPresent = $false
            $errors.Add('pan helper observer block is missing but the observer was requested')
        } else {
            $result.observerPresent = $true
            $result.observerReadySeen = Get-JsonProperty $obs 'ready_seen'
            $result.observerExitCode = Get-JsonProperty $obs 'exit_code'
            $result.observerFramesWritten = Get-JsonProperty $obs 'frames_written'
            $result.observerValidFrames = Get-JsonProperty $obs 'valid_frames'
            $result.observerForcedKill = Get-JsonProperty $obs 'forced_kill'
            if ((Get-JsonProperty $obs 'ready_seen') -ne $true) { $errors.Add('pan helper observer.ready_seen is not true (no valid baseline)') }
            if ($null -eq $result.observerExitCode) { $errors.Add('pan helper observer.exit_code is missing') }
            elseif ([int]$result.observerExitCode -ne 0) { $errors.Add('pan helper observer.exit_code is not 0 (' + [string]$result.observerExitCode + ')') }
            if ($null -eq $result.observerFramesWritten -or [long]$result.observerFramesWritten -lt 1) { $errors.Add('pan helper observer wrote no frames') }
            # frames_written includes warmup; valid_frames (counts.valid) is the
            # post-warmup measured-loop count, so require at least one measured
            # frame. Motion is not required (a real stall is a valid outcome).
            if ($null -eq $result.observerValidFrames -or [long]$result.observerValidFrames -lt 1) { $errors.Add('pan helper observer recorded no valid measured-loop frames') }
            if ((Get-JsonProperty $obs 'exited') -ne $true) { $errors.Add('pan helper observer did not exit normally') }
            if ((Get-JsonProperty $obs 'forced_kill') -eq $true) { $errors.Add('pan helper observer process was force-killed') }
            # Strictly validate observer.errors as a JSON array of non-blank
            # strings. An empty array is admissible. A real message is surfaced
            # (and therefore fails the trial); a `{}` object, null or any other
            # malformed shape fails explicitly instead of being rendered as a
            # blank error. Get-JsonProperty is deliberately NOT changed (its
            # array-unrolling return is relied on elsewhere); Get-JsonPropertyRaw
            # preserves the JSON array shape for this check.
            $obsErrors = Get-JsonPropertyRaw $obs 'errors'
            if ($null -eq $obsErrors) {
                $errors.Add('pan helper observer.errors is missing or null')
            } elseif ($obsErrors -isnot [System.Array]) {
                $errors.Add('pan helper observer.errors is not a JSON array of strings')
            } else {
                $obsErrIndex = 0
                foreach ($oe in $obsErrors) {
                    if ($oe -isnot [string] -or [string]::IsNullOrWhiteSpace($oe)) {
                        $errors.Add('pan helper observer.errors[' + $obsErrIndex + '] is not a non-blank string')
                    } else {
                        $errors.Add('pan helper observer: ' + $oe)
                    }
                    $obsErrIndex++
                }
            }
            # Independent observer-coverage revalidation against the trial's own
            # phase-boundaries window (do NOT trust the helper's complete_coverage
            # flag alone). The observer must have lived from before input_start
            # through after input_end, used the same QPC frequency, exited 0 for
            # the stop file, and not be fatal. Missing/null/wrong-typed fields
            # fail closed. Motion is not required (a real stall is valid).
            $winStart = Get-JsonProperty $pb 'input_start_qpc'
            $winEnd = Get-JsonProperty $pb 'input_end_qpc'
            $winFreq = Get-JsonProperty $pb 'qpc_frequency'
            $mSchema = Get-JsonProperty $obs 'metadata_schema'
            $mFreq = Get-JsonProperty $obs 'metadata_qpc_frequency'
            $mStart = Get-JsonProperty $obs 'metadata_qpc_start'
            $mReady = Get-JsonProperty $obs 'metadata_qpc_ready'
            $mEnd = Get-JsonProperty $obs 'metadata_qpc_end'
            $mExitCode = Get-JsonProperty $obs 'metadata_exit_code'
            $mExitReason = Get-JsonProperty $obs 'metadata_exit_reason'
            $mFatal = Get-JsonProperty $obs 'metadata_fatal'
            $result.observerCompleteCoverage = Get-JsonProperty $obs 'complete_coverage'
            if ($mSchema -isnot [string] -or [string]$mSchema -ne 'vacards.screen-observer/1') { $errors.Add('pan helper observer.metadata_schema is not vacards.screen-observer/1') }
            if (-not (Test-TrialStrictInt $winFreq) -or [long]$winFreq -le 0) { $errors.Add('pan helper phase_boundaries.qpc_frequency is not a positive integer') }
            elseif (-not (Test-TrialStrictInt $mFreq) -or [long]$mFreq -ne [long]$winFreq) { $errors.Add('pan helper observer.metadata_qpc_frequency does not match phase_boundaries.qpc_frequency') }
            if ($mFatal -isnot [bool] -or $mFatal -ne $false) { $errors.Add('pan helper observer.metadata_fatal is not strictly false') }
            if (-not (Test-TrialStrictInt $mExitCode) -or [long]$mExitCode -ne 0) { $errors.Add('pan helper observer.metadata_exit_code is not 0') }
            if ($mExitReason -isnot [string] -or [string]$mExitReason -ne 'stop_file') { $errors.Add('pan helper observer.metadata_exit_reason is not stop_file') }
            if (-not (Test-TrialStrictInt $mStart) -or [long]$mStart -le 0) { $errors.Add('pan helper observer.metadata_qpc_start is not a positive integer') }
            if (-not (Test-TrialStrictInt $mReady) -or [long]$mReady -le 0) { $errors.Add('pan helper observer.metadata_qpc_ready is not a positive integer') }
            if (-not (Test-TrialStrictInt $mEnd) -or [long]$mEnd -le 0) { $errors.Add('pan helper observer.metadata_qpc_end is not a positive integer') }
            if (-not (Test-TrialStrictInt $winStart) -or [long]$winStart -le 0) { $errors.Add('pan helper phase_boundaries.input_start_qpc is not a positive integer') }
            if (-not (Test-TrialStrictInt $winEnd) -or [long]$winEnd -le 0) { $errors.Add('pan helper phase_boundaries.input_end_qpc is not a positive integer') }
            if ((Test-TrialStrictInt $mStart) -and (Test-TrialStrictInt $winStart) -and [long]$mStart -gt [long]$winStart) { $errors.Add('pan helper observer.metadata_qpc_start is after phase_boundaries.input_start_qpc') }
            if ((Test-TrialStrictInt $mEnd) -and (Test-TrialStrictInt $winEnd) -and [long]$mEnd -lt [long]$winEnd) { $errors.Add('pan helper observer.metadata_qpc_end is before phase_boundaries.input_end_qpc') }
            if ((Test-TrialStrictInt $mReady) -and (Test-TrialStrictInt $mEnd) -and [long]$mReady -gt [long]$mEnd) { $errors.Add('pan helper observer.metadata_qpc_ready is after metadata_qpc_end') }
            if ((Test-TrialStrictInt $mReady) -and (Test-TrialStrictInt $winStart) -and [long]$mReady -gt [long]$winStart) { $errors.Add('pan helper observer.metadata_qpc_ready is after phase_boundaries.input_start_qpc (warmup extended into the measured window)') }
            if ((Test-TrialStrictInt $mStart) -and (Test-TrialStrictInt $mReady) -and [long]$mStart -gt [long]$mReady) { $errors.Add('pan helper observer.metadata_qpc_start is after metadata_qpc_ready') }
            if ((Get-JsonProperty $obs 'complete_coverage') -ne $true) { $errors.Add('pan helper observer.complete_coverage is not true') }
        }
    }
    $result.errors = @($errors)
    return $result
}
function Test-TrialCsv([string]$CsvPath, $PhaseRange, [string]$OutputDirectory, [double]$ExpectedCanvasScale = 1.0) {
    # Global header/row/backend/schema strictness is unchanged. Different
    # startup/resize logical dimensions are legitimate and retained. Every CSV
    # interval is labelled [sample_us - interval_ms*1000, sample_us]; only rows
    # wholly inside the proven input window go to steady-pan.csv. No row is
    # dropped or repaired. The single exception to the positive-dimension rule
    # is the structurally complete first raw row when the canvas was logged
    # before its first allocation (both dimensions exactly 0, all activity and
    # timing samples zero, interval wholly before the verified input start); it
    # is annotated initialization_row/excluded_reason and excluded from
    # measurement. Any other zero or non-positive dimension is still invalid.
    $result = [ordered]@{
        path = $CsvPath; present = $false; sha256 = $null; headerPresent = $false; headerExact = $false
        observedHeader = @(); expectedHeader = @($CsvExpectedHeader); rowCount = 0; dataRowCount = 0
        renderers = @(); backends = @(); schemaVersions = @(); canvasIds = @()
        dimensions = @(); scaleValues = @()
        inputEventCount = 0; steadyInputEventCount = 0
        truncatedLastRow = $false; endsWithNewline = $null
        steadyRowCount = 0; corruptedRowCount = 0
        initializationRowCount = 0; initializationRows = @()
        phases = [ordered]@{ startup = 0; boundary = 0; steady = 0; post_input = 0; unclassified = 0 }
        steadyCsvPath = $null; steadyCsvSha256 = $null; classificationPath = $null
        classificationSha256 = $null; steadyCanvasIds = @(); steadyDimensions = @()
        coverage = [ordered]@{}; excludedDuration = [ordered]@{}
        provenance = [ordered]@{}
        phaseRange = $PhaseRange
        errors = @(); warnings = @()
    }
    $errors = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $CsvPath -PathType Leaf)) {
        $errors.Add('rendering.csv is missing')
        $result.errors = @($errors)
        return $result
    }
    $result.present = $true
    try { $result.sha256 = Get-FileSha256 $CsvPath } catch { $errors.Add('rendering.csv hash unavailable') }
    $text = $null
    try { $text = [IO.File]::ReadAllText($CsvPath) } catch { $errors.Add('rendering.csv unreadable'); $result.errors = @($errors); return $result }
    $lines = @($text -split "`r?`n")
    $result.endsWithNewline = ($text.Length -gt 0 -and ($text.EndsWith("`n") -or $text.EndsWith("`r")))
    if (-not $result.endsWithNewline) {
        $warnings.Add('rendering.csv does not end with a newline; a complete final row is still accepted, a partial one is rejected below.')
    }
    # Keep every non-empty physical line, including a final line with no trailing
    # newline. Truncated rows are rejected below, never dropped or repaired.
    $nonEmpty = New-Object System.Collections.Generic.List[string]
    foreach ($line in $lines) { if (-not [string]::IsNullOrWhiteSpace($line)) { $nonEmpty.Add($line) } }
    if ($nonEmpty.Count -eq 0) { $errors.Add('rendering.csv is empty'); $result.errors = @($errors); return $result }
    $header = @($nonEmpty[0] -split ',')
    $result.headerPresent = $true
    $result.observedHeader = @($header)
    $headerOk = $true
    if ($header.Count -ne $CsvExpectedHeader.Count) {
        $headerOk = $false
        $errors.Add('CSV header column count is ' + $header.Count + ', expected ' + $CsvExpectedHeader.Count)
    } else {
        for ($c = 0; $c -lt $header.Count; $c++) {
            if ($header[$c] -ne $CsvExpectedHeader[$c]) {
                $headerOk = $false
                $errors.Add('CSV header column ' + ($c + 1) + ' mismatch')
                break
            }
        }
    }
    $result.headerExact = $headerOk
    if (-not $headerOk) { $result.errors = @($errors); return $result }
    if ($nonEmpty.Count -lt 2) { $errors.Add('rendering.csv has a header but no data rows'); $result.errors = @($errors); return $result }

    $index = @{}
    for ($c = 0; $c -lt $header.Count; $c++) { $index[$header[$c]] = $c }

    # Resolve the proven input window once, before the row loop, so the
    # first-row initialization tolerance can never be granted without a
    # verified input start.
    $rangeValid = ($null -ne $PhaseRange -and $PhaseRange.valid)
    $startUs = $null; $endUs = $null
    if ($rangeValid) {
        $startUs = [double]$PhaseRange.input_start_us
        $endUs = [double]$PhaseRange.input_end_us
    }

    $rows = New-Object System.Collections.ArrayList
    $renderers = New-Object System.Collections.Generic.HashSet[string]
    $backends = New-Object System.Collections.Generic.HashSet[string]
    $schemas = New-Object System.Collections.Generic.HashSet[string]
    $canvasIds = New-Object System.Collections.Generic.HashSet[string]
    $dimensions = New-Object System.Collections.Generic.HashSet[string]
    $scales = New-Object System.Collections.Generic.HashSet[string]
    $inputTotal = 0L
    $corrupted = 0

    for ($i = 1; $i -lt $nonEmpty.Count; $i++) {
        $raw = $nonEmpty[$i]
        $dataRow = $i
        $fields = @($raw -split ',')
        if ($fields.Count -ne $CsvExpectedHeader.Count) {
            $corrupted++
            $errors.Add('CSV row ' + $dataRow + ' has ' + $fields.Count + ' columns, expected ' + $CsvExpectedHeader.Count)
            if ($i -eq ($nonEmpty.Count - 1)) {
                $result.truncatedLastRow = $true
                $errors.Add('CSV final row is truncated; it is rejected, not repaired')
            }
            [void]$rows.Add([ordered]@{
                original_row_index = $dataRow; complete = $false; raw = $raw; row_sha256 = (Get-TextSha256 $raw)
                sample_us = 0; interval_ms = 0.0; interval_start_us = $null; interval_end_us = $null
                canvas_id = $null; logical_width = $null; logical_height = $null; dimension = $null
                scale = $null; scale_is_integer = $false; gtk_renderer = $null; canvas_backend = $null
                schema_version = $null; motion_events = 0
                phase = 'unclassified'
            })
            continue
        }
        $renderer = $fields[$index['gtk_renderer']].Trim()
        $backend = $fields[$index['canvas_backend']].Trim()
        $schema = $fields[$index['schema_version']].Trim()
        $canvasIdRaw = $fields[$index['canvas_id']].Trim()
        $widthRaw = $fields[$index['logical_width']].Trim()
        $heightRaw = $fields[$index['logical_height']].Trim()
        $scaleText = $fields[$index['scale']].Trim()
        $renderers.Add($renderer) | Out-Null
        $backends.Add($backend) | Out-Null
        $schemas.Add($schema) | Out-Null
        $canvasIds.Add($canvasIdRaw) | Out-Null
        $dimensions.Add($widthRaw + 'x' + $heightRaw) | Out-Null
        $scales.Add($scaleText) | Out-Null
        $canvasIdValue = 0
        if (-not [int]::TryParse($canvasIdRaw, [ref]$canvasIdValue) -or $canvasIdValue -lt 0) {
            $errors.Add('CSV row ' + $dataRow + ' has invalid canvas_id=' + $canvasIdRaw)
        }
        $widthValue = 0
        $heightValue = 0
        # Dimensions are parsed here but validated after the interval and the
        # initialization-row predicate are known (see below).
        $widthParsed = [int]::TryParse($widthRaw, [ref]$widthValue)
        $heightParsed = [int]::TryParse($heightRaw, [ref]$heightValue)
        $sampleUs = 0L
        $intervalMs = 0.0
        $scaleValue = 0.0
        $parseOk = [long]::TryParse($fields[$index['sample_us']].Trim(), [ref]$sampleUs)
        if ($parseOk) { $parseOk = [double]::TryParse($fields[$index['interval_ms']].Trim(), [ref]$intervalMs) }
        if ($parseOk) { $parseOk = [double]::TryParse($scaleText, [ref]$scaleValue) }
        if (-not $parseOk) {
            $errors.Add('CSV row ' + $dataRow + ' has an unparsable sample_us/interval_ms/scale')
        }
        $scaleIsInteger = $parseOk -and ($scaleValue -eq [Math]::Floor($scaleValue))
        if ($parseOk -and -not $scaleIsInteger) {
            $errors.Add('CSV row ' + $dataRow + ' has a fractional scale (' + $scaleText + '); rejected, never rescaled')
        }
        $rowInput = 0
        foreach ($name in $CsvInputEventNames) {
            if ($index.ContainsKey($name)) {
                $value = 0
                if ([int]::TryParse($fields[$index[$name]].Trim(), [ref]$value)) { $rowInput += $value }
                else { $errors.Add('CSV row ' + $dataRow + ' has non-integer ' + $name) }
            }
        }
        $inputTotal += $rowInput
        $intervalStart = $null
        $intervalEnd = $null
        if ($parseOk) {
            $intervalStart = [double]$sampleUs - ($intervalMs * 1000.0)
            $intervalEnd = [double]$sampleUs
        }
        # The canvas logs its logical size from the widget allocation, which is
        # still 0x0 on the very first startup sample before the canvas has been
        # allocated. That one initialization snapshot is tolerated only when it
        # is the structurally complete first raw row, both raw dimensions are
        # exactly 0, every event counter and timing sample (plus gtk_cycles and
        # the dragging flag) is an integer zero, renderer/backend/schema/scale are
        # the expected values, and its whole interval ends at or before the
        # verified input start. Any other non-positive dimension stays invalid.
        $zeroDimStartupRow = $false
        if ($dataRow -eq 1 -and $widthParsed -and $heightParsed -and
            $widthRaw -eq '0' -and $heightRaw -eq '0' -and
            $parseOk -and $scaleIsInteger -and $scaleValue -eq $ExpectedCanvasScale -and
            $renderer -eq $CsvExpectedRenderer -and $backend -eq $CsvExpectedBackend -and
            $schema -eq [string]$CsvExpectedSchema -and
            $rangeValid -and $null -ne $intervalEnd -and [double]$intervalEnd -le $startUs) {
            $zeroDimStartupRow = $true
            $draggingValue = 0L
            if (-not [long]::TryParse($fields[$index['dragging']].Trim(), [ref]$draggingValue) -or $draggingValue -ne 0) {
                $zeroDimStartupRow = $false
            }
            $gtkCyclesValue = 0L
            if (-not [long]::TryParse($fields[$index['gtk_cycles']].Trim(), [ref]$gtkCyclesValue) -or $gtkCyclesValue -ne 0) {
                $zeroDimStartupRow = $false
            }
            foreach ($name in $CsvEventNames) {
                if ($zeroDimStartupRow -and $index.ContainsKey($name)) {
                    $eventValue = 0L
                    if (-not [long]::TryParse($fields[$index[$name]].Trim(), [ref]$eventValue) -or $eventValue -ne 0) {
                        $zeroDimStartupRow = $false
                    }
                }
            }
            foreach ($timingName in $CsvTimingNames) {
                foreach ($suffix in $CsvTimingSuffixes) {
                    if ($zeroDimStartupRow) {
                        $timingValue = 0L
                        if (-not [long]::TryParse($fields[$index[$timingName + $suffix]].Trim(), [ref]$timingValue) -or $timingValue -ne 0) {
                            $zeroDimStartupRow = $false
                        }
                    }
                }
            }
        }
        if (-not $widthParsed -or $widthValue -le 0) {
            if (-not $zeroDimStartupRow) {
                $errors.Add('CSV row ' + $dataRow + ' has invalid logical_width=' + $widthRaw)
            }
        }
        if (-not $heightParsed -or $heightValue -le 0) {
            if (-not $zeroDimStartupRow) {
                $errors.Add('CSV row ' + $dataRow + ' has invalid logical_height=' + $heightRaw)
            }
        }
        [void]$rows.Add([ordered]@{
            original_row_index = $dataRow
            complete = $parseOk
            raw = $raw
            row_sha256 = (Get-TextSha256 $raw)
            sample_us = $sampleUs
            interval_ms = $intervalMs
            interval_start_us = $intervalStart
            interval_end_us = $intervalEnd
            canvas_id = $canvasIdRaw
            logical_width = $widthRaw
            logical_height = $heightRaw
            dimension = ($widthRaw + 'x' + $heightRaw)
            scale = $scaleText
            scale_is_integer = $scaleIsInteger
            gtk_renderer = $renderer
            canvas_backend = $backend
            schema_version = $schema
            motion_events = $rowInput
            phase = 'unclassified'
            initialization_row = $zeroDimStartupRow
            excluded_reason = if ($zeroDimStartupRow) {
                'initialization/unallocated canvas snapshot: structurally complete first raw row with logical size 0x0 before the first allocation, all event counters and timing samples zero, expected renderer/backend/schema/scale, interval wholly before the verified input start; retained in rendering.csv and excluded from steady-pan.csv measurement'
            } else { $null }
        })
    }
    $result.rowCount = $nonEmpty.Count - 1
    $result.dataRowCount = $result.rowCount
    $result.corruptedRowCount = $corrupted
    $result.renderers = @($renderers | Sort-Object)
    $result.backends = @($backends | Sort-Object)
    $result.schemaVersions = @($schemas | Sort-Object)
    $result.canvasIds = @($canvasIds | Sort-Object)
    $result.dimensions = @($dimensions | Sort-Object)
    $result.scaleValues = @($scales | Sort-Object)
    $result.inputEventCount = [int64]$inputTotal

    # Retained but excluded initialization rows (at most the first raw row).
    $initializationRows = @($rows | Where-Object { $_.Contains('initialization_row') -and $_.initialization_row })
    $result.initializationRowCount = [int]$initializationRows.Count
    $result.initializationRows = @($initializationRows | ForEach-Object {
        [ordered]@{
            original_row_index = $_.original_row_index
            row_sha256 = $_.row_sha256
            logical_width = $_.logical_width
            logical_height = $_.logical_height
            excluded_reason = $_.excluded_reason
        }
    })

    if ($result.renderers.Count -ne 1 -or $result.renderers[0] -ne $CsvExpectedRenderer) {
        $errors.Add('every CSV row must be gtk_renderer=' + $CsvExpectedRenderer + ' (observed: ' + (($result.renderers -join ', ')) + ')')
    }
    if ($result.backends.Count -ne 1 -or $result.backends[0] -ne $CsvExpectedBackend) {
        $errors.Add('every CSV row must be canvas_backend=' + $CsvExpectedBackend + ' (observed: ' + (($result.backends -join ', ')) + ')')
    }
    if ($result.schemaVersions.Count -ne 1 -or $result.schemaVersions[0] -ne [string]$CsvExpectedSchema) {
        $errors.Add('every CSV row must be schema_version=' + $CsvExpectedSchema + ' (observed: ' + (($result.schemaVersions -join ', ')) + ')')
    }
    if ($result.inputEventCount -le 0) { $errors.Add('CSV input event count must be positive (observed ' + $result.inputEventCount + ')') }

    # ---- phase classification against the proven QPC-microsecond window ----
    if (-not $rangeValid) {
        $errors.Add('proven QPC input phase range is unavailable; CSV intervals cannot be classified (no speculative offset)')
    }
    $completeRows = @($rows | Where-Object { $_.complete -and $_.scale_is_integer })
    $csvFirst = $null; $csvLast = $null
    foreach ($r in $completeRows) {
        if ($null -eq $csvFirst -or [double]$r.interval_start_us -lt $csvFirst) { $csvFirst = [double]$r.interval_start_us }
        if ($null -eq $csvLast -or [double]$r.interval_end_us -gt $csvLast) { $csvLast = [double]$r.interval_end_us }
    }
    if ($rangeValid -and $null -ne $csvFirst) {
        if ($endUs -lt $csvFirst -or $startUs -gt $csvLast) {
            $errors.Add('proven input window [' + [Math]::Round($startUs, 1) + ',' + [Math]::Round($endUs, 1) + '] does not overlap the CSV sample range [' + [Math]::Round($csvFirst, 1) + ',' + [Math]::Round($csvLast, 1) + ']')
        }
        if ($startUs -lt $csvFirst) { $warnings.Add('input window starts before the first CSV sample; early input is not covered') }
        if ($endUs -gt $csvLast) { $warnings.Add('input window ends after the last CSV sample; late input is not covered') }
    }
    foreach ($r in $completeRows) {
        if (-not $rangeValid -or $null -eq $r.interval_start_us) { $r.phase = 'unclassified' }
        elseif ([double]$r.interval_start_us -ge $startUs -and [double]$r.interval_end_us -le $endUs) { $r.phase = 'steady' }
        elseif ([double]$r.interval_end_us -le $startUs) { $r.phase = 'startup' }
        elseif ([double]$r.interval_start_us -ge $endUs) { $r.phase = 'post_input' }
        else { $r.phase = 'boundary' }
        if ($result.phases.Contains($r.phase)) { $result.phases[$r.phase] = [int]$result.phases[$r.phase] + 1 }
        else { $result.phases['unclassified'] = [int]$result.phases['unclassified'] + 1 }
    }
    $steadyRows = @($rows | Where-Object { $_.phase -eq 'steady' })
    $result.steadyRowCount = $steadyRows.Count

    # ---- steady configuration: one canvas, one logical dimension, scale 1 ----
    $steadyCanvas = New-Object System.Collections.Generic.HashSet[string]
    $steadyDims = New-Object System.Collections.Generic.HashSet[string]
    $steadyInput = 0L
    foreach ($r in $steadyRows) {
        $steadyCanvas.Add([string]$r.canvas_id) | Out-Null
        $steadyDims.Add([string]$r.dimension) | Out-Null
        $steadyInput += [int64]$r.motion_events
        if ([double]$r.scale -ne $ExpectedCanvasScale) { $errors.Add('steady row ' + $r.original_row_index + ' has scale ' + $r.scale + ', expected ' + $ExpectedCanvasScale) }
    }
    $result.steadyCanvasIds = @($steadyCanvas | Sort-Object)
    $result.steadyDimensions = @($steadyDims | Sort-Object)
    $result.steadyInputEventCount = [int64]$steadyInput
    if ($result.steadyRowCount -lt 10) { $errors.Add('steady-pan.csv must contain at least 10 complete steady rows (observed ' + $result.steadyRowCount + ')') }
    if ($result.steadyCanvasIds.Count -ne 1) { $errors.Add('steady rows must share one canvas_id (observed: ' + (($result.steadyCanvasIds -join ', ')) + ')') }
    if ($result.steadyDimensions.Count -ne 1) { $errors.Add('steady rows must share one logical dimension (observed: ' + (($result.steadyDimensions -join ', ')) + ')') }
    if ($result.steadyInputEventCount -le 0) { $errors.Add('steady rows must contain positive app motion/button events (observed ' + $result.steadyInputEventCount + ')') }

    # ---- retained artifacts: steady-pan.csv + classification JSON ----
    $steadyPath = $null; $classPath = $null
    if (-not [string]::IsNullOrEmpty($OutputDirectory) -and (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
        $steadyPath = Join-Path $OutputDirectory 'steady-pan.csv'
        $classPath = Join-Path $OutputDirectory 'csv-phases.json'
        $steadyOut = New-Object System.Collections.Generic.List[string]
        $steadyOut.Add(($header -join ','))
        foreach ($r in $steadyRows) { $steadyOut.Add([string]$r.raw) }
        $steadyText = (($steadyOut -join "`n")) + "`n"
        try {
            [IO.File]::WriteAllText($steadyPath, $steadyText, [System.Text.UTF8Encoding]::new($false))
            $result.steadyCsvPath = $steadyPath
            $result.steadyCsvSha256 = Get-FileSha256 $steadyPath
        } catch { $errors.Add('could not write steady-pan.csv: ' + $_.Exception.Message) }
    } else {
        $errors.Add('output directory is unavailable; steady-pan.csv not written')
    }

    # ---- coverage + excluded-duration labels ----
    $sumDur = @{ startup = 0.0; boundary = 0.0; steady = 0.0; post_input = 0.0; unclassified = 0.0 }
    $steadySpan = 0.0
    foreach ($r in $completeRows) {
        $dur = [double]$r.interval_ms * 1000.0
        if ($sumDur.ContainsKey($r.phase)) { $sumDur[$r.phase] += $dur }
    }
    if ($steadyRows.Count -gt 0) {
        $steadyMin = ($steadyRows | ForEach-Object { [double]$_.interval_start_us } | Measure-Object -Minimum).Minimum
        $steadyMax = ($steadyRows | ForEach-Object { [double]$_.interval_end_us } | Measure-Object -Maximum).Maximum
        $steadySpan = $steadyMax - $steadyMin
    }
    $excluded = $sumDur.startup + $sumDur.boundary + $sumDur.post_input
    $result.coverage = [ordered]@{
        csv_first_sample_us = $csvFirst
        csv_last_sample_us = $csvLast
        csv_span_us = if ($null -ne $csvFirst -and $null -ne $csvLast) { $csvLast - $csvFirst } else { $null }
        input_phase_us = if ($rangeValid) { $endUs - $startUs } else { $null }
        steady_span_us = $steadySpan
        total_data_rows = $result.rowCount
        complete_rows = $completeRows.Count
        corrupted_rows = $corrupted
        initialization_rows = $result.initializationRowCount
        initialization_row_indices = @($result.initializationRows | ForEach-Object { $_.original_row_index })
        note = 'steady coverage is the sum of complete CSV intervals wholly inside the proven input window; startup/resize and boundary/post-input intervals are retained but excluded.'
    }
    $result.excludedDuration = [ordered]@{
        startup_ms = [Math]::Round($sumDur.startup / 1000.0, 3)
        boundary_ms = [Math]::Round($sumDur.boundary / 1000.0, 3)
        post_input_ms = [Math]::Round($sumDur.post_input / 1000.0, 3)
        unclassified_ms = [Math]::Round($sumDur.unclassified / 1000.0, 3)
        total_excluded_ms = [Math]::Round($excluded / 1000.0, 3)
        note = 'excluded boundary duration = startup/resize + boundary-overlap + post-input intervals; all rows remain in rendering.csv.'
    }
    $result.provenance = [ordered]@{
        rendering_csv_sha256 = $result.sha256
        steady_pan_csv_sha256 = $result.steadyCsvSha256
        original_row_indices = @($rows | ForEach-Object { $_.original_row_index })
        row_hashes = @($rows | ForEach-Object { $_.row_sha256 })
        event_semantics = 'CSV event/timing columns are already per-interval deltas; this classifier sums them once and does not re-difference adjacent rows.'
        clock_bracket = 'phase boundaries come from the helper QPC window whose GLib monotonic basis was bracket-verified against the loaded app DLL'
    }
    $classification = [ordered]@{
        schema = 'vacards-app-csv-phases/1'
        rendering_csv = $CsvPath
        rendering_csv_sha256 = $result.sha256
        header = $header
        phase_range = $PhaseRange
        counts = $result.phases
        steady_row_count = $result.steadyRowCount
        corrupted_row_count = $corrupted
        initialization_rows = $result.initializationRows
        coverage = $result.coverage
        excluded_duration = $result.excludedDuration
        steady_pan_csv = $steadyPath
        steady_pan_csv_sha256 = $result.steadyCsvSha256
        rows = @($rows)
    }
    if (-not [string]::IsNullOrEmpty($classPath)) {
        try {
            [IO.File]::WriteAllText($classPath, ($classification | ConvertTo-Json -Depth 8), [System.Text.UTF8Encoding]::new($false))
            $result.classificationPath = $classPath
            $result.classificationSha256 = Get-FileSha256 $classPath
        } catch { $errors.Add('could not write csv-phases.json: ' + $_.Exception.Message) }
    }

    $result.warnings = @($warnings)
    $result.errors = @($errors)
    return $result
}
function Write-RunSummary {
    $state.capturedUtc = [DateTime]::UtcNow.ToString('o')
    $state.output.summaryPath = $summaryPath
    try {
        $json = $state | ConvertTo-Json -Depth 16
        [IO.File]::WriteAllText($summaryPath, $json, [System.Text.UTF8Encoding]::new($false))
    } catch {
        Write-Warning ('Could not write run summary: ' + $_.Exception.Message)
    }
}
function Add-ValidationError([string]$Message) {
    $validationErrors.Add($Message) | Out-Null
    $state.warnings += $Message
}

# ---------------------------------------------------------------------------
# Platform / physical-console gate. Session 0 (SSH/service) has no desktop.
# ---------------------------------------------------------------------------
if ($env:OS -ne 'Windows_NT') { throw 'This app trial harness requires Windows.' }
$state.host = [ordered]@{
    computerName = $env:COMPUTERNAME
    userName     = [Environment]::UserName
    osVersion    = [Environment]::OSVersion.VersionString
    psVersion    = $PSVersionTable.PSVersion.ToString()
    is64BitProcess = [Environment]::Is64BitProcess
}
$state.session = Get-PhysicalConsoleInfo
if (-not $state.session.physicalConsole) {
    throw ('Refusing a non-physical-console run: session=' + $state.session.sessionId +
           ' activeConsole=' + $state.session.activeConsoleSessionId +
           ' protocol=' + $state.session.protocolName +
           ' remote=' + $state.session.isRemote +
           '; require interactive physical console session ' + $RequiredConsoleSession + ' with no RDP.')
}

# ---------------------------------------------------------------------------
# App root and pinned identities.
# ---------------------------------------------------------------------------
$appRootFull = Resolve-FullPath $AppRoot
if (-not (Test-Path -LiteralPath $appRootFull -PathType Container)) { throw ('AppRoot is not a directory: ' + $appRootFull) }
$requiredFiles = [ordered]@{
    'VACards-Test.exe'   = (Join-Path $appRootFull 'VACards-Test.exe')
    'bin/inkscape.exe'   = (Join-Path $appRootFull 'bin\inkscape.exe')
    'bin/libgtk-4-1.dll' = (Join-Path $appRootFull 'bin\libgtk-4-1.dll')
    'bin/libcairo-2.dll' = (Join-Path $appRootFull 'bin\libcairo-2.dll')
}
$missing = @()
foreach ($key in $requiredFiles.Keys) {
    if (-not (Test-Path -LiteralPath $requiredFiles[$key] -PathType Leaf)) { $missing += $key }
}
if ($missing.Count -gt 0) { throw ('AppRoot is missing required files: ' + ($missing -join ', ')) }

# ---------------------------------------------------------------------------
# Fixture.
# ---------------------------------------------------------------------------
$fixtureSource = Resolve-FullPath $FixtureSvg
if (-not (Test-Path -LiteralPath $fixtureSource -PathType Leaf)) { throw ('Fixture SVG was not found: ' + $fixtureSource) }

function Stop-OwnedProcesses {
    $killed = @()
    foreach ($item in @(
        [ordered]@{ name = 'app'; process = $appProcess; pid = $appPid },
        [ordered]@{ name = 'launcher'; process = $launcherProcess; pid = $launcherPid }
    )) {
        if ($null -eq $item.process) { continue }
        if (-not (Test-ProcessAlive $item.process)) { continue }
        try {
            # Only ever our captured PIDs; never a pre-existing inkscape/launcher.
            if ([int]$item.process.Id -ne [int]$item.pid) { continue }
            $item.process.Kill()
            $killed += ($item.name + ':' + $item.pid)
        } catch {
            $state.warnings += ('Could not kill owned ' + $item.name + ' process ' + $item.pid + ': ' + $_.Exception.Message)
        }
    }
    return $killed
}

# ---------------------------------------------------------------------------
# New run directory, isolated profile and fixture copy.
# ---------------------------------------------------------------------------
$outputFull = Resolve-FullPath $OutputDirectory
if (Test-Path -LiteralPath $outputFull) { throw 'OutputDirectory must be a NEW directory; use a fresh path for each isolated run.' }
$null = New-Item -ItemType Directory -Path $outputFull -ErrorAction Stop
$outputCreated = $true
$summaryPath = Join-Path $outputFull 'run-summary.json'

try {
    # -----------------------------------------------------------------------
    # Opt-in maximized-workarea calibration gate (BEFORE any GUI launch/input).
    # Both parameters together or both empty. The pinned file is hashed and
    # strictly validated; a fresh in-run measurement is never the expected one.
    # -----------------------------------------------------------------------
    $maximizedMode = $false
    $maxCal = $null
    $hasCalPath = -not [string]::IsNullOrWhiteSpace($MaximizedCalibrationPath)
    $hasCalSha = -not [string]::IsNullOrWhiteSpace($ExpectedMaximizedCalibrationSha256)
    if ($hasCalPath -xor $hasCalSha) {
        throw 'MaximizedCalibrationPath and ExpectedMaximizedCalibrationSha256 must be supplied together or both left empty.'
    }
    if ($hasCalPath) {
        if ($Workload -ne 'object-drag') { throw 'MaximizedCalibrationPath is object-drag only.' }
        $calRead = Read-TrialMaximizedCalibration -Path $MaximizedCalibrationPath -ExpectedSha256 $ExpectedMaximizedCalibrationSha256
        $state.maximizedCalibration = [ordered]@{
            provided = $true; requested = $true; mode = 'maximized-workarea'
            path = $MaximizedCalibrationPath
            expectedSha256 = ([string]$ExpectedMaximizedCalibrationSha256).ToLowerInvariant()
            actualSha256 = $calRead.actualSha256
            schema = $calRead.schema
            calibration = $calRead.calibration
            errors = @($calRead.errors)
        }
        if (-not $calRead.ok) {
            throw ('MaximizedCalibrationPath rejected before launch: ' + $calRead.reason)
        }
        $maximizedMode = $true
        $maxCal = $calRead.calibration
        $state.maximizedCalibration.path = $calRead.calibration.source_path
    }

    # -----------------------------------------------------------------------
    # Optional screen-observer hook gate (BEFORE any GUI launch). All four
    # observer parameters together or all empty; forwarded to the pan helper,
    # which re-validates the exe hash + ROI config and pins the ROI to the same
    # maximized calibration monitor/work geometry before any input.
    # -----------------------------------------------------------------------
    $observerRequested = $false
    $hasObsExe = -not [string]::IsNullOrWhiteSpace($ObserverExePath)
    $hasObsExeSha = -not [string]::IsNullOrWhiteSpace($ExpectedObserverExeSha256)
    $hasObsCfg = -not [string]::IsNullOrWhiteSpace($ObserverRoiConfigPath)
    $hasObsCfgSha = -not [string]::IsNullOrWhiteSpace($ExpectedObserverRoiConfigSha256)
    $obsCount = @(@($hasObsExe, $hasObsExeSha, $hasObsCfg, $hasObsCfgSha) | Where-Object { $_ -eq $true }).Count
    if ($obsCount -gt 0 -and $obsCount -lt 4) {
        throw 'ObserverExePath, ExpectedObserverExeSha256, ObserverRoiConfigPath and ExpectedObserverRoiConfigSha256 must be supplied together or all left empty.'
    }
    if ($obsCount -eq 4) {
        if ($Workload -ne 'object-drag') { throw 'Observer hook is object-drag only.' }
        if (-not $maximizedMode) { throw 'Observer hook requires the maximized-workarea calibration.' }
        # Forward absolute paths so the helper resolves them independently of
        # its own working directory.
        $ObserverExePath = Resolve-FullPath $ObserverExePath
        $ObserverRoiConfigPath = Resolve-FullPath $ObserverRoiConfigPath
        $observerRequested = $true
    }
    $state.observer = [ordered]@{
        requested = $observerRequested
        exePath = if ($observerRequested) { $ObserverExePath } else { $null }
        expectedExeSha256 = if ($observerRequested) { ([string]$ExpectedObserverExeSha256).ToLowerInvariant() } else { $null }
        roiConfigPath = if ($observerRequested) { $ObserverRoiConfigPath } else { $null }
        expectedRoiConfigSha256 = if ($observerRequested) { ([string]$ExpectedObserverRoiConfigSha256).ToLowerInvariant() } else { $null }
    }

    $profileDir = Join-Path $outputFull 'profile'
    $null = New-Item -ItemType Directory -Path $profileDir
    $null = New-Item -ItemType Directory -Path (Join-Path $profileDir 'Cache')
    $fixtureCopy = Join-Path $outputFull 'fixture-simple.svg'
    Copy-Item -LiteralPath $fixtureSource -Destination $fixtureCopy
    $preferencesPath = Join-Path $profileDir 'preferences.xml'
    $preferencesXml = Get-TrialPreferencesXml $Workload
    [IO.File]::WriteAllText($preferencesPath, $preferencesXml, [System.Text.UTF8Encoding]::new($false))

    $csvPath = Join-Path $outputFull 'rendering.csv'
    $stdoutPath = Join-Path $outputFull 'app-stdout.log'
    $stderrPath = Join-Path $outputFull 'app-stderr.log'
    $panDir = Join-Path $outputFull 'pan'

    $state.output = [ordered]@{
        directory = $outputFull; summaryPath = $summaryPath; csvPath = $csvPath
        stdoutLog = $stdoutPath; stderrLog = $stderrPath; panDirectory = $panDir
    }
    $state.profile = [ordered]@{
        directory = $profileDir; preferencesPath = $preferencesPath
        preferences = [ordered]@{
            show_diagnostics = 1; request_opengl = 0; windows_accelerated = 0
            savewindowgeometry = 2
            desktop_geometry = if ($maximizedMode) {
                [ordered]@{
                    width = 1280; height = 800; maximized = 1; fullscreen = 0
                    requested_maximized = $true; window_mode = 'maximized-workarea'
                    expected_client_width = [int](Get-TrialCalValue $maxCal 'client_width')
                    expected_client_height = [int](Get-TrialCalValue $maxCal 'client_height')
                    expected_dpi = [int](Get-TrialCalValue $maxCal 'dpi')
                    expected_monitor_bounds = @(Get-TrialCalValue $maxCal 'monitor_bounds')
                    expected_work_bounds = @(Get-TrialCalValue $maxCal 'work_bounds')
                }
            } else {
                [ordered]@{ width = 1280; height = 800; maximized = 0; fullscreen = 0 }
            }
            workload = $Workload
            window_mode = if ($maximizedMode) { 'maximized-workarea' } else { 'normal-client' }
            live_content_selector = if ($Workload -eq 'object-drag') { '/tools/select/show=content' } else { $null }
            snapping_disabled = if ($Workload -eq 'object-drag') { '/options/snapping/snap-global-toggle=0' } else { $null }
        }
        isolated = $true; note = 'Fresh profile; the installed/user VA Studio profile is never used.'
    }
    $state.fixture = [ordered]@{
        source = $fixtureSource; copy = $fixtureCopy
        sha256 = (Get-FileSha256 $fixtureCopy)
        sizeBytes = (Get-Item -LiteralPath $fixtureCopy).Length
        workload = $Workload
    }
    if ($Workload -eq 'object-drag') {
        if ([string]::IsNullOrWhiteSpace($ExpectedDragFixtureSha256)) {
            throw 'Workload=object-drag requires -ExpectedDragFixtureSha256 (the pinned drag fixture hash) before any GUI launch.'
        }
        $dragFixture = Test-DragFixture -Path $fixtureCopy -ExpectedSha256 $ExpectedDragFixtureSha256 -Label 'isolated drag'
        $state.fixture.dragContract = $dragFixture
        if (@($dragFixture.errors).Count -gt 0) {
            throw ('Drag fixture contract failed before GUI launch: ' + (@($dragFixture.errors) -join ' | '))
        }
    }

    # -----------------------------------------------------------------------
    # Process-local environment: save the exact diagnostic contract.
    # -----------------------------------------------------------------------
    $managedEnv = @(
        'GSK_RENDERER', 'GDK_DISABLE', 'GDK_BACKEND', 'GDK_DEBUG',
        'GDK_WIN32_FORCE_DCOMP', 'GDK_WIN32_CAIRO_GDI_BUFFER',
        'VACARDS_REDRAW_DEADLINE', 'VACARDS_DOCUMENT_DEADLINE',
        'VACARDS_TEST_PROFILE_DIR', 'VACARDS_RENDER_STATS_LOG',
        'VACARDS_FRAME_TIMING', 'VACARDS_INPUT_LATENCY'
    )
    $savedEnv = [ordered]@{}
    $previousEnv = [ordered]@{}
    foreach ($name in $managedEnv) {
        $value = [Environment]::GetEnvironmentVariable($name, 'Process')
        $savedEnv[$name] = $value
        $previousEnv[$name] = if ($null -eq $value) { '<unset>' } else { [string]$value }
    }
    $gdiControl = Get-GdiBufferControl $GdiBufferMode
    $appliedEnv = Get-TrialAppliedEnvironment -GdiControl $gdiControl -ProfileDirectory $profileDir -RenderStatsLog $csvPath
    $state.environment = [ordered]@{
        requested = $appliedEnv
        gdiBufferControl = [ordered]@{
            parameter          = 'GdiBufferMode'
            requestedValue     = $GdiBufferMode
            controlLabel       = $gdiControl.controlLabel
            envVariable        = $gdiControl.envVariable
            requestedEnvValue  = $gdiControl.envLiteral
            effectiveEnvValue  = $appliedEnv['GDK_WIN32_CAIRO_GDI_BUFFER']
            legacyDisabledValue = '0'
            implementationConfirmed = $false
            ambientValueObserved = $previousEnv['GDK_WIN32_CAIRO_GDI_BUFFER']
            note = 'Records the REQUESTED environment setting only. It does not confirm the patched GTK buffer path was used: a stock DLL ignores this variable, and the effective env value alone is not evidence of behavior. Process-scoped, copied into the owned child PSI, never inherited, and restored in finally.'
        }
        previousProcessEnvironment = $previousEnv
        note = 'Process-scoped only; original values restored in finally. User profile and installed app are never used.'
    }

    $tag = 'vatrial' + [guid]::NewGuid().ToString('N')
    $launchStartedUtc = [DateTime]::UtcNow

    # Pinned app identities (also recorded into the summary on mismatch).
    $actualHashes = [ordered]@{
        'VACards-Test.exe'   = Get-FileSha256 $requiredFiles['VACards-Test.exe']
        'bin/inkscape.exe'   = Get-FileSha256 $requiredFiles['bin/inkscape.exe']
        'bin/libgtk-4-1.dll' = Get-FileSha256 $requiredFiles['bin/libgtk-4-1.dll']
        'bin/libcairo-2.dll' = Get-FileSha256 $requiredFiles['bin/libcairo-2.dll']
    }
    $expectedHashes = [ordered]@{
        exe   = $ExpectedExeSha256.ToLowerInvariant()
        gtk   = $ExpectedGtkSha256.ToLowerInvariant()
        cairo = $ExpectedCairoSha256.ToLowerInvariant()
    }
    $pinnedMismatches = @()
    if ($actualHashes['bin/inkscape.exe']   -ne $expectedHashes.exe)   { $pinnedMismatches += 'bin/inkscape.exe' }
    if ($actualHashes['bin/libgtk-4-1.dll'] -ne $expectedHashes.gtk)   { $pinnedMismatches += 'bin/libgtk-4-1.dll' }
    if ($actualHashes['bin/libcairo-2.dll'] -ne $expectedHashes.cairo) { $pinnedMismatches += 'bin/libcairo-2.dll' }
    $state.app = [ordered]@{
        appRoot        = $appRootFull
        expectedHashes = $expectedHashes
        actualHashes   = $actualHashes
        pinnedMismatches = @($pinnedMismatches)
        hashComparisons = [ordered]@{
            exe   = ($actualHashes['bin/inkscape.exe']   -eq $expectedHashes.exe)
            gtk   = ($actualHashes['bin/libgtk-4-1.dll'] -eq $expectedHashes.gtk)
            cairo = ($actualHashes['bin/libcairo-2.dll'] -eq $expectedHashes.cairo)
        }
    }
    if ($pinnedMismatches.Count -gt 0) {
        throw ('Pinned app hash mismatch for: ' + ($pinnedMismatches -join ', ') + '; refusing this AppRoot.')
    }

    # Pan helper identity (separate deliverable). Prepared interface, fail closed.
    $panHelperFull = Resolve-FullPath $PanHelperPath
    $panHelperDeadline = [DateTime]::UtcNow.AddSeconds($PanHelperWaitSeconds)
    while (-not (Test-Path -LiteralPath $panHelperFull -PathType Leaf) -and [DateTime]::UtcNow -lt $panHelperDeadline) {
        Start-Sleep -Milliseconds 500
    }
    if (-not (Test-Path -LiteralPath $panHelperFull -PathType Leaf)) {
        throw ('Pan helper not found: ' + $panHelperFull + '. The deterministic pan helper is a separate deliverable ' +
               '(interface: TargetProcessId, OutputDirectory, ClientWidth, ClientHeight, Loops, StepsPerLeg, StepIntervalMs). ' +
               'This runner does not implement input interop; rerun after the helper lands.')
    }
    $panHelperActualHash = Get-FileSha256 $panHelperFull
    $state.panHelper = [ordered]@{
        path               = $panHelperFull
        expectedSha256     = $ExpectedPanHelperSha256.ToLowerInvariant()
        actualSha256       = $panHelperActualHash
        hashMatches        = ($panHelperActualHash -eq $ExpectedPanHelperSha256.ToLowerInvariant())
        interface          = [ordered]@{
            TargetProcessId = '<owned child pid>'; OutputDirectory = '<new output directory>'
            ClientWidth     = $PanClientWidth; ClientHeight = $PanClientHeight
            Loops           = $PanLoops; StepsPerLeg = $PanStepsPerLeg; StepIntervalMs = $PanStepIntervalMs
        }
    }
    if (-not $state.panHelper.hashMatches) {
        throw ('Pinned pan helper hash mismatch for ' + $panHelperFull + '; refusing this helper.')
    }

    # Apply the process-local environment contract (also mirrored into the
    # launcher's PSI below; the control-derived GDI buffer value is included).
    Set-ManagedProcessEnvironment $appliedEnv

    Write-Host 'VA Studio fixed default-Cairo app trial (diagnostic; no PASS).'
    Write-Host ('AppRoot    : ' + $appRootFull)
    Write-Host ('Output     : ' + $outputFull)
    Write-Host ('Tag        : ' + $tag)
    Write-Host ('exe sha256 : ' + $actualHashes['bin/inkscape.exe'])
    Write-Host ('gtk sha256 : ' + $actualHashes['bin/libgtk-4-1.dll'])
    Write-Host ('cairo      : ' + $actualHashes['bin/libcairo-2.dll'])

    # -----------------------------------------------------------------------
    # Owned launch with retained handle + asynchronous stdout/stderr drain.
    # -----------------------------------------------------------------------
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $requiredFiles['VACards-Test.exe']
    $psi.Arguments = '--app-id-tag=' + $tag + ' "' + $fixtureCopy + '"'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $false
    $psi.WorkingDirectory = $appRootFull
    foreach ($key in $appliedEnv.Keys) {
        if ($appliedEnv[$key] -eq '<cleared>') { $null = $psi.EnvironmentVariables.Remove($key) }
        else { $psi.EnvironmentVariables[$key] = [string]$appliedEnv[$key] }
    }
    $launcherProcess = New-Object System.Diagnostics.Process
    $launcherProcess.StartInfo = $psi
    $launchStartedQpc = [System.Diagnostics.Stopwatch]::GetTimestamp()
    if (-not $launcherProcess.Start()) { throw 'Could not start VACards-Test.exe.' }
    $launcherPid = [int]$launcherProcess.Id
    $stdoutStream = [IO.File]::Create($stdoutPath)
    $stderrStream = [IO.File]::Create($stderrPath)
    $stdoutTask = $launcherProcess.StandardOutput.BaseStream.CopyToAsync($stdoutStream)
    $stderrTask = $launcherProcess.StandardError.BaseStream.CopyToAsync($stderrStream)
    $state.launch = [ordered]@{
        appIdTag = $tag; launcherPath = $requiredFiles['VACards-Test.exe']
        launcherProcessId = $launcherPid; appProcessId = $null
        launchStartedUtc = $launchStartedUtc.ToString('o'); launchStartedQpc = [int64]$launchStartedQpc
        childCommandLine = $null; childParentProcessId = $null; childName = $null
    }

    # Discover the owned child process by parent PID + name + app-id tag.
    $cimFailed = $false
    $launchDeadline = [DateTime]::UtcNow.AddSeconds($LaunchTimeoutSeconds)
    while ($null -eq $appProcess -and [DateTime]::UtcNow -lt $launchDeadline) {
        if ($launcherProcess.HasExited) { break }
        Start-Sleep -Milliseconds 250
        try {
            $children = @(Get-CimInstance -ClassName Win32_Process -Filter ('ParentProcessId=' + $launcherPid) -ErrorAction Stop)
        } catch { $cimFailed = $true; $children = @() }
        foreach ($child in $children) {
            $childName = [string](Get-JsonProperty $child 'Name')
            $childCommand = [string](Get-JsonProperty $child 'CommandLine')
            if ($childName -ieq 'inkscape.exe' -and $childCommand -and $childCommand.Contains($tag)) {
                $candidate = Get-Process -Id ([int](Get-JsonProperty $child 'ProcessId')) -ErrorAction SilentlyContinue
                if ($null -ne $candidate) {
                    $appProcess = $candidate
                    $appPid = [int]$candidate.Id
                    $state.launch.childCommandLine = $childCommand
                    $state.launch.childParentProcessId = [int](Get-JsonProperty $child 'ParentProcessId')
                    $state.launch.childName = $childName
                    $state.launch.childExecutablePath = [string](Get-JsonProperty $child 'ExecutablePath')
                    break
                }
            }
        }
    }
    if ($null -eq $appProcess) {
        # No fallback adoption: an unmatched recent inkscape process could belong
        # to the user or the installed app, and we must never attach to or kill it.
        $discoveryNote = if ($cimFailed) { 'Win32_Process child enumeration failed' } else { 'no owned inkscape.exe child matched the unique tag' }
        throw ('The owned inkscape.exe process did not appear (' + $discoveryNote + '). Inspect app-stdout.log/app-stderr.log; nothing was captured. No unrelated process was adopted.')
    }
    $state.launch.appProcessId = $appPid

    # Gate: actual child command line must carry this run's unique tag and parent.
    $childCommandLine = [string]$state.launch.childCommandLine
    if ([string]::IsNullOrEmpty($childCommandLine)) {
        try {
            $liveChild = Get-CimInstance -ClassName Win32_Process -Filter ('ProcessId=' + $appPid) -ErrorAction Stop
            $childCommandLine = [string](Get-JsonProperty $liveChild 'CommandLine')
            $state.launch.childCommandLine = $childCommandLine
        } catch { $state.warnings += 'Child command line unavailable for verification.' }
    }
    if ([string]::IsNullOrEmpty($childCommandLine) -or -not $childCommandLine.Contains($tag)) {
        Add-ValidationError 'Owned child command line does not contain the unique --app-id-tag.'
    }
    if ($null -ne $state.launch.childParentProcessId -and [int]$state.launch.childParentProcessId -ne $launcherPid) {
        Add-ValidationError 'Owned child parent process is not the retained launcher process.'
    }

    # -----------------------------------------------------------------------
    # Bounded wait for the owned main window, then a settle interval.
    # -----------------------------------------------------------------------
    $windowDeadline = [DateTime]::UtcNow.AddSeconds($LaunchTimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 250
        try { $appProcess.Refresh() } catch {}
    } while ($appProcess.MainWindowHandle -eq 0 -and (Test-ProcessAlive $appProcess) -and [DateTime]::UtcNow -lt $windowDeadline)
    if (-not (Test-ProcessAlive $appProcess)) { throw 'The application exited before its window appeared.' }
    if ($appProcess.MainWindowHandle -eq 0) { throw 'The application window did not appear within the timeout.' }
    $windowReadyUtc = [DateTime]::UtcNow
    $state.window = [ordered]@{
        mainWindowHandle = $appProcess.MainWindowHandle.ToInt64()
        title = [string]$appProcess.MainWindowTitle
        readyUtc = $windowReadyUtc.ToString('o')
        readyWaitMs = [int]($windowReadyUtc - $launchStartedUtc).TotalMilliseconds
        settleSeconds = $SettleSeconds
    }
    if ($SettleSeconds -gt 0) { Start-Sleep -Seconds $SettleSeconds }

    # -----------------------------------------------------------------------
    # Loaded-module verification against the pinned AppRoot.
    # -----------------------------------------------------------------------
    $moduleInfo = [ordered]@{ mainModule = $null; gtk = $null; cairo = $null; glib = $null; count = $null; unavailable = $false; errors = @() }
    $moduleErrors = New-Object System.Collections.Generic.List[string]
    try {
        $modules = @($appProcess.Modules)
        $moduleInfo.count = $modules.Count
        foreach ($module in $modules) {
            $name = [string]$module.ModuleName
            if ($name -ieq 'inkscape.exe') {
                $entry = [ordered]@{ name = $name; fileName = [string]$module.FileName; sha256 = $null; insideAppRoot = $false; matchesExpected = $false }
                try { $entry.sha256 = Get-FileSha256 $entry.fileName } catch { $moduleErrors.Add('could not hash main module') }
                $entry.insideAppRoot = Test-PathInside $entry.fileName $appRootFull
                $entry.matchesExpected = ($entry.sha256 -eq $expectedHashes.exe)
                $moduleInfo.mainModule = $entry
            } elseif ($name -ieq 'libgtk-4-1.dll' -or $name -ieq 'libcairo-2.dll') {
                $entry = [ordered]@{ name = $name; fileName = [string]$module.FileName; sha256 = $null; insideAppRoot = $false; matchesExpected = $false }
                try { $entry.sha256 = Get-FileSha256 $entry.fileName } catch { $moduleErrors.Add('could not hash ' + $name) }
                $entry.insideAppRoot = Test-PathInside $entry.fileName $appRootFull
                $entry.matchesExpected = if ($name -ieq 'libgtk-4-1.dll') { $entry.sha256 -eq $expectedHashes.gtk } else { $entry.sha256 -eq $expectedHashes.cairo }
                if ($name -ieq 'libgtk-4-1.dll') { $moduleInfo.gtk = $entry } else { $moduleInfo.cairo = $entry }
            } elseif ($name -ieq 'libglib-2.0-0.dll') {
                # Exact GLib the app loaded: its path/hash are passed to the pan
                # helper for the monotonic-clock bracket contract. No other arm
                # and no global PATH is ever used to load it.
                $entry = [ordered]@{ name = $name; fileName = [string]$module.FileName; sha256 = $null; insideAppRoot = $false; matchesExpected = $null }
                try { $entry.sha256 = Get-FileSha256 $entry.fileName } catch { $moduleErrors.Add('could not hash libglib-2.0-0.dll') }
                $entry.insideAppRoot = Test-PathInside $entry.fileName $appRootFull
                $moduleInfo.glib = $entry
            }
        }
    } catch {
        $moduleInfo.unavailable = $true
        $moduleErrors.Add('loaded module enumeration failed: ' + $_.Exception.Message)
    }
    if ($null -eq $moduleInfo.mainModule) { $moduleErrors.Add('loaded main module inkscape.exe not found') }
    if ($null -eq $moduleInfo.gtk) { $moduleErrors.Add('loaded libgtk-4-1.dll not found') }
    if ($null -eq $moduleInfo.cairo) { $moduleErrors.Add('loaded libcairo-2.dll not found') }
    if ($null -eq $moduleInfo.glib) { $moduleErrors.Add('loaded libglib-2.0-0.dll not found; the GLib clock contract cannot be satisfied') }
    foreach ($key in @('mainModule', 'gtk', 'cairo')) {
        $entry = $moduleInfo[$key]
        if ($null -eq $entry) { continue }
        if (-not $entry.insideAppRoot) { $moduleErrors.Add(($entry.name + ' was loaded from outside AppRoot: ' + $entry.fileName)) }
        if (-not $entry.matchesExpected) { $moduleErrors.Add(($entry.name + ' loaded-module hash does not match the pinned hash')) }
    }
    if ($null -ne $moduleInfo.glib) {
        if (-not $moduleInfo.glib.insideAppRoot) { $moduleErrors.Add('libglib-2.0-0.dll was loaded from outside AppRoot: ' + $moduleInfo.glib.fileName) }
        if ([string]::IsNullOrEmpty($moduleInfo.glib.sha256)) { $moduleErrors.Add('libglib-2.0-0.dll loaded-module hash is unavailable') }
    }
    $moduleInfo.errors = @($moduleErrors)
    $state.loadedModules = $moduleInfo
    if ($moduleErrors.Count -gt 0) { foreach ($e in $moduleErrors) { Add-ValidationError $e } }

    # -----------------------------------------------------------------------
    # Single pan-helper invocation in its own retained PowerShell child. This
    # runner never synthesises input and never executes the helper in-process.
    # No -ExecutionPolicy argument is ever passed.
    # -----------------------------------------------------------------------
    $panBeforeUtc = [DateTime]::UtcNow
    $panBeforeQpc = [System.Diagnostics.Stopwatch]::GetTimestamp()
    $panExitCode = $null
    $panError = $null
    $panTimedOut = $false
    $panProcess = $null
    $panStdoutPath = Join-Path $outputFull 'pan-helper-stdout.log'
    $panStderrPath = Join-Path $outputFull 'pan-helper-stderr.log'
    $panStdoutStream = $null
    $panStderrStream = $null
    $panStdoutTask = $null
    $panStderrTask = $null
    $panSummaryPath = Join-Path $panDir 'input-summary.json'
    $panSummary = $null
    $panValidation = $null
    $phaseRange = $null
    $glibPath = $null
    $glibSha = $null
    if ($null -ne $moduleInfo.glib) {
        $glibPath = [string]$moduleInfo.glib.fileName
        $glibSha = [string]$moduleInfo.glib.sha256
    }
    if (-not (Test-Path -LiteralPath $panDir)) {
        if ([string]::IsNullOrEmpty($glibPath) -or [string]::IsNullOrEmpty($glibSha)) {
            $panError = 'loaded libglib-2.0-0.dll path/hash unavailable; refusing the pan helper because the clock contract cannot be satisfied'
            Add-ValidationError $panError
        } else {
            try {
                $psExe = Join-Path $PSHOME 'powershell.exe'
                if (-not (Test-Path -LiteralPath $psExe -PathType Leaf)) {
                    $psExe = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
                }
                $panArgList = @(
                    '-NoProfile', '-NonInteractive', '-File', $panHelperFull,
                    '-Workload', $Workload,
                    '-TargetProcessId', [string]$appPid,
                    '-OutputDirectory', $panDir,
                    '-ClientWidth', [string]$PanClientWidth,
                    '-ClientHeight', [string]$PanClientHeight,
                    '-Loops', [string]$PanLoops,
                    '-StepsPerLeg', [string]$PanStepsPerLeg,
                    '-StepIntervalMs', [string]$PanStepIntervalMs,
                    '-GlibDllPath', $glibPath,
                    '-ExpectedGlibSha256', $glibSha,
                    # This runner drives the pinned synthetic fixture only; for
                    # that fixture the GTK client-side titlebar exposes no native
                    # HTCAPTION, so opt in to the single verified CSD titlebar point.
                    '-AllowVerifiedCsdCaption'
                )
                if ($Workload -eq 'object-drag') {
                    # The helper saves baseline/endpoint/returned.svg under panDir
                    # from the actual isolated fixture copy opened in the GUI.
                    $panArgList += @('-DocumentPath', $fixtureCopy)
                }
                if ($maximizedMode) {
                    # Forward the SAME calibration path/hash to the helper; it
                    # re-validates before any input.
                    $panArgList += @(
                        '-MaximizedCalibrationPath', [string](Get-TrialCalValue $maxCal 'source_path'),
                        '-ExpectedMaximizedCalibrationSha256', ([string]$ExpectedMaximizedCalibrationSha256).ToLowerInvariant()
                    )
                }
                if ($observerRequested) {
                    # Forward the SAME observer exe + ROI config path/hashes; the
                    # helper re-validates and pins them before any input.
                    $panArgList += @(
                        '-ObserverExePath', $ObserverExePath,
                        '-ExpectedObserverExeSha256', ([string]$ExpectedObserverExeSha256).ToLowerInvariant(),
                        '-ObserverRoiConfigPath', $ObserverRoiConfigPath,
                        '-ExpectedObserverRoiConfigSha256', ([string]$ExpectedObserverRoiConfigSha256).ToLowerInvariant()
                    )
                }
                $panQuoted = @($panArgList | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } })
                $panPsi = New-Object System.Diagnostics.ProcessStartInfo
                $panPsi.FileName = $psExe
                $panPsi.Arguments = ($panQuoted -join ' ')
                $panPsi.UseShellExecute = $false
                $panPsi.RedirectStandardOutput = $true
                $panPsi.RedirectStandardError = $true
                $panPsi.CreateNoWindow = $true
                $panPsi.WorkingDirectory = $appRootFull
                $panProcess = New-Object System.Diagnostics.Process
                $panProcess.StartInfo = $panPsi
                if (-not $panProcess.Start()) { throw 'could not start the pan helper PowerShell child' }
                $panStdoutStream = [IO.File]::Create($panStdoutPath)
                $panStderrStream = [IO.File]::Create($panStderrPath)
                $panStdoutTask = $panProcess.StandardOutput.BaseStream.CopyToAsync($panStdoutStream)
                $panStderrTask = $panProcess.StandardError.BaseStream.CopyToAsync($panStderrStream)
                $panDeadline = [DateTime]::UtcNow.AddSeconds($PanHelperTimeoutSeconds)
                while (-not $panProcess.HasExited -and [DateTime]::UtcNow -lt $panDeadline) { Start-Sleep -Milliseconds 200 }
                if (-not $panProcess.HasExited) {
                    # Kill only this owned helper child; the trial is invalid.
                    $panTimedOut = $true
                    $panError = 'pan helper child exceeded the ' + $PanHelperTimeoutSeconds + 's bound'
                    try { $panProcess.Kill() } catch {}
                    try { [void]$panProcess.WaitForExit(5000) } catch {}
                    Add-ValidationError $panError
                } else {
                    $panExitCode = [int]$panProcess.ExitCode
                }
            } catch {
                $panError = $_.Exception.Message
                if ($null -eq $panExitCode) { $panExitCode = 99 }
            } finally {
                try { [System.Threading.Tasks.Task]::WaitAll(@($panStdoutTask, $panStderrTask), 8000) | Out-Null } catch {}
                foreach ($stream in @($panStdoutStream, $panStderrStream)) {
                    if ($null -ne $stream) { try { $stream.Flush(); $stream.Dispose() } catch {} }
                }
            }
        }
    } else {
        $panError = 'pan output directory unexpectedly already exists'
        $panExitCode = 98
    }
    $panAfterUtc = [DateTime]::UtcNow
    $panAfterQpc = [System.Diagnostics.Stopwatch]::GetTimestamp()

    # Enforce the helper's own summary. Exit code alone is never sufficient and
    # injected events are never claimed as delivered.
    if (Test-Path -LiteralPath $panSummaryPath -PathType Leaf) {
        try { $panSummary = (Get-Content -Raw -LiteralPath $panSummaryPath | ConvertFrom-Json) } catch { $panSummary = $null }
    }
    $expectedPanEvents = if ($Workload -eq 'object-drag') { $DragExpectedEventCount } else { 1 + ($PanLoops * ((2 * $PanStepsPerLeg) + 2)) }
    $panWindowMode = if ($maximizedMode) { 'maximized-workarea' } else { 'normal-client' }
    $expectedCanvasScale = if ($maximizedMode) { [double](Get-TrialCalValue $maxCal 'canvas_scale') } else { 1.0 }
    $oracleFactor = if ($maximizedMode) { [double](Get-TrialCalValue $maxCal 'physical_pixels_per_logical_pixel') } else { 1.0 }
    $state.workload = [ordered]@{
        mode = $Workload
        windowMode = $panWindowMode
        requestedMaximized = [bool]$maximizedMode
        protocolIdentity = if ($Workload -eq 'object-drag') { $DragProtocolIdentity } else { 'pan-middle-button-v1' }
        expectedGestureEventCount = $expectedPanEvents
        expectedCanvasScale = $expectedCanvasScale
        physicalPixelsPerLogicalPixel = if ($maximizedMode) { $oracleFactor } else { $null }
        documentPath = if ($Workload -eq 'object-drag') { $fixtureCopy } else { $null }
        targetId = if ($Workload -eq 'object-drag') { $DragTargetId } else { $null }
        dragFixtureSha256Expected = if ($Workload -eq 'object-drag') { $ExpectedDragFixtureSha256.ToLowerInvariant() } else { $null }
        preferenceSupport = if ($Workload -eq 'object-drag') {
            'live-content /tools/select/show and /options/snapping/snap-global-toggle verified in current source (inkscape-preferences.cpp:983, actions-canvas-snapping.cpp:114,140) and in measured app source f137dd39; the pinned ExpectedExeSha256 remains authoritative, and an unsupported preference would surface as a failed drag-oracle geometry check, never a pass.'
        } else { $null }
    }
    $expectedHwnd = 0L
    if ($null -ne $state.window -and $state.window.Contains('mainWindowHandle')) { $expectedHwnd = [long]$state.window.mainWindowHandle }
    $expectedProtocol = if ($Workload -eq 'object-drag') { $DragProtocolIdentity } else { '' }
    $panValidation = Test-PanSummary -Summary $panSummary -ExpectedEventCount $expectedPanEvents -ExpectedProtocolIdentity $expectedProtocol -AppPid $appPid -ExpectedHwnd $expectedHwnd -ClientWidth $PanClientWidth -ClientHeight $PanClientHeight -ConsoleSession $RequiredConsoleSession -ExpectedWindowMode $panWindowMode -MaximizedCalibration $maxCal -ObserverRequested:$observerRequested
    $phaseRange = Get-PanPhaseRange $panSummary
    if ($null -ne $panExitCode -and $panExitCode -ne 0) {
        $panSuffix = if ($panError) { ' (' + $panError + ')' } else { '' }
        Add-ValidationError ('Pan helper exit code was ' + $panExitCode + $panSuffix)
    }
    foreach ($e in @($panValidation.errors)) { Add-ValidationError ('pan helper summary: ' + $e) }
    foreach ($e in @($phaseRange.errors)) { Add-ValidationError ('pan helper phase range: ' + $e) }

    $panArtifacts = Write-PanJsonArtifacts $panDir
    $state.panHelper.exitCode = $panExitCode
    $state.panHelper.timedOut = $panTimedOut
    $state.panHelper.error = $panError
    $state.panHelper.childProcessId = if ($null -ne $panProcess) { $panProcess.Id } else { $null }
    $state.panHelper.stdoutLog = $panStdoutPath
    $state.panHelper.stderrLog = $panStderrPath
    $state.panHelper.inputSummaryPath = $panSummaryPath
    $state.panHelper.expectedEventCount = $expectedPanEvents
    $state.panHelper.injectedIsNotDelivered = $true
    $state.panHelper.validation = $panValidation
    $state.panHelper.observer = if ($null -ne $panSummary) { Get-JsonProperty $panSummary 'observer' } else { $null }
    if ($maximizedMode) {
        $state.maximizedCalibration.observedMode = $panValidation.windowMode
        $state.maximizedCalibration.observedClient = @($panValidation.clientAfter)
        $state.maximizedCalibration.observedDpi = $panValidation.dpi
        $state.maximizedCalibration.observedMaximized = $panValidation.maximizedProofOk
    }
    $state.panHelper.phaseRange = $phaseRange
    $state.panHelper.glibDllPath = $glibPath
    $state.panHelper.glibSha256 = $glibSha
    $state.panHelper.timeoutSeconds = $PanHelperTimeoutSeconds
    $state.panHelper.invokedAtUtc = $panBeforeUtc.ToString('o')
    $state.panHelper.invokedAtQpc = [int64]$panBeforeQpc
    $state.panHelper.completedAtUtc = $panAfterUtc.ToString('o')
    $state.panHelper.completedAtQpc = [int64]$panAfterQpc
    $state.panHelper.durationMs = [int]($panAfterUtc - $panBeforeUtc).TotalMilliseconds
    $state.panHelper.output = $panArtifacts
    $state.panHelper.stdoutSha256 = if (Test-Path -LiteralPath $panStdoutPath -PathType Leaf) { Get-FileSha256 $panStdoutPath } else { $null }
    $state.panHelper.stderrSha256 = if (Test-Path -LiteralPath $panStderrPath -PathType Leaf) { Get-FileSha256 $panStderrPath } else { $null }
    $state.timing = [ordered]@{
        phaseBoundaries = if ($null -ne $panSummary) { Get-JsonProperty $panSummary 'phase_boundaries' } else { $panArtifacts.phaseBoundaries }
        phaseBoundarySource = 'pan helper input-summary.json explicit input_start/end QPC + UTC + qpc_frequency'
        phaseRange = $phaseRange
        clockBasis = if ($null -ne $panSummary) { Get-JsonProperty $panSummary 'clock_contract' } else { $null }
        stderrTimingScope = 'whole-trial'
        stderrTimingNote = 'Existing stderr timing lines are not timestamped; they are whole-trial only and are not attributed to phases or to a gesture p95.'
        stderrTimingLabels = @($StderrTimingLabels)
    }

    # -----------------------------------------------------------------------
    # Outside-GUI object-drag geometry oracle. It reads the helper's native
    # baseline/endpoint/returned.svg snapshots and is run OUTSIDE the measured
    # gesture window. Its errors invalidate the trial even when the telemetry
    # gates passed, and its full JSON is exposed in the run summary.
    # -----------------------------------------------------------------------
    $dragOutcome = $null
    if ($Workload -eq 'object-drag') {
        $oraclePath = if (-not [string]::IsNullOrWhiteSpace($DragOraclePath)) { Resolve-FullPath $DragOraclePath }
                      else { Join-Path $PSScriptRoot 'Test-AppDragOutcome.ps1' }
        $dragOutcome = Invoke-DragOutcomeOracle -PanDir $panDir -OraclePath $oraclePath -TimeoutSeconds $DragOracleTimeoutSeconds -MaximizedMode:$maximizedMode -PhysicalPixelsPerLogicalPixel $oracleFactor
        $state.workload.dragOutcomeStatus = $dragOutcome.status
        $state.workload.dragOutcomeValid = $dragOutcome.valid
        $state.workload.dragOraclePath = $oraclePath
        foreach ($e in @($dragOutcome.errors)) { Add-ValidationError ('drag oracle: ' + $e) }
    }
    $state.dragOutcome = $dragOutcome

    # Second settle.
    if ($SettleSeconds -gt 0) { Start-Sleep -Seconds $SettleSeconds }

    # -----------------------------------------------------------------------
    # Normal exit: WM_CLOSE / CloseMainWindow, then bounded wait for CSV flush.
    # -----------------------------------------------------------------------
    $closeRequestedUtc = [DateTime]::UtcNow
    $closeMethod = 'none'
    try { $appProcess.Refresh() } catch {}
    if ($appProcess.MainWindowHandle -ne 0) {
        $posted = $false
        try { $posted = [VACardsTrialNative]::PostMessageW($appProcess.MainWindowHandle, [VACardsTrialNative]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) } catch {}
        if ($posted) { $closeMethod = 'WM_CLOSE' }
    }
    if ($closeMethod -eq 'none') {
        try { if ($appProcess.CloseMainWindow()) { $closeMethod = 'CloseMainWindow' } } catch {}
    }
    if ($closeMethod -eq 'none') { Add-ValidationError 'Could not request a normal close (no owned main window / WM_CLOSE failed).' }

    $closeDeadline = [DateTime]::UtcNow.AddSeconds($CloseTimeoutSeconds)
    while ((Test-ProcessAlive $appProcess) -and [DateTime]::UtcNow -lt $closeDeadline) { Start-Sleep -Milliseconds 250 }
    $normalExit = -not (Test-ProcessAlive $appProcess)
    $closeWaitSeconds = [Math]::Round(([DateTime]::UtcNow - $closeRequestedUtc).TotalSeconds, 3)
    $appExit = Get-ProcessExitInfo $appProcess
    # The launcher returns the app's exit code and exits after the child; give it a moment.
    if ($normalExit -and (Test-ProcessAlive $launcherProcess)) {
        $launcherDeadline = [DateTime]::UtcNow.AddSeconds(10)
        while ((Test-ProcessAlive $launcherProcess) -and [DateTime]::UtcNow -lt $launcherDeadline) { Start-Sleep -Milliseconds 100 }
    }
    $launcherExit = Get-ProcessExitInfo $launcherProcess
    $launcherForceKilled = @()
    if ($normalExit -and (Test-ProcessAlive $launcherProcess)) {
        Add-ValidationError 'The retained launcher did not exit after its owned child; the trial lifecycle is incomplete.'
        $launcherForceKilled = @(Stop-OwnedProcesses)
        $launcherExit = Get-ProcessExitInfo $launcherProcess
    }
    $state.exit = [ordered]@{
        closeRequestedUtc = $closeRequestedUtc.ToString('o')
        closeMethod = $closeMethod
        normalExit = $normalExit
        closeWaitSeconds = $closeWaitSeconds
        configuredCloseTimeoutSeconds = $CloseTimeoutSeconds
        appHasExited = $appExit.hasExited; appExitCode = $appExit.exitCode
        launcherHasExited = $launcherExit.hasExited; launcherExitCode = $launcherExit.exitCode
        launcherForceKilled = @($launcherForceKilled)
        killOnlyOwnProcesses = $true; killedOwnProcesses = @()
    }
    if (-not $normalExit) {
        Add-ValidationError 'The application did not exit within the close timeout; partial logs preserved and the run is invalid.'
        # Owned-process-only cleanup on timeout; the run is already invalid.
        $state.exit.killedOwnProcesses = @(Stop-OwnedProcesses)
    }

    # Drain the retained launcher streams before reading the CSV.
    try { [System.Threading.Tasks.Task]::WaitAll(@($stdoutTask, $stderrTask), 8000) | Out-Null } catch {}
    foreach ($stream in @($stdoutStream, $stderrStream)) {
        if ($null -ne $stream) { try { $stream.Flush(); $stream.Dispose() } catch {} }
    }
    $stdoutStream = $null; $stderrStream = $null

    # -----------------------------------------------------------------------
    # Schema-4 CSV validation. No repair, no fabricated FPS.
    # -----------------------------------------------------------------------
    $state.csv = Test-TrialCsv -CsvPath $csvPath -PhaseRange $phaseRange -OutputDirectory $outputFull -ExpectedCanvasScale $expectedCanvasScale
    if ($state.csv.errors.Count -gt 0) { foreach ($e in $state.csv.errors) { Add-ValidationError $e } }
    if ($state.csv.present -and -not $normalExit) {
        Add-ValidationError 'CSV was read after a non-normal exit; buffered rows may be missing and the run is invalid.'
    }

    # Object-drag telemetry gate: at least one wholly-steady row with dragging=1
    # and positive button_motion. Boundary press/release intervals are legitimately
    # excluded, so an exact per-row button count is never required.
    $dragTelemetry = $null
    if ($Workload -eq 'object-drag') {
        $steadyPath = if ($state.csv.Contains('steadyCsvPath') -and $state.csv.steadyCsvPath) { $state.csv.steadyCsvPath }
                      else { Join-Path $outputFull 'steady-pan.csv' }
        $dragTelemetry = Test-DragSteadyTelemetry -SteadyCsvPath $steadyPath
        foreach ($e in @($dragTelemetry.errors)) { Add-ValidationError ('drag telemetry: ' + $e) }
    }
    $state.dragTelemetry = $dragTelemetry

    $state.trialStatus = if ($validationErrors.Count -eq 0) { 'valid' } else { 'invalid' }
    if ($validationErrors.Count -eq 0) { $state.failure = $null } else { $state.failure = ($validationErrors -join ' | ') }
    $state.validationErrors = @($validationErrors)
} catch {
    $state.trialStatus = 'failed'
    $state.failure = [string]$_.Exception.Message
    $state.validationErrors = @($validationErrors)
    # Safe cleanup: the processes are ours; the run will never be reported valid.
    $state.exit.normalExit = $false
    $state.exit.killedOwnProcesses = @(Stop-OwnedProcesses)
} finally {
    if ($state.trialStatus -eq 'initializing') {
        $state.trialStatus = 'failed'
        if (-not $state.failure) { $state.failure = 'Harness aborted before it could set a trial status.' }
    }
    # Restore process environment exactly (control variable included).
    if ($null -ne $savedEnv) {
        Restore-ManagedProcessEnvironment -Names $managedEnv -Saved $savedEnv
    }
    # Close drains/streams if an exception short-circuited the normal path.
    try { if ($stdoutTask) { [System.Threading.Tasks.Task]::WaitAll(@($stdoutTask, $stderrTask), 3000) | Out-Null } } catch {}
    foreach ($stream in @($stdoutStream, $stderrStream)) {
        if ($null -ne $stream) { try { $stream.Flush(); $stream.Dispose() } catch {} }
    }
    # Final safety net for owned processes that are somehow still alive.
    if ($state.trialStatus -ne 'valid') {
        $lateKills = @(Stop-OwnedProcesses)
        if ($lateKills.Count -gt 0 -and $state.exit) {
            $existing = @()
            if ($state.exit.Contains('killedOwnProcesses')) { $existing = @($state.exit.killedOwnProcesses) }
            $state.exit.killedOwnProcesses = @($existing + $lateKills)
        }
    }
    if ($outputCreated -and -not [string]::IsNullOrEmpty($summaryPath)) {
        $state.capturedUtc = [DateTime]::UtcNow.ToString('o')
        Write-RunSummary
        Write-Host ''
        Write-Host ('Trial status : ' + $state.trialStatus)
        Write-Host ('Summary      : ' + $summaryPath)
        if ($state.failure) { Write-Warning $state.failure }
    }
}

if ($state.trialStatus -ne 'valid') {
    throw ('App trial was not valid (' + $state.trialStatus + '): ' + $state.failure)
}
Write-Host 'App trial gates passed as valid. Diagnostic only: no PASS, no FPS.'
