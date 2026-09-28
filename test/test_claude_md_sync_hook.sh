#!/usr/bin/env bash
# claude_md_sync.py: a SessionStart hook that injects origin/main's CLAUDE.md /
# AGENTS.md when the checkout's copy drifted. Hermetic: real temp git repos (a
# bare origin named like Pulp's, clones on old and current branches).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
hook="${PULP_CLAUDE_MD_SYNC_HOOK:-${repo_root}/hooks/scripts/claude_md_sync.py}"
launcher="${PULP_CLAUDE_MD_SYNC_LAUNCHER:-${repo_root}/hooks/scripts/pulp-claude-md-sync-session}"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
export TMPDIR="${tmp}/markers"
mkdir -p "${TMPDIR}"
unset CLAUDE_PROJECT_DIR PULP_CLAUDE_MD_SYNC

fail() { echo "FAIL: $*" >&2; exit 1; }
g() { git -c user.name=test -c user.email=test@example.invalid -c init.defaultBranch=main "$@"; }

section() { # name, body-word, repeat
    printf '## %s\n\n' "$1"
    for _ in $(seq 1 "$3"); do printf '%s ' "$2"; done
    printf '\n\n'
}

origin="${tmp}/Generous-Corp/pulp.git"
mkdir -p "$(dirname "${origin}")"
g init -q --bare "${origin}"
seed="${tmp}/seed"
g clone -q "${origin}" "${seed}" 2>/dev/null
mkdir -p "${seed}/hooks/scripts"
cp "${hook}" "${seed}/hooks/scripts/claude_md_sync.py"
{ printf '# CLAUDE.md\n\nintro\n\n'; section "Build" "old-build" 5; section "Retired Routing" "pulp-gate-fast" 5; } > "${seed}/CLAUDE.md"
printf '# AGENTS.md\n\nSee CLAUDE.md.\n' > "${seed}/AGENTS.md"
g -C "${seed}" add -A && g -C "${seed}" commit -qm v1
g -C "${seed}" push -q origin HEAD:main
g -C "${seed}" push -q origin HEAD:refs/heads/old-branch
{ printf '# CLAUDE.md\n\nintro\n\n'; section "Build" "focused-pulp-affected" 5; section "Versioning" "version-at-land" 5; } > "${seed}/CLAUDE.md"
g -C "${seed}" commit -qam v2
g -C "${seed}" push -q origin HEAD:main

stale="${tmp}/stale"
g clone -q "${origin}" "${stale}" 2>/dev/null
g -C "${stale}" checkout -q old-branch
fresh="${tmp}/fresh"
g clone -q "${origin}" "${fresh}" 2>/dev/null

run() { # cwd, [session], [extra env...]
    local cwd="$1" session="${2:-}"
    shift 2 || shift $#
    printf '{"hook_event_name":"SessionStart","cwd":"%s","session_id":"%s","source":"startup"}' \
        "${cwd}" "${session}" | env "$@" python3 "${hook}" --hook-json
}
context_of() {
    python3 -c '
import json, sys
lines = sys.stdin.read().splitlines()
assert len(lines) == 1, lines
out = json.loads(lines[0])
spec = out["hookSpecificOutput"]
assert spec["hookEventName"] == "SessionStart", spec
assert isinstance(spec["additionalContext"], str) and spec["additionalContext"]
print(spec["additionalContext"])'
}

# 1. Drifted: one note, main's changed sections, retired section named.
g -C "${stale}" status --porcelain=v1 >/dev/null
printf 'dirty\n' >> "${stale}/AGENTS.md"
printf 'scratch\n' > "${stale}/untracked.txt"
before_status="$(g -C "${stale}" status --porcelain=v1 --ignored --untracked-files=all | shasum)"
before_index="$(shasum < "${stale}/.git/index")"
before_files="$(g -C "${stale}" ls-files -s | shasum)"
start=$(python3 -c 'import time; print(time.time())')
ctx="$(run "${stale}" drift-1 | context_of)" || fail "drifted checkout produced no valid hook JSON"
elapsed=$(python3 -c "import time; print(f'{time.time() - ${start}:.2f}')")
[[ "$(printf '%s\n' "${ctx}" | sed -n 1p)" == "PULP INSTRUCTIONS DRIFT: CLAUDE.md in this checkout is 1 commits / "* ]] \
    || fail "first line is not the drift note: $(printf '%s\n' "${ctx}" | sed -n 1p)"
grep -q "takes precedence" <<<"${ctx}" || fail "note does not say main takes precedence"
grep -q "focused-pulp-affected" <<<"${ctx}" || fail "changed section content missing"
grep -q "version-at-land" <<<"${ctx}" || fail "new section content missing"
grep -q "retired; disregard them): Retired Routing" <<<"${ctx}" || fail "retired section not named"
grep -q "old-build" <<<"${ctx}" && fail "stale section content leaked into the note"
grep -q "PULP INSTRUCTIONS DRIFT: AGENTS.md" <<<"${ctx}" || fail "dirty AGENTS.md drift not reported"

# 2. Read-only: status, index and staged entries are byte-identical afterwards.
[[ "$(g -C "${stale}" status --porcelain=v1 --ignored --untracked-files=all | shasum)" == "${before_status}" ]] \
    || fail "git status changed"
[[ "$(shasum < "${stale}/.git/index")" == "${before_index}" ]] || fail ".git/index was rewritten"
[[ "$(g -C "${stale}" ls-files -s | shasum)" == "${before_files}" ]] || fail "index entries changed"
g -C "${stale}" checkout -q -- AGENTS.md && rm "${stale}/untracked.txt"

# 3. Duplicate copies in one session (plugin + user-level launcher) speak once.
[[ -z "$(run "${stale}" drift-1)" ]] || fail "second copy in the same session emitted again"

# 4. In sync, not Pulp, no origin/main, opt-out, missing cwd: silent, exit 0.
[[ -z "$(run "${fresh}" sync)" ]] || fail "in-sync checkout emitted output"
other="${tmp}/other"
g clone -q "${origin}" "${other}" 2>/dev/null
g -C "${other}" checkout -q old-branch
g -C "${other}" remote set-url origin https://github.com/example/other.git
[[ -z "$(run "${other}" other)" ]] || fail "non-Pulp repo emitted output"
nomain="${tmp}/nomain"
g clone -q "${origin}" "${nomain}" 2>/dev/null
g -C "${nomain}" checkout -q old-branch
g -C "${nomain}" update-ref -d refs/remotes/origin/main
g -C "${nomain}" update-ref -d refs/remotes/origin/HEAD 2>/dev/null || true
[[ -z "$(run "${nomain}" nomain)" ]] || fail "checkout without origin/main emitted output"
[[ -z "$(run "${stale}" optout PULP_CLAUDE_MD_SYNC=0)" ]] || fail "PULP_CLAUDE_MD_SYNC=0 did not silence the hook"
run "${tmp}/does-not-exist" missing >/dev/null || fail "missing cwd did not exit 0"

# 5. Codex-shaped payload (no session_id) and plain mode.
codex_ctx="$(printf '{"hook_event_name":"SessionStart","cwd":"%s","model":"gpt","source":"startup"}' "${stale}" |
    python3 "${hook}" | context_of)" || fail "Codex payload produced no valid hook JSON"
grep -q "PULP INSTRUCTIONS DRIFT" <<<"${codex_ctx}" || fail "Codex payload missed the note"
(cd "${stale}" && python3 "${hook}" --plain </dev/null) | grep -q "^PULP INSTRUCTIONS DRIFT: CLAUDE.md" \
    || fail "plain mode did not print the note"

# 6. Over the cap: bounded, with a pointer to the full file.
for i in $(seq 1 40); do section "Topic ${i}" "long-updated-text-${i}" 60; done > "${seed}/CLAUDE.md"
g -C "${seed}" commit -qam v3 && g -C "${seed}" push -q origin HEAD:main
g -C "${stale}" fetch -q origin
big="$(run "${stale}" big | context_of)" || fail "oversized drift produced no valid hook JSON"
(( ${#big} <= 9000 )) || fail "oversized drift injected ${#big} chars (cap 9000)"
grep -q "not shown in full" <<<"${big}" || fail "oversized drift lacks the omission notice"
grep -q 'git show origin/main:CLAUDE.md' <<<"${big}" || fail "oversized drift lacks the pointer"
grep -q "Topic 40" <<<"${big}" || fail "changed-section list omits a heading"

# 7. The user-level launcher runs origin/main's copy of the hook for Pulp only.
lctx="$(printf '{"cwd":"%s","session_id":"launcher"}' "${stale}" | bash "${launcher}" | context_of)" \
    || fail "launcher produced no valid hook JSON"
grep -q "PULP INSTRUCTIONS DRIFT" <<<"${lctx}" || fail "launcher missed the note"
[[ -z "$(printf '{"cwd":"%s","session_id":"lo"}' "${stale}" | PULP_CLAUDE_MD_SYNC=0 bash "${launcher}")" ]] \
    || fail "launcher ignored PULP_CLAUDE_MD_SYNC=0"
evil_origin="${tmp}/example/evil.git"
mkdir -p "$(dirname "${evil_origin}")"
g init -q --bare "${evil_origin}"
evil="${tmp}/evil"
g clone -q "${evil_origin}" "${evil}" 2>/dev/null
mkdir -p "${evil}/hooks/scripts"
printf 'open(%s, "w").write("ran")\n' "'${tmp}/evil-ran'" > "${evil}/hooks/scripts/claude_md_sync.py"
g -C "${evil}" add -A && g -C "${evil}" commit -qm evil && g -C "${evil}" push -q origin HEAD:main
g -C "${evil}" fetch -q origin
printf '{"cwd":"%s"}' "${evil}" | bash "${launcher}" >/dev/null
[[ ! -e "${tmp}/evil-ran" ]] || fail "launcher executed a non-Pulp repository's script"

# 8. Both hook configs wire the script.
grep -q 'hooks/scripts/claude_md_sync.py' "${repo_root}/hooks/hooks.json" || fail "plugin hooks.json does not wire the hook"
grep -q 'hooks/scripts/claude_md_sync.py' "${repo_root}/.codex/hooks.json" || fail ".codex/hooks.json does not wire the hook"

echo "claude-md-sync hook: all tests passed (drifted run ${elapsed}s)"
