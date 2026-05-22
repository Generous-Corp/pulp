#!/bin/bash
# Local CLI release builder — builds on macOS + SSH VMs, creates GitHub release
#
# Usage:
#   ./tools/scripts/release-cli-local.sh v0.1.0
#
# Prerequisites:
#   - macOS with Xcode CLT and CMake
#   - SSH access to 'ubuntu' VM (Linux arm64 build)
#   - SSH access to 'win2' VM (Windows x64 build) — optional
#   - gh CLI authenticated (for creating GitHub releases)
#
# For cloud CI builds, push a tag or use workflow_dispatch on release-cli.yml.

set -e

VERSION="${1:?Usage: $0 <version> (e.g., v0.1.0)}"
VERSION_NUM="${VERSION#v}"  # strip leading v

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST_DIR="$REPO_ROOT/dist"
MAC_BUILD_DIR="$REPO_ROOT/build-release-cli"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

info() { echo "  ✓ $*"; }
fail() { echo "  ✗ $*" >&2; }
step() { echo ""; echo "── $* ──"; }

# ── macOS arm64 ──────────────────────────────────────────────────────────────

step "Building macOS arm64"

cmake -S "$REPO_ROOT" -B "$MAC_BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPULP_BUILD_TESTS=OFF \
    -DPULP_BUILD_WEBVIEW=ON 2>&1 | tail -5
cmake --build "$MAC_BUILD_DIR" --target pulp-cli --config Release 2>&1 | tail -3
if [ ! -f "$MAC_BUILD_DIR/tools/cli/pulp" ]; then
    fail "macOS build failed"
    exit 1
fi

cp "$MAC_BUILD_DIR/tools/cli/pulp" "$DIST_DIR/pulp"
strip "$DIST_DIR/pulp"
(cd "$DIST_DIR" && tar czf "pulp-darwin-arm64.tar.gz" pulp && rm pulp)
info "pulp-darwin-arm64.tar.gz ($(du -h "$DIST_DIR/pulp-darwin-arm64.tar.gz" | cut -f1))"

# ── macOS arm64 SDK ────────────────────────────────────────────────────────

step "Building macOS arm64 SDK"

SDK_STAGING="$DIST_DIR/pulp-sdk-staging"
rm -rf "$SDK_STAGING"
cmake --install "$MAC_BUILD_DIR" --prefix "$SDK_STAGING" --config Release 2>&1 | tail -5
if [ -f "$SDK_STAGING/version.txt" ]; then
    SDK_WEBVIEW_LIB="$SDK_STAGING/lib/libpulp-view-core.a" python3 - <<'PY'
from pathlib import Path
import os
data = Path(os.environ["SDK_WEBVIEW_LIB"]).read_bytes()
assert b"WebViewPanel" in data
assert b"make_webview_embedded_resource_fetcher" in data
PY
    (cd "$DIST_DIR" && mv pulp-sdk-staging pulp-sdk && tar czf "pulp-sdk-darwin-arm64.tar.gz" pulp-sdk && rm -rf pulp-sdk)
    info "pulp-sdk-darwin-arm64.tar.gz ($(du -h "$DIST_DIR/pulp-sdk-darwin-arm64.tar.gz" | cut -f1))"
else
    fail "SDK install failed — version.txt not found"
fi

# ── Linux arm64 (via SSH) ───────────────────────────────────────────────────

step "Building Linux arm64 (ssh ubuntu)"

if ssh -o ConnectTimeout=5 -o BatchMode=yes ubuntu "echo ok" &>/dev/null; then
    # Sync source
    rsync -az --delete \
        --exclude='build*' --exclude='.git' --exclude='external/skia-build' \
        --exclude='dist' --exclude='planning' --exclude='node_modules' \
        "$REPO_ROOT/" ubuntu:~/pulp/

    # Configure release build fresh enough to keep SDK contents aligned
    # with the GitHub release workflow.
    ssh ubuntu "cd ~/pulp && \
        cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Release \
            -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_GPU=OFF \
            -DPULP_BUILD_WEBVIEW=ON" 2>&1 | tail -5

    # Build
    ssh ubuntu "cd ~/pulp && cmake --build build-cli --target pulp-cli 2>&1 | tail -5"

    if ssh ubuntu "test -f ~/pulp/build-cli/tools/cli/pulp && echo ok" | grep -q ok; then
        ssh ubuntu "strip ~/pulp/build-cli/tools/cli/pulp"
        scp ubuntu:~/pulp/build-cli/tools/cli/pulp "$DIST_DIR/pulp"
        (cd "$DIST_DIR" && tar czf "pulp-linux-arm64.tar.gz" pulp && rm pulp)
        info "pulp-linux-arm64.tar.gz ($(du -h "$DIST_DIR/pulp-linux-arm64.tar.gz" | cut -f1))"
    else
        fail "Linux build failed — binary not found"
    fi
else
    fail "Cannot reach ubuntu VM — skipping Linux build"
fi

# ── Windows x64 (via SSH) ───────────────────────────────────────────────────

step "Building Windows x64 (ssh win2)"

if ssh -o ConnectTimeout=5 -o BatchMode=yes win2 "echo ok" &>/dev/null; then
    # Check for compiler
    if ssh win2 "where cl 2>nul" &>/dev/null; then
        # Sync source via rsync (requires rsync on Windows, e.g., via Git Bash)
        rsync -az --delete \
            --exclude='build*' --exclude='.git' --exclude='external/skia-build' \
            --exclude='dist' --exclude='planning' \
            "$REPO_ROOT/" win2:~/pulp/

        ssh win2 "cd ~/pulp && cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_WEBVIEW=ON 2>&1 | tail -5"
        ssh win2 "cd ~/pulp && cmake --build build-cli --target pulp-cli --config Release 2>&1 | tail -5"

        if ssh win2 "if exist ~/pulp/build-cli/tools/cli/Release/pulp.exe (echo ok)" | grep -q ok; then
            scp win2:~/pulp/build-cli/tools/cli/Release/pulp.exe "$DIST_DIR/pulp.exe"
            (cd "$DIST_DIR" && zip -q "pulp-windows-x64.zip" pulp.exe && rm pulp.exe)
            info "pulp-windows-x64.zip ($(du -h "$DIST_DIR/pulp-windows-x64.zip" | cut -f1))"
        else
            fail "Windows build failed — binary not found"
        fi
    else
        fail "No C++ compiler on win2 — install VS Build Tools 2022+"
        echo "    Download: https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022"
        echo "    Select workload: 'Desktop development with C++'"
    fi
else
    fail "Cannot reach win2 VM — skipping Windows build"
fi

# ── Summary ─────────────────────────────────────────────────────────────────

step "Release artifacts"

ls -lh "$DIST_DIR/"

ARTIFACT_COUNT=$(ls "$DIST_DIR/" | wc -l | tr -d ' ')
echo ""
echo "  $ARTIFACT_COUNT artifact(s) in dist/"

# ── Create GitHub release (if gh is available) ──────────────────────────────

if command -v gh &>/dev/null; then
    echo ""
    read -r -p "Create GitHub release $VERSION with these artifacts? [y/N] " response
    if [[ "$response" =~ ^[yY] ]]; then
        gh release create "$VERSION" \
            --title "Pulp CLI $VERSION" \
            --notes "## Install

**macOS / Linux:**
\`\`\`bash
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
\`\`\`

**Windows (PowerShell):**
\`\`\`powershell
irm https://www.generouscorp.com/pulp/install.ps1 | iex
\`\`\`

Or download the binary for your platform below." \
            "$DIST_DIR"/*

        info "Release $VERSION created"
        echo "  https://github.com/danielraffel/pulp/releases/tag/$VERSION"
    else
        echo "Skipped. Create manually with:"
        echo "  gh release create $VERSION dist/*"
    fi
else
    echo ""
    echo "  gh CLI not found — create release manually:"
    echo "  gh release create $VERSION dist/*"
fi
