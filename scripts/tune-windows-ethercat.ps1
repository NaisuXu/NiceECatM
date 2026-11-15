# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$AdapterName,
    [ValidateSet('Inspect', 'Apply', 'Restore')][string]$Mode = 'Inspect',
    [ValidateSet('InterruptModeration', 'EEE')][ValidateNotNullOrEmpty()][string[]]$Setting = @('InterruptModeration'),
    [ValidateNotNullOrEmpty()][string]$BackupPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$allowedKeywords = @('*InterruptModeration', '*EEE')
$backupSchema = 'NiceECatM.AdapterTuning.v1'
if ($env:OS -ne 'Windows_NT') { throw 'This script requires Windows and the NetAdapter module.' }

function Get-TargetAdapter {
    # Match the literal name after enumeration; never pass a user wildcard to a setter.
    $matches = @(Get-NetAdapter -Physical -ErrorAction Stop | Where-Object { $_.Name -ceq $AdapterName })
    if ($matches.Count -ne 1) { throw 'AdapterName must identify exactly one physical adapter by its full name.' }
    $target = $matches[0]
    if (!$target.HardwareInterface -or $target.NdisMedium -ne 0 -or $target.NdisPhysicalMedium -ne 14) {
        throw 'The selected adapter must be a physical wired Ethernet adapter (802.3).'
    }
    return $target
}

function Get-TargetProperties {
    # The query is read-only. Filter literal names again before retaining CIM objects for writes.
    return @(Get-NetAdapterAdvancedProperty -Name $AdapterName -AllProperties -ErrorAction Stop |
        Where-Object { $_.Name -ceq $AdapterName -and $_.RegistryKeyword -in $allowedKeywords })
}

function Same-Value([string[]]$Left, [string[]]$Right) {
    return (($Left -join ',') -ceq ($Right -join ','))
}

function Assert-TargetIdentity {
    $current = Get-TargetAdapter
    if ([guid]$current.InterfaceGuid -ne $targetGuid) { throw 'The adapter identity changed; refusing to modify another adapter.' }
    return $current
}

$adapter = Get-TargetAdapter
$targetGuid = [guid]$adapter.InterfaceGuid
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdministrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$properties = @(Get-TargetProperties)
foreach ($keyword in $allowedKeywords) {
    if (@($properties | Where-Object RegistryKeyword -CEQ $keyword).Count -gt 1) {
        throw "Multiple properties matched $keyword; no settings were changed."
    }
}

if ($Mode -eq 'Inspect') {
    [pscustomobject]@{
        AdapterName = $adapter.Name; InterfaceGuid = $targetGuid.ToString('D')
        Description = $adapter.InterfaceDescription; Status = $adapter.Status
        LinkSpeed = $adapter.LinkSpeed; Administrator = $isAdministrator
    } | Format-List
    $inspection = @(foreach ($keyword in $allowedKeywords) {
        $property = @($properties | Where-Object RegistryKeyword -CEQ $keyword)
        [pscustomobject]@{
            RegistryKeyword = $keyword; Supported = ($property.Count -eq 1)
            RegistryValue = if ($property.Count) { $property[0].RegistryValue -join ', ' } else { '-' }
            DisplayValue = if ($property.Count) { $property[0].DisplayValue } else { 'Unsupported' }
        }
    })
    $inspection | Format-Table -AutoSize
    return
}
if (!$isAdministrator) { throw "$Mode requires an administrator PowerShell session. No automatic elevation is performed." }
if ($Mode -eq 'Restore' -and !$BackupPath) { throw 'Restore requires -BackupPath from an earlier Apply operation.' }

$desired = @()
if ($Mode -eq 'Restore') {
    $BackupPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BackupPath)
    $backup = Get-Content -LiteralPath $BackupPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($backup.schema -cne $backupSchema -or [guid]$backup.interfaceGuid -ne $targetGuid) {
        throw 'Backup schema or InterfaceGuid does not match this adapter.'
    }
    $entries = @($backup.properties)
    if ($entries.Count -lt 1 -or $entries.Count -gt $allowedKeywords.Count) { throw 'Invalid backup property count.' }
    $seen = @()
    foreach ($entry in $entries) {
        if ($entry.registryKeyword -cnotin $allowedKeywords -or $entry.registryKeyword -cin $seen) {
            throw 'Backup contains an unsupported or duplicate registry keyword.'
        }
        $seen += $entry.registryKeyword
        $values = @($entry.registryValue)
        $registryNumber = [uint32]0
        if ($values.Count -ne 1 -or ![uint32]::TryParse([string]$values[0], [ref]$registryNumber)) {
            throw 'Backup registry values must contain one unsigned DWORD value.'
        }
        $desired += [pscustomobject]@{ Keyword = $entry.registryKeyword; Value = [string[]]$values }
    }
} else {
    foreach ($keyword in $allowedKeywords) {
        if ($keyword.Substring(1) -notin $Setting) { continue }
        $desired += [pscustomobject]@{ Keyword = $keyword; Value = [string[]]@('0') }
    }
}

$plan = @()
foreach ($entry in $desired) {
    $property = @($properties | Where-Object RegistryKeyword -CEQ $entry.Keyword)
    if (!$property.Count) { Write-Warning "Skipping unsupported property $($entry.Keyword)."; continue }
    $original = [string[]]@($property[0].RegistryValue)
    $originalNumber = [uint32]0
    if ($original.Count -ne 1 -or ![uint32]::TryParse($original[0], [ref]$originalNumber)) {
        throw "Cannot safely back up the original DWORD value of $($entry.Keyword)."
    }
    if (Same-Value $original $entry.Value) { continue }
    $plan += [pscustomobject]@{ Property = $property[0]; Keyword = $entry.Keyword; Value = $entry.Value; Original = $original }
}
if (!$plan.Count) { Write-Output 'No supported settings need changing; the adapter was not restarted.'; return }

if ($Mode -eq 'Apply') {
    if (!$BackupPath) {
        $backupDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'build/adapter-tuning'
        $backupFile = 'adapter-{0}-{1}.json' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'), [guid]::NewGuid().ToString('N')
        $BackupPath = Join-Path $backupDirectory $backupFile
    }
    $BackupPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BackupPath)
    if (Test-Path -LiteralPath $BackupPath) { throw 'BackupPath already exists; existing backups are never overwritten.' }
}
if (!$PSCmdlet.ShouldProcess("$AdapterName ($targetGuid)", "$Mode $($plan.Keyword -join ', '); restart this adapter")) { return }
$null = Assert-TargetIdentity

if ($Mode -eq 'Apply') {
    $backup = [ordered]@{
        schema = $backupSchema; createdUtc = [DateTime]::UtcNow.ToString('o')
        adapterName = $adapter.Name; interfaceGuid = $targetGuid.ToString('D')
        properties = @($plan | ForEach-Object { [ordered]@{ registryKeyword = $_.Keyword; registryValue = @($_.Original) } })
    }
    $jsonBytes = [Text.Encoding]::UTF8.GetBytes(($backup | ConvertTo-Json -Depth 5))
    $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($BackupPath))
    # CreateNew makes the no-overwrite guarantee atomic, including user-supplied paths.
    $stream = [IO.File]::Open($BackupPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($jsonBytes, 0, $jsonBytes.Length); $stream.Flush() } finally { $stream.Dispose() }
    Write-Output "Original values saved to $BackupPath"
}

$attempted = @()
try {
    foreach ($entry in $plan) {
        $null = Assert-TargetIdentity
        # Include an attempted write in rollback even if the driver reports a partial failure.
        $attempted += $entry
        Set-NetAdapterAdvancedProperty -InputObject $entry.Property -RegistryValue $entry.Value -NoRestart -Confirm:$false -ErrorAction Stop
    }
    $restartTarget = Assert-TargetIdentity
    Restart-NetAdapter -InputObject $restartTarget -Confirm:$false -ErrorAction Stop
    $null = Assert-TargetIdentity
    $readback = @(Get-TargetProperties)
    foreach ($entry in $plan) {
        $actual = @($readback | Where-Object RegistryKeyword -CEQ $entry.Keyword)
        if ($actual.Count -ne 1 -or !(Same-Value ([string[]]$actual[0].RegistryValue) $entry.Value)) {
            throw "Readback did not confirm $($entry.Keyword)."
        }
    }
    Write-Output "$Mode completed for $AdapterName; the target adapter was restarted once."
} catch {
    $operationError = $_
    for ($index = $attempted.Count - 1; $index -ge 0; --$index) {
        $entry = $attempted[$index]
        try {
            $null = Assert-TargetIdentity
            Set-NetAdapterAdvancedProperty -InputObject $entry.Property -RegistryValue $entry.Original -NoRestart -Confirm:$false -ErrorAction Stop
        } catch { Write-Warning "Rollback failed for $($entry.Keyword): $($_.Exception.Message)" }
    }
    if ($attempted.Count) {
        try {
            $restartTarget = Assert-TargetIdentity
            Restart-NetAdapter -InputObject $restartTarget -Confirm:$false -ErrorAction Stop
        } catch { Write-Warning "Rollback restart failed: $($_.Exception.Message)" }
    }
    throw "${Mode} failed: $($operationError.Exception.Message). Rollback was attempted; backup retained at $BackupPath."
}
