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

# #2067: pulp-mcp is the Claude Code plugin's MCP server. The plugin's
# .mcp.json invokes tools/mcp/pulp-mcp-launcher which resolves pulp-mcp
# from PATH — so once ${INSTALL_DIR} is on PATH (added below), the
# plugin "just works". Pre-#2067 releases do not ship pulp-mcp; in that
# case the plugin's /mcp panel will report "cannot locate pulp-mcp
# binary" until the user upgrades the CLI.
if [ -x "${INSTALL_DIR}/pulp-mcp" ]; then
    info "Installed: pulp-mcp (Claude Code plugin MCP server)"
fi

# #2087: Pulp users running `curl install.sh | sh` historically ended
# up with a fresh CLI but no SDK. Any project then built against
# whatever stale ~/.pulp/sdk/<old>/ they had — sometimes months behind
# — silently missing every framework fix in between (Spectr was the
# visible victim). Auto-install the latest SDK matching the CLI's own
# version so install.sh leaves the user with a coherent CLI+SDK pair.
#
# Failure mode: SDK install can fail on a transient network error.
# Don't fail the whole install — the CLI is still usable; the user can
# retry with `pulp sdk install` later. Print a clear nudge.
SDK_VERSION_FROM_CLI=$("${INSTALL_DIR}/pulp" version 2>/dev/null \
    | awk '/Pulp SDK version:/ {print $4}' \
    | head -n 1)
if [ -z "$SDK_VERSION_FROM_CLI" ]; then
    SDK_VERSION_FROM_CLI=$(echo "$INSTALLED_VERSION" | awk '{print $NF}' \
        | sed -E 's/^v//')
fi

echo ""
info "Installing matching SDK v${SDK_VERSION_FROM_CLI}..."
if PATH="${INSTALL_DIR}:$PATH" "${INSTALL_DIR}/pulp" sdk install 2>&1 \
       | sed 's/^/  /'; then
    info "Installed SDK at ~/.pulp/sdk/${SDK_VERSION_FROM_CLI}/"
else
    echo ""
    echo "  (warning) SDK install failed — the CLI is still usable, but"
    echo "  building a project will need the SDK. Retry with:"
    echo "    pulp sdk install"
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
echo "  Done! Installed pulp ${INSTALLED_VERSION} CLI + SDK v${SDK_VERSION_FROM_CLI}"
echo "  at ${INSTALL_DIR}/ and ~/.pulp/sdk/${SDK_VERSION_FROM_CLI}/."
echo ""
echo "  Next: run \`pulp create my-plugin\` to scaffold your first plugin."
echo ""
echo "  Tip: new projects default to floating-SDK mode (track latest) so"
echo "       you automatically pick up framework fixes on every rebuild."
echo "       Pin a specific SDK version for a project with:"
echo "         cd my-plugin && pulp project pin <version>"
echo "       Pinned projects don't auto-update. Run \`pulp project unpin\`"
echo "       to switch back to floating mode."
echo ""
echo "  If 'pulp' is not found, restart your shell or run:"
echo "    export PATH=\"${INSTALL_DIR}:\$PATH\""
echo ""
