[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $projectRoot 'build' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -BuildDirectory $BuildDirectory }
$artifactDirectory = Join-Path $projectRoot 'artifacts'
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$testLog = Join-Path $artifactDirectory 'headless-tests.log'
$smokeLog = Join-Path $artifactDirectory 'headless-smoke.log'
$junit = Join-Path $artifactDirectory 'core-tests.xml'
$smokeReport = Join-Path $artifactDirectory 'headless-smoke.json'
Set-Content -LiteralPath $testLog -Value '' -Encoding utf8
Set-Content -LiteralPath $smokeLog -Value '' -Encoding utf8
if (Test-Path -LiteralPath $smokeReport) { Remove-Item -LiteralPath $smokeReport -Force }

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
& $ctestPath --test-dir $BuildDirectory -C $Configuration --output-on-failure --no-tests=error --output-junit $junit 2>&1 | Tee-Object -FilePath $testLog -Append
$testExit = $LASTEXITCODE
$executable = Join-Path (Join-Path $BuildDirectory $Configuration) 'WindowsExplorer.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "Application executable was not found: $executable" }
# Waiting for the process avoids the asynchronous GUI-subsystem launch behavior of PowerShell.
$smokeProcess = Start-Process -FilePath $executable -ArgumentList @('--headless-smoke', '--report', ('"' + $smokeReport + '"')) -WindowStyle Hidden -PassThru -RedirectStandardOutput $smokeLog -RedirectStandardError (Join-Path $artifactDirectory 'headless-smoke-errors.log')
if (-not $smokeProcess.WaitForExit(60000)) {
    Stop-Process -Id $smokeProcess.Id -Force -ErrorAction SilentlyContinue
    throw "The headless smoke process timed out after 60 seconds. Reports: $artifactDirectory"
}
$smokeProcess.WaitForExit()
$smokeProcess.Refresh()
$smokeExit = $smokeProcess.ExitCode
if (Test-Path -LiteralPath $smokeLog) { Get-Content -LiteralPath $smokeLog | Write-Host }
if (Test-Path -LiteralPath $smokeReport) {
    $reportSummary = Get-Content -LiteralPath $smokeReport -Raw | ConvertFrom-Json
    foreach ($check in $reportSummary.results) {
        if ($check.passed -ne $true) { Write-Host "FAIL: $($check.name): $($check.detail)" }
    }
}
if ($testExit -ne 0 -or $smokeExit -ne 0) { throw "Headless checks failed: CTest=$testExit, smoke=$smokeExit. Reports: $artifactDirectory" }
if (-not (Test-Path -LiteralPath $smokeReport)) { throw "Smoke test did not create its report: $smokeReport" }
$smokeSummary = Get-Content -LiteralPath $smokeReport -Raw | ConvertFrom-Json
if ($smokeSummary.headless -ne $true -or $smokeSummary.passed -ne $true) { throw "Smoke report did not confirm successful headless checks: $smokeReport" }
Write-Host "All headless checks passed. Reports: $artifactDirectory"
