[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = '',
    [int]$Parallel = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $projectRoot 'build' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$artifactDirectory = Join-Path $projectRoot 'artifacts'
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$buildLog = Join-Path $artifactDirectory 'build.log'
Set-Content -LiteralPath $buildLog -Value '' -Encoding utf8

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakePath = $cmakeCommand.Source }
else {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswherePath)) { throw 'CMake and Visual Studio 2022 Build Tools were not found.' }
    $installation = & $vswherePath -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Visual Studio 2022 with the C++ tools was not found.' }
    $cmakePath = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $cmakePath)) { throw 'Install the CMake component in Visual Studio 2022.' }
}

& $cmakePath -S $projectRoot -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON 2>&1 | Tee-Object -FilePath $buildLog -Append
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed (exit $LASTEXITCODE). See $buildLog" }
& $cmakePath --build $BuildDirectory --config $Configuration --parallel ([Math]::Max(1, $Parallel)) 2>&1 | Tee-Object -FilePath $buildLog -Append
if ($LASTEXITCODE -ne 0) { throw "C++ build failed (exit $LASTEXITCODE). See $buildLog" }
Write-Host "Built $Configuration in $BuildDirectory"
