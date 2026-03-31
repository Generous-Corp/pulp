#!/usr/bin/env bash
# validate-build.sh — clean outer-loop build validation with quiet-on-success output

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QUIET=true
SKIP_TESTS=false
KEEP_WORKTREE=false
REF="HEAD"

while [ $# -gt 0 ]; do
    case "$1" in
        --quiet) QUIET=true ;;
        --verbose) QUIET=false ;;
        --no-tests) SKIP_TESTS=true ;;
        --keep-worktree) KEEP_WORKTREE=true ;;
        --ref)
            shift
            REF="${1:-HEAD}"
            ;;
        --ref=*)
            REF="${1#--ref=}"
            ;;
        --help|-h)
            cat <<'EOF'
Usage: ./validate-build.sh [--quiet] [--verbose] [--no-tests] [--keep-worktree] [--ref <git-ref>]

Creates a detached clean worktree at the requested git ref (default: current HEAD),
bootstraps dependencies, configures, builds, installs, and optionally runs tests. Output is quiet on success
by default and prints logs only on failure. Use --verbose to print progress messages.
EOF
            exit 0
            ;;
    esac
    shift
done

tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/pulp-validate.XXXXXX")"
src_dir="$tmp_root/src"
build_dir="$tmp_root/build"
setup_log="$tmp_root/setup.log"
configure_log="$tmp_root/configure.log"
build_log="$tmp_root/build.log"
install_dir="$tmp_root/install"
install_log="$tmp_root/install.log"
smoke_dir="$tmp_root/sdk-smoke"
smoke_log="$tmp_root/smoke.log"
test_log="$tmp_root/test.log"

cleanup() {
    if [ "$KEEP_WORKTREE" = true ]; then
        echo "Keeping validation worktree at $src_dir"
        return
    fi
    git -C "$ROOT" worktree remove --force "$src_dir" >/dev/null 2>&1 || true
    rm -rf "$tmp_root"
}
trap cleanup EXIT

if ! $QUIET; then
    echo "Creating clean validation worktree..."
fi
git -C "$ROOT" worktree add --detach "$src_dir" "$REF" >/dev/null

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
run_or_dump "configure" "$configure_log" cmake -S "$src_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug
run_or_dump "build" "$build_log" cmake --build "$build_dir" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
run_or_dump "install" "$install_log" cmake --install "$build_dir" --prefix "$install_dir"

mkdir -p "$smoke_dir"
cat >"$smoke_dir/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(PulpSDKSmoke LANGUAGES CXX)

find_package(Pulp REQUIRED CONFIG)

add_library(smoke INTERFACE)
target_link_libraries(smoke INTERFACE Pulp::format)
EOF

run_or_dump "install smoke test" "$smoke_log" \
    cmake -S "$smoke_dir" -B "$smoke_dir/build" -DCMAKE_PREFIX_PATH="$install_dir"

if [ "$SKIP_TESTS" = false ]; then
    run_or_dump "test" "$test_log" ctest --test-dir "$build_dir" --output-on-failure
fi

if ! $QUIET; then
    echo "Clean validation passed in $build_dir"
fi
