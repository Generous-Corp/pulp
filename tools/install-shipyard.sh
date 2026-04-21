#!/usr/bin/env bash
# install-shipyard.sh — install the pinned Shipyard release declared
# in tools/shipyard.toml.
#
# Delegates to Shipyard's official installer (install.sh) via
# SHIPYARD_VERSION, which lands the binary at ~/.local/bin/shipyard.
# This is a thin wrapper: Pulp owns the version pin; Shipyard owns
# the download, verification, and install mechanics. Keeping the two
# responsibilities split means we pick up upstream installer fixes
# (e.g. the v0.22.x install.sh family) without re-implementing them
# here.
#
# Usage:
#   ./tools/install-shipyard.sh           # install pinned version
#   ./tools/install-shipyard.sh --status  # show installed vs pinned
#
# Exit codes:
#   0   success (or already installed and matching pin)
#   1   user error (bad flag, missing tools)
#   2   download / verification failure (propagated from installer)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIN_FILE="$SCRIPT_DIR/shipyard.toml"
UPSTREAM_INSTALLER="https://raw.githubusercontent.com/danielraffel/Shipyard/main/install.sh"

# ── Argument parsing ────────────────────────────────────────────────────────

MODE=install
for arg in "$@"; do
    case "$arg" in
        --status)  MODE=status ;;
        -h|--help)
            sed -n '2,22p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Error: unknown argument '$arg'" >&2
            exit 1
            ;;
    esac
done

# ── Read the pinned version from tools/shipyard.toml ────────────────────────
# The pin file is the single source of truth for the Shipyard version
# Pulp uses; bumping it requires a PR per tools/shipyard.toml's header.

if ! [ -f "$PIN_FILE" ]; then
    echo "Error: pin file not found at $PIN_FILE" >&2
    exit 1
fi

VERSION="$(sed -n '/^\[shipyard\]/,/^\[/p' "$PIN_FILE" \
    | sed -n 's/^version[[:space:]]*=[[:space:]]*"\(.*\)"$/\1/p' \
    | head -1)"

if [ -z "$VERSION" ]; then
    echo "Error: could not parse version from $PIN_FILE" >&2
    exit 1
fi

# ── Status mode: report and exit ────────────────────────────────────────────

if [ "$MODE" = "status" ]; then
    echo "Pinned (tools/shipyard.toml): $VERSION"
    if command -v shipyard >/dev/null 2>&1; then
        echo "shipyard on PATH:             $(command -v shipyard)"
        if installed="$(shipyard --version 2>/dev/null)"; then
            echo "Installed version:            $installed"
        fi
    else
        echo "shipyard on PATH:             (not found — run ./tools/install-shipyard.sh)"
    fi
    exit 0
fi

# ── Legacy cleanup ──────────────────────────────────────────────────────────
# Earlier versions of this script installed to ~/.pulp/shipyard/<v>/
# with a symlink at ~/.pulp/bin/shipyard. When that bin dir comes
# first on PATH, the stale symlink shadows the canonical
# ~/.local/bin/shipyard and users silently run an old binary.
# Remove the stale artifacts so the upstream installer's PATH entry
# (~/.local/bin) takes over cleanly.

if [ -L "$HOME/.pulp/bin/shipyard" ]; then
    echo "→ Removing legacy symlink $HOME/.pulp/bin/shipyard"
    rm -f "$HOME/.pulp/bin/shipyard"
fi
if [ -d "$HOME/.pulp/shipyard" ]; then
    echo "→ Removing legacy install tree $HOME/.pulp/shipyard"
    rm -rf "$HOME/.pulp/shipyard"
fi

# ── Delegate to upstream installer ──────────────────────────────────────────

echo "→ Installing Shipyard $VERSION via upstream install.sh"
echo "    source: $UPSTREAM_INSTALLER"

SHIPYARD_VERSION="$VERSION" bash <(curl -fsSL "$UPSTREAM_INSTALLER")

# ── Final report ────────────────────────────────────────────────────────────

echo ""
if command -v shipyard >/dev/null 2>&1; then
    echo "✓ Shipyard $VERSION installed at $(command -v shipyard)."
else
    echo "Shipyard installed to ~/.local/bin/shipyard."
    case ":$PATH:" in
        *":$HOME/.local/bin:"*) ;;
        *)
            echo ""
            echo "Add ~/.local/bin to your PATH to use shipyard from anywhere:"
            echo ""
            echo "    export PATH=\"\$HOME/.local/bin:\$PATH\""
            ;;
    esac
fi
