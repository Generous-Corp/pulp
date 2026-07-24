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
#   PULP_NO_MODIFY_PATH — set to 1 to skip shell-profile modification
#
# Scope:
#   Installs Pulp CLI artifacts only. It does not install Shipyard or the
#   GitHub CLI (`gh`); those are optional source-checkout contributor tools.

set -e

# ── Configuration ────────────────────────────────────────────────────────────

REPO="Generous-Corp/pulp"
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
            echo "  --no-modify-path   Don't modify PATH or PULP_MCP_BINARY"
            echo ""
            echo "Environment variables:"
            echo "  PULP_INSTALL_DIR   Install directory"
            echo "  PULP_VERSION       Version to install"
            echo "  PULP_NO_MODIFY_PATH"
            echo "                       Don't modify the shell profile"
            echo ""
            echo "Scope:"
            echo "  Installs Pulp CLI artifacts only."
            echo "  Does not install Shipyard or GitHub CLI (gh)."
            exit 0
            ;;
    esac
done

# Persisted paths must not depend on the directory from which the installer ran.
case "$INSTALL_DIR" in
    /*) ;;
    *) INSTALL_DIR="$(pwd)/$INSTALL_DIR" ;;
esac

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

# Testability / debugging hook: print the resolved platform and exit
# without touching the network. Exercised by
# test/test_install_platform_detect.sh with a mocked `uname`.
if [ "${PULP_PRINT_PLATFORM:-0}" = "1" ]; then
    echo "$PLATFORM"
    exit 0
fi

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
if ! curl -fsSL "$DOWNLOAD_URL" -o "$TMP_DIR/pulp.tar.gz"; then
    echo ""
    echo "Error: could not download pulp-$PLATFORM for this version."
    case "$PLATFORM" in
        darwin-x64)
            echo "The Intel (x86_64) macOS build ships in current releases, but"
            echo "older releases predate Intel support and won't have it. Try the"
            echo "latest release (omit --version), or build from source:"
            ;;
        *)
            echo "Pre-built binaries may not be available for this version."
            echo "To build from source instead:"
            ;;
    esac
    echo "  git clone https://github.com/$REPO.git && cd pulp && ./setup.sh"
    exit 1
fi

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

# ── Shell environment ────────────────────────────────────────────────────────

quote_sh_profile_value() {
    QUOTED_VALUE=$(printf '%s' "$1" | sed "s/'/'\\\\''/g")
    printf "'%s'" "$QUOTED_VALUE"
}

quote_fish_profile_value() {
    QUOTED_VALUE=$(printf '%s' "$1" |
        sed -e 's/\\/\\\\/g' -e "s/'/\\\\'/g")
    printf "'%s'" "$QUOTED_VALUE"
}

if [ "$NO_MODIFY_PATH" = "1" ]; then
    echo ""
    echo "Skipping shell-profile modification. Add manually:"
    PATH_VALUE=$(quote_sh_profile_value "$INSTALL_DIR")
    printf '  export PATH=%s:$PATH\n' "$PATH_VALUE"
    if [ -x "$INSTALL_DIR/pulp-mcp" ]; then
        MCP_VALUE=$(quote_sh_profile_value "$INSTALL_DIR/pulp-mcp")
        printf '  export PULP_MCP_BINARY=%s\n' "$MCP_VALUE"
    fi
else
    SHELL_NAME=$(basename "${SHELL:-/bin/sh}")
    case "$SHELL_NAME" in
        zsh)  PROFILE="$HOME/.zshrc" ;;
        bash)
            if [ -f "$HOME/.bash_profile" ]; then PROFILE="$HOME/.bash_profile"
            else PROFILE="$HOME/.bashrc"; fi
            ;;
        fish)
            PROFILE="$HOME/.config/fish/config.fish"
            mkdir -p "$HOME/.config/fish"
            ;;
        *)    PROFILE="$HOME/.profile" ;;
    esac

    PATH_VALUE=$(quote_sh_profile_value "$INSTALL_DIR")
    MCP_VALUE=$(quote_sh_profile_value "$INSTALL_DIR/pulp-mcp")
    PATH_LINE="export PATH=${PATH_VALUE}:\$PATH"
    MCP_LINE="export PULP_MCP_BINARY=${MCP_VALUE}"
    if [ "$SHELL_NAME" = "fish" ]; then
        PATH_VALUE=$(quote_fish_profile_value "$INSTALL_DIR")
        MCP_VALUE=$(quote_fish_profile_value "$INSTALL_DIR/pulp-mcp")
        PATH_LINE="set -gx PATH ${PATH_VALUE} \$PATH"
        MCP_LINE="set -gx PULP_MCP_BINARY ${MCP_VALUE}"
    elif [ "$SHELL_NAME" = "bash" ]; then
        # Bash may read both files when .bash_profile sources .bashrc. Keep the
        # installer-owned fallback from overriding an assignment in either
        # profile, regardless of their source order.
        MCP_LINE="if [ -z \"\${PULP_MCP_BINARY+x}\" ]; then export PULP_MCP_BINARY=${MCP_VALUE}; fi"
    fi

    profile_has_install_path() {
        PULP_PROFILE_INSTALL_DIR="$INSTALL_DIR" awk '
            /^[[:space:]]*#/ { next }
            {
                statement = $0
                sub(/^[[:space:]]+/, "", statement)
                if (statement ~ /^export[[:space:]]+/) {
                    sub(/^export[[:space:]]+/, "", statement)
                }
                if (statement ~ /^PATH[[:space:]]*=/ &&
                    index(statement, ENVIRON["PULP_PROFILE_INSTALL_DIR"])) {
                    found = 1
                }
                token_count = split(statement, tokens, /[[:space:]]+/)
                if (tokens[1] == "set") {
                    for (i = 2; i <= token_count; ++i) {
                        if (substr(tokens[i], 1, 1) != "-") {
                            if (tokens[i] == "PATH" &&
                                index(statement,
                                      ENVIRON["PULP_PROFILE_INSTALL_DIR"])) {
                                found = 1
                            }
                            break
                        }
                    }
                }
            }
            END { exit(found ? 0 : 1) }
        ' "$PROFILE" 2>/dev/null
    }

    if ! profile_has_install_path; then
        echo "" >> "$PROFILE"
        echo "# Pulp CLI" >> "$PROFILE"
        printf '%s\n' "$PATH_LINE" >> "$PROFILE"
        echo "Added $INSTALL_DIR to PATH in $PROFILE"
    fi
    ensure_mcp_profile() {
        MCP_PROFILE="$1"
        MCP_START="# >>> Pulp MCP (managed by installer) >>>"
        MCP_END="# <<< Pulp MCP (managed by installer) <<<"
        if [ -f "$MCP_PROFILE" ]; then
            MCP_MARKER_SHAPE=$(awk -v start="$MCP_START" -v end="$MCP_END" '
                $0 == start { starts += 1; start_line = NR }
                $0 == end { ends += 1; end_line = NR }
                END {
                    if (starts == 0 && ends == 0) print "none"
                    else if (starts == 1 && ends == 1 && start_line < end_line) print "valid"
                    else print "malformed"
                }
            ' "$MCP_PROFILE")
        else
            MCP_MARKER_SHAPE="none"
        fi
        if [ "$MCP_MARKER_SHAPE" = "valid" ]; then
                # Preserve symlinked profiles used by dotfile managers: update
                # the target atomically instead of replacing the link itself.
                MCP_TARGET="$MCP_PROFILE"
                if [ -L "$MCP_PROFILE" ]; then
                    MCP_LINK_TARGET=$(readlink "$MCP_PROFILE")
                    case "$MCP_LINK_TARGET" in
                        /*) MCP_TARGET="$MCP_LINK_TARGET" ;;
                        *) MCP_TARGET="$(dirname "$MCP_PROFILE")/$MCP_LINK_TARGET" ;;
                    esac
                fi
                MCP_TMP="${MCP_TARGET}.pulp-mcp.tmp.$$"
                MCP_RENDER="${MCP_TMP}.render"
                cp -p "$MCP_TARGET" "$MCP_TMP"
                PULP_MCP_RENDER_LINE="$MCP_LINE" \
                awk -v start="$MCP_START" -v end="$MCP_END" '
                    $0 == start {
                        print start
                        print ENVIRON["PULP_MCP_RENDER_LINE"]
                        managed = 1
                        next
                    }
                    managed && $0 == end { print end; managed = 0; next }
                    !managed { print }
                ' "$MCP_TARGET" > "$MCP_RENDER"
                cat "$MCP_RENDER" > "$MCP_TMP"
                rm "$MCP_RENDER"
                mv "$MCP_TMP" "$MCP_TARGET"
                echo "Updated PULP_MCP_BINARY in $MCP_PROFILE"
        elif [ "$MCP_MARKER_SHAPE" = "malformed" ]; then
            echo "Preserved malformed Pulp MCP block in $MCP_PROFILE; repair it manually"
        elif awk '
            /^[[:space:]]*#/ { next }
            {
                statement = $0
                sub(/^[[:space:]]+/, "", statement)
                if (statement ~ /^export[[:space:]]+/) {
                    sub(/^export[[:space:]]+/, "", statement)
                }
                if (statement ~ /^PULP_MCP_BINARY[[:space:]]*=/) {
                    found = 1
                }
                token_count = split(statement, tokens, /[[:space:]]+/)
                if (tokens[1] == "set") {
                    inactive = 0
                    for (i = 2; i <= token_count; ++i) {
                        if (tokens[i] == "-e" || tokens[i] == "--erase" ||
                            tokens[i] == "-q" || tokens[i] == "--query") {
                            inactive = 1
                        } else if (substr(tokens[i], 1, 1) != "-") {
                            if (tokens[i] == "PULP_MCP_BINARY" && !inactive) {
                                found = 1
                            }
                            break
                        }
                    }
                }
            }
            END { exit(found ? 0 : 1) }
        ' "$MCP_PROFILE" 2>/dev/null; then
            echo "Preserved user-managed PULP_MCP_BINARY in $MCP_PROFILE"
        else
            echo "" >> "$MCP_PROFILE"
            printf '%s\n' "$MCP_START" >> "$MCP_PROFILE"
            printf '%s\n' "$MCP_LINE" >> "$MCP_PROFILE"
            printf '%s\n' "$MCP_END" >> "$MCP_PROFILE"
            echo "Added PULP_MCP_BINARY to $MCP_PROFILE"
        fi
    }

    if [ -x "$INSTALL_DIR/pulp-mcp" ]; then
        if [ "$SHELL_NAME" = "bash" ]; then
            ensure_mcp_profile "$HOME/.bashrc"
            if [ -f "$HOME/.bash_profile" ]; then
                ensure_mcp_profile "$HOME/.bash_profile"
            else
                ensure_mcp_profile "$HOME/.profile"
            fi
        else
            ensure_mcp_profile "$PROFILE"
        fi
    fi
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
