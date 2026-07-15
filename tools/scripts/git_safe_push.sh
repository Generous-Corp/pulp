#!/usr/bin/env bash
# git_safe_push.sh — push a branch WITHOUT the shared-worktree footguns.
#
# This repo shares one .git across 100+ worktrees. In that topology, plain
# `git push origin <branch>` has two silent failure modes that cost a full
# session on 2026-07-15 (see planning/friction/2026-07-15-git-state-in-shared-
# worktree-hell.md):
#
#   1. NO-OP PUSH. `git checkout -B <branch>` can leave HEAD DETACHED while the
#      branch ref lags behind your commits. `git push origin <branch>` then
#      pushes the stale branch ref, finds it already matches the remote, and
#      reports "Everything up-to-date" while pushing NOTHING. A pre-push hook is
#      structurally blind to this — a no-op push never fires it.
#   2. STALE REASONING. Without a fresh fetch, behind/ahead and --force-with-lease
#      logic runs against an out-of-date origin ref.
#
# This wrapper makes the safe path the default, the way `shipyard pr` does for
# the ship flow:
#   - refuse on detached HEAD (push what a branch points at, not an anonymous HEAD)
#   - fetch the target ref first
#   - always push HEAD:<branch> (what you actually built), never a bare name
#   - verify remote == local HEAD afterward, and FAIL LOUD if not
#
# Usage:
#   tools/scripts/git_safe_push.sh [<branch>] [-- <extra git push args>]
#   # <branch> defaults to the current branch. Extra args (e.g. --force-with-lease)
#   # pass through to git push.
set -euo pipefail

remote="origin"
branch=""
extra=()
while [ $# -gt 0 ]; do
  case "$1" in
    --) shift; extra=("$@"); break ;;
    *)  [ -z "$branch" ] && branch="$1" || extra+=("$1"); shift ;;
  esac
done

# 1. HEAD must be attached — a detached HEAD is the root of the no-op-push trap.
current="$(git symbolic-ref -q --short HEAD || true)"
if [ -z "$current" ]; then
  echo "git-safe-push: HEAD is DETACHED — refusing." >&2
  echo "  Your commits are on an anonymous HEAD ($(git rev-parse --short HEAD))." >&2
  echo "  In a shared-worktree repo this is how a push silently no-ops." >&2
  echo "  Attach first:  git switch -c <branch>   (or  git branch -f <branch> HEAD && git switch <branch>)" >&2
  exit 1
fi
branch="${branch:-$current}"

# 2. Fresh view of the target ref before any reasoning or lease.
git fetch --quiet "$remote" "$branch" 2>/dev/null || true

# 3. Push what you actually built: HEAD -> <branch>. Never a bare branch name,
#    which can resolve to a lagging ref.
head_sha="$(git rev-parse HEAD)"
git push "$remote" "HEAD:refs/heads/$branch" "${extra[@]}"

# 4. Verify. "Everything up-to-date" is not proof — compare SHAs.
git fetch --quiet "$remote" "$branch" 2>/dev/null || true
remote_sha="$(git rev-parse "refs/remotes/$remote/$branch" 2>/dev/null || echo none)"
if [ "$remote_sha" != "$head_sha" ]; then
  echo "git-safe-push: PUSH DID NOT LAND what you built." >&2
  echo "  local HEAD : $head_sha" >&2
  echo "  ${remote}/${branch} : $remote_sha" >&2
  echo "  The push was a no-op or landed a different ref. Do NOT assume success." >&2
  exit 1
fi
echo "git-safe-push: ${remote}/${branch} == local HEAD (${head_sha:0:9}) — verified."
