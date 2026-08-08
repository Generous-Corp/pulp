#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
hook="${repo_root}/hooks/scripts/inject-worktree-lineage.sh"
tmp="$(mktemp -d)"
trap 'find "${tmp}" -depth -delete 2>/dev/null || true' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

git -C "${tmp}" init -q
git -C "${tmp}" config user.name test
git -C "${tmp}" config user.email test@example.com
git -C "${tmp}" remote add origin git@github.com:Generous-Corp/pulp.git
git -C "${tmp}" checkout -q -b old-work
git -C "${tmp}" commit --allow-empty -qm init
mkdir "${tmp}/markers"

run_hook() {
    local session="$1"
    printf '{"cwd":"%s","session_id":"%s"}' "${tmp}" "${session}" |
        TMPDIR="${tmp}/markers" bash "${hook}"
}

out="$(run_hook unclassified)"
printf '%s' "${out}" | python3 -c '
import json, sys
ctx = json.load(sys.stdin)["hookSpecificOutput"]["additionalContext"]
assert "UNCLASSIFIED" in ctx, ctx
' || fail "unclassified worktree did not inject a warning"

head="$(git -C "${tmp}" rev-parse HEAD)"
git -C "${tmp}" config --local branch.old-work.pulpWorktreeStatus active
git -C "${tmp}" config --local branch.old-work.pulpWorktreeDurableSha "${head}"
[[ -z "$(run_hook active)" ]] || fail "exact active lineage should be silent"

git -C "${tmp}" config --local branch.old-work.pulpWorktreeDurableSha "0000000000000000000000000000000000000000"
out="$(run_hook stale-active)"
printf '%s' "${out}" | python3 -c '
import json, sys
ctx = json.load(sys.stdin)["hookSpecificOutput"]["additionalContext"]
assert "recorded SHA does not match HEAD" in ctx, ctx
' || fail "stale active lineage did not inject a warning"

git -C "${tmp}" config --local branch.old-work.pulpWorktreeStatus superseded
git -C "${tmp}" config --local branch.old-work.pulpWorktreeSuccessor feature/replacement
git -C "${tmp}" config --local branch.old-work.pulpWorktreeNote 'continued elsewhere'
out="$(run_hook superseded)"
printf '%s' "${out}" | python3 -c '
import json, sys
ctx = json.load(sys.stdin)["hookSpecificOutput"]["additionalContext"]
assert "SUPERSEDED" in ctx, ctx
assert "feature/replacement" in ctx, ctx
assert "continued elsewhere" in ctx, ctx
' || fail "superseded worktree did not identify its successor"

[[ -z "$(run_hook superseded)" ]] || fail "duplicate hook invocation was not suppressed"

git -C "${tmp}" config --local branch.old-work.pulpWorktreeStatus merged
git -C "${tmp}" config --local branch.old-work.pulpWorktreePr https://github.com/Generous-Corp/pulp/pull/123
out="$(run_hook merged)"
printf '%s' "${out}" | python3 -c '
import json, sys
ctx = json.load(sys.stdin)["hookSpecificOutput"]["additionalContext"]
assert "MERGED" in ctx and "pull/123" in ctx, ctx
' || fail "merged worktree did not inject its PR"

git -C "${tmp}" config --local branch.old-work.pulpWorktreeStatus archived
git -C "${tmp}" config --local branch.old-work.pulpWorktreeArchive /durable/old-work.bundle
out="$(run_hook archived)"
printf '%s' "${out}" | python3 -c '
import json, sys
ctx = json.load(sys.stdin)["hookSpecificOutput"]["additionalContext"]
assert "ARCHIVED" in ctx and "old-work.bundle" in ctx, ctx
' || fail "archived worktree did not inject its archive"

echo "inject-worktree-lineage hook: all tests passed"
