#Requires -Version 5.1
# SPDX-License-Identifier: GPL-2.0-or-later
<#
  Focused OUTCOME test that Invoke-AppTrial.ps1 Test-PanSummary INDEPENDENTLY
  revalidates observer metadata coverage against its own phase-boundaries input
  window (REVIEW F2). The ACTUAL Test-PanSummary and its helpers are extracted
  from the Trial script; a synthetic pan summary is fed through it. Cases:
    - valid full coverage -> no observer-coverage error;
    - qpc_end before trial input_end -> fail;
    - qpc_frequency mismatch -> fail;
    - cap exit_reason -> fail;
    - fatal=true -> fail;
    - missing/null metadata fields -> fail;
    - helper complete_coverage=false -> fail.
  Exits 0 only when every check passes.
#>
param(
    [Parameter(Mandatory = $true)][string]$TrialPath,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$script:pass = 0
$script:fail = 0
function Check([string]$Name, [bool]$Cond, [string]$Detail) {
    if ($Cond) { $script:pass++; Write-Host ('PASS ' + $Name) }
    else { $script:fail++; Write-Host ('FAIL ' + $Name + ' :: ' + $Detail) }
}

$t = $null; $e = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($TrialPath, [ref]$t, [ref]$e)
if (@($e).Count -gt 0) { Write-Host ('FAIL Trial parse :: ' + (($e | ForEach-Object { $_.Message }) -join '; ')); exit 1 }
$fnNames = @('Get-JsonProperty', 'Get-JsonPropertyRaw', 'Get-TrialCalValue', 'Test-TrialStrictInt', 'Test-PanSummary')
$fns = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and ($fnNames -contains $n.Name) }, $true))
Check 'trial helpers found' ($fns.Count -eq 5) ('found ' + $fns.Count)
foreach ($f in $fns) { Invoke-Expression $f.Extent.Text }

$work = Join-Path $WorkDir 'trial-coverage'
if (-not (Test-Path -LiteralPath $work)) { $null = New-Item -ItemType Directory -Path $work -Force }

$inStart = [long]10000000
$inEnd = [long]10009000
$freq = [long]10000000

function New-Observer {
    param($Schema, $Freq, $Start, $Ready, $End, $Reason, $Fatal, $ExitCode, $CompleteCoverage, $Frames, $Valid)
    return [ordered]@{
        ready_seen = $true; exit_code = 0; frames_written = $Frames; valid_frames = $Valid
        forced_kill = $false; exited = $true; errors = @()
        complete_coverage = $CompleteCoverage
        metadata_schema = $Schema; metadata_qpc_frequency = $Freq
        metadata_qpc_start = $Start; metadata_qpc_ready = $Ready; metadata_qpc_end = $End
        metadata_exit_code = $ExitCode; metadata_exit_reason = $Reason; metadata_fatal = $Fatal
    }
}
function New-Summary($Observer) {
    $s = [ordered]@{
        status = 'ok'; exit_code = 0
        target_process = [ordered]@{ id = 4242 }
        hwnd = '0x1234'; client_after = @(800, 600); dpi = 96
        window_mode = 'normal-client'; injected_event_count = 10
        window_duration_seconds = 18.0
        cpu_seconds_before = 1.0; cpu_seconds_after = 2.0; cpu_seconds_delta = 1.0
        session = [ordered]@{ target_session = 1; active_console_session = 1; client_protocol = 0; is_remote = 0 }
        clock_contract = [ordered]@{ status = 'ok'; sample_count = 20; all_in_bracket = $true }
        phase_boundaries = [ordered]@{ complete = $true; qpc_frequency = $freq; input_start_qpc = $inStart; input_end_qpc = $inEnd; input_start_utc = 't0'; input_end_utc = 't1' }
        observer = $Observer
    }
    return ($s | ConvertTo-Json -Depth 8 | ConvertFrom-Json)
}
function Run-Pan($Observer) {
    $summary = New-Summary $Observer
    return (Test-PanSummary -Summary $summary -ExpectedEventCount 10 -AppPid 4242 -ExpectedHwnd 0x1234 -ClientWidth 800 -ClientHeight 600 -ConsoleSession 1 -ExpectedWindowMode 'normal-client' -ObserverRequested $true)
}
function ObserverErrors($Result) { return (@($Result.errors) -join ' || ') }

$obsErrPattern = 'observer\.(metadata_|complete_coverage)'

# 1. Valid full coverage -> no observer-coverage error from the trial.
$good = New-Observer 'vacards.screen-observer/1' $freq ($inStart - 1000) ($inStart - 100) ($inEnd + 1000) 'stop_file' $false 0 $true 9 3
$r1 = Run-Pan $good
Check 'valid coverage -> no trial observer-coverage error' (-not ((ObserverErrors $r1) -match $obsErrPattern)) (ObserverErrors $r1)

# 2. qpc_end before the trial input_end -> fail.
$early = New-Observer 'vacards.screen-observer/1' $freq ($inStart - 1000) ($inStart - 100) ($inEnd - 1) 'stop_file' $false 0 $true 9 3
$r2 = Run-Pan $early
Check 'early qpc_end -> trial fail' ((ObserverErrors $r2) -match 'metadata_qpc_end is before phase_boundaries\.input_end_qpc') (ObserverErrors $r2)

# 3. qpc_frequency mismatch -> fail.
$freqBad = New-Observer 'vacards.screen-observer/1' ($freq + 1) ($inStart - 1000) ($inStart - 100) ($inEnd + 1000) 'stop_file' $false 0 $true 9 3
$r3 = Run-Pan $freqBad
Check 'frequency mismatch -> trial fail' ((ObserverErrors $r3) -match 'metadata_qpc_frequency does not match phase_boundaries\.qpc_frequency') (ObserverErrors $r3)

# 4. Cap exit_reason -> fail.
$cap = New-Observer 'vacards.screen-observer/1' $freq ($inStart - 1000) ($inStart - 100) ($inEnd + 1000) 'max_duration' $false 0 $true 9 3
$r4 = Run-Pan $cap
Check 'cap reason -> trial fail' ((ObserverErrors $r4) -match 'metadata_exit_reason is not stop_file') (ObserverErrors $r4)

# 5. fatal=true -> fail.
$fatal = New-Observer 'vacards.screen-observer/1' $freq ($inStart - 1000) ($inStart - 100) ($inEnd + 1000) 'stop_file' $true 0 $true 9 3
$r5 = Run-Pan $fatal
Check 'fatal=true -> trial fail' ((ObserverErrors $r5) -match 'metadata_fatal is not strictly false') (ObserverErrors $r5)

# 6. Missing/null metadata fields -> fail closed.
$missing = New-Observer $null $null $null $null $null $null $null $null $false 9 3
$r6 = Run-Pan $missing
Check 'null metadata -> trial fail' ((ObserverErrors $r6) -match 'metadata_schema is not vacards\.screen-observer/1') (ObserverErrors $r6)

# 7. Helper complete_coverage=false -> fail even if raw fields look valid.
$flag = New-Observer 'vacards.screen-observer/1' $freq ($inStart - 1000) ($inStart - 100) ($inEnd + 1000) 'stop_file' $false 0 $false 9 3
$r7 = Run-Pan $flag
Check 'complete_coverage=false -> trial fail' ((ObserverErrors $r7) -match 'observer\.complete_coverage is not true') (ObserverErrors $r7)

# 8. qpc_start after trial input_start -> fail.
$late = New-Observer 'vacards.screen-observer/1' $freq ($inStart + 1) ($inStart + 2) ($inEnd + 1000) 'stop_file' $false 0 $true 9 3
$r8 = Run-Pan $late
Check 'late qpc_start -> trial fail' ((ObserverErrors $r8) -match 'metadata_qpc_start is after phase_boundaries\.input_start_qpc') (ObserverErrors $r8)

# 9. qpc_ready after trial input_start (warmup into the measured window) -> fail.
$readyLate = New-Observer 'vacards.screen-observer/1' $freq ($inStart - 1000) ($inStart + 1) ($inEnd + 1000) 'stop_file' $false 0 $true 9 3
$r9 = Run-Pan $readyLate
Check 'late qpc_ready -> trial fail' ((ObserverErrors $r9) -match 'metadata_qpc_ready is after phase_boundaries\.input_start_qpc') (ObserverErrors $r9)

Write-Host ''
Write-Host ('RESULT pass=' + $script:pass + ' fail=' + $script:fail)
if ($script:fail -gt 0) { exit 1 }
exit 0
