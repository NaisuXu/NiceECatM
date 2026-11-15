# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

param([string]$Executable = 'dist/windows-x64/NiceECatM.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $projectRoot
$oldVideo = $env:SDL_VIDEODRIVER
$oldRenderer = $env:SDL_RENDER_DRIVER
$oldCapture = $env:NICEECATM_DISABLE_PCAP
try {
    $sourceApp = (Resolve-Path -LiteralPath $Executable).Path
    $packageRoot = Split-Path -Parent $sourceApp
    $smokeRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build\windows-mingw-release\smoke'))
    $isolatedRoot = Join-Path $smokeRoot 'app'
    $workspacePrefix = [IO.Path]::GetFullPath($projectRoot).TrimEnd('\') + '\'
    if (!$smokeRoot.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $sourceApp.StartsWith($smokeRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The smoke directory must be inside this workspace and separate from the source executable.'
    }
    # The desktop stores scripts and preferences beside its executable. Run a
    # private copy so UI checks never modify the installed application's data.
    if (Test-Path -LiteralPath $isolatedRoot) {
        Remove-Item -LiteralPath $isolatedRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $isolatedRoot | Out-Null
    $app = Join-Path $isolatedRoot 'NiceECatM.exe'
    Copy-Item -LiteralPath $sourceApp -Destination $app
    foreach ($resourceDirectory in @('esi', 'scripts')) {
        $resourcePath = Join-Path $packageRoot $resourceDirectory
        if (Test-Path -LiteralPath $resourcePath -PathType Container) {
            Copy-Item -LiteralPath $resourcePath -Destination $isolatedRoot -Recurse
        }
    }
    $env:SDL_VIDEODRIVER = 'dummy'
    $env:SDL_RENDER_DRIVER = 'software'
    foreach ($language in @('en', 'zh-CN')) {
        foreach ($tab in @('device', 'registers', 'pdo', 'mailbox', 'sdo', 'diagnostics', 'scripts')) {
            & $app --ui-smoke --language $language --tab $tab --device 5 --frames 25 --screenshot (Join-Path $smokeRoot "$language-$tab.bmp")
            if ($LASTEXITCODE -ne 0) { throw "GUI smoke failed: $language / $tab" }
        }
        foreach ($size in @(@(1600, 1000), @(1366, 768), @(1100, 720))) {
            $capture = Join-Path $smokeRoot "$language-pdo-$($size[0])x$($size[1]).bmp"
            & $app --ui-smoke --language $language --tab pdo --width $size[0] --height $size[1] --frames 25 --screenshot $capture
            if ($LASTEXITCODE -ne 0) { throw "Responsive GUI smoke failed: $language / $size" }
            $bitmap = [IO.File]::ReadAllBytes($capture)
            if ([BitConverter]::ToInt32($bitmap,18) -ne $size[0] -or [Math]::Abs([BitConverter]::ToInt32($bitmap,22)) -ne $size[1]) {
                throw "Unexpected capture dimensions: $capture"
            }
            # Exercise a populated history chart. The cyclic smoke mode also
            # checks the page/card scroll ranges, so an empty PRE-OP chart
            # cannot hide a plot-size regression.
            $historyCapture = Join-Path $smokeRoot "$language-diagnostics-running-$($size[0])x$($size[1]).bmp"
            & $app --ui-smoke-cyclic --language $language --tab diagnostics --width $size[0] --height $size[1] --frames 60 --screenshot $historyCapture
            if ($LASTEXITCODE -ne 0) { throw "Running diagnostics smoke failed: $language / $size" }
        }
    }
    $simulation = (& $app --simulate --scan | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or !$simulation.ok -or !$simulation.simulation -or $simulation.slaves.Count -ne 5) { throw 'Simulation scan failed.' }
    $env:NICEECATM_DISABLE_PCAP = '1'
    $missingDriver = (& $app --adapters | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 1 -or $missingDriver.ok) { throw 'Missing driver did not produce an explicit failure.' }
    & $app --ui-smoke --frames 25 --screenshot (Join-Path $smokeRoot 'no-driver.bmp')
    if ($LASTEXITCODE -ne 0) { throw 'GUI failed without a capture driver.' }
    Write-Output '14 bilingual page captures, 6 window-size captures, 6 populated diagnostics layout checks, simulation scan and missing-driver checks passed. No hardware scan performed.'
} finally {
    $env:SDL_VIDEODRIVER = $oldVideo
    $env:SDL_RENDER_DRIVER = $oldRenderer
    $env:NICEECATM_DISABLE_PCAP = $oldCapture
    Pop-Location
}
