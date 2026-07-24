# Pulp CLI installer for Windows
#
# Usage:
#   irm https://www.generouscorp.com/pulp/install.ps1 | iex
#
# Or with options:
#   $env:PULP_VERSION = "0.1.0"; irm https://www.generouscorp.com/pulp/install.ps1 | iex
#
# Scope:
#   Installs Pulp CLI artifacts only. It does not install Shipyard or the
#   GitHub CLI (`gh`); those are optional source-checkout contributor tools.

$ErrorActionPreference = "Stop"

# ── Configuration ────────────────────────────────────────────────────────────

$Repo = "Generous-Corp/pulp"
$InstallDir = if ($env:PULP_INSTALL_DIR) { $env:PULP_INSTALL_DIR } else { "$env:USERPROFILE\.pulp\bin" }
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)
$Version = if ($env:PULP_VERSION) { $env:PULP_VERSION } else { "latest" }
$NoModifyPath = $env:PULP_NO_MODIFY_PATH -eq "1"

# ── Platform detection ───────────────────────────────────────────────────────

$Arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
switch ($Arch) {
    "X64"   { $Platform = "windows-x64" }
    "Arm64" { $Platform = "windows-arm64" }
    default {
        Write-Error "Unsupported architecture: $Arch"
        exit 1
    }
}

Write-Host "Installing Pulp CLI for $Platform..." -ForegroundColor Cyan

# ── Download ─────────────────────────────────────────────────────────────────

if ($Version -eq "latest") {
    Write-Host "Fetching latest release..."
    $ReleaseUrl = "https://api.github.com/repos/$Repo/releases/latest"
    try {
        $Release = Invoke-RestMethod -Uri $ReleaseUrl -Headers @{ "User-Agent" = "pulp-installer" }
        $Asset = $Release.assets | Where-Object { $_.name -like "pulp-$Platform*" } | Select-Object -First 1
        $DownloadUrl = $Asset.browser_download_url
    } catch {
        $DownloadUrl = $null
    }
} else {
    $DownloadUrl = "https://github.com/$Repo/releases/download/v$Version/pulp-$Platform.zip"
}

if (-not $DownloadUrl) {
    Write-Host ""
    Write-Host "Error: could not find release for $Platform" -ForegroundColor Red
    Write-Host ""
    Write-Host "Pre-built binaries may not be available yet."
    Write-Host "To build from source instead:"
    Write-Host "  git clone https://github.com/$Repo.git; cd pulp; powershell -ExecutionPolicy Bypass -File .\setup.ps1"
    exit 1
}

# ── Install ──────────────────────────────────────────────────────────────────

$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) "pulp-install-$(Get-Random)"
New-Item -ItemType Directory -Path $TmpDir -Force | Out-Null

try {
    $ZipPath = Join-Path $TmpDir "pulp.zip"
    Write-Host "Downloading $DownloadUrl..."
    Invoke-WebRequest -Uri $DownloadUrl -OutFile $ZipPath -UseBasicParsing

    # Create install directory
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

    Write-Host "Extracting to $InstallDir..."
    Expand-Archive -Path $ZipPath -DestinationPath $InstallDir -Force

    # Verify
    $PulpExe = Join-Path $InstallDir "pulp.exe"
    if (Test-Path $PulpExe) {
        $InstalledVersion = & $PulpExe --version 2>$null
        Write-Host "Installed: pulp $InstalledVersion" -ForegroundColor Green
    }

    $PulpMcpExe = Join-Path $InstallDir "pulp-mcp.exe"
    if ((Test-Path $PulpMcpExe) -and (-not $NoModifyPath)) {
        $PulpRegistryKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("Software\Pulp")
        try {
            $CurrentUserMcp = [Environment]::GetEnvironmentVariable("PULP_MCP_BINARY", "User")
            $ManagedMcp = $PulpRegistryKey.GetValue(
                "ManagedPulpMcpBinary",
                $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $ShouldManageMcp = (-not $CurrentUserMcp) -or
                ($ManagedMcp -and ($CurrentUserMcp -eq $ManagedMcp))
            if ($ShouldManageMcp) {
                [Environment]::SetEnvironmentVariable("PULP_MCP_BINARY", $PulpMcpExe, "User")
                $PulpRegistryKey.SetValue(
                    "ManagedPulpMcpBinary",
                    $PulpMcpExe,
                    [Microsoft.Win32.RegistryValueKind]::String)
                $env:PULP_MCP_BINARY = $PulpMcpExe
                Write-Host "Configured PULP_MCP_BINARY for the Claude Code plugin" -ForegroundColor Green
            } else {
                Write-Host "Preserved user-managed PULP_MCP_BINARY=$CurrentUserMcp"
            }
        } finally {
            $PulpRegistryKey.Dispose()
        }
    } elseif ((Test-Path $PulpMcpExe) -and $NoModifyPath) {
        Write-Host "Set PULP_MCP_BINARY=$PulpMcpExe before starting Claude Code"
    }
} finally {
    Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
}

# ── PATH ─────────────────────────────────────────────────────────────────────

if (-not $NoModifyPath) {
    $CurrentPath = [Environment]::GetEnvironmentVariable("PATH", "User")
    if ($CurrentPath -notlike "*$InstallDir*") {
        [Environment]::SetEnvironmentVariable("PATH", "$InstallDir;$CurrentPath", "User")
        $env:PATH = "$InstallDir;$env:PATH"
        Write-Host "Added $InstallDir to user PATH" -ForegroundColor Green
    }
} else {
    Write-Host "Skipped user environment changes (PULP_NO_MODIFY_PATH=1)"
}

# ── Done ─────────────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "Pulp CLI installed successfully!" -ForegroundColor Green
Write-Host "This installed Pulp only; it did not install Shipyard or GitHub CLI (gh)."
Write-Host ""
Write-Host "Get started:"
Write-Host "  pulp create MyPlugin        # create your first plugin"
Write-Host "  pulp doctor              # check your environment"
Write-Host ""
Write-Host "Or clone the framework:"
Write-Host "  git clone https://github.com/$Repo.git"
Write-Host "  cd pulp; powershell -ExecutionPolicy Bypass -File .\setup.ps1"
Write-Host "  See docs/guides/local-ci.md for optional source-checkout PR/CI tooling"
Write-Host ""
Write-Host "You may need to restart your terminal for PATH changes to take effect."
