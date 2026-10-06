[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = '',
    [string]$Python = 'python',
    [string[]]$Scenes = @('Home', 'Share', 'View', 'Computer', 'Network', 'Picture', 'Drive', 'DiskImage', 'Compressed', 'Search', 'Library', 'Recycle', 'Application', 'Shortcut', 'Music', 'Video', 'ModernHome', 'ModernShare', 'ModernView'),
    [switch]$CaptureOnly,
    [switch]$InstalledRibbon,
    [switch]$CrashDiagnostics,
    [switch]$SkipReferenceDownload
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$captureScriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
. (Join-Path $PSScriptRoot 'windows-environment.ps1')
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $projectRoot 'build' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$executable = Join-Path $BuildDirectory ($Configuration + '/WindowsExplorer.exe')
if (-not (Test-Path -LiteralPath $executable)) { throw "Build the native application first: $executable" }
$pythonCommand = Get-Command $Python -ErrorAction Stop
$pythonPath = $pythonCommand.Source
$manifestPath = Join-Path $projectRoot 'tests/visual/windows10-reference.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$comparer = Join-Path $PSScriptRoot 'visual_check.py'
& $pythonPath -B (Join-Path $projectRoot 'tests/visual/test_visual_check.py')
if ($LASTEXITCODE -ne 0) { throw 'The headless visual comparator checks failed.' }
if (-not $CaptureOnly -and -not $SkipReferenceDownload) {
    & $pythonPath -B $comparer --fetch
    if ($LASTEXITCODE -ne 0) { throw 'The pinned online Windows 10 screenshots could not be verified.' }
}
$runId = 'run-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N')
$runDirectory = Join-Path $projectRoot ('artifacts/visual/' + $runId)
$binaryDirectory = Join-Path $runDirectory 'binary'
New-Item -ItemType Directory -Path $binaryDirectory -Force | Out-Null
$builtExecutable = $executable
$binarySha256 = (Get-FileHash -LiteralPath $builtExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
$executable = Join-Path $binaryDirectory 'WindowsExplorer.exe'
Copy-Item -LiteralPath $builtExecutable -Destination $executable
if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $binarySha256) {
    throw 'The application changed while the immutable visual-test executable was being copied.'
}
$diagnosticPdbSha256 = $null
$privateDumpDirectory = $null
if ($CrashDiagnostics) {
    $builtPdb = [IO.Path]::ChangeExtension($builtExecutable, 'pdb')
    if (-not (Test-Path -LiteralPath $builtPdb)) { throw 'Crash diagnostics require the matching native build PDB.' }
    $diagnosticPdbSha256 = (Get-FileHash -LiteralPath $builtPdb -Algorithm SHA256).Hash.ToLowerInvariant()
    $frozenPdb = Join-Path $binaryDirectory 'WindowsExplorer.pdb'
    Copy-Item -LiteralPath $builtPdb -Destination $frozenPdb
    if ((Get-FileHash -LiteralPath $frozenPdb -Algorithm SHA256).Hash.ToLowerInvariant() -ne $diagnosticPdbSha256) {
        throw 'The native PDB changed while its immutable diagnostic snapshot was being copied.'
    }
    $privateDumpDirectory = Join-Path $runDirectory 'private-source/diagnostics'
    New-Item -ItemType Directory -Path $privateDumpDirectory -Force | Out-Null
}
$manifestSha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceManifestPath = $manifestPath
$manifestPath = Join-Path $runDirectory 'reference-manifest.json'
Copy-Item -LiteralPath $sourceManifestPath -Destination $manifestPath
if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifestSha256) {
    throw 'The reference manifest changed while its immutable protocol snapshot was being copied.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$comparerSha256 = (Get-FileHash -LiteralPath $comparer -Algorithm SHA256).Hash.ToLowerInvariant()
$fixture = Join-Path $runDirectory 'Owned fixture'
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $fixture '.native-visual-fixture'), [Guid]::NewGuid().ToString('D'), [Text.UTF8Encoding]::new($false))
foreach ($name in @('Documents', 'Pictures', 'Music', 'Videos')) {
    New-Item -ItemType Directory -Path (Join-Path $fixture $name) | Out-Null
}
[IO.File]::WriteAllText((Join-Path $fixture 'Read me.txt'), 'Owned deterministic native Explorer screenshot fixture.', [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $fixture 'Budget.csv'), "Item,Amount`nExample,42`n", [Text.UTF8Encoding]::new($false))
Add-Type -AssemblyName System.IO.Compression
$archiveStream = [IO.File]::Open((Join-Path $fixture 'Archive.zip'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
try {
    $archive = [IO.Compression.ZipArchive]::new($archiveStream, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        $entry = $archive.CreateEntry('Read me.txt', [IO.Compression.CompressionLevel]::NoCompression)
        $entry.LastWriteTime = [DateTimeOffset]::Parse('2020-01-02T03:04:05+00:00')
        $entryStream = $entry.Open()
        try {
            $archiveContent = [Text.UTF8Encoding]::new($false).GetBytes('Owned deterministic native archive member.')
            $entryStream.Write($archiveContent, 0, $archiveContent.Length)
        } finally { $entryStream.Dispose() }
    } finally { $archive.Dispose() }
} finally { $archiveStream.Dispose() }
$compressedSourceFixture = $null
$compressedSourceFolderNativePath = $null
$compressedSourceArchive = $null
$compressedSourceArchiveSha256 = $null
if ($Scenes -contains 'Compressed') {
    # The source's Downloads caption and disabled destination gallery show a
    # ZIP selected outside its namespace. Keep the inside-ZIP fixture above
    # for the independent native member/Extract-to capture.
    $compressedSourceFixture = Join-Path $fixture 'Downloads'
    New-Item -ItemType Directory -Path $compressedSourceFixture | Out-Null
    $compressedSourceArchive = Join-Path $compressedSourceFixture 'Archive.zip'
    [IO.File]::Copy((Join-Path $fixture 'Archive.zip'), $compressedSourceArchive, $false)
    $compressedSourceArchiveSha256 = (Get-FileHash -LiteralPath $compressedSourceArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    # Shell filesystem display names can expand an 8.3 parent. Resolve the
    # existing owned folder, rather than accepting a suffix or another folder.
    if (-not ('ExplorerVisual.NativePaths' -as [type])) {
        Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
using System.Text;
namespace ExplorerVisual {
    public static class NativePaths {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        public static extern uint GetLongPathNameW(string path, StringBuilder output, uint characters);
    }
}
'@
    }
    $nativePath = [Text.StringBuilder]::new(32768)
    $nativePathLength = [ExplorerVisual.NativePaths]::GetLongPathNameW(
        [IO.Path]::GetFullPath($compressedSourceFixture), $nativePath, [uint32]$nativePath.Capacity)
    $nativePathError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    if ($nativePathLength -eq 0 -or $nativePathLength -ge $nativePath.Capacity) {
        throw "Owned Compressed containing-folder long-path resolution failed: length $nativePathLength; native error $nativePathError"
    }
    $compressedSourceFolderNativePath = $nativePath.ToString()
}
# A tiny owned PNG; no external thumbnail, network storage, or user files.
[IO.File]::WriteAllBytes((Join-Path $fixture 'Photo.png'), [Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2AAAAI0lEQVR4nGNkqLjOQApgIkk1w6gG4gATkergYFQDMYDkUAIAMscBb9kZy0QAAAAASUVORK5CYII='))
foreach ($entry in Get-ChildItem -LiteralPath $fixture -File) {
    $entry.LastWriteTimeUtc = [DateTime]::SpecifyKind([DateTime]'2020-01-02T03:04:05', [DateTimeKind]::Utc)
}
$fixtureBuilderSha256 = $null
$fixtureBuilderReport = $null
$fixtureBuilderFirstReport = $null
$fixtureBuilderStages = [Collections.Generic.List[object]]::new()
$script:visualChildLaunchBlocked = $false
$script:undrainedVisualChild = $null
$script:undrainedVisualChildId = $null
function Invoke-VisualChildProcess([string]$Filename, [string[]]$Arguments, [string]$Stdout,
        [string]$Stderr, [string]$ReceiptPath, [string]$Context) {
    $childProcess = $null
    $failure = $null
    $stage = 'launch-guard'
    $receipt = [ordered]@{
        context = $Context; processId = $null; startUtc = $null; endUtc = $null;
        launchAttemptUtc = $null; observedUtc = $null; elapsedMilliseconds = $null;
        elapsedMeaning = 'Measured parent stopwatch through launch, wait, bounded cleanup and native exit readback; not process execution time.';
        waitMilliseconds = 30000; drainMilliseconds = 5000; timeout = $null; exited = $null; exitCode = $null;
        launchAttempted = $false; handleCached = $false; killAttempted = $false; killSucceeded = $false;
        drainAttempted = $false; drained = $null; failureStage = $null; operationError = $null;
        killError = $null; drainError = $null; exitReadbackError = $null; releaseError = $null;
        blockedByProcessId = $null
    }
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    try {
        if ($script:visualChildLaunchBlocked) {
            $receipt.blockedByProcessId = $script:undrainedVisualChildId
            throw 'A previous native child has no confirmed exit; no further native process will be launched.'
        }
        $stage = 'launch'
        $receipt.launchAttempted = $true
        $receipt.launchAttemptUtc = [DateTime]::UtcNow.ToString('o')
        $childProcess = Start-Process -FilePath $Filename -ArgumentList $Arguments -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $Stdout -RedirectStandardError $Stderr
        $receipt.processId = $childProcess.Id
        $stage = 'handle'
        # Windows PowerShell 5.1 must retain this handle before waiting to
        # expose the actual native ExitCode reliably after a fast child exits.
        $null = $childProcess.Handle
        $receipt.handleCached = $true
        $stage = 'start-time'
        $receipt.startUtc = $childProcess.StartTime.ToUniversalTime().ToString('o')
        $stage = 'wait'
        $receipt.exited = $childProcess.WaitForExit(30000)
        $receipt.timeout = -not $receipt.exited
        if ($receipt.timeout) {
            $failure = 'Native child exceeded the original 30000 ms wait.'
            $receipt.failureStage = 'wait-timeout'
            $receipt.operationError = $failure
        }
    } catch {
        $receipt.failureStage = $stage
        $receipt.operationError = $_.Exception.Message
        $failure = $_.Exception.Message
    } finally {
        if ($childProcess -and $receipt.exited -ne $true) {
            # Kill this retained Process object only. Do not search by name or
            # release its ownership while its kernel exit remains uncertain.
            if ($receipt.handleCached) {
                $receipt.killAttempted = $true
                try { $childProcess.Kill(); $receipt.killSucceeded = $true }
                catch { $receipt.killError = $_.Exception.Message }
                $receipt.drainAttempted = $true
                try {
                    $receipt.drained = $childProcess.WaitForExit(5000)
                    $receipt.exited = $receipt.drained
                } catch { $receipt.drainError = $_.Exception.Message }
            } else {
                $receipt.killError = 'No owned kernel handle was retained; PID-only termination or exit observation is unsafe and was not attempted.'
            }
        }
        if ($childProcess -and $receipt.exited -eq $true) {
            try {
                $childProcess.Refresh()
                $actualExitCode = $childProcess.ExitCode
                if ($null -eq $actualExitCode) { throw 'An exited native child returned no ExitCode.' }
                $receipt.exitCode = $actualExitCode
                $receipt.endUtc = $childProcess.ExitTime.ToUniversalTime().ToString('o')
            } catch { $receipt.exitReadbackError = $_.Exception.Message }
            try { $childProcess.Dispose() }
            catch { $receipt.releaseError = $_.Exception.Message }
        } elseif (-not $script:visualChildLaunchBlocked) {
            $script:visualChildLaunchBlocked = $true
            $script:undrainedVisualChild = $childProcess
            $script:undrainedVisualChildId = $receipt.processId
        }
        $elapsed.Stop()
        $receipt.elapsedMilliseconds = $elapsed.Elapsed.TotalMilliseconds
        $receipt.observedUtc = [DateTime]::UtcNow.ToString('o')
        [IO.File]::WriteAllText($ReceiptPath, ($receipt | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    }
    $errors = @(@($failure, $receipt.killError, $receipt.drainError, $receipt.exitReadbackError, $receipt.releaseError) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($receipt.exited -ne $true) { $errors += 'The exact native child did not reach a confirmed kernel exit; subsequent native launches are blocked.' }
    if ($errors.Count -gt 0) { throw "$Context failed: $($errors -join '; '); process receipt: $ReceiptPath" }
    return [PSCustomObject]$receipt
}
if (@($Scenes | Where-Object { $_ -in @('Search', 'Library', 'Application', 'Shortcut', 'Music', 'Video', 'DiskImage') }).Count -gt 0) {
    $builtFixtureBuilder = Join-Path $BuildDirectory ($Configuration + '/visual_fixture_builder.exe')
    if (-not (Test-Path -LiteralPath $builtFixtureBuilder)) {
        throw "Build the native semantic-fixture helper first: $builtFixtureBuilder"
    }
    $fixtureBuilderSha256 = (Get-FileHash -LiteralPath $builtFixtureBuilder -Algorithm SHA256).Hash.ToLowerInvariant()
    $fixtureBuilder = Join-Path $binaryDirectory 'visual_fixture_builder.exe'
    Copy-Item -LiteralPath $builtFixtureBuilder -Destination $fixtureBuilder
    if ((Get-FileHash -LiteralPath $fixtureBuilder -Algorithm SHA256).Hash.ToLowerInvariant() -ne $fixtureBuilderSha256) {
        throw 'The native fixture builder changed while its immutable executable was being copied.'
    }
    function Test-FixtureIdentity($Left, $Right) {
        return $null -ne $Left -and $null -ne $Right -and
            $Left.volume -is [string] -and $Right.volume -is [string] -and
            $Left.volume -cmatch '^[0-9]{1,20}$' -and $Right.volume -cmatch '^[0-9]{1,20}$' -and
            $Left.fileId -is [string] -and $Right.fileId -is [string] -and
            $Left.fileId -cmatch '^[0-9a-f]{32}$' -and $Right.fileId -cmatch '^[0-9a-f]{32}$' -and
            $Left.volume -ceq $Right.volume -and $Left.fileId -ceq $Right.fileId
    }
    function Test-FixtureDescriptor($Left, $Right) {
        return $null -ne $Left -and $null -ne $Right -and
            (Test-FixtureIdentity $Left.identity $Right.identity) -and
            $Left.bytes -gt 0 -and $Left.bytes -le 1048576 -and $Left.bytes -eq $Right.bytes -and
            $Left.sha256 -is [string] -and $Right.sha256 -is [string] -and
            $Left.sha256 -cmatch '^[0-9a-f]{64}$' -and $Right.sha256 -cmatch '^[0-9a-f]{64}$' -and
            $Left.sha256 -ceq $Right.sha256 -and $Left.attributes -eq $Right.attributes -and
            $Left.creation -eq $Right.creation -and $Left.write -eq $Right.write -and $Left.change -eq $Right.change
    }
    $fixtureBuilderFirstReport = Join-Path $fixture 'Native fixture first-stage report.json'
    $fixtureBuilderReport = Join-Path $fixture 'Native fixture report.json'
    $builderCommon = @('--path', ('"' + $fixture + '"'), '--executable', ('"' + $executable + '"'))
    $firstArguments = $builderCommon + @('--stage', 'first', '--report', ('"' + $fixtureBuilderFirstReport + '"'))
    $firstBuilder = Invoke-VisualChildProcess $fixtureBuilder $firstArguments `
        (Join-Path $runDirectory 'fixture-first-stdout.log') (Join-Path $runDirectory 'fixture-first-stderr.log') `
        (Join-Path $runDirectory 'fixture-first-process.json') 'First native semantic fixture stage'
    $fixtureBuilderStages.Add($firstBuilder)
    if ($firstBuilder.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $fixtureBuilderFirstReport)) {
        throw "First native semantic fixture stage failed: exit $($firstBuilder.ExitCode); artifacts: $runDirectory"
    }
    $firstProof = Get-Content -LiteralPath $fixtureBuilderFirstReport -Raw | ConvertFrom-Json
    $handoffPath = Join-Path $fixture '.native-library-stage.bin'
    $ownedLibraryPath = Join-Path $fixture 'Owned library.library-ms'
    if ($firstProof.stage -cne 'first' -or $firstProof.passed -ne $true -or $firstProof.completeFixture -ne $false -or
        $firstProof.headless -ne $true -or $firstProof.privateDesktop -ne $true -or $firstProof.inputDesktopUnchanged -ne $true -or
        $firstProof.visibleInputDesktopWindows -ne $false -or $firstProof.executedFixture -ne $false -or
        $firstProof.created -ne 4 -or $firstProof.createdThisStage -ne 4 -or @($firstProof.fixtureFiles).Count -ne 4 -or $firstProof.libraryLocations -ne 1 -or
        @($firstProof.sourceFolders).Count -ne 3 -or $firstProof.searchRelativeToday -ne $true -or
        $firstProof.searchVerificationStage -cne 'first' -or $firstProof.searchSourceStatePreserved -ne $true -or
        @($firstProof.searchSourceFolders).Count -ne 2 -or @($firstProof.searchSourceFiles).Count -ne 5 -or
        $firstProof.searchScope -cne 'Search scope' -or $firstProof.searchRecursive -ne $true -or $firstProof.searchResultCount -ne 2 -or $firstProof.searchExactIdentities -ne $true -or
        $firstProof.searchEarlierExcluded -ne $true -or $firstProof.searchOutsideScopeExcluded -ne $true -or
        -not (Test-FixtureIdentity $firstProof.fixtureRoot $firstProof.fixtureRoot) -or
        -not (Test-FixtureDescriptor $firstProof.libraryDescriptorAfter $firstProof.libraryDescriptorAfter) -or
        -not (Test-FixtureDescriptor $firstProof.handoff $firstProof.handoff) -or
        -not (Test-Path -LiteralPath $handoffPath) -or -not (Test-Path -LiteralPath $ownedLibraryPath)) {
        throw 'The first native stage did not prove an incomplete owned fixture and one genuinely persisted library member.'
    }
    $fixtureGuid = [Guid](Get-Content -LiteralPath (Join-Path $fixture '.native-visual-fixture') -Raw)
    if ([Guid]$firstProof.fixtureGuid -ne $fixtureGuid -or
        (Get-FileHash -LiteralPath $handoffPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $firstProof.handoff.sha256 -or
        (Get-Item -LiteralPath $handoffPath).Length -ne $firstProof.handoff.bytes -or
        (Get-FileHash -LiteralPath $ownedLibraryPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $firstProof.libraryDescriptorAfter.sha256 -or
        (Get-Item -LiteralPath $ownedLibraryPath).Length -ne $firstProof.libraryDescriptorAfter.bytes) {
        throw 'The first-stage native library/handoff bytes or owned fixture GUID changed before completion.'
    }
    # Each genuinely native stage keeps the original 30s wait and 5s exact-child
    # cleanup. A partial stage is never a complete fixture or capture admission.
    $completeArguments = $builderCommon + @('--stage', 'complete', '--report', ('"' + $fixtureBuilderReport + '"'))
    $builder = Invoke-VisualChildProcess $fixtureBuilder $completeArguments `
        (Join-Path $runDirectory 'fixture-stdout.log') (Join-Path $runDirectory 'fixture-stderr.log') `
        (Join-Path $runDirectory 'fixture-process.json') 'Complete native semantic fixture stage'
    $fixtureBuilderStages.Add($builder)
    if ($builder.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $fixtureBuilderReport)) {
        throw "Native semantic fixture creation failed: exit $($builder.ExitCode); artifacts: $runDirectory"
    }
    $fixtureProof = Get-Content -LiteralPath $fixtureBuilderReport -Raw | ConvertFrom-Json
    if ($fixtureProof.stage -cne 'complete' -or $fixtureProof.completeFixture -ne $true -or
        $fixtureProof.passed -ne $true -or $fixtureProof.headless -ne $true -or
        $fixtureProof.privateDesktop -ne $true -or $fixtureProof.inputDesktopUnchanged -ne $true -or
        $fixtureProof.visibleInputDesktopWindows -ne $false -or $fixtureProof.executedFixture -ne $false -or
        $fixtureProof.created -ne 7 -or $fixtureProof.createdThisStage -ne 3 -or @($fixtureProof.fixtureFiles).Count -ne 7 -or
        $fixtureProof.libraryLocations -ne 3 -or $fixtureProof.libraryDefault -ne 'Documents' -or
        $fixtureProof.libraryTemplate -ne 'Documents' -or $fixtureProof.isoBuilder -ne 'IMAPI2FS' -or
        $fixtureProof.isoBuilderStatus -ne 0 -or $fixtureProof.isoVolumeVerified -ne $true -or
        $fixtureProof.searchRelativeToday -ne $true -or $fixtureProof.searchRecursive -ne $true -or
        $fixtureProof.searchScope -ne 'Search scope' -or $fixtureProof.searchResultCount -ne 2 -or
        $fixtureProof.searchExactIdentities -ne $true -or $fixtureProof.searchEarlierExcluded -ne $true -or
        $fixtureProof.searchOutsideScopeExcluded -ne $true) {
        throw 'The native helper did not prove valid owned semantic fixtures on its private desktop.'
    }
    if ($fixtureProof.searchVerificationStage -cne 'complete' -or $fixtureProof.searchSourceStatePreserved -ne $true -or
        @($fixtureProof.searchSourceFolders).Count -ne 2 -or @($fixtureProof.searchSourceFiles).Count -ne 5) {
        throw 'Completion did not freshly verify native Today results and preserve every original search source.'
    }
    if ([Guid]$fixtureProof.fixtureGuid -ne $fixtureGuid -or @($fixtureProof.sourceFolders).Count -ne 3 -or
        -not (Test-FixtureIdentity $firstProof.fixtureRoot $fixtureProof.fixtureRoot) -or
        -not (Test-FixtureDescriptor $firstProof.libraryDescriptorAfter $fixtureProof.libraryDescriptorBefore) -or
        -not (Test-FixtureDescriptor $firstProof.handoff $fixtureProof.handoff) -or
        -not (Test-FixtureDescriptor $fixtureProof.libraryDescriptorAfter $fixtureProof.libraryDescriptorAfter) -or
        (Get-FileHash -LiteralPath $ownedLibraryPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $fixtureProof.libraryDescriptorAfter.sha256 -or
        (Get-Item -LiteralPath $ownedLibraryPath).Length -ne $fixtureProof.libraryDescriptorAfter.bytes -or
        (Get-FileHash -LiteralPath $handoffPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $firstProof.handoff.sha256) {
        throw 'Native completion did not preserve the exact first-stage source/descriptor handoff and publish the full committed library.'
    }
    $expectedFolderNames = @('Documents', 'Pictures', 'Music')
    for ($index = 0; $index -lt $expectedFolderNames.Count; ++$index) {
        if ($firstProof.sourceFolders[$index].name -cne $expectedFolderNames[$index] -or
            $fixtureProof.sourceFolders[$index].name -cne $expectedFolderNames[$index] -or
            -not (Test-FixtureIdentity $firstProof.sourceFolders[$index].identity $fixtureProof.sourceFolders[$index].identity)) {
            throw 'A complete native library source folder differs from the first-stage full FileID.'
        }
    }
    $expectedSearchFolderNames = @('Search scope', 'Search scope/Child')
    for ($index = 0; $index -lt $expectedSearchFolderNames.Count; ++$index) {
        if ($firstProof.searchSourceFolders[$index].name -cne $expectedSearchFolderNames[$index] -or
            $fixtureProof.searchSourceFolders[$index].name -cne $expectedSearchFolderNames[$index] -or
            -not (Test-FixtureIdentity $firstProof.searchSourceFolders[$index].identity $fixtureProof.searchSourceFolders[$index].identity)) {
            throw 'An owned Today search scope folder differs from its first-stage full FileID.'
        }
    }
    $expectedSearchFileNames = @('Search scope/Today.txt', 'Search scope/Earlier.txt',
        'Search scope/Child/Today child.txt', 'Search scope/Child/Earlier child.txt', 'Outside today.txt')
    for ($index = 0; $index -lt $expectedSearchFileNames.Count; ++$index) {
        if ($firstProof.searchSourceFiles[$index].name -cne $expectedSearchFileNames[$index] -or
            $fixtureProof.searchSourceFiles[$index].name -cne $expectedSearchFileNames[$index] -or
            $firstProof.searchSourceFiles[$index].snapshot.bytes -ne 6 -or
            -not (Test-FixtureDescriptor $firstProof.searchSourceFiles[$index].snapshot $fixtureProof.searchSourceFiles[$index].snapshot)) {
            throw 'An owned Today source differs from its first-stage full FileID, bytes or metadata.'
        }
    }
}

function Invoke-PrivateCapture([string]$Scene, [int]$Width, [int]$Height, [string]$Suffix, [bool]$SourceMatched = $false) {
    $nativePage = if ($Scene.StartsWith('Modern')) { $Scene.Substring(6) } else { $Scene }
    $sceneDirectory = Join-Path $runDirectory $(if ($SourceMatched -or $Scene -in @('Network', 'Recycle')) { 'private-source/' + $Scene } else { $Scene })
    New-Item -ItemType Directory -Path $sceneDirectory -Force | Out-Null
    $screenshot = Join-Path $sceneDirectory ($Suffix + '.png')
    $capture = Join-Path $sceneDirectory ($Suffix + '.json')
    $location = if ($SourceMatched -and $Scene -eq 'Home') { 'shell:Desktop' }
        elseif ($SourceMatched -and $Scene -in @('Computer', 'Drive')) { 'shell:MyComputerFolder' }
        elseif ($Scene -eq 'Network') { 'shell:::{f02c1a0d-be21-4350-88b0-7367fc96ef3c}' }
        elseif ($Scene -eq 'Recycle') { 'shell:::{645ff040-5081-101b-9f08-00aa002f954e}' }
        elseif ($SourceMatched -and $Scene.StartsWith('Modern')) { 'shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}' }
        elseif ($SourceMatched -and $Scene -eq 'Compressed') { $compressedSourceFixture }
        elseif ($Scene -eq 'Compressed') { Join-Path $fixture 'Archive.zip' }
        elseif ($Scene -eq 'Search') { Join-Path $fixture 'Owned search.search-ms' }
        elseif ($Scene -eq 'Library') { Join-Path $fixture 'Owned library.library-ms' }
        elseif ($Scene -eq 'Music') { Join-Path $fixture 'Music' }
        elseif ($Scene -eq 'Video') { Join-Path $fixture 'Videos' }
        else { $fixture }
    $argumentList = @('--headless-visual', '--screenshot', ('"' + $screenshot + '"'),
        '--report', ('"' + $capture + '"'), '--page', $nativePage, '--width', $Width, '--height', $Height, '--dpi', 96, '--theme', 'Light')
    if ($SourceMatched -and $Scene -eq 'Library') { $argumentList += '--source-documents-library' }
    else { $argumentList += @('--path', ('"' + $location + '"')) }
    if ($InstalledRibbon) { $argumentList += '--installed-ribbon' }
    $searchMenuRequested = $false
    if ($Scene -eq 'Search') {
        $searchProtocol = @($manifest.scenes | Where-Object { $_.name -eq 'Search' })
        $searchMenuRequested = $searchProtocol.Count -eq 1 -and
            ([string]$searchProtocol[0].state).Contains('Date modified menu open')
        if ($searchMenuRequested) { $argumentList += '--open-search-date-menu' }
    }
    if ($CrashDiagnostics) {
        $dumpFilename = Join-Path $privateDumpDirectory ($Scene + '-' + $Suffix + '-' + [Guid]::NewGuid().ToString('N') + '.dmp')
        $argumentList += @('--crash-dump', ('"' + $dumpFilename + '"'))
    }
    if ($SourceMatched -and $Scene -eq 'Home') {
        $argumentList += @('--select-shell', '::{59031a47-3f72-44a7-89c5-5595fe6b30ee}', '--view', 'Tiles')
    } elseif ($SourceMatched -and $Scene -eq 'Drive') {
        # Read eligibility from the actual volume containing the owned fixture.
        # Double the trailing backslash before the closing command-line quote.
        $volume = [IO.Path]::GetPathRoot($fixture).TrimEnd([char]92)
        $argumentList += @('--select-shell', ('"' + $volume + '\\"'), '--view', 'Tiles')
    } elseif ($SourceMatched -and $Scene.StartsWith('Modern')) {
        $argumentList += @('--view', 'Default')
    } elseif ($SourceMatched -and $Scene -eq 'Compressed') {
        $argumentList += @('--select', 'Archive.zip', '--view', 'Details')
    } elseif ($Scene.StartsWith('Modern')) {
        # Match the reference's actual visible view state through native public
        # view/selection APIs; folder eligibility still comes from Windows.
        $argumentList += @('--select', '"Read me.txt"', '--view', 'Tiles')
    } elseif ($nativePage -eq 'Home') { $argumentList += @('--select', 'Documents') }
    elseif ($nativePage -eq 'Share') { $argumentList += @('--select', '"Read me.txt"') }
    elseif ($nativePage -eq 'View' -or $nativePage -eq 'Picture') { $argumentList += @('--select', 'Photo.png', '--details', '--view', 'Details') }
    elseif ($Scene -eq 'Compressed') { $argumentList += @('--select', '"Read me.txt"', '--view', 'Details') }
    elseif ($Scene -eq 'Application') { $argumentList += @('--select', 'Application.exe', '--view', 'Details') }
    elseif ($Scene -eq 'Shortcut') { $argumentList += @('--select', '"Application shortcut.lnk"', '--view', 'Details') }
    elseif ($Scene -eq 'DiskImage') { $argumentList += @('--select', '"Owned disc image.iso"', '--view', 'Details') }
    elseif ($Scene -eq 'Music') { $argumentList += @('--select', '"Owned audio.wav"', '--view', 'Details') }
    elseif ($Scene -eq 'Video') { $argumentList += @('--select', '"Owned video.avi"', '--view', 'Details') }
    $process = Invoke-VisualChildProcess $executable $argumentList `
        (Join-Path $sceneDirectory ($Suffix + '-stdout.log')) (Join-Path $sceneDirectory ($Suffix + '-stderr.log')) `
        (Join-Path $sceneDirectory ($Suffix + '-process.json')) "Private-desktop native capture $Scene/$Suffix"
    if ($SourceMatched -and $Scene -eq 'Library' -and $process.ExitCode -eq 9 -and
        (Test-Path -LiteralPath $capture) -and -not (Test-Path -LiteralPath $screenshot)) {
        $unavailable = Get-Content -LiteralPath $capture -Raw | ConvertFrom-Json
        if ($unavailable.headless -ne $true -or $unavailable.privateDesktop -ne $true -or
            $unavailable.inputDesktopUnchanged -ne $true -or $unavailable.visibleInputDesktopWindows -ne $false -or
            $unavailable.documentsLibrarySource.requested -ne $true -or
            $unavailable.documentsLibrarySource.displayUnsupported -ne $true) {
            throw 'Documents Library preflight did not prove the explicit isolated display restriction.'
        }
        $source = $unavailable.documentsLibrarySource
        if ($source.unavailable -ne $true -and ($source.leaseReadHresult -ne 0 -or $source.writeProtected -ne $true -or
            $source.verifyReadHresult -ne 0 -or $source.currentMatches -ne $true -or
            $source.backingFileUnchanged -ne $true -or $source.metadataUnchanged -ne $true)) {
            throw 'Protected Documents Library metadata did not preserve its exact native source.'
        }
        if ($source.unavailable -eq $true -and $source.resolveReadHresult -ge 0 -and
            $source.leaseReadHresult -ge 0 -and $source.loadReadHresult -ge 0 -and $source.fileReadHresult -ge 0) {
            throw 'Documents Library unavailable preflight lacks a native failure HRESULT.'
        }
        return [PSCustomObject]@{ SourceRestricted = $true; Inventory = $unavailable; Capture = $capture; Screenshot = $null }
    }
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $capture) -or -not (Test-Path -LiteralPath $screenshot)) {
        throw "Private-desktop native capture failed: $Scene exit $($process.ExitCode); artifacts: $sceneDirectory"
    }
    $inventory = Get-Content -LiteralPath $capture -Raw | ConvertFrom-Json
    if ($SourceMatched -and $Scene -eq 'Library') { throw 'Current-profile Library display must stop before App creation.' }
    if ($inventory.commandReadiness.requested -ne $true -or $inventory.commandReadiness.ready -ne $true -or
        $inventory.commandReadiness.readHresult -ne 0 -or $inventory.commandReadiness.pendingCapabilities -ne 0 -or
        $inventory.commandReadiness.workerTasks -ne 0 -or $inventory.commandReadiness.selectionBatchPending -ne $false -or
        @($inventory.nativeRibbonProviders | Where-Object { $_.pending -eq $true }).Count -ne 0) {
        throw "Native contextual command state did not settle before capture: $Scene"
    }
    $requestedLayout = if ($InstalledRibbon) { 'InstalledWindows10' } else { 'Authored' }
    if ($inventory.ribbonLayout -ne $requestedLayout -or ($InstalledRibbon -and $inventory.installedRibbonStatus -ne 0)) {
        throw "Native Ribbon layout did not match the requested backend: requested $requestedLayout; actual $($inventory.ribbonLayout); HRESULT $($inventory.installedRibbonStatus)"
    }
    $nativeContexts = @($inventory.nativeRibbonContexts)
    if ($inventory.installedFeatures.readRequested -ne $true -or $nativeContexts.Count -ne 11 -or
        @($nativeContexts | Where-Object { $_.readHresult -ne 0 -or $_.nativeIdentifier -le 0 -or
            $_.availability -notin @(0, 1, 2) }).Count -ne 0 -or
        @($nativeContexts.logicalContext | Select-Object -Unique).Count -ne 11) {
        throw "Native feature/template provenance was absent or invalid: $Scene"
    }
    if ($SourceMatched -and $Scene.StartsWith('Modern')) {
        $addressEntries = @($inventory.widgets | Where-Object { $_.id -eq 104 -and $_.class -eq 'Edit' })
        if ($addressEntries.Count -ne 1 -or -not ([string]$addressEntries[0].text).EndsWith(
                '::{679f85cb-0220-4080-b29b-5540cc05aab6}', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Modern source fixture did not prove the actual native Quick Access namespace: $Scene"
        }
    }
    if ($Scene -eq 'Recycle') {
        $addressEntries = @($inventory.widgets | Where-Object { $_.id -eq 104 -and $_.class -eq 'Edit' })
        if ($addressEntries.Count -ne 1 -or -not ([string]$addressEntries[0].text).EndsWith(
                '::{645ff040-5081-101b-9f08-00aa002f954e}', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Recycle source fixture did not prove the actual read-only Recycle Bin namespace.'
        }
    }
    if ($SourceMatched -and $Scene -eq 'Compressed') {
        $addressEntries = @($inventory.widgets | Where-Object { $_.id -eq 104 -and $_.class -eq 'Edit' })
        $extractAll = @($inventory.nativeRibbonCommands | Where-Object { $_.id -eq 172 })
        $destinations = @($inventory.nativeRibbonCommands | Where-Object { $_.id -eq 1220 })
        $archiveSelection = @($inventory.nativeRibbonProviders | Where-Object { $_.command -eq 172 })
        if ($addressEntries.Count -ne 1 -or -not [StringComparer]::OrdinalIgnoreCase.Equals(
                [string]$addressEntries[0].text, $compressedSourceFolderNativePath) -or
            $archiveSelection.Count -ne 1 -or $archiveSelection[0].selectedCount -ne 1 -or
            $extractAll.Count -ne 1 -or $extractAll[0].enabledReadHresult -ne 0 -or $extractAll[0].enabled -ne $true -or
            $destinations.Count -ne 1 -or $destinations[0].enabledReadHresult -ne 0 -or $destinations[0].enabled -ne $false -or
            (Get-FileHash -LiteralPath $compressedSourceArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $compressedSourceArchiveSha256) {
            throw 'Compressed comparison did not prove its owned containing Downloads folder, one selected intact ZIP, native Extract all enabled and native Extract-to gallery disabled.'
        }
    }
    if ($searchMenuRequested -and ($inventory.searchDateMenu.requested -ne $true -or
            $inventory.searchDateMenu.readHresult -ne 0 -or $inventory.searchDateMenu.expanded -ne $true -or
            $inventory.searchDateMenu.expectedRows -ne 8 -or $inventory.searchDateMenu.matchedRows -ne 8 -or
            $inventory.searchDateMenu.relativeToday -ne $true -or $inventory.searchDateMenu.scopeReadHresult -ne 0 -or
            $inventory.searchDateMenu.resultReadHresult -ne 0 -or $inventory.searchDateMenu.resultCount -ne 2 -or
            $inventory.searchDateMenu.expectedResults -ne 2 -or $inventory.searchDateMenu.matchedIdentities -ne 2 -or
            $inventory.searchDateMenu.unexpectedPaths -ne 0 -or $inventory.searchDateMenu.duplicateIdentities -ne 0 -or
            $inventory.searchDatePopup.window -eq 0 -or $inventory.searchDatePopup.readHresult -ne 0 -or
            $inventory.searchDatePopup.ownedPrivate -ne $true -or $inventory.searchDatePopup.printed -ne $true -or
            $inventory.searchDatePopup.physicalRows -ne 8 -or $inventory.searchDatePopup.uniqueColors -lt 12 -or
            $inventory.searchDatePopup.inkFraction -lt 0.002 -or $inventory.searchDatePopup.unpaintedFraction -gt 0.001 -or
            $inventory.searchDatePopup.minimumRowUniqueColors -lt 2 -or $inventory.searchDatePopup.minimumRowInkFraction -lt 0.002)) {
        throw 'Search capture did not prove the declared native Date modified popup and all eight installed filter rows.'
    }
    if ($searchMenuRequested -and ($inventory.searchDateExpansion.executionAttempts -ne 0 -or
            $inventory.searchDateExpansion.ribbonWindow -eq 0 -or $inventory.searchDateExpansion.nativeCommand -eq 0 -or
            @($inventory.searchDateExpansion.parents | Where-Object accepted).Count -ne 1)) {
        throw 'Search Date expansion did not prove one actual Ribbon gallery and zero execution callbacks.'
    }
    if ($searchMenuRequested -and ($inventory.searchDateMenu.submitReadHresult -ne 0 -or
            $inventory.searchDateMenu.submitCount -ne 1 -or $inventory.searchDateMenu.recentCount -ne 1 -or
            $inventory.searchDateMenu.recentMatchesQuery -ne $true -or $inventory.searchDateMenu.scopeNavigationReadHresult -ne 0 -or
            $inventory.searchDateMenu.physicalScopeReady -ne $true -or $inventory.searchDateMenu.savedInputUnchanged -ne $true -or
            $inventory.searchDateMenu.nativeViewChanged -ne $true -or $inventory.searchDateMenu.scopePreserved -ne $true -or
            $inventory.searchDateMenu.historyCommitted -ne $true -or $inventory.searchDateMenu.factoryRetained -ne $true -or
            $inventory.searchDateMenu.retainedFactoriesBefore -ne 0 -or $inventory.searchDateMenu.retainedFactoriesAfter -ne 1 -or
            $inventory.searchDateMenu.navigationDelta -ne 1 -or $inventory.searchDateMenu.recentEnabledReadHresult -ne 0 -or
            $inventory.searchDateMenu.recentEnabled -ne $true)) {
        throw 'Search source fixture did not prove its genuine physical scope, one Enter submission, one completed native search navigation/factory, committed history, sole MRU entry and unchanged saved input.'
    }
    if ($inventory.headless -ne $true -or $inventory.privateDesktop -ne $true -or $inventory.inputDesktopUnchanged -ne $true -or
        $inventory.visibleInputDesktopWindows -ne $false -or $inventory.renderer -ne 'native-PrintWindow-WIC' -or
        $inventory.printWindowSucceeded -ne $true -or $inventory.windowDpi -ne $inventory.layoutDpi -or
        $inventory.uniqueColors -lt 12 -or $inventory.visibleChildren -lt 1 -or $inventory.unpaintedFraction -gt 0.001) {
        throw "Capture did not prove native noninteractive rendering: $Scene"
    }
    if ((Get-FileHash -LiteralPath $comparer -Algorithm SHA256).Hash.ToLowerInvariant() -ne $comparerSha256) {
        throw 'The visual comparison tool changed during the immutable capture run.'
    }
    if ((Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $captureScriptSha256) {
        throw 'The visual capture protocol script changed during the capture run.'
    }
    & $pythonPath -B $comparer --manifest $manifestPath --validate-capture --require-ribbon --actual $screenshot --capture $capture | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Actual native PNG or command band failed capture invariants: $Scene" }
    return [PSCustomObject]@{ SourceRestricted = $false; Inventory = $inventory; Screenshot = $screenshot; Capture = $capture }
}

$results = [Collections.Generic.List[object]]::new()
foreach ($scene in $Scenes) {
    if ($scene -notmatch '^(Home|Share|View|Computer|Network|Picture|Drive|DiskImage|Compressed|Search|Library|Recycle|Application|Shortcut|Music|Video|ModernHome|ModernShare|ModernView)$') {
        throw "Unknown native scene: $scene"
    }
    $referenceScene = @($manifest.scenes | Where-Object { $_.name -eq $scene })
    $width = if ($referenceScene.Count -eq 1) { [int]$referenceScene[0].width } else { 1200 }
    $height = if ($referenceScene.Count -eq 1) { [int]$referenceScene[0].height } else { 800 }
    $row = [ordered]@{ scene = $scene; nativeCapture = $false; referenceComparison = $null; referenceRestriction = $null; error = $null }
    $restricted = @($manifest.unavailableComparisons | Where-Object { $_.scene -eq $scene })
    if ($restricted.Count -eq 1) { $row.referenceRestriction = $restricted[0].reason }
    try {
        $pilot = Invoke-PrivateCapture $scene $width $height 'pilot'
        # Match visible-frame dimensions by changing the actual HWND geometry,
        # never by resizing the captured PNG. Native invisible borders differ.
        $adjustedWidth = $width + ($width - [int]$pilot.Inventory.width)
        $adjustedHeight = $height + ($height - [int]$pilot.Inventory.height)
        $final = Invoke-PrivateCapture $scene $adjustedWidth $adjustedHeight 'native'
        $row.nativeCapture = $true
        $row.nativeWindowDpi = $final.Inventory.windowDpi
        $row.ribbonLayout = $final.Inventory.ribbonLayout
        $row.layoutDpi = $final.Inventory.layoutDpi
        $row.screenshot = $final.Screenshot
        $row.inventory = $final.Capture
        if ($scene -in @('Network', 'Recycle')) {
            $row.sourceFixture = if ($scene -eq 'Recycle') { 'Actual native Recycle Bin namespace; read-only, no selection or command invocation; private item names.' }
                else { 'Native Network namespace; read-only; current network item names are private.' }
            $row.sourceCapturePublishable = $false
            $row.privateSourceInventory = $final.Capture
            $row.privateSourceScreenshot = $final.Screenshot
        }
        if (-not $CaptureOnly -and $referenceScene.Count -eq 1) {
            $comparedCapture = $final
            if ($scene -eq 'Library') {
                $source = Invoke-PrivateCapture $scene $width $height 'protected-metadata' $true
                if ($source.SourceRestricted -ne $true) { throw 'Current-profile Library display did not stop at protected metadata.' }
                $row.referenceRestriction = 'Current-profile Library display is unsupported: native browsing can rewrite its descriptor, and a protective read lease changes provider availability. No source image or custom-library substitute was compared.'
                $row.sourceFixture = 'Protected actual Documents Library metadata only; no App or content view created.'
                $row.sourceCapturePublishable = $false
                $row.comparisonCapturePublishable = $false
                $row.privateSourceInventory = $source.Capture
                $row.sourceLibrary = $source.Inventory.documentsLibrarySource
                $results.Add([PSCustomObject]$row)
                continue
            }
            if ($scene -in @('Home', 'Computer', 'Drive', 'Compressed') -or $scene.StartsWith('Modern')) {
                # This read-only native namespace fixture reproduces the source's
                # actual SFGAO selection eligibility. Its Desktop body may have
                # personal item names, so private-source files must not publish.
                $sourcePilot = Invoke-PrivateCapture $scene $width $height 'pilot' $true
                $sourceWidth = $width + ($width - [int]$sourcePilot.Inventory.width)
                $sourceHeight = $height + ($height - [int]$sourcePilot.Inventory.height)
                $comparedCapture = Invoke-PrivateCapture $scene $sourceWidth $sourceHeight 'native' $true
                $row.sourceFixture = if ($scene -eq 'Home') { 'Native Desktop UsersFilesFolder; read-only, private body' }
                    elseif ($scene -eq 'Computer') { 'Native Computer namespace; read-only, private body' }
                    elseif ($scene -eq 'Drive') { 'Native Computer namespace and actual owned-fixture volume; read-only, private drive labels' }
                    elseif ($scene -eq 'Compressed') { 'Owned Downloads containing folder with Archive.zip selected; source-native Extract all enabled and destination gallery disabled. Primary inside-ZIP member capture retained separately.' }
                    else { 'Native Quick Access namespace; read-only, private body' }
                $row.sourceCapturePublishable = $false
                $row.privateSourceInventory = $comparedCapture.Capture
                $row.privateSourceScreenshot = $comparedCapture.Screenshot
            }
            $comparisonDirectory = if ($scene -eq 'Recycle') { Join-Path $runDirectory 'private-source/Recycle/comparison' }
                else { Join-Path $runDirectory 'comparison' }
            if ($scene -eq 'Recycle') {
                $row.privateDerivedComparisonDirectory = $comparisonDirectory
                $row.comparisonCapturePublishable = $false
            }
            $comparisonArguments = @('-B', $comparer, '--manifest', $manifestPath, '--scene', $scene, '--actual', $comparedCapture.Screenshot,
                '--capture', $comparedCapture.Capture, '--output', $comparisonDirectory)
            if ($scene -in @('Home', 'View')) {
                $chromeScene = 'Modern' + $scene
                $chromePilot = Invoke-PrivateCapture $chromeScene $width $height 'chrome-pilot' $true
                $chromeWidth = $width + ($width - [int]$chromePilot.Inventory.width)
                $chromeHeight = $height + ($height - [int]$chromePilot.Inventory.height)
                $chromeCapture = Invoke-PrivateCapture $chromeScene $chromeWidth $chromeHeight 'chrome-native' $true
                $comparisonArguments += @('--chrome-actual', $chromeCapture.Screenshot, '--chrome-capture', $chromeCapture.Capture)
                $row.chromeSourceFixture = 'Native Quick Access namespace; read-only, private body'
                $row.chromeSourceInventory = $chromeCapture.Capture
                $row.chromeSourceCapturePublishable = $false
            }
            & $pythonPath @comparisonArguments
            $row.referenceComparison = $LASTEXITCODE -eq 0
        }
    } catch {
        $row.error = $_.Exception.Message
    }
    $results.Add([PSCustomObject]$row)
}
$failed = @($results | Where-Object { $_.nativeCapture -ne $true -or $_.referenceComparison -eq $false -or $null -ne $_.error }).Count
$compared = @($results | Where-Object { $null -ne $_.referenceComparison }).Count
$summary = [ordered]@{ headless = $true; privateDesktop = $true; captureOnly = [bool]$CaptureOnly;
    executableSha256 = $binarySha256;
    captureEnvironment = [ordered]@{ os = [Environment]::OSVersion.VersionString;
        windows = Get-ExplorerWindowsEnvironment;
        uiCulture = [Globalization.CultureInfo]::CurrentUICulture.Name;
        culture = [Globalization.CultureInfo]::CurrentCulture.Name; requestedTheme = 'Light';
        sourceBuildThemeAccent = 'Not stated by publishers; palette and state differences remain compared.' };
    crashDiagnostics = [bool]$CrashDiagnostics; diagnosticPdbSha256 = $diagnosticPdbSha256;
    chosenLayout = $(if ($InstalledRibbon) { 'InstalledWindows10' } else { 'Authored' });
    referenceManifestSha256 = $manifestSha256; referenceManifest = $manifestPath; comparisonToolSha256 = $comparerSha256;
    captureScriptSha256 = $captureScriptSha256;
    fixtureBuilderSha256 = $fixtureBuilderSha256; fixtureBuilderReport = $fixtureBuilderReport;
    fixtureBuilderFirstStageReport = $fixtureBuilderFirstReport; fixtureBuilderStages = $fixtureBuilderStages.ToArray();
    publicationReviewRequired = $true;
    publicationScope = 'Owned file contents; native navigation may show current-profile pins. Publish clean CI-profile captures or review local images before distribution. Never publish private-source files.';
    passed = $failed -eq 0; failed = $failed; results = $results; referenceScenesCompared = $compared;
    allRequestedScenesCompared = $compared -eq $Scenes.Count; wholeApplicationParityEstablished = $false;
    referenceCoverage = 'Only scenes with a declared sourced baseline are compared. A capture alone does not establish visual parity.' }
$summaryPath = Join-Path $runDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "Native headless visual artifacts: $runDirectory"
if ($failed -ne 0) { throw "$failed native visual scenes failed; review $summaryPath" }
Write-Host 'Native capture checks passed. Reference comparison scope is recorded per scene.'
