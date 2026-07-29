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
    # With pipefail, grep -q exits as soon as it matches and the upstream
    # printf can receive SIGPIPE for a long report, turning a successful match
    # into a failed pipeline. Feed grep directly so this assertion observes
    # only whether the expected text is present.
    elif ! grep -Fq -- "$needle" <<<"$out"; then
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

# `${#arr[@]:-0}` is a bad substitution in bash 5 but silently tolerated by the
# bash 3.2 macOS ships, so this suite passes locally and fails on Linux. Grep for
# it directly — the behavior itself is unreachable from here.
if grep -n '\[@\]:-' "$SCRIPT"; then
    echo "  FAIL  \${#arr[@]:-N} is a bash-5 bad substitution; arrays are initialized, so use \${#arr[@]}"
    fail=$((fail + 1))
else
    printf '  ok    no bash-5-invalid array substitutions\n'
    pass=$((pass + 1))
fi

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

# 7. A real gate failure must fail, even on a Python too old to run every gate.
#    This is the case that matters: an old interpreter must never launder a
#    genuine defect into "inconclusive". Deterministic on any Python.
r="$(make_repo)"
cat > "$r/tools/scripts/gates.sh" <<'GATES'
#!/bin/sh
echo "  deps-audit self-tests: failing"     # version artifact
echo "  ✗ doc: docs/reference/compat/css.md NOT updated"   # real
echo "gates: ✗ one or more gates failed (see above)."
exit 1
GATES
chmod +x "$r/tools/scripts/gates.sh"
printf 'x\n' > "$r/notes.md"
git -C "$r" add -A >/dev/null && git -C "$r" commit -qm gates
check "fails on a real gate problem despite version artifacts" 1 "compat/css.md NOT updated" "$r"

# 8. A run whose problems are ALL version artifacts is inconclusive, not a
#    failure — only meaningful on a Python that cannot run those gates.
if python3 -c 'import sys; sys.exit(0 if sys.version_info < (3,11) else 1)' 2>/dev/null; then
    r="$(make_repo)"
    cat > "$r/tools/scripts/gates.sh" <<'GATES'
#!/bin/sh
echo "  deps-audit self-tests: failing"
echo "gates: ✗ one or more gates failed (see above)."
exit 1
GATES
    chmod +x "$r/tools/scripts/gates.sh"
    printf 'x\n' > "$r/notes.md"
    git -C "$r" add -A >/dev/null && git -C "$r" commit -qm gates
    check "treats version-only gate problems as inconclusive" 0 "known Python 3.11+ artifact" "$r"
else
    printf '  skip  version-artifact case (needs Python < 3.11 to be meaningful)\n'
fi

# 9. With no test target, the script must SAY it ran no tests. Printing "Ready to
#    hand off" while having executed nothing is the loophole this closes.
r="$(make_repo)"
mkdir -p "$r/core/signal/src" "$r/test"
printf 'int f() { return 1; }\n' > "$r/core/signal/src/thing.cpp"
printf 'TEST_CASE("f") {}\n' > "$r/test/test_thing.cpp"
git -C "$r" add -A >/dev/null && git -C "$r" commit -qm "src plus test"
check "states plainly when it ran no tests" 0 "ran NO tests" "$r"

# 10. A real failure whose text merely MENTIONS a version-sensitive word must
#     still fail. The artifact filter errs toward silence, so a loose match here
#     would hide a defect rather than just add noise.
r="$(make_repo)"
cat > "$r/tools/scripts/gates.sh" <<'GATES'
#!/bin/sh
echo "  ✗ config-doc: tomllib parser rejected .shipyard/config.toml"
echo "gates: ✗ one or more gates failed (see above)."
exit 1
GATES
chmod +x "$r/tools/scripts/gates.sh"
printf 'x\n' > "$r/notes.md"
git -C "$r" add -A >/dev/null && git -C "$r" commit -qm gates
check "does not suppress a real failure that mentions tomllib" 1 "config-doc: tomllib parser" "$r"

# 11. An identical branch has nothing to say.
r="$(make_repo)"
check "exits cleanly with no changes" 0 "nothing to check" "$r"

echo ""
if [ "$fail" -ne 0 ]; then
    printf '  %d passed, %d FAILED\n\n' "$pass" "$fail"
    exit 1
fi
printf '  %d passed\n\n' "$pass"
exit 0
