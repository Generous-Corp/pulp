#!/usr/bin/env bash
# Inject continuity state before an agent resumes a Pulp worktree.
# The shared Git config is authoritative even when the branch predates this hook.
set -u

input="$(cat 2>/dev/null || true)"
parsed=""
if [[ -n "${input}" ]] && command -v python3 >/dev/null 2>&1; then
    parsed="$(printf '%s' "${input}" | python3 -c '
import json, sys
try:
    value = json.load(sys.stdin)
    print(value.get("cwd") or value.get("project_dir") or "")
    print(value.get("session_id") or "")
except Exception:
    pass
' 2>/dev/null || true)"
fi

hook_cwd="$(printf '%s\n' "${parsed}" | sed -n '1p')"
hook_session="$(printf '%s\n' "${parsed}" | sed -n '2p')"
cwd="${PULP_LINEAGE_CWD:-${hook_cwd:-${CLAUDE_PROJECT_DIR:-${PWD:-.}}}}"
root="$(git -C "${cwd}" rev-parse --show-superproject-working-tree 2>/dev/null || true)"
[[ -n "${root}" ]] || root="$(git -C "${cwd}" rev-parse --show-toplevel 2>/dev/null || true)"
[[ -n "${root}" ]] || exit 0

remote="$(git -C "${root}" config --get remote.origin.url 2>/dev/null || true)"
case "${remote}" in
    *Generous-Corp/pulp|*Generous-Corp/pulp.git|*danielraffel/pulp|*danielraffel/pulp.git) ;;
    *) exit 0 ;;
esac

branch="$(git -C "${root}" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
if [[ -z "${branch}" ]]; then
    message="PULP WORKTREE WARNING: this session started on a detached HEAD at ${root}. Identify its durable branch or archive before changing or removing it."
else
    key="branch.${branch}.pulpWorktree"
    status="$(git -C "${root}" config --local --get "${key}Status" 2>/dev/null || true)"
    durable_sha="$(git -C "${root}" config --local --get "${key}DurableSha" 2>/dev/null || true)"
    successor="$(git -C "${root}" config --local --get "${key}Successor" 2>/dev/null || true)"
    pr="$(git -C "${root}" config --local --get "${key}Pr" 2>/dev/null || true)"
    archive="$(git -C "${root}" config --local --get "${key}Archive" 2>/dev/null || true)"
    note="$(git -C "${root}" config --local --get "${key}Note" 2>/dev/null || true)"
    head="$(git -C "${root}" rev-parse HEAD 2>/dev/null || true)"

    case "${status}" in
        active)
            if [[ -n "${durable_sha}" && "${durable_sha}" == "${head}" ]]; then
                exit 0
            fi
            message="PULP WORKTREE WARNING: ${branch} is marked active but its recorded SHA does not match HEAD. Reconcile lineage before changing or removing ${root}."
            ;;
        superseded)
            message="PULP WORKTREE SUPERSEDED: do not resume ${branch} at ${root}. Continue at: ${successor:-unknown successor}."
            ;;
        merged)
            message="PULP WORKTREE MERGED: do not resume ${branch} at ${root}. Start fresh from origin/main. PR: ${pr:-not recorded}."
            ;;
        archived)
            message="PULP WORKTREE ARCHIVED: do not resume ${branch} at ${root} without explicitly restoring/reclassifying it. Archive: ${archive:-not recorded}."
            ;;
        *)
            message="PULP WORKTREE UNCLASSIFIED: ${branch} at ${root} has no continuity record. Before editing, determine whether it is active, superseded, merged, or archived, then run tools/scripts/worktree_lineage.sh mark."
            ;;
    esac

    [[ -z "${note}" ]] || message="${message} Note: ${note}"
fi

if ! command -v python3 >/dev/null 2>&1; then
    printf '%s\n' "${message}" >&2
    exit 0
fi

# Global and plugin hooks may coexist. Emit only once for a given session.
if [[ -n "${hook_session}" ]]; then
    safe_session="$(printf '%s' "${hook_session}" | tr -cd 'A-Za-z0-9._-')"
    marker="${TMPDIR:-/tmp}/pulp-lineage-session-${safe_session}"
    if [[ -n "${safe_session}" && -e "${marker}" ]]; then
        exit 0
    fi
    [[ -z "${safe_session}" ]] || : > "${marker}" 2>/dev/null || true
fi

PULP_LINEAGE_MESSAGE="${message}" python3 -c '
import json, os
print(json.dumps({"hookSpecificOutput": {
    "hookEventName": "SessionStart",
    "additionalContext": os.environ["PULP_LINEAGE_MESSAGE"],
}, "suppressOutput": False}))
'
exit 0
