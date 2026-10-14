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
# Installer source is pinned to the same Shipyard release as the
# binary itself (`tags/<pinned-version>/install.sh`), so a given
# Pulp commit always runs the same installer code — the same
# supply-chain story as any other pinned dep.
#
# Usage:
#   ./tools/install-shipyard.sh           # install pinned version (first-time setup)
#   ./tools/install-shipyard.sh --status  # show installed vs pinned
#
# After the first install, `shipyard update` (Shipyard v0.55.0+) is
# the preferred in-tool upgrade path. This wrapper is only needed
# for the initial bootstrap and for `--status` checks against the pin.
#
# The version comes from tools/toolchain/manifest.toml (generated into
# tools/shipyard.toml), and so do the sha256 digests of every release asset
# (generated into tools/toolchain/shipyard.sha256). The upstream installer
# checks no digest, so this wrapper downloads the asset itself and refuses one
# whose sha256 differs from the pin before the installer runs, then checks
# that the binary the installer put in place is the pinned one.
#
# Exit codes:
#   0   success (or already installed and matching pin)
#   1   user error (bad flag, missing tools)
#   2   download / verification failure (a digest mismatch, or propagated
#       from the installer)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIN_FILE="$SCRIPT_DIR/shipyard.toml"

# ── Argument parsing ────────────────────────────────────────────────────────

MODE=install
for arg in "$@"; do
    case "$arg" in
        --status)  MODE=status ;;
        -h|--help)
            sed -n '2,26p' "$0" | sed 's/^# \?//'
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

SUMS_FILE="$SCRIPT_DIR/toolchain/shipyard.sha256"
REPO="$(sed -n '/^\[shipyard\]/,/^\[/p' "$PIN_FILE" \
    | sed -n 's/^repo[[:space:]]*=[[:space:]]*"\(.*\)"$/\1/p' \
    | head -1)"
REPO="${REPO:-danielraffel/Shipyard}"

# Supply-chain: pin the installer script to the same Shipyard release
# tag as the binary it installs. Downloading `install.sh` from `main`
# means the same Pulp commit can run different installer code over
# time; pinning to `refs/tags/<VERSION>` makes the installer
# reproducible alongside everything else in this repo.
# The script is pinned by sha256 too (tools/toolchain/shipyard.sha256), and
# is downloaded, verified and only then run.
UPSTREAM_INSTALLER="https://raw.githubusercontent.com/${REPO}/refs/tags/${VERSION}/install.sh"

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

# ── Queue-file truncation recovery (#528) ───────────────────────────────────
# A crash between open(O_TRUNC) and the subsequent write() in Shipyard
# can leave the machine-global job queue at zero bytes. Any subsequent
# Shipyard invocation then dies with JSONDecodeError, which blocks
# autonomous ship cycles. Re-running `./tools/install-shipyard.sh` is
# the documented recovery path; defensively re-initialize the queue
# file when we see it truncated. Carried over from the pre-wrapper
# script — the regression test `test_install_shipyard.py` still
# covers this path.
repair_truncated_queue_file() {
    local state_dir=""
    case "$(uname -s)" in
        Darwin)     state_dir="$HOME/Library/Application Support/shipyard" ;;
        Linux)      state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/shipyard" ;;
        MINGW*|MSYS*|CYGWIN*) state_dir="$HOME/AppData/Local/shipyard" ;;
        *)          return 0 ;;
    esac

    local queue_file="$state_dir/queue/queue.json"
    if [ -f "$queue_file" ] && [ ! -s "$queue_file" ]; then
        echo "→ Shipyard queue file is empty — reinitializing (#528)"
        mkdir -p "$(dirname "$queue_file")"
        echo '{"jobs": []}' > "$queue_file"
    fi
}

repair_truncated_queue_file

# ── CI token preflight ──────────────────────────────────────────────────────
# The upstream installer resolves the release through the GitHub API, and
# sends `Authorization: Bearer` only when SHIPYARD_GITHUB_TOKEN or GITHUB_TOKEN
# is set. Anonymous calls from shared hosted-runner IPs exhaust the 60/hour
# limit on busy days and fail with HTTP 403, so under GitHub Actions a missing
# token is refused here, naming the fix, instead of surfacing as a flaky
# download error. Local runs stay anonymous-OK.
if [ "${GITHUB_ACTIONS:-}" = "true" ] \
    && [ -z "${SHIPYARD_GITHUB_TOKEN:-}" ] && [ -z "${GITHUB_TOKEN:-}" ]; then
    echo "Error: install-shipyard.sh is running under GitHub Actions without a token." >&2
    echo "  Anonymous GitHub API release lookups are rate-limited (HTTP 403) on" >&2
    echo "  shared runner IPs. Pass the job token to this step:" >&2
    echo "" >&2
    echo "      env:" >&2
    echo "        GITHUB_TOKEN: \${{ github.token }}" >&2
    echo "" >&2
    echo "  (SHIPYARD_GITHUB_TOKEN is also accepted.)" >&2
    exit 1
fi

# ── Verify the pinned release assets ────────────────────────────────────────
# Asset names follow the upstream installer: the CLI, plus the workstream
# provider on Linux and Windows. macOS ships a .dmg holding the binary.

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

pinned_digest() {
    awk -v name="$1" '$1 !~ /^#/ && $2 == name {print $1}' "$SUMS_FILE"
}

case "$(uname -s)" in
    Darwin) ASSET_OS=macos ;;
    Linux)  ASSET_OS=linux ;;
    MINGW*|MSYS*|CYGWIN*) ASSET_OS=windows ;;
    *) echo "Error: unsupported OS $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    arm64|aarch64) ASSET_ARCH=arm64 ;;
    x86_64|amd64)  ASSET_ARCH=x64 ;;
    *) echo "Error: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
EXE=""
[ "$ASSET_OS" = windows ] && EXE=".exe"
if [ "$ASSET_OS" = macos ]; then
    ASSETS="shipyard-macos-$ASSET_ARCH.dmg"
else
    ASSETS="shipyard-$ASSET_OS-$ASSET_ARCH$EXE shipyard-workstream-provider-$ASSET_OS-$ASSET_ARCH$EXE"
fi

if ! [ -f "$SUMS_FILE" ]; then
    echo "Error: $SUMS_FILE is missing; fix: python3 tools/toolchain/generate.py" >&2
    exit 1
fi

VERIFY=1
if [ "${SHIPYARD_SKIP_DOWNLOAD:-0}" = "1" ]; then
    # The installer reuses the binary already in its install dir and downloads
    # nothing, so there is no download to check.
    VERIFY=0
    echo "→ NOT VERIFIED: SHIPYARD_SKIP_DOWNLOAD=1 reuses the installed binary; its digest is not checked"
fi

VERIFY_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pulp-shipyard-verify.XXXXXX")"
trap 'rm -rf "$VERIFY_DIR"' EXIT
for asset in $ASSETS; do
    [ "$VERIFY" = 1 ] || break
    want="$(pinned_digest "$asset")"
    url="https://github.com/${REPO}/releases/download/${VERSION}/${asset}"
    if [ -z "$want" ]; then
        echo "Error: tools/toolchain/manifest.toml pins no sha256 for $asset ($VERSION)." >&2
        echo "  fix: python3 tools/toolchain/generate.py --shipyard $VERSION" >&2
        exit 2
    fi
    if ! curl -fsSL "$url" -o "$VERIFY_DIR/$asset"; then
        echo "Error: could not download $url to verify it." >&2
        exit 2
    fi
    got="$(sha256_of "$VERIFY_DIR/$asset")"
    if [ "$got" != "$want" ]; then
        echo "Error: $url does not match the pin; refusing to install." >&2
        echo "  pinned sha256:   $want (tools/toolchain/manifest.toml [shipyard.assets])" >&2
        echo "  received sha256: $got" >&2
        echo "  The release asset was replaced after it was pinned. Confirm with the Shipyard" >&2
        echo "  release notes or its publisher, then accept it in a reviewed PR:" >&2
        echo "    python3 tools/toolchain/generate.py --shipyard $VERSION" >&2
        exit 2
    fi
done
[ "$VERIFY" = 1 ] && echo "→ Verified $ASSETS against tools/toolchain/manifest.toml"

# The binaries the installer should put in place, as "<name> <sha256>" lines:
# the raw assets on Linux and Windows, the files inside the verified .dmg on
# macOS.
INSTALL_DIR="${SHIPYARD_INSTALL_DIR:-$HOME/.local/bin}"
EXPECTED_BINARIES=""
if [ "$VERIFY" = 0 ]; then
    :
elif [ "$ASSET_OS" = macos ]; then
    mnt="$VERIFY_DIR/mnt"
    if hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$VERIFY_DIR/$ASSETS" >/dev/null 2>&1; then
        for binary in shipyard shipyard-workstream-provider; do
            if [ -f "$mnt/$binary" ]; then
                EXPECTED_BINARIES="$EXPECTED_BINARIES$binary $(sha256_of "$mnt/$binary")
"
            fi
        done
        hdiutil detach "$mnt" >/dev/null 2>&1 || true
    fi
    case "$EXPECTED_BINARIES" in
        "shipyard "*) ;;
        *)
            echo "Error: could not read the shipyard binary inside the verified $ASSETS." >&2
            exit 2
            ;;
    esac
else
    EXPECTED_BINARIES="shipyard$EXE $(pinned_digest "shipyard-$ASSET_OS-$ASSET_ARCH$EXE")
shipyard-workstream-provider$EXE $(pinned_digest "shipyard-workstream-provider-$ASSET_OS-$ASSET_ARCH$EXE")
"
fi

# ── Verify the pinned upstream installer script ─────────────────────────────

INSTALLER_FILE="$VERIFY_DIR/install.sh"
installer_want="$(pinned_digest install.sh)"
if [ -z "$installer_want" ]; then
    echo "Error: tools/toolchain/manifest.toml pins no sha256 for install.sh ($VERSION)." >&2
    echo "  fix: python3 tools/toolchain/generate.py --shipyard $VERSION" >&2
    exit 2
fi
if ! curl -fsSL "$UPSTREAM_INSTALLER" -o "$INSTALLER_FILE"; then
    echo "Error: could not download $UPSTREAM_INSTALLER to verify it." >&2
    exit 2
fi
installer_got="$(sha256_of "$INSTALLER_FILE")"
if [ "$installer_got" != "$installer_want" ]; then
    echo "Error: $UPSTREAM_INSTALLER does not match the pin; refusing to run it." >&2
    echo "  pinned sha256:   $installer_want (tools/toolchain/manifest.toml [shipyard] installer_sha256)" >&2
    echo "  received sha256: $installer_got" >&2
    echo "  The script at that tag changed after it was pinned. Confirm with the Shipyard" >&2
    echo "  publisher, then accept it in a reviewed PR:" >&2
    echo "    python3 tools/toolchain/generate.py --shipyard $VERSION" >&2
    exit 2
fi

# Keep the binaries the installer is about to replace, so a binary that turns
# out not to be the pinned one can be removed and the previous one put back.
BACKUP_DIR="$VERIFY_DIR/previous"
mkdir -p "$BACKUP_DIR"
for binary in "shipyard$EXE" "shipyard-workstream-provider$EXE"; do
    if [ -f "$INSTALL_DIR/$binary" ]; then
        cp -p "$INSTALL_DIR/$binary" "$BACKUP_DIR/$binary"
    fi
done

restore_previous() {
    for binary in "shipyard$EXE" "shipyard-workstream-provider$EXE"; do
        rm -f "$INSTALL_DIR/$binary"
        if [ -f "$BACKUP_DIR/$binary" ]; then
            cp -p "$BACKUP_DIR/$binary" "$INSTALL_DIR/.$binary.restore.$$"
            mv -f "$INSTALL_DIR/.$binary.restore.$$" "$INSTALL_DIR/$binary"
            echo "  restored the previous $INSTALL_DIR/$binary" >&2
        else
            echo "  removed $INSTALL_DIR/$binary (there was no previous one)" >&2
        fi
    done
}

# ── Delegate to upstream installer ──────────────────────────────────────────

echo "→ Installing Shipyard $VERSION via upstream install.sh"
echo "    source: $UPSTREAM_INSTALLER"

SHIPYARD_VERSION="$VERSION" bash "$INSTALLER_FILE"

# The installer downloads the assets again; check that what it installed is
# what was verified above, so a replacement between the two downloads is
# caught. On a mismatch the new binaries are removed and the previous ones put
# back before exiting, so nothing unverified is left on PATH.
if [ "$VERIFY" = 1 ]; then
    while read -r binary want; do
        [ -n "$binary" ] || continue
        installed_sha="$(sha256_of "$INSTALL_DIR/$binary" 2>/dev/null || true)"
        if [ "$installed_sha" != "$want" ]; then
            echo "Error: $INSTALL_DIR/$binary is not the pinned Shipyard $VERSION binary." >&2
            echo "  expected sha256: $want" >&2
            echo "  installed sha256: ${installed_sha:-unreadable}" >&2
            restore_previous
            echo "  The release changed while installing: report it before accepting any new digest." >&2
            exit 2
        fi
    done <<EOF_EXPECTED
$EXPECTED_BINARIES
EOF_EXPECTED
fi

# ── Legacy cleanup (post-success only) ──────────────────────────────────────
# Earlier versions of this script installed to ~/.pulp/shipyard/<v>/
# with a symlink at ~/.pulp/bin/shipyard. When that bin dir comes
# first on PATH, the stale symlink shadows the canonical
# ~/.local/bin/shipyard and users silently run an old binary.
#
# Only clean up AFTER the upstream installer succeeded — otherwise a
# network failure would delete the user's only working binary and
# leave them with no shipyard on PATH. Running the wrapper is an
# idempotent operation; cleanup on the NEXT successful run is fine.

if [ -L "$HOME/.pulp/bin/shipyard" ]; then
    echo "→ Removing legacy symlink $HOME/.pulp/bin/shipyard"
    rm -f "$HOME/.pulp/bin/shipyard"
fi
if [ -d "$HOME/.pulp/shipyard" ]; then
    echo "→ Removing legacy install tree $HOME/.pulp/shipyard"
    rm -rf "$HOME/.pulp/shipyard"
fi

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
