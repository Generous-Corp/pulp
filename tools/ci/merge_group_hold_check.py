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
`reviewed: <sha>`, or an APPROVED / CHANGES_REQUESTED review on that commit.

An APPROVE counts only from the fleet App (`shipyard-local`), the identity
every agent posts verdicts as through `ghapp`. A human account's APPROVE never
counts here, whatever its association: an agent holding the owner's personal
`gh` login posted APPROVED reviews and `verdict: APPROVE` comments on pull
requests it then armed, and such an APPROVE after the App's HOLD would clear
the HOLD. This matches Shipyard's default rule with no `human_reviewers`
listed. A HOLD counts from the fleet App or an OWNER, MEMBER or COLLABORATOR,
and from the pull request's own author (the author asking to hold their own
head is the conservative signal): a HOLD can only stop a landing.
`test_merge_group_hold_check.py` reads the same captured answer Shipyard's
scorecard tests read and must agree.

A HOLD as the latest trusted verdict fails the step, naming the pull request,
the HOLD's URL and the fix.

**It fails closed.** Anything it cannot read fails the step too: a read that
errors, a GraphQL answer with `errors`, a missing token, a chain that does not
reach the base, or a comment or review list it could not read in full. That is
a deliberate ruling: a stalled queue is the lesser harm, since a re-run of the
merge group's `classify` job (or a re-queue) retries, while a pass on input it
cannot judge is exactly how a held head lands. Each message names the pull
request (or the group), the error and the fix.

The chain is walked one commit at a time (`git/commits/<sha>`), so a group of
any size is read in full; the compare API's 250-commit cap would hide the
newest entries. Comments and reviews are paginated to their `totalCount`.

Usage (in the merge_group preamble):
    python3 tools/ci/merge_group_hold_check.py --repo OWNER/REPO \\
        --base "$MERGE_GROUP_BASE_SHA" --head "$GITHUB_SHA"
with GH_TOKEN (or GITHUB_TOKEN) in the environment. Exit 0 pass, 1 held or
unreadable.
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
PAGE_SIZE = 100
# One connection per query, paginated forward; `$after` is null on the first page.
COMMENTS_QUERY = (
    "query($owner:String!,$name:String!,$number:Int!,$after:String){repository(owner:$owner,"
    "name:$name){pullRequest(number:$number){author{login} comments(first:100,after:$after)"
    "{totalCount pageInfo{hasNextPage endCursor}"
    " nodes{author{login} authorAssociation body createdAt url}}}}}"
)
REVIEWS_QUERY = (
    "query($owner:String!,$name:String!,$number:Int!,$after:String){repository(owner:$owner,"
    "name:$name){pullRequest(number:$number){author{login} reviews(first:100,after:$after)"
    "{totalCount pageInfo{hasNextPage endCursor}"
    " nodes{author{login} authorAssociation state submittedAt url commit{oid}}}}}}"
)
# A merge queue builds at most 100 entries; anything longer is not a queue group.
MAX_ENTRIES = 512
FIX = ("re-run the merge group's classify job, or re-queue the pull requests; if the "
       "HOLD check itself is wrong, dequeue the held pull request and re-queue it after "
       "`verdict: APPROVE head:<sha>`")


class Unreadable(Exception):
    """Input the check cannot judge; the group fails closed."""

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


def trusted(login: Optional[str], association: Optional[str], author: Optional[str],
            approve: bool = True) -> bool:
    """Whether a verdict counts: an APPROVE only from the fleet App; a HOLD from
    the fleet App, an OWNER/MEMBER/COLLABORATOR, or the pull request's author."""
    if not login:
        return False
    fleet = normalized(login) == FLEET_APP
    if approve:
        return fleet
    own = author is not None and normalized(author) == normalized(login)
    return fleet or own or association in TRUSTED_ASSOCIATIONS


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
                and trusted(login, comment.get("authorAssociation"), author, parsed[0]):
            found.append({"approve": parsed[0], "at": comment.get("createdAt") or "",
                          "url": comment.get("url")})
    for review in ((pr.get("reviews") or {}).get("nodes") or []):
        state = review.get("state")
        if state not in ("APPROVED", "CHANGES_REQUESTED"):
            continue
        login = (review.get("author") or {}).get("login")
        if ((review.get("commit") or {}).get("oid") or "").lower() == head.lower() \
                and trusted(login, review.get("authorAssociation"), author,
                            state == "APPROVED"):
            found.append({"approve": state == "APPROVED", "at": review.get("submittedAt") or "",
                          "url": review.get("url")})
    # The newest wins; at the same time a HOLD wins.
    return max(found, key=lambda item: (item["at"], not item["approve"]), default=None)


def read(fetch: Fetch, method: str, path: str,
         payload: Optional[Dict[str, Any]] = None) -> Any:
    """`fetch`, with every failure (and a GraphQL `errors` answer) as Unreadable."""
    try:
        answer = fetch(method, path, payload)
    except (OSError, ValueError, urllib.error.URLError) as error:
        raise Unreadable(f"{method} {path} failed: {error}") from error
    if path == "graphql" and (not isinstance(answer, dict) or answer.get("errors")
                              or answer.get("data") is None):
        errors = answer.get("errors") if isinstance(answer, dict) else answer
        raise Unreadable(f"GraphQL answered with errors: {json.dumps(errors)[:300]}")
    return answer


def group_entries(fetch: Fetch, repo: str, base: str, head: str) -> List[Tuple[int, str]]:
    """`(pull request, head it lands)` for each queued pull request in the group.

    One `git/commits/<sha>` read per merge commit, from the group head back to
    the base, so the group is read in full whatever its size."""
    entries: List[Tuple[int, str]] = []
    sha = head
    while sha != base:
        if len(entries) >= MAX_ENTRIES:
            raise Unreadable(f"the group head does not reach {base[:12]} within "
                             f"{MAX_ENTRIES} merge commits")
        try:
            commit = read(fetch, "GET", f"repos/{repo}/git/commits/{sha}")
            parents = [parent["sha"].lower() for parent in commit.get("parents") or []]
            subject = (commit.get("message") or "").split("\n", 1)[0]
        except (KeyError, TypeError, AttributeError) as error:
            raise Unreadable(f"commit {sha[:12]} could not be read ({error!r})") from error
        match = MERGE_SUBJECT.match(subject)
        if len(parents) != 2 or not match:
            raise Unreadable(f"commit {sha[:12]} on the group's first-parent chain is not a "
                             f"queue merge commit ({subject!r}), so the chain does not reach "
                             f"{base[:12]}")
        entries.append((int(match.group(1)), parents[1]))
        sha = parents[0]
    return entries


def connection(fetch: Fetch, owner: str, name: str, number: int,
               query: str, field: str) -> Tuple[Optional[str], List[Dict[str, Any]]]:
    """`(author, every node)` of one pull-request connection, read to its totalCount."""
    nodes: List[Dict[str, Any]] = []
    after: Optional[str] = None
    author: Optional[str] = None
    total = None
    while True:
        answer = read(fetch, "POST", "graphql", {"query": query, "variables": {
            "owner": owner, "name": name, "number": number, "after": after}})
        try:
            pull = answer["data"]["repository"]["pullRequest"]
            author = ((pull.get("author") or {}).get("login"))
            page = pull[field]
            nodes.extend(page["nodes"] or [])
            total = page["totalCount"]
            more = page["pageInfo"]["hasNextPage"]
            after = page["pageInfo"]["endCursor"]
        except (KeyError, TypeError, AttributeError) as error:
            raise Unreadable(f"its {field} answer was malformed ({error!r})") from error
        if not more:
            break
        if not after or len(nodes) > total:
            raise Unreadable(f"its {field} pagination did not advance")
    if total != len(nodes):
        raise Unreadable(f"read {len(nodes)} of {total} {field}")
    return author, nodes


def pull_request(fetch: Fetch, repo: str, number: int) -> Dict[str, Any]:
    """The pull request's author and every comment and review."""
    owner, _, name = repo.partition("/")
    author, comments = connection(fetch, owner, name, number, COMMENTS_QUERY, "comments")
    _, reviews = connection(fetch, owner, name, number, REVIEWS_QUERY, "reviews")
    return {"author": {"login": author} if author else None,
            "comments": {"totalCount": len(comments), "nodes": comments},
            "reviews": {"totalCount": len(reviews), "nodes": reviews}}


def check(fetch: Fetch, repo: str, base: str, head: str) -> Tuple[List[str], List[str]]:
    """`(holds, unreadable)`: one message per held pull request, and per input it
    could not judge. Either fails the group."""
    holds: List[str] = []
    unreadable: List[str] = []
    try:
        entries = group_entries(fetch, repo, base, head)
    except Unreadable as error:
        return [], [f"merge group {head[:12]} (base {base[:12]}): its pull requests could "
                    f"not be read: {error}. Fix: {FIX}"]
    for number, pr_head in entries:
        try:
            pr = pull_request(fetch, repo, number)
        except Unreadable as error:
            unreadable.append(f"#{number}: its verdicts could not be read: {error}. Fix: {FIX}. "
                              f"https://github.com/{repo}/pull/{number}")
            continue
        verdict = latest_verdict(pr, pr_head)
        if verdict is not None and not verdict["approve"]:
            holds.append(
                f"#{number} is held: the latest trusted verdict on {pr_head} is HOLD "
                f"({verdict['url']}). Fix: push a new head that addresses the HOLD and get a "
                f"`verdict: APPROVE head:<new head>` from a reviewer through `ghapp` (the fleet "
                f"App; a human account's APPROVE does not clear a HOLD), or have that reviewer "
                f"post it on {pr_head[:12]} if the HOLD no longer applies; then re-queue it. "
                f"https://github.com/{repo}/pull/{number}"
            )
    return holds, unreadable


def main(argv: Optional[List[str]] = None, fetch: Optional[Fetch] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", required=True)
    parser.add_argument("--base", required=True, help="merge_group.base_sha")
    parser.add_argument("--head", required=True, help="the merge group commit")
    args = parser.parse_args(argv)
    if fetch is None:
        token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if not token:
            print(f"::error title=merge-group HOLD check::merge group {args.head[:12]}: no "
                  f"GH_TOKEN or GITHUB_TOKEN in the environment, so nothing could be read. "
                  f"Fix: hand the step the workflow token (GH_TOKEN=${{{{ github.token }}}}), "
                  f"then {FIX}")
            return 1
        fetch = github(token)
    holds, unreadable = check(fetch, args.repo, args.base.lower(), args.head.lower())
    for message in unreadable:
        print(f"::error title=merge-group HOLD check (unreadable)::{message}")
    for hold in holds:
        print(f"::error title=merge-group HOLD check::{hold}")
    if not holds and not unreadable:
        print("merge-group HOLD check: no queued pull request is held")
    return 1 if holds or unreadable else 0


if __name__ == "__main__":
    sys.exit(main())
