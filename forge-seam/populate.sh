#!/usr/bin/env bash
# Put the seam INTO a Forge worktree: forge-seam/populate.sh [worktree]
#
# sync.sh runs one way -- worktree back to here -- so rebuilding a worktree was
# a list of steps in a README: apply the patch, copy the sources somewhere, copy
# the tests, remember to register the test target. Steps in a README are steps
# that get skipped, and skipping them fails LATE and quietly: a worktree missing
# the CMake registrations configures three products instead of four, links
# everything it knows about, and reports success. `git apply --check` says
# nothing about any of it, because it only answers "does this apply", never
# "is this everything".
#
# So the recovery is one command, and it verifies rather than assumes.
#
#   git worktree add /tmp/forge-cur "$(cat forge-seam/patches/BASE)"
#   forge-seam/populate.sh /tmp/forge-cur
#
# Idempotent: re-running over a populated worktree re-copies and re-checks.

set -uo pipefail

SEAM="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${1:-/tmp/forge-cur}"

# The shell and its views. Same list sync.sh carries, for the same reason: a
# file missing from it drifts in silence.
MODULAR_FILES=(
    build_monitor mention_overlay modular_shell module_summary
    patch_explanation patch_loader process_engine rack_layout rack_preview
)

die() { echo "populate: $*" >&2; exit 1; }

[ -d "$DEST" ] || die "no worktree at $DEST"
[ -f "$DEST/CMakeLists.txt" ] || die "$DEST does not look like a Forge checkout"

# 1. The chrome changes, which are a diff against a specific commit.
base="$(tr -d '[:space:]' < "$SEAM/patches/BASE" 2>/dev/null)"
[ -n "$base" ] || die "patches/BASE is missing — nothing records which commit
       the chrome patch applies to"
if git -C "$DEST" apply --check "$SEAM/patches/0001-chrome-copy-from-the-shell.patch" 2>/dev/null; then
    git -C "$DEST" apply "$SEAM/patches/0001-chrome-copy-from-the-shell.patch" \
        || die "the chrome patch failed to apply"
    echo "  applied the chrome patch"
else
    # Already applied is fine and common; anything else is not.
    if git -C "$DEST" apply --reverse --check \
           "$SEAM/patches/0001-chrome-copy-from-the-shell.patch" 2>/dev/null; then
        echo "  chrome patch already applied"
    else
        die "the chrome patch neither applies nor is already applied to $DEST.
       It is a diff against $base — check the worktree is at that commit."
    fi
fi

# 2. The shell and its views, header beside header and source beside source.
for stem in "${MODULAR_FILES[@]}"; do
    [ -f "$SEAM/modular/$stem.hpp" ] && cp "$SEAM/modular/$stem.hpp" "$DEST/include/forge/"
    [ -f "$SEAM/modular/$stem.cpp" ] && cp "$SEAM/modular/$stem.cpp" "$DEST/src/"
done
echo "  copied ${#MODULAR_FILES[@]} shell sources"

# 3. The plugin directory: format entry points and its own CMakeLists.
mkdir -p "$DEST/modular"
for f in "$SEAM"/modular/*_entry.cpp "$SEAM"/modular/main.cpp \
         "$SEAM"/modular/CMakeLists.txt; do
    [ -f "$f" ] && cp "$f" "$DEST/modular/"
done
echo "  copied the plugin directory"

# 4. The tests.
mkdir -p "$DEST/test"
cp "$SEAM"/test/* "$DEST/test/" 2>/dev/null
echo "  copied the tests"

# 5. Verify, because every one of these has been silently absent before and
#    the build said nothing. A populate that cannot say what it produced is
#    the README it replaced.
bad=0
for f in "$DEST/src/modular_shell.cpp" "$DEST/modular/CMakeLists.txt" \
         "$DEST/test/test_chrome_no_leak.cpp"; do
    [ -f "$f" ] || { echo "populate: MISSING $f" >&2; bad=1; }
done
grep -q "add_subdirectory(modular)" "$DEST/CMakeLists.txt" \
    || { echo "populate: the worktree does not add modular/ to the build" >&2; bad=1; }
grep -q "modular_shell.cpp" "$DEST/CMakeLists.txt" \
    || { echo "populate: modular sources are not in FORGE_SOURCES — this
       configures and then fails to link" >&2; bad=1; }
grep -q "forge-test-chrome-no-leak" "$DEST/CMakeLists.txt" \
    || { echo "populate: the chrome test target is not registered — the
       worktree will build with no coverage of the shared chrome" >&2; bad=1; }

[ "$bad" -eq 0 ] || die "the worktree is incomplete (see above)"
echo "populated $DEST from the seam"
