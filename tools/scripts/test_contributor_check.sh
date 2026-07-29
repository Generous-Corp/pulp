#!/usr/bin/env bash
# Self-tests for contributor_check.sh.
#
# Each case builds a throwaway git repo containing only the script, so the
# assertions are about the script's logic and never about the state of this
# checkout. Sections that need repo tooling (gates.sh, coverage) are absent
# there and take their SKIP path, which is itself part of what is verified:
# a missing optional tool must not turn into a failure.
#
#   tools/scripts/test_contributor_check.sh
#
# Runs under bash 3.2 (macOS default) — no mapfile, no associative arrays.

set -uo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/contributor_check.sh"
[ -x "$SCRIPT" ] || { echo "not executable: $SCRIPT" >&2; exit 1; }

pass=0
fail=0

# Build a repo whose HEAD differs from `main` by whatever the caller stages.
# Echoes the repo path.
make_repo() {
    local d
    d="$(mktemp -d)"
    mkdir -p "$d/tools/scripts"
    cp "$SCRIPT" "$d/tools/scripts/contributor_check.sh"
    git -C "$d" init -q -b main
    git -C "$d" config user.email t@example.com
    git -C "$d" config user.name test
    echo "seed" > "$d/README.md"
    git -C "$d" add -A >/dev/null
    git -C "$d" commit -qm seed
    git -C "$d" checkout -q -b work
    echo "$d"
}

# check <name> <expected-exit> <expected-substring> <repo>
check() {
    local name="$1" want="$2" needle="$3" repo="$4" out rc
    out="$(cd "$repo" && BASE=main /bin/bash tools/scripts/contributor_check.sh 2>&1)"
    rc=$?
    if [ "$rc" -ne "$want" ]; then
        printf '  FAIL  %s\n        expected exit %s, got %s\n' "$name" "$want" "$rc"
        echo "$out" | sed 's/^/        | /'
        fail=$((fail + 1))
    elif ! printf '%s' "$out" | grep -q "$needle"; then
        printf '  FAIL  %s\n        output missing: %s\n' "$name" "$needle"
        echo "$out" | sed 's/^/        | /'
        fail=$((fail + 1))
    else
        printf '  ok    %s\n' "$name"
        pass=$((pass + 1))
    fi
    rm -rf "$repo"
}

echo ""
echo "contributor_check.sh self-tests"

# 1. A version bump must be rejected — version-at-land assigns these post-merge,
#    so a contributor-side bump only produces a conflict.
r="$(make_repo)"
printf 'project(x VERSION 1.2.3)\n' > "$r/CMakeLists.txt"
git -C "$r" add -A >/dev/null && git -C "$r" commit -qm "bump"
check "rejects a VERSION bump" 1 "version/changelog edits detected" "$r"

# 2. Shipped source without a test must be rejected.
r="$(make_repo)"
mkdir -p "$r/core/signal/src"
printf 'int f() { return 1; }\n' > "$r/core/signal/src/thing.cpp"
git -C "$r" add -A >/dev/null && git -C "$r" commit -qm "src only"
check "rejects core/ change with no test" 1 "no test change" "$r"

# 3. The same change WITH a test must pass. This is the control: without it,
#    case 2 would also pass a script that simply always failed.
r="$(make_repo)"
mkdir -p "$r/core/signal/src" "$r/test"
printf 'int f() { return 1; }\n' > "$r/core/signal/src/thing.cpp"
printf 'TEST_CASE("f") {}\n' > "$r/test/test_thing.cpp"
git -C "$r" add -A >/dev/null && git -C "$r" commit -qm "src plus test"
check "accepts core/ change with a test" 0 "source and tests both changed" "$r"

# 4. Docs-only work is legitimate and must not be asked for a test.
r="$(make_repo)"
printf 'hello\n' > "$r/docs.md"
git -C "$r" add -A >/dev/null && git -C "$r" commit -qm docs
check "accepts a docs-only change" 0 "no shipped-source changes" "$r"

# 5. An oversized file warns but does not block — a large generated or data
#    file is legitimate; the point is to make the author justify it.
r="$(make_repo)"
mkdir -p "$r/core/signal/src" "$r/test"
awk 'BEGIN { for (i = 0; i < 1200; i++) print "// line" }' > "$r/core/signal/src/big.cpp"
printf 'TEST_CASE("big") {}\n' > "$r/test/test_big.cpp"
git -C "$r" add -A >/dev/null && git -C "$r" commit -qm big
check "warns on a >1000 line file without blocking" 0 "over ~1000" "$r"

# 6. Coverage must not run a multi-minute build for a diff with no C/C++ in it.
#    A check that appears to hang is a check people learn to interrupt.
r="$(make_repo)"
printf 'hello\n' > "$r/notes.md"
git -C "$r" add -A >/dev/null && git -C "$r" commit -qm notes
check "skips coverage when no C/C++ changed" 0 "coverage not applicable" "$r"

# 7. An identical branch has nothing to say.
r="$(make_repo)"
check "exits cleanly with no changes" 0 "nothing to check" "$r"

echo ""
if [ "$fail" -ne 0 ]; then
    printf '  %d passed, %d FAILED\n\n' "$pass" "$fail"
    exit 1
fi
printf '  %d passed\n\n' "$pass"
exit 0
