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

The hint is ALWAYS ADVISORY: exits 0. Any failure (no ``gh``, no auth,
network, timeout, unexpected JSON, a Git error) is silent.
``PULP_ALLOW_QUEUE_REBASE=1`` silences the hint.

``--refuse-queued`` is the blocking companion the hook runs first, before any
gate and before the diff-coverage build. For every branch update whose open PR
is IN the merge queue (``mergeQueueEntry`` set) it prints why and exits 1,
whatever the push contains. A queued PR never needs a rebase or a merge of main:
the queue builds it on top of current main. Moving its head while queued is the
loop that rebuilt for minutes, had GitHub re-queue the new head, got rejected,
and dequeued again. There is no override, and ``PULP_SKIP_PREPUSH`` does not
bypass it: an override recreates that loop. The refusal names the only two ways
forward: open a new PR, or dequeue through the guarded path with a stated
reason and then push. A lookup that cannot answer (no ``gh``,
auth, network, timeout, bad JSON) fails OPEN with a one-line notice. Armed but
not yet queued is left to the advisory hint.
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


class LookupError_(Exception):
    """The PR lookup could not answer; the caller fails open."""


def lookup_pr(owner: str, name: str, branch: str) -> dict | None:
    """Return the branch's open PR node, None when there is none.

    Raises LookupError_ when the lookup itself cannot answer.
    """
    binary = gh_binary()
    if not binary:
        raise LookupError_("neither ghapp nor gh is on PATH")
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
        res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", timeout=timeout)
    except subprocess.TimeoutExpired:
        raise LookupError_(f"GitHub lookup timed out after {timeout:g}s") from None
    except OSError as exc:
        raise LookupError_(f"could not run {binary}: {exc}") from None
    if res.returncode != 0:
        detail = (res.stderr.strip().splitlines() or ["no output"])[-1]
        raise LookupError_(f"GitHub lookup failed: {detail[:160]}")
    try:
        nodes = json.loads(res.stdout)["data"]["repository"]["pullRequests"]["nodes"]
    except Exception:
        raise LookupError_("GitHub lookup returned unexpected JSON") from None
    return (nodes[0] or {}) if nodes else None


def armed_pr(owner: str, name: str, branch: str) -> dict | None:
    """Return {"number", "state"} for an armed/queued open PR, else None (fail open)."""
    try:
        pr = lookup_pr(owner, name, branch)
    except LookupError_:
        return None
    if not pr:
        return None
    if pr.get("mergeQueueEntry"):
        return {"number": pr.get("number"), "state": "in the merge queue"}
    if pr.get("autoMergeRequest"):
        return {"number": pr.get("number"), "state": "auto-merge armed"}
    return None


def refuse_queued_message(branch: str, pr: dict) -> str:
    return "\n".join(
        [
            "",
            f"x pre-push: refusing to push {branch}: PR #{pr.get('number')} is in the merge queue.",
            "   This PR is in the merge queue; a queued PR does not need a rebase, the queue merges it",
            "   on top of current main. To change it anyway, dequeue deliberately first.",
            "   There is no override for this check. The only ways forward are:",
            "     1. open a new PR for the change, or",
            "     2. dequeue through the guarded path with a stated reason, e.g.",
            "        GHAPP_ALLOW_QUEUE_REMOVAL=1 GHAPP_QUEUE_REMOVAL_REASON=defect-fix (dequeuePullRequest),",
            "        then push.",
            "",
        ]
    )


def update_records(text: str):
    """Yield (branch, local_sha, remote_sha) for each branch update other than main."""
    for line in text.splitlines():
        fields = line.split()
        if len(fields) != 4:
            continue
        _local_ref, local_sha, remote_ref, remote_sha = fields
        if ZERO_SHA in (local_sha, remote_sha) or not remote_ref.startswith("refs/heads/"):
            continue  # a deletion, or a new branch that cannot have a queued PR yet
        branch = remote_ref[len("refs/heads/"):]
        if branch == "main" or local_sha == remote_sha:
            continue
        yield branch, local_sha, remote_sha


def refuse_queued(records: str, owner: str, name: str) -> int:
    """Exit status for --refuse-queued: 1 when any pushed branch's PR is queued."""
    rc = 0
    for branch, _local_sha, _remote_sha in update_records(records):
        try:
            pr = lookup_pr(owner, name, branch)
        except LookupError_ as exc:
            print(
                f"[pre-push] queued-PR check skipped for {branch}: {exc}. "
                "Continuing; the push is not blocked.",
                file=sys.stderr,
            )
            continue
        if pr and pr.get("mergeQueueEntry"):
            print(refuse_queued_message(branch, pr), file=sys.stderr)
            rc = 1
    return rc


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
    import argparse

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], allow_abbrev=False)
    ap.add_argument("--remote-url", default="")
    ap.add_argument("--root", default=None)
    ap.add_argument(
        "--refuse-queued",
        action="store_true",
        help="exit 1 when a pushed branch's PR is in the merge queue (fails open)",
    )
    args = ap.parse_args(argv)
    owner, name = parse_repo(args.remote_url)
    records = sys.stdin.read()

    if args.refuse_queued:
        return refuse_queued(records, owner, name)

    if os.environ.get("PULP_ALLOW_QUEUE_REBASE") == "1":
        return 0
    root = Path(args.root) if args.root else Path.cwd()
    for branch, local_sha, remote_sha in update_records(records):
        if not content_neutral_refresh(root, remote_sha, local_sha):
            continue
        pr = armed_pr(owner, name, branch)
        if pr is not None:
            hint(branch, pr)
    return 0


if __name__ == "__main__":
    if "--refuse-queued" in sys.argv[1:]:
        try:
            sys.exit(main(sys.argv[1:]))
        except SystemExit:
            raise
        except Exception as exc:  # a bug here fails open, never blocks a push
            print(f"[pre-push] queued-PR check skipped: internal error: {exc}", file=sys.stderr)
            sys.exit(0)
    try:
        main(sys.argv[1:])
    except Exception:
        pass  # advisory: a bug here must never affect a push
    sys.exit(0)
