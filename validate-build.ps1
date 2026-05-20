#!/usr/bin/env pwsh
# validate-build.ps1 — clean outer-loop build validation for Windows

param(
    [switch]$Quiet = $true,
    [switch]$Verbose,
    [switch]$NoTests,
    [switch]$Smoke,
    [switch]$KeepWorktree,
    [string]$Ref = "HEAD"
)

if ($Verbose) { $Quiet = $false }
if ($Smoke) { $NoTests = $true }
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tempId = [Guid]::NewGuid().ToString("N").Substring(0, 8)
$TempRoot = Join-Path $env:SystemDrive ("pv." + $tempId)
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
        $previousPreference = $ErrorActionPreference
        try {
            # Windows PowerShell can surface normal stderr chatter from native tools
            # (for example git clone progress) as NativeCommandError records. Judge
            # these validation steps by process exit code instead of stderr noise.
            $ErrorActionPreference = "Continue"
            & $Action *> $LogFile
            if ($LASTEXITCODE -ne 0) { throw "nonzero exit code" }
        } finally {
            $ErrorActionPreference = $previousPreference
        }
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
    $fallbacks = @(
        "C:\Program Files\Git\bin\bash.exe",
        "C:\Program Files\Git\usr\bin\bash.exe"
    )
    foreach ($candidate in $fallbacks) {
        if (Test-Path $candidate) { return $candidate }
    }

    $cmd = Get-Command bash -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    throw "bash not found on PATH and no Git Bash fallback was found"
}

function Resolve-VcVars {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return $null
    }

    $isArm64Host = $env:PROCESSOR_ARCHITECTURE -eq "ARM64"
    $toolComponent = if ($isArm64Host) {
        "Microsoft.VisualStudio.Component.VC.Tools.ARM64"
    } else {
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
    }
    $vcvarsName = if ($isArm64Host) { "vcvarsarm64.bat" } else { "vcvars64.bat" }

    $vswhere = Get-Command vswhere.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue
    if (-not $vswhere) {
        $bundledVsWhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $bundledVsWhere) { $vswhere = $bundledVsWhere }
    }

    if ($vswhere) {
        $installPath = & $vswhere -latest -products * -requires $toolComponent -property installationPath 2>$null
        if ($LASTEXITCODE -eq 0 -and $installPath) {
            $candidate = Join-Path $installPath ("VC\Auxiliary\Build\" + $vcvarsName)
            if (Test-Path $candidate) { return $candidate }
        }
    }

    $fallbacks = @(
        ("C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\" + $vcvarsName),
        ("C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\" + $vcvarsName),
        ("C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\" + $vcvarsName),
        ("C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\" + $vcvarsName)
    )
    if ($isArm64Host) {
        $fallbacks += @(
            "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
            "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
            "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
            "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        )
    }
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

    $wrapper = Join-Path $env:TEMP ("pulp-vcvars-" + [Guid]::NewGuid().ToString("N") + ".cmd")
    try {
        $cmdLine = 'call "' + $BatchFile + '" >nul'
        @("@echo off", $cmdLine, "set") | Set-Content -Path $wrapper -Encoding ASCII
        $lines = & cmd.exe /d /c $wrapper
        if ($LASTEXITCODE -ne 0) {
            throw "failed to import MSVC environment from $BatchFile"
        }
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $wrapper
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

    $configureArgs = @(
        "-S", $SrcDir,
        "-B", $BuildDir,
        "-DCMAKE_BUILD_TYPE=Release"
    )
    if ($Smoke) {
        $configureArgs += @(
            "-DPULP_BUILD_TESTS=OFF",
            "-DPULP_BUILD_EXAMPLES=OFF",
            "-DPULP_ENABLE_GPU=OFF"
        )
    }
    Run-OrDump "configure" $ConfigureLog {
        cmake @configureArgs
    }

    $Jobs = [Environment]::ProcessorCount
    Run-OrDump "build" $BuildLog {
        cmake --build $BuildDir --config Release --parallel $Jobs
    }

    Run-OrDump "install" $InstallLog {
        cmake --install $BuildDir --prefix $InstallDir --config Release
    }

    $SmokeBuildLog = Join-Path $TmpRoot "smoke-build.log"
    New-Item -ItemType Directory -Force -Path $SmokeDir | Out-Null

    Run-OrDump "install smoke configure" $SmokeLog {
        cmake -S (Join-Path $SrcDir "tools/validation/sdk-smoke") -B (Join-Path $SmokeDir "build") "-DCMAKE_PREFIX_PATH=$InstallDir"
    }

    Run-OrDump "install smoke build" $SmokeBuildLog {
        cmake --build (Join-Path $SmokeDir "build") --config Release
    }

    if (-not $NoTests) {
        Run-OrDump "test" $TestLog {
            ctest --test-dir $BuildDir --output-on-failure -C Release
        }
    }

    if (-not $Quiet) {
        Write-Host "Clean validation passed in $BuildDir"
    }
} finally {
    Cleanup
}
