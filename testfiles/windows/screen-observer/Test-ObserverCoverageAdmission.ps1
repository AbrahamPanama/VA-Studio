#Requires -Version 5.1
# SPDX-License-Identifier: GPL-2.0-or-later
<#
  Focused OUTCOME test for the observer metadata coverage admission added by
  next4-diagnostic-admission-repair (REVIEW F2). No GUI/input/real observer: a
  sleeping child stands in for the owned observer and synthetic
  observer-metadata.json / frames.csv are written. Requires the helper to prove
  the observer's own QPC lifetime covered the measured input window:
    - valid stop_file metadata with full coverage is admitted;
    - early qpc_end (< input_end) fails;
    - a cap exit_reason (max_duration/max_frames/max_output_bytes) fails;
    - fatal=true fails;
    - missing metadata fails;
    - qpc_frequency mismatch fails;
    - a legitimate zero-motion (content_changes=0) run still passes;
    - warmup-only (valid=0), qpc_start after input_start and null window fail.
  Exits 0 only when every check passes.
#>
param(
    [Parameter(Mandatory = $true)][string]$PanPath,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$script:pass = 0
$script:fail = 0
function Check([string]$Name, [bool]$Cond, [string]$Detail) {
    if ($Cond) { $script:pass++; Write-Host ('PASS ' + $Name) }
    else { $script:fail++; Write-Host ('FAIL ' + $Name + ' :: ' + $Detail) }
}

# Minimal QPC host type used by Stop-ScreenObserver / coverage checks.
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
namespace VAPan {
    public static class Win32 {
        [DllImport("kernel32.dll")] public static extern bool QueryPerformanceCounter(out long v);
        [DllImport("kernel32.dll")] public static extern bool QueryPerformanceFrequency(out long v);
        public static long Qpc() { long v; QueryPerformanceCounter(out v); return v; }
        public static long QpcFrequency() { long v; QueryPerformanceFrequency(out v); return v; }
    }
}
'@

$t = $null; $e = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($PanPath, [ref]$t, [ref]$e)
if (@($e).Count -gt 0) { Write-Host ('FAIL Pan parse :: ' + (($e | ForEach-Object { $_.Message }) -join '; ')); exit 1 }
$fnNames = @('Get-CalValue', 'Test-StrictJsonInt', 'Test-ObserverMetadataCoverage', 'Stop-ScreenObserver')
$fns = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and ($fnNames -contains $n.Name) }, $true))
Check 'admission helpers found' ($fns.Count -eq 4) ('found ' + $fns.Count)
foreach ($f in $fns) { Invoke-Expression $f.Extent.Text }

$work = Join-Path $WorkDir 'stop-coverage'
if (-not (Test-Path -LiteralPath $work)) { $null = New-Item -ItemType Directory -Path $work -Force }

$freq = [long][VAPan.Win32]::QpcFrequency()
$base = [long]10000000
$inStart = $base + 1000
$inEnd = $base + 9000

function New-Meta {
    param(
        [long]$Start, [long]$Ready, [long]$End, [string]$Reason,
        $Fatal, $ExitCode, $Schema, $Freq, $Frames, $Valid, $Changes
    )
    return ([ordered]@{
        schema = $Schema; qpc_frequency = $Freq
        qpc_start = $Start; qpc_ready = $Ready; qpc_end = $End
        exit_code = $ExitCode; exit_reason = $Reason; fatal = $Fatal
        counts = [ordered]@{ frames_written = $Frames; valid = $Valid; missing = 1; merged = 0; ambiguous = 0; content_changes = $Changes }
    } | ConvertTo-Json -Depth 5 -Compress)
}
function New-DummyObserver {
    return (Start-Process -FilePath 'powershell.exe' -ArgumentList '-NoProfile -Command "Start-Sleep -Seconds 60"' -PassThru -WindowStyle Hidden)
}
function Write-OutDir([string]$Name, [string]$MetadataJson, [bool]$WriteMetadata = $true) {
    $d = Join-Path $work $Name
    if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
    $null = New-Item -ItemType Directory -Path $d -Force
    if ($WriteMetadata) { [IO.File]::WriteAllText((Join-Path $d 'observer-metadata.json'), $MetadataJson, [System.Text.UTF8Encoding]::new($false)) }
    [IO.File]::WriteAllText((Join-Path $d 'frames.csv'), "seq,phase`n1,warmup`n", [System.Text.UTF8Encoding]::new($false))
    return $d
}
function Invoke-Stop([string]$OutDir, $InStart, $InEnd) {
    $script:observerProcess = New-DummyObserver
    $script:observerStopFile = Join-Path $OutDir 'observer-stop'
    $script:observerOutDir = $OutDir
    $script:observerOutStream = $null; $script:observerErrStream = $null
    $script:observerOutTask = $null; $script:observerErrTask = $null
    $script:observerCpuBefore = $null
    return (Stop-ScreenObserver -StopTimeoutMs 300 -InputStartQpc $InStart -InputEndQpc $InEnd)
}
function ErrorsText($Stop) { return (@($Stop['errors']) -join ' || ') }
function CoverageText($Stop) { return (@($Stop['coverage_reasons']) -join ' || ') }

# 1. Valid stop_file with full coverage and zero motion -> admitted coverage.
$mdGood = New-Meta $base ($base + 900) ($base + 10000) 'stop_file' $false 0 'vacards.screen-observer/1' $freq 9 3 0
$d1 = Write-OutDir 'valid-stop-file' $mdGood
$s1 = Invoke-Stop $d1 $inStart $inEnd
Check 'valid stop_file full coverage admitted' ((CoverageText $s1) -eq '') (CoverageText $s1)
Check 'valid stop_file complete_coverage=true' ($s1['complete_coverage'] -eq $true) ([string]$s1['complete_coverage'])
Check 'coverage start stored' ([long]$s1['metadata_qpc_start'] -eq $base) ([string]$s1['metadata_qpc_start'])
Check 'coverage ready stored' ([long]$s1['metadata_qpc_ready'] -eq ($base + 900)) ([string]$s1['metadata_qpc_ready'])
Check 'coverage end stored' ([long]$s1['metadata_qpc_end'] -eq ($base + 10000)) ([string]$s1['metadata_qpc_end'])
Check 'coverage input window stored' (([long]$s1['metadata_input_start_qpc'] -eq $inStart) -and ([long]$s1['metadata_input_end_qpc'] -eq $inEnd)) ([string]$s1['metadata_input_start_qpc'])
Check 'coverage start_within_input flag' ($s1['coverage_start_within_input'] -eq $true) ([string]$s1['coverage_start_within_input'])
Check 'coverage end_covers_input flag' ($s1['coverage_end_covers_input'] -eq $true) ([string]$s1['coverage_end_covers_input'])
Check 'coverage ready_before_input flag' ($s1['coverage_ready_before_input'] -eq $true) ([string]$s1['coverage_ready_before_input'])
Check 'coverage frequency matches flag' ($s1['coverage_frequency_matches'] -eq $true) ([string]$s1['coverage_frequency_matches'])
Check 'coverage exit_reason stored' ([string]$s1['metadata_exit_reason'] -eq 'stop_file') ([string]$s1['metadata_exit_reason'])
Check 'coverage schema stored' ([string]$s1['metadata_schema'] -eq 'vacards.screen-observer/1') ([string]$s1['metadata_schema'])

# 2. Early observer end (cap hit / early exit) -> inadmissible coverage.
$mdEarly = New-Meta $base ($base + 900) ($inEnd - 1) 'stop_file' $false 0 'vacards.screen-observer/1' $freq 9 3 0
$d2 = Write-OutDir 'early-end' $mdEarly
$s2 = Invoke-Stop $d2 $inStart $inEnd
Check 'early qpc_end fails coverage' ((CoverageText $s2) -match 'qpc_end is before input_end_qpc') (CoverageText $s2)
Check 'early qpc_end complete_coverage=false' ($s2['complete_coverage'] -eq $false) ([string]$s2['complete_coverage'])

# 3. Cap exit reason -> not exactly stop_file.
foreach ($cap in @('max_duration', 'max_frames', 'max_output_bytes')) {
    $mdCap = New-Meta $base ($base + 900) ($base + 10000) $cap $false 0 'vacards.screen-observer/1' $freq 9 3 0
    $d3 = Write-OutDir ('cap-' + $cap) $mdCap
    $s3 = Invoke-Stop $d3 $inStart $inEnd
    Check ('cap reason ' + $cap + ' fails') ((CoverageText $s3) -match 'exit_reason is not stop_file') (CoverageText $s3)
}

# 4. fatal=true -> inadmissible.
$mdFatal = New-Meta $base ($base + 900) ($base + 10000) 'stop_file' $true 0 'vacards.screen-observer/1' $freq 9 3 0
$d4 = Write-OutDir 'fatal' $mdFatal
$s4 = Invoke-Stop $d4 $inStart $inEnd
Check 'fatal=true fails coverage' ((CoverageText $s4) -match 'fatal is not strictly false') (CoverageText $s4)

# 5. Missing metadata file -> fail closed.
$d5 = Write-OutDir 'missing-metadata' '' $false
$s5 = Invoke-Stop $d5 $inStart $inEnd
Check 'missing metadata fails' ((ErrorsText $s5) -match 'observer-metadata\.json is missing') (ErrorsText $s5)
Check 'missing metadata complete_coverage=false' ($s5['complete_coverage'] -eq $false) ([string]$s5['complete_coverage'])

# 6. qpc_frequency mismatch -> inadmissible.
$mdFreq = New-Meta $base ($base + 900) ($base + 10000) 'stop_file' $false 0 'vacards.screen-observer/1' ($freq + 1) 9 3 0
$d6 = Write-OutDir 'freq-mismatch' $mdFreq
$s6 = Invoke-Stop $d6 $inStart $inEnd
Check 'frequency mismatch fails coverage' ((CoverageText $s6) -match 'qpc_frequency does not match') (CoverageText $s6)

# 7. Legitimate zero motion (content_changes 0) full coverage -> admitted.
$mdStall = New-Meta $base ($base + 900) ($base + 10000) 'stop_file' $false 0 'vacards.screen-observer/1' $freq 9 3 0
$d7 = Write-OutDir 'legit-zero-motion' $mdStall
$s7 = Invoke-Stop $d7 $inStart $inEnd
Check 'zero motion full coverage admitted' ((CoverageText $s7) -eq '') (CoverageText $s7)
Check 'zero motion complete_coverage=true' ($s7['complete_coverage'] -eq $true) ([string]$s7['complete_coverage'])
Check 'zero motion not rejected for motion' (-not ((ErrorsText $s7) -match 'content_change')) (ErrorsText $s7)

# 8. Preserved: warmup-only (valid=0) rejected even with frames_written>0.
$mdWarm = New-Meta $base ($base + 900) ($base + 10000) 'stop_file' $false 0 'vacards.screen-observer/1' $freq 5 0 0
$d8 = Write-OutDir 'warmup-only' $mdWarm
$s8 = Invoke-Stop $d8 $inStart $inEnd
Check 'warmup-only valid=0 rejected' ((ErrorsText $s8) -match 'no valid measured-loop frames') (ErrorsText $s8)

# 9. qpc_start after input_start -> inadmissible.
$mdLate = New-Meta ($inStart + 1) ($base + 900) ($base + 10000) 'stop_file' $false 0 'vacards.screen-observer/1' $freq 9 3 0
$d9 = Write-OutDir 'late-start' $mdLate
$s9 = Invoke-Stop $d9 $inStart $inEnd
Check 'qpc_start after input_start fails' ((CoverageText $s9) -match 'qpc_start is after input_start_qpc') (CoverageText $s9)

# 9b. qpc_ready after input_start (warmup extended into the measured window) -> inadmissible.
$mdReady = New-Meta $base ($inStart + 1) ($base + 10000) 'stop_file' $false 0 'vacards.screen-observer/1' $freq 9 3 0
$d9b = Write-OutDir 'late-ready' $mdReady
$s9b = Invoke-Stop $d9b $inStart $inEnd
Check 'qpc_ready after input_start fails' ((CoverageText $s9b) -match 'qpc_ready is after input_start_qpc') (CoverageText $s9b)
Check 'late ready complete_coverage=false' ($s9b['complete_coverage'] -eq $false) ([string]$s9b['complete_coverage'])
Check 'late ready raw fields retained' (([long]$s9b['metadata_qpc_ready'] -eq ($inStart + 1)) -and ([long]$s9b['metadata_input_start_qpc'] -eq $inStart)) ([string]$s9b['metadata_qpc_ready'])

# 10. Missing/null input window -> fail closed.
$d10 = Write-OutDir 'null-window' $mdGood
$s10 = Invoke-Stop $d10 $null $null
Check 'null input window fails' ((CoverageText $s10) -match 'input_start_qpc is missing') (CoverageText $s10)

# 11. Wrong schema -> inadmissible.
$mdSchema = New-Meta $base ($base + 900) ($base + 10000) 'stop_file' $false 0 'wrong/1' $freq 9 3 0
$d11 = Write-OutDir 'wrong-schema' $mdSchema
$s11 = Invoke-Stop $d11 $inStart $inEnd
Check 'wrong schema fails' ((CoverageText $s11) -match 'schema is not vacards.screen-observer/1') (CoverageText $s11)

# 12. Wrong-typed exit_code (string) -> inadmissible.
$mdStr = New-Meta $base ($base + 900) ($base + 10000) 'stop_file' $false '0' 'vacards.screen-observer/1' $freq 9 3 0
$d12 = Write-OutDir 'string-exit' $mdStr
$s12 = Invoke-Stop $d12 $inStart $inEnd
Check 'string exit_code fails' ((CoverageText $s12) -match 'exit_code is not 0') (CoverageText $s12)

# 13. Preserved: metadata hash failure (exclusive lock) invalidates.
$d13 = Write-OutDir 'hash-fail' $mdGood
$lock = [IO.File]::Open((Join-Path $d13 'observer-metadata.json'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
try { $s13 = Invoke-Stop $d13 $inStart $inEnd } finally { $lock.Dispose() }
Check 'metadata hash failure invalidates' ((ErrorsText $s13) -match 'observer-metadata\.json sha256 could not be computed') (ErrorsText $s13)

Write-Host ''
Write-Host ('RESULT pass=' + $script:pass + ' fail=' + $script:fail)
if ($script:fail -gt 0) { exit 1 }
exit 0
