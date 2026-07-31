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
