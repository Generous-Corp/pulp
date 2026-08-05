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

# Every file Forge Modular owns -- DERIVED, from the one place that already
# has to be right.
#
# This was a hand-written list, and populate.sh carried a second copy of it.
# They drifted: portmap and module_catalog were added to one and not the other,
# and a rebuilt worktree compiled without two of its headers. Worse, a source
# that exists only in the CHECKOUT is invisible to a list written HERE -- which
# is a one-way door, because /tmp is cleared and the seam is the only copy.
#
# So the list comes from Forge's CMakeLists, where our chrome patch registers
# these sources. A file that is not there is not compiled, so anything real is
# named there by construction, and nothing has to be remembered.
CHECKOUT_CMAKE="$SRC/CMakeLists.txt"
registered=""
if [ -f "$CHECKOUT_CMAKE" ]; then
    registered="$(awk '/foreach\(_forge_modular_src/,/^endforeach/' \
                      "$CHECKOUT_CMAKE" \
                  | grep -oE '[a-z_]+\.cpp' | sed 's/\.cpp$//')"
fi

# Union with what this repo already carries, so a file dropped from the
# registration is still synced rather than quietly abandoned here.
carried=""
for existing in "$SEAM"/modular/*.cpp "$SEAM"/modular/*.hpp; do
    [ -f "$existing" ] || continue
    stem="$(basename "$existing")"; stem="${stem%.*}"
    case "$stem" in main|*_entry) continue ;; esac
    carried="$carried$stem
"
done

MODULAR_FILES=()
while IFS= read -r stem; do
    [ -n "$stem" ] && MODULAR_FILES+=("$stem")
done < <(printf '%s\n%s\n' "$registered" "$carried" | sort -u | grep -v '^$')

[ "${#MODULAR_FILES[@]}" -gt 0 ] || { echo "sync: no modular sources found —
       neither $CHECKOUT_CMAKE nor $SEAM/modular named any" >&2; exit 3; }

# A file this repo carries that the build does NOT register is a file that is
# never compiled. CMake skips a missing source with if(EXISTS) rather than
# failing, so this fails open unless something says so out loud.
if [ -n "$registered" ]; then
    for existing in "$SEAM"/modular/*.cpp; do
        [ -f "$existing" ] || continue
        stem="$(basename "$existing")"; stem="${stem%.*}"
        case "$stem" in main|*_entry) continue ;; esac
        printf '%s\n' "$registered" | grep -qx "$stem" || {
            echo "  NOT BUILT: modular/$stem.cpp is carried here but is not in" >&2
            echo "       the foreach(_forge_modular_src) list in the chrome patch," >&2
            echo "       so CMake skips it silently and it is never compiled." >&2
            exit 3
        }
    done
fi

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

# The baselines, which are written INTO the checkout.
#
# populate.sh copies them out; nothing copied them back. A baseline refreshed
# with FORGE_NO_LEAK_UPDATE=1 therefore landed in /tmp only, and /tmp is
# cleared -- so a deliberate re-record was silently undone, and the guard came
# back red against the image it was supposed to have replaced. Anything the
# checkout writes has to come home the same way anything it edits does.
for baseline in "$SRC"/test/baselines/chrome-home/*; do
    [ -f "$baseline" ] || continue
    name="$(basename "$baseline")"
    mkdir -p "$SEAM/test/baselines/chrome-home"
    report "baselines/chrome-home/$name" "$baseline" \
           "$SEAM/test/baselines/chrome-home/$name"
done
if [ "$CHECK_ONLY" -eq 0 ]; then
    # settings_surface carries the shell-contributed settings rows (the
    # ForgeShell::settings_choices hook), and test_chrome.cpp carries the
    # corrected open-permissions tab index its own assertion had enshrined.
    ( cd "$SRC" && git diff -- include/forge/shell.hpp include/forge/chrome.hpp \
        src/chrome.cpp src/settings_surface.hpp src/settings_surface.cpp \
        test/test_chrome.cpp \
        include/forge/fx_shell.hpp include/forge/instrument_shell.hpp \
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
