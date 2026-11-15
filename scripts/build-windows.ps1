# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

param(
    [ValidateRange(1, 128)][int]$Jobs = 8,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
function Invoke-BuildTool([string]$Tool, [string[]]$ToolArguments) {
    $toolPath = (Get-Command $Tool -ErrorAction Stop).Source
    $previousPreference = $ErrorActionPreference
    try {
        # Windows PowerShell treats redirected native stderr as ErrorRecord,
        # including nonfatal compiler warnings. Check the process exit code.
        $ErrorActionPreference = 'Continue'
        & $toolPath @ToolArguments
        $toolExitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($toolExitCode -ne 0) { throw "$Tool failed with exit code $toolExitCode." }
}
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $projectRoot
try {
    $configureArguments = @('--preset', 'windows-mingw-release')
    $buildArguments = @('--build', '--preset', 'windows-mingw-release', '--parallel', "$Jobs")
    if ($Clean) {
        # Recreate the CMake configuration and rebuild every target from the
        # local dependency sources retained under third_party.
        $configureArguments += '--fresh'
        $buildArguments += '--clean-first'
    }
    Invoke-BuildTool cmake $configureArguments
    Invoke-BuildTool cmake $buildArguments
    Invoke-BuildTool ctest @('--preset', 'windows-mingw-release')
    # Install over the portable directory. Do not delete user scripts, ESI
    # descriptions or preferences already stored beside the executable.
    Invoke-BuildTool cmake @('--install', 'build/windows-mingw-release', '--prefix', 'dist/windows-x64', '--strip')
} finally { Pop-Location }
