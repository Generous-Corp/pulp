#!/usr/bin/env python3
"""Enqueue a green release PR only when it would not delay a real PR.

release-please keeps one standing release PR per surface open and updates it
as commits land. Landing it takes a merge-queue slot and a full build, so it
goes in only when the queue for `main` holds nothing but release PRs, and the
release PR's required checks are green. Two paths bypass the idle test, never
the green one:

* the max-wait backstop: a release PR opened more than `--max-wait-hours` ago
  is enqueued even into a busy queue. release-please opens the PR at the first
  releasable merge after a release, so its age is how long that change has
  waited, and a queue that is never idle cannot starve releases;
* `--force`, the manual on-demand path (`workflow_dispatch`).

At most one release PR is in the queue at a time: while one is queued, the
other waits for the next idle window, so the two never hold two slots a real
PR could need. A release PR is recognized by its exact branch name and the
matching surface label, never by a prefix.

Immediately before enqueueing, this script proves the exact head itself: it
runs the release-PR proof (`generated_version_bump_check.py`, passed in as
`--proof-script` and loaded by the workflow from origin/main) on that head,
and enqueues only if the proof passes. It trusts no commit status for that
decision, because any workflow's token can post one. The proof also refuses
a half-built head (an SDK PR still missing its derived-file refresh, or a
commit the release bot did not sign). In Actions it runs only from main, on
a push, schedule or dispatch event.

The enqueue is `enqueuePullRequest` bound to the head this run read green
(`expectedHeadOid`), so a release-please force-push in between is refused
rather than landing unchecked bytes. The queue's own method (MERGE) applies.

    python3 tools/scripts/release_pr_enqueue.py --repo-name OWNER/REPO \\
        --proof-script PATH [--repo .] [--max-wait-hours 4] [--force] [--dry-run]
    python3 tools/scripts/release_pr_enqueue.py --repo-name OWNER/REPO --report-frozen

`--report-frozen` writes `sdk=true|false` and `plugin=true|false` to
$GITHUB_OUTPUT: whether release-please.yml must leave that surface's release
PR alone. A PR is frozen while it is in the merge queue (a rewrite would eject
it), once it reaches the backstop age, and while it is green and only waiting
for an idle queue: every rewrite restarts its checks, and at Pulp's merge rate
an always-current release PR might never be green. Changes merged meanwhile
go in the next release PR.

Runs `gh api graphql` with the caller's GH_TOKEN, which must be a user token
(RELEASE_BOT_TOKEN) so the merge group's checks are not suppressed; the proof
reads GitHub with GH_READ_TOKEN. Exit 0 when
every release PR was enqueued or deliberately left waiting; 1 when a GitHub
call failed.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

RELEASE_BRANCHES = {
    "release-please--branches--main": "sdk",
    "release-please--branches--main--components--plugin": "plugin",
}
PENDING_LABEL = "autorelease: pending"
SURFACE_LABELS = {"release: sdk": "sdk", "release: plugin": "plugin"}
# The proof runs only where this is main's own code.
TRUSTED_EVENTS = {"push", "schedule", "workflow_dispatch"}
REFRESH_SUBJECT = "chore: refresh release-derived files"
GREEN = {"SUCCESS", "NEUTRAL", "SKIPPED"}

_LIST_QUERY = """
query($owner: String!, $name: String!) {
  repository(owner: $owner, name: $name) {
    mergeQueue(branch: "main") {
      entries(first: 100) { nodes { state pullRequest { number headRefName } } }
    }
    pullRequests(states: OPEN, baseRefName: "main", labels: ["%s"], first: 20) {
      nodes {
        id number title isDraft isCrossRepository createdAt
        headRefName headRefOid mergeable body
        mergeQueueEntry { state }
        labels(first: 20) { nodes { name } }
      }
    }
  }
}
""" % PENDING_LABEL

_CHECKS_QUERY = """
query($owner: String!, $name: String!, $number: Int!, $cursor: String) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      headRefOid
      commits(last: 1) { nodes { commit { oid statusCheckRollup {
        contexts(first: 100, after: $cursor) {
          pageInfo { hasNextPage endCursor }
          nodes {
            __typename
            ... on CheckRun { name status conclusion isRequired(pullRequestNumber: $number) }
            ... on StatusContext { context state isRequired(pullRequestNumber: $number) }
          }
        }
      } } } }
    }
  }
}
"""

_ENQUEUE = """
mutation($id: ID!, $head: GitObjectID!) {
  enqueuePullRequest(input: {pullRequestId: $id, expectedHeadOid: $head}) {
    mergeQueueEntry { state position }
  }
}
"""


class GitHubError(RuntimeError):
    pass


@dataclass
class Decision:
    number: int
    action: str  # "enqueue" | "wait" | "skip"
    reason: str


def surface_of(pull: dict) -> str | None:
    names = {label["name"] for label in ((pull.get("labels") or {}).get("nodes") or [])}
    found = [surface for label, surface in SURFACE_LABELS.items() if label in names]
    return found[0] if len(found) == 1 else None


def is_release_pr(pull: dict) -> bool:
    """Exact release branch, same repository, and the label of that branch's surface."""
    surface = RELEASE_BRANCHES.get(pull.get("headRefName", ""))
    return (
        surface is not None
        and not pull.get("isCrossRepository", True)
        and surface_of(pull) == surface
    )


def _is_release_entry(entry: dict) -> bool:
    return (entry.get("pullRequest") or {}).get("headRefName", "") in RELEASE_BRANCHES


def required_checks_green(contexts: list[dict]) -> tuple[bool, str]:
    """Green iff at least one required context exists and every one passed."""
    required = [c for c in contexts if c.get("isRequired")]
    if not required:
        return False, "no required check has reported on the head yet"
    for context in required:
        if context.get("__typename") == "StatusContext":
            name, state = context.get("context"), context.get("state")
            if state != "SUCCESS":
                return False, f"required status {name} is {state}"
            continue
        name = context.get("name")
        if context.get("status") != "COMPLETED":
            return False, f"required check {name} is {context.get('status')}"
        if context.get("conclusion") not in GREEN:
            return False, f"required check {name} concluded {context.get('conclusion')}"
    return True, f"{len(required)} required checks green"


def decide(
    pull: dict,
    queue: list[dict],
    green: tuple[bool, str],
    now: datetime,
    max_wait_hours: float,
    force: bool,
) -> Decision:
    number = pull["number"]
    if pull.get("mergeQueueEntry"):
        return Decision(number, "skip", "already in the merge queue")
    if pull.get("isDraft"):
        return Decision(number, "skip", "draft (a deliberate release hold)")
    if pull.get("mergeable") == "CONFLICTING":
        return Decision(number, "skip", "conflicts with main; release-please rebuilds it on its next update")
    ok, why = green
    if not ok:
        return Decision(number, "wait", why)
    queued_releases = [entry for entry in queue if _is_release_entry(entry)]
    if queued_releases:
        return Decision(number, "wait", "another release PR is in the merge queue; one at a time")
    others = [entry for entry in queue if not _is_release_entry(entry)]
    if not others:
        return Decision(number, "enqueue", f"queue holds no other PR; {why}")
    if force:
        return Decision(number, "enqueue", f"manual dispatch with {len(others)} PRs queued; {why}")
    opened = datetime.fromisoformat(pull["createdAt"].replace("Z", "+00:00"))
    waited = (now - opened).total_seconds() / 3600
    if waited >= max_wait_hours:
        return Decision(
            number, "enqueue",
            f"backstop: open {waited:.1f} h >= {max_wait_hours:g} h with {len(others)} PRs queued; {why}",
        )
    return Decision(
        number, "wait",
        f"{len(others)} PRs queued; open {waited:.1f} h of the {max_wait_hours:g} h backstop",
    )


def refuse_untrusted_run(env: dict[str, str]) -> str | None:
    """Why this run may not enqueue, or None. Outside Actions nothing applies."""
    if env.get("GITHUB_ACTIONS") != "true":
        return None
    if env.get("GITHUB_REF") != "refs/heads/main":
        return f"runs only from main, not {env.get('GITHUB_REF')!r}"
    if env.get("GITHUB_EVENT_NAME") not in TRUSTED_EVENTS:
        return f"runs only on {sorted(TRUSTED_EVENTS)}, not {env.get('GITHUB_EVENT_NAME')!r}"
    return None


def _git(repo: str, *args: str) -> str:
    return subprocess.run(["git", "-C", repo, *args], check=True, capture_output=True,
                          text=True, encoding="utf-8").stdout.strip()


def prove_head(repo: str, repo_name: str, pull: dict, head: str, proof_script: str,
               run: Any = subprocess.run, read_token: str | None = None) -> tuple[bool, str]:
    """Run main's release-PR proof on exactly `head`; (passed, why)."""
    branch = pull["headRefName"]
    try:
        _git(repo, "fetch", "--quiet", "--no-tags", "origin",
             f"+refs/heads/{branch}:refs/remotes/origin/{branch}")
        if _git(repo, "rev-parse", f"refs/remotes/origin/{branch}") != head:
            return False, "head moved before the proof"
        release = head
        if _git(repo, "log", "-1", "--format=%s", head) == REFRESH_SUBJECT:
            release = _git(repo, "rev-parse", f"{head}^")
        root = _git(repo, "rev-parse", f"{release}^")
    except subprocess.CalledProcessError as error:
        return False, f"could not read the head: {(error.stderr or '').strip()}"
    names = [node["name"] for node in ((pull.get("labels") or {}).get("nodes") or [])]
    event = {"repository": {"full_name": repo_name}, "pull_request": {
        "number": pull["number"], "state": "open", "title": pull.get("title"),
        "body": pull.get("body") or "", "commits": 1 if release == head else 2,
        "labels": [{"name": name} for name in names],
        "head": {"ref": branch, "sha": head, "repo": {"full_name": repo_name}},
        "base": {"ref": "main", "sha": root},
    }}
    with tempfile.NamedTemporaryFile("w", suffix=".json", encoding="utf-8",
                                     delete=False) as handle:
        json.dump(event, handle)
    env = dict(os.environ)
    if read_token:
        env["GH_TOKEN"] = read_token
    try:
        proc = run([sys.executable, proof_script, "--repo", repo, "--event-name", "pull_request",
                    "--event-path", handle.name, "--base", root, "--head", head],
                   capture_output=True, text=True, encoding="utf-8", env=env)
    finally:
        os.unlink(handle.name)
    if proc.returncode != 0:
        reason = (proc.stderr or "").strip().splitlines()
        return False, f"release-PR proof refused: {reason[-1] if reason else proc.returncode}"
    return True, "release-PR proof passed"


def frozen(pull: dict, green: tuple[bool, str], now: datetime,
           max_wait_hours: float) -> tuple[bool, str]:
    """Whether release-please.yml must not rewrite this release PR now."""
    if pull.get("mergeQueueEntry"):
        return True, "in the merge queue"
    opened = datetime.fromisoformat(pull["createdAt"].replace("Z", "+00:00"))
    if (now - opened).total_seconds() / 3600 >= max_wait_hours:
        return True, "at the backstop age; landing it as is"
    if green[0]:
        return True, "green and waiting for an idle queue"
    return False, green[1]


class GitHub:
    def __init__(self, repo_name: str) -> None:
        self.repo_name = repo_name
        self.owner, self.name = repo_name.split("/", 1)

    def graphql(self, query: str, **variables) -> dict:
        args = ["gh", "api", "graphql", "-f", f"query={query}"]
        for key, value in variables.items():
            if value is None:
                continue
            flag = "-F" if isinstance(value, int) else "-f"
            args += [flag, f"{key}={value}"]
        proc = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
        if proc.returncode != 0:
            raise GitHubError(proc.stderr.strip() or f"gh api graphql exited {proc.returncode}")
        payload = json.loads(proc.stdout)
        if payload.get("errors"):
            raise GitHubError(json.dumps(payload["errors"]))
        return payload["data"]

    def listing(self) -> tuple[list[dict], list[dict]]:
        repo = self.graphql(_LIST_QUERY, owner=self.owner, name=self.name)["repository"]
        queue = ((repo.get("mergeQueue") or {}).get("entries") or {}).get("nodes") or []
        return queue, repo["pullRequests"]["nodes"]

    def checks(self, number: int, head: str) -> tuple[bool, str]:
        contexts: list[dict] = []
        cursor = None
        while True:
            pull = self.graphql(
                _CHECKS_QUERY, owner=self.owner, name=self.name, number=number, cursor=cursor
            )["repository"]["pullRequest"]
            nodes = pull["commits"]["nodes"]
            if pull["headRefOid"] != head or not nodes or nodes[0]["commit"]["oid"] != head:
                return False, "head moved while reading checks"
            rollup = nodes[0]["commit"].get("statusCheckRollup")
            if not rollup:
                return False, "no checks have reported on the head yet"
            page = rollup["contexts"]
            contexts.extend(page["nodes"])
            if not page["pageInfo"]["hasNextPage"]:
                return required_checks_green(contexts)
            cursor = page["pageInfo"]["endCursor"]

    def enqueue(self, pull_id: str, head: str) -> dict:
        return self.graphql(_ENQUEUE, id=pull_id, head=head)["enqueuePullRequest"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-name", required=True, help="OWNER/REPO")
    parser.add_argument("--max-wait-hours", type=float, default=4.0)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--report-frozen", action="store_true")
    parser.add_argument("--proof-script", default=None,
                        help="main's generated_version_bump_check.py (required to enqueue)")
    parser.add_argument("--repo", default=".", help="a checkout of main with full history")
    args = parser.parse_args(argv)
    if not args.report_frozen:
        refusal = refuse_untrusted_run(dict(os.environ))
        if refusal:
            print(f"release-pr-enqueue: refused: {refusal}", file=sys.stderr)
            return 1
        if not args.proof_script:
            print("release-pr-enqueue: --proof-script is required to enqueue", file=sys.stderr)
            return 2
    github = GitHub(args.repo_name)
    now = datetime.now(timezone.utc)
    try:
        queue, pulls = github.listing()
        releases = [pull for pull in pulls if is_release_pr(pull)]
        if args.report_frozen:
            held = set()
            for pull in releases:
                hold, why = frozen(pull, github.checks(pull["number"], pull["headRefOid"]),
                                   now, args.max_wait_hours)
                print(f"release-pr-enqueue: #{pull['number']} "
                      f"{'frozen' if hold else 'may update'} ({why})", file=sys.stderr)
                if hold:
                    held.add(surface_of(pull))
            lines = [f"{surface}={'true' if surface in held else 'false'}"
                     for surface in sorted(set(SURFACE_LABELS.values()))]
            print("\n".join(lines))
            output = os.environ.get("GITHUB_OUTPUT")
            if output:
                with open(output, "a", encoding="utf-8") as handle:
                    handle.write("\n".join(lines) + "\n")
            return 0
        if not releases:
            print("release-pr-enqueue: no open release PR")
            return 0
        # Oldest first: the change that has waited longest goes in first.
        for pull in sorted(releases, key=lambda p: p["createdAt"]):
            green = github.checks(pull["number"], pull["headRefOid"])
            decision = decide(pull, queue, green, now, args.max_wait_hours, args.force)
            print(f"release-pr-enqueue: #{decision.number} {pull['title']!r} at "
                  f"{pull['headRefOid'][:12]}: {decision.action} ({decision.reason})")
            if decision.action != "enqueue":
                continue
            # Prove exactly the head the enqueue will name, from main's code,
            # immediately before the mutation; expectedHeadOid then refuses
            # the enqueue if the head moves after the proof.
            proven, why = prove_head(args.repo, args.repo_name, pull, pull["headRefOid"],
                                     args.proof_script, read_token=os.environ.get("GH_READ_TOKEN"))
            print(f"release-pr-enqueue: #{decision.number}: {why}")
            if not proven:
                continue
            if not args.dry_run:
                entry = github.enqueue(pull["id"], pull["headRefOid"])
                print(f"release-pr-enqueue: #{decision.number} queued: {json.dumps(entry)}")
            # One release PR per idle window; the other waits for the next.
            break
    except GitHubError as error:
        print(f"release-pr-enqueue: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
