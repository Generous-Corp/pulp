# Pulp CLI installer for Windows
# Usage: irm https://www.generouscorp.com/pulp/install.ps1 | iex
#
# Environment variables:
#   PULP_VERSION      — install a specific version (default: latest)
#   PULP_INSTALL_DIR  — install directory (default: ~/.pulp/bin)

$ErrorActionPreference = "Stop"

# ── Configuration ────────────────────────────────────────────────────────────

$GithubRepo = "danielraffel/pulp"
$InstallDir = if ($env:PULP_INSTALL_DIR) { $env:PULP_INSTALL_DIR } else { "$env:USERPROFILE\.pulp\bin" }
$Version = if ($env:PULP_VERSION) { $env:PULP_VERSION } else { "latest" }

# ── Resolve version ─────────────────────────────────────────────────────────

if ($Version -eq "latest") {
    Write-Host "  Fetching latest release..."
    try {
        $Release = Invoke-RestMethod -Uri "https://api.github.com/repos/$GithubRepo/releases/latest"
        $Version = $Release.tag_name -replace '^v', ''
    } catch {
        Write-Error "Could not determine latest version. Set PULP_VERSION explicitly."
        exit 1
    }
}

$Arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
switch ($Arch) {
    "X64"   { $Platform = "windows-x64" }
    "Arm64" { $Platform = "windows-arm64" }
    default {
        Write-Error "Unsupported architecture: $Arch"
        exit 1
    }
}

$Zipfile = "pulp-$Platform.zip"
$Url = "https://github.com/$GithubRepo/releases/download/v$Version/$Zipfile"

# ── Download and install ────────────────────────────────────────────────────

Write-Host ""
Write-Host "Installing Pulp CLI v$Version for $Platform..."
Write-Host ""

# Create install directory
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

# Download to temp
$TmpDir = New-Item -ItemType Directory -Force -Path "$env:TEMP\pulp-install-$(Get-Random)"
$TmpFile = Join-Path $TmpDir $Zipfile

try {
    Write-Host "  Downloading $Zipfile..."
    Invoke-WebRequest -Uri $Url -OutFile $TmpFile -UseBasicParsing
} catch {
    Write-Error @"
Download failed.

  The release v$Version may not exist yet for $Platform.
  Check available releases: https://github.com/$GithubRepo/releases

  If you're building from source instead:
    git clone https://github.com/$GithubRepo.git
    cd pulp && .\setup.sh
"@
    exit 1
}

# Extract
Write-Host "  Extracting..."
Expand-Archive -Path $TmpFile -DestinationPath $InstallDir -Force

# Cleanup temp
Remove-Item -Recurse -Force $TmpDir

# Verify
$PulpExe = Join-Path $InstallDir "pulp.exe"
if (-not (Test-Path $PulpExe)) {
    Write-Error "Installation failed - pulp.exe not found at $PulpExe"
    exit 1
}

try {
    $InstalledVersion = (& $PulpExe version 2>$null | Select-Object -First 1)
} catch {
    $InstalledVersion = $null
}
if (-not $InstalledVersion) {
    $InstalledVersion = "pulp v$Version"
}
Write-Host "  Installed: $InstalledVersion"

$PulpCppExe = Join-Path $InstallDir "pulp-cpp.exe"
if (Test-Path $PulpCppExe) {
    Write-Host "  Installed: pulp-cpp delegate"
} else {
    Write-Host "  No pulp-cpp delegate found; this is expected only for pre-0.78.0 releases."
}

# #2067: pulp-mcp is the Claude Code plugin's MCP server. The plugin
# resolves it from PATH via its launcher; pre-#2067 release zips do not
# ship pulp-mcp.
$PulpMcpExe = Join-Path $InstallDir "pulp-mcp.exe"
if (Test-Path $PulpMcpExe) {
    Write-Host "  Installed: pulp-mcp (Claude Code plugin MCP server)"
}

# ── Add to PATH ─────────────────────────────────────────────────────────────

$UserPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($UserPath -notlike "*$InstallDir*") {
    [Environment]::SetEnvironmentVariable("PATH", "$InstallDir;$UserPath", "User")
    Write-Host "  Added $InstallDir to user PATH"
}

# Update current session
if ($env:PATH -notlike "*$InstallDir*") {
    $env:PATH = "$InstallDir;$env:PATH"
}

# ── Done ────────────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "  Done! Run ``pulp new my-plugin`` to create your first plugin."
Write-Host ""
Write-Host "  If 'pulp' is not found, restart your terminal."
Write-Host ""
