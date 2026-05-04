#!/bin/bash
# Pulp development environment setup
# Run this after cloning to bootstrap a working build environment.
#
# Usage:
#   ./setup.sh              # Interactive setup
#   ./setup.sh --ci         # Non-interactive (CI/automation)
#   ./setup.sh --deps-only  # Bootstrap dependencies without configuring/building
#   ./setup.sh --dry-run    # Show what would be done without doing it

set -e

# ── Configuration ────────────────────────────────────────────────────────────

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRY_RUN=false
CI_MODE=false
DEPS_ONLY=false
ERRORS=0

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        --ci)      CI_MODE=true ;;
        --deps-only) DEPS_ONLY=true ;;
        --help|-h)
            echo "Usage: ./setup.sh [--ci] [--deps-only] [--dry-run]"
            echo ""
            echo "Options:"
            echo "  --ci        Non-interactive mode for CI/automation"
            echo "  --deps-only Bootstrap external dependencies and stop before configure/build"
            echo "  --dry-run   Show what would be done without doing it"
            exit 0
            ;;
    esac
done

# ── Helpers ──────────────────────────────────────────────────────────────────

info()  { echo "  ✓ $*"; }
warn()  { echo "  ⚠ $*"; }
fail()  { echo "  ✗ $*"; ERRORS=$((ERRORS + 1)); }
step()  { echo ""; echo "── $* ──"; }
dry()   { if $DRY_RUN; then echo "  [dry-run] $*"; return 0; fi; return 1; }

prepend_path_if_dir() {
    local dir="$1"
    [ -n "$dir" ] || return 0
    [ -d "$dir" ] || return 0

    case ":$PATH:" in
        *":$dir:"*) ;;
        *) PATH="$dir:$PATH" ;;
    esac
}

prompt_yn() {
    if $CI_MODE; then return 0; fi
    local prompt="$1 [Y/n] "
    read -r -p "$prompt" response
    case "$response" in
        [nN]*) return 1 ;;
        *) return 0 ;;
    esac
}

skia_has_lfs_pointer() {
    local skia_check="$1"
    local candidate

    candidate="$(find "$skia_check" \( -name "*.a" -o -name "*.lib" \) -print -quit 2>/dev/null || true)"
    [ -n "$candidate" ] || return 1
    [ -f "$candidate" ] || return 1
    head -1 "$candidate" 2>/dev/null | grep -q "version https://git-lfs"
}

fetchcontent_cache_root() {
    if [ -n "${PULP_SHARED_FETCHCONTENT_SOURCE_DIR:-}" ]; then
        echo "$PULP_SHARED_FETCHCONTENT_SOURCE_DIR"
        return
    fi

    case "$PLATFORM" in
        macOS)
            echo "$HOME/Library/Caches/Pulp/fetchcontent-src"
            ;;
        Windows)
            if [ -n "${LOCALAPPDATA:-}" ]; then
                echo "$LOCALAPPDATA/Pulp/fetchcontent-src"
            else
                echo "$HOME/AppData/Local/Pulp/fetchcontent-src"
            fi
            ;;
        *)
            echo "${XDG_CACHE_HOME:-$HOME/.cache}/pulp/fetchcontent-src"
            ;;
    esac
}

fetchcontent_cache_dir_name() {
    local base_name="$1"
    local ref="$2"
    local sanitized_ref

    sanitized_ref="$(printf '%s' "$ref" | tr -c 'A-Za-z0-9._-' '_' | tr -s '_')"
    sanitized_ref="${sanitized_ref#_}"
    sanitized_ref="${sanitized_ref%_}"

    if [ -n "$sanitized_ref" ]; then
        printf '%s-%s\n' "$base_name" "$sanitized_ref"
    else
        printf '%s\n' "$base_name"
    fi
}

find_local_git_seed() {
    local repo="$1"
    local exclude_path="$2"
    local candidate
    local remote

    if [ -d "$FETCHCONTENT_CACHE_ROOT" ]; then
        while IFS= read -r -d '' candidate; do
            [ "$candidate" = "$exclude_path" ] && continue
            [ -d "$candidate/.git" ] || continue
            remote="$(git -C "$candidate" remote get-url origin 2>/dev/null || true)"
            if [ "$remote" = "$repo" ]; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done < <(find "$FETCHCONTENT_CACHE_ROOT" -mindepth 1 -maxdepth 1 -type d -print0 2>/dev/null)
    fi

    if [ -d "$REPO_ROOT/build/_deps" ]; then
        while IFS= read -r -d '' candidate; do
            [ -d "$candidate/.git" ] || continue
            remote="$(git -C "$candidate" remote get-url origin 2>/dev/null || true)"
            if [ "$remote" = "$repo" ]; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done < <(find "$REPO_ROOT/build/_deps" -mindepth 1 -maxdepth 1 -type d -name '*-src' -print0 2>/dev/null)
    fi

    if [ -d "$REPO_ROOT/external" ]; then
        while IFS= read -r -d '' candidate; do
            [ -d "$candidate/.git" ] || continue
            remote="$(git -C "$candidate" remote get-url origin 2>/dev/null || true)"
            if [ "$remote" = "$repo" ]; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done < <(find "$REPO_ROOT/external" -mindepth 1 -maxdepth 1 -type d -print0 2>/dev/null)
    fi

    return 1
}

reuse_shared_git_source() {
    local label="$1"
    local shared_dir="$2"
    local local_dir="$3"
    local marker_path="$4"

    if [ -e "$local_dir" ] && [ ! -L "$local_dir" ] && [ -e "$local_dir/$marker_path" ]; then
        info "$label present"
        return 0
    fi

    if [ -L "$local_dir" ]; then
        if [ -e "$local_dir/$marker_path" ]; then
            info "$label linked from shared cache"
            return 0
        fi
        rm "$local_dir"
    fi

    if [ ! -d "$shared_dir/.git" ]; then
        warn "Shared $label cache missing at $shared_dir"
        return 1
    fi

    mkdir -p "$(dirname "$local_dir")"

    if [ "$PLATFORM" = "Windows" ]; then
        info "Cloning $label from shared cache..."
        dry "git -c protocol.file.allow=always clone --local --no-hardlinks $shared_dir $local_dir" || {
            if ! git -c protocol.file.allow=always clone --local --no-hardlinks "$shared_dir" "$local_dir" >/dev/null 2>&1; then
                git -c protocol.file.allow=always clone "$shared_dir" "$local_dir"
            fi
        }

        if [ -f "$local_dir/.gitmodules" ]; then
            while read -r key submodule_path; do
                submodule_name="${key#submodule.}"
                submodule_name="${submodule_name%.path}"
                git -C "$local_dir" config "submodule.${submodule_name}.url" "${shared_dir}/${submodule_path}"
            done < <(git -C "$local_dir" config --file .gitmodules --get-regexp '^submodule\..*\.path$')

            dry "git -c protocol.file.allow=always -C $local_dir submodule update --init --recursive" || \
                git -c protocol.file.allow=always -C "$local_dir" submodule update --init --recursive
        fi
    else
        info "Linking $label from shared cache..."
        dry "ln -s $shared_dir $local_dir" || ln -s "$shared_dir" "$local_dir"
    fi

    if [ -e "$local_dir/$marker_path" ]; then
        info "$label ready"
    else
        fail "$label setup incomplete at $local_dir"
        return 1
    fi
}

wgpu_runtime_url_name() {
    local url_os=""
    local url_arch=""
    local url_compiler=""

    case "$PLATFORM" in
        macOS)
            url_os="macos"
            ;;
        Linux)
            url_os="linux"
            ;;
        Windows)
            url_os="windows"
            if command -v cl >/dev/null 2>&1; then
                url_compiler="msvc"
            else
                url_compiler="gnu"
            fi
            ;;
        *)
            return 1
            ;;
    esac

    case "$ARCH" in
        x86_64|amd64)
            url_arch="x86_64"
            ;;
        arm64|aarch64)
            url_arch="aarch64"
            ;;
        x86|i686)
            url_arch="i686"
            ;;
        *)
            return 1
            ;;
    esac

    if [ -n "$url_compiler" ]; then
        printf 'wgpu-%s-%s-%s-release\n' "$url_os" "$url_arch" "$url_compiler"
    else
        printf 'wgpu-%s-%s-release\n' "$url_os" "$url_arch"
    fi
}

ensure_shared_archive_source() {
    local label="$1"
    local url="$2"
    local dir_name="$3"
    local seed_dir="$4"
    local target="$FETCHCONTENT_CACHE_ROOT/$dir_name"
    local lockdir="$FETCHCONTENT_CACHE_ROOT/.${dir_name}.lock"

    mkdir -p "$FETCHCONTENT_CACHE_ROOT"

    (
        set -e
        waited=false
        while ! mkdir "$lockdir" 2>/dev/null; do
            if [ "$waited" = false ]; then
                info "Waiting for shared $label source cache lock..."
                waited=true
            fi
            sleep 1
        done
        trap 'rmdir "$lockdir" >/dev/null 2>&1 || true' EXIT

        if [ -d "$target" ]; then
            return 0
        fi

        if [ -d "$seed_dir" ]; then
            info "Seeding shared $label source cache from local directory: $seed_dir"
            if ! dry "cp -R $seed_dir $target"; then
                cp -R "$seed_dir" "$target"
            fi
            return 0
        fi

        info "Downloading shared $label source cache..."
        if dry "curl -L --fail $url -o $target.zip && unzip -q $target.zip -d $target"; then
            return 0
        fi

        local tmpdir
        tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/pulp-${dir_name}.XXXXXX")"
        trap 'rm -rf "$tmpdir" >/dev/null 2>&1 || true; rmdir "$lockdir" >/dev/null 2>&1 || true' EXIT
        curl -L --fail "$url" -o "$tmpdir/archive.zip"
        mkdir -p "$target"
        unzip -q "$tmpdir/archive.zip" -d "$target"
        rm -rf "$tmpdir"
    )
}

# Retry a git network command up to 3 times with exponential backoff.
# GitHub occasionally returns 5xx on git-over-https; without retry any
# transient outage fails the whole CI job. Applies to clone/fetch/
# submodule-update (the VST3 SDK in particular has five sub-submodules,
# any of which failing aborts the lot). See #418.
retry_git() {
    local label="$1"
    shift
    local attempts=3
    local sleep_s=5
    local i
    for i in $(seq 1 $attempts); do
        if "$@"; then
            return 0
        fi
        if [ "$i" -lt "$attempts" ]; then
            info "$label git attempt $i/$attempts failed; retrying in ${sleep_s}s..."
            sleep "$sleep_s"
            sleep_s=$((sleep_s * 2))
        fi
    done
    warn "$label git command failed after $attempts attempts"
    return 1
}

# git clone refuses to write into a non-empty target, so a generic
# retry_git around `git clone URL DEST` fails on attempt 2+ with
# "destination path already exists" instead of actually retrying
# the network fetch. Wrap clone with a clean-target-between-attempts
# shim. See #438 P1 Codex review on #425.
retry_git_clone() {
    local label="$1"
    local target="${@: -1}"   # last positional arg is the destination
    shift
    local attempts=3
    local sleep_s=5
    local i
    for i in $(seq 1 $attempts); do
        if "$@"; then
            return 0
        fi
        if [ "$i" -lt "$attempts" ]; then
            # Scrub partial clone — safe because we're inside
            # ensure_shared_git_source's per-target lock and the
            # target only exists because an earlier attempt partially
            # wrote it.
            if [ -n "$target" ] && [ -e "$target" ]; then
                rm -rf "$target"
            fi
            info "$label clone attempt $i/$attempts failed; retrying in ${sleep_s}s..."
            sleep "$sleep_s"
            sleep_s=$((sleep_s * 2))
        fi
    done
    warn "$label clone failed after $attempts attempts"
    return 1
}

# Retry-on-failure wrapper around `ensure_shared_git_source`.
#
# Motivating incident (#1375): the v0.74.0 + v0.74.1 release-cli runs
# both died at the same point on the `windows-arm64` matrix leg —
# `Priming shared Yoga source cache...` immediately followed by
# `##[error]Process completed with exit code 127.`. 127 is the standard
# "command not found" exit, almost always a transient PATH / shell
# wrapper hiccup on Windows runners (cmd-vs-bash, MSYS-vs-Git-Bash,
# etc). The same priming step succeeded for the 5 deps before it on
# both runs, and succeeded entirely on v0.75.0 — so this is flaky,
# not deterministic, and the natural mitigation is a retry.
#
# Doing the retry inside `ensure_shared_git_source` doesn't help — the
# subshell uses `set -e` and a 127 inside it tears the function down
# before any inner retry can engage. Wrap the WHOLE call instead, and
# scrub any half-written target between attempts so the next try gets
# a clean clone path.
#
# Every shared-source priming call goes through this wrapper so the
# same flake doesn't migrate to a different dep next release.
ensure_shared_git_source_with_retry() {
    local label="$1"
    local repo="$2"
    local ref="$3"
    local dir_name="$4"
    local target="$FETCHCONTENT_CACHE_ROOT/$dir_name"
    local lockdir="$FETCHCONTENT_CACHE_ROOT/.${dir_name}.lock"
    local attempts="${PULP_PRIMING_RETRY_ATTEMPTS:-3}"
    local sleep_s=5
    local i rc

    for i in $(seq 1 "$attempts"); do
        rc=0
        ensure_shared_git_source "$label" "$repo" "$ref" "$dir_name" || rc=$?
        if [ "$rc" -eq 0 ]; then
            if [ "$i" -gt 1 ]; then
                info "$label priming succeeded on attempt $i/$attempts"
            fi
            return 0
        fi
        if [ "$i" -lt "$attempts" ]; then
            warn "$label priming attempt $i/$attempts failed (exit=$rc) — retrying after ${sleep_s}s"
            # Scrub any partial target so the next attempt sees a clean
            # state. Mirrors retry_git_clone's per-attempt cleanup.
            if [ -e "$target" ]; then
                rm -rf "$target"
            fi
            # Best-effort lock release in case the inner subshell exited
            # before its EXIT trap fired (e.g. SIGKILL / 127).
            rmdir "$lockdir" >/dev/null 2>&1 || true
            sleep "$sleep_s"
            sleep_s=$((sleep_s * 2))
        else
            fail "$label priming failed after $attempts attempts (last exit=$rc)"
            return "$rc"
        fi
    done
}

ensure_shared_git_source() {
    local label="$1"
    local repo="$2"
    local ref="$3"
    local dir_name="$4"
    local target="$FETCHCONTENT_CACHE_ROOT/$dir_name"
    local lockdir="$FETCHCONTENT_CACHE_ROOT/.${dir_name}.lock"
    local current_remote=""
    local seed_repo=""
    local checkout_ref="$ref"

    mkdir -p "$FETCHCONTENT_CACHE_ROOT"

    (
        set -e
        waited=false
        while ! mkdir "$lockdir" 2>/dev/null; do
            if [ "$waited" = false ]; then
                info "Waiting for shared $label source cache lock..."
                waited=true
            fi
            sleep 1
        done
        trap 'rmdir "$lockdir" >/dev/null 2>&1 || true' EXIT

        if [ -d "$target/.git" ]; then
            current_remote="$(git -C "$target" remote get-url origin 2>/dev/null || true)"
            if [ -n "$current_remote" ] && [ "$current_remote" != "$repo" ]; then
                info "Updating shared $label cache origin to $repo"
                dry "git -C $target remote set-url origin $repo" || git -C "$target" remote set-url origin "$repo"
            fi
        else
            info "Priming shared $label source cache..."
            seed_repo="$(find_local_git_seed "$repo" "$target" || true)"
            if [ -n "$seed_repo" ]; then
                info "Seeding shared $label source cache from local clone: $seed_repo"
                if ! dry "git clone --local --no-hardlinks $seed_repo $target"; then
                    if ! git clone --local --no-hardlinks "$seed_repo" "$target" >/dev/null 2>&1; then
                        git clone "$seed_repo" "$target"
                    fi
                fi
            elif ! dry "git clone --filter=blob:none $repo $target"; then
                # Network clone: retry to tolerate transient GitHub
                # blips. retry_git_clone removes a partial $target
                # between attempts so the second try can actually
                # re-run the network fetch.
                retry_git_clone "$label" \
                    git clone --filter=blob:none "$repo" "$target"
            fi

            current_remote="$(git -C "$target" remote get-url origin 2>/dev/null || true)"
            if [ -n "$current_remote" ] && [ "$current_remote" != "$repo" ]; then
                info "Updating shared $label cache origin to $repo"
                dry "git -C $target remote set-url origin $repo" || git -C "$target" remote set-url origin "$repo"
            fi
        fi

        if ! git -C "$target" cat-file -e "${ref}^{commit}" 2>/dev/null; then
            info "Updating shared $label source cache to include $ref..."
            dry "git -C $target fetch --tags origin" || \
                retry_git "$label fetch --tags" git -C "$target" fetch --tags origin
        fi

        if ! git -C "$target" cat-file -e "${ref}^{commit}" 2>/dev/null; then
            info "Fetching explicit shared $label ref $ref..."
            dry "git -C $target fetch --force origin $ref" || \
                retry_git "$label fetch $ref" git -C "$target" fetch --force origin "$ref"
            checkout_ref="FETCH_HEAD"
        fi

        if dry "git -C $target checkout --detach $checkout_ref"; then
            :
        else
            git -C "$target" checkout --detach "$checkout_ref"
        fi

        if [ -f "$target/.gitmodules" ]; then
            # submodule update fetches over the network per sub-submodule
            # (e.g. VST3 SDK has 5 sub-submodules). Retry the whole thing
            # so one transient failure doesn't cost a full CI cycle (#402).
            dry "git -c protocol.file.allow=always -C $target submodule update --init --recursive" || \
                retry_git "$label submodule update" \
                    git -c protocol.file.allow=always -C "$target" submodule update --init --recursive
        fi
    )
}

# ── Platform detection ───────────────────────────────────────────────────────

step "Detecting platform"

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Darwin)  PLATFORM="macOS" ;;
    Linux)   PLATFORM="Linux" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM="Windows" ;;
    *)       PLATFORM="Unknown" ;;
esac

info "$PLATFORM ($ARCH)"

if [ "$PLATFORM" = "Windows" ]; then
    if command -v cl >/dev/null 2>&1; then
        info "MSVC toolchain already available in PATH"
    else
        warn "MSVC environment not loaded; prefer `powershell -ExecutionPolicy Bypass -File .\\setup.ps1` or run from a Developer PowerShell/Command Prompt"
    fi
fi

if [ "$PLATFORM" = "Linux" ]; then
    # Non-interactive SSH shells often omit ~/.local/bin even when user tools
    # such as git-lfs are installed there. Prefer the common user-local bin
    # path before dependency checks so direct setup.sh and doctor agree.
    prepend_path_if_dir "$HOME/.local/bin"
fi

# ── Check: C++20 compiler ───────────────────────────────────────────────────

step "Checking C++20 compiler"

if command -v clang++ &>/dev/null; then
    CLANG_VERSION=$(clang++ --version 2>&1 | head -1)
    info "clang++: $CLANG_VERSION"
elif command -v g++ &>/dev/null; then
    GCC_VERSION=$(g++ --version 2>&1 | head -1)
    info "g++: $GCC_VERSION"
elif [ "$PLATFORM" = "Windows" ] && command -v cl &>/dev/null; then
    CL_VERSION=$(cl 2>&1 | head -1)
    info "cl: $CL_VERSION"
else
    fail "No C++20 compiler found"
    if [ "$PLATFORM" = "macOS" ]; then
        echo "    Fix: xcode-select --install"
    elif [ "$PLATFORM" = "Linux" ]; then
        echo "    Fix: sudo apt install g++-13  (or equivalent for your distro)"
    elif [ "$PLATFORM" = "Windows" ]; then
        echo "    Fix: install Visual Studio Build Tools with C++ workload or run from a Developer Command Prompt"
    fi
fi

# macOS: check Xcode CLT
if [ "$PLATFORM" = "macOS" ]; then
    if xcode-select -p &>/dev/null; then
        info "Xcode Command Line Tools installed"
    else
        warn "Xcode Command Line Tools not found"
        if prompt_yn "Install Xcode Command Line Tools?"; then
            dry "xcode-select --install" || xcode-select --install
        else
            echo "    Manual fix: xcode-select --install"
        fi
    fi
fi

# ── Check: CMake ────────────────────────────────────────────────────────────

step "Checking CMake"

if command -v cmake &>/dev/null; then
    CMAKE_VERSION=$(cmake --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
    CMAKE_MAJOR=$(echo "$CMAKE_VERSION" | cut -d. -f1)
    CMAKE_MINOR=$(echo "$CMAKE_VERSION" | cut -d. -f2)

    if [ "$CMAKE_MAJOR" -gt 3 ] || { [ "$CMAKE_MAJOR" -eq 3 ] && [ "$CMAKE_MINOR" -ge 24 ]; }; then
        info "cmake $CMAKE_VERSION"
    else
        fail "cmake $CMAKE_VERSION is too old (need 3.24+)"
        if [ "$PLATFORM" = "macOS" ]; then
            echo "    Fix: brew upgrade cmake"
        else
            echo "    Fix: install CMake 3.24+ from https://cmake.org/download/"
        fi
    fi
else
    fail "CMake not found"
    if [ "$PLATFORM" = "macOS" ]; then
        echo "    Fix: brew install cmake"
    elif [ "$PLATFORM" = "Linux" ]; then
        echo "    Fix: sudo apt install cmake  (or snap install cmake --classic)"
    fi
fi

# ── Check: git-lfs ──────────────────────────────────────────────────────────

step "Checking git-lfs"

if command -v git-lfs &>/dev/null || git lfs version &>/dev/null; then
    GIT_LFS_VERSION=$(git lfs version 2>&1 | head -1)
    info "git-lfs: $GIT_LFS_VERSION"

    # Force local filter repair without touching hooks. This repo intentionally
    # uses core.hooksPath=/dev/null, so we only want the filter config needed
    # for checkout/pull on fresh worktrees and SSH sessions.
    info "Ensuring git-lfs filters are initialized for this repository..."
    if dry "git -C $REPO_ROOT lfs install --local --force --skip-repo"; then
        :
    elif git -C "$REPO_ROOT" lfs install --local --force --skip-repo; then
        info "git-lfs filters initialized for $REPO_ROOT"
    else
        fail "git-lfs filter setup failed for this repository"
        echo "    Fix: git -C \"$REPO_ROOT\" lfs install --local --force --skip-repo"
    fi
else
    fail "git-lfs not found"
    if [ "$PLATFORM" = "macOS" ]; then
        if command -v brew &>/dev/null; then
            if prompt_yn "Install git-lfs via Homebrew?"; then
                dry "brew install git-lfs && git lfs install" || {
                    brew install git-lfs
                    git lfs install
                }
                info "git-lfs installed"
            fi
        else
            echo "    Fix: brew install git-lfs && git lfs install"
            echo "    Or: https://git-lfs.com"
        fi
    elif [ "$PLATFORM" = "Linux" ]; then
        echo "    Fix: sudo apt install git-lfs && git lfs install"
    fi
fi

# ── Check: git-lfs files pulled ─────────────────────────────────────────────

step "Checking git-lfs files"

# Check if Skia files are actual binaries or just LFS pointers
SKIA_CHECK="$REPO_ROOT/external/skia-build"
if [ -d "$SKIA_CHECK" ]; then
    if skia_has_lfs_pointer "$SKIA_CHECK"; then
        warn "Skia files are LFS pointers (not actual binaries)"
        echo "    Pulling LFS files..."
        if dry "git -C $REPO_ROOT lfs pull"; then
            :
        elif git -C "$REPO_ROOT" lfs pull; then
            if skia_has_lfs_pointer "$SKIA_CHECK"; then
                fail "Skia files are still LFS pointers after git lfs pull"
                echo "    Fix: git -C \"$REPO_ROOT\" lfs install --local && git -C \"$REPO_ROOT\" lfs pull"
            else
                info "LFS files pulled"
            fi
        else
            fail "git-lfs pull failed"
            echo "    Fix: git -C \"$REPO_ROOT\" lfs install --local && git -C \"$REPO_ROOT\" lfs pull"
        fi
    else
        info "Skia binaries present"
    fi
else
    info "Skia build directory not found (GPU rendering stays disabled unless Skia is present)"
fi

# ── Install agent-agnostic git hooks ───────────────────────────────────────
# Wires core.hooksPath at .githooks so every push runs the versioning +
# skill-sync gates (advisory by default; CI sets PULP_ENFORCE_PREPUSH=1
# to upgrade to hard fail). Idempotent — safe to rerun.

if [ -x "$REPO_ROOT/tools/scripts/install-githooks.sh" ]; then
    step "Installing git hooks"
    "$REPO_ROOT/tools/scripts/install-githooks.sh" || {
        info "install-githooks exited non-zero; continuing setup."
    }
fi

# ── Check: External SDKs ───────────────────────────────────────────────────

step "Setting up external SDKs"

FETCHCONTENT_CACHE_ROOT="$(fetchcontent_cache_root)"
info "Shared FetchContent source cache: $FETCHCONTENT_CACHE_ROOT"

ensure_shared_git_source_with_retry "CHOC" "https://github.com/Tracktion/choc.git" \
    "f0f5cdf5a938b8b779fea6c083571cce5ccab925" "$(fetchcontent_cache_dir_name "choc" "f0f5cdf5a938b8b779fea6c083571cce5ccab925")"

ensure_shared_git_source_with_retry "WebGPU-distribution" "https://github.com/eliemichel/WebGPU-distribution.git" \
    "17dcd42a7683355e7a40ac4e97e77f36dff5b5ab" "$(fetchcontent_cache_dir_name "webgpu" "17dcd42a7683355e7a40ac4e97e77f36dff5b5ab")"

WGPU_NATIVE_VERSION="v24.0.3.1"
WGPU_RUNTIME_NAME="$(wgpu_runtime_url_name || true)"
if [ -n "$WGPU_RUNTIME_NAME" ]; then
    ensure_shared_archive_source "wgpu-native runtime" \
        "https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_NATIVE_VERSION}/${WGPU_RUNTIME_NAME}.zip" \
        "$(fetchcontent_cache_dir_name "$WGPU_RUNTIME_NAME" "$WGPU_NATIVE_VERSION")" \
        "$REPO_ROOT/build/_deps/${WGPU_RUNTIME_NAME}-src"
fi

ensure_shared_git_source_with_retry "SDL3" "https://github.com/libsdl-org/SDL.git" \
    "release-3.2.12" "$(fetchcontent_cache_dir_name "sdl3" "release-3.2.12")"

ensure_shared_git_source_with_retry "CLAP" "https://github.com/free-audio/clap.git" \
    "1.2.2" "$(fetchcontent_cache_dir_name "clap" "1.2.2")"

ensure_shared_git_source_with_retry "LV2" "https://github.com/lv2/lv2.git" \
    "v1.18.10" "$(fetchcontent_cache_dir_name "lv2" "v1.18.10")"

ensure_shared_git_source_with_retry "Yoga" "https://github.com/facebook/yoga.git" \
    "v3.2.1" "$(fetchcontent_cache_dir_name "yoga" "v3.2.1")"

ensure_shared_git_source_with_retry "Catch2" "https://github.com/catchorg/Catch2.git" \
    "v3.7.1" "$(fetchcontent_cache_dir_name "catch2" "v3.7.1")"

# VST3 SDK
VST3_SDK_REF="v3.7.12_build_20"
VST3_SHARED_DIR="$FETCHCONTENT_CACHE_ROOT/$(fetchcontent_cache_dir_name "vst3sdk" "$VST3_SDK_REF")"
ensure_shared_git_source_with_retry "VST3 SDK" "https://github.com/steinbergmedia/vst3sdk.git" \
    "$VST3_SDK_REF" "$(fetchcontent_cache_dir_name "vst3sdk" "$VST3_SDK_REF")"
VST3_DIR="$REPO_ROOT/external/vst3sdk"
reuse_shared_git_source "VST3 SDK" "$VST3_SHARED_DIR" "$VST3_DIR" "pluginterfaces"

# AudioUnit SDK (macOS only)
if [ "$PLATFORM" = "macOS" ]; then
    # Pinned to 1.3.0 — the 1.4.0 release started including <expected>
    # (C++23) in its headers, but the AppleClang/libc++ shipping on
    # GitHub-hosted macOS-14 runners doesn't yet expose std::expected
    # even at -std=c++23. 1.3.0 is the last C++17/20-friendly tag;
    # bump to 1.4.x when AppleClang catches up. See issue #155.
    AU_SDK_REF="AudioUnitSDK-1.3.0"
    AU_SHARED_DIR="$FETCHCONTENT_CACHE_ROOT/$(fetchcontent_cache_dir_name "AudioUnitSDK" "$AU_SDK_REF")"
    ensure_shared_git_source_with_retry "AudioUnitSDK" "https://github.com/apple/AudioUnitSDK.git" \
        "$AU_SDK_REF" "$(fetchcontent_cache_dir_name "AudioUnitSDK" "$AU_SDK_REF")"
    AU_DIR="$REPO_ROOT/external/AudioUnitSDK"
    reuse_shared_git_source "AudioUnitSDK" "$AU_SHARED_DIR" "$AU_DIR" "include/AudioUnitSDK/AUBase.h"
fi

# Linux: check ALSA dev headers
if [ "$PLATFORM" = "Linux" ]; then
    if pkg-config --exists alsa 2>/dev/null; then
        info "ALSA dev headers present"
    else
        fail "ALSA dev headers not found"
        echo "    Fix: sudo apt install libasound2-dev"
    fi

    # SDL3 pulls in desktop windowing backends on Linux. CI and first-time
    # contributors hit configure failures if the X11/Wayland development
    # headers are missing, so surface that early with a targeted fix hint.
    MISSING_LINUX_DESKTOP_DEPS=()
    for pkg in x11 xext xrandr xrender xfixes xi xinerama xkbcommon wayland-client egl gbm drm; do
        if ! pkg-config --exists "$pkg" 2>/dev/null; then
            MISSING_LINUX_DESKTOP_DEPS+=("$pkg")
        fi
    done
    if [ ${#MISSING_LINUX_DESKTOP_DEPS[@]} -gt 0 ]; then
        warn "Missing Linux desktop dependencies: ${MISSING_LINUX_DESKTOP_DEPS[*]}"
        echo "    These are required for full native UI/GPU-capable builds on a fresh Linux machine."
        echo "    Common Ubuntu fix: sudo apt install libx11-dev libxext-dev libxrandr-dev libxrender-dev libxfixes-dev libxi-dev libxinerama-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libgbm-dev libdrm-dev libdbus-1-dev"
    else
        info "Linux desktop development headers present"
    fi
fi

# ── Optional: lcov (for full LCOV_EXCL_* marker support) ──────────────────
#
# Issue #1058: Pulp's coverage pipeline (scripts/run_coverage.sh +
# tools/scripts/local_diff_cover.sh) pipes the raw .lcov from
# `llvm-cov export` through `lcov --filter region` so source-level
# `LCOV_EXCL_START` / `LCOV_EXCL_STOP` markers actually propagate to
# diff-cover. The pipeline still works without lcov (it falls back to
# the unfiltered .lcov + a warning); installing lcov just unlocks the
# per-block exclusions.

step "Checking lcov (optional, for LCOV_EXCL markers)"

if command -v lcov &>/dev/null; then
    info "lcov: $(lcov --version 2>&1 | head -1)"
else
    warn "lcov not found — LCOV_EXCL_START/STOP markers won't be honored in coverage reports"
    if [ "$PLATFORM" = "macOS" ]; then
        echo "    Optional fix: brew install lcov"
    elif [ "$PLATFORM" = "Linux" ]; then
        echo "    Optional fix: sudo apt install lcov"
    fi
fi

# ── Summary before build ───────────────────────────────────────────────────

if [ $ERRORS -gt 0 ]; then
    echo ""
    echo "Found $ERRORS issue(s). Fix them and re-run ./setup.sh"
    echo "Or run: pulp doctor  (after building)"
    exit 1
fi

if $DRY_RUN; then
    echo ""
    echo "Dry run complete. No changes were made."
    echo "Run ./setup.sh without --dry-run to execute."
    exit 0
fi

if $DEPS_ONLY; then
    step "Dependency bootstrap complete"
    echo ""
    echo "  Dependencies are ready. Skipping configure/build because --deps-only was requested."
    exit 0
fi

# ── Configure ───────────────────────────────────────────────────────────────

step "Configuring CMake"

cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" -DCMAKE_BUILD_TYPE=Debug

# ── Build ───────────────────────────────────────────────────────────────────

step "Building"

JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
cmake --build "$REPO_ROOT/build" -j"$JOBS"

# ── Test ────────────────────────────────────────────────────────────────────

step "Running tests"

CTEST_ARGS=(--test-dir "$REPO_ROOT/build" --output-on-failure)
if [ "$OS" = "windows" ]; then
    CTEST_ARGS+=(-C Debug)
fi
ctest "${CTEST_ARGS[@]}"

# ── Report ──────────────────────────────────────────────────────────────────

step "Setup complete"

echo ""
echo "  Build artifacts:"

for fmt_dir in VST3 CLAP AU; do
    dir="$REPO_ROOT/build/$fmt_dir"
    if [ -d "$dir" ]; then
        for plugin in "$dir"/*; do
            [ -e "$plugin" ] && echo "    $fmt_dir: $(basename "$plugin")"
        done
    fi
done

# Find standalone binaries
for example_dir in "$REPO_ROOT/build/examples"/*/; do
    for bin in "$example_dir"Pulp* "$example_dir"pulp-*; do
        [ -x "$bin" ] && [ ! -d "$bin" ] && echo "    Standalone: $(basename "$bin")"
    done
done

echo ""
echo "  CLI:  $REPO_ROOT/build/tools/cli/pulp"
echo ""
echo "  Next steps:"
echo "    pulp build          # rebuild after changes"
echo "    pulp test           # run tests"
echo "    pulp doctor         # diagnose environment issues"
echo "    pulp validate       # validate plugin formats"
echo ""
