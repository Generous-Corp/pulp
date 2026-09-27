#!/usr/bin/env python3
"""Observe, and optionally police, a local `git push` of a main-refresh merge.

`.shipyard/config.toml` sets `[merge] refresh_branch = "only-if-conflicting"`:
in a merge-queue repo a PR branch is refreshed onto `main` only when the PR is
CONFLICTING, a required check on its head is failing, or the queue ejected it at
its current head. Shipyard enforces that for its own `ghapp` update-branch
calls. A local `git merge origin/main && git push` never reaches that guard,
yet it moves the PR head and so cancels any in-flight required `macos` gate
(`build.yml` cancels superseded PR runs) while the queue re-tests the merge
anyway.

This script runs from `.githooks/pre-push`. It reads Git's pushed-ref records
on stdin and, for each branch update other than `main`:

1. collects the branch's own new commits (``local ^remote ^origin/main``);
2. skips the push entirely unless one of them is a merge with a parent already
   on ``origin/main`` (the common case costs two `git rev-list` calls);
3. classifies it as a PURE refresh when every own commit is such a merge AND
   each merge's tree equals Git's clean automatic merge of its parents. A merge
   that resolved a conflict, or carried any edit beyond the automatic result,
   is not pure; neither is a push that also carries ordinary commits;
4. appends one JSONL telemetry line (always, never blocking);
5. applies ``PULP_REFRESH_PUSH_POLICY`` (``warn`` default, ``refuse``, ``off``)
   to a pure refresh of an open PR that has no justification to refresh.

Network lookups go through ``ghapp`` (or ``gh``) under one shared deadline of at
most five seconds and fail open: a lookup failure is logged and never blocks.

Exit status: 0 allow, 1 refuse (only in ``refuse`` mode without the override),
2 internal error (the hook treats that as advisory).
"""

from __future__ import annotations

import datetime as _dt
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ZERO_SHA = "0" * 40
MAIN_REF = "refs/remotes/origin/main"
DEFAULT_REPO = ("Generous-Corp", "pulp")
NETWORK_BUDGET_S = 5.0
GATE_WORKFLOW = "Build and Test"
GATE_CHECK = "macos"
FAILING_CONCLUSIONS = {
    "FAILURE",
    "TIMED_OUT",
    "CANCELLED",
    "ACTION_REQUIRED",
    "STARTUP_FAILURE",
}
FAILING_STATES = {"FAILURE", "ERROR"}
MODES = {"warn", "refuse", "off"}


# ── git ─────────────────────────────────────────────────────────────────────


def git(root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(root), *args],
        capture_output=True,
        text=True,
        check=check,
    )


def rev_exists(root: Path, rev: str) -> bool:
    return git(root, "rev-parse", "--verify", "--quiet", rev + "^{commit}", check=False).returncode == 0


def own_commits(root: Path, local_sha: str, remote_sha: str) -> list[str]:
    """Commits this push adds to the branch that are not already on main."""
    args = ["rev-list", local_sha]
    if remote_sha != ZERO_SHA and rev_exists(root, remote_sha):
        args.append("^" + remote_sha)
    if rev_exists(root, MAIN_REF):
        args.append("^" + MAIN_REF)
    out = git(root, *args).stdout.split()
    return out


def parents(root: Path, sha: str) -> list[str]:
    return git(root, "rev-list", "--parents", "-n", "1", sha).stdout.split()[1:]


def on_main(root: Path, sha: str) -> bool:
    if not rev_exists(root, MAIN_REF):
        return False
    return git(root, "merge-base", "--is-ancestor", sha, MAIN_REF, check=False).returncode == 0


def is_main_merge(root: Path, sha: str, ps: list[str]) -> bool:
    return len(ps) >= 2 and any(on_main(root, p) for p in ps)


def merge_is_pure(root: Path, sha: str, ps: list[str]) -> bool:
    """True when the merge's tree is exactly Git's clean automatic merge."""
    if len(ps) != 2:
        return False
    tree = git(root, "rev-parse", sha + "^{tree}").stdout.strip()
    res = git(root, "merge-tree", "--write-tree", "--no-messages", ps[0], ps[1], check=False)
    if res.returncode == 0:
        return res.stdout.split()[0] == tree if res.stdout.split() else False
    if res.returncode == 1:
        return False  # the automatic merge conflicts: the pushed tree resolved it
    # Git without `merge-tree --write-tree`: fall back to the combined diff,
    # which lists every path that differs from ALL parents (an evil or
    # conflict-resolved hunk). Empty means nothing beyond the parents' content.
    cc = git(root, "diff-tree", "--cc", "--name-only", "--no-commit-id", sha, check=False)
    return cc.returncode == 0 and not cc.stdout.strip()


def classify(root: Path, local_sha: str, remote_sha: str) -> dict | None:
    commits = own_commits(root, local_sha, remote_sha)
    main_merges: list[str] = []
    pure_merges = 0
    others = 0
    for sha in commits:
        ps = parents(root, sha)
        if is_main_merge(root, sha, ps):
            main_merges.append(sha)
            if merge_is_pure(root, sha, ps):
                pure_merges += 1
        else:
            others += 1
    if not main_merges:
        return None
    return {
        "main_merges": len(main_merges),
        "other_commits": others,
        "pure_refresh": others == 0 and pure_merges == len(main_merges),
        "merge_sha": main_merges[0],
    }


# ── repository policy ───────────────────────────────────────────────────────


def repo_policy(root: Path) -> str:
    cfg = root / ".shipyard" / "config.toml"
    try:
        text = cfg.read_text(encoding="utf-8")
    except OSError:
        return "unset"
    try:
        import tomllib

        return str(tomllib.loads(text).get("merge", {}).get("refresh_branch", "unset"))
    except Exception:
        m = re.search(r'^\s*refresh_branch\s*=\s*"([^"]+)"', text, re.M)
        return m.group(1) if m else "unset"


def parse_repo(url: str) -> tuple[str, str]:
    m = re.search(r"github\.com[:/]+([^/]+)/([^/]+?)(?:\.git)?/?$", url or "")
    return (m.group(1), m.group(2)) if m else DEFAULT_REPO


# ── GitHub lookup (bounded, fail open) ──────────────────────────────────────

PR_QUERY = """
query($owner:String!,$name:String!,$branch:String!){
  repository(owner:$owner,name:$name){
    pullRequests(headRefName:$branch,states:OPEN,first:1){nodes{
      number mergeable headRefOid
      timelineItems(itemTypes:[REMOVED_FROM_MERGE_QUEUE_EVENT],last:10){nodes{
        ... on RemovedFromMergeQueueEvent{beforeCommit{oid}}}}
      commits(last:1){nodes{commit{statusCheckRollup{contexts(first:100){nodes{
        __typename
        ... on CheckRun{name status conclusion
          checkSuite{workflowRun{workflow{name}}}}
        ... on StatusContext{context state}}}}}}}}}}}
"""

REQUIRED_QUERY = """
query($owner:String!,$name:String!,$number:Int!){
  repository(owner:$owner,name:$name){pullRequest(number:$number){
    commits(last:1){nodes{commit{statusCheckRollup{contexts(first:100){nodes{
      __typename
      ... on CheckRun{name conclusion isRequired(pullRequestNumber:$number)}
      ... on StatusContext{context state isRequired(pullRequestNumber:$number)}}}}}}}}}
"""


class LookupError_(Exception):
    pass


def gh_binary() -> str | None:
    override = os.environ.get("PULP_REFRESH_PUSH_GH")
    if override:
        return override
    return shutil.which("ghapp") or shutil.which("gh")


def graphql(binary: str, query: str, variables: dict, deadline: float) -> dict:
    remaining = deadline - time.monotonic()
    if remaining <= 0.05:
        raise LookupError_("timeout")
    cmd = [binary, "api", "graphql", "-f", "query=" + query]
    for key, value in variables.items():
        cmd += ["-F" if isinstance(value, int) else "-f", f"{key}={value}"]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=remaining)
    except subprocess.TimeoutExpired:
        raise LookupError_("timeout")
    except OSError as exc:
        raise LookupError_(f"exec:{exc.errno}")
    if res.returncode != 0:
        raise LookupError_(f"exit:{res.returncode}")
    try:
        data = json.loads(res.stdout)
    except json.JSONDecodeError:
        raise LookupError_("bad-json")
    if not isinstance(data, dict) or data.get("errors") or "data" not in data:
        raise LookupError_("graphql-error")
    return data["data"]


def _contexts(pr_or_node: dict) -> list[dict]:
    try:
        commit = pr_or_node["commits"]["nodes"][0]["commit"]
        rollup = commit.get("statusCheckRollup") or {}
        return (rollup.get("contexts") or {}).get("nodes") or []
    except (KeyError, IndexError, TypeError):
        return []


def _failed(ctx: dict) -> bool:
    if ctx.get("__typename") == "CheckRun":
        return (ctx.get("conclusion") or "") in FAILING_CONCLUSIONS
    return (ctx.get("state") or "") in FAILING_STATES


def lookup_pr(owner: str, name: str, branch: str) -> dict:
    """Return PR facts, or {"lookup": "<reason>"} on any failure (fail open)."""
    if os.environ.get("PULP_REFRESH_PUSH_OFFLINE") == "1":
        return {"lookup": "skipped-offline"}
    binary = gh_binary()
    if not binary:
        return {"lookup": "failed:no-gh"}
    budget = min(NETWORK_BUDGET_S, float(os.environ.get("PULP_REFRESH_PUSH_TIMEOUT", NETWORK_BUDGET_S)))
    deadline = time.monotonic() + budget
    try:
        data = graphql(binary, PR_QUERY, {"owner": owner, "name": name, "branch": branch}, deadline)
        nodes = (((data.get("repository") or {}).get("pullRequests") or {}).get("nodes")) or []
        if not nodes:
            return {"lookup": "no-pr"}
        pr = nodes[0]
        head = pr.get("headRefOid")
        contexts = _contexts(pr)
        gate_in_flight = any(
            c.get("__typename") == "CheckRun"
            and c.get("name") == GATE_CHECK
            and (((c.get("checkSuite") or {}).get("workflowRun") or {}).get("workflow") or {}).get("name")
            == GATE_WORKFLOW
            and c.get("status") != "COMPLETED"
            for c in contexts
        )
        ejected = any(
            ((n or {}).get("beforeCommit") or {}).get("oid") == head
            for n in ((pr.get("timelineItems") or {}).get("nodes") or [])
        )
        facts = {
            "lookup": "ok",
            "pr": pr.get("number"),
            "mergeable": pr.get("mergeable"),
            "gate_in_flight": gate_in_flight,
            "ejected_at_head": ejected,
            "failing_required": False,
        }
        if any(_failed(c) for c in contexts):
            # Only a failing check can make a refresh justified, so the second
            # (required-ness) query runs only then. If it cannot finish inside
            # the budget, any failing check counts: that errs toward silence.
            try:
                req = graphql(
                    binary, REQUIRED_QUERY, {"owner": owner, "name": name, "number": int(pr["number"])}, deadline
                )
                req_ctx = _contexts(((req.get("repository") or {}).get("pullRequest")) or {})
                facts["failing_required"] = any(_failed(c) and c.get("isRequired") for c in req_ctx)
            except LookupError_ as exc:
                facts["failing_required"] = True
                facts["lookup"] = f"partial:{exc}"
        return facts
    except LookupError_ as exc:
        return {"lookup": f"failed:{exc}"}
    except Exception as exc:  # never let a lookup bug block a push
        return {"lookup": f"failed:{type(exc).__name__}"}


# ── decision + telemetry ────────────────────────────────────────────────────


def log_path() -> Path:
    override = os.environ.get("PULP_REFRESH_PUSH_LOG")
    if override:
        return Path(override)
    state = os.environ.get("XDG_STATE_HOME") or str(Path.home() / ".local" / "state")
    return Path(state) / "pulp" / "refresh-pushes.jsonl"


def append_log(record: dict) -> None:
    try:
        path = log_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(record, sort_keys=True) + "\n")
    except OSError:
        pass  # telemetry never blocks


def decide(policy: str, mode: str, cls: dict, facts: dict) -> str:
    if not cls["pure_refresh"]:
        return "allowed:not-pure"
    if mode == "off":
        return "off"
    if policy != "only-if-conflicting":
        return "allowed:repo-policy"
    lookup = facts.get("lookup", "")
    if lookup == "no-pr":
        return "allowed:no-pr"
    if not (lookup == "ok" or lookup.startswith("partial:")):
        return "allowed:lookup-failed"
    if facts.get("mergeable") == "CONFLICTING":
        return "allowed:conflicting"
    if facts.get("failing_required"):
        return "allowed:failing-required-check"
    if facts.get("ejected_at_head"):
        return "allowed:ejected-at-head"
    if mode == "refuse":
        if os.environ.get("PULP_ALLOW_REFRESH_PUSH") == "1":
            return "overridden"
        return "refused"
    return "warned"


def warn(branch: str, facts: dict, decision: str) -> None:
    pr = facts.get("pr")
    gate = "IS" if facts.get("gate_in_flight") else "may be"
    lines = [
        "",
        f"⚠ pre-push: pure main-refresh of PR #{pr} ({branch}) with no reason to refresh.",
        "   The PR is not CONFLICTING, no required check on its head is failing, and the",
        "   merge queue has not ejected it at this head. `.shipyard/config.toml` sets",
        '   [merge] refresh_branch = "only-if-conflicting".',
        f"   Cost: moving the head cancels the in-flight ~30 min `macos` gate (one {gate}",
        "   running now) and the merge queue re-tests the merge result anyway.",
        "   Instead: drop the merge (git reset --hard @{u}) and enqueue the PR as it is.",
    ]
    if decision == "refused":
        lines.append("   Refused (PULP_REFRESH_PUSH_POLICY=refuse). Override once: PULP_ALLOW_REFRESH_PUSH=1 git push")
    elif decision == "overridden":
        lines.append("   Allowed by PULP_ALLOW_REFRESH_PUSH=1.")
    else:
        lines.append("   Advisory (PULP_REFRESH_PUSH_POLICY=warn); set =refuse to block, =off to silence.")
    lines.append("")
    print("\n".join(lines), file=sys.stderr)


def main(argv: list[str]) -> int:
    import argparse

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--remote-url", default="")
    ap.add_argument("--root", default=None)
    args = ap.parse_args(argv)

    root = Path(args.root) if args.root else Path(git(Path.cwd(), "rev-parse", "--show-toplevel").stdout.strip())
    mode = os.environ.get("PULP_REFRESH_PUSH_POLICY", "warn").strip().lower() or "warn"
    if mode not in MODES:
        print(f"[pre-push] refresh-push: unknown PULP_REFRESH_PUSH_POLICY={mode!r}; using warn", file=sys.stderr)
        mode = "warn"
    policy: str | None = None
    owner, name = parse_repo(args.remote_url)
    rc = 0

    for line in sys.stdin.read().splitlines():
        fields = line.split()
        if len(fields) != 4:
            continue
        local_ref, local_sha, remote_ref, remote_sha = fields
        if local_sha == ZERO_SHA or not remote_ref.startswith("refs/heads/"):
            continue
        branch = remote_ref[len("refs/heads/"):]
        if branch == "main":
            continue
        cls = classify(root, local_sha, remote_sha)
        if cls is None:
            continue  # no merge from main in this push: nothing to observe
        if policy is None:
            policy = repo_policy(root)
        # Looked up for every merge-from-main push, pure or not, so the log
        # carries the PR and gate state for the control population too.
        facts = lookup_pr(owner, name, branch)
        decision = decide(policy, mode, cls, facts)
        if decision in {"warned", "refused", "overridden"}:
            warn(branch, facts, decision)
        if decision == "refused":
            rc = 1
        append_log(
            {
                "ts": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "repo": f"{owner}/{name}",
                "branch": branch,
                "pr": facts.get("pr"),
                "head_before": None if remote_sha == ZERO_SHA else remote_sha,
                "head_after": local_sha,
                "main_merges": cls["main_merges"],
                "other_commits": cls["other_commits"],
                "pure_refresh": cls["pure_refresh"],
                "mergeable": facts.get("mergeable"),
                "failing_required": facts.get("failing_required"),
                "ejected_at_head": facts.get("ejected_at_head"),
                "gate_in_flight": facts.get("gate_in_flight"),
                "lookup": facts.get("lookup"),
                "policy": policy,
                "mode": mode,
                "decision": decision,
                "via": "shipyard"
                if os.environ.get("SHIPYARD_PR_RUNNING") == "1" or os.environ.get("PULP_VIA_SHIPYARD") == "1"
                else "direct",
            }
        )
    return rc


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Exception as exc:  # the hook treats 2 as advisory: never block on a bug
        print(f"[pre-push] refresh-push check internal error: {exc}", file=sys.stderr)
        sys.exit(2)
