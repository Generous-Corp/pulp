# windows_build_env.ps1 — prepare a Windows CI job to build with Ninja, cl and
# (when it is available) ccache.
#
# The Visual Studio generator ignores CMAKE_<LANG>_COMPILER_LAUNCHER, so under it
# ccache never sees a compile. Building with Ninja needs cl on PATH, which only
# the developer environment provides, so this script:
#
#   1. makes ccache available when it can: one already on PATH is used as is,
#      otherwise choco installs it when choco exists. A runner with neither
#      (the macpro x64 golden has no choco) builds without a compiler cache and
#      says so; that is not an error;
#   2. enters the developer environment for the runner's architecture
#      (vcvars64.bat on X64, vcvarsarm64.bat on ARM64) and exports it to every
#      later step through GITHUB_ENV;
#   3. makes sure ninja is on PATH, installing it with choco when it is missing.
#
# Run it as one `shell: powershell` step before Configure. It is written for
# Windows PowerShell 5.1, which every Windows image has; the macpro golden has
# no PowerShell 7 (pwsh).

$ErrorActionPreference = 'Stop'

if (Get-Command ccache -ErrorAction SilentlyContinue) {
    Write-Host "ccache already on PATH: $((Get-Command ccache).Source)"
} elseif (Get-Command choco -ErrorAction SilentlyContinue) {
    choco install ccache --no-progress -y
    if ($LASTEXITCODE -ne 0) {
        Write-Host "::warning::choco install ccache failed ($LASTEXITCODE); compiling without a compiler cache"
    }
} else {
    Write-Host "::notice::no ccache on PATH and no choco to install it; compiling without a compiler cache"
}

switch ($env:RUNNER_ARCH) {
    'ARM64' {
        $component = 'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
        $vcvarsName = 'vcvarsarm64.bat'
    }
    default {
        $component = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
        $vcvarsName = 'vcvars64.bat'
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere" }
$vs = & $vswhere -latest -products * -requires $component -property installationPath
if (-not $vs) { throw "no Visual Studio installation with $component" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\$vcvarsName"
if (-not (Test-Path $vcvars)) { throw "$vcvarsName not found under $vs" }
Write-Host "Entering the developer environment: $vcvars"
# Written without a byte-order mark: Windows PowerShell 5.1's
# `Out-File -Encoding utf8` can emit one, which would corrupt the first name.
$lines = cmd /c "`"$vcvars`" >nul && set" | Where-Object { $_ -match '^[^=]+=' }
if (-not $lines) { throw "$vcvarsName printed no environment" }
[System.IO.File]::AppendAllText($env:GITHUB_ENV, (($lines -join "`n") + "`n"),
    (New-Object System.Text.UTF8Encoding $false))
# The exported PATH applies from the next step on; adopt it here too, so the
# ninja check below also finds the copy Visual Studio ships.
foreach ($line in $lines) {
    if ($line -match '^Path=(.*)$') { $env:Path = $Matches[1] }
}

if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
        throw 'ninja is not on PATH and choco is not available to install it'
    }
    choco install ninja --no-progress -y
    if ($LASTEXITCODE -ne 0) { throw "ninja install failed ($LASTEXITCODE)" }
}
Write-Host "ninja: $((Get-Command ninja).Source)"
