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

printf '\n%s\n' "$([ "$bad" -eq 0 ] && echo 'all good' || echo FAILED)"
[ "$bad" -eq 0 ]
