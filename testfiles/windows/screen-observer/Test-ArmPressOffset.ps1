#Requires -Version 5.1
# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
  No-GUI, pure PS 5.1 tests for the dual-arm press-offset repair.

.DESCRIPTION
  Extracts the ACTUAL Get-ArmPressOffset / Read-MaximizedCalibration helpers from
  Invoke-AppPan.ps1 and the mirror Get-ArmPressOffset /
  Read-TrialMaximizedCalibration helpers from Invoke-AppTrial.ps1 and exercises
  them with synthetic calibrations (no window, no SendInput, no app).

    * legacy normal-client offset is exactly 4;
    * maximized offset is ceil(4*physical_pixels_per_logical_pixel)+2:
      1.491279 -> 8, 1.0 -> 6, 1.5 -> 8, 0.1 -> 3;
    * missing / NaN / Infinity / non-positive / >1000 / wrong-typed scale ->
      $null (fail closed), for both Pan and Trial;
    * Read-MaximizedCalibration admits the derived forward press
      (select-offset) and return press (select+120+offset) and rejects a
      calibration whose select is too near either client edge;
    * Read-TrialMaximizedCalibration reaches the same verdicts independently.

  Exit 0 = pass; exit 1 = a check failed; 2 = unreadable/unparseable input.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PanPath,
    [Parameter(Mandatory = $true)][string]$TrialPath,
    [string]$OutputJson
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
trap {
    Write-Output ("UNHANDLED line " + $_.InvocationInfo.ScriptLineNumber + " :: " + $_)
    Write-Output 'ARMOFFSET FAIL unhandled'
    exit 1
}

foreach ($p in @($PanPath, $TrialPath)) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Output ("ARMOFFSET FAIL script-not-found " + $p); exit 2 }
}
$PanPath = (Resolve-Path -LiteralPath $PanPath).Path
$TrialPath = (Resolve-Path -LiteralPath $TrialPath).Path

$script:results = New-Object System.Collections.ArrayList
function Add-Result([string]$Name, [bool]$Ok, [string]$Detail) {
    [void]$script:results.Add([ordered]@{ name = $Name; ok = $Ok; detail = $Detail })
    $tag = if ($Ok) { 'PASS' } else { 'FAIL' }
    Write-Output ("ARMOFFSET CHECK {0} {1} :: {2}" -f $tag, $Name, $Detail)
}
function Get-FnText($Ast, [string]$Name) {
    $fn = $Ast.Find({ param($n) ($n -is [System.Management.Automation.Language.FunctionDefinitionAst]) -and ($n.Name -eq $Name) }, $true)
    if ($null -eq $fn) { return $null }
    return $fn.Extent.Text
}
function Get-Ast([string]$Path, [string]$Tag) {
    $t = $null; $e = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$t, [ref]$e)
    if (@($e).Count -gt 0) { Write-Output ("ARMOFFSET FAIL parse-" + $Tag + " :: " + (($e | ForEach-Object { $_.Message }) -join '; ')); exit 2 }
    return $ast
}
function Get-FnList($Ast, [string[]]$Names, [string]$Tag) {
    $parts = @()
    foreach ($n in $Names) {
        $txt = Get-FnText $Ast $n
        if ($null -eq $txt) { Write-Output ("ARMOFFSET FAIL missing-" + $Tag + "-" + $n); exit 2 }
        $parts += $txt
    }
    return ($parts -join "`n")
}
function Cal([object]$Ppt, [bool]$HasPpt = $true) {
    $c = [ordered]@{}
    if ($HasPpt) { $c['physical_pixels_per_logical_pixel'] = $Ppt }
    return $c
}
function Offset($C, [bool]$Max = $true) { return (Get-ArmPressOffset -MaximizedMode:$Max -Calibration $C) }

# ============================================================ PAN helper =======
# Dot-source at SCRIPT scope so the definitions are visible to every later call.
$panAst = Get-Ast $PanPath 'pan'
. ([scriptblock]::Create((Get-FnList $panAst @('Get-CalValue', 'Test-StrictJsonInt', 'Get-ArmPressOffset', 'Read-CalibrationBounds', 'Read-MaximizedCalibration') 'pan')))

Add-Result 'legacy-offset-4' ((Offset (Cal 1.491279) $false) -eq 4) ('legacy=' + [string](Offset (Cal 1.491279) $false))
Add-Result 'max-offset-8-at-pinned-1.491279' ((Offset (Cal 1.491279)) -eq 8) ('offset=' + [string](Offset (Cal 1.491279)))
Add-Result 'max-offset-6-at-scale-1' ((Offset (Cal 1.0)) -eq 6) ('offset=' + [string](Offset (Cal 1.0)))
Add-Result 'max-offset-8-at-scale-1.5' ((Offset (Cal 1.5)) -eq 8) ('offset=' + [string](Offset (Cal 1.5)))
Add-Result 'max-offset-ceil-margin-small-scale' ((Offset (Cal 0.1)) -eq 3) ('offset=' + [string](Offset (Cal 0.1)))
Add-Result 'max-offset-null-missing' ($null -eq (Offset (Cal $null $false))) 'missing scale -> null'
Add-Result 'max-offset-null-nan' ($null -eq (Offset (Cal ([double]::NaN)))) 'NaN scale -> null'
Add-Result 'max-offset-null-infinity' ($null -eq (Offset (Cal ([double]::PositiveInfinity)))) 'Infinity scale -> null'
Add-Result 'max-offset-null-zero' ($null -eq (Offset (Cal 0.0))) 'zero scale -> null'
Add-Result 'max-offset-null-negative' ($null -eq (Offset (Cal (-1.5)))) 'negative scale -> null'
Add-Result 'max-offset-null-too-large' ($null -eq (Offset (Cal 1001.0))) '>1000 scale -> null'
Add-Result 'max-offset-null-string' ($null -eq (Offset (Cal '1.5'))) 'string scale -> null'
Add-Result 'max-offset-null-bool' ($null -eq (Offset (Cal $true))) 'boolean scale -> null'
Add-Result 'max-offset-null-array' ($null -eq (Offset (Cal @(1.5, 2.0)))) 'array scale -> null'
Add-Result 'max-offset-null-object' ($null -eq (Offset (Cal ([pscustomobject]@{ a = 1 })))) 'object scale -> null'
Add-Result 'max-offset-null-nullcalibration' ($null -eq (Offset $null)) 'null calibration -> null'

$tmpDir = Join-Path ([IO.Path]::GetTempPath()) ('vaoffset-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $tmpDir -Force
$script:calSeq = 0
function New-CalFile($Object) {
    $script:calSeq++
    $path = Join-Path $tmpDir ('cal-' + $script:calSeq + '.json')
    [IO.File]::WriteAllText($path, ($Object | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    return $path
}
function New-GoodCal {
    return [ordered]@{
        schema = 'vacards-maximized-drag-calibration/1'
        calibration_id = 'cal-arm-offset-0001'
        monitor_device = '\\.\DISPLAY2'
        monitor_bounds = @(1920, 0, 5760, 2160)
        work_bounds = @(1920, 0, 5760, 2088)
        dpi = 96
        client_width = 3840
        client_height = 2088
        select_client_x = 1376
        select_client_y = 1317
        physical_pixels_per_logical_pixel = 1.491279
        canvas_scale = 1
    }
}
function Get-Sha([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Run-PanCal($Object) {
    $p = New-CalFile $Object
    return (Read-MaximizedCalibration -Path $p -ExpectedSha256 (Get-Sha $p))
}

$good = Run-PanCal (New-GoodCal)
Add-Result 'pan-accept-pinned-calibration' ([bool]$good.ok) ('ok=' + $good.ok + ' reason=' + [string]$good.reason)

$fwd = New-GoodCal; $fwd.select_client_x = 3
$x = Run-PanCal $fwd
Add-Result 'pan-reject-forward-press-beyond-client' ((-not $x.ok) -and ([string]$x.reason).Contains('arm press')) ('reason=' + [string]$x.reason)

$ret = New-GoodCal; $ret.select_client_x = 3715
$x = Run-PanCal $ret
Add-Result 'pan-reject-return-press-beyond-client' ((-not $x.ok) -and ([string]$x.reason).Contains('return press')) ('reason=' + [string]$x.reason)

$scale1 = New-GoodCal; $scale1.physical_pixels_per_logical_pixel = 1.0; $scale1.select_client_x = 3
$x = Run-PanCal $scale1
Add-Result 'pan-scale1-offset-6-forward-fails-at-3' ((-not $x.ok) -and ([string]$x.reason).Contains('arm press (select_client_x-6)')) ('reason=' + [string]$x.reason)
$scale1ok = New-GoodCal; $scale1ok.physical_pixels_per_logical_pixel = 1.0; $scale1ok.select_client_x = 6
$x = Run-PanCal $scale1ok
Add-Result 'pan-scale1-offset-6-forward-ok-at-6' ([bool]$x.ok) ('ok=' + $x.ok + ' reason=' + [string]$x.reason)

$missing = New-GoodCal; $missing.Remove('physical_pixels_per_logical_pixel')
$x = Run-PanCal $missing
Add-Result 'pan-reject-missing-scale' ((-not $x.ok) -and ([string]$x.reason).Contains('physical_pixels_per_logical_pixel')) ('reason=' + [string]$x.reason)

$badScale = New-GoodCal; $badScale.physical_pixels_per_logical_pixel = 'nope'
$x = Run-PanCal $badScale
Add-Result 'pan-reject-string-scale' ((-not $x.ok) -and ([string]$x.reason).Contains('physical_pixels_per_logical_pixel')) ('reason=' + [string]$x.reason)

# ============================================================ TRIAL helper =====
# The trial functions define the same Get-ArmPressOffset name; import them only
# after all Pan checks have recorded their evidence.
$trialAst = Get-Ast $TrialPath 'trial'
. ([scriptblock]::Create((Get-FnList $trialAst @('Get-FileSha256', 'Get-TrialCalValue', 'Test-TrialStrictInt', 'Get-ArmPressOffset', 'Read-TrialCalibrationBounds', 'Read-TrialMaximizedCalibration') 'trial')))

Add-Result 'trial-legacy-offset-4' ((Offset (Cal 1.491279) $false) -eq 4) ('legacy=' + [string](Offset (Cal 1.491279) $false))
Add-Result 'trial-max-offset-8-at-pinned-1.491279' ((Offset (Cal 1.491279)) -eq 8) ('offset=' + [string](Offset (Cal 1.491279)))
Add-Result 'trial-max-offset-6-at-scale-1' ((Offset (Cal 1.0)) -eq 6) ('offset=' + [string](Offset (Cal 1.0)))
Add-Result 'trial-max-offset-null-missing' ($null -eq (Offset (Cal $null $false))) 'missing scale -> null'
Add-Result 'trial-max-offset-null-nan' ($null -eq (Offset (Cal ([double]::NaN)))) 'NaN scale -> null'
Add-Result 'trial-max-offset-null-string' ($null -eq (Offset (Cal '1.5'))) 'string scale -> null'

function Run-TrialCal($Object) {
    $p = New-CalFile $Object
    return (Read-TrialMaximizedCalibration -Path $p -ExpectedSha256 (Get-Sha $p))
}
$tgoods = Run-TrialCal (New-GoodCal)
Add-Result 'trial-accept-pinned-calibration' ([bool]$tgoods.ok) ('ok=' + $tgoods.ok + ' reason=' + [string]$tgoods.reason)
$tfwd = New-GoodCal; $tfwd.select_client_x = 3
$x = Run-TrialCal $tfwd
Add-Result 'trial-reject-forward-press-beyond-client' ((-not $x.ok) -and ([string]$x.reason).Contains('arm press')) ('reason=' + [string]$x.reason)
$tret = New-GoodCal; $tret.select_client_x = 3715
$x = Run-TrialCal $tret
Add-Result 'trial-reject-return-press-beyond-client' ((-not $x.ok) -and ([string]$x.reason).Contains('return press')) ('reason=' + [string]$x.reason)

# ============================================================ static wiring ===
$panSrc = [IO.File]::ReadAllText($PanPath); $trialSrc = [IO.File]::ReadAllText($TrialPath)
Add-Result 'pan-shared-helper-used-forward' ($panSrc.Contains('$armPressX = $selectX - $armOffset')) 'forward press derives from the shared offset'
Add-Result 'pan-shared-helper-used-return' ($panSrc.Contains('-PressX ($selectX + 120 + $armOffset)')) 'return press derives from the shared offset'
Add-Result 'pan-helper-failclosed' ($panSrc.Contains('arm press offset could not be derived')) 'null offset fails closed before input'
Add-Result 'trial-helper-defined' ($trialSrc.Contains('function Get-ArmPressOffset')) 'trial mirrors the helper'
Add-Result 'pan-anchors-unchanged' ($panSrc.Contains('-AnchorX $selectX') -and $panSrc.Contains('-AnchorX ($selectX + 120)')) 'fixed anchors unchanged'
Add-Result 'pan-120px-travel-unchanged' ($panSrc.Contains('$dragEndX   = $dragStartX + 120') -and $panSrc.Contains('FromX ($selectX + 120)')) '120 px travel unchanged'

# ------------------------------------------------------------------ report --
$failed = @($script:results | Where-Object { -not $_.ok })
$summary = [ordered]@{
    schema        = 'vacards-app-pan-arm-offset-test/1'
    pan           = $PanPath
    pan_sha256    = (Get-FileHash -LiteralPath $PanPath -Algorithm SHA256).Hash.ToLowerInvariant()
    trial         = $TrialPath
    trial_sha256  = (Get-FileHash -LiteralPath $TrialPath -Algorithm SHA256).Hash.ToLowerInvariant()
    total         = $script:results.Count
    failed        = $failed.Count
    results       = $script:results
}
if (-not [string]::IsNullOrEmpty($OutputJson)) {
    [IO.File]::WriteAllText($OutputJson, ($summary | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
}
try { Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue } catch {}
if ($failed.Count -gt 0) {
    foreach ($f in $failed) { Write-Output ("ARMOFFSET FAILED {0} :: {1}" -f $f.name, $f.detail) }
    Write-Output ("ARMOFFSET FAIL failed={0} total={1}" -f $failed.Count, $script:results.Count)
    exit 1
}
Write-Output ("ARMOFFSET PASS total={0}" -f $script:results.Count)
exit 0
