#!/usr/bin/env bash
# Copy the Forge Modular sources out of the disposable Forge checkout and into
# this repo, where they are durable.
#
# The shell is built inside a Forge worktree because it compiles against Forge's
# chrome, and that worktree lives under /tmp -- which macOS clears, and which
# another agent may rebuild from scratch at any time. Everything Forge Modular
# owns therefore has to be copied back here to survive. Run this at the end of
# any session that touched the shell; it is cheap and idempotent.
#
#   forge-seam/sync.sh            copy, then report what changed
#   forge-seam/sync.sh --check    report only, exit 1 if anything drifted
#
# The check mode is what a pre-commit or a session close can call: it answers
# "is anything living only in /tmp right now?" without touching a file.

set -euo pipefail

SEAM="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${FORGE_CHECKOUT:-/tmp/forge-cur}"
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

if [ ! -d "$SRC" ]; then
    echo "forge checkout not found at $SRC" >&2
    echo "set FORGE_CHECKOUT to point at it" >&2
    exit 2
fi

# Every file Forge Modular owns. A file that exists in the checkout but is not
# named here is NOT copied -- that is deliberate, because the checkout also
# holds Forge's own sources, which this repo must never carry.
MODULAR_FILES=(
    build_monitor mention_overlay modular_shell module_summary
    patch_explanation patch_loader module_catalog portmap process_engine rack_layout rack_preview
)

# A file this repo carries that the list above forgets is a file that drifts in
# silence -- it is here, it looks synced, and nothing ever copies it.
# module_summary was exactly that for as long as it existed.
for existing in "$SEAM"/modular/*.cpp "$SEAM"/modular/*.hpp; do
    stem="$(basename "$existing")"; stem="${stem%.*}"
    listed=0
    for known in "${MODULAR_FILES[@]}"; do
        [ "$known" = "$stem" ] && listed=1 && break
    done
    # The format entry points and the standalone main live in their own
    # subdirectories in the checkout, so they are not stems of this list.
    case "$stem" in main|*_entry) listed=1 ;; esac
    if [ "$listed" -eq 0 ]; then
        echo "  NOT SYNCED: modular/$stem — add it to MODULAR_FILES" >&2
        exit 3
    fi
done

drift=0
report() { # <label> <src> <dst>
    if [ ! -f "$2" ]; then return; fi
    if ! diff -q "$2" "$3" >/dev/null 2>&1; then
        drift=$((drift + 1))
        echo "  drifted: $1"
        [ "$CHECK_ONLY" -eq 1 ] || cp "$2" "$3"
    fi
}

for stem in "${MODULAR_FILES[@]}"; do
    report "$stem.hpp" "$SRC/include/forge/$stem.hpp" "$SEAM/modular/$stem.hpp"
    report "$stem.cpp" "$SRC/src/$stem.cpp"           "$SEAM/modular/$stem.cpp"
done

# The seam itself: the no-leak guard and the shell's virtuals live in Forge's
# own files, so they travel as a patch rather than as copies.
if [ -f "$SRC/test/test_chrome_no_leak.cpp" ]; then
    report "test_chrome_no_leak.cpp" "$SRC/test/test_chrome_no_leak.cpp" \
           "$SEAM/test/test_chrome_no_leak.cpp"
fi
if [ "$CHECK_ONLY" -eq 0 ]; then
    ( cd "$SRC" && git diff -- include/forge/shell.hpp include/forge/chrome.hpp \
        src/chrome.cpp include/forge/fx_shell.hpp include/forge/instrument_shell.hpp \
        include/forge/midi_shell.hpp CMakeLists.txt ) \
        > "$SEAM/patches/0001-chrome-copy-from-the-shell.patch" || true
    # The commit the patch is a diff AGAINST, recorded beside it.
    #
    # The resume note said `git worktree add /tmp/forge-cur origin/main`, and
    # Forge's main moves. Once it had, the patch no longer applied there --
    # CMakeLists.txt, chrome.hpp and chrome.cpp all rejected -- and the
    # instruction gave no way to find the base it WAS built from. A patch
    # whose base is a moving reference has a shelf life nobody can see.
    ( cd "$SRC" && git rev-parse HEAD ) > "$SEAM/patches/BASE" || true
fi

if [ "$drift" -eq 0 ]; then
    echo "seam is current with $SRC"
    exit 0
fi

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "$drift file(s) live only in $SRC -- run forge-seam/sync.sh" >&2
    exit 1
fi
echo "synced $drift file(s) from $SRC"
