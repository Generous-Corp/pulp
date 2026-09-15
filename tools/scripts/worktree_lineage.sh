#!/usr/bin/env bash
# Record why a Pulp worktree exists and where its work went.
#
# State is stored on the local branch in the repository's shared Git config, so
# every sibling worktree can see it and the record survives worktree removal.
set -euo pipefail

die() {
    echo "worktree_lineage: $*" >&2
    exit 2
}

usage() {
    cat <<'EOF'
Usage:
  worktree_lineage.sh mark --status STATUS [options]
  worktree_lineage.sh show [--branch BRANCH | --path PATH]
  worktree_lineage.sh list
  worktree_lineage.sh reconcile [--branch BRANCH] [--repo OWNER/REPO] [--dry-run]

Statuses: active, superseded, merged, archived

Mark options:
  --branch BRANCH       Branch to update (defaults to the branch at --path/cwd)
  --path PATH           Worktree path (defaults to cwd)
  --owner TEXT          Current owner or agent/session label
  --successor TEXT      Successor branch, worktree, handoff, or goal
  --pr URL              Pull request that landed the work
  --archive PATH        Verified Git bundle containing the exact branch/head
  --note TEXT           Short disposition/context note

`superseded` requires --successor. `merged` requires --pr unless the exact head
is already an ancestor of origin/main. `archived` requires an existing archive;
its SHA-256 is recorded automatically.

`reconcile` closes out what nobody marked: for every registered worktree whose
exact head is the second parent of a "Merge pull request #N" commit on
origin/main's first-parent line, it records `merged` with that PR URL and the
merge commit as its note. No API call is made; the proof is the merge commit
itself, which is stronger than a typed URL. Squash-landed heads and heads not in
origin/main are listed as unresolved and left untouched. `--repo` overrides the
OWNER/REPO read from the origin remote (required when origin is not github.com).
EOF
}

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" ||
    die "run inside a Git worktree"

absolute_common_dir() {
    local root="$1" raw
    raw="$(git -C "${root}" rev-parse --git-common-dir 2>/dev/null)" || return 1
    [[ "${raw}" == /* ]] || raw="${root}/${raw}"
    (cd "${raw}" && pwd -P)
}

common_dir="$(absolute_common_dir "${repo_root}")"

command_name="${1:-}"
[[ -n "${command_name}" ]] || { usage; exit 2; }
shift

branch=""
path=""
status=""
owner=""
successor=""
pr=""
archive=""
note=""
repo_slug=""
dry_run=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --branch) [[ $# -ge 2 ]] || die "--branch requires a value"; branch="$2"; shift 2 ;;
        --path) [[ $# -ge 2 ]] || die "--path requires a value"; path="$2"; shift 2 ;;
        --status) [[ $# -ge 2 ]] || die "--status requires a value"; status="$2"; shift 2 ;;
        --owner) [[ $# -ge 2 ]] || die "--owner requires a value"; owner="$2"; shift 2 ;;
        --successor) [[ $# -ge 2 ]] || die "--successor requires a value"; successor="$2"; shift 2 ;;
        --pr) [[ $# -ge 2 ]] || die "--pr requires a value"; pr="$2"; shift 2 ;;
        --archive) [[ $# -ge 2 ]] || die "--archive requires a value"; archive="$2"; shift 2 ;;
        --note) [[ $# -ge 2 ]] || die "--note requires a value"; note="$2"; shift 2 ;;
        --repo) [[ $# -ge 2 ]] || die "--repo requires a value"; repo_slug="$2"; shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument '$1'" ;;
    esac
done

for value in "${owner}" "${successor}" "${pr}" "${archive}" "${note}"; do
    [[ "${value}" != *$'\n'* && "${value}" != *$'\t'* ]] ||
        die "metadata values cannot contain tabs or newlines"
done

resolve_branch() {
    if [[ -n "${branch}" ]]; then
        git show-ref --verify --quiet "refs/heads/${branch}" ||
            die "local branch not found: ${branch}"
        return
    fi
    local target="${path:-${repo_root}}"
    [[ -d "${target}" ]] || die "worktree path not found: ${target}"
    local target_common
    target_common="$(absolute_common_dir "${target}")" ||
        die "not a Git worktree: ${target}"
    [[ "${target_common}" == "${common_dir}" ]] ||
        die "path belongs to a different repository: ${target}"
    branch="$(git -C "${target}" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
    [[ -n "${branch}" ]] || die "detached worktrees require --branch"
    path="$(cd "${target}" && pwd -P)"
}

key() {
    printf 'branch.%s.pulpWorktree%s' "${branch}" "$1"
}

get_value() {
    git config --local --get "$(key "$1")" </dev/null 2>/dev/null || true
}

set_if_present() {
    local field="$1" value="$2"
    if [[ -n "${value}" ]]; then
        git config --local "$(key "${field}")" "${value}" </dev/null
    fi
}

unset_field() {
    git config --local --unset-all "$(key "$1")" </dev/null 2>/dev/null || true
}

show_branch() {
    local b="$1"
    branch="${b}"
    printf 'branch\t%s\n' "${branch}"
    printf 'status\t%s\n' "$(get_value Status)"
    printf 'durable_sha\t%s\n' "$(get_value DurableSha)"
    printf 'updated_at\t%s\n' "$(get_value UpdatedAt)"
    printf 'owner\t%s\n' "$(get_value Owner)"
    printf 'last_path\t%s\n' "$(get_value LastPath)"
    printf 'successor\t%s\n' "$(get_value Successor)"
    printf 'pr\t%s\n' "$(get_value Pr)"
    printf 'archive\t%s\n' "$(get_value Archive)"
    printf 'archive_sha256\t%s\n' "$(get_value ArchiveSha256)"
    printf 'note\t%s\n' "$(get_value Note)"
}

github_slug_from_origin() {
    local url
    url="$(git remote get-url origin 2>/dev/null)" || return 1
    [[ "${url}" =~ github\.com[:/]([^/]+)/([^/]+)$ ]] || return 1
    local owner="${BASH_REMATCH[1]}" name="${BASH_REMATCH[2]}"
    name="${name%/}"; name="${name%.git}"
    [[ -n "${owner}" && -n "${name}" ]] || return 1
    printf '%s/%s\n' "${owner}" "${name}"
}

# One row per registered worktree: path, head, branch (empty when detached).
registered_worktrees() {
    git worktree list --porcelain | awk '
        /^worktree / { w = substr($0, 10) }
        /^HEAD / { h = $2 }
        /^branch / { b = substr($0, 8); sub("^refs/heads/", "", b) }
        /^$/ { if (w != "") printf "%s\t%s\t%s\n", w, h, b; w = h = b = "" }
        END { if (w != "") printf "%s\t%s\t%s\n", w, h, b }'
}

case "${command_name}" in
    reconcile)
        git show-ref --verify --quiet refs/remotes/origin/main ||
            die "reconcile needs origin/main; fetch it first"
        [[ -n "${repo_slug}" ]] || repo_slug="$(github_slug_from_origin || true)"
        [[ "${repo_slug}" =~ ^[^/]+/[^/]+$ ]] ||
            die "origin is not a github.com remote; pass --repo OWNER/REPO"
        main_tip="$(git rev-parse refs/remotes/origin/main)"
        # First-parent merges on main, oldest evidence last: "<merge> <p1> <p2>\t<subject>".
        merge_table="$(git log --first-parent --merges --format='%H %P%x09%s' refs/remotes/origin/main)"
        printf 'RESULT\tBRANCH\tHEAD\tDETAIL\n'
        while IFS=$'\t' read -r wt_path wt_head wt_branch; do
            [[ -n "${wt_branch}" ]] || continue
            if [[ -n "${branch}" && "${wt_branch}" != "${branch}" ]]; then continue; fi
            branch="${wt_branch}"
            current_status="$(get_value Status)"
            current_pr="$(get_value Pr)"
            current_sha="$(get_value DurableSha)"
            branch=""
            if [[ "${current_status}" == merged && -n "${current_pr}" && "${current_sha}" == "${wt_head}" ]]; then
                printf 'already\t%s\t%s\t%s\n' "${wt_branch}" "${wt_head:0:12}" "${current_pr}"
                continue
            fi
            if [[ "${wt_head}" == "${main_tip}" ]]; then
                printf 'unresolved\t%s\t%s\tat the origin/main tip, no commits of its own\n' \
                    "${wt_branch}" "${wt_head:0:12}"
                continue
            fi
            if ! git merge-base --is-ancestor "${wt_head}" refs/remotes/origin/main 2>/dev/null; then
                printf 'unresolved\t%s\t%s\tnot in origin/main\n' "${wt_branch}" "${wt_head:0:12}"
                continue
            fi
            merge_line="$(awk -F'\t' -v h="${wt_head}" '{ split($1, p, " "); if (p[3] == h) { print; exit } }' <<<"${merge_table}")"
            if [[ -z "${merge_line}" ]]; then
                printf 'unresolved\t%s\t%s\tno merge commit on origin/main has this head as its second parent (squash- or fast-forward-landed?)\n' \
                    "${wt_branch}" "${wt_head:0:12}"
                continue
            fi
            merge_sha="${merge_line%% *}"
            subject="${merge_line#*$'\t'}"
            if [[ ! "${subject}" =~ ^Merge\ pull\ request\ \#([0-9]+)\  ]]; then
                printf 'unresolved\t%s\t%s\tmerge commit %s names no pull request: %s\n' \
                    "${wt_branch}" "${wt_head:0:12}" "${merge_sha:0:12}" "${subject}"
                continue
            fi
            url="https://github.com/${repo_slug}/pull/${BASH_REMATCH[1]}"
            if [[ "${dry_run}" -eq 1 ]]; then
                printf 'would-mark\t%s\t%s\t%s\n' "${wt_branch}" "${wt_head:0:12}" "${url}"
                continue
            fi
            branch="${wt_branch}"
            unset_field Archive; unset_field ArchiveSha256
            git config --local "$(key Status)" merged
            git config --local "$(key DurableSha)" "${wt_head}"
            git config --local "$(key UpdatedAt)" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
            git config --local "$(key LastPath)" "${wt_path}"
            git config --local "$(key Pr)" "${url}"
            git config --local "$(key Note)" "reconciled from origin/main merge commit ${merge_sha:0:12}"
            branch=""
            printf 'merged\t%s\t%s\t%s\n' "${wt_branch}" "${wt_head:0:12}" "${url}"
        done < <(registered_worktrees)
        ;;
    mark)
        resolve_branch
        case "${status}" in
            active|superseded|merged|archived) ;;
            '') die "mark requires --status" ;;
            *) die "invalid status '${status}'" ;;
        esac

        head="$(git rev-parse "refs/heads/${branch}")"
        if [[ "${status}" == "superseded" && -z "${successor}" ]]; then
            die "superseded requires --successor"
        fi
        if [[ "${status}" == "merged" && -z "${pr}" ]]; then
            git show-ref --verify --quiet refs/remotes/origin/main ||
                die "merged requires --pr when origin/main is unavailable"
            git merge-base --is-ancestor "${head}" origin/main ||
                die "exact head is not in origin/main; provide the merged PR"
        fi
        if [[ -n "${pr}" && ! "${pr}" =~ ^https://github.com/[^/]+/[^/]+/pull/[0-9]+$ ]]; then
            die "--pr must be a full GitHub pull-request URL"
        fi
        archive_sha=""
        if [[ "${status}" == "archived" ]]; then
            [[ -n "${archive}" ]] || die "archived requires --archive"
            [[ -f "${archive}" ]] || die "archive not found: ${archive}"
            archive="$(cd "$(dirname "${archive}")" && pwd -P)/$(basename "${archive}")"
            git bundle verify "${archive}" >/dev/null 2>&1 ||
                die "archive is not a valid Git bundle: ${archive}"
            git bundle list-heads "${archive}" "refs/heads/${branch}" \
                | awk -v head="${head}" '$1 == head { found=1 } END { exit !found }' ||
                die "archive does not contain refs/heads/${branch} at ${head}"
            archive_sha="$(shasum -a 256 "${archive}" | awk '{print $1}')"
        fi

        case "${status}" in
            active)
                unset_field Successor; unset_field Pr
                unset_field Archive; unset_field ArchiveSha256
                ;;
            superseded)
                unset_field Pr; unset_field Archive; unset_field ArchiveSha256
                ;;
            merged)
                unset_field Archive; unset_field ArchiveSha256
                ;;
            archived)
                unset_field Pr
                ;;
        esac

        git config --local "$(key Status)" "${status}"
        git config --local "$(key DurableSha)" "${head}"
        git config --local "$(key UpdatedAt)" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        [[ -n "${path}" ]] && git config --local "$(key LastPath)" "${path}"
        set_if_present Owner "${owner}"
        set_if_present Successor "${successor}"
        set_if_present Pr "${pr}"
        set_if_present Archive "${archive}"
        set_if_present ArchiveSha256 "${archive_sha}"
        set_if_present Note "${note}"
        show_branch "${branch}"
        ;;
    show)
        resolve_branch
        show_branch "${branch}"
        ;;
    list)
        # Read the whole lineage table in a fixed number of git invocations. A
        # per-branch `git config --get` loop costs one process per field per
        # branch, so a checkout carrying a few thousand local branches spends
        # tens of thousands of spawns and minutes of wall time to print a table
        # that is already sitting in one config file. git lowercases the
        # trailing key component, so the lookups below are lowercase even
        # though `key()` writes them camel-cased.
        printf 'STATUS\tHEAD_MATCH\tUPDATED\tDURABLE_SHA\tBRANCH\tLAST_PATH\tSUCCESSOR\tPR_OR_ARCHIVE\n'
        awk -F'\t' '
            NR == FNR {
                space = index($0, " ")
                if (space == 0) { name = $0; value = "" }
                else { name = substr($0, 1, space - 1); value = substr($0, space + 1) }
                # A value holding a newline arrives as continuation lines that
                # are not config keys; skip them rather than guess at them.
                if (name ~ /^branch\./) cfg[name] = value
                next
            }
            {
                branch = $1
                prefix = "branch." branch ".pulpworktree"
                state = cfg[prefix "status"]
                if (state == "") next
                durable = cfg[prefix "durablesha"]
                head_match = (durable == $2) ? "yes" : "no"
                destination = cfg[prefix "pr"]
                if (destination == "") destination = cfg[prefix "archive"]
                printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", \
                    state, head_match, cfg[prefix "updatedat"], durable, \
                    branch, cfg[prefix "lastpath"], cfg[prefix "successor"], \
                    destination
            }
        ' <(git config --local --get-regexp '^branch\..*\.pulpworktree' 2>/dev/null || true) \
          <(git for-each-ref --format='%(refname:short)%09%(objectname)' refs/heads | sort)
        ;;
    *)
        usage
        exit 2
        ;;
esac
