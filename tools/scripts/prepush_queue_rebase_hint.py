#!/usr/bin/env python3
"""Hint when a push only re-bases an auto-merge-armed or queued PR onto main.

The merge queue tests every entry on top of current `main`, so an armed or
queued PR never needs a hand refresh. Moving its head anyway cancels the
running required `macos` gate and makes the PR wait for a fresh one, while the
queue re-tests the merge result regardless.

Runs from `.githooks/pre-push`, before `PULP_SKIP_PREPUSH` is honoured (a
post-rebase `--force-with-lease` push is the case it exists for). It reads Git's
pushed-ref records on stdin and, for each branch update other than `main`:

1. decides LOCALLY whether the push carries no content change: the PR's diff
   against its own base on main (``merge-base(head, origin/main)..head``) has
   the same stable patch-id before and after, and that base moved. That covers
   a rebase onto main and a merge of main. A push that only adds main merges on
   top of the old head is left to ``refresh_push_check.py``, which already warns
   about it, so the two never print for the same push;
2. only then asks GitHub (one GraphQL call via ``ghapp`` or ``gh``, bounded by
   ``PULP_QUEUE_REBASE_TIMEOUT`` seconds, default 3) whether the branch's open
   PR has ``autoMergeRequest`` or ``mergeQueueEntry`` set. REST ``auto_merge``
   reads null for a PR the queue already holds, so GraphQL is the source;
3. prints a short hint when it is armed or queued.

ALWAYS ADVISORY: exits 0. Any failure (no ``gh``, no auth, network, timeout,
unexpected JSON, a Git error) is silent. ``PULP_ALLOW_QUEUE_REBASE=1`` silences
the hint.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ZERO_SHA = "0" * 40
MAIN_REF = "refs/remotes/origin/main"
DEFAULT_REPO = ("Generous-Corp", "pulp")
DEFAULT_TIMEOUT_S = 3.0

PR_QUERY = """
query($owner:String!,$name:String!,$branch:String!){
  repository(owner:$owner,name:$name){
    pullRequests(headRefName:$branch,states:OPEN,first:1){nodes{
      number headRefOid
      autoMergeRequest{enabledAt}
      mergeQueueEntry{state position}}}}}
"""


def git(root: Path, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(root), *args],
        capture_output=True,
        text=True,
        input=stdin,
    )


def is_commit(root: Path, rev: str) -> bool:
    return git(root, "rev-parse", "--verify", "--quiet", rev + "^{commit}").returncode == 0


def is_ancestor(root: Path, a: str, b: str) -> bool:
    return git(root, "merge-base", "--is-ancestor", a, b).returncode == 0


def merge_base(root: Path, a: str, b: str) -> str | None:
    res = git(root, "merge-base", a, b)
    return res.stdout.strip() if res.returncode == 0 and res.stdout.strip() else None


def patch_id(root: Path, base: str, head: str) -> str | None:
    diff = git(root, "diff", "--binary", "--no-color", "--no-ext-diff", base, head)
    if diff.returncode != 0:
        return None
    if not diff.stdout.strip():
        return ""
    pid = git(root, "patch-id", "--stable", stdin=diff.stdout)
    if pid.returncode != 0 or not pid.stdout.split():
        return None
    return pid.stdout.split()[0]


def only_main_merges_added(root: Path, old: str, new: str) -> bool:
    """New head = old head plus merges whose other parent is on main, nothing else."""
    commits = git(root, "rev-list", "--parents", new, "^" + old, "^" + MAIN_REF).stdout.splitlines()
    if not commits:
        return False
    for line in commits:
        shas = line.split()
        ps = shas[1:]
        if len(ps) < 2 or not any(is_ancestor(root, p, MAIN_REF) for p in ps):
            return False
    return True


def content_neutral_refresh(root: Path, old: str, new: str) -> bool:
    """True when `new` carries the same PR diff as `old`, re-based on newer main."""
    if not (is_commit(root, old) and is_commit(root, new) and is_commit(root, MAIN_REF)):
        return False
    old_base = merge_base(root, old, MAIN_REF)
    new_base = merge_base(root, new, MAIN_REF)
    if not old_base or not new_base or old_base == new_base:
        return False  # the base did not move: not a refresh onto main
    if not is_ancestor(root, old_base, new_base):
        return False
    before = patch_id(root, old_base, old)
    after = patch_id(root, new_base, new)
    if not before or before != after:
        return False  # unreadable, empty (nothing to protect), or changed content
    if is_ancestor(root, old, new) and only_main_merges_added(root, old, new):
        return False  # refresh_push_check.py owns a plain merge-of-main push
    return True


def parse_repo(url: str) -> tuple[str, str]:
    m = re.search(r"github\.com[:/]+([^/]+)/([^/]+?)(?:\.git)?/?$", url or "")
    return (m.group(1), m.group(2)) if m else DEFAULT_REPO


def gh_binary() -> str | None:
    override = os.environ.get("PULP_QUEUE_REBASE_GH")
    if override:
        return override
    return shutil.which("ghapp") or shutil.which("gh")


def armed_pr(owner: str, name: str, branch: str) -> dict | None:
    """Return {"number", "state"} for an armed/queued open PR, else None (fail open)."""
    binary = gh_binary()
    if not binary:
        return None
    try:
        timeout = float(os.environ.get("PULP_QUEUE_REBASE_TIMEOUT", DEFAULT_TIMEOUT_S))
    except ValueError:
        timeout = DEFAULT_TIMEOUT_S
    cmd = [
        binary, "api", "graphql",
        "-f", "query=" + PR_QUERY,
        "-f", f"owner={owner}", "-f", f"name={name}", "-f", f"branch={branch}",
    ]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        if res.returncode != 0:
            return None
        data = json.loads(res.stdout)
        nodes = data["data"]["repository"]["pullRequests"]["nodes"]
    except Exception:
        return None
    if not nodes:
        return None
    pr = nodes[0] or {}
    if pr.get("mergeQueueEntry"):
        return {"number": pr.get("number"), "state": "in the merge queue"}
    if pr.get("autoMergeRequest"):
        return {"number": pr.get("number"), "state": "auto-merge armed"}
    return None


def hint(branch: str, pr: dict) -> None:
    print(
        "\n".join(
            [
                "",
                f"ℹ pre-push: PR #{pr['number']} ({branch}) is {pr['state']}, and this push only re-bases it onto main.",
                "   The merge queue tests the PR on top of current main already; moving the head cancels the",
                "   running `macos` gate. Keep the old head and enqueue with `shipyard ship --pr "
                f"{pr['number']}`. Silence: PULP_ALLOW_QUEUE_REBASE=1",
                "",
            ]
        ),
        file=sys.stderr,
    )


def main(argv: list[str]) -> int:
    if os.environ.get("PULP_ALLOW_QUEUE_REBASE") == "1":
        return 0
    import argparse

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--remote-url", default="")
    ap.add_argument("--root", default=None)
    args = ap.parse_args(argv)
    root = Path(args.root) if args.root else Path.cwd()
    owner, name = parse_repo(args.remote_url)

    for line in sys.stdin.read().splitlines():
        fields = line.split()
        if len(fields) != 4:
            continue
        _local_ref, local_sha, remote_ref, remote_sha = fields
        if ZERO_SHA in (local_sha, remote_sha) or not remote_ref.startswith("refs/heads/"):
            continue
        branch = remote_ref[len("refs/heads/"):]
        if branch == "main" or local_sha == remote_sha:
            continue
        if not content_neutral_refresh(root, remote_sha, local_sha):
            continue
        pr = armed_pr(owner, name, branch)
        if pr is not None:
            hint(branch, pr)
    return 0


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except Exception:
        pass  # advisory: a bug here must never affect a push
    sys.exit(0)
