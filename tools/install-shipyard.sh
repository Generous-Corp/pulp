#!/usr/bin/env bash
# install-shipyard.sh — download and verify the pinned Shipyard release
# declared in tools/shipyard.toml.
#
# Installs to ~/.pulp/shipyard/<version>/shipyard and symlinks
# ~/.pulp/bin/shipyard → that file. Add ~/.pulp/bin to PATH once and
# every Pulp checkout uses the pinned version automatically.
#
# Usage:
#   ./tools/install-shipyard.sh           # install pinned version
#   ./tools/install-shipyard.sh --status  # show installed vs pinned
#   ./tools/install-shipyard.sh --force   # reinstall even if present
#
# Exit codes:
#   0   success (or already installed and matching pin)
#   1   user error (bad flag, missing tools)
#   2   download / verification failure

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PULP_ROOT="$(dirname "$SCRIPT_DIR")"
PIN_FILE="$SCRIPT_DIR/shipyard.toml"

# ── Argument parsing ────────────────────────────────────────────────────────

MODE=install
for arg in "$@"; do
    case "$arg" in
        --status)
            MODE=status
            ;;
        --force)
            MODE=force
            ;;
        -h|--help)
            sed -n '2,18p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Error: unknown argument '$arg'" >&2
            exit 1
            ;;
    esac
done

# ── Read the pinned version from tools/shipyard.toml ────────────────────────
# Tiny TOML reader: extract `key = "value"` pairs from the [shipyard]
# section without depending on a Python TOML library. Falls back to
# python3 if grep+sed cannot do it.

if ! [ -f "$PIN_FILE" ]; then
    echo "Error: pin file not found at $PIN_FILE" >&2
    exit 1
fi

read_toml_value() {
    local key="$1"
    sed -n "/^\\[shipyard\\]/,/^\\[/p" "$PIN_FILE" \
        | sed -n "s/^${key}[[:space:]]*=[[:space:]]*\"\\(.*\\)\"$/\\1/p" \
        | head -1
}

read_toml_asset() {
    local platform="$1"
    sed -n "/^\\[shipyard.assets\\]/,/^\\[/p" "$PIN_FILE" \
        | sed -n "s/^${platform}[[:space:]]*=[[:space:]]*\"\\(.*\\)\"$/\\1/p" \
        | head -1
}

VERSION="$(read_toml_value version)"
REPO="$(read_toml_value repo)"
CHECKSUMS_ASSET="$(read_toml_value checksums_asset)"

if [ -z "$VERSION" ] || [ -z "$REPO" ] || [ -z "$CHECKSUMS_ASSET" ]; then
    echo "Error: could not parse $PIN_FILE — missing version/repo/checksums_asset" >&2
    exit 1
fi

# ── Detect host platform ────────────────────────────────────────────────────

OS="$(uname -s)"
ARCH="$(uname -m)"
case "$OS" in
    Darwin)
        case "$ARCH" in
            arm64|aarch64) PLATFORM=darwin-arm64 ;;
            x86_64)        PLATFORM=darwin-x64 ;;
            *) echo "Error: unsupported macOS arch $ARCH" >&2; exit 1 ;;
        esac
        ;;
    Linux)
        case "$ARCH" in
            aarch64|arm64) PLATFORM=linux-arm64 ;;
            x86_64)        PLATFORM=linux-x64 ;;
            *) echo "Error: unsupported Linux arch $ARCH" >&2; exit 1 ;;
        esac
        ;;
    MINGW*|MSYS*|CYGWIN*)
        PLATFORM=windows-x64
        ;;
    *)
        echo "Error: unsupported OS $OS" >&2
        exit 1
        ;;
esac

ASSET_NAME="$(read_toml_asset "$PLATFORM")"
if [ -z "$ASSET_NAME" ]; then
    echo "Error: no asset declared in tools/shipyard.toml for platform $PLATFORM" >&2
    exit 1
fi

# ── Paths ───────────────────────────────────────────────────────────────────

PULP_HOME="${PULP_HOME:-$HOME/.pulp}"
SHIPYARD_DIR="$PULP_HOME/shipyard/$VERSION"
SHIPYARD_BIN_DIR="$PULP_HOME/bin"
INSTALL_BINARY_NAME="shipyard"
case "$PLATFORM" in
    windows-*) INSTALL_BINARY_NAME="shipyard.exe" ;;
esac
INSTALLED="$SHIPYARD_DIR/$INSTALL_BINARY_NAME"
SYMLINK="$SHIPYARD_BIN_DIR/$INSTALL_BINARY_NAME"

# ── Status mode: report and exit ────────────────────────────────────────────

if [ "$MODE" = "status" ]; then
    echo "Pinned (tools/shipyard.toml): $VERSION ($ASSET_NAME)"
    if [ -x "$INSTALLED" ]; then
        echo "Installed:                    $VERSION at $INSTALLED"
    else
        echo "Installed:                    (not installed — run ./tools/install-shipyard.sh)"
    fi
    if command -v shipyard >/dev/null 2>&1; then
        echo "shipyard on PATH:             $(command -v shipyard)"
        if shipyard --version 2>/dev/null; then :; fi
    else
        echo "shipyard on PATH:             (not on PATH — add $SHIPYARD_BIN_DIR to PATH)"
    fi
    exit 0
fi

# ── Skip if already installed and we're not forcing ─────────────────────────

if [ "$MODE" = "install" ] && [ -x "$INSTALLED" ] && [ -L "$SYMLINK" ]; then
    if [ "$(readlink "$SYMLINK")" = "$INSTALLED" ]; then
        echo "Shipyard $VERSION already installed at $INSTALLED"
        echo "Run with --force to reinstall."
        exit 0
    fi
fi

# ── Download + verify + install ─────────────────────────────────────────────

mkdir -p "$SHIPYARD_DIR" "$SHIPYARD_BIN_DIR"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

BASE_URL="https://github.com/${REPO}/releases/download/${VERSION}"

echo "→ Downloading $ASSET_NAME from $BASE_URL/"
curl -fsSL --retry 3 -o "$TMPDIR/$ASSET_NAME" "$BASE_URL/$ASSET_NAME"

echo "→ Downloading $CHECKSUMS_ASSET"
curl -fsSL --retry 3 -o "$TMPDIR/$CHECKSUMS_ASSET" "$BASE_URL/$CHECKSUMS_ASSET"

echo "→ Verifying SHA-256"
EXPECTED="$(grep -E "[[:space:]]\\*?${ASSET_NAME}\$" "$TMPDIR/$CHECKSUMS_ASSET" | awk '{print $1}' | head -1)"
if [ -z "$EXPECTED" ]; then
    echo "Error: $ASSET_NAME not listed in $CHECKSUMS_ASSET" >&2
    exit 2
fi

if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL="$(sha256sum "$TMPDIR/$ASSET_NAME" | awk '{print $1}')"
else
    ACTUAL="$(shasum -a 256 "$TMPDIR/$ASSET_NAME" | awk '{print $1}')"
fi

if [ "$ACTUAL" != "$EXPECTED" ]; then
    echo "Error: SHA-256 mismatch for $ASSET_NAME" >&2
    echo "  expected: $EXPECTED" >&2
    echo "  actual:   $ACTUAL" >&2
    exit 2
fi

echo "→ Installing to $INSTALLED"
mv "$TMPDIR/$ASSET_NAME" "$INSTALLED"
chmod +x "$INSTALLED"

ln -sf "$INSTALLED" "$SYMLINK"
echo "→ Symlinked $SYMLINK → $INSTALLED"

# ── Queue-file truncation recovery (#528) ───────────────────────────────────
# A crash between open(O_TRUNC) and the subsequent write() in Shipyard can
# leave the machine-global job queue at zero bytes. Any subsequent Shipyard
# invocation then dies with JSONDecodeError, which blocks autonomous
# ship cycles. Defensively re-initialize the file when we see it truncated.
#
# Platform layout matches shipyard.core.config._default_state_dir():
#   macOS   → ~/Library/Application Support/shipyard/queue/queue.json
#   Windows → ~/AppData/Local/shipyard/queue/queue.json
#   Linux   → ~/.local/state/shipyard/queue/queue.json

case "$PLATFORM" in
    darwin-*)  SHIPYARD_STATE_DIR="$HOME/Library/Application Support/shipyard" ;;
    windows-*) SHIPYARD_STATE_DIR="$HOME/AppData/Local/shipyard" ;;
    linux-*)   SHIPYARD_STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/shipyard" ;;
    *)         SHIPYARD_STATE_DIR="" ;;
esac

if [ -n "$SHIPYARD_STATE_DIR" ]; then
    SHIPYARD_QUEUE_FILE="$SHIPYARD_STATE_DIR/queue/queue.json"
    if [ -f "$SHIPYARD_QUEUE_FILE" ] && [ ! -s "$SHIPYARD_QUEUE_FILE" ]; then
        echo "→ Shipyard queue file is empty — reinitializing (#528)"
        echo '{"jobs": []}' > "$SHIPYARD_QUEUE_FILE"
    fi
fi

# ── Final report ────────────────────────────────────────────────────────────

echo ""
echo "✓ Shipyard $VERSION installed."
echo ""
case ":$PATH:" in
    *":$SHIPYARD_BIN_DIR:"*)
        echo "$SHIPYARD_BIN_DIR is already on PATH — you can run \`shipyard\` now."
        ;;
    *)
        echo "Add $SHIPYARD_BIN_DIR to your PATH to use \`shipyard\` from anywhere:"
        echo ""
        echo "    export PATH=\"$SHIPYARD_BIN_DIR:\$PATH\""
        ;;
esac
