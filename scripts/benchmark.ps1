[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = '',
    [switch]$InstalledRibbon
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'windows-environment.ps1')
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $projectRoot 'build' }
$executable = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ($Configuration + '/WindowsExplorer.exe')
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build the native application before benchmarking.' }
$runId = 'run-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N')
$directory = Join-Path $projectRoot ('artifacts/performance/' + $runId)
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$benchmarkExecutable = Join-Path $directory 'WindowsExplorer.exe'
$builtSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
Copy-Item -LiteralPath $executable -Destination $benchmarkExecutable
$copiedSha256 = (Get-FileHash -LiteralPath $benchmarkExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($copiedSha256 -ne $builtSha256) { throw 'The executable changed while its immutable benchmark copy was made.' }
@{
    executableSha256 = $copiedSha256
    startedUtc = [DateTime]::UtcNow.ToString('o')
    os = [Environment]::OSVersion.VersionString
    windows = Get-ExplorerWindowsEnvironment
    logicalProcessors = [Environment]::ProcessorCount
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $directory 'environment.json') -Encoding utf8
$report = Join-Path $directory 'native-navigation.json'
$benchmarkArguments = @('--headless-benchmark', '--report', ('"' + $report + '"'))
if ($InstalledRibbon) { $benchmarkArguments += '--installed-ribbon' }
$process = Start-Process -FilePath $benchmarkExecutable -ArgumentList $benchmarkArguments `
    -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $directory 'stdout.log') `
    -RedirectStandardError (Join-Path $directory 'stderr.log')
# Windows PowerShell 5.1 reports no ExitCode unless the handle was opened.
$null = $process.Handle
if (-not $process.WaitForExit(180000)) {
    $process.Kill()
    throw "Headless benchmark exceeded three minutes: $directory"
}
$process.Refresh()
if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $report)) {
    throw "Headless native benchmark failed, exit $($process.ExitCode): $directory"
}
$result = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
if ($result.passed -ne $true -or $result.headless -ne $true -or $result.privateDesktop -ne $true -or
    $result.inputDesktopUnchanged -ne $true -or $result.visibleInputDesktopWindows -ne $false -or $result.cases.Count -ne 3) {
    throw "Incomplete native benchmark report: $report"
}
$expectedLayout = if ($InstalledRibbon) { 'InstalledWindows10' } else { 'Authored' }
if ($result.ribbonLayout -ne $expectedLayout -or ($InstalledRibbon -and $result.installedRibbonStatus -ne 0)) {
    throw "The requested native Ribbon layout was unavailable: $report"
}
if ($result.schema -ne 1 -or $result.samplesPerFolder -ne 5 -or $result.desktopVisibilityObservations -le 0) {
    throw "Incomplete native benchmark observation protocol: $report"
}
function Assert-NonnegativeFiniteNumber($Value, [string]$Description) {
    if ($null -eq $Value -or $Value -isnot [ValueType] -or $Value -is [bool] -or $Value -lt 0 -or
        [double]::IsNaN([double]$Value) -or [double]::IsInfinity([double]$Value)) {
        throw "Invalid $Description in native benchmark: $report"
    }
}
if ($null -eq $result.startup -or $result.startup.nativeTargetMatched -ne $true) {
    throw "Initial native view did not match the retained startup target: $report"
}
$previousStartupPhase = 0.0
foreach ($phase in @('privateDesktopReadyMs', 'platformReadyMs', 'createReturnedMs', 'nativeViewReadyMs')) {
    $observation = $result.startup.PSObject.Properties[$phase]
    if ($null -eq $observation) { throw "Missing startup phase $phase`: $report" }
    Assert-NonnegativeFiniteNumber $observation.Value "startup.$phase"
    if ([double]$observation.Value + 0.002 -lt $previousStartupPhase) {
        throw "Startup observations differ from their actual phase order: $report"
    }
    $previousStartupPhase = [double]$observation.Value
}
$expectedCounts = @(10, 1000, 10000)
$metrics = @{ navigation = 'navigationMs'; nativeNavigationCallback = 'nativeNavigationCallbackMs'; firstItem = 'firstItemMs'; fullyPopulated = 'fullyPopulatedMs' }
for ($caseIndex = 0; $caseIndex -lt $expectedCounts.Count; $caseIndex++) {
    $case = $result.cases[$caseIndex]
    if ($case.items -ne $expectedCounts[$caseIndex] -or $case.samples.Count -ne 5) {
        throw "Native navigation fixture or sample count differs: $report"
    }
    foreach ($metric in $metrics.Keys) {
        foreach ($field in @('meanMs', 'medianMs', 'p95Ms', 'maxMs')) {
            Assert-NonnegativeFiniteNumber $case.$metric.$field "$metric.$field"
        }
        $sampleField = $metrics[$metric]
        foreach ($sample in $case.samples) { Assert-NonnegativeFiniteNumber $sample.$sampleField $sampleField }
        $sorted = @($case.samples | ForEach-Object { [double]$_.$sampleField } | Sort-Object)
        $mean = ($sorted | Measure-Object -Average).Average
        if ([Math]::Abs($case.$metric.meanMs - $mean) -gt 0.002 -or
            [Math]::Abs($case.$metric.medianMs - $sorted[2]) -gt 0.002 -or
            [Math]::Abs($case.$metric.p95Ms - $sorted[4]) -gt 0.002 -or
            [Math]::Abs($case.$metric.maxMs - $sorted[4]) -gt 0.002) {
            throw "Native navigation summaries differ from their measured samples: $report"
        }
    }
}
foreach ($vector in @(@{ name = 'viewChangeMs'; count = 6 }, @{ name = 'cachedCommandUpdateMs'; count = 20 })) {
    $values = $result.($vector.name)
    if ($values.Count -ne $vector.count) { throw "Incomplete $($vector.name) benchmark: $report" }
    foreach ($value in $values) { Assert-NonnegativeFiniteNumber $value $vector.name }
}
$expectedSelections = @(
    @{ action = 'selectAll'; selected = 10000 },
    @{ action = 'invertAllToNone'; selected = 0 },
    @{ action = 'invertNoneToAll'; selected = 10000 },
    @{ action = 'selectNone'; selected = 0 }
)
if ($result.selectionChangeMs.Count -ne $expectedSelections.Count) {
    throw "Incomplete native selection benchmark: $report"
}
for ($index = 0; $index -lt $expectedSelections.Count; $index++) {
    $measured = $result.selectionChangeMs[$index]
    $expected = $expectedSelections[$index]
    if ($measured.action -ne $expected.action -or $measured.selected -ne $expected.selected -or
        $null -eq $measured.durationMs) {
        throw "Native selection result differs from its owned fixture: $report"
    }
    Assert-NonnegativeFiniteNumber $measured.durationMs 'selection duration'
    foreach ($phase in @('commandMs', 'deferredWorkMs', 'countReadbackMs',
            'commandStateReadyAfterReadbackMs', 'commandStateReadyTotalMs',
            'immediateCountReadAtMs', 'immediateCountReadCostMs')) {
        $phaseProperty = $measured.PSObject.Properties[$phase]
        if ($null -eq $phaseProperty) {
            throw "Incomplete native selection phase timing: $report"
        }
        Assert-NonnegativeFiniteNumber $phaseProperty.Value $phase
    }
    if ([Math]::Abs(($measured.commandMs + $measured.deferredWorkMs + $measured.countReadbackMs) - $measured.durationMs) -gt 0.002) {
        throw "Native selection phase timings do not sum to the measured duration: $report"
    }
    if ([Math]::Abs(($measured.durationMs + $measured.commandStateReadyAfterReadbackMs) - $measured.commandStateReadyTotalMs) -gt 0.002) {
        throw "Native command readiness timing does not sum to its measured phases: $report"
    }
    if ($null -eq $measured.hostWork -or $measured.hostWork.status -ne 0 -or
        $measured.hostWork.updateCount -lt 1) {
        throw "Missing actual creator STA command-update timing: $report"
    }
    foreach ($phase in @('totalUpdateMs', 'viewReadbackMs', 'selectionCountAttributesMs',
            'selectionStatusMs', 'selectionHostEligibilityMs', 'selectionKindsMs',
            'namespacePreparationMs', 'providerCatalogMs', 'stateTaskSchedulingMs',
            'contextMs', 'ribbonInvalidationMs')) {
        $phaseProperty = $measured.hostWork.PSObject.Properties[$phase]
        if ($null -eq $phaseProperty) { throw "Missing creator STA phase $phase`: $report" }
        Assert-NonnegativeFiniteNumber $phaseProperty.Value $phase
    }
    foreach ($counter in @('generationChanges', 'equivalentSelectionRefreshes', 'uncertainSelectionRefreshes')) {
        Assert-NonnegativeFiniteNumber $measured.$counter $counter
        if ($measured.$counter -ne [Math]::Floor($measured.$counter)) { throw "Noninteger selection counter: $report" }
    }
    Assert-NonnegativeFiniteNumber $measured.immediateSelectedCount 'immediate selected count'
    if ($measured.immediateSelectedCount -ne [Math]::Floor($measured.immediateSelectedCount) -or
        $measured.immediateSelectedCount -gt 10000 -or
        $measured.immediateCountReadAtMs -gt $measured.durationMs + 0.002 -or
        $measured.immediateCountReadCostMs -gt $measured.immediateCountReadAtMs + 0.002) {
        throw "Invalid immediate native selection observation: $report"
    }
    if ($null -eq $measured.hostWork.PSObject.Properties['completedStateWorkers']) {
        throw "Missing completed native worker profiling: $report"
    }
    foreach ($worker in $measured.hostWork.completedStateWorkers) {
        if ($worker.timingStatus -ne 0 -or $worker.selectionBatch -isnot [bool] -or $null -eq $worker.status) {
            throw "Unestablished completed native worker profiling: $report"
        }
        Assert-NonnegativeFiniteNumber $worker.command 'profiled command identifier'
        foreach ($phase in @('workerMs', 'dataObjectExportMs', 'identityConstructionMs', 'contextBindMs',
                'menuQueryMs', 'menuEnumerationMs', 'stateReductionMs')) {
            Assert-NonnegativeFiniteNumber $worker.$phase "native worker $phase"
        }
    }
    Write-Host "10,000 items: $($measured.action); immediate count $($measured.immediateSelectedCount) at $($measured.immediateCountReadAtMs) ms; creator queue drained $($measured.durationMs) ms; native command states ready $($measured.commandStateReadyTotalMs) ms; selected $($measured.selected)"
}
foreach ($case in $result.cases) {
    Write-Host "$($case.items) items: navigation p95 $($case.navigation.p95Ms) ms; first item p95 $($case.firstItem.p95Ms) ms; populated p95 $($case.fullyPopulated.p95Ms) ms"
}
Write-Host "Private bytes: $($result.privateBytes); full native timing report: $report"
