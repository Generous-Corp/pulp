#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="${SCRIPT_DIR}/worktree_lineage.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

cfgval() {
    git -C "${tmp}/repo" config --get "branch.$1.pulpWorktree$2" 2>/dev/null || true
}

# `list` renders the lineage config table, so assert every emitted column
# against the config read the plain way. Loose `grep -q` probes leave most of
# the row unfalsifiable: a join that drops or transposes a column still
# contains the strings they look for.
assert_list_row() {
    local branch="$1" output="$2" row expected destination durable head_match
    row="$(awk -F'\t' -v b="${branch}" '$5 == b { print; found = 1 } END { exit !found }' <<<"${output}")" || {
        echo "list emitted no row for ${branch}" >&2
        exit 1
    }
    destination="$(cfgval "${branch}" Pr)"
    [[ -n "${destination}" ]] || destination="$(cfgval "${branch}" Archive)"
    durable="$(cfgval "${branch}" DurableSha)"
    head_match=no
    [[ "${durable}" == "$(git -C "${tmp}/repo" rev-parse "refs/heads/${branch}")" ]] && head_match=yes
    expected="$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' \
        "$(cfgval "${branch}" Status)" "${head_match}" "$(cfgval "${branch}" UpdatedAt)" \
        "${durable}" "${branch}" "$(cfgval "${branch}" LastPath)" \
        "$(cfgval "${branch}" Successor)" "${destination}")"
    if [[ "${row}" != "${expected}" ]]; then
        echo "list row for ${branch} disagrees with its config" >&2
        echo "  got:      ${row}" >&2
        echo "  expected: ${expected}" >&2
        exit 1
    fi
}

git init -q "${tmp}/repo"
git -C "${tmp}/repo" config user.email test@example.invalid
git -C "${tmp}/repo" config user.name test
printf 'base\n' > "${tmp}/repo/file.txt"
git -C "${tmp}/repo" add file.txt
git -C "${tmp}/repo" commit -qm base
git -C "${tmp}/repo" branch feature/example
git -C "${tmp}/repo" worktree add -q "${tmp}/worktree" feature/example

(cd "${tmp}/worktree" && "${TOOL}" mark --status active --owner test-agent --note fixture >/dev/null)
test "$(git -C "${tmp}/repo" config branch.feature/example.pulpWorktreeStatus)" = active
test "$(git -C "${tmp}/repo" config branch.feature/example.pulpWorktreeOwner)" = test-agent
test "$(git -C "${tmp}/repo" config branch.feature/example.pulpWorktreeDurableSha)" = \
    "$(git -C "${tmp}/worktree" rev-parse HEAD)"

if (cd "${tmp}/worktree" && "${TOOL}" mark --status superseded >/dev/null 2>&1); then
    echo "superseded without successor unexpectedly succeeded" >&2
    exit 1
fi
(cd "${tmp}/worktree" && "${TOOL}" mark --status superseded --successor feature/replacement >/dev/null)
test "$(git -C "${tmp}/repo" config branch.feature/example.pulpWorktreeSuccessor)" = feature/replacement

if (cd "${tmp}/worktree" && "${TOOL}" mark --status archived --archive "${tmp}/missing" >/dev/null 2>&1); then
    echo "missing archive unexpectedly succeeded" >&2
    exit 1
fi
git -C "${tmp}/repo" bundle create "${tmp}/archive.bundle" refs/heads/feature/example
(cd "${tmp}/worktree" && "${TOOL}" mark --status archived --archive "${tmp}/archive.bundle" >/dev/null)
expected_sha="$(shasum -a 256 "${tmp}/archive.bundle" | awk '{print $1}')"
test "$(git -C "${tmp}/repo" config branch.feature/example.pulpWorktreeArchiveSha256)" = "${expected_sha}"

list_output="$(cd "${tmp}/repo" && "${TOOL}" list)"
grep -q $'archived\t' <<<"${list_output}"
grep -q $'archived\tyes\t' <<<"${list_output}"
grep -q 'feature/example' <<<"${list_output}"
grep -q "${tmp}/archive.bundle" <<<"${list_output}"
assert_list_row feature/example "${list_output}"

git -C "${tmp}/repo" branch feature/second
(cd "${tmp}/repo" && "${TOOL}" mark --branch feature/second --status active >/dev/null)
list_output="$(cd "${tmp}/repo" && "${TOOL}" list)"
test "$(grep -c '^archived' <<<"${list_output}")" = 1
test "$(grep -c '^active' <<<"${list_output}")" = 1
assert_list_row feature/example "${list_output}"
assert_list_row feature/second "${list_output}"

(cd "${tmp}/repo" && "${TOOL}" mark --branch feature/example --status active >/dev/null)
test -z "$(git -C "${tmp}/repo" config --get branch.feature/example.pulpWorktreeArchive || true)"
test -z "$(git -C "${tmp}/repo" config --get branch.feature/example.pulpWorktreeSuccessor || true)"

git init -q "${tmp}/other"
if (cd "${tmp}/repo" && "${TOOL}" show --path "${tmp}/other" >/dev/null 2>&1); then
    echo "a worktree from another repository unexpectedly succeeded" >&2
    exit 1
fi

if (cd "${tmp}/repo" && "${TOOL}" mark --branch feature/example --status merged \
        --pr not-a-pr >/dev/null 2>&1); then
    echo "an invalid PR URL unexpectedly succeeded" >&2
    exit 1
fi

git -C "${tmp}/worktree" checkout -q --detach
if (cd "${tmp}/worktree" && "${TOOL}" show >/dev/null 2>&1); then
    echo "detached show without --branch unexpectedly succeeded" >&2
    exit 1
fi

echo "worktree_lineage: all tests passed"
