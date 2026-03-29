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
            x86_64)        PLATFORM="darwin-x64" ;;
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
echo "Pulp CLI installed successfully!"
echo ""
echo "Get started:"
echo "  pulp create MyPlugin     # create your first plugin"
echo "  pulp doctor              # check your environment"
echo ""
echo "Or clone the framework:"
echo "  git clone https://github.com/$REPO.git"
echo "  cd pulp && ./setup.sh"
echo ""
if [ "$NO_MODIFY_PATH" != "1" ]; then
    echo "Restart your shell or run: source $PROFILE"
fi
