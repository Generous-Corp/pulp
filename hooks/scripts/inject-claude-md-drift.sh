#!/usr/bin/env bash
# Warn when the CLAUDE.md an agent was handed is not origin/main's.
#
# Claude Code and Codex inject the project's CLAUDE.md / AGENTS.md into the
# agent's context from the launching checkout's WORKING TREE, with no tool call
# attached. Nothing downstream can tell whether that copy is current: a checkout
# that has not moved for days hands every session it launches a contract that
# main has since rewritten, and the agent quotes it as fact. A memory note
# saying "verify injected context" cannot help, because nothing ever prompts
# the verification; this hook is the prompt.
#
# It compares the working-tree bytes of each injected file with the same path
# on the locally fetched origin/main and, only when they differ, prints one
# short block saying what the copy is (this checkout's committed copy, a stale
# committed revision, or a local edit) and the command that reads the current
# one. When the two match it prints nothing, so its output is never noise. It
# never fetches: the yardstick is whatever origin/main this checkout already
# knows, and the block states that tip's date so a stale yardstick is visible
# too. Every git call here is a ref, tree, or object lookup; there is no
# history walk, so it stays well inside the hook's time budget on a cold cache.
#
# Agent-neutral: plain stdout, exit 0 (both agents add SessionStart stdout to
# context). A clean no-op outside a git checkout, in a clone that has never
# fetched its remote default branch, or when a file is absent on either side.
# Runs under macOS's bash 3.2: no arrays, no mapfile.

set -u

# A partial (promisor) clone fetches missing objects on demand. Asking whether
# a blob exists must never reach the network from a session hook; git 2.46+
# honors this variable, and older git is handled by skipping that probe.
export GIT_NO_LAZY_FETCH=1

candidate="${CLAUDE_PROJECT_DIR:-${PWD:-.}}"
root="$(git -C "$candidate" rev-parse --show-superproject-working-tree 2>/dev/null || true)"
[ -n "$root" ] || root="$(git -C "$candidate" rev-parse --show-toplevel 2>/dev/null || true)"
[ -n "$root" ] || exit 0

# The remote's default branch when the clone recorded one, else origin/main.
ref="$(git -C "$root" symbolic-ref -q refs/remotes/origin/HEAD 2>/dev/null || true)"
[ -n "$ref" ] || ref="refs/remotes/origin/main"
git -C "$root" rev-parse -q --verify "${ref}^{commit}" >/dev/null 2>&1 || exit 0
ref_name="${ref#refs/remotes/}"
tip="$(git -C "$root" log -1 --format='%h (%cs)' "$ref" 2>/dev/null || true)"

probe_ok=1
if git -C "$root" config --get-regexp '^remote\..*\.promisor$' >/dev/null 2>&1; then
    probe_ok="$(git --version 2>/dev/null | awk '{
        split($3, v, ".")
        print (v[1] > 2 || (v[1] == 2 && v[2] >= 46)) ? 1 : 0
    }')"
fi

for file in CLAUDE.md AGENTS.md; do
    [ -f "$root/$file" ] || continue
    main_blob="$(git -C "$root" rev-parse -q --verify "${ref}:${file}" 2>/dev/null || true)"
    [ -n "$main_blob" ] || continue
    tree_blob="$(git -C "$root" hash-object "$file" 2>/dev/null || true)"
    [ -n "$tree_blob" ] || continue
    [ "$tree_blob" != "$main_blob" ] || continue

    head_blob="$(git -C "$root" rev-parse -q --verify "HEAD:${file}" 2>/dev/null || true)"
    if [ "$tree_blob" = "$head_blob" ]; then
        behind="$(git -C "$root" rev-list --count "HEAD..${ref}" 2>/dev/null || echo '?')"
        base="$(git -C "$root" merge-base HEAD "$ref" 2>/dev/null || true)"
        base_date=""
        [ -z "$base" ] || base_date="$(git -C "$root" log -1 --format=%cs "$base" 2>/dev/null || true)"
        verdict="It is this checkout's committed copy (HEAD), and HEAD is ${behind} commits behind ${ref_name}"
        [ -z "$base_date" ] || verdict="${verdict}; they last shared a commit on ${base_date}"
        verdict="${verdict}."
    elif [ "$probe_ok" = 1 ] && git -C "$root" cat-file -e "$tree_blob" 2>/dev/null; then
        verdict="It differs from HEAD's copy too (git status shows it modified), yet it is a blob this repository already holds: a committed revision checked out here at some point, not a hand edit."
    elif [ "$probe_ok" = 1 ]; then
        verdict="It differs from HEAD's copy too and matches no object this repository holds: uncommitted local edits."
    else
        verdict="It differs from HEAD's copy too (git status shows it modified)."
    fi

    cat <<EOF
── ${file} drift ──
The ${file} in this session's context is the working-tree copy at ${root}, and it is not ${ref_name}'s.
${verdict}
${ref_name} as fetched here: ${tip}. This hook never fetches.
Facts quoted from it (CI routing, gates, workflow) may be stale. Before relying on one, read the current contract:
    git show ${ref_name}:${file}
    git diff ${ref_name} -- ${file}
EOF
done
exit 0
