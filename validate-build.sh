#!/usr/bin/env bash
# validate-build.sh — clean outer-loop build validation with quiet-on-success output

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QUIET=true
SKIP_TESTS=false
SMOKE_ONLY=false
KEEP_WORKTREE=false
NO_LOCK=false
REF="HEAD"
EXCLUDE_REGEX=""
EXPECT_SMOKE="${PULP_EXPECT_SMOKE:-0}"
REUSE_PREPARED="${PULP_VALIDATE_REUSE_PREPARED:-0}"
ROOT_OVERRIDE="${PULP_VALIDATE_ROOT_OVERRIDE:-}"
ORIGINAL_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --quiet) QUIET=true ;;
        --verbose) QUIET=false ;;
        --no-tests) SKIP_TESTS=true ;;
        --smoke)
            SMOKE_ONLY=true
            SKIP_TESTS=true
            ;;
        --keep-worktree) KEEP_WORKTREE=true ;;
        --exclude-regex)
            shift
            EXCLUDE_REGEX="${1:-}"
            ;;
        --exclude-regex=*)
            EXCLUDE_REGEX="${1#--exclude-regex=}"
            ;;
        --ref)
            shift
            REF="${1:-HEAD}"
            ;;
        --ref=*)
            REF="${1#--ref=}"
            ;;
        --no-lock) NO_LOCK=true ;;
        --help|-h)
            cat <<'EOF'
Usage: ./validate-build.sh [--quiet] [--verbose] [--no-tests] [--smoke] [--keep-worktree] [--no-lock] [--ref <git-ref>] [--exclude-regex <pattern>]

Creates a detached clean worktree at the requested git ref (default: current HEAD),
bootstraps dependencies, configures, builds, installs, and optionally runs tests. Output is quiet on success
by default and prints logs only on failure. Use --verbose to print progress messages.
By default the script also takes a per-host validation lock so concurrent agents wait instead of colliding.
Use --smoke for a fast install/export preflight: it disables tests/examples/GPU in the clean build,
still installs the SDK, and runs the installed-SDK find_package(Pulp) smoke configure.
EOF
            exit 0
            ;;
    esac
    shift
done

if [ "$EXPECT_SMOKE" = "1" ] && [ "$SMOKE_ONLY" != true ]; then
    echo "Smoke validation contract violated: expected --smoke to be active" >&2
    exit 2
fi

if [ "$EXPECT_SMOKE" = "1" ] && [ "$SKIP_TESTS" != true ]; then
    echo "Smoke validation contract violated: tests must be skipped" >&2
    exit 2
fi

validation_lock_path() {
    if [ -n "${PULP_VALIDATE_LOCK_PATH_OVERRIDE:-}" ]; then
        printf '%s\n' "$PULP_VALIDATE_LOCK_PATH_OVERRIDE"
        return
    fi

    case "$(uname -s)" in
        Darwin)
            if [ -n "${HOME:-}" ]; then
                mkdir -p "$HOME/Library/Application Support/Pulp/local-ci"
                printf '%s\n' "$HOME/Library/Application Support/Pulp/local-ci/host-validate.lock"
                return
            fi
            ;;
        *)
            if [ -n "${XDG_STATE_HOME:-}" ]; then
                mkdir -p "${XDG_STATE_HOME}/pulp/local-ci"
                printf '%s\n' "${XDG_STATE_HOME}/pulp/local-ci/host-validate.lock"
                return
            fi
            if [ -n "${HOME:-}" ]; then
                mkdir -p "$HOME/.local/state/pulp/local-ci"
                printf '%s\n' "$HOME/.local/state/pulp/local-ci/host-validate.lock"
                return
            fi
            ;;
    esac

    printf '%s\n' "${TMPDIR:-/tmp}/pulp-host-validate.lock"
}

acquire_validation_lock() {
    if [ "$NO_LOCK" = true ] || [ "${PULP_VALIDATE_NO_LOCK:-0}" = "1" ] || [ -n "${PULP_VALIDATE_LOCK_HELD:-}" ]; then
        return 0
    fi

    if ! command -v python3 >/dev/null 2>&1; then
        echo "warning: python3 not found; continuing without host validation lock" >&2
        return 0
    fi

    local lock_path
    lock_path="$(validation_lock_path)"

    exec python3 - "$lock_path" "$0" "$@" <<'PY'
import fcntl
import os
import sys

lock_path = sys.argv[1]
cmd = sys.argv[2:]

fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o644)
flags = fcntl.fcntl(fd, fcntl.F_GETFD)
fcntl.fcntl(fd, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)

try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    sys.stderr.write(f"Waiting for host validation lock: {lock_path}\n")
    sys.stderr.flush()
    fcntl.flock(fd, fcntl.LOCK_EX)

env = os.environ.copy()
env["PULP_VALIDATE_LOCK_HELD"] = "1"
env["PULP_VALIDATE_LOCK_PATH"] = lock_path
env["PULP_VALIDATE_LOCK_FD"] = str(fd)
os.execvpe(cmd[0], cmd, env)
PY
}

acquire_validation_lock "${ORIGINAL_ARGS[@]}"

if [ -n "$ROOT_OVERRIDE" ]; then
    tmp_root="$ROOT_OVERRIDE"
else
    tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/pulp-validate.XXXXXX")"
fi
src_dir="$tmp_root/src"
build_dir="$tmp_root/build"
setup_log="$tmp_root/setup.log"
configure_log="$tmp_root/configure.log"
build_log="$tmp_root/build.log"
install_dir="$tmp_root/install"
install_log="$tmp_root/install.log"
smoke_dir="$tmp_root/sdk-smoke"
smoke_log="$tmp_root/smoke.log"
smoke_build_log="$tmp_root/smoke-build.log"
test_log="$tmp_root/test.log"
prepared_state_file="$tmp_root/prepared-state.txt"

cleanup() {
    if [ "$KEEP_WORKTREE" = true ]; then
        echo "Keeping validation worktree at $src_dir"
        return
    fi
    git -C "$ROOT" worktree remove --force "$src_dir" >/dev/null 2>&1 || true
    rm -rf "$tmp_root"
}
trap cleanup EXIT

validation_mode="full"
if [ "$SMOKE_ONLY" = true ]; then
    validation_mode="smoke"
fi

prepared_state_matches() {
    [ -f "$prepared_state_file" ] || return 1
    [ -d "$src_dir" ] || return 1
    [ -d "$build_dir" ] || return 1
    [ -d "$install_dir" ] || return 1

    local stored_ref stored_validation current_ref
    stored_ref="$(sed -n '1p' "$prepared_state_file" 2>/dev/null || true)"
    stored_validation="$(sed -n '2p' "$prepared_state_file" 2>/dev/null || true)"
    current_ref="$(git -C "$src_dir" rev-parse HEAD 2>/dev/null || true)"

    [ "$stored_ref" = "$REF" ] &&
        [ "$stored_validation" = "$validation_mode" ] &&
        [ "$current_ref" = "$REF" ]
}

reset_prepared_root() {
    git -C "$ROOT" worktree remove --force "$src_dir" >/dev/null 2>&1 || true
    rm -rf "$tmp_root"
    mkdir -p "$tmp_root"
}

write_prepared_state() {
    mkdir -p "$tmp_root"
    printf '%s\n%s\n' "$REF" "$validation_mode" >"$prepared_state_file"
}

if ! $QUIET; then
    echo "Creating clean validation worktree..."
fi
if [ "$SMOKE_ONLY" = true ]; then
    echo "__PULP_VALIDATION__:smoke"
else
    echo "__PULP_VALIDATION__:full"
fi
if [ "$SKIP_TESTS" = true ]; then
    echo "__PULP_TEST_POLICY__:skip"
else
    echo "__PULP_TEST_POLICY__:run"
fi
if [ "$REUSE_PREPARED" = "1" ] && prepared_state_matches; then
    echo "__PULP_PREPARED__:reused"
else
    echo "__PULP_PREPARED__:clean"
    reset_prepared_root
    git -C "$ROOT" worktree add --detach "$src_dir" "$REF" >/dev/null
fi

run_or_dump() {
    local label="$1"
    local logfile="$2"
    shift 2
    if "$@" >"$logfile" 2>&1; then
        return 0
    fi
    echo ""
    echo "Validation failed during: $label"
    echo "---- $label log ----"
    cat "$logfile"
    exit 1
}

run_or_dump "dependency bootstrap" "$setup_log" bash -lc "cd \"$src_dir\" && ./setup.sh --ci --deps-only"
configure_args=(-S "$src_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug)
if [ "$SMOKE_ONLY" = true ]; then
    SKIP_TESTS=true
    configure_args+=(-DPULP_BUILD_TESTS=OFF -DPULP_BUILD_EXAMPLES=OFF)
fi
run_or_dump "configure" "$configure_log" cmake "${configure_args[@]}"
run_or_dump "build" "$build_log" cmake --build "$build_dir" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
run_or_dump "install" "$install_log" cmake --install "$build_dir" --prefix "$install_dir"

mkdir -p "$smoke_dir"
run_or_dump "install smoke configure" "$smoke_log" \
    cmake -S "$src_dir/tools/validation/sdk-smoke" -B "$smoke_dir/build" -DCMAKE_PREFIX_PATH="$install_dir"
run_or_dump "install smoke build" "$smoke_build_log" \
    cmake --build "$smoke_dir/build" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

write_prepared_state

if [ "$SKIP_TESTS" = false ]; then
    if [ "$SMOKE_ONLY" = true ]; then
        echo "Smoke validation contract violated: refusing to run ctest" >&2
        exit 2
    fi
    ctest_args=(--test-dir "$build_dir" --output-on-failure)
    if [ -n "$EXCLUDE_REGEX" ]; then
        ctest_args+=(--exclude-regex "$EXCLUDE_REGEX")
    fi
    run_or_dump "test" "$test_log" ctest "${ctest_args[@]}"
fi

if ! $QUIET; then
    echo "Clean validation passed in $build_dir"
fi
