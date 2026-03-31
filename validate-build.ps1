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

New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

function Cleanup {
    if ($KeepWorktree) {
        Write-Host "Keeping validation worktree at $SrcDir"
        return
    }
    try { git -C $Root worktree remove --force $SrcDir *> $null } catch {}
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

try {
    if (-not $Quiet) { Write-Host "Creating clean validation worktree..." }
    git -C $Root worktree add --detach $SrcDir $Ref *> $null

    Run-OrDump "dependency bootstrap" $SetupLog {
        bash -lc "cd '$SrcDir' && ./setup.sh --ci --deps-only"
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
