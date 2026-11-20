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
    $eepromFixtures = @(Get-ChildItem -LiteralPath (Join-Path $isolatedRoot 'esi') -Filter '*.xml' -File)
    if ($eepromFixtures.Count -eq 0) { throw 'The release has no ESI fixtures for EEPROM smoke checks.' }
    $eepromChecks = 0
    $esiExportChecks = 0
    foreach ($language in @('en', 'zh-CN')) {
        # Deterministic injected results; this mode performs no HTTPS requests.
        foreach ($updateState in @('available', 'current', 'unavailable')) {
            & $app --ui-smoke-update $updateState --language $language --width 1100 --height 720 --frames 25 --screenshot (Join-Path $smokeRoot "$language-update-$updateState.bmp")
            if ($LASTEXITCODE -ne 0) { throw "Silent update/header smoke failed: $language / $updateState" }
        }
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
            $dictionaryCapture = Join-Path $smokeRoot "$language-sdo-populated-$($size[0])x$($size[1]).bmp"
            & $app --ui-smoke --language $language --tab sdo --device 4 --width $size[0] --height $size[1] --frames 40 --screenshot $dictionaryCapture
            if ($LASTEXITCODE -ne 0) { throw "Populated SDO dictionary smoke failed: $language / $size" }
            foreach ($eepromFixture in $eepromFixtures) {
                $imageCapture = Join-Path $smokeRoot "$language-eeprom-$($eepromFixture.BaseName)-$($size[0])x$($size[1]).bmp"
                & $app --ui-smoke --language $language --tab eeprom --esi-image $eepromFixture.FullName --width $size[0] --height $size[1] --frames 30 --screenshot $imageCapture
                if ($LASTEXITCODE -ne 0) { throw "Offline EEPROM image/layout smoke failed: $language / $($eepromFixture.Name) / $size" }
                ++$eepromChecks
            }
        }
    }
    foreach ($eepromFixture in $eepromFixtures) {
        $binaryImage = Join-Path $smokeRoot "$($eepromFixture.BaseName).bin"
        $export = (& $app --sii-export $eepromFixture.FullName --output $binaryImage | ConvertFrom-Json)
        if ($LASTEXITCODE -ne 0 -or !$export.ok -or (Get-Item -LiteralPath $binaryImage).Length -ne $export.bytes) {
            throw "Offline SII export failed: $($eepromFixture.Name)"
        }
        $description = Join-Path $smokeRoot "$($eepromFixture.BaseName).generated.xml"
        $env:NICEECATM_DISABLE_PCAP = '1'
        $esiExport = (& $app --esi-export $binaryImage --output $description | ConvertFrom-Json)
        if ($LASTEXITCODE -ne 0 -or !$esiExport.ok -or $esiExport.vendor -ne $export.vendor -or $esiExport.product -ne $export.product -or !$esiExport.limitation) {
            throw "Offline ESI export without capture driver failed: $($eepromFixture.Name)"
        }
        $env:NICEECATM_DISABLE_PCAP = $oldCapture
        $rebuiltImage = Join-Path $smokeRoot "$($eepromFixture.BaseName).generated.bin"
        $esiImport = (& $app --sii-export $description --output $rebuiltImage | ConvertFrom-Json)
        if ($LASTEXITCODE -ne 0 -or !$esiImport.ok -or $esiImport.vendor -ne $export.vendor -or $esiImport.product -ne $export.product) {
            throw "Generated ESI re-import failed: $($eepromFixture.Name)"
        }
        ++$esiExportChecks
        foreach ($language in @('en', 'zh-CN')) {
            & $app --ui-smoke --language $language --tab eeprom --esi-image $binaryImage --frames 30
            if ($LASTEXITCODE -ne 0) { throw "Offline EEPROM BIN import smoke failed: $language / $($eepromFixture.Name)" }
            ++$eepromChecks
            & $app --ui-smoke --language $language --tab eeprom --esi-image $description --frames 30
            if ($LASTEXITCODE -ne 0) { throw "Generated ESI GUI import smoke failed: $language / $($eepromFixture.Name)" }
            ++$esiExportChecks
        }
    }
    $simulation = (& $app --simulate --scan | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or !$simulation.ok -or !$simulation.simulation -or $simulation.slaves.Count -ne 5) { throw 'Simulation scan failed.' }
    $env:NICEECATM_DISABLE_PCAP = '1'
    $missingDriver = (& $app --adapters | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 1 -or $missingDriver.ok) { throw 'Missing driver did not produce an explicit failure.' }
    & $app --ui-smoke --frames 25 --screenshot (Join-Path $smokeRoot 'no-driver.bmp')
    if ($LASTEXITCODE -ne 0) { throw 'GUI failed without a capture driver.' }
    Write-Output "14 bilingual page captures, 6 silent-update/header checks, 6 window-size captures, 6 populated diagnostics, 6 populated SDO dictionary checks, $eepromChecks offline EEPROM XML/BIN checks, $esiExportChecks basic ESI export/re-import checks, simulation scan and missing-driver checks passed. No hardware scan performed."
} finally {
    $env:SDL_VIDEODRIVER = $oldVideo
    $env:SDL_RENDER_DRIVER = $oldRenderer
    $env:NICEECATM_DISABLE_PCAP = $oldCapture
    Pop-Location
}
