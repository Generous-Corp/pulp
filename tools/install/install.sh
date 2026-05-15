#!/bin/bash
# Pulp CLI installer — one-liner install for macOS and Linux
#
# Usage:
#   curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
#
# Or with options:
#   curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh -s -- --version 0.1.0
#
# Environment variables:
#   PULP_INSTALL_DIR  — install directory (default: ~/.pulp/bin)
#   PULP_VERSION      — version to install (default: latest)
#   PULP_NO_MODIFY_PATH — set to 1 to skip PATH modification
#
# Scope:
#   Installs Pulp CLI artifacts only. It does not install Shipyard or the
#   GitHub CLI (`gh`); those are optional source-checkout contributor tools.

set -e

# ── Configuration ────────────────────────────────────────────────────────────

REPO="danielraffel/pulp"
INSTALL_DIR="${PULP_INSTALL_DIR:-$HOME/.pulp/bin}"
VERSION="${PULP_VERSION:-latest}"
NO_MODIFY_PATH="${PULP_NO_MODIFY_PATH:-0}"

for arg in "$@"; do
    case "$arg" in
        --version=*) VERSION="${arg#*=}" ;;
        --version)   shift; VERSION="$1" ;;
        --dir=*)     INSTALL_DIR="${arg#*=}" ;;
        --no-modify-path) NO_MODIFY_PATH=1 ;;
        --help|-h)
            echo "Pulp CLI Installer"
            echo ""
            echo "Usage: curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh"
            echo ""
            echo "Options:"
            echo "  --version <ver>    Install specific version (default: latest)"
            echo "  --dir <path>       Install directory (default: ~/.pulp/bin)"
            echo "  --no-modify-path   Don't add to PATH"
            echo ""
            echo "Environment variables:"
            echo "  PULP_INSTALL_DIR   Install directory"
            echo "  PULP_VERSION       Version to install"
            echo ""
            echo "Scope:"
            echo "  Installs Pulp CLI artifacts only."
            echo "  Does not install Shipyard or GitHub CLI (gh)."
            exit 0
            ;;
    esac
done

# ── Platform detection ───────────────────────────────────────────────────────

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Darwin)
        case "$ARCH" in
            arm64|aarch64) PLATFORM="darwin-arm64" ;;
            x86_64)
                echo "Warning: Intel Mac (x86_64) is not officially supported."
                echo "Pre-built binaries are for Apple Silicon (ARM64) only."
                echo "You can build from source instead: git clone && ./setup.sh"
                PLATFORM="darwin-arm64"  # try ARM64 via Rosetta
                ;;
            *)             echo "Error: unsupported architecture: $ARCH"; exit 1 ;;
        esac
        ;;
    Linux)
        case "$ARCH" in
            x86_64|amd64)  PLATFORM="linux-x64" ;;
            aarch64|arm64) PLATFORM="linux-arm64" ;;
            *)             echo "Error: unsupported architecture: $ARCH"; exit 1 ;;
        esac
        ;;
    *)
        echo "Error: unsupported OS: $OS"
        echo "For Windows, use: irm https://www.generouscorp.com/pulp/install.ps1 | iex"
        exit 1
        ;;
esac

echo "Installing Pulp CLI for $PLATFORM..."

# ── Download ─────────────────────────────────────────────────────────────────

if [ "$VERSION" = "latest" ]; then
    RELEASE_URL="https://api.github.com/repos/$REPO/releases/latest"
    echo "Fetching latest release..."
    DOWNLOAD_URL=$(curl -fsSL "$RELEASE_URL" | grep "browser_download_url.*pulp-$PLATFORM" | head -1 | cut -d '"' -f 4)
else
    DOWNLOAD_URL="https://github.com/$REPO/releases/download/v$VERSION/pulp-$PLATFORM.tar.gz"
fi

if [ -z "$DOWNLOAD_URL" ]; then
    echo "Error: could not find release for $PLATFORM"
    echo ""
    echo "Pre-built binaries may not be available yet."
    echo "To build from source instead:"
    echo "  git clone https://github.com/$REPO.git && cd pulp && ./setup.sh"
    exit 1
fi

# Create temp directory
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

echo "Downloading $DOWNLOAD_URL..."
curl -fsSL "$DOWNLOAD_URL" -o "$TMP_DIR/pulp.tar.gz"

# ── Install ──────────────────────────────────────────────────────────────────

mkdir -p "$INSTALL_DIR"

echo "Extracting to $INSTALL_DIR..."
tar xzf "$TMP_DIR/pulp.tar.gz" -C "$INSTALL_DIR"
chmod +x "$INSTALL_DIR/pulp"

# Verify
INSTALLED_VERSION=$("$INSTALL_DIR/pulp" --version 2>/dev/null || echo "unknown")
echo "Installed: pulp $INSTALLED_VERSION"

# ── SDK ──────────────────────────────────────────────────────────────────────
#
# pulp #2087 — install.sh used to install only the CLI. Users would then
# `pulp build` against a non-existent (or ancient) SDK, get cryptic errors,
# and never realize the CLI/SDK distinction. Install the matching SDK now
# so the next `pulp build` Just Works.
#
# Skippable for power users who want to manage SDKs manually
# (PULP_SKIP_SDK_INSTALL=1).
if [ "${PULP_SKIP_SDK_INSTALL:-0}" != "1" ]; then
    echo ""
    echo "Installing matching SDK..."
    if "$INSTALL_DIR/pulp" sdk install >/tmp/pulp-install-sdk.log 2>&1; then
        SDK_INSTALLED=$("$INSTALL_DIR/pulp" sdk status 2>/dev/null | grep -E '\(downloaded\)' | head -1 | awk '{print $1}')
        echo "  ✓ SDK ${SDK_INSTALLED:-installed} alongside CLI"
    else
        echo "  ⚠ SDK install failed (CLI is fine; run \`pulp sdk install\` manually)"
        tail -5 /tmp/pulp-install-sdk.log 2>&1 | sed 's/^/    /'
    fi
fi

# ── PATH ─────────────────────────────────────────────────────────────────────

if [ "$NO_MODIFY_PATH" = "1" ]; then
    echo ""
    echo "Skipping PATH modification. Add manually:"
    echo "  export PATH=\"$INSTALL_DIR:\$PATH\""
else
    # Check if already in PATH
    case ":$PATH:" in
        *":$INSTALL_DIR:"*) ;; # already in PATH
        *)
            # Detect shell and profile
            SHELL_NAME=$(basename "$SHELL")
            case "$SHELL_NAME" in
                zsh)  PROFILE="$HOME/.zshrc" ;;
                bash)
                    if [ -f "$HOME/.bash_profile" ]; then PROFILE="$HOME/.bash_profile"
                    else PROFILE="$HOME/.bashrc"; fi
                    ;;
                fish) PROFILE="$HOME/.config/fish/config.fish" ;;
                *)    PROFILE="$HOME/.profile" ;;
            esac

            EXPORT_LINE="export PATH=\"$INSTALL_DIR:\$PATH\""
            if [ "$SHELL_NAME" = "fish" ]; then
                EXPORT_LINE="set -gx PATH $INSTALL_DIR \$PATH"
            fi

            if [ -f "$PROFILE" ] && grep -qF "$INSTALL_DIR" "$PROFILE" 2>/dev/null; then
                : # Already in profile
            else
                echo "" >> "$PROFILE"
                echo "# Pulp CLI" >> "$PROFILE"
                echo "$EXPORT_LINE" >> "$PROFILE"
                echo "Added $INSTALL_DIR to PATH in $PROFILE"
            fi
            ;;
    esac
fi

# ── Done ─────────────────────────────────────────────────────────────────────

echo ""
echo "Pulp installed successfully!"
echo "This installed the Pulp CLI + matching SDK."
echo "(Shipyard and GitHub CLI are NOT installed by this script.)"
echo ""
echo "Get started:"
echo "  pulp create MyPlugin     # create your first plugin"
echo "  pulp doctor              # check your environment"
echo "  pulp sdk status          # see installed SDK versions"
echo ""
echo "Stay current (or pin a project):"
echo "  pulp upgrade             # refresh CLI to the latest release"
echo "  pulp sdk install         # add the latest SDK (if newer than installed)"
echo "  pulp project bump <ver>  # pin THIS project to a specific SDK"
echo ""
echo "Or clone the framework:"
echo "  git clone https://github.com/$REPO.git"
echo "  cd pulp && ./setup.sh"
echo "  ./tools/install-shipyard.sh   # optional: source-checkout PR/CI tooling"
echo ""
if [ "$NO_MODIFY_PATH" != "1" ]; then
    echo "Restart your shell or run: source $PROFILE"
fi
