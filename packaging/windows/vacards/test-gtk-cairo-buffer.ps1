# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
  Build and run the GTK4/GDK Windows Cairo-buffer pixel regression and compare
  a baseline GTK build against a candidate (patched) GTK build.

.DESCRIPTION
  Outcome-based driver for testfiles/windows/gtk-cairo-buffer-test.c.
  It builds the harness against a GTK prefix with pkg-config, verifies that the
  process loaded the exact expected libgtk/libcairo files (path + SHA-256), runs
  each requested case serially, and compares raw presented-pixel dumps
  byte-for-byte between the baseline and candidate builds. The harness
  self-asserts the exact pixel oracles; the driver adds baseline/candidate
  identity and completeness.

  Test configuration (recorded, not a product change):
    GSK_RENDERER=cairo   force the Cairo renderer path (GskCairoRenderer)
    GDK_DISABLE=dcomp    disable DirectComposition (GDK_FEATURE_DCOMP)
    GDK_BACKEND=win32
  GTK registers "dcomp" under GDK_DISABLE (gdk/gdk.c gdk_feature_keys), not
  under GDK_DEBUG; GDK_DEBUG is cleared and must not carry dcomp. With the
  DComp device disabled the Win32 Cairo context has no swap chain and uses the
  GDI buffer path. GDK_WIN32_FORCE_DCOMP and GDK_WIN32_CAIRO_GDI_BUFFER are
  removed from the child environment (MSYS2 defaults DComp off).

  Each arm's expected libcairo path is resolved from the same fixed locations
  the child loader searches: the arm's own <GtkPrefix>\bin\libcairo-2.dll when
  that file exists (the packaged candidate bundles the pinned Cairo), otherwise
  the pinned Cairo from -CairoPrefix (default
  C:\vacards\deps\cairo-parity-20260906-r2\install). In every case the file must
  hash to the pinned parity Cairo SHA-256; a divergent or missing required
  module makes the label INVALID. The pinned dependency itself is unchanged.

  Completeness: a requested case with no CASE line, a SKIP, a PASS whose
  required dump is missing, or a FAIL observed with exit 0 makes the run
  incomplete/invalid and the driver exits nonzero. A FAIL always exits nonzero.
  A child that terminates with a negative Windows exit code (NTSTATUS, e.g.
  0xC0000005) is reported as ERROR/crash, counted as incomplete with its exit
  code, and can never be read as UNEXECUTED or a pass.
  The required `visual_artwork` case also requires its normalized `.png`
  (CairoPNG) artifact and a fixed `LAYOUT` report; a missing PNG or a
  baseline/candidate layout/font difference is a failure.

  Physical classification: a nonzero session id is NOT physical (RDP is
  nonzero, and session 0 has been seen reporting WTSClientProtocolType=Console).
  The harness records id, active console, WTS client protocol type and
  SM_REMOTESESSION; physical is claimed only when id == active console,
  protocol == console and not remote. RDP/legacy/unknown and session 0 are
  INVALID, `physical_claim` is false, and a `-Physical` run is refused. Actual
  GDK monitor dimensions/scale are recorded in RESULTS.json.

  Physical-session A/B (root coordinates user/session 1):
    1. Log in to the physical Windows desktop (console/session 1).
    2. Baseline and candidate must both load the pinned parity Cairo. A known
       valid baseline is the stock app prefix, which bundles the pinned Cairo;
       the candidate is the patched app prefix bundling the same Cairo:
         .\test-gtk-cairo-buffer.ps1 `
            -BaselineGtkPrefix 'C:\Program Files\VA Studio 1.0 beta 5' `
            -CandidateGtkPrefix C:\vacards\<candidate-app> `
            -CandidateGtkSha256 <recorded rebuilt libgtk-4-1.dll sha256> `
            -CairoPrefix C:\vacards\deps\cairo-parity-20260906-r2\install `
            -Scales 1,2 -OutDir <evidence> -Physical `
            -FaultCases alloc_latch,first_transfer,partial_transfer,retry_backoff
       T2 and T3 are forced into every run; T6 is also forced at scale 2.
       A PASS needs the primary display at 200% (scale 2) during the run;
       an unavailable scale-2 monitor makes T6 INCOMPLETE.
       Each arm's libcairo is resolved from its own bin when present (the path
       the loader uses) and must hash to the pinned Cairo; otherwise the pinned
       Cairo from -CairoPrefix is used. A stock MSYS2 prefix carries its own
       divergent libcairo and is not a valid arm for this pinned comparison.
    3. Physical-4K performance is only claimable from these session-1 runs.
       Session 0 runs cannot capture pixels; they are UNEXECUTED/incomplete and
       are never a qualified pass.

  This script does not build, install or modify the product or the GTK patch.
#>
[CmdletBinding()]
param(
  [string]$SourceDir = '',
  [string]$BaselineGtkPrefix = 'C:\msys64\ucrt64',
  [string]$CandidateGtkPrefix = '',
  [string]$CandidateGtkSha256 = '',
  [string]$CairoPrefix = 'C:\vacards\deps\cairo-parity-20260906-r2\install',
  [string]$OutDir = 'C:\vacards\gtk-gdi-buffer-20260921\regression-prep\evidence\gtk-cairo-buffer',
  [double[]]$Scales = @(1),
  [string[]]$Cases = @('full','partial','multi_damage','alpha','alpha_half','resize','minimize_restore','gtk_renderer','visual_artwork','popup','dialog','lifecycle'),
  [string[]]$FaultCases = @('alloc_latch','first_transfer','partial_transfer','retry_backoff'),
  [int]$Rounds = 50,
  [switch]$SkipBuild,
  [switch]$Physical,
  [switch]$ListMonitors
)

$ErrorActionPreference = 'Stop'
$script:Failures = 0
$script:Incomplete = 0
$script:Crashes = 0
$script:ProofCases = @('minimize_restore','first_partial','odd_client')

$script:KnownCases = @(
  'full','partial','multi_damage','alpha','alpha_half','resize',
  'minimize_restore','gtk_renderer','visual_artwork','popup','dialog','lifecycle',
  'first_partial','retention','odd_client'
)

# Expected raw dump base names per case (scale suffix is appended at check
# time). lifecyle has no pixel dump.
$script:CaseDumps = @{
  'full'             = @('full')
  'partial'          = @('partial')
  'multi_damage'     = @('multi_damage')
  'alpha'            = @('alpha0','alpha1')
  'alpha_half'       = @('alpha_half')
  'resize'           = @('resize_grow','resize_shrink')
  'minimize_restore' = @('minimize_restore')
  'first_partial'   = @('first_create','first_grow','first_shrink')
  'odd_client'      = @('odd_201','odd_203')
  'gtk_renderer'     = @('gtk_renderer')
  'visual_artwork'   = @('visual_artwork')
  'popup'            = @('popup')
  'dialog'           = @('dialog','dialog_parent_after')
}

# Cases whose normalized PNG must also exist (human-review artifact written
# with the CairoPNG API); a missing PNG is a failure, so the case cannot be
# omitted silently.
$script:CasePngDumps = @{
  'visual_artwork' = @('visual_artwork')
}

function Write-Log([string]$message) {
  $line = "{0:o} {1}" -f (Get-Date), $message
  Write-Host $line
  Add-Content -LiteralPath $script:LogPath -Value $line -Encoding utf8
}

function Invoke-Tool([string]$file, [string[]]$arguments, [string]$workdir) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $file
  $psi.Arguments = (($arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
      }) -join ' ')
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true
  $psi.WorkingDirectory = $workdir
  $p = [System.Diagnostics.Process]::Start($psi)
  $outTask = $p.StandardOutput.ReadToEndAsync()
  $errTask = $p.StandardError.ReadToEndAsync()
  $p.WaitForExit()
  $stdout = $outTask.GetAwaiter().GetResult()
  $stderr = $errTask.GetAwaiter().GetResult()
  return [pscustomobject]@{ ExitCode = $p.ExitCode; Stdout = $stdout; Stderr = $stderr }
}

# Run the harness with both redirected pipes drained before wait.
function Invoke-Harness([string[]]$Arguments, [string]$BinDir, [int]$TimeoutSec = 600,
                        [string]$BufferMode = '', [string]$Fault = '') {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $script:ExePath
  $psi.Arguments = (($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
      }) -join ' ')
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true
  $psi.EnvironmentVariables['PATH'] = ($BinDir.TrimEnd('\') + ';' + $env:PATH)
  $psi.EnvironmentVariables['GSK_RENDERER'] = 'cairo'
  $psi.EnvironmentVariables['GDK_DISABLE'] = 'dcomp'
  $psi.EnvironmentVariables['GDK_BACKEND'] = 'win32'
  # GDK_DEBUG=dcomp is NOT a GDK flag (dcomp lives under GDK_DISABLE); a stray
  # GDK_DEBUG value changes behaviour, so clear it. The candidate patch adds
  # GDK_WIN32_CAIRO_GDI_BUFFER and GDK_WIN32_FORCE_DCOMP; neither may be
  # inherited, so the candidate runs the default new-buffer path and DComp
  # stays off.
  $psi.EnvironmentVariables.Remove('GDK_DEBUG') | Out-Null
  $psi.EnvironmentVariables.Remove('VACARDS_TEST_GDK_GDI_FAULT') | Out-Null
  if ($Fault) { $psi.EnvironmentVariables['VACARDS_TEST_GDK_GDI_FAULT'] = $Fault }
  $psi.EnvironmentVariables.Remove('GDK_WIN32_CAIRO_GDI_BUFFER') | Out-Null
  if ($BufferMode -eq 'off') { $psi.EnvironmentVariables['GDK_WIN32_CAIRO_GDI_BUFFER'] = '0' }
  if (-not $Fault -and @($Arguments | Where-Object { $_ -in (@('retention','lifecycle') + $script:ProofCases) }).Count) {
    $psi.EnvironmentVariables['G_DEBUG'] = 'fatal-warnings'
  } else {
    $psi.EnvironmentVariables.Remove('G_DEBUG') | Out-Null
  }
  $psi.EnvironmentVariables.Remove('GDK_WIN32_FORCE_DCOMP') | Out-Null
  $p = [System.Diagnostics.Process]::Start($psi)
  $outTask = $p.StandardOutput.ReadToEndAsync()
  $errTask = $p.StandardError.ReadToEndAsync()
  $timedOut = $false
  if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    $timedOut = $true
    $p.Kill()
    $p.WaitForExit()
    Write-Log "HARNESS TIMEOUT after ${TimeoutSec}s"
  }
  return [pscustomobject]@{
    ExitCode = $p.ExitCode
    Stdout   = $outTask.GetAwaiter().GetResult()
    Stderr   = $errTask.GetAwaiter().GetResult()
    TimedOut = $timedOut
  }
}

function Get-MsiTool([string]$name) {
  $cmd = Get-Command $name -ErrorAction SilentlyContinue
  if (-not $cmd) { throw "missing tool on PATH: $name (run from the UCRT64 shell)" }
  return $cmd.Source
}

function Build-Harness([string]$gtkPrefix, [string]$exePath) {
  $source = Join-Path $SourceDir 'testfiles\windows\gtk-cairo-buffer-test.c'
  if (-not (Test-Path -LiteralPath $source)) { throw "missing source: $source" }
  $env:PKG_CONFIG_PATH = (Join-Path $gtkPrefix 'lib\pkgconfig') + ';' + $env:PKG_CONFIG_PATH
  $cflags = (Invoke-Tool -file (Get-MsiTool 'pkg-config') -arguments @('--cflags','gtk4') -workdir $SourceDir).Stdout.Trim()
  $libs = (Invoke-Tool -file (Get-MsiTool 'pkg-config') -arguments @('--libs','gtk4') -workdir $SourceDir).Stdout.Trim()
  if (-not $cflags -or -not $libs) { throw "pkg-config did not resolve gtk4 under $gtkPrefix" }
  $gcc = Get-MsiTool 'gcc'
  $gccArgs = @('-O1','-g','-Wall','-o',$exePath,$source) + ($cflags -split '\s+') + ($libs -split '\s+') + @('-luser32','-lgdi32','-lwtsapi32','-lpsapi')
  $r = Invoke-Tool -file $gcc -arguments $gccArgs -workdir $SourceDir
  if ($r.ExitCode -ne 0) {
    Add-Content -LiteralPath $script:LogPath -Value $r.Stdout -Encoding utf8
    Add-Content -LiteralPath $script:LogPath -Value $r.Stderr -Encoding utf8
    throw "harness build failed (exit $($r.ExitCode)); see log"
  }
  Write-Log "BUILD ok $exePath"
}

function Get-FileSha256([string]$path) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
  return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

# The pinned Cairo dependency identity. This is the SHA-256 of the exact
# libcairo-2.dll both arms must load (the parity Cairo used as -CairoPrefix);
# the candidate must not replace or relax it.
$script:PinnedCairoSha256 = '7f634c034e2e3c320b8f15b5eca8194a44ba14b03ab3668c1efd1ce9c9d6ea96'

# Resolve the explicit expected libcairo path for one arm. The child PATH set in
# Run-Label is "<gtkPrefix>\bin;<cairoPrefix>\bin", so the loader takes the
# arm's own bundled Cairo first and the pinned Cairo prefix only when the arm
# does not carry one. The expected path is one of those two fixed locations
# (never the path the process happens to report) and its SHA-256 must equal the
# pinned Cairo; a divergent file in the first existing location is INVALID, not
# silently replaced by another location the loader would not use.
function Resolve-CairoExpectation([string]$gtkPrefix, [string]$cairoPrefix, [string]$pinnedHash) {
  $gtkCairo = Join-Path $gtkPrefix 'bin\libcairo-2.dll'
  $cairoCairo = Join-Path $cairoPrefix 'bin\libcairo-2.dll'
  $candidates = @($gtkCairo)
  if ($cairoCairo -and $cairoCairo -ne $gtkCairo) { $candidates += $cairoCairo }
  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      $hash = Get-FileSha256 $candidate
      if ($hash -ne $pinnedHash) {
        return [pscustomobject]@{ Ok = $false; Path = $candidate; Hash = $hash
          Detail = "expected-cairo-hash-not-pinned path=$candidate sha256=$hash pinned=$pinnedHash" }
      }
      return [pscustomobject]@{ Ok = $true; Path = $candidate; Hash = $hash
        Detail = "path=$candidate sha256=$hash" }
    }
  }
  return [pscustomobject]@{ Ok = $false; Path = $gtkCairo; Hash = $null
    Detail = "expected-cairo-missing candidates=$($candidates -join ';')" }
}

# A negative Windows Process.ExitCode is an NTSTATUS/exception termination
# (e.g. -1073741819 = 0xC0000005 access violation): the child crashed, it did
# not merely decline to print a CASE line.
function Test-ProcessCrash([int]$exitCode) { return $exitCode -lt 0 }

function Format-ExitCode([int]$exitCode) {
  if ($exitCode -lt 0) {
    # Show the NTSTATUS as unsigned hex (e.g. -1073741819 -> 0xC0000005).
    $unsigned = [int64]$exitCode + 0x100000000
    return ('{0} (0x{1:X8})' -f $exitCode, $unsigned)
  }
  return "$exitCode"
}

# Verify one loaded module against an explicit expected file: the loaded path
# must equal the expected path. With -PinnedHash (libcairo) both the expected
# file and the loaded file must hash to that pinned value; without it (libgtk,
# libgdk) the expected file's own hash is the reference. A required module that
# is absent or mismatched is INVALID.
function Test-ModuleIdentity([string]$label, [hashtable]$loaded, [string]$module,
                             [string]$expectedPath, [string]$PinnedHash = '',
                             [switch]$Required) {
  $value = $loaded[$module]
  if (-not $value -or $value -match '\(not loaded\)') {
    if ($Required) {
      Write-Log "INVALID $label required module $module not loaded (expected $expectedPath)"
      return [pscustomobject]@{ Ok = $false; Detail = "not-loaded expected=$expectedPath" }
    }
    return [pscustomobject]@{ Ok = $true; Detail = 'not-loaded(optional)' }
  }
  $loadedFull = [System.IO.Path]::GetFullPath($value)
  $expectedFull = [System.IO.Path]::GetFullPath($expectedPath)
  if (-not $expectedFull.Equals($loadedFull, [System.StringComparison]::OrdinalIgnoreCase)) {
    Write-Log "INVALID $label $module loaded from $loadedFull expected $expectedFull"
    return [pscustomobject]@{ Ok = $false; Detail = "path-mismatch loaded=$loadedFull expected=$expectedFull" }
  }
  $expectedHash = Get-FileSha256 $expectedFull
  $loadedHash = Get-FileSha256 $loadedFull
  if ($PinnedHash) {
    if ($expectedHash -ne $PinnedHash) {
      Write-Log "INVALID $label $module expected file hash not pinned path=$expectedFull sha256=$expectedHash pinned=$PinnedHash"
      return [pscustomobject]@{ Ok = $false; Detail = "expected-hash-not-pinned path=$expectedFull sha256=$expectedHash pinned=$PinnedHash" }
    }
    if ($loadedHash -ne $PinnedHash) {
      Write-Log "INVALID $label $module hash mismatch expected=$PinnedHash loaded=$loadedHash"
      return [pscustomobject]@{ Ok = $false; Detail = "hash-mismatch expected=$PinnedHash loaded=$loadedHash" }
    }
    return [pscustomobject]@{ Ok = $true; Detail = "path=$loadedFull sha256=$loadedHash pinned" }
  }
  if (-not $expectedHash -or $expectedHash -ne $loadedHash) {
    Write-Log "INVALID $label $module hash mismatch expected=$expectedHash loaded=$loadedHash"
    return [pscustomobject]@{ Ok = $false; Detail = "hash-mismatch expected=$expectedHash loaded=$loadedHash" }
  }
  return [pscustomobject]@{ Ok = $true; Detail = "path=$loadedFull sha256=$loadedHash" }
}

function Parse-Harness([string]$stdout) {
  $cases = @{}
  $loaded = @{}
  $envInfo = $null
  $session = $null
  $monitors = @()
  $layout = @{}
  foreach ($line in ($stdout -split "`r?`n")) {
    if ($line -match '^CASE\s+(\S+)\s+RESULT\s+(PASS|FAIL|SKIP)\s*(.*)$') {
      $cases[$Matches[1]] = [pscustomobject]@{ Result = $Matches[2]; Detail = $Matches[3].Trim() }
    } elseif ($line -match '^LOADED\s+(\S+)\s+(.*)$') {
      $loaded[$Matches[1]] = $Matches[2].Trim()
    } elseif ($line -match '^ENV\s+GDK_DISABLE=(\S*)\s+GDK_DEBUG=(\S*)\s+GSK_RENDERER=(\S*)\s+GDK_BACKEND=(\S*)') {
      $envInfo = [pscustomobject]@{
        GdkDisable = $Matches[1]; GdkDebug = $Matches[2]
        GskRenderer = $Matches[3]; GdkBackend = $Matches[4]
      }
    } elseif ($line -match '^SESSION\s+id=(\d+)\s+physical_requested=(\d+)\s+active_console=(\d+)\s+protocol=(\S+)\s+remote=(\d+)\s+physical_ok=(\d+)') {
      $session = [pscustomobject]@{
        Id = [long]$Matches[1]
        PhysicalRequested = ([int]$Matches[2] -ne 0)
        ActiveConsole = [long]$Matches[3]
        Protocol = $Matches[4]
        Remote = ([int]$Matches[5] -ne 0)
        PhysicalOk = ([int]$Matches[6] -ne 0)
      }
    } elseif ($line -match '^MONITOR\s+(\d+)\s+scale=(\d+)\s+x=(-?\d+)\s+y=(-?\d+)\s+w=(\d+)\s+h=(\d+)') {
      $monitors += [pscustomobject]@{
        Index = [int]$Matches[1]; Scale = [int]$Matches[2]
        X = [int]$Matches[3]; Y = [int]$Matches[4]
        Width = [int]$Matches[5]; Height = [int]$Matches[6]
      }
    } elseif ($line -match '^LAYOUT\s+(\S+)\s+(.*)$') {
      $layout[$Matches[1]] = ($Matches[1] + ' ' + $Matches[2].Trim())
    }
  }
  return [pscustomobject]@{ Cases = $cases; Loaded = $loaded; Env = $envInfo;
                            Session = $session; Monitors = $monitors; Layout = $layout }
}

# The child must actually run the intended backend/renderer path.
function Test-RendererConfig($parsed) {
  $errors = @()
  if (-not $parsed.Env) { return @('missing ENV line') }
  $disable = @($parsed.Env.GdkDisable -split '[,;: ]+' | Where-Object { $_ })
  if ($disable -notcontains 'dcomp') {
    $errors += "GDK_DISABLE='$($parsed.Env.GdkDisable)' does not contain dcomp"
  }
  $debug = @($parsed.Env.GdkDebug -split '[,;: ]+' | Where-Object { $_ })
  if ($debug -contains 'dcomp') {
    $errors += "GDK_DEBUG='$($parsed.Env.GdkDebug)' still contains dcomp (must be GDK_DISABLE)"
  }
  if ($parsed.Env.GskRenderer -ne 'cairo') {
    $errors += "GSK_RENDERER='$($parsed.Env.GskRenderer)' expected 'cairo'"
  }
  if ($parsed.Env.GdkBackend -ne 'win32') {
    $errors += "GDK_BACKEND='$($parsed.Env.GdkBackend)' expected 'win32'"
  }
  return $errors
}

# Physical requires the active console session with a console protocol and no
# Remote Desktop session; a nonzero session id alone is never physical, and the
# driver recomputes this from the recorded fields (never trusting one flag).
function Test-PhysicalSession($s) {
  if (-not $s) { return $false }
  return ($s.Id -ne 0) -and ($s.Id -eq $s.ActiveConsole) -and
         ($s.Protocol -eq '0') -and (-not $s.Remote) -and [bool]$s.PhysicalOk
}

function Get-DumpHashes([string]$dir) {
  $map = @{}
  if (Test-Path -LiteralPath $dir) {
    Get-ChildItem -LiteralPath $dir -Filter '*.bgra' | ForEach-Object {
      $map[$_.BaseName] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
  }
  return $map
}

function Run-Label([string]$label, [string]$gtkPrefix, [string]$cairoPrefix, [string]$outRoot) {
  $dumpDir = Join-Path $outRoot 'dumps'
  $logDir = Join-Path $outRoot 'logs'
  New-Item -ItemType Directory -Force -Path $dumpDir, $logDir | Out-Null
  if (-not $cairoPrefix) { $cairoPrefix = $gtkPrefix }

  $childPath = (Join-Path $gtkPrefix 'bin').TrimEnd('\') + ';' +
               (Join-Path $cairoPrefix 'bin').TrimEnd('\')
  $gtkExpect = Join-Path $gtkPrefix 'bin\libgtk-4-1.dll'
  $gdkExpect = Join-Path $gtkPrefix 'bin\libgdk-4-1.dll'
  $moduleLog = Join-Path $logDir 'loaded-modules.txt'

  $caseTable = @{}
  $invalid = $false
  $loadedOk = $true
  $envChecked = $false
  $sessionInfo = $null
  $monitors = @()
  $layout = @{}

  # Per-arm expected libcairo path: the arm's own bin when it bundles the
  # pinned Cairo, else the pinned Cairo prefix. INVALID if the resolved file is
  # missing or does not hash to the pinned Cairo.
  $cairoResolution = Resolve-CairoExpectation -gtkPrefix $gtkPrefix -cairoPrefix $cairoPrefix -pinnedHash $script:PinnedCairoSha256
  $cairoExpect = $cairoResolution.Path
  if ($cairoResolution.Ok) {
    Write-Log "CAIRO-EXPECT $label $($cairoResolution.Detail)"
  } else {
    Write-Log "INVALID $label cairo expectation invalid: $($cairoResolution.Detail)"
    $invalid = $true
  }

  foreach ($case in $Cases) {
    $caseScales = if ($case -eq 'odd_client') { @(2) } else { $Scales }
    foreach ($scale in $caseScales) {
      $scaleStr = $scale.ToString([System.Globalization.CultureInfo]::InvariantCulture)
      $harnessArgs = @('--out', $dumpDir, '--scale', $scaleStr, '--rounds', "$Rounds")
      if ($Physical) { $harnessArgs += '--physical' }
      $harnessArgs += @('--case', $case)
      $r = Invoke-Harness -Arguments $harnessArgs -BinDir $childPath -TimeoutSec 300
      $tag = "case-$case-scale$scaleStr"
      Set-Content -LiteralPath (Join-Path $logDir "$tag.out") -Value $r.Stdout -Encoding utf8
      Set-Content -LiteralPath (Join-Path $logDir "$tag.err") -Value $r.Stderr -Encoding utf8
      $parsed = Parse-Harness $r.Stdout
      if ($r.Stderr -cmatch 'GDK-WIN32|(^|[\s-])(WARNING|CRITICAL) \*\*') {
        Write-Log "INVALID $label case=$case scale=$scaleStr warning in stderr"
        $script:Failures++
      }

      # Layout/font reports and monitors are per-run; merge them across cases,
      # because the first case is not necessarily visual_artwork.
      foreach ($lk in $parsed.Layout.Keys) { $layout[$lk] = $parsed.Layout[$lk] }
      if ($parsed.Monitors.Count -gt 0) { $monitors = @($parsed.Monitors) }

      if (-not $envChecked) {
        $envErrors = @(Test-RendererConfig $parsed)
        foreach ($e in $envErrors) { Write-Log "INVALID $label $e" }
        if ($envErrors.Count -gt 0) { $invalid = $true }
        $sessionInfo = $parsed.Session
        if ($sessionInfo) {
          Write-Log ("SESSION $label id=$($sessionInfo.Id) requested=$($sessionInfo.PhysicalRequested) active_console=$($sessionInfo.ActiveConsole) protocol=$($sessionInfo.Protocol) remote=$($sessionInfo.Remote) physical_ok=$($sessionInfo.PhysicalOk)")
          if ($Physical -and -not (Test-PhysicalSession $sessionInfo)) {
            Write-Log ("INVALID $label physical run not on active console (id=$($sessionInfo.Id) active_console=$($sessionInfo.ActiveConsole) protocol=$($sessionInfo.Protocol) remote=$($sessionInfo.Remote))")
            $invalid = $true
          }
        } else {
          Write-Log "INVALID $label missing SESSION line"
          $invalid = $true
        }
        $envChecked = $true
      }

      $gtkPin = if ($label -eq 'candidate') { $CandidateGtkSha256 } else { '' }
      $gtkRes = Test-ModuleIdentity $label $parsed.Loaded 'libgtk-4-1.dll' $gtkExpect -PinnedHash $gtkPin -Required
      $cairoRes = Test-ModuleIdentity $label $parsed.Loaded 'libcairo-2.dll' $cairoExpect -PinnedHash $script:PinnedCairoSha256 -Required
      Add-Content -LiteralPath $moduleLog -Value "$label libgtk-4-1.dll $($gtkRes.Detail)" -Encoding utf8
      Add-Content -LiteralPath $moduleLog -Value "$label libcairo-2.dll $($cairoRes.Detail)" -Encoding utf8
      if (-not $gtkRes.Ok -or -not $cairoRes.Ok) { $loadedOk = $false; $invalid = $true }
      if ($parsed.Loaded.ContainsKey('libgdk-4-1.dll')) {
        $gdkRes = Test-ModuleIdentity $label $parsed.Loaded 'libgdk-4-1.dll' $gdkExpect
        Add-Content -LiteralPath $moduleLog -Value "$label libgdk-4-1.dll $($gdkRes.Detail)" -Encoding utf8
        if (-not $gdkRes.Ok) { $loadedOk = $false; $invalid = $true }
      }

      if ($r.TimedOut) {
        Write-Log "TIMEOUT $label case=$case scale=$scaleStr"
        $script:Incomplete++
        continue
      }

      $exitText = Format-ExitCode $r.ExitCode
      $crashed = Test-ProcessCrash $r.ExitCode

      if (-not $parsed.Cases.ContainsKey($case)) {
        if ($crashed) {
          Write-Log "ERROR $label case=$case scale=$scaleStr process crashed exit=$exitText (no CASE line)"
          $script:Crashes++
          $script:Incomplete++
        } else {
          Write-Log "UNEXECUTED $label case=$case scale=$scaleStr exit=$exitText (no CASE line)"
          $script:Incomplete++
        }
        continue
      }
      $res = $parsed.Cases[$case]
      $key = "$case@$scaleStr"
      $caseTable[$key] = $res
      if ($crashed) {
        Write-Log "ERROR $label case=$case scale=$scaleStr process crashed exit=$exitText after CASE $($res.Result)"
        $script:Crashes++
        $script:Incomplete++
      }
      if ($res.Result -eq 'PASS' -and $r.ExitCode -ne 0) {
        Write-Log "INVALID $label case=$case scale=$scaleStr PASS with exit=$exitText"
        if (-not $crashed) { $script:Incomplete++ }
      }
      if ($res.Result -eq 'FAIL' -and $r.ExitCode -eq 0) {
        Write-Log "INVALID $label case=$case scale=$scaleStr FAIL with exit=0 (test FAIL must be nonzero)"
        $script:Incomplete++
      }
      if ($res.Result -eq 'SKIP' -and $r.ExitCode -eq 0) {
        Write-Log "INVALID $label case=$case scale=$scaleStr SKIP with exit=0 (skip is not a qualified pass)"
        $script:Incomplete++
      }
      if ($label -eq 'baseline' -and $case -in $script:ProofCases) {
        if ($res.Result -eq 'FAIL' -and $res.Detail -eq 'proof-oracle-mismatch' -and
            $r.ExitCode -eq 1 -and -not $crashed) {
          Write-Log "EXPECTED-FAIL baseline case=$case scale=$scaleStr"
          $caseTable[$key] = [pscustomobject]@{ Result = 'EXPECTED-FAIL'; Detail = $res.Detail }
        } elseif ($res.Result -eq 'PASS') {
          Write-Log "INVALID baseline case=$case scale=$scaleStr unexpectedly PASS"
          $script:Failures++
        } elseif (-not $crashed) {
          Write-Log "INCOMPLETE baseline case=$case scale=$scaleStr result=$($res.Result) detail=$($res.Detail)"
          $script:Incomplete++
        }
      } elseif ($res.Result -eq 'FAIL') { $script:Failures++ }
      elseif ($res.Result -eq 'SKIP') { $script:Incomplete++ }
    }
  }

  $dumpHashes = Get-DumpHashes $dumpDir
  foreach ($key in @($caseTable.Keys)) {
    $parts = $key.Split('@')
    $cname = $parts[0]
    $sstr = $parts[1]
    if (-not $script:CaseDumps.ContainsKey($cname)) { continue }
    if ($caseTable[$key].Result -ne 'PASS') { continue }
    foreach ($dump in $script:CaseDumps[$cname]) {
      $name = "$dump-s$sstr"
      if (-not $dumpHashes.ContainsKey($name)) {
        Write-Log "MISSING-DUMP $label case=$cname scale=$sstr expected=$name.bgra"
        $script:Failures++
      }
    }
  }

  # The visual_artwork PNG is a required human-review artifact (CairoPNG). Its
  # absence is a failure, so the case cannot be omitted silently.
  foreach ($key in @($caseTable.Keys)) {
    $parts = $key.Split('@')
    $cname = $parts[0]
    $sstr = $parts[1]
    if (-not $script:CasePngDumps.ContainsKey($cname)) { continue }
    if ($caseTable[$key].Result -ne 'PASS') { continue }
    foreach ($dump in $script:CasePngDumps[$cname]) {
      $name = "$dump-s$sstr.png"
      if (-not (Test-Path -LiteralPath (Join-Path $dumpDir $name))) {
        Write-Log "MISSING-DUMP $label case=$cname scale=$sstr expected=$name"
        $script:Failures++
      }
    }
  }

  # A PASSed case that must carry a fixed layout/font report but produced none
  # is incomplete: the baseline/candidate layout comparison would be vacuous.
  foreach ($key in @($caseTable.Keys)) {
    $parts = $key.Split('@')
    $cname = $parts[0]
    if ($cname -ne 'visual_artwork') { continue }
    if ($caseTable[$key].Result -ne 'PASS') { continue }
    if (-not $layout.ContainsKey($cname)) {
      Write-Log "INVALID $label case=$cname missing LAYOUT report"
      $script:Incomplete++
    }
  }

  if ($invalid) { $script:Incomplete++ }
  return [pscustomobject]@{
    Label       = $label
    Prefix      = $gtkPrefix
    CairoPrefix = $cairoPrefix
    LoadedOk    = $loadedOk
    Cases       = $caseTable
    DumpHashes  = $dumpHashes
    DumpDir     = $dumpDir
    Session     = $sessionInfo
    Monitors    = $monitors
    Layout      = $layout
  }
}

# ---------------------------------------------------------------- main

if (-not $SourceDir) {
  $SourceDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
}
$repoRoot = (Resolve-Path $SourceDir).Path
$runId = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $PID
$evidence = Join-Path $OutDir $runId
$exeDir = Join-Path $OutDir 'bin'
New-Item -ItemType Directory -Force -Path $evidence, $exeDir | Out-Null
$script:LogPath = Join-Path $evidence 'driver.log'
$script:ExePath = Join-Path $exeDir 'gtk-cairo-buffer-test.exe'

if ($CandidateGtkSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
    $CandidateGtkSha256 -match '^0{64}$' -or
    -not $CandidateGtkPrefix -or
    -not (Test-Path -LiteralPath (Join-Path $CandidateGtkPrefix 'bin\libgtk-4-1.dll') -PathType Leaf)) {
  throw 'candidate prefix and fixed rebuilt-DLL SHA-256 are required'
}
if ((Get-FileSha256 (Join-Path $CandidateGtkPrefix 'bin\libgtk-4-1.dll')) -ne $CandidateGtkSha256) {
  throw 'candidate DLL SHA-256 differs from fixed pin'
}
if ($Cases -contains 'lifecycle' -and $Rounds -lt 26) { throw 'lifecycle requires -Rounds >= 26' }
foreach ($c in $Cases) {
  if ($script:KnownCases -notcontains $c) { throw "unknown -Cases value: $c" }
}
foreach ($f in $FaultCases) {
  if ($f -notin @('alloc_latch','first_transfer','partial_transfer','retry_backoff')) {
    throw "unknown -FaultCases value: $f"
  }
}
foreach ($s in $Scales) {
  if ($s -le 0) { throw "invalid -Scales value: $s" }
}
$Cases = @(@($Cases) + $script:ProofCases | Select-Object -Unique)

$sessionId = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
Write-Log "SOURCE $repoRoot"
Write-Log "SHELL MSYSTEM=$env:MSYSTEM (UCRT64 required for a valid build)"
Write-Log "SESSION driver=$sessionId physical=$($Physical.IsPresent)"
Write-Log "CONFIG GSK_RENDERER=cairo GDK_DISABLE=dcomp GDK_BACKEND=win32 GDK_DEBUG=cleared"

if ($env:MSYSTEM -and $env:MSYSTEM -ne 'UCRT64') {
  throw "run from the MSYS2 UCRT64 shell (MSYSTEM=$env:MSYSTEM)"
}
if ($Physical -and $sessionId -eq 0) {
  Write-Log 'INVALID physical run requested from session 0 (pre-check; harness session classification is authoritative)'
  $script:Incomplete++
}

# Pure oracle self-test is host-independent and must pass before any GUI run.
if (-not $SkipBuild) {
  Build-Harness -gtkPrefix $BaselineGtkPrefix -exePath $script:ExePath
} elseif (-not (Test-Path -LiteralPath $script:ExePath)) {
  throw "-SkipBuild set but $script:ExePath does not exist"
}

$st = Invoke-Harness -Arguments @('--self-test') -BinDir (Join-Path $BaselineGtkPrefix 'bin') -TimeoutSec 60
Set-Content -LiteralPath (Join-Path $evidence 'selftest.out') -Value $st.Stdout -Encoding utf8
if ($st.ExitCode -ne 0 -or $st.Stdout -notmatch 'SELFTEST PASS' -or $st.Stderr -cmatch 'GDK-WIN32|(^|[\s-])(WARNING|CRITICAL) \*\*') {
  Write-Log "SELFTEST FAIL exit=$($st.ExitCode)"
  exit 1
}
Write-Log 'SELFTEST PASS'

if ($ListMonitors) {
  $lm = Invoke-Harness -Arguments @('--list-monitors') -BinDir (Join-Path $BaselineGtkPrefix 'bin') -TimeoutSec 60
  Set-Content -LiteralPath (Join-Path $evidence 'monitors.out') -Value $lm.Stdout -Encoding utf8
  if ($lm.Stderr -cmatch 'GDK-WIN32|(^|[\s-])(WARNING|CRITICAL) \*\*') { Write-Log 'INVALID monitors warning'; $script:Failures++ }
  Write-Log 'MONITORS written'
}

# T1 uses two fresh children with identical case/size/frame sequence.
$probe = @{}
$probeChildPath = (Join-Path $CandidateGtkPrefix 'bin').TrimEnd('\') + ';' +
                  (Join-Path $CairoPrefix 'bin').TrimEnd('\')
foreach ($mode in @('on','off')) {
  $args = @('--case','retention','--scale',($Scales[0].ToString([System.Globalization.CultureInfo]::InvariantCulture)),'--out',(Join-Path $evidence "retention-$mode"))
  if ($Physical) { $args += '--physical' }
  if ($mode -eq 'on') { $args += '--expect-retained' }
  $r = Invoke-Harness -Arguments $args -BinDir $probeChildPath -TimeoutSec 60 -BufferMode $mode
  Set-Content -LiteralPath (Join-Path $evidence "retention-$mode.out") $r.Stdout
  Set-Content -LiteralPath (Join-Path $evidence "retention-$mode.err") $r.Stderr
  $parsed = Parse-Harness $r.Stdout
  $counts = [regex]::Match($r.Stdout, 'RETENTION mode=(on|off) initial=(\d+) before=(\d+) drawn=(\d+) destroyed=(\d+)')
  $id = Test-ModuleIdentity "probe-$mode" $parsed.Loaded 'libgtk-4-1.dll' (Join-Path $CandidateGtkPrefix 'bin\libgtk-4-1.dll') -PinnedHash $CandidateGtkSha256 -Required
  $cairo = Resolve-CairoExpectation -gtkPrefix $CandidateGtkPrefix -cairoPrefix $CairoPrefix -pinnedHash $script:PinnedCairoSha256
  $cid = if ($cairo.Ok) { Test-ModuleIdentity "probe-$mode" $parsed.Loaded 'libcairo-2.dll' $cairo.Path -PinnedHash $script:PinnedCairoSha256 -Required } else { [pscustomobject]@{ Ok = $false } }
  $expectedEnv = if ($mode -eq 'on') { 'unset' } else { '0' }
  if ($r.TimedOut -or $r.ExitCode -ne 0 -or $r.Stderr -cmatch 'GDK-WIN32|(^|[\s-])(WARNING|CRITICAL) \*\*' -or
      -not $id.Ok -or -not $cid.Ok -or -not $cairo.Ok -or
      $r.Stdout -notmatch "ENV BUFFER=$expectedEnv" -or
      -not $parsed.Cases.ContainsKey('retention') -or $parsed.Cases['retention'].Result -ne 'PASS' -or
      -not $counts.Success -or $counts.Groups[1].Value -ne $mode) {
    Write-Log "INVALID retention-$mode child exit=$($r.ExitCode) timeout=$($r.TimedOut)"
    # Only an oracle FAIL from the child is a patch failure; skips, timeouts
    # and identity/environment problems leave the proof incomplete.
    if ($parsed.Cases.ContainsKey('retention') -and $parsed.Cases['retention'].Result -eq 'FAIL') {
      $script:Failures++
    } else {
      $script:Incomplete++
    }
    continue
  }
  $probe[$mode] = @([int]$counts.Groups[2].Value, [int]$counts.Groups[3].Value,
                    [int]$counts.Groups[4].Value, [int]$counts.Groups[5].Value)
  Write-Log "RETENTION $mode initial=$($probe[$mode][0]) before=$($probe[$mode][1]) drawn=$($probe[$mode][2]) destroyed=$($probe[$mode][3])"
}
if (($probe.Count -eq 2 -and ($probe.on[2] -ne $probe.on[1] -or
       $probe.on[2] -le $probe.on[3] -or $probe.off[2] -gt $probe.off[1] + 1 -or
       ($probe.on[2] - $probe.on[0]) -le ($probe.off[2] - $probe.off[0])))) {
  Write-Log 'INVALID retention on/off differential'; $script:Failures++
}

$base = Run-Label -label 'baseline' -gtkPrefix $BaselineGtkPrefix -cairoPrefix $CairoPrefix -outRoot (Join-Path $evidence 'baseline')

$cand = $null
if ($CandidateGtkPrefix) {
  $cand = Run-Label -label 'candidate' -gtkPrefix $CandidateGtkPrefix -cairoPrefix $CairoPrefix -outRoot (Join-Path $evidence 'candidate')
}
if (-not $cand -or -not $base.Cases.ContainsKey('odd_client@2') -or
    -not $cand.Cases.ContainsKey('odd_client@2')) {
  Write-Log 'INCOMPLETE T6 odd_client scale=2 not run'
  $script:Incomplete++
}

# T4 fault children use only the pinned candidate; stderr must contain exactly
# the one warning expected for that child and no other warning or critical.
$allFaults = @('alloc_latch','first_transfer','partial_transfer','retry_backoff')
$requestedFaults = @($FaultCases | Select-Object -Unique)
foreach ($faultCase in $allFaults) {
  if ($requestedFaults -notcontains $faultCase) {
    Write-Log "INCOMPLETE T4 $faultCase not run"
    $script:Incomplete++
  }
}
$faultScale = $Scales[-1].ToString([System.Globalization.CultureInfo]::InvariantCulture)
foreach ($faultCase in $requestedFaults) {
  $faultValue = @{ alloc_latch='alloc-once'; first_transfer='bitblt-once';
                   partial_transfer='bitblt-after:1:2'; retry_backoff='bitblt-after:1:5' }[$faultCase]
  $args = @('--case',$faultCase,'--scale',$faultScale,'--out',(Join-Path $evidence "fault-$faultCase"))
  if ($Physical) { $args += '--physical' }
  $r = Invoke-Harness -Arguments $args -BinDir $probeChildPath -TimeoutSec 30 -Fault $faultValue
  Set-Content -LiteralPath (Join-Path $evidence "$faultCase.out") $r.Stdout
  Set-Content -LiteralPath (Join-Path $evidence "$faultCase.err") $r.Stderr
  $parsed = Parse-Harness $r.Stdout
  $configOk = @(Test-RendererConfig $parsed).Count -eq 0
  $sessionOk = -not $Physical -or (Test-PhysicalSession $parsed.Session)
  $gtkId = Test-ModuleIdentity "fault-$faultCase" $parsed.Loaded 'libgtk-4-1.dll' (Join-Path $CandidateGtkPrefix 'bin\libgtk-4-1.dll') -PinnedHash $CandidateGtkSha256 -Required
  $cairoId = Test-ModuleIdentity "fault-$faultCase" $parsed.Loaded 'libcairo-2.dll' $cairo.Path -PinnedHash $script:PinnedCairoSha256 -Required
  $gdkOk = -not $parsed.Loaded.ContainsKey('libgdk-4-1.dll') -or (Test-ModuleIdentity "fault-$faultCase" $parsed.Loaded 'libgdk-4-1.dll' (Join-Path $CandidateGtkPrefix 'bin\libgdk-4-1.dll')).Ok
  $expected = 'GDK-WIN32: BitBlt failed; retaining pending presentation'
  if ($faultCase -eq 'alloc_latch') {
    $size = [regex]::Match($r.Stdout, 'FAULT_SIZE width=(\d+) height=(\d+)')
    $expected = if ($size.Success) { "GDK-WIN32: could not allocate a $($size.Groups[1].Value)x$($size.Groups[2].Value) reusable GDI backing store (out of memory); using the per-frame fallback" } else { '' }
  }
  $warnings = @([regex]::Matches($r.Stderr, '(?m)^.*(?:WARNING|CRITICAL|GDK-WIN32:).*$') | ForEach-Object { $_.Value })
  if ($r.TimedOut -or (Test-ProcessCrash $r.ExitCode) -or -not $gtkId.Ok -or -not $cairoId.Ok -or
      -not $cairo.Ok -or -not $gdkOk -or -not $configOk -or -not $sessionOk -or -not $expected -or
      -not $parsed.Cases.ContainsKey($faultCase) -or
      $parsed.Cases[$faultCase].Result -eq 'SKIP') {
    Write-Log "INCOMPLETE T4 $faultCase exit=$($r.ExitCode) warning_count=$($warnings.Count)"
    $script:Incomplete++
  } elseif ($warnings.Count -ne 1 -or $warnings[0] -notmatch ([regex]::Escape($expected) + '\s*$')) {
    Write-Log "FAIL T4 $faultCase warning_count=$($warnings.Count) expected=$expected"
    $script:Failures++
  } elseif ($r.ExitCode -ne 0 -or $parsed.Cases[$faultCase].Result -ne 'PASS') {
    Write-Log "FAIL T4 $faultCase exit=$($r.ExitCode)"; $script:Failures++
  } else { Write-Log "PASS T4 $faultCase warning_count=1" }
}

# Baseline/candidate presented-pixel comparison over the union of dump names so
# a dump missing from either side is caught.
$compare = @{}
if ($cand) {
  if ($base.DumpHashes.Count -eq 0 -or $cand.DumpHashes.Count -eq 0) {
    Write-Log "INVALID dump set empty baseline=$($base.DumpHashes.Count) candidate=$($cand.DumpHashes.Count)"
    $script:Incomplete++
  }
  $names = @(@($base.DumpHashes.Keys) + @($cand.DumpHashes.Keys) | Sort-Object -Unique)
  foreach ($name in $names) {
    if ($name -match '^(minimize_restore|first_create|first_grow|first_shrink|odd_201|odd_203)-s') {
      $compare[$name] = 'PROOF-CASE-NO-BASELINE-COMPARE'; continue
    }
    $hasBase = $base.DumpHashes.ContainsKey($name)
    $hasCand = $cand.DumpHashes.ContainsKey($name)
    if ($hasBase -and -not $hasCand) {
      $compare[$name] = 'CANDIDATE-MISSING'; $script:Failures++
    } elseif ($hasCand -and -not $hasBase) {
      $compare[$name] = 'BASELINE-MISSING'; $script:Failures++
    } elseif ($base.DumpHashes[$name] -ne $cand.DumpHashes[$name]) {
      $compare[$name] = 'DIFF'; $script:Failures++
    } else {
      $compare[$name] = 'EQUAL'
    }
  }
}

# Fixed layout/font report must be byte-identical between the arms; a difference
# means the drawing or font resolution changed, not just the buffer path.
$layoutCompare = @{}
if ($cand) {
  $layoutNames = @(@($base.Layout.Keys) + @($cand.Layout.Keys) | Sort-Object -Unique)
  foreach ($name in $layoutNames) {
    $hasBase = $base.Layout.ContainsKey($name)
    $hasCand = $cand.Layout.ContainsKey($name)
    if ($hasBase -and -not $hasCand) {
      $layoutCompare[$name] = 'CANDIDATE-MISSING'; $script:Failures++
    } elseif ($hasCand -and -not $hasBase) {
      $layoutCompare[$name] = 'BASELINE-MISSING'; $script:Failures++
    } elseif ($base.Layout[$name] -ne $cand.Layout[$name]) {
      $layoutCompare[$name] = 'DIFF'; $script:Failures++
    } else {
      $layoutCompare[$name] = 'EQUAL'
    }
  }
}

# Physical is claimed only from the active-console/console-protocol fields,
# never from a nonzero driver session id (RDP is nonzero too).
$baseSession = $base.Session
$physicalOk = Test-PhysicalSession $baseSession
$physicalClaim = $Physical.IsPresent -and $physicalOk

# Combined per-case verdict table (baseline + candidate).
$verdict = @{}
foreach ($label in @($base, $cand) | Where-Object { $_ }) {
  foreach ($key in $label.Cases.Keys) {
    if (-not $verdict.ContainsKey($key)) {
      $verdict[$key] = [pscustomobject]@{ Baseline = $null; Candidate = $null }
    }
    if ($label.Label -eq 'baseline') { $verdict[$key].Baseline = $label.Cases[$key].Result }
    else { $verdict[$key].Candidate = $label.Cases[$key].Result }
  }
}

$result = [pscustomobject]@{
  run_id              = $runId
  source              = $repoRoot
  physical_requested  = $Physical.IsPresent
  physical_claim      = $physicalClaim
  driver_session_id   = $sessionId
  session_id          = if ($baseSession) { $baseSession.Id } else { $null }
  active_console      = if ($baseSession) { $baseSession.ActiveConsole } else { $null }
  protocol            = if ($baseSession) { $baseSession.Protocol } else { $null }
  remote              = if ($baseSession) { $baseSession.Remote } else { $null }
  physical_ok         = $physicalOk
  harness_physical_ok = if ($baseSession) { $baseSession.PhysicalOk } else { $null }
  monitors            = @($base.Monitors)
  renderer            = 'GSK_RENDERER=cairo GDK_DISABLE=dcomp GDK_BACKEND=win32 GDK_DEBUG=cleared'
  baseline_prefix     = $BaselineGtkPrefix
  candidate_prefix    = $CandidateGtkPrefix
  cairo_prefix        = $CairoPrefix
  baseline_loaded_ok  = $base.LoadedOk
  candidate_loaded_ok = if ($cand) { $cand.LoadedOk } else { $null }
  dump_compare        = $compare
  layout_compare      = $layoutCompare
  case_verdicts       = $verdict
  failures            = $script:Failures
  incomplete          = $script:Incomplete
  crashes             = $script:Crashes
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidence 'RESULTS.json') -Encoding utf8

$summary = @()
$summary += "# GTK Cairo buffer regression run $runId"
$summary += ""
$summary += "- Source: $repoRoot"
$summary += "- Physical requested: $($Physical.IsPresent); physical claim: $physicalClaim"
$summary += "- Session: id=$($result.session_id) active_console=$($result.active_console) protocol=$($result.protocol) remote=$($result.remote) physical_ok=$($result.physical_ok) (driver session $sessionId)"
$summary += "- Monitors: $(@($base.Monitors).Count)"
$summary += "- Renderer: GSK_RENDERER=cairo, GDK_DISABLE=dcomp, GDK_BACKEND=win32, GDK_DEBUG cleared"
$summary += "- Baseline: $BaselineGtkPrefix (loaded-ok: $($base.LoadedOk))"
$summary += "- Candidate: $CandidateGtkPrefix (loaded-ok: $(if ($cand) { $cand.LoadedOk } else { 'n/a' }))"
$summary += "- Cairo prefix: $CairoPrefix (pinned SHA-256 $script:PinnedCairoSha256)"
$summary += "- Failures: $($script:Failures); incomplete: $($script:Incomplete); crashes: $($script:Crashes)"
$summary += ""
$summary += "Dump comparison:"
foreach ($k in ($compare.Keys | Sort-Object)) { $summary += ("- {0}: {1}" -f $k, $compare[$k]) }
$summary += ""
$summary += "Layout/font comparison (visual_artwork):"
foreach ($k in ($layoutCompare.Keys | Sort-Object)) { $summary += ("- {0}: {1} => {2}" -f $k, $layoutCompare[$k], $base.Layout[$k]) }
$summary += ""
$summary += "Non-console/non-physical runs validate no pixel oracle and are UNEXECUTED/incomplete; they cannot claim physical performance. Physical requires id == active console, WTS protocol console, and not SM_REMOTESESSION."
$summary -join "`n" | Set-Content -LiteralPath (Join-Path $evidence 'SUMMARY.md') -Encoding utf8

Write-Log "EVIDENCE $evidence"
Write-Log "VERDICT failures=$($script:Failures) incomplete=$($script:Incomplete) crashes=$($script:Crashes)"
if ($script:Failures -gt 0 -or $script:Incomplete -gt 0) { exit 1 }
exit 0
