# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build\windows-mingw-release'
$builtExecutable = Join-Path $buildRoot 'NiceECatM.exe'
if (!(Test-Path -LiteralPath $builtExecutable -PathType Leaf)) {
    throw 'Build the Windows release first: ./scripts/build-windows.ps1'
}
$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($builtExecutable).FileVersion
if ($version -notmatch '^\d{12}$') {
    throw "Expected a yyyymmddhhmm file version, received '$version'."
}

$packageName = "NiceECatM-$version-windows-x64"
$packageWorkspace = [IO.Path]::GetFullPath((Join-Path $buildRoot 'package'))
$stageContainer = Join-Path $packageWorkspace ([Guid]::NewGuid().ToString('N'))
$stageRoot = Join-Path $stageContainer $packageName
$temporaryArchive = Join-Path $stageContainer "$packageName.zip"
$temporaryChecksum = "$temporaryArchive.sha256"
$outputRoot = Join-Path $projectRoot 'dist'
$archivePath = Join-Path $outputRoot "$packageName.zip"
$checksumPath = "$archivePath.sha256"

function Remove-PackageStage {
    # Delete only this invocation's private staging files, followed by empty
    # directories. The installed dist/windows-x64 directory is never cleaned.
    $resolvedStage = [IO.Path]::GetFullPath($stageContainer)
    if (!$resolvedStage.StartsWith($packageWorkspace + '\', [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolvedStage) -notmatch '^[0-9a-f]{32}$') {
        throw 'Refusing to clean a directory outside the private package stage.'
    }
    if (!(Test-Path -LiteralPath $resolvedStage -PathType Container)) { return }
    $stageEntries = @(Get-ChildItem -LiteralPath $resolvedStage -Force -Recurse)
    if ($stageEntries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw 'Refusing to clean a package stage containing filesystem links.'
    }
    foreach ($stageEntry in $stageEntries) {
        if (!$stageEntry.FullName.StartsWith($resolvedStage + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to clean a file outside the private package stage.'
        }
    }
    foreach ($stageFile in $stageEntries | Where-Object { !$_.PSIsContainer }) {
        Remove-Item -LiteralPath $stageFile.FullName -Force
    }
    foreach ($stageDirectory in $stageEntries | Where-Object { $_.PSIsContainer } | Sort-Object { $_.FullName.Length } -Descending) {
        Remove-Item -LiteralPath $stageDirectory.FullName -Force
    }
    Remove-Item -LiteralPath $resolvedStage -Force
}

try {
    New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $cmake --install $buildRoot --prefix $stageRoot --strip
        $installExitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($installExitCode -ne 0) { throw "cmake install failed with exit code $installExitCode." }

    foreach ($requiredFile in @('NiceECatM.exe', 'LICENSE', 'README.md', 'README.zh-CN.md', 'THIRD_PARTY.md')) {
        if (!(Test-Path -LiteralPath (Join-Path $stageRoot $requiredFile) -PathType Leaf)) {
            throw "The installed package is missing $requiredFile. Reconfigure and rebuild the Windows release."
        }
    }
    $installedVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $stageRoot 'NiceECatM.exe')).FileVersion
    if ($installedVersion -ne $version) { throw 'The installed executable version differs from the built executable.' }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stageRoot, $temporaryArchive, [IO.Compression.CompressionLevel]::Optimal, $true)
    $hash = (Get-FileHash -LiteralPath $temporaryArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText($temporaryChecksum, "$hash  $packageName.zip`n", (New-Object Text.UTF8Encoding($false)))
    New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
    Move-Item -LiteralPath $temporaryArchive -Destination $archivePath -Force
    Move-Item -LiteralPath $temporaryChecksum -Destination $checksumPath -Force
    Write-Output "Package: $archivePath"
    Write-Output "SHA256: $hash"
    Write-Output "Checksum: $checksumPath"
} finally {
    Remove-PackageStage
}
