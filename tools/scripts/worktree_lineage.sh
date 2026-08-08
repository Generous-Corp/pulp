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

case "${command_name}" in
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
        printf 'STATUS\tHEAD_MATCH\tUPDATED\tDURABLE_SHA\tBRANCH\tLAST_PATH\tSUCCESSOR\tPR_OR_ARCHIVE\n'
        while IFS= read -r b; do
            branch="${b}"
            state="$(get_value Status)"
            [[ -n "${state}" ]] || continue
            destination="$(get_value Pr)"
            [[ -n "${destination}" ]] || destination="$(get_value Archive)"
            durable_sha="$(get_value DurableSha)"
            current_sha="$(git rev-parse "refs/heads/${branch}" </dev/null)"
            head_match=no
            [[ "${durable_sha}" == "${current_sha}" ]] && head_match=yes
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "${state}" "${head_match}" "$(get_value UpdatedAt)" "${durable_sha}" \
                "${branch}" "$(get_value LastPath)" "$(get_value Successor)" \
                "${destination}"
        done < <(git for-each-ref --format='%(refname:short)' refs/heads | sort)
        ;;
    *)
        usage
        exit 2
        ;;
esac
