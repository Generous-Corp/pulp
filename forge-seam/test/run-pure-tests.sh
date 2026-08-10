#!/usr/bin/env bash
# Run the seam's C++ tests that need nothing but Catch2, without a Forge build.
#
#   forge-seam/test/run-pure-tests.sh
#
# The seam's tests are registered in Forge's CMakeLists and build inside a Forge
# worktree, so "I could not run it here" has been an accepted answer -- and an
# assertion nobody runs is an assertion that can be wrong for months. It was:
# `test_chrome_no_leak.cpp`'s ending-classification case carried a fixture
# string no generator prints any more, so it asserted `progress != progress` and
# failed. Nothing noticed, because that file also needs Skia, ImageIO and
# forge_core, and would not compile standalone for anybody.
#
# Some of those cases need none of that. `classify()`, `outcome_of()`,
# `closing_block()` and `format_failure_report()` are pure functions over
# strings; a test of them needs `build_monitor.cpp` and Catch2 and nothing else.
# This compiles exactly that and runs it, in about ten seconds, from a checkout
# with no Forge worktree at all.
#
# It is NOT a replacement for the Forge build: a test that touches the chrome,
# a view, or the SDK belongs there and is not listed here. It is the answer to
# "this assertion is pure, so why is it unrun".
#
# Catch2 comes from a warm Forge build's FetchContent tree. If none is on this
# machine the script FAILS rather than skipping -- a skip that reports success
# is the defect this exists to prevent.

set -uo pipefail

SEAM="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/forge-seam-pure-tests.$$"

# Every test here compiles against build_monitor.cpp alone. Adding one that
# needs another source means adding that source, not loosening this.
SOURCES=(build_monitor.cpp)
TESTS=(test_build_failure.cpp)

die() { echo "run-pure-tests: $*" >&2; rm -rf "$WORK"; exit 2; }

# A Catch2 that somebody's Forge build already fetched. Newest first, so a
# stale tree is not preferred over a current one.
catch2_root() {
    local best="" newest=0 cache
    for cache in "${FORGE_BUILD_DIRS:-}" /tmp/forge-*/build /Volumes/Workshop/Code/forge*/build; do
        [ -n "$cache" ] || continue
        local deps="$cache/_deps"
        [ -f "$deps/catch2-build/src/libCatch2Main.a" ] || continue
        local when
        when=$(stat -f %m "$deps/catch2-build/src/libCatch2Main.a" 2>/dev/null || echo 0)
        if [ "$when" -gt "$newest" ]; then newest=$when; best=$deps; fi
    done
    [ -n "$best" ] && printf '%s' "$best"
}

C2="$(catch2_root)"
[ -n "$C2" ] || die "no Catch2 found. It comes from a Forge build's _deps tree;
       build Forge once, or point FORGE_BUILD_DIRS at a build directory that
       has one. Not skipping: a skip that exits 0 reads as a pass."

mkdir -p "$WORK/inc/forge" || die "could not create $WORK"
cp "$SEAM"/modular/*.hpp "$WORK/inc/forge/" || die "could not stage the headers"

srcs=()
for f in "${SOURCES[@]}"; do
    [ -f "$SEAM/modular/$f" ] || die "missing source $f"
    srcs+=("$SEAM/modular/$f")
done
for f in "${TESTS[@]}"; do
    [ -f "$SEAM/test/$f" ] || die "missing test $f"
    srcs+=("$SEAM/test/$f")
done

echo "run-pure-tests: Catch2 from $C2"
clang++ -std=c++20 -O1 -Wall -Wextra \
    -I"$WORK/inc" \
    -I"$C2/catch2-src/src" -I"$C2/catch2-build/generated-includes" \
    "${srcs[@]}" \
    "$C2/catch2-build/src/libCatch2Main.a" "$C2/catch2-build/src/libCatch2.a" \
    -o "$WORK/pure" || die "the pure tests did not compile"

# The binary's exit status is the RESULT. Piping it through anything, or
# letting the cleanup below be the last command, reports the wrong thing.
"$WORK/pure" "$@"
ec=$?
rm -rf "$WORK"
exit $ec
