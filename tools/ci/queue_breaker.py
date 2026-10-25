#!/usr/bin/env python3
"""Merge-queue circuit breaker: fail doomed merge groups before any VM.

When one test fails on main's base, every merge group the ALLGREEN queue forms
inherits it, spends a gate VM, fails, and ejects its innocent entries, while
the pull request that fixes the test waits for a VM behind them. The breaker
lets whoever opens that fix stop the waste with one API call:

    ghapp api -X POST repos/OWNER/REPO/actions/variables \\
      -f name=PULP_QUEUE_BREAKER \\
      -f value='{"test":"<ctest name>","fix_pr":<N>,"until":"<UTC ISO-8601>"}'

While the variable is set and live, the merge-group `classify` job in
`build.yml` runs this check (the protected base branch's copy, never a queue
entry's) and FAILS every group that cannot fix the test, before any VM: a
failed classify fails `macos` closed. A group is admitted when it carries
`fix_pr`, or when one of its pull requests carries the `queue-unblock` label
applied by the fleet App or a repository admin/maintainer. A blocked group is
never skipped: skipped reads as green, and green would merge untested code.

The breaker is inactive (every group proceeds) when the variable is unset,
when `until` has passed, once `fix_pr` has merged, or when the state cannot be
read. `until` must be at most `MAX_HOURS` ahead; a later one is ignored rather
than honored, so a forgotten variable cannot hold the queue for a day. Lift it
early by deleting the variable. It fails open everywhere: the breaker only
saves VMs, and a blocked queue it cannot justify is the worse outcome.

When a group is blocked, the `queue-breaker-notify` job (a separate job, so
the write token never shares a job with a queue entry's code) posts the reason
on each of the group's pull requests, once per breaker state.

`PULP_QUEUE_BREAKER_CONTROL` is the positive control: the same JSON, read by
`classify` on pull_request events and judged report-only against the pull
request itself, so a run log shows the vars channel reaching the check
(`would fail`) without blocking anything.

    queue_breaker.py check-group --repo O/R --base SHA --head SHA [--output FILE]
    queue_breaker.py check-group --repo O/R --pr N --report-only
    queue_breaker.py notify --repo O/R --prs "N M" --reason TEXT

The state is read from `QUEUE_BREAKER` (the variable's value); the token from
GH_TOKEN or GITHUB_TOKEN. Only the standard library is imported, because
`classify` runs this one file on its own.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from typing import Any, Callable, Dict, List, Optional, Tuple

API = "https://api.github.com"
UNBLOCK_LABEL = "queue-unblock"
FLEET_APP = "shipyard-local"
TRUSTED_PERMISSIONS = ("admin", "maintain")
MAX_HOURS = 3
MERGE_SUBJECT = re.compile(r"^Merge pull request #(\d+) from ")
# A merge queue builds at most 100 entries; anything longer is not a queue group.
MAX_ENTRIES = 128
TITLE = "queue-breaker"
LABEL_EVENTS_QUERY = (
    "query($owner:String!,$name:String!,$number:Int!){repository(owner:$owner,name:$name)"
    "{pullRequest(number:$number){timelineItems(last:100,itemTypes:[LABELED_EVENT])"
    "{nodes{... on LabeledEvent{label{name} actor{__typename login}}}}}}}"
)

# (method, path or "graphql", payload) -> decoded JSON
Fetch = Callable[[str, str, Optional[Dict[str, Any]]], Any]


def github(token: str) -> Fetch:
    def fetch(method: str, path: str, payload: Optional[Dict[str, Any]] = None) -> Any:
        url = f"{API}/graphql" if path == "graphql" else f"{API}/{path}"
        data = json.dumps(payload).encode() if payload is not None else None
        request = urllib.request.Request(url, data=data, method=method, headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "User-Agent": "pulp-queue-breaker",
        })
        with urllib.request.urlopen(request, timeout=30) as response:
            body = response.read().decode()
        return json.loads(body) if body else None
    return fetch


def parse_state(raw: str, now: datetime) -> Tuple[Optional[Dict[str, Any]], str]:
    """`(state, note)`: the live breaker, or None and why it is inactive."""
    if not raw.strip():
        return None, "unset"
    try:
        state = json.loads(raw)
        test = str(state["test"]).strip()
        until = datetime.fromisoformat(str(state["until"]).replace("Z", "+00:00"))
        fix = state.get("fix_pr")
        fix = None if fix in (None, "", 0) else int(fix)
    except (ValueError, TypeError, KeyError, AttributeError) as error:
        return None, f"malformed ({error!r}); expected {{test, fix_pr, until}}"
    if not test:
        return None, "malformed (empty test)"
    if until.tzinfo is None:
        return None, "malformed (until has no timezone)"
    if until <= now:
        return None, f"expired at {until.isoformat()}"
    if until > now + timedelta(hours=MAX_HOURS):
        return None, (f"ignored: until {until.isoformat()} is more than {MAX_HOURS} h ahead; "
                      f"set an until within {MAX_HOURS} h")
    return {"test": test, "fix_pr": fix, "until": until.isoformat()}, "open"


def describe(state: Dict[str, Any]) -> str:
    fix = f"#{state['fix_pr']}" if state["fix_pr"] else "no fix named"
    return f"{state['test']} is failing on main ({fix}, until {state['until']})"


def group_prs(fetch: Fetch, repo: str, base: str, head: str) -> List[int]:
    """The queued pull requests of a merge group: the queue merge commits on the
    first-parent chain from `head` back to `base`, one read per commit."""
    prs: List[int] = []
    sha = head
    while sha != base:
        if len(prs) >= MAX_ENTRIES:
            raise ValueError(f"the group head does not reach {base[:12]}")
        commit = fetch("GET", f"repos/{repo}/git/commits/{sha}", None)
        parents = [parent["sha"].lower() for parent in commit.get("parents") or []]
        match = MERGE_SUBJECT.match((commit.get("message") or "").split("\n", 1)[0])
        if len(parents) != 2 or not match:
            raise ValueError(f"commit {sha[:12]} is not a queue merge commit")
        prs.append(int(match.group(1)))
        sha = parents[0]
    return prs


def normalized(login: str) -> str:
    login = login[len("app/"):] if login.startswith("app/") else login
    return (login[: -len("[bot]")] if login.endswith("[bot]") else login).lower()


def trusted_actor(fetch: Fetch, repo: str, actor: Dict[str, Any]) -> bool:
    """The fleet App itself (a `Bot` named shipyard-local: a user can register
    that login), or a repository admin or maintainer."""
    login = actor.get("login") or ""
    if not login:
        return False
    if actor.get("__typename") == "Bot":
        return normalized(login) == FLEET_APP
    answer = fetch("GET", f"repos/{repo}/collaborators/{login}/permission", None) or {}
    return answer.get("permission") in TRUSTED_PERMISSIONS or \
        answer.get("role_name") in TRUSTED_PERMISSIONS


def unblocked(fetch: Fetch, repo: str, number: int, labels: List[str]) -> bool:
    """Whether `number` carries queue-unblock, last applied by a trusted actor."""
    if UNBLOCK_LABEL not in labels:
        return False
    owner, _, name = repo.partition("/")
    answer = fetch("POST", "graphql", {"query": LABEL_EVENTS_QUERY, "variables": {
        "owner": owner, "name": name, "number": number}})
    if not isinstance(answer, dict) or answer.get("errors"):
        raise ValueError(f"label events of #{number} unreadable")
    nodes = answer["data"]["repository"]["pullRequest"]["timelineItems"]["nodes"] or []
    applied = [n for n in nodes if (n.get("label") or {}).get("name") == UNBLOCK_LABEL]
    return bool(applied) and trusted_actor(fetch, repo, applied[-1].get("actor") or {})


def decide(fetch: Fetch, repo: str, state: Dict[str, Any], prs: List[int]) -> Tuple[str, str]:
    """`("proceed" | "admit" | "block", reason)` for a group of `prs`."""
    fix = state["fix_pr"]
    if fix:
        pull = fetch("GET", f"repos/{repo}/pulls/{fix}", None) or {}
        if pull.get("merged"):
            return "proceed", f"the fix #{fix} has merged; the breaker no longer applies"
        if fix in prs:
            return "admit", f"this group carries the fix #{fix}"
    for number in prs:
        pull = fetch("GET", f"repos/{repo}/pulls/{number}", None) or {}
        labels = [label.get("name") for label in pull.get("labels") or []]
        if unblocked(fetch, repo, number, labels):
            return "admit", f"#{number} carries {UNBLOCK_LABEL} from a trusted actor"
    listed = ", ".join(f"#{n}" for n in prs) or "no pull request"
    return "block", (
        f"{describe(state)}, and this group ({listed}) carries neither the fix nor a trusted "
        f"{UNBLOCK_LABEL} label, so it would fail that test after a full gate. Failed before "
        f"any VM. It re-queues normally once the fix merges, the breaker expires, or "
        f"PULP_QUEUE_BREAKER is deleted.")


def check_group(fetch: Fetch, repo: str, raw: str, now: datetime, base: str = "",
                head: str = "", pr: Optional[int] = None) -> Tuple[str, str, List[int]]:
    """`(decision, message, prs)`. Fails open: any read error is "proceed"."""
    state, note = parse_state(raw, now)
    if state is None:
        return "proceed", f"queue breaker: {note}; proceeding", []
    try:
        prs = [pr] if pr is not None else group_prs(fetch, repo, base.lower(), head.lower())
        decision, reason = decide(fetch, repo, state, prs)
    except (urllib.error.URLError, OSError, ValueError, KeyError, TypeError,
            AttributeError) as error:
        return "proceed", (f"queue breaker: open ({describe(state)}) but the group could not "
                           f"be judged ({error}); proceeding (fails open)"), []
    return decision, f"queue breaker: {decision}: {reason}", prs


def marker(state: Dict[str, Any]) -> str:
    return f"<!-- queue-breaker {state['test']} {state['until']} -->"


def notify(fetch: Fetch, repo: str, raw: str, now: datetime, prs: List[int],
           reason: str) -> List[int]:
    """Post `reason` on each pull request once per breaker state. Returns the
    pull requests commented on."""
    state, _ = parse_state(raw, now)
    if state is None:
        return []
    tag = marker(state)
    posted = []
    for number in prs:
        comments = fetch("GET", f"repos/{repo}/issues/{number}/comments?per_page=100", None) or []
        if any(tag in (comment.get("body") or "") for comment in comments):
            continue
        fetch("POST", f"repos/{repo}/issues/{number}/comments", {"body": (
            f"**Merge queue circuit breaker:** {reason}\n\nNothing about this pull request "
            f"caused it. Leave it queued, or re-queue it after the breaker lifts.\n\n{tag}")})
        posted.append(number)
    return posted


def write_outputs(path: str, values: Dict[str, str]) -> None:
    if not path:
        return
    with open(path, "a", encoding="utf-8") as handle:
        for key, value in values.items():
            handle.write(f"{key}={value.replace(chr(10), ' ')}\n")


def main(argv: Optional[List[str]] = None, fetch: Optional[Fetch] = None,
         now: Optional[datetime] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("command", choices=("check-group", "notify"))
    parser.add_argument("--repo", required=True)
    parser.add_argument("--base", default="")
    parser.add_argument("--head", default="")
    parser.add_argument("--pr", type=int, default=None)
    parser.add_argument("--report-only", action="store_true",
                        help="log the decision, never fail (the positive control)")
    parser.add_argument("--output", default="", help="a GITHUB_OUTPUT file")
    parser.add_argument("--prs", default="", help="notify: the blocked pull requests")
    parser.add_argument("--reason", default="", help="notify: the block's reason")
    args = parser.parse_args(argv)
    raw = os.environ.get("QUEUE_BREAKER", "")
    now = now or datetime.now(timezone.utc)
    if fetch is None:
        fetch = github(os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or "")
    if args.command == "notify":
        prs = [int(n) for n in args.prs.split()]
        try:
            posted = notify(fetch, args.repo, raw, now, prs, args.reason)
        except (urllib.error.URLError, OSError, ValueError) as error:
            print(f"::warning title={TITLE}::could not comment on the blocked pull "
                  f"requests ({error})")
            return 0
        print(f"queue breaker: commented on {', '.join(f'#{n}' for n in posted) or 'none (already told)'}")
        return 0
    decision, message, prs = check_group(fetch, args.repo, raw, now, args.base, args.head,
                                         args.pr)
    if decision == "block" and not args.report_only:
        print(f"::error title={TITLE}::{message}")
        write_outputs(args.output, {"blocked_prs": " ".join(str(n) for n in prs),
                                    "reason": message})
        return 1
    prefix = "(report-only control) would fail: " if decision == "block" else ""
    print(f"::notice title={TITLE}::{prefix}{message}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
