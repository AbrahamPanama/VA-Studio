#Requires -Version 5.1
# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
  No-GUI, pure PS 5.1 tests for the observer diagnostic-array repair.

.DESCRIPTION
  Two independently verifiable contracts, both exercised through the ACTUAL
  extracted functions (no GUI, no observer, no app):

  A. PRODUCER (Invoke-AppPan.ps1 Set-ObserverSummary): errors and
     coverage_reasons are ALWAYS true JSON arrays. With no errors they serialize
     as [] (never {}), with one/multiple messages they serialize as JSON strings
     in order, and a real message is never suppressed. A PS 5.1
     ConvertTo-Json -> ConvertFrom-Json roundtrip is run for every case.

  B. CONSUMER (Invoke-AppTrial.ps1 Test-PanSummary): observer.errors is admitted
     only as a JSON array of non-blank strings. [] is allowed; a real message is
     surfaced (so the trial fails); {} / null / missing / scalar / non-string /
     blank elements fail explicitly with a non-blank diagnostic.

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
    Write-Output 'OBSERR FAIL unhandled'
    exit 1
}

foreach ($p in @($PanPath, $TrialPath)) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Output ("OBSERR FAIL script-not-found " + $p); exit 2 }
}
$PanPath = (Resolve-Path -LiteralPath $PanPath).Path
$TrialPath = (Resolve-Path -LiteralPath $TrialPath).Path

$script:results = New-Object System.Collections.ArrayList
function Add-Result([string]$Name, [bool]$Ok, [string]$Detail) {
    [void]$script:results.Add([ordered]@{ name = $Name; ok = $Ok; detail = $Detail })
    $tag = if ($Ok) { 'PASS' } else { 'FAIL' }
    Write-Output ("OBSERR CHECK {0} {1} :: {2}" -f $tag, $Name, $Detail)
}
function Get-FnText($ast, [string]$Name) {
    $fn = $ast.Find({ param($n) ($n -is [System.Management.Automation.Language.FunctionDefinitionAst]) -and ($n.Name -eq $Name) }, $true)
    if ($null -eq $fn) { return $null }
    return $fn.Extent.Text
}
function Get-Ast([string]$Path, [string]$Tag) {
    $t = $null; $e = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$t, [ref]$e)
    if (@($e).Count -gt 0) { Write-Output ("OBSERR FAIL parse-" + $Tag + " :: " + (($e | ForEach-Object { $_.Message }) -join '; ')); exit 2 }
    return $ast
}
function Get-FnList($Ast, [string[]]$Names, [string]$Tag) {
    $parts = @()
    foreach ($n in $Names) {
        $txt = Get-FnText $Ast $n
        if ($null -eq $txt) { Write-Output ("OBSERR FAIL missing-" + $Tag + "-" + $n); exit 2 }
        $parts += $txt
    }
    return ($parts -join "`n")
}

# =============================== PART A: producer roundtrip ===================
# Dot-source at SCRIPT scope so the definitions are visible to every later call.
$panAst = Get-Ast $PanPath 'pan'
. ([scriptblock]::Create((Get-FnList $panAst @('Set-ObserverSummary') 'pan')))

function New-Start {
    return [ordered]@{
        exe_actual_sha256 = 'a' * 64; ready_file = 'ready.json'; ready_seen = $true
        ready_qpc = 900L; ready_wait_ms = 12; process_id = 4242
        started_qpc = 800L; started_utc = 'start'; cpu_seconds_before = 4.0
    }
}
function New-Stop($Errors, $Coverage) {
    return [ordered]@{
        stopped_qpc = 1200L; stopped_utc = 'stop'; exited = $true; exit_code = 0
        forced_kill = $false; timed_out = $false; cpu_seconds_after = 5.0; cpu_seconds_delta = 1.0
        metadata_path = 'm.json'; metadata_sha256 = 'b' * 64; metadata_schema = 'vacards.screen-observer/1'
        metadata_qpc_frequency = 10000000L; metadata_qpc_start = 9999000L; metadata_qpc_ready = 9999900L; metadata_qpc_end = 10010000L
        metadata_exit_code = 0; metadata_exit_reason = 'stop_file'; metadata_fatal = $false
        metadata_input_start_qpc = 10000000L; metadata_input_end_qpc = 10009000L
        coverage_start_within_input = $true; coverage_end_covers_input = $true
        coverage_ready_within_lifetime = $true; coverage_ready_before_input = $true
        coverage_frequency_matches = $true; complete_coverage = $true
        coverage_reasons = $Coverage
        frames_csv_path = 'f.csv'; frames_csv_sha256 = 'c' * 64
        frames_written = 9; valid_frames = 3; missing_frames = 0; merged_frames = 0
        ambiguous_frames = 0; content_changes = 2
        errors = $Errors
    }
}
function New-Meta {
    return [ordered]@{ path = 'roi.json'; actual_sha256 = 'd' * 64; expected_sha256 = 'd' * 64; schema = 'vacards-screen-observer-roi/1'; roi_id = 'roi-1' }
}
function Run-Producer($Start, $Stop) {
    $script:summary = [ordered]@{}
    Set-ObserverSummary -Start $Start -Stop $Stop -Meta (New-Meta) -ExePath 'obs.exe' -ExpectedExeSha256 ('e' * 64) -OutDir 'out' -StopFile 'stop'
    return $script:summary['observer']
}
function Roundtrip($Observer) {
    $json = ($Observer | ConvertTo-Json -Depth 8 -Compress)
    return [ordered]@{ json = $json; parsed = ($json | ConvertFrom-Json) }
}

$r = Roundtrip (Run-Producer (New-Start) (New-Stop @() @()))
Add-Result 'producer-empty-errors-serialize-brackets' ($r.json -match '"errors":\[\]') $r.json.Substring(0, [Math]::Min(200, $r.json.Length))
Add-Result 'producer-empty-coverage-serialize-brackets' ($r.json -match '"coverage_reasons":\[\]') 'coverage_reasons is []'
Add-Result 'producer-empty-errors-not-object' (-not ($r.json -match '"errors":\{\}')) 'no {} for empty errors'
Add-Result 'producer-empty-errors-roundtrip-array' (($r.parsed.errors -is [System.Array]) -and (@($r.parsed.errors).Count -eq 0)) ('type=' + $r.parsed.errors.GetType().FullName)
Add-Result 'producer-empty-coverage-roundtrip-array' (($r.parsed.coverage_reasons -is [System.Array]) -and (@($r.parsed.coverage_reasons).Count -eq 0)) ('type=' + $r.parsed.coverage_reasons.GetType().FullName)

$r = Roundtrip (Run-Producer (New-Start) (New-Stop @('boom') @('cov one')))
Add-Result 'producer-one-error-array' (($r.parsed.errors -is [System.Array]) -and (@($r.parsed.errors).Count -eq 1) -and ([string]$r.parsed.errors[0] -eq 'boom')) $r.json
Add-Result 'producer-one-coverage-array' (($r.parsed.coverage_reasons -is [System.Array]) -and (@($r.parsed.coverage_reasons).Count -eq 1) -and ([string]$r.parsed.coverage_reasons[0] -eq 'cov one')) 'coverage one'

$r = Roundtrip (Run-Producer (New-Start) (New-Stop @('a', 'b', 'c') @('x', 'y')))
Add-Result 'producer-multiple-errors-array' (($r.parsed.errors -is [System.Array]) -and (@($r.parsed.errors).Count -eq 3) -and ((@($r.parsed.errors) -join '|') -eq 'a|b|c')) $r.json
Add-Result 'producer-multiple-coverage-array' (($r.parsed.coverage_reasons -is [System.Array]) -and (@($r.parsed.coverage_reasons).Count -eq 2) -and ((@($r.parsed.coverage_reasons) -join '|') -eq 'x|y')) 'coverage multiple'
Add-Result 'producer-real-message-not-suppressed' ($r.parsed.errors -contains 'b') 'real error retained'

# No stop -> still true arrays, never AutomationNull {}.
$r = Roundtrip (Run-Producer (New-Start) $null)
Add-Result 'producer-nostop-errors-array' (($r.json -match '"errors":\[\]') -and ($r.parsed.errors -is [System.Array])) $r.json.Substring(0, [Math]::Min(200, $r.json.Length))
Add-Result 'producer-nostop-coverage-array' (($r.json -match '"coverage_reasons":\[\]') -and ($r.parsed.coverage_reasons -is [System.Array])) 'no-stop coverage []'

# =============================== PART B: consumer strictness ==================
$trialAst = Get-Ast $TrialPath 'trial'
. ([scriptblock]::Create((Get-FnList $trialAst @('Get-JsonProperty', 'Get-JsonPropertyRaw', 'Get-TrialCalValue', 'Test-TrialStrictInt', 'Test-PanSummary') 'trial')))

$template = @'
{
  "status": "ok", "exit_code": 0,
  "target_process": { "id": 4242 },
  "hwnd": "0x1234", "client_after": [800, 600], "dpi": 96,
  "window_mode": "normal-client", "injected_event_count": 10,
  "window_duration_seconds": 18.0,
  "cpu_seconds_before": 1.0, "cpu_seconds_after": 2.0, "cpu_seconds_delta": 1.0,
  "session": { "target_session": 1, "active_console_session": 1, "client_protocol": 0, "is_remote": 0 },
  "clock_contract": { "status": "ok", "sample_count": 20, "all_in_bracket": true },
  "phase_boundaries": { "complete": true, "qpc_frequency": 10000000, "input_start_qpc": 10000000, "input_end_qpc": 10009000, "input_start_utc": "t0", "input_end_utc": "t1" },
  "observer": {
    "ready_seen": true, "exit_code": 0, "frames_written": 9, "valid_frames": 3,
    "forced_kill": false, "exited": true, "complete_coverage": true,
    "metadata_schema": "vacards.screen-observer/1", "metadata_qpc_frequency": 10000000,
    "metadata_qpc_start": 9999000, "metadata_qpc_ready": 9999900, "metadata_qpc_end": 10010000,
    "metadata_exit_code": 0, "metadata_exit_reason": "stop_file", "metadata_fatal": false,
    "errors": __ERRJSON__
  }
}
'@
function Run-Consumer([string]$ErrorsJson, [switch]$Omit) {
    $json = $template.Replace('__ERRJSON__', $ErrorsJson)
    $s = $json | ConvertFrom-Json
    if ($Omit) { $s.observer.PSObject.Properties.Remove('errors') }
    return (Test-PanSummary -Summary $s -ExpectedEventCount 10 -AppPid 4242 -ExpectedHwnd 0x1234 -ClientWidth 800 -ClientHeight 600 -ConsoleSession 1 -ExpectedWindowMode 'normal-client' -ObserverRequested $true)
}
function ObsMessages($Result) { return (@($Result.errors) -join ' || ') }
$obsErrPattern = 'observer\.errors|pan helper observer:'

$c = Run-Consumer '[]'
Add-Result 'consumer-empty-array-allowed' (-not ((ObsMessages $c) -match $obsErrPattern)) (ObsMessages $c)

$c = Run-Consumer '["real problem"]'
Add-Result 'consumer-real-message-surfaced' ((ObsMessages $c) -match 'pan helper observer: real problem') (ObsMessages $c)

$c = Run-Consumer '["first","second"]'
Add-Result 'consumer-multiple-messages-surfaced' (((ObsMessages $c) -match 'pan helper observer: first') -and ((ObsMessages $c) -match 'pan helper observer: second')) (ObsMessages $c)

$c = Run-Consumer '{}'
Add-Result 'consumer-object-malformed-fails-explicitly' (((ObsMessages $c) -match 'observer\.errors is not a JSON array') -and (-not ((ObsMessages $c) -match 'pan helper observer:\s*(\|\||$)'))) (ObsMessages $c)

$c = Run-Consumer 'null'
Add-Result 'consumer-null-fails-explicitly' ((ObsMessages $c) -match 'observer\.errors is missing or null') (ObsMessages $c)

$c = Run-Consumer '[]' -Omit
Add-Result 'consumer-missing-field-fails-explicitly' ((ObsMessages $c) -match 'observer\.errors is missing or null') (ObsMessages $c)

$c = Run-Consumer '42'
Add-Result 'consumer-scalar-fails-explicitly' ((ObsMessages $c) -match 'observer\.errors is not a JSON array') (ObsMessages $c)

$c = Run-Consumer '[42]'
Add-Result 'consumer-nonstring-element-fails' ((ObsMessages $c) -match 'observer\.errors\[0\] is not a non-blank string') (ObsMessages $c)

$c = Run-Consumer '[""]'
Add-Result 'consumer-blank-element-fails' ((ObsMessages $c) -match 'observer\.errors\[0\] is not a non-blank string') (ObsMessages $c)

$c = Run-Consumer '["   "]'
Add-Result 'consumer-whitespace-element-fails' ((ObsMessages $c) -match 'observer\.errors\[0\] is not a non-blank string') (ObsMessages $c)

$c = Run-Consumer '["ok",7]'
Add-Result 'consumer-mixed-elements-indexed' (((ObsMessages $c) -match 'pan helper observer: ok') -and ((ObsMessages $c) -match 'observer\.errors\[1\] is not a non-blank string')) (ObsMessages $c)

# ================= PART C: real producer -> JSON -> real consumer ============
# Root-required end-to-end chain: extract the ACTUAL Set-ObserverSummary output,
# ConvertTo-Json it, ConvertFrom-Json it, embed it as the observer block of a
# full pan summary and run the ACTUAL Test-PanSummary. No hand-written observer
# JSON is used here.
function New-FullSummary($Observer) {
    return [ordered]@{
        status = 'ok'; exit_code = 0
        target_process = [ordered]@{ id = 4242 }
        hwnd = '0x1234'; client_after = @(800, 600); dpi = 96
        window_mode = 'normal-client'; injected_event_count = 10
        window_duration_seconds = 18.0
        cpu_seconds_before = 1.0; cpu_seconds_after = 2.0; cpu_seconds_delta = 1.0
        session = [ordered]@{ target_session = 1; active_console_session = 1; client_protocol = 0; is_remote = 0 }
        clock_contract = [ordered]@{ status = 'ok'; sample_count = 20; all_in_bracket = $true }
        phase_boundaries = [ordered]@{ complete = $true; qpc_frequency = 10000000; input_start_qpc = 10000000; input_end_qpc = 10009000; input_start_utc = 't0'; input_end_utc = 't1' }
        observer = $Observer
    }
}
function Run-E2E($Observer) {
    $json = (New-FullSummary $Observer) | ConvertTo-Json -Depth 10 -Compress
    $parsed = $json | ConvertFrom-Json
    $result = Test-PanSummary -Summary $parsed -ExpectedEventCount 10 -AppPid 4242 -ExpectedHwnd 0x1234 -ClientWidth 800 -ClientHeight 600 -ConsoleSession 1 -ExpectedWindowMode 'normal-client' -ObserverRequested $true
    return [ordered]@{ json = $json; result = $result }
}

$e = Run-E2E (Run-Producer (New-Start) (New-Stop @() @()))
Add-Result 'e2e-producer-empty-json-brackets' ($e.json -match '"errors":\[\]') 'real producer emits [] in the full summary'
Add-Result 'e2e-producer-empty-consumer-admits' (-not ((ObsMessages $e.result) -match $obsErrPattern)) (ObsMessages $e.result)

$e = Run-E2E (Run-Producer (New-Start) (New-Stop @('boom') @()))
Add-Result 'e2e-producer-singleton-consumer-surfaces' ((ObsMessages $e.result) -match 'pan helper observer: boom') (ObsMessages $e.result)

$e = Run-E2E (Run-Producer (New-Start) (New-Stop @('a', 'b') @()))
Add-Result 'e2e-producer-multiple-consumer-surfaces' (((ObsMessages $e.result) -match 'pan helper observer: a') -and ((ObsMessages $e.result) -match 'pan helper observer: b')) (ObsMessages $e.result)

# Regression shape: reproduce the OLD producer's if-pipeline AutomationNull,
# which ConvertTo-Json serializes as `{}`. The real consumer must now fail
# explicitly instead of rendering a blank error. The hashtable-literal
# if-expression is exactly the old Set-ObserverSummary shape.
$oldObserver = [ordered]@{
    ready_seen = $true; exit_code = 0; frames_written = 9; valid_frames = 3
    forced_kill = $false; exited = $true; complete_coverage = $true
    metadata_schema = 'vacards.screen-observer/1'; metadata_qpc_frequency = 10000000
    metadata_qpc_start = 9999000; metadata_qpc_ready = 9999900; metadata_qpc_end = 10010000
    metadata_exit_code = 0; metadata_exit_reason = 'stop_file'; metadata_fatal = $false
    errors = if ($true) { @() } else { @() }
}
$e = Run-E2E $oldObserver
Add-Result 'e2e-oldstyle-object-json-is-braces' ($e.json -match '"errors":\{\}') 'simulated old producer serializes {}'
Add-Result 'e2e-oldstyle-object-consumer-fails-explicitly' (((ObsMessages $e.result) -match 'observer\.errors is not a JSON array') -and (-not ((ObsMessages $e.result) -match 'pan helper observer:\s*(\|\||$)'))) (ObsMessages $e.result)

# ------------------------------------------------------------------ report --
$failed = @($script:results | Where-Object { -not $_.ok })
$summary = [ordered]@{
    schema        = 'vacards-observer-errors-roundtrip-test/1'
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
if ($failed.Count -gt 0) {
    foreach ($f in $failed) { Write-Output ("OBSERR FAILED {0} :: {1}" -f $f.name, $f.detail) }
    Write-Output ("OBSERR FAIL failed={0} total={1}" -f $failed.Count, $script:results.Count)
    exit 1
}
Write-Output ("OBSERR PASS total={0}" -f $script:results.Count)
exit 0
