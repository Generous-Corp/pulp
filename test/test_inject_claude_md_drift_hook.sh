#!/usr/bin/env bash
# The SessionStart drift hook must speak exactly when the injected CLAUDE.md is
# not origin/main's, name what the copy is, and stay silent otherwise.
#
# Every silent case below is paired with a speaking case on the same fixture
# and the same invocation, so silence is a measured result, not an instrument
# that never ran.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
hook="${PULP_DRIFT_HOOK:-${repo_root}/hooks/scripts/inject-claude-md-drift.sh}"
tmp="$(cd "$(mktemp -d)" && pwd -P)"
trap 'find "${tmp}" -depth -delete 2>/dev/null || true' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# The hook is exercised the way the wrappers run it: cwd inside the checkout,
# CLAUDE_PROJECT_DIR unset, stdin empty.
run_hook() {
    (cd "$1" && env -u CLAUDE_PROJECT_DIR bash "${hook}" </dev/null)
}

expect_silent() {
    local dir="$1" label="$2" out
    out="$(run_hook "${dir}")" || fail "${label}: hook exited non-zero"
    [[ -z "${out}" ]] || fail "${label}: expected silence, got: ${out}"
}

expect_contains() {
    local out="$1" label="$2"
    shift 2
    local needle
    for needle in "$@"; do
        [[ "${out}" == *"${needle}"* ]] || fail "${label}: missing '${needle}' in: ${out}"
    done
}

git_commit() {
    local dir="$1" msg="$2"
    GIT_COMMITTER_DATE="$3" git -C "${dir}" -c user.name=test -c user.email=test@example.com \
        commit -q --date="$3" -m "${msg}"
}

# ── Fixture: an upstream with three revisions of CLAUDE.md, and a clone ──────
upstream="${tmp}/upstream"
git init -q -b main "${upstream}"
printf 'contract v1\n' >"${upstream}/CLAUDE.md"
printf 'Read CLAUDE.md first.\n' >"${upstream}/AGENTS.md"
git -C "${upstream}" add CLAUDE.md AGENTS.md
git_commit "${upstream}" "v1" "2026-01-01T00:00:00Z"
printf 'contract v2\n' >"${upstream}/CLAUDE.md"
git -C "${upstream}" add CLAUDE.md
git_commit "${upstream}" "v2" "2026-02-01T00:00:00Z"
printf 'contract v3\n' >"${upstream}/CLAUDE.md"
git -C "${upstream}" add CLAUDE.md
git_commit "${upstream}" "v3" "2026-03-01T00:00:00Z"
v1="$(git -C "${upstream}" rev-parse --short main~2)"
v3="$(git -C "${upstream}" rev-parse --short main)"

clone="${tmp}/clone"
git clone -q "${upstream}" "${clone}"

# 1. Working tree == origin/main: silent.
expect_silent "${clone}" "current checkout"

# 2. Checkout of an older commit: the copy is HEAD's, and HEAD is behind.
git -C "${clone}" checkout -q -b old-work main~2
out="$(run_hook "${clone}")"
expect_contains "${out}" "checkout behind" \
    "── CLAUDE.md drift ──" \
    "working-tree copy at ${clone}" \
    "not origin/main's" \
    "committed copy (HEAD), and HEAD is 2 commits behind origin/main" \
    "last shared a commit on 2026-01-01" \
    "origin/main as fetched here: ${v3} (2026-03-01)" \
    "git show origin/main:CLAUDE.md" \
    "git diff origin/main -- CLAUDE.md"
[[ "${out}" != *"AGENTS.md drift"* ]] || fail "unchanged AGENTS.md was reported"

# 2b. The same invocation from a subdirectory finds the checkout root.
mkdir -p "${clone}/sub/dir"
out_sub="$(run_hook "${clone}/sub/dir")"
[[ "${out_sub}" == "${out}" ]] || fail "subdirectory run differs: ${out_sub}"

# 3. Back on main, the working tree overwritten with a HISTORICAL revision:
#    modified relative to HEAD, yet a blob the repository holds, so it is a
#    stale committed copy rather than a hand edit.
git -C "${clone}" checkout -q main
expect_silent "${clone}" "control: back on main"
git -C "${clone}" show "main~1:CLAUDE.md" >"${clone}/CLAUDE.md"
[[ -n "$(git -C "${clone}" status --porcelain -- CLAUDE.md)" ]] || fail "fixture: CLAUDE.md not modified"
out="$(run_hook "${clone}")"
expect_contains "${out}" "stale committed copy" \
    "differs from HEAD's copy too" \
    "a blob this repository already holds" \
    "not a hand edit" \
    "git show origin/main:CLAUDE.md"
[[ "${out}" != *"uncommitted local edits"* ]] || fail "historical blob reported as a hand edit"

# 4. A novel local edit: no object in the repository matches it.
printf 'contract v3\nplus a line typed here\n' >"${clone}/CLAUDE.md"
out="$(run_hook "${clone}")"
expect_contains "${out}" "hand edit" \
    "differs from HEAD's copy too" \
    "matches no object this repository holds: uncommitted local edits"
[[ "${out}" != *"not a hand edit"* ]] || fail "hand edit reported as a committed revision"
git -C "${clone}" checkout -q -- CLAUDE.md
expect_silent "${clone}" "control: restored working tree"

# 5. AGENTS.md drifts on its own and is named as such.
printf 'Read CLAUDE.md first.\nAnd this.\n' >"${clone}/AGENTS.md"
out="$(run_hook "${clone}")"
expect_contains "${out}" "AGENTS.md drift" \
    "── AGENTS.md drift ──" \
    "git show origin/main:AGENTS.md"
[[ "${out}" != *"CLAUDE.md drift"* ]] || fail "unchanged CLAUDE.md was reported alongside AGENTS.md"
git -C "${clone}" checkout -q -- AGENTS.md

# 6. A clone whose recorded default branch is not called main still compares
#    against the remote's default branch.
upstream_trunk="${tmp}/upstream-trunk"
git init -q -b trunk "${upstream_trunk}"
printf 'trunk v1\n' >"${upstream_trunk}/CLAUDE.md"
git -C "${upstream_trunk}" add CLAUDE.md
git_commit "${upstream_trunk}" "t1" "2026-01-01T00:00:00Z"
printf 'trunk v2\n' >"${upstream_trunk}/CLAUDE.md"
git -C "${upstream_trunk}" add CLAUDE.md
git_commit "${upstream_trunk}" "t2" "2026-02-01T00:00:00Z"
clone_trunk="${tmp}/clone-trunk"
git clone -q "${upstream_trunk}" "${clone_trunk}"
expect_silent "${clone_trunk}" "trunk clone current"
git -C "${clone_trunk}" checkout -q -b old trunk~1
out="$(run_hook "${clone_trunk}")"
expect_contains "${out}" "non-main default branch" \
    "not origin/trunk's" \
    "1 commits behind origin/trunk" \
    "git show origin/trunk:CLAUDE.md"

# 7. No-op cases, each on a fixture where the file itself differs from main
#    so that silence comes from the guard and not from equality.
#    a. A repository that has never fetched a remote default branch.
fresh="${tmp}/fresh"
git init -q -b main "${fresh}"
printf 'never fetched\n' >"${fresh}/CLAUDE.md"
git -C "${fresh}" add CLAUDE.md
git_commit "${fresh}" "local" "2026-01-01T00:00:00Z"
expect_silent "${fresh}" "no remote"
git -C "${fresh}" remote add origin "${upstream}"
expect_silent "${fresh}" "remote configured but never fetched"
#    b. Not a git repository at all.
mkdir -p "${tmp}/plain"
printf 'not tracked\n' >"${tmp}/plain/CLAUDE.md"
expect_silent "${tmp}/plain" "outside git"
#    c. The file is absent from the working tree.
git -C "${clone}" checkout -q -b no-file main
rm "${clone}/CLAUDE.md"
expect_silent "${clone}" "file absent in working tree"
git -C "${clone}" checkout -q -- CLAUDE.md

echo "inject-claude-md-drift hook: all tests passed"
