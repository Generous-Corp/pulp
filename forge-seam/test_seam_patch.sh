#!/usr/bin/env bash
# Can the seam actually be rebuilt from what is checked in?
#
# The recovery procedure said to branch from `origin/main`. Forge's main
# moves, and once it had, the generated chrome patch no longer applied there
# -- CMakeLists.txt, chrome.hpp and chrome.cpp all rejected -- with nothing
# recorded to say which commit it HAD been built from. The seam looked
# complete and was unrecoverable.
#
# `git apply --check` is the whole test, but it must not be piped: piping puts
# the pipeline's exit status on the last command, so `git apply --check … |
# head` reports success no matter what git said. That mistake is how this
# problem was nearly missed.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH="$HERE/patches/0001-chrome-copy-from-the-shell.patch"
BASE_FILE="$HERE/patches/BASE"
bad=0
ok()    { printf '  ok     %s\n' "$*"; }
wrong() { printf '  WRONG  %s\n' "$*"; bad=$((bad + 1)); }

[ -f "$PATCH" ] && ok "the chrome patch is checked in" \
                || wrong "no chrome patch — the seam cannot be rebuilt"

# Applying cleanly is not the same as being complete. The patch registered
# every OTHER product and never `add_subdirectory(modular)`, so a rebuilt
# worktree configured happily with ForgeFx, ForgeInstrument and ForgeMidi and
# no Forge Modular at all -- and `git apply --check` was perfectly happy about
# it. That step lived only in whoever set the worktree up.
grep -q "add_subdirectory(modular)" "$PATCH" \
    && ok "the patch registers the modular plugin" \
    || wrong "the patch never adds modular/ to the build — a worktree rebuilt
         from it configures without Forge Modular, and applies cleanly while
         doing so"

# Registering the subdirectory is only half. The plugin builds ${FORGE_SOURCES},
# so the shell and its views have to be IN that list: with the subdirectory
# added and the sources missing, the build configures, compiles, and fails at
# link with "typeinfo for forge_modular::ForgeModularShell" -- the shell the
# product is made of, absent from the library the product links. Checking only
# the subdirectory would have called that seam complete.
# Matched on the FACT, not a spelling. The first version of this looked for
# the literal "src/modular_shell.cpp" and failed against a patch that lists
# the same files through a loop over bare names -- a test asserting how
# something is written rather than whether it is there.
missing_src=""
for src in modular_shell rack_preview patch_explanation rack_layout; do
    grep -q "$src\.cpp" "$PATCH" || missing_src="$missing_src $src.cpp"
done
if [ -n "$missing_src" ]; then
    wrong "the patch never adds these to FORGE_SOURCES:$missing_src — the
         modular plugin will configure and then fail to link"
elif ! grep -q "FORGE_SOURCES" "$PATCH"; then
    wrong "the sources are named but never appended to FORGE_SOURCES"
else
    ok "the patch adds the modular sources to the build"
fi

# And the test target. It was a step in a README -- "register the no-leak test
# in CMakeLists.txt" -- which a rebuilt worktree simply does not do, leaving no
# coverage of the shared chrome at all while everything still built. The
# rebuild that exposed this configured, compiled and linked, and only the
# missing binary gave it away.
grep -q "forge-test-chrome-no-leak" "$PATCH" \
    && ok "the patch registers the chrome test target" \
    || wrong "the patch never registers forge-test-chrome-no-leak — a worktree
         rebuilt from it has no chrome coverage, and nothing about the build
         says so"

if [ ! -f "$BASE_FILE" ]; then
    wrong "no patches/BASE — nothing records which commit the patch applies to,
         so the only way to find it is trial and error"
else
    base="$(tr -d '[:space:]' < "$BASE_FILE")"
    case "$base" in
        [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*)
            ok "patches/BASE names a commit ($(printf '%.10s' "$base"))" ;;
        *) wrong "patches/BASE is not a commit id: '$base'" ;;
    esac

    # Only meaningful where the Forge checkout exists. A missing checkout is a
    # skip and says so; it is never counted as a pass.
    SRC="${FORGE_SRC:-/tmp/forge-cur}"
    if [ ! -d "$SRC/.git" ] && [ ! -f "$SRC/.git" ]; then
        printf '  skip   no Forge checkout at %s — cannot verify the patch
         applies. This is a skip, not a pass.\n' "$SRC"
    elif ! git -C "$SRC" cat-file -e "$base^{commit}" 2>/dev/null; then
        wrong "the commit in patches/BASE is not in $SRC — the recorded base
         does not exist, so the patch cannot be applied to it"
    else
        W="$(mktemp -d)/w"
        git -C "$SRC" worktree add -q --detach "$W" "$base" 2>/dev/null
        # NOT piped, deliberately: see the note at the top.
        git -C "$W" apply --check "$PATCH" 2>/dev/null
        rc=$?
        git -C "$SRC" worktree remove --force "$W" 2>/dev/null
        [ "$rc" -eq 0 ] \
            && ok "the patch applies cleanly to the commit BASE names" \
            || wrong "the patch does NOT apply to its own recorded base
         (git apply --check exit $rc) — the seam is not recoverable"
    fi
fi

# ---------------------------------------------------------------------------
# The two scripts must not disagree about which files the seam owns.
#
# They did. populate.sh and sync.sh each carried a hand-written list, portmap
# and module_catalog were added to one only, and a rebuilt worktree failed to
# compile with "file not found". Both lists are now derived, and these check
# that the derivation actually covers the cases the lists got wrong.

# Both are checked by RUNNING them over a file they have never heard of.
#
# Grepping the scripts for a hardcoded list was the first attempt, and it was
# unfailable: removing sync.sh's derivation left the text still matching, and
# the check reported ok on a script that had just been broken. A test that
# passes on the broken thing tests nothing. So each script is handed a novel
# file and asked to carry it.

# populate.sh: a source the seam carries must land in the worktree, whether or
# not anything ever named it.
# populate.sh needs a real Forge worktree -- it applies the chrome patch first
# and stops if that fails -- so this probes the checkout rather than a stub. A
# stub dest made this report a failure that was really "not a git repo", which
# is worse than no test.
POP_DEST="${FORGE_SRC:-/tmp/forge-cur}"
if [ ! -d "$POP_DEST/src" ]; then
    printf '  skip   no Forge checkout at %s — cannot verify populate copies
         an unnamed source. This is a skip, not a pass.\n' "$POP_DEST"
else
    echo "// novel" > "$HERE/modular/zz_probe.cpp"
    echo "// novel" > "$HERE/modular/zz_probe.hpp"
    bash "$HERE/populate.sh" "$POP_DEST" >/dev/null 2>&1
    if [ -f "$POP_DEST/src/zz_probe.cpp" ] \
       && [ -f "$POP_DEST/include/forge/zz_probe.hpp" ]; then
        ok "populate.sh copies a source no list has ever named"
    else
        wrong "populate.sh did not copy zz_probe — it is naming files rather
         than reading them, which is the drift that broke the last rebuild"
    fi
    rm -f "$HERE/modular/zz_probe.cpp" "$HERE/modular/zz_probe.hpp" \
          "$POP_DEST/src/zz_probe.cpp" "$POP_DEST/include/forge/zz_probe.hpp"
fi

# sync.sh: a source that exists only in the CHECKOUT must come back here. This
# is the one-way door -- /tmp is cleared, and a file the list forgets is gone.
tmp_src="$(mktemp -d)/c"
mkdir -p "$tmp_src/src" "$tmp_src/include/forge"
# The fixture registers everything the seam really carries, plus the probe.
# Registering only the probe would trip the not-built guard on every real
# source, and the test would fail for a reason it is not about.
{
    echo "foreach(_forge_modular_src"
    for f in "$HERE"/modular/*.cpp; do
        b="$(basename "$f")"
        case "$b" in main.cpp|*_entry.cpp) continue ;; esac
        echo "    $b"
    done
    echo "    zz_probe.cpp)"
    echo "endforeach()"
} > "$tmp_src/CMakeLists.txt"
echo "// born in the checkout" > "$tmp_src/src/zz_probe.cpp"
sync_out="$(FORGE_CHECKOUT="$tmp_src" bash "$HERE/sync.sh" --check 2>&1)"
case "$sync_out" in
    *zz_probe*) ok "sync.sh brings back a source that exists only in the checkout" ;;
    *) wrong "sync.sh ignored zz_probe.cpp — a file created in /tmp would be
         lost when /tmp is cleared, which has already happened once
         (got: $sync_out)" ;;
esac
rm -rf "$(dirname "$tmp_src")"

# Every source the seam carries must be registered in the chrome patch, or
# CMake's if(EXISTS) skips it in silence and it is never compiled.
unbuilt=""
for f in "$HERE"/modular/*.cpp; do
    [ -f "$f" ] || continue
    b="$(basename "$f")"; stem="${b%.cpp}"
    case "$stem" in main|*_entry) continue ;; esac
    grep -q "$stem\.cpp" "$PATCH" || unbuilt="$unbuilt $stem"
done
[ -z "$unbuilt" ] \
    && ok "every carried source is registered in the chrome patch" \
    || wrong "carried but never compiled (CMake skips a missing source
         silently, so this fails open):$unbuilt"

# ---------------------------------------------------------------------------
# The scanner and the reader must agree on what a scan records.
#
# CARTOG writes "scan": N; PortMap::kScanVersion is what the reader expects.
# They live in different products -- a Rack plugin and the app -- so nothing
# links them, and a scanner taught to measure something new without bumping
# both would keep reporting entries as fully measured while they are not.
# That is the failure this whole field exists to catch, so it must not be
# possible to reintroduce it in the mechanism itself.
CARTOG_SRC="$(dirname "$HERE")/examples/forge-modular/src/CARTOG.cpp"
PORTMAP_HDR="$HERE/modular/portmap.hpp"
if [ ! -f "$CARTOG_SRC" ] || [ ! -f "$PORTMAP_HDR" ]; then
    printf '  skip   CARTOG.cpp or portmap.hpp not found — cannot compare the
         scan version. This is a skip, not a pass.\n'
else
    # The scanner writes its JSON from C++ string literals, so the quotes in
    # the source are escaped -- matching on a bare "scan" finds nothing and
    # reports the field as missing, which looks identical to it being missing.
    written="$(grep -oE '\\?"scan\\?": *[0-9]+' "$CARTOG_SRC" \
                | grep -oE '[0-9]+' | head -1)"
    expected="$(grep -oE 'kScanVersion *= *[0-9]+' "$PORTMAP_HDR" \
                | grep -oE '[0-9]+' | head -1)"
    if [ -z "$written" ] || [ -z "$expected" ]; then
        wrong "could not find the scan version in CARTOG.cpp ('$written') or
         portmap.hpp ('$expected') — one of them stopped declaring it"
    elif [ "$written" != "$expected" ]; then
        wrong "CARTOG writes scan $written but the reader expects $expected —
         every entry the new scanner writes would read as stale, or worse, an
         older entry would read as current"
    else
        ok "the scanner and the port-map reader agree on scan version $written"
    fi
fi

printf '\n%s\n' "$([ "$bad" -eq 0 ] && echo 'all good' || echo FAILED)"
[ "$bad" -eq 0 ]
