#!/usr/bin/env pwsh
# validate-build.ps1 — clean outer-loop build validation for Windows

param(
    [switch]$Quiet = $true,
    [switch]$Verbose,
    [switch]$NoTests,
    [switch]$KeepWorktree,
    [string]$Ref = "HEAD"
)

if ($Verbose) { $Quiet = $false }
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$TempRoot = Join-Path $env:TEMP ("pulp-validate." + [Guid]::NewGuid().ToString("N"))
$SrcDir = Join-Path $TempRoot "src"
$BuildDir = Join-Path $TempRoot "build"
$InstallDir = Join-Path $TempRoot "install"
$SmokeDir = Join-Path $TempRoot "smoke"
$SetupLog = Join-Path $TempRoot "setup.log"
$ConfigureLog = Join-Path $TempRoot "configure.log"
$BuildLog = Join-Path $TempRoot "build.log"
$InstallLog = Join-Path $TempRoot "install.log"
$SmokeLog = Join-Path $TempRoot "smoke.log"
$TestLog = Join-Path $TempRoot "test.log"
$BashExe = $null
$VcVarsBat = $null
$UsingExistingCheckout = $false

New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

function Cleanup {
    if ($KeepWorktree) {
        Write-Host "Keeping validation worktree at $SrcDir"
        return
    }
    if (-not $UsingExistingCheckout) {
        try { git -C $Root worktree remove --force $SrcDir *> $null } catch {}
    }
    Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
}

function Run-OrDump {
    param(
        [string]$Label,
        [string]$LogFile,
        [scriptblock]$Action
    )
    try {
        & $Action *> $LogFile
        if ($LASTEXITCODE -ne 0) { throw "nonzero exit code" }
    } catch {
        Write-Host ""
        Write-Host "Validation failed during: $Label"
        Write-Host "---- $Label log ----"
        Get-Content $LogFile
        Cleanup
        exit 1
    }
}

function Resolve-Bash {
    $cmd = Get-Command bash -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $fallbacks = @(
        "C:\Program Files\Git\bin\bash.exe",
        "C:\Program Files\Git\usr\bin\bash.exe"
    )
    foreach ($candidate in $fallbacks) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw "bash not found on PATH and no Git Bash fallback was found"
}

function Resolve-VcVars {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return $null
    }

    $vswhere = Get-Command vswhere.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue
    if (-not $vswhere) {
        $bundledVsWhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $bundledVsWhere) { $vswhere = $bundledVsWhere }
    }

    if ($vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($LASTEXITCODE -eq 0 -and $installPath) {
            $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    $fallbacks = @(
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($candidate in $fallbacks) {
        if (Test-Path $candidate) { return $candidate }
    }

    return $null
}

function Convert-ToBashPath {
    param([string]$PathValue)

    $normalized = $PathValue -replace '\\', '/'
    if ($normalized -match '^([A-Za-z]):/(.*)$') {
        return "/" + $matches[1].ToLowerInvariant() + "/" + $matches[2]
    }
    return $normalized
}

function Import-VcVarsEnvironment {
    param([string]$BatchFile)

    if (-not $BatchFile) { return }

    $lines = & cmd.exe /d /c "call ""$BatchFile"" >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "failed to import MSVC environment from $BatchFile"
    }

    foreach ($line in $lines) {
        if ($line -match '^(.*?)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
}

try {
    $BashExe = Resolve-Bash
    $VcVarsBat = Resolve-VcVars
    Import-VcVarsEnvironment -BatchFile $VcVarsBat
    $SrcDirBash = Convert-ToBashPath -PathValue $SrcDir
    if (-not $Quiet) { Write-Host "Creating clean validation worktree..." }
    $worktreeCmd = 'git -C "{0}" worktree add --detach "{1}" "{2}" >nul 2>nul' -f $Root, $SrcDir, $Ref
    & cmd.exe /d /c $worktreeCmd
    if ($LASTEXITCODE -ne 0) {
        $status = git -C $Root status --porcelain
        if ($LASTEXITCODE -ne 0) { throw "git worktree add failed" }
        if ($status) { throw "git worktree add failed and current checkout is dirty" }
        $UsingExistingCheckout = $true
        $SrcDir = $Root
        $SrcDirBash = Convert-ToBashPath -PathValue $SrcDir
        if (-not $Quiet) { Write-Host "Falling back to validating the current checkout because a clean worktree could not be created." }
    }

    Run-OrDump "dependency bootstrap" $SetupLog {
        & $BashExe -lc "cd '$SrcDirBash' && ./setup.sh --ci --deps-only"
    }

    Run-OrDump "configure" $ConfigureLog {
        cmake -S $SrcDir -B $BuildDir -DCMAKE_BUILD_TYPE=Debug
    }

    $Jobs = [Environment]::ProcessorCount
    Run-OrDump "build" $BuildLog {
        cmake --build $BuildDir --config Debug --parallel $Jobs
    }

    Run-OrDump "install" $InstallLog {
        cmake --install $BuildDir --prefix $InstallDir --config Debug
    }

    New-Item -ItemType Directory -Force -Path $SmokeDir | Out-Null
    @"
cmake_minimum_required(VERSION 3.24)
project(PulpSDKSmoke LANGUAGES CXX)

find_package(Pulp REQUIRED CONFIG)

add_library(smoke INTERFACE)
target_link_libraries(smoke INTERFACE Pulp::format)
"@ | Set-Content -Path (Join-Path $SmokeDir "CMakeLists.txt")

    Run-OrDump "install smoke test" $SmokeLog {
        cmake -S $SmokeDir -B (Join-Path $SmokeDir "build") -DCMAKE_PREFIX_PATH=$InstallDir
    }

    if (-not $NoTests) {
        Run-OrDump "test" $TestLog {
            ctest --test-dir $BuildDir --output-on-failure -C Debug
        }
    }

    if (-not $Quiet) {
        Write-Host "Clean validation passed in $BuildDir"
    }
} finally {
    Cleanup
}
