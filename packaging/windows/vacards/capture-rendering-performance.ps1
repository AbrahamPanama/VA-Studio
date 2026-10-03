# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
Collect read-only Windows counters for one explicitly selected application process.
.DESCRIPTION
Creates a new output directory containing metadata.json, samples.json, process.csv,
gpu-engine.csv and gpu-memory.csv. CPU percent is reported both relative to one
logical CPU (which may exceed 100%) and normalized across the machine's logical
CPUs. GPU values come from Windows formatted performance counters, not GPU timers
or physical display frame rates. Missing GPU counters are unavailable, never zero.
No application settings, services or drivers are changed. No document names or
process command lines are collected. Choose a local output directory. Collection
and metadata query time are recorded; slow counter providers can overrun the
requested interval. Each CIM operation has a two-second operation timeout.
.EXAMPLE
.\capture-rendering-performance.ps1 -TargetProcessId 1234 -OutputDirectory C:\VACards\diagnostics\run-01 -DurationSeconds 30
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [Alias('ProcessId')]
    [ValidateRange(1, 2147483647)]
    [int]$TargetProcessId,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputDirectory,

    [ValidateRange(1, 300)]
    [int]$DurationSeconds = 30,

    [ValidateRange(1, 30)]
    [int]$IntervalSeconds = 1
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This collector requires Windows.' }

$outputPath = [IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($OutputDirectory))
if (Test-Path -LiteralPath $outputPath) { throw 'OutputDirectory must be a new directory.' }
$initialProcess = Get-Process -Id $TargetProcessId -ErrorAction Stop
$processStarted = $initialProcess.StartTime.ToUniversalTime()
$processName = $initialProcess.ProcessName
$initialProcess.Dispose()
# Do not use -Force: a concurrent creator must not cause existing evidence to be reused.
New-Item -ItemType Directory -Path $outputPath -ErrorAction Stop | Out-Null

function Get-CounterFailureStatus($Failure) {
    # A stable error identifier avoids recording incidental path/command text.
    return [string]$Failure.FullyQualifiedErrorId
}

function Get-MetadataQuery([string]$ClassName, [string[]]$Properties) {
    try {
        $values = @(Get-CimInstance -ClassName $ClassName -Property $Properties -OperationTimeoutSec 2 |
            Select-Object -Property $Properties)
        return [pscustomobject]@{ Status = 'available'; Values = $values; ErrorId = $null }
    } catch {
        return [pscustomobject]@{ Status = 'unavailable'; Values = @(); ErrorId = (Get-CounterFailureStatus $_) }
    }
}

function Get-GpuClass([string]$ClassName, [string[]]$RequiredProperties) {
    try {
        $definition = Get-CimClass -ClassName $ClassName -OperationTimeoutSec 2
        $properties = @($definition.CimClassProperties | Select-Object -ExpandProperty Name)
        $missing = @($RequiredProperties | Where-Object { $properties -notcontains $_ })
        return [pscustomobject]@{
            ClassName = $ClassName
            Status = $(if ($missing.Count) { 'unavailable_missing_properties' } else { 'available' })
            RequiredProperties = $RequiredProperties
            MissingProperties = $missing
            ErrorId = $null
        }
    } catch {
        return [pscustomobject]@{
            ClassName = $ClassName; Status = 'unavailable'; RequiredProperties = $RequiredProperties
            MissingProperties = @(); ErrorId = (Get-CounterFailureStatus $_)
        }
    }
}

function Get-GpuSample($Definition, [int]$ProcessIdentifier) {
    if ($Definition.Status -ne 'available') {
        return [pscustomobject]@{ Status = $Definition.Status; Instances = @(); ErrorId = $Definition.ErrorId }
    }
    try {
        $prefix = 'pid_' + $ProcessIdentifier + '_'
        # WQL underscores are wildcards; apply an exact prefix check as well.
        $instances = @(Get-CimInstance -ClassName $Definition.ClassName -Filter ("Name LIKE '" + $prefix + "%'") -OperationTimeoutSec 2 |
            Where-Object { ([string]$_.Name).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) } |
            Select-Object -Property $Definition.RequiredProperties)
        $missingValues = $false
        foreach ($instance in $instances) {
            foreach ($property in $Definition.RequiredProperties) {
                if ($null -eq $instance.$property) { $missingValues = $true }
            }
        }
        return [pscustomobject]@{
            Status = $(if (-not $instances.Count) { 'unavailable_no_matching_instances' }
                elseif ($missingValues) { 'partial_missing_values' } else { 'available' })
            Instances = $instances; ErrorId = $null
        }
    } catch {
        return [pscustomobject]@{ Status = 'unavailable'; Instances = @(); ErrorId = (Get-CounterFailureStatus $_) }
    }
}

$metadataTimer = [Diagnostics.Stopwatch]::StartNew()
$os = Get-MetadataQuery 'Win32_OperatingSystem' @('Caption', 'Version', 'BuildNumber', 'OSArchitecture')
$computer = Get-MetadataQuery 'Win32_ComputerSystem' @('NumberOfLogicalProcessors')
$video = Get-MetadataQuery 'Win32_VideoController' @('Name', 'DriverVersion', 'DriverDate', 'PNPDeviceID')
$logicalProcessors = $null
if ($computer.Status -eq 'available' -and $computer.Values.Count -gt 0) {
    $reportedProcessors = $computer.Values[0].NumberOfLogicalProcessors
    if ($null -ne $reportedProcessors -and [int]$reportedProcessors -gt 0) { $logicalProcessors = [int]$reportedProcessors }
}
$engineDefinition = Get-GpuClass 'Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine' @('Name', 'UtilizationPercentage')
$memoryDefinition = Get-GpuClass 'Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory' @('Name', 'DedicatedUsage', 'SharedUsage')
$metadataTimer.Stop()

$metadata = [ordered]@{
    SchemaVersion = 1
    CapturedUtc = [DateTime]::UtcNow.ToString('o')
    ComputerName = $env:COMPUTERNAME
    TargetProcessId = $TargetProcessId
    ProcessName = $processName
    ProcessStartedUtc = $processStarted.ToString('o')
    LogicalProcessorCount = $logicalProcessors
    LogicalProcessorCountStatus = $(if ($null -ne $logicalProcessors) { 'available' } else { 'unavailable' })
    OperatingSystem = $os
    VideoControllers = $video
    ProcessorQuery = $computer
    GpuEngineClass = $engineDefinition
    GpuMemoryClass = $memoryDefinition
    PowerShellVersion = $PSVersionTable.PSVersion.ToString()
    RequestedDurationSeconds = $DurationSeconds
    RequestedIntervalSeconds = $IntervalSeconds
    CimOperationTimeoutSeconds = 2
    MetadataQuerySeconds = $metadataTimer.Elapsed.TotalSeconds
    Notes = @(
        'CpuOneCorePercent = 100 * delta process CPU seconds / actual interval seconds; values over 100 are valid.'
        'CpuSystemPercent divides CpuOneCorePercent by the queried machine logical processor count.'
        'GPU engine rows are independent Windows formatted counters; do not sum engine percentages as whole-GPU utilization.'
        'GPU memory is reported per matching performance-counter instance, not estimated from the application cache budget.'
        'Null counters are unavailable; zero is retained only when a provider explicitly reports zero.'
        'The first CPU row establishes a baseline. Provider collection cost and scheduling overruns are retained.'
        'GPU and CPU readings occur sequentially; rows are not an instantaneous atomic machine snapshot.'
    )
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'metadata.json') -Encoding UTF8

$samples = New-Object 'Collections.Generic.List[object]'
$processRows = New-Object 'Collections.Generic.List[object]'
$engineRows = New-Object 'Collections.Generic.List[object]'
$memoryRows = New-Object 'Collections.Generic.List[object]'
$watch = [Diagnostics.Stopwatch]::StartNew()
$nextSampleSeconds = 0.0
$previousCpuSeconds = $null
$previousCpuTime = $null
$stopReason = 'duration_complete'

while ($nextSampleSeconds -le $DurationSeconds) {
    if ($samples.Count -gt 0 -and $watch.Elapsed.TotalSeconds -ge $DurationSeconds) { break }
    $delay = $nextSampleSeconds - $watch.Elapsed.TotalSeconds
    if ($delay -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Ceiling($delay * 1000)) }
    $sampleStart = $watch.Elapsed.TotalSeconds
    $sampleIndex = $samples.Count
    $cpuOneCore = $null; $cpuSystem = $null; $cpuSeconds = $null
    $workingBytes = $null; $privateBytes = $null; $actualInterval = $null
    $processError = $null; $processStatus = 'available'; $cpuStatus = 'unavailable'
    $process = $null
    try {
        $process = Get-Process -Id $TargetProcessId -ErrorAction Stop
        if ($process.StartTime.ToUniversalTime() -ne $processStarted) {
            $processStatus = 'unavailable_process_id_reused'
        } else {
            $workingBytes = $process.WorkingSet64
            $privateBytes = $process.PrivateMemorySize64
            $cpuValue = $process.CPU
            if ($null -ne $cpuValue) { $cpuSeconds = [double]$cpuValue }
        }
    } catch {
        $processStatus = 'unavailable_exited_or_denied'
        $processError = Get-CounterFailureStatus $_
        $workingBytes = $null; $privateBytes = $null; $cpuSeconds = $null
    } finally {
        if ($null -ne $process) { $process.Dispose() }
    }
    $cpuReadTime = $watch.Elapsed.TotalSeconds
    $cpuReadUtc = [DateTime]::UtcNow.ToString('o')
    if ($null -ne $cpuSeconds) {
        if ($null -eq $previousCpuSeconds) {
            $cpuStatus = 'baseline'
        } else {
            $actualInterval = $cpuReadTime - $previousCpuTime
            if ($actualInterval -gt 0 -and $cpuSeconds -ge $previousCpuSeconds) {
                $cpuOneCore = 100 * ($cpuSeconds - $previousCpuSeconds) / $actualInterval
                if ($null -ne $logicalProcessors) { $cpuSystem = $cpuOneCore / $logicalProcessors }
                $cpuStatus = 'available'
            } else { $cpuStatus = 'unavailable_counter_discontinuity' }
        }
        $previousCpuSeconds = $cpuSeconds; $previousCpuTime = $cpuReadTime
    }

    if ($processStatus -eq 'available') {
        $engines = Get-GpuSample $engineDefinition $TargetProcessId
        $memory = Get-GpuSample $memoryDefinition $TargetProcessId
    } else {
        $engines = [pscustomobject]@{ Status = 'unavailable_process'; Instances = @(); ErrorId = $null }
        $memory = [pscustomobject]@{ Status = 'unavailable_process'; Instances = @(); ErrorId = $null }
    }

    $sampleCost = $watch.Elapsed.TotalSeconds - $sampleStart
    $row = [pscustomobject][ordered]@{
        SampleIndex = $sampleIndex; ElapsedSeconds = $cpuReadTime; Utc = $cpuReadUtc
        CollectionEndUtc = [DateTime]::UtcNow.ToString('o')
        TargetProcessId = $TargetProcessId; ProcessStatus = $processStatus; ProcessErrorId = $processError
        CpuStatus = $cpuStatus; ActualCpuIntervalSeconds = $actualInterval; TotalCpuSeconds = $cpuSeconds
        CpuOneCorePercent = $cpuOneCore; CpuSystemPercent = $cpuSystem
        WorkingSetBytes = $workingBytes; PrivateBytes = $privateBytes
        GpuEngineStatus = $engines.Status; GpuEngineErrorId = $engines.ErrorId
        GpuMemoryStatus = $memory.Status; GpuMemoryErrorId = $memory.ErrorId
        CollectionSeconds = $sampleCost; ScheduleDelaySeconds = [Math]::Max(0.0, $sampleStart - $nextSampleSeconds)
        IntervalOverrun = ($sampleCost -gt $IntervalSeconds)
    }
    $processRows.Add($row)
    $samples.Add([pscustomobject]@{ Process = $row; GpuEngines = @($engines.Instances); GpuMemory = @($memory.Instances) })
    foreach ($instance in $engines.Instances) {
        $engineRows.Add([pscustomobject]@{
            SampleIndex = $sampleIndex; ElapsedSeconds = $cpuReadTime
            Status = $(if ($null -eq $instance.UtilizationPercentage) { 'unavailable_missing_value' } else { 'available' })
            Instance = $instance.Name; UtilizationPercentage = $instance.UtilizationPercentage
        })
    }
    if (-not $engines.Instances.Count) {
        $engineRows.Add([pscustomobject]@{ SampleIndex = $sampleIndex; ElapsedSeconds = $cpuReadTime; Status = $engines.Status; Instance = $null; UtilizationPercentage = $null })
    }
    foreach ($instance in $memory.Instances) {
        $memoryRows.Add([pscustomobject]@{
            SampleIndex = $sampleIndex; ElapsedSeconds = $cpuReadTime
            Status = $(if ($null -eq $instance.DedicatedUsage -and $null -eq $instance.SharedUsage) { 'unavailable_missing_values' }
                elseif ($null -eq $instance.DedicatedUsage -or $null -eq $instance.SharedUsage) { 'partial_missing_values' } else { 'available' })
            Instance = $instance.Name; DedicatedBytes = $instance.DedicatedUsage; SharedBytes = $instance.SharedUsage
        })
    }
    if (-not $memory.Instances.Count) {
        $memoryRows.Add([pscustomobject]@{ SampleIndex = $sampleIndex; ElapsedSeconds = $cpuReadTime; Status = $memory.Status; Instance = $null; DedicatedBytes = $null; SharedBytes = $null })
    }
    if ($processStatus -ne 'available') { $stopReason = $processStatus; break }
    # Skip missed deadlines rather than bursting repeated provider queries.
    $nextSampleSeconds = [Math]::Max($nextSampleSeconds + $IntervalSeconds,
        [Math]::Ceiling($watch.Elapsed.TotalSeconds / $IntervalSeconds) * $IntervalSeconds)
}
$watch.Stop()
$metadata['ActualCollectionSeconds'] = $watch.Elapsed.TotalSeconds
$metadata['SampleCount'] = $samples.Count
$metadata['StopReason'] = $stopReason
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'metadata.json') -Encoding UTF8
ConvertTo-Json -InputObject @($samples.ToArray()) -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'samples.json') -Encoding UTF8
$processRows | Export-Csv -LiteralPath (Join-Path $outputPath 'process.csv') -NoTypeInformation -Encoding UTF8
$engineRows | Export-Csv -LiteralPath (Join-Path $outputPath 'gpu-engine.csv') -NoTypeInformation -Encoding UTF8
$memoryRows | Export-Csv -LiteralPath (Join-Path $outputPath 'gpu-memory.csv') -NoTypeInformation -Encoding UTF8
Write-Output ([pscustomobject]@{ OutputDirectory = $outputPath; SampleCount = $samples.Count; ActualCollectionSeconds = $watch.Elapsed.TotalSeconds; StopReason = $stopReason })
