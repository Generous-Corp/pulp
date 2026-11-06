#!/usr/bin/env python3
"""Resolve the git base ref the change classifier should diff HEAD against.

`classify_changes.py --mode=diff --base <ref>` diffs `<ref>...HEAD`. WHICH
ref is correct depends entirely on the triggering event, and getting it
wrong silently breaks the classifier in one of two directions:

pull_request
    The base is the PR's target-branch tip (`github.event.pull_request.base.sha`).
    `origin/main` is a serviceable stand-in but drifts when main moves under a
    long-lived PR, so prefer the event's pinned sha.

push
    `origin/main` is WRONG and yields an EMPTY diff. On a push to main the
    checked-out HEAD *is* the new main tip, so the remote-tracking ref
    `origin/main` resolves to HEAD and `origin/main...HEAD` compares a commit
    to itself. The classifier is fail-closed (an empty diff means "build"), so
    the bug is not a wrong answer — it is a permanently useless answer that
    never distinguishes a docs merge from a core merge. The correct base is
    `github.event.before`: the branch tip the push moved away from.

merge_group
    `github.event.merge_group.base_sha` is NOT main. It is the merge group
    commit's first parent, and when other pull requests are queued ahead it is
    the PREVIOUS SPECULATIVE GROUP (refs/heads/gh-readonly-queue/main/pr-<N>-
    <base_sha>), which already contains every entry ahead of this one. A group
    that passes lands every entry ahead of it too, so diffing against that sha
    classified only the LAST pull request: a docs-only entry at the end of a
    batch reported "no native build input" and carried a red C++ entry ahead of
    it onto main.

    The base for a merge group is therefore main's own tip
    (`github.event.merge_group.base_ref`, fetched at depth 1), reached by
    walking the first-parent chain down from `base_sha`: each speculative group
    is `merge(previous group, entry)`, so the chain meets main after one step
    per entry ahead. The diff main..HEAD then holds every entry the group would
    land. If the walk cannot reach main (no base ref, a fetch fails, main moved
    off the chain, or more than MAX_QUEUE_WALK entries) the resolver prints the
    fallback, which every caller treats as "native build required".

workflow_dispatch / anything else
    No event-pinned base exists. Fall back to `origin/main`; a shallow caller
    without that ref fails closed to a native build.

FALLBACK CONTRACT
-----------------
A missing / malformed / all-zero base sha falls back to `origin/main` rather
than erroring. `github.event.before` is the all-zero sha
(0000000000000000000000000000000000000000) when a push CREATES a ref, and a
force-push can leave a sha that no longer exists locally. Falling back is safe
in the direction that matters: the resulting diff is empty or wrong-but-wide,
and `classify_changes.py` treats both as "native build required". The
optimization degrades; the build never wrongly skips.

Usage (reads the GitHub Actions context from the environment):
    python3 tools/scripts/resolve_classify_base.py
        env: GITHUB_EVENT_NAME, PR_BASE_SHA, PUSH_BEFORE_SHA,
             MERGE_GROUP_BASE_SHA, MERGE_GROUP_BASE_REF

Prints the resolved base ref to stdout. Exit code is always 0 — the base is
data, not a pass/fail signal.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
from typing import Callable, Optional

DEFAULT_BASE = "origin/main"
ZERO_SHA = "0" * 40
_SHA_RE = re.compile(r"\A[0-9a-fA-F]{40}\Z")

# More queue entries ahead than GitHub's merge queue will build at once. The
# walk stops there and fails closed rather than fetching without bound.
MAX_QUEUE_WALK = 64
_PARENT_RE = re.compile(r"^parent ([0-9a-f]{40})$", re.MULTILINE)

PR_EVENTS = ("pull_request", "pull_request_target")
PUSH_EVENTS = ("push",)


def _usable_sha(raw: str | None) -> str | None:
    """Return a 40-hex non-zero sha, or None when it cannot be trusted."""
    candidate = (raw or "").strip()
    if not candidate or candidate == ZERO_SHA:
        return None
    if not _SHA_RE.match(candidate):
        return None
    return candidate


def resolve_base(
    event_name: str,
    pr_base_sha: str | None = None,
    push_before_sha: str | None = None,
    merge_group_base_sha: str | None = None,
    *,
    default: str = DEFAULT_BASE,
) -> str:
    """Pure function: GitHub event context -> the ref to diff HEAD against."""
    event = (event_name or "").strip()
    if event in PR_EVENTS:
        return _usable_sha(pr_base_sha) or default
    if event in PUSH_EVENTS:
        return _usable_sha(push_before_sha) or default
    if event == "merge_group":
        return _usable_sha(merge_group_base_sha) or default
    return default


def merge_group_main_base(
    group_base_sha: str | None,
    main_tip_sha: str | None,
    first_parent: Callable[[str], Optional[str]],
    *,
    max_steps: int = MAX_QUEUE_WALK,
) -> str | None:
    """Walk a merge group's first-parent chain down to main's tip.

    `group_base_sha` is `merge_group.base_sha`: main itself for the first entry
    in the queue, the previous speculative group for every later one.
    `first_parent(sha)` returns a commit's first parent, or None when the
    commit cannot be read. Returns `main_tip_sha` once the chain reaches it,
    or None when it cannot be reached; the caller must then fail closed.
    """
    current = _usable_sha(group_base_sha)
    main_tip = _usable_sha(main_tip_sha)
    if current is None or main_tip is None:
        return None
    current, main_tip = current.lower(), main_tip.lower()
    for _ in range(max_steps + 1):
        if current == main_tip:
            return main_tip
        parent = _usable_sha(first_parent(current))
        if parent is None:
            return None
        current = parent.lower()
    return None


def _git(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args], capture_output=True, text=True, encoding="utf-8", check=False
    )


def _git_first_parent(sha: str) -> str | None:
    """A commit's first parent, fetching the commit at depth 1 when absent.

    `cat-file -p` prints the raw commit object, so its parent lines are
    readable in a shallow checkout that holds none of the parents.
    """
    if _git("cat-file", "-e", f"{sha}^{{commit}}").returncode != 0:
        _git("fetch", "--no-tags", "--depth=1", "origin", sha)
    shown = _git("cat-file", "-p", sha)
    if shown.returncode != 0:
        return None
    header = shown.stdout.split("\n\n", 1)[0]
    match = _PARENT_RE.search(header)
    return match.group(1) if match else None


def _git_main_tip(base_ref: str | None) -> str | None:
    """Main's current tip: `merge_group.base_ref` fetched at depth 1."""
    ref = (base_ref or "").strip()
    if not ref.startswith("refs/heads/"):
        return None
    fetched = _git("fetch", "--no-tags", "--depth=1", "origin", ref)
    if fetched.returncode != 0:
        return None
    tip = _git("rev-parse", "--verify", "FETCH_HEAD^{commit}")
    return tip.stdout.strip() if tip.returncode == 0 else None


def resolve_from_env(environ: dict[str, str] | None = None) -> str:
    env = os.environ if environ is None else environ
    event = (env.get("GITHUB_EVENT_NAME", "") or "").strip()
    base = resolve_base(
        event,
        env.get("PR_BASE_SHA"),
        env.get("PUSH_BEFORE_SHA"),
        env.get("MERGE_GROUP_BASE_SHA"),
    )
    if event != "merge_group" or base == DEFAULT_BASE:
        return base
    main_tip = _git_main_tip(env.get("MERGE_GROUP_BASE_REF"))
    resolved = merge_group_main_base(base, main_tip, _git_first_parent)
    if resolved is None:
        sys.stderr.write(
            f"[classify-base] merge group base {base} does not reach "
            f"{env.get('MERGE_GROUP_BASE_REF') or '(no base ref)'} tip "
            f"{main_tip or '(unfetched)'}; failing closed\n"
        )
        return DEFAULT_BASE
    if resolved != base.lower():
        sys.stderr.write(
            f"[classify-base] merge group is stacked on queue entries ahead: "
            f"base_sha {base} -> main {resolved}\n"
        )
    return resolved


def main() -> int:
    base = resolve_from_env()
    sys.stderr.write(
        f"[classify-base] event={os.environ.get('GITHUB_EVENT_NAME', '') or '(unset)'} "
        f"base={base}\n"
    )
    print(base)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
