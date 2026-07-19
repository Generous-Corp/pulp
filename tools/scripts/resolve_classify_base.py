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

merge_group / workflow_dispatch / anything else
    No event-pinned base exists. `origin/main` is the right default; a merge
    group's ref is a synthetic `gh-readonly-queue/...` branch whose merged
    tree is genuinely meant to be compared against main.

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
        env: GITHUB_EVENT_NAME, PR_BASE_SHA, PUSH_BEFORE_SHA

Prints the resolved base ref to stdout. Exit code is always 0 — the base is
data, not a pass/fail signal.
"""
from __future__ import annotations

import os
import re
import sys

DEFAULT_BASE = "origin/main"
ZERO_SHA = "0" * 40
_SHA_RE = re.compile(r"\A[0-9a-fA-F]{40}\Z")

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
    *,
    default: str = DEFAULT_BASE,
) -> str:
    """Pure function: GitHub event context -> the ref to diff HEAD against."""
    event = (event_name or "").strip()
    if event in PR_EVENTS:
        return _usable_sha(pr_base_sha) or default
    if event in PUSH_EVENTS:
        return _usable_sha(push_before_sha) or default
    return default


def main() -> int:
    base = resolve_base(
        os.environ.get("GITHUB_EVENT_NAME", ""),
        os.environ.get("PR_BASE_SHA"),
        os.environ.get("PUSH_BEFORE_SHA"),
    )
    sys.stderr.write(
        f"[classify-base] event={os.environ.get('GITHUB_EVENT_NAME', '') or '(unset)'} "
        f"base={base}\n"
    )
    print(base)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
