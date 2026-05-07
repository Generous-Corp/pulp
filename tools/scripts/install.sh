#!/bin/bash
# Pulp CLI installer for macOS and Linux
# Usage: curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
#
# Environment variables:
#   PULP_VERSION    — install a specific version (default: latest)
#   PULP_INSTALL_DIR — install directory (default: ~/.pulp/bin)
#   PULP_NO_MODIFY_PATH — set to 1 to skip PATH modification

set -e

# ── Configuration ────────────────────────────────────────────────────────────

GITHUB_REPO="danielraffel/pulp"
INSTALL_DIR="${PULP_INSTALL_DIR:-$HOME/.pulp/bin}"
VERSION="${PULP_VERSION:-latest}"
NO_MODIFY_PATH="${PULP_NO_MODIFY_PATH:-0}"

# ── Helpers ──────────────────────────────────────────────────────────────────

info()  { echo "  $*"; }
error() { echo "Error: $*" >&2; exit 1; }

# ── Platform detection ───────────────────────────────────────────────────────

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Darwin)  PLATFORM="darwin" ;;
    Linux)   PLATFORM="linux" ;;
    *)       error "Unsupported OS: $OS. Use install.ps1 for Windows." ;;
esac

case "$ARCH" in
    x86_64|amd64)  ARCH="x64" ;;
    arm64|aarch64) ARCH="arm64" ;;
    *)             error "Unsupported architecture: $ARCH" ;;
esac

# ── Resolve version ─────────────────────────────────────────────────────────

if [ "$VERSION" = "latest" ]; then
    info "Fetching latest release..."
    VERSION=$(curl -fsSL "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" \
        | grep '"tag_name"' | sed 's/.*"v\(.*\)".*/\1/' 2>/dev/null) || true

    if [ -z "$VERSION" ]; then
        error "Could not determine latest version. Set PULP_VERSION explicitly."
    fi
fi

TARBALL="pulp-${PLATFORM}-${ARCH}.tar.gz"
URL="https://github.com/${GITHUB_REPO}/releases/download/v${VERSION}/${TARBALL}"

# ── Download and install ────────────────────────────────────────────────────

echo ""
echo "Installing Pulp CLI v${VERSION} for ${PLATFORM} (${ARCH})..."
echo ""

# Create install directory
mkdir -p "$INSTALL_DIR"

# Download to temp file
TMPDIR_INSTALL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_INSTALL"' EXIT

info "Downloading ${TARBALL}..."
HTTP_CODE=$(curl -fSL -w "%{http_code}" -o "${TMPDIR_INSTALL}/${TARBALL}" "$URL" 2>/dev/null) || true

if [ "$HTTP_CODE" != "200" ] && [ ! -s "${TMPDIR_INSTALL}/${TARBALL}" ]; then
    echo ""
    error "Download failed (HTTP ${HTTP_CODE}).

  The release v${VERSION} may not exist yet for ${PLATFORM}-${ARCH}.
  Check available releases: https://github.com/${GITHUB_REPO}/releases

  If you're building from source instead:
    git clone https://github.com/${GITHUB_REPO}.git
    cd pulp && ./setup.sh"
fi

# Extract
info "Extracting..."
tar -xzf "${TMPDIR_INSTALL}/${TARBALL}" -C "$INSTALL_DIR"

# Verify
if [ ! -x "${INSTALL_DIR}/pulp" ]; then
    error "Installation failed — ${INSTALL_DIR}/pulp not found or not executable."
fi

INSTALLED_VERSION=$("${INSTALL_DIR}/pulp" version 2>/dev/null | head -n 1 || echo "unknown")
if [ -z "$INSTALLED_VERSION" ]; then
    INSTALLED_VERSION="unknown"
fi
info "Installed: ${INSTALLED_VERSION}"

if [ -x "${INSTALL_DIR}/pulp-cpp" ]; then
    info "Installed: pulp-cpp delegate"
else
    info "No pulp-cpp delegate found; this is expected only for pre-0.78.0 releases."
fi

# ── Add to PATH ─────────────────────────────────────────────────────────────

add_to_path() {
    local shell_profile="$1"
    local export_line="export PATH=\"${INSTALL_DIR}:\$PATH\""

    if [ -f "$shell_profile" ] && grep -qF "$INSTALL_DIR" "$shell_profile" 2>/dev/null; then
        return 0  # Already in profile
    fi

    echo "" >> "$shell_profile"
    echo "# Pulp CLI" >> "$shell_profile"
    echo "$export_line" >> "$shell_profile"
    info "Added ${INSTALL_DIR} to PATH in ${shell_profile}"
}

if [ "$NO_MODIFY_PATH" != "1" ]; then
    case "$PATH" in
        *"$INSTALL_DIR"*) ;;  # Already in PATH
        *)
            # Detect shell and update profile
            CURRENT_SHELL="$(basename "${SHELL:-/bin/sh}")"
            case "$CURRENT_SHELL" in
                zsh)  add_to_path "$HOME/.zshrc" ;;
                bash)
                    if [ -f "$HOME/.bash_profile" ]; then
                        add_to_path "$HOME/.bash_profile"
                    else
                        add_to_path "$HOME/.bashrc"
                    fi
                    ;;
                fish)
                    mkdir -p "$HOME/.config/fish"
                    FISH_CONFIG="$HOME/.config/fish/config.fish"
                    if ! grep -qF "$INSTALL_DIR" "$FISH_CONFIG" 2>/dev/null; then
                        echo "" >> "$FISH_CONFIG"
                        echo "# Pulp CLI" >> "$FISH_CONFIG"
                        echo "set -gx PATH ${INSTALL_DIR} \$PATH" >> "$FISH_CONFIG"
                        info "Added ${INSTALL_DIR} to PATH in ${FISH_CONFIG}"
                    fi
                    ;;
                *)  add_to_path "$HOME/.profile" ;;
            esac
            ;;
    esac
fi

# ── Done ────────────────────────────────────────────────────────────────────

echo ""
echo "  Done! Run \`pulp create my-plugin\` to create your first plugin."
echo ""
echo "  If 'pulp' is not found, restart your shell or run:"
echo "    export PATH=\"${INSTALL_DIR}:\$PATH\""
echo ""
