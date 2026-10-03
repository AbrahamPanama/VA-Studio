#Requires -Version 5.1
# SPDX-License-Identifier: GPL-2.0-or-later
<#
  Focused OUTCOME test for the band-aware visual comparator added by
  next4-diagnostic-admission-repair (REVIEW F1). The top toolbar band is
  y < 160 physical client pixels; every pixel at y >= 160 must be EXACTLY equal
  and tolerance (<=1/channel in <=32 pixels) applies only inside the band.
  Synthetic PNGs are compared through the ACTUAL Compare-BandAwareImage helper
  extracted from Invoke-AppVisualRegression.ps1 and the ACTUAL embedded
  VAVisualDiffSource. Cases:
    - exact copy -> MATCH;
    - 1px delta1 at y=50 (toolbar) -> MATCH_UI_TOLERANCE;
    - 1px delta1 at y=159 -> MATCH_UI_TOLERANCE;
    - same 1px delta1 at y=160 (canvas) -> PIXEL_MISMATCH (outside band exact);
    - 33px delta1 inside toolbar -> PIXEL_MISMATCH (over tolerance count);
    - 1px delta2 inside toolbar -> PIXEL_MISMATCH (over channel tolerance);
    - 1px delta1 at y=199 -> PIXEL_MISMATCH.
  Exits 0 only when every check passes.
#>
param(
    [Parameter(Mandatory = $true)][string]$VisualPath,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$script:pass = 0
$script:fail = 0
function Check([string]$Name, [bool]$Cond, [string]$Detail) {
    if ($Cond) { $script:pass++; Write-Host ('PASS ' + $Name) }
    else { $script:fail++; Write-Host ('FAIL ' + $Name + ' :: ' + $Detail) }
}

$raw = Get-Content -Raw -LiteralPath $VisualPath
Check 'fixed band height 160 present' ($raw -match '\$script:toolbarBandHeightPx\s*=\s*160') 'no fixed 160 band constant'
$srcMatch = [regex]::Match($raw, '(?s)\$VAVisualDiffSource\s*=\s*@''\r?\n(.*?)\r?\n''@')
Check 'embedded VAVisualDiffSource extracted' $srcMatch.Success 'regex did not match'
if (-not $srcMatch.Success) { Write-Host ('RESULT pass=' + $script:pass + ' fail=' + $script:fail); exit 1 }
Add-Type -AssemblyName System.Drawing -ErrorAction Stop | Out-Null
Add-Type -TypeDefinition $srcMatch.Groups[1].Value -Language CSharp -ReferencedAssemblies 'System.Drawing'

$t = $null; $e = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($VisualPath, [ref]$t, [ref]$e)
if (@($e).Count -gt 0) { Write-Host ('FAIL visual parse :: ' + (($e | ForEach-Object { $_.Message }) -join '; ')); exit 1 }
$fn = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Compare-BandAwareImage' }, $true))
Check 'Compare-BandAwareImage found' ($fn.Count -eq 1) ('found ' + $fn.Count)
foreach ($f in $fn) { Invoke-Expression $f.Extent.Text }

$work = Join-Path $WorkDir 'band-image'
if (-not (Test-Path -LiteralPath $work)) { $null = New-Item -ItemType Directory -Path $work -Force }
$W = 64; $H = 200

function New-Png([string]$Name, [int[]]$Pixels, [int]$Delta) {
    # Pixels is a flat list x,y,x,y,... each painted white-with-blue-delta.
    $bmp = New-Object System.Drawing.Bitmap($W, $H)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $g.Dispose()
    for ($i = 0; $i + 1 -lt $Pixels.Count; $i += 2) {
        $bmp.SetPixel([int]$Pixels[$i], [int]$Pixels[$i + 1], [System.Drawing.Color]::FromArgb(255, 255, 255, (255 - $Delta)))
    }
    $p = Join-Path $work $Name
    $bmp.Save($p, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $p
}
function Invoke-BandCompare([string]$A, [string]$B, [int]$Tol = 32, [int]$Chan = 1) {
    return (Compare-BandAwareImage -PathA $A -PathB $B -BandHeightPx 160 -UiToleranceChannel $Chan -UiTolerancePixels $Tol)
}

$base = New-Png 'base.png' @() 0
$exact = New-Png 'exact.png' @() 0
$toolbar1 = New-Png 'toolbar1.png' @(10, 50) 1
$toolbar159 = New-Png 'toolbar159.png' @(11, 159) 1
$canvas160 = New-Png 'canvas160.png' @(10, 160) 1
$canvas199 = New-Png 'canvas199.png' @(10, 199) 1
$many = @(); 0..32 | ForEach-Object { $many += @($_, 60) }
$toolbar33 = New-Png 'toolbar33.png' $many 1
$delta2 = New-Png 'delta2.png' @(12, 70) 2

$rExact = Invoke-BandCompare $base $exact
Check 'exact copy -> MATCH' ([string]$rExact['status'] -eq 'MATCH') ([string]$rExact['status'])
Check 'exact outsideBandDiffPixels=0' ([int]$rExact['outsideBandDiffPixels'] -eq 0) ([string]$rExact['outsideBandDiffPixels'])

$rT1 = Invoke-BandCompare $base $toolbar1
Check '1px delta1 toolbar -> MATCH_UI_TOLERANCE' ([string]$rT1['status'] -eq 'MATCH_UI_TOLERANCE') ([string]$rT1['status'])
Check '1px delta1 toolbar inside small=1' ([int]$rT1['insideBandSmallDiffPixels'] -eq 1) ([string]$rT1['insideBandSmallDiffPixels'])
Check '1px delta1 toolbar outside=0' ([int]$rT1['outsideBandDiffPixels'] -eq 0) ([string]$rT1['outsideBandDiffPixels'])

$rT159 = Invoke-BandCompare $base $toolbar159
Check '1px delta1 at y=159 -> MATCH_UI_TOLERANCE' ([string]$rT159['status'] -eq 'MATCH_UI_TOLERANCE') ([string]$rT159['status'])

$rC160 = Invoke-BandCompare $base $canvas160
Check '1px delta1 at y=160 -> PIXEL_MISMATCH' ([string]$rC160['status'] -eq 'PIXEL_MISMATCH') ([string]$rC160['status'])
Check 'y=160 counted outside band' ([int]$rC160['outsideBandDiffPixels'] -eq 1) ([string]$rC160['outsideBandDiffPixels'])

$rC199 = Invoke-BandCompare $base $canvas199
Check '1px delta1 at y=199 -> PIXEL_MISMATCH' ([string]$rC199['status'] -eq 'PIXEL_MISMATCH') ([string]$rC199['status'])

$r33 = Invoke-BandCompare $base $toolbar33
Check '33px delta1 toolbar -> PIXEL_MISMATCH' ([string]$r33['status'] -eq 'PIXEL_MISMATCH') ([string]$r33['status'])
Check '33px small count=33' ([int]$r33['insideBandSmallDiffPixels'] -eq 33) ([string]$r33['insideBandSmallDiffPixels'])

$rD2 = Invoke-BandCompare $base $delta2
Check '1px delta2 toolbar -> PIXEL_MISMATCH' ([string]$rD2['status'] -eq 'PIXEL_MISMATCH') ([string]$rD2['status'])
Check 'delta2 counted as large' ([int]$rD2['insideBandLargeDiffPixels'] -eq 1) ([string]$rD2['insideBandLargeDiffPixels'])

Write-Host ''
Write-Host ('RESULT pass=' + $script:pass + ' fail=' + $script:fail)
if ($script:fail -gt 0) { exit 1 }
exit 0
