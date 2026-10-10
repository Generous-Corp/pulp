<#
.SYNOPSIS
    Configure once and incrementally build Pulp in a reusable Windows tree.

.DESCRIPTION
    MSBuild projects generated in a Windows VM can repeatedly re-run CMake when
    source and build trees have different timestamp behavior. This wrapper
    records the exact source/configuration fingerprint and enables CMake's
    regeneration suppression only for a matching warm tree. A source or option
    change invalidates the fingerprint and forces a fresh configure before the
    next build.

    The default is a Release build with four MSBuild workers. Use -Reconfigure
    after changing toolchain or dependency state. This is a local VM helper;
    CI keeps its existing configure and validation contract.
#>

[CmdletBinding()]
param(
    [string]$Source = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Build = $(if ($env:PULP_WINDOWS_BUILD) { $env:PULP_WINDOWS_BUILD } else { 'C:\pulp-build\warm' }),
    [ValidateSet('ARM64EC', 'ARM64', 'x64')][string]$Platform = $(if ($env:PULP_WINDOWS_PLATFORM) { $env:PULP_WINDOWS_PLATFORM } else { 'ARM64EC' }),
    [ValidateRange(1, 32)][int]$Jobs = $(if ($env:PULP_WINDOWS_JOBS) { [int]$env:PULP_WINDOWS_JOBS } else { 4 }),
    [string]$Target = 'ALL_BUILD',
    [switch]$Reconfigure,
    [switch]$Clean,
    [switch]$Install,
    [string]$InstallPrefix = $(if ($env:PULP_WINDOWS_INSTALL) { $env:PULP_WINDOWS_INSTALL } else { '' })
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param([Parameter(Mandatory = $true)][string]$File, [Parameter(ValueFromRemainingArguments = $true)][object[]]$Arguments)
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "'$File' failed with exit code $LASTEXITCODE"
    }
}

function Get-SourceFingerprint {
    param([string]$Root)
    $head = (& git -C $Root rev-parse HEAD 2>$null).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $head) { throw "Source is not a Git checkout: $Root" }
    $status = (& git -C $Root status --porcelain=v1 --untracked-files=all 2>$null) -join "`n"
    $diff = (& git -C $Root diff --binary 2>$null) -join "`n"
    $staged = (& git -C $Root diff --cached --binary 2>$null) -join "`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes("$head`n$status`n$diff`n$staged")
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

$Source = [IO.Path]::GetFullPath($Source)
$Build = [IO.Path]::GetFullPath($Build)
$statePath = Join-Path $Build 'pulp-warm-build.json'
$fingerprint = Get-SourceFingerprint $Source
$options = [ordered]@{
    source = $Source
    platform = $Platform
    jobs = $Jobs
    generator = 'Visual Studio 17 2022'
    configuration = 'Release'
    suppress_regeneration = $true
    vc_tools = [string]$env:VCToolsInstallDir
    windows_sdk = [string]$env:WindowsSdkDir
    visual_studio = [string]$env:VisualStudioVersion
}
$optionsJson = $options | ConvertTo-Json -Compress
$needsConfigure = $Reconfigure -or -not (Test-Path (Join-Path $Build 'CMakeCache.txt'))
if (Test-Path $statePath) {
    $state = Get-Content -Raw $statePath | ConvertFrom-Json
    if ($state.fingerprint -ne $fingerprint -or $state.options -ne $optionsJson) {
        $needsConfigure = $true
    }
} else {
    $needsConfigure = $true
}

if ($Clean) {
    if (Test-Path $Build) { Remove-Item -LiteralPath $Build -Recurse -Force }
    $needsConfigure = $true
}
New-Item -ItemType Directory -Path $Build -Force | Out-Null

if ($needsConfigure) {
    Write-Host "[pulp] configuring $Platform (source fingerprint $fingerprint)"
    $configure = @('-S', $Source, '-B', $Build, '-G', 'Visual Studio 17 2022', '-A', $Platform,
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_SUPPRESS_REGENERATION=ON',
        '-DPULP_BUILD_EXAMPLES=OFF')
    Invoke-Native cmake @configure
    [ordered]@{ fingerprint = $fingerprint; options = $optionsJson; configured_at = [DateTimeOffset]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
} else {
    Write-Host "[pulp] reusing configured warm tree $Build (fingerprint $fingerprint)"
}

$buildArgs = @('--build', $Build, '--config', 'Release', '--target', $Target, '--parallel', $Jobs)
Write-Host "[pulp] incremental build target=$Target jobs=$Jobs"
Invoke-Native cmake @buildArgs

if ($Install) {
    if (-not $InstallPrefix) { throw '-Install requires -InstallPrefix' }
    Write-Host "[pulp] installing to $InstallPrefix"
    Invoke-Native cmake @('--install', $Build, '--config', 'Release', '--prefix', $InstallPrefix)
}
