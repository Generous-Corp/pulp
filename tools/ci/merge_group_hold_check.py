#!/usr/bin/env python3
"""Fail a merge group, before any VM, when a queued pull request is held.

A `verdict: HOLD head:<sha>` posted on a pull request that was already armed
or queued did not stop it: six pull requests merged on 2026-10-12/13 minutes
after a HOLD naming their merge head. Shipyard's `verdict-hold-hook` now
dequeues or disarms such a head when the HOLD posts; this is the backstop for
a HOLD it could not act on (posted outside `ghapp`, on a host without the
hook, or while the queue was already building the group).

The merge-group commit is a chain of merge commits, one per queued pull
request: walking first parents from the group head back to
`merge_group.base_sha`, each commit's second parent is the head that pull
request lands, and its subject names the pull request (`Merge pull request
#N from ...`). For each of them the check reads the pull request's comments
and reviews and takes the latest trusted verdict naming that head: a comment
whose last non-blank line is `verdict: APPROVE|HOLD head:<sha>` or
`reviewed: <sha>`, or an APPROVED / CHANGES_REQUESTED review on that commit,
from the fleet App (`shipyard-local`) or an OWNER, MEMBER or COLLABORATOR who
is not the pull request's distinguishable author. This is Shipyard's
`head_approval::trusted_reviewer` rule; `test_merge_group_hold_check.py`
reads the same captured answer Shipyard's scorecard tests read and must agree.

A HOLD as the latest trusted verdict fails the step, naming the pull request,
the HOLD's URL and the fix. Anything it cannot read passes with a warning
annotation: the backstop must never stop the queue on an API hiccup, and the
hook remains the primary layer.

Usage (in the merge_group preamble):
    python3 tools/ci/merge_group_hold_check.py --repo OWNER/REPO \\
        --base "$MERGE_GROUP_BASE_SHA" --head "$GITHUB_SHA"
with GH_TOKEN (or GITHUB_TOKEN) in the environment. Exit 0 pass, 1 held.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from typing import Any, Callable, Dict, List, Optional, Tuple

API = "https://api.github.com"
MERGE_SUBJECT = re.compile(r"^Merge pull request #(\d+) from ")
MARKER = re.compile(
    r"^\s*(?:verdict:\s*(APPROVE|HOLD)\s+head:\s*([0-9a-fA-F]{40})|reviewed:\s*([0-9a-fA-F]{40}))\s*$"
)
TRUSTED_ASSOCIATIONS = ("OWNER", "MEMBER", "COLLABORATOR")
FLEET_APP = "shipyard-local"
VERDICTS_QUERY = (
    "query($owner:String!,$name:String!,$number:Int!){repository(owner:$owner,name:$name)"
    "{pullRequest(number:$number){headRefOid author{login} comments(last:100){totalCount"
    " nodes{author{login} authorAssociation body createdAt url}} reviews(last:100){totalCount"
    " nodes{author{login} authorAssociation state submittedAt url commit{oid}}}}}}"
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
            "User-Agent": "pulp-merge-group-hold-check",
        })
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode())
    return fetch


def normalized(login: str) -> str:
    login = login[len("app/"):] if login.startswith("app/") else login
    return (login[: -len("[bot]")] if login.endswith("[bot]") else login).lower()


def trusted(login: Optional[str], association: Optional[str], author: Optional[str]) -> bool:
    if not login:
        return False
    fleet = normalized(login) == FLEET_APP
    if not (fleet or association in TRUSTED_ASSOCIATIONS):
        return False
    return author is None or normalized(author) != normalized(login) \
        or normalized(author) == FLEET_APP


def verdict_line(body: str) -> Optional[Tuple[bool, str]]:
    """`(approve, head)` from a body's last non-blank line, when it is a marker."""
    lines = [line for line in body.splitlines() if line.strip()]
    match = MARKER.match(lines[-1]) if lines else None
    if not match:
        return None
    if match.group(3):
        return True, match.group(3).lower()
    return match.group(1) == "APPROVE", match.group(2).lower()


def latest_verdict(pr: Dict[str, Any], head: str) -> Optional[Dict[str, Any]]:
    """The latest trusted verdict naming `head`: `{approve, at, url}`, or None."""
    author = ((pr.get("author") or {}).get("login"))
    found: List[Dict[str, Any]] = []
    for comment in ((pr.get("comments") or {}).get("nodes") or []):
        parsed = verdict_line(comment.get("body") or "")
        login = (comment.get("author") or {}).get("login")
        if parsed and parsed[1] == head.lower() \
                and trusted(login, comment.get("authorAssociation"), author):
            found.append({"approve": parsed[0], "at": comment.get("createdAt") or "",
                          "url": comment.get("url")})
    for review in ((pr.get("reviews") or {}).get("nodes") or []):
        state = review.get("state")
        if state not in ("APPROVED", "CHANGES_REQUESTED"):
            continue
        login = (review.get("author") or {}).get("login")
        if ((review.get("commit") or {}).get("oid") or "").lower() == head.lower() \
                and trusted(login, review.get("authorAssociation"), author):
            found.append({"approve": state == "APPROVED", "at": review.get("submittedAt") or "",
                          "url": review.get("url")})
    # The newest wins; at the same time a HOLD wins.
    return max(found, key=lambda item: (item["at"], not item["approve"]), default=None)


def group_entries(fetch: Fetch, repo: str, base: str, head: str) -> List[Tuple[int, str]]:
    """`(pull request, head it lands)` for each queued pull request in the group."""
    compare = fetch("GET", f"repos/{repo}/compare/{base}...{head}", None)
    commits = {commit["sha"]: commit for commit in compare.get("commits") or []}
    entries: List[Tuple[int, str]] = []
    sha = head
    while sha != base and sha in commits:
        commit = commits[sha]
        parents = [parent["sha"] for parent in commit.get("parents") or []]
        subject = (commit.get("commit") or {}).get("message", "").split("\n", 1)[0]
        match = MERGE_SUBJECT.match(subject)
        if len(parents) != 2 or not match:
            raise ValueError(f"commit {sha[:12]} on the group's first-parent chain is not a "
                             f"queue merge commit ({subject!r})")
        entries.append((int(match.group(1)), parents[1]))
        sha = parents[0]
    if sha != base:
        raise ValueError(f"the group head does not reach {base[:12]} through its first parents")
    return entries


def check(fetch: Fetch, repo: str, base: str, head: str) -> Tuple[List[str], List[str]]:
    """`(holds, warnings)`: one message per held pull request, and what was unread."""
    holds: List[str] = []
    warnings: List[str] = []
    try:
        entries = group_entries(fetch, repo, base, head)
    except (ValueError, KeyError, OSError, urllib.error.URLError) as error:
        return [], [f"the merge group's pull requests could not be read: {error}"]
    owner, _, name = repo.partition("/")
    for number, pr_head in entries:
        try:
            answer = fetch("POST", "graphql", {"query": VERDICTS_QUERY, "variables": {
                "owner": owner, "name": name, "number": number}})
            pr = answer["data"]["repository"]["pullRequest"]
        except (KeyError, TypeError, OSError, urllib.error.URLError) as error:
            warnings.append(f"#{number}: its verdicts could not be read ({error})")
            continue
        verdict = latest_verdict(pr, pr_head)
        if verdict is not None and not verdict["approve"]:
            holds.append(
                f"#{number} is held: the latest trusted verdict on {pr_head} is HOLD "
                f"({verdict['url']}). Fix: push a new head that addresses the HOLD and get a "
                f"`verdict: APPROVE head:<new head>`, or post that APPROVE on {pr_head[:12]} "
                f"if the HOLD no longer applies; then re-queue it. "
                f"https://github.com/{repo}/pull/{number}"
            )
    return holds, warnings


def main(argv: Optional[List[str]] = None, fetch: Optional[Fetch] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", required=True)
    parser.add_argument("--base", required=True, help="merge_group.base_sha")
    parser.add_argument("--head", required=True, help="the merge group commit")
    args = parser.parse_args(argv)
    if fetch is None:
        token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if not token:
            print("::warning title=merge-group HOLD check::no GH_TOKEN; not checked")
            return 0
        fetch = github(token)
    holds, warnings = check(fetch, args.repo, args.base.lower(), args.head.lower())
    for warning in warnings:
        print(f"::warning title=merge-group HOLD check::{warning}; not checked (the "
              "verdict-hold hook is the primary layer)")
    for hold in holds:
        print(f"::error title=merge-group HOLD check::{hold}")
    if not holds:
        print("merge-group HOLD check: no queued pull request is held")
    return 1 if holds else 0


if __name__ == "__main__":
    sys.exit(main())
