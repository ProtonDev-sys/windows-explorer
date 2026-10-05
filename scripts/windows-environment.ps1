function Get-ExplorerWindowsEnvironment {
    $windowsBuildInfo = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $windowsNativeVersions = [ordered]@{}
    foreach ($windowsNativeRelativePath in @('explorer.exe', 'System32/ExplorerFrame.dll', 'System32/UIRibbon.dll')) {
        $windowsNativePath = Join-Path $env:SystemRoot $windowsNativeRelativePath
        $windowsNativeVersions[$windowsNativeRelativePath] = if (Test-Path -LiteralPath $windowsNativePath -PathType Leaf) {
            $windowsNativeInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($windowsNativePath)
            [ordered]@{
                fixedFileVersion = $windowsNativeInfo.FileVersionRaw.ToString()
                fileVersionResource = $windowsNativeInfo.FileVersion
            }
        } else { $null }
    }
    [ordered]@{
        os = [Environment]::OSVersion.VersionString
        buildNumber = [string]$windowsBuildInfo.CurrentBuildNumber
        updateBuildRevision = [int]$windowsBuildInfo.UBR
        fullBuild = ([string]$windowsBuildInfo.CurrentBuildNumber + '.' + [string]$windowsBuildInfo.UBR)
        displayVersion = [string]$windowsBuildInfo.DisplayVersion
        edition = [string]$windowsBuildInfo.EditionID
        installationType = [string]$windowsBuildInfo.InstallationType
        nativeBinaries = $windowsNativeVersions
    }
}
