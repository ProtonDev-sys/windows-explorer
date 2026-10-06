[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'windows-environment.ps1')
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $projectRoot 'build' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$artifactDirectory = Join-Path $projectRoot 'artifacts'
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$testLog = Join-Path $artifactDirectory 'headless-tests.log'
$junit = Join-Path $artifactDirectory 'core-tests.xml'
$smokeReport = Join-Path $artifactDirectory 'headless-smoke.json'
$nativeSmokeReport = Join-Path $BuildDirectory 'headless-smoke.json'
$installedSmokeReport = Join-Path $artifactDirectory 'headless-smoke-installed.json'
$nativeInstalledSmokeReport = Join-Path $BuildDirectory 'headless-smoke-installed.json'
$environmentReport = Join-Path $artifactDirectory 'test-environment.json'
Set-Content -LiteralPath $testLog -Value '' -Encoding utf8
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -BuildDirectory $BuildDirectory }

$ctestCommand = Get-Command ctest -ErrorAction SilentlyContinue
if ($ctestCommand) { $ctestPath = $ctestCommand.Source }
else {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCommand) { $ctestPath = Join-Path (Split-Path -Parent $cmakeCommand.Source) 'ctest.exe' }
    else {
        $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswherePath)) { throw 'CTest was not found.' }
        $installation = & $vswherePath -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $ctestPath = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
    }
}
if (-not (Test-Path -LiteralPath $ctestPath)) { throw 'CTest executable was not found.' }
$configuredTestsText = & $ctestPath --test-dir $BuildDirectory -C $Configuration --show-only=json-v1
if ($LASTEXITCODE -ne 0) { throw 'Could not inventory configured headless tests.' }
$configuredTests = ($configuredTestsText -join "`n") | ConvertFrom-Json
$installedHostConfigured = @($configuredTests.tests | Where-Object { $_.name -eq 'installed_shell_host' }).Count -eq 1
$executable = Join-Path (Join-Path $BuildDirectory $Configuration) 'WindowsExplorer.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "Application executable was not found: $executable" }
$executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
$testStartedUtc = [DateTime]::UtcNow
& $ctestPath --test-dir $BuildDirectory -C $Configuration --parallel 1 --output-on-failure --no-tests=error --output-junit $junit 2>&1 | Tee-Object -FilePath $testLog -Append
$testExit = $LASTEXITCODE
$executableUnchanged = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant() -eq $executableSha256
# CTest already ran the complete private-desktop host suite. Preserve that
# exact report rather than running every native Shell/UIA check a second time.
$hostReports = @(@{ source = $nativeSmokeReport; destination = $smokeReport; layout = 'Authored'; scope = 'General' })
if ($installedHostConfigured) {
    $hostReports += @{ source = $nativeInstalledSmokeReport; destination = $installedSmokeReport; layout = 'InstalledWindows10'; scope = 'General' }
}
foreach ($libraryHost in @(
    @{ name = 'hidden_library_shell_host'; file = 'headless-smoke-library.json'; layout = 'Authored' },
    @{ name = 'installed_library_shell_host'; file = 'headless-smoke-library-installed.json'; layout = 'InstalledWindows10' }
)) {
    if (@($configuredTests.tests | Where-Object { $_.name -eq $libraryHost.name }).Count -eq 1) {
        $hostReports += @{ source = (Join-Path $BuildDirectory $libraryHost.file); destination = (Join-Path $artifactDirectory $libraryHost.file); layout = $libraryHost.layout; scope = 'Library' }
    }
}
foreach ($hostReport in $hostReports) {
    # Retain previous reports as evidence. An interrupted native run must never
    # consume their old success as the result of this invocation.
    $hostReport.fresh = $executableUnchanged -and (Test-Path -LiteralPath $hostReport.source) -and
        (Get-Item -LiteralPath $hostReport.source).LastWriteTimeUtc -ge $testStartedUtc
    if ($hostReport.fresh) {
        Copy-Item -LiteralPath $hostReport.source -Destination $hostReport.destination
    }
}
$junitFresh = (Test-Path -LiteralPath $junit) -and (Get-Item -LiteralPath $junit).LastWriteTimeUtc -ge $testStartedUtc
$testResults = if ($junitFresh) { [xml](Get-Content -LiteralPath $junit -Raw) } else { $null }
function Get-NativeTestStatus([string]$Name) {
    if ($null -eq $testResults) { return 'not-run' }
    $cases = @($testResults.testsuite.testcase | Where-Object { $_.name -eq $Name })
    if ($cases.Count -ne 1) { return 'not-run' }
    $case = $cases[0]
    if ($null -ne $case.SelectSingleNode('skipped') -or $case.status -eq 'notrun') { return 'skipped' }
    if ($null -ne $case.SelectSingleNode('failure') -or $case.status -ne 'run') { return 'failed' }
    return 'passed'
}
function Test-ConfiguredOptIn([string]$Name, [string]$Variable) {
    $tests = @($configuredTests.tests | Where-Object { $_.name -eq $Name })
    if ($tests.Count -ne 1) { return $false }
    $property = @($tests[0].properties | Where-Object { $_.name -eq 'ENVIRONMENT' })
    return $property.Count -eq 1 -and @($property[0].value) -ccontains ($Variable + '=1')
}
$historyStatus = Get-NativeTestStatus 'native_shell_history'
$searchOptionsStatus = Get-NativeTestStatus 'native_search_options'
$viewPersistenceStatus = Get-NativeTestStatus 'native_view_persistence'
$transferStatus = Get-NativeTestStatus 'native_shell_transfer'
$dropsStatus = Get-NativeTestStatus 'native_shell_drops'
$disposableRunner = $env:GITHUB_ACTIONS -ceq 'true'
$historyOptIn = Test-ConfiguredOptIn 'native_shell_history' 'WINDOWSEXPLORER_NATIVE_HISTORY_TEST'
$searchOptIn = Test-ConfiguredOptIn 'native_search_options' 'WINDOWSEXPLORER_SEARCH_OPTIONS_TEST'
$viewPersistenceOptIn = Test-ConfiguredOptIn 'native_view_persistence' 'WINDOWSEXPLORER_VIEW_PERSISTENCE_TEST'
$transferOptIn = $env:WINDOWSEXPLORER_NATIVE_TRANSFER_TEST -ceq '1'
@{
    executableSha256 = $executableSha256
    executableUnchanged = $executableUnchanged
    startedUtc = $testStartedUtc.ToString('o')
    completedUtc = [DateTime]::UtcNow.ToString('o')
    os = [Environment]::OSVersion.VersionString
    windows = Get-ExplorerWindowsEnvironment
    disposableHostedRunner = $disposableRunner
    nativeHistoryOptInConfigured = $historyOptIn
    nativeSearchOptionsOptInConfigured = $searchOptIn
    nativeViewPersistenceOptInConfigured = $viewPersistenceOptIn
    nativeTransferOptInExplicit = $transferOptIn
    nativeDropsOptInExplicit = $transferOptIn
    nativeHistoryMutationEnabled = $disposableRunner -and $historyOptIn -and $historyStatus -ne 'skipped' -and $historyStatus -ne 'not-run'
    nativeSearchOptionsMutationEnabled = $disposableRunner -and $searchOptIn -and $searchOptionsStatus -ne 'skipped' -and $searchOptionsStatus -ne 'not-run'
    nativeViewPersistenceMutationEnabled = $disposableRunner -and $viewPersistenceOptIn -and $viewPersistenceStatus -ne 'skipped' -and $viewPersistenceStatus -ne 'not-run'
    nativeTransferMutationEnabled = $disposableRunner -and $transferOptIn -and $transferStatus -ne 'skipped' -and $transferStatus -ne 'not-run'
    nativeDropsMutationEnabled = $disposableRunner -and $transferOptIn -and $dropsStatus -ne 'skipped' -and $dropsStatus -ne 'not-run'
    nativeHistoryTestStatus = $historyStatus
    nativeSearchOptionsTestStatus = $searchOptionsStatus
    nativeViewPersistenceTestStatus = $viewPersistenceStatus
    nativeTransferTestStatus = $transferStatus
    nativeDropsTestStatus = $dropsStatus
    installedHostConfigured = $installedHostConfigured
    junitFresh = $junitFresh
    hostReportFreshness = @($hostReports | ForEach-Object { @{ layout = $_.layout; scope = $_.scope; fresh = $_.fresh; source = $_.source } })
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $environmentReport -Encoding utf8
foreach ($hostReport in $hostReports) {
    if ($hostReport.fresh -and (Test-Path -LiteralPath $hostReport.destination)) {
        $reportSummary = Get-Content -LiteralPath $hostReport.destination -Raw | ConvertFrom-Json
        foreach ($check in $reportSummary.results) {
            if ($check.passed -ne $true) { Write-Host "FAIL [$($hostReport.layout)]: $($check.name): $($check.detail)" }
        }
    }
}
if ($testExit -ne 0) { throw "Headless checks failed: CTest=$testExit. Reports: $artifactDirectory" }
if (-not $executableUnchanged -or -not $junitFresh) { throw 'Headless executable changed or CTest did not produce a fresh result.' }
if ($disposableRunner) {
    foreach ($nativeGate in @(
        @{ name = 'native_shell_history'; optedIn = $historyOptIn; status = $historyStatus },
        @{ name = 'native_search_options'; optedIn = $searchOptIn; status = $searchOptionsStatus },
        @{ name = 'native_view_persistence'; optedIn = $viewPersistenceOptIn; status = $viewPersistenceStatus },
        @{ name = 'native_shell_transfer'; optedIn = $transferOptIn; status = $transferStatus },
        @{ name = 'native_shell_drops'; optedIn = $transferOptIn; status = $dropsStatus }
    )) {
        if ($nativeGate.optedIn -and $nativeGate.status -ne 'passed') {
            throw "Opted-in disposable-CI test must actually pass: $($nativeGate.name)=$($nativeGate.status)."
        }
    }
}
foreach ($hostReport in $hostReports) {
    if (-not $hostReport.fresh -or -not (Test-Path -LiteralPath $hostReport.destination)) {
        throw "Smoke test did not create a fresh report: $($hostReport.source)"
    }
    $smokeSummary = Get-Content -LiteralPath $hostReport.destination -Raw | ConvertFrom-Json
    if ($smokeSummary.headless -ne $true -or $smokeSummary.privateDesktop -ne $true -or
        $smokeSummary.inputDesktopUnchanged -ne $true -or $smokeSummary.visibleInputDesktopWindows -ne $false -or
        $smokeSummary.passed -ne $true -or $smokeSummary.failed -ne 0 -or
        $smokeSummary.checks -ne $smokeSummary.results.Count -or $smokeSummary.checks -le 0 -or
        @($smokeSummary.results | Where-Object { $_.passed -ne $true }).Count -ne 0 -or
        $smokeSummary.ribbonLayout -ne $hostReport.layout -or $smokeSummary.smokeScope -ne $hostReport.scope -or
        ($hostReport.layout -eq 'InstalledWindows10' -and $smokeSummary.installedRibbonStatus -ne 0)) {
        throw "Smoke report did not confirm its native layout and isolation: $($hostReport.destination)"
    }
}
Write-Host "All headless checks passed. Reports: $artifactDirectory"
