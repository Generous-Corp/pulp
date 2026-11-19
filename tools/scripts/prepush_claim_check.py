#!/usr/bin/env python3
"""Fail a push when any other actor holds a live `repair-claim:` on any of the branch's open PRs.

Run by .githooks/pre-push as the last step before every allowed push. Paginated GraphQL reads;
a failed read warns and allows. PULP_REPAIR_CLAIM_ID (or --own-claim) names your own claim.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shlex
import subprocess
import sys


CLAIM = re.compile(r"repair-claim: (\S+) until (\d{4}-\d\d-\d\d)[T ](\d\d:\d\d(?::\d\d)?)\s*(?:Z|UTC)")
RELEASE = re.compile(r"^repair-claim-released:[ \t]*(?:([\w.-]+/[\w.-]+)(?![\w./-]))?", re.M)


def warning(message: str) -> int:
    print(f"⚠ pre-push: claim check {message}; continuing (CI and the repair lane still check).", file=sys.stderr)
    return 0


def parse_until(date: str, clock: str) -> dt.datetime:
    return dt.datetime.fromisoformat(f"{date}T{clock}").replace(tzinfo=dt.timezone.utc)


PR_QUERY = """query($owner:String!,$name:String!,$branch:String!,$prsCursor:String){
  repository(owner:$owner,name:$name){
    pullRequests(headRefName:$branch,states:OPEN,first:100,after:$prsCursor){
      nodes{number comments(first:100){nodes{body} pageInfo{hasNextPage endCursor}}}
      pageInfo{hasNextPage endCursor}
    }
  }
}"""
COMMENTS_QUERY = """query($owner:String!,$name:String!,$number:Int!,$commentsCursor:String!){
  repository(owner:$owner,name:$name){
    pullRequest(number:$number){
      comments(first:100,after:$commentsCursor){nodes{body} pageInfo{hasNextPage endCursor}}
    }
  }
}"""


def graphql(repo: str, query: str, **variables: str | int) -> dict:
    owner, name = repo.split("/", 1)
    command = shlex.split(os.environ.get("PULP_CLAIM_CHECK_GH", "ghapp api graphql"))
    command += ["-f", f"query={query}", "-F", f"owner={owner}", "-F", f"name={name}"]
    for key, value in variables.items():
        # Cursors and branch names are opaque strings, even when all digits.
        command += ["-F" if isinstance(value, int) else "-f", f"{key}={value}"]
    try:
        result = subprocess.run(command, check=False, capture_output=True, text=True, encoding="utf-8", timeout=15,
                                env={**os.environ, "GH_REPO": repo})  # ghapp needs exact repo provenance
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeError(str(exc)) from exc
    if result.returncode != 0:
        detail = " ".join(result.stderr.split())
        raise RuntimeError(detail or f"command exited {result.returncode}")
    try:
        payload = json.loads(result.stdout)
        if payload.get("errors"):
            raise ValueError(payload["errors"])
        repository = payload["data"]["repository"]
        if not isinstance(repository, dict):
            raise ValueError("missing repository")
        return repository
    except (ValueError, KeyError, TypeError) as exc:
        raise RuntimeError(f"invalid GraphQL response: {exc}") from exc


def next_cursor(connection: dict, seen: set[str]) -> str | None:
    info = connection["pageInfo"]
    if info["hasNextPage"] is False:
        return None
    cursor = info["endCursor"]
    if info["hasNextPage"] is not True or not isinstance(cursor, str) or not cursor or cursor in seen:
        raise ValueError("invalid or repeated pagination cursor")
    seen.add(cursor)
    return cursor


def read_prs(repo: str, branch: str):
    """Yield each PR only after reading its comments in chronological order."""
    variables: dict[str, str] = {"branch": branch}
    seen_prs: set[str] = set()
    try:
        while True:
            connection = graphql(repo, PR_QUERY, **variables)["pullRequests"]
            for pr in connection["nodes"]:
                comments = pr["comments"]
                bodies = list(comments["nodes"])
                seen_comments: set[str] = set()
                while cursor := next_cursor(comments, seen_comments):
                    comments = graphql(repo, COMMENTS_QUERY, number=pr["number"],
                                       commentsCursor=cursor)["pullRequest"]["comments"]
                    bodies.extend(comments["nodes"])
                yield {"number": pr["number"], "comments": {"nodes": bodies}}
            cursor = next_cursor(connection, seen_prs)
            if cursor is None:
                return
            variables["prsCursor"] = cursor
    except (ValueError, KeyError, TypeError) as exc:
        raise RuntimeError(f"invalid GraphQL connection: {exc}") from exc


def check_pr(repo: str, pr: dict, own: str | None) -> int:
    claims: dict[str, dt.datetime] = {}
    for comment in pr.get("comments", {}).get("nodes", []):
        body = comment.get("body", "")
        events = [(m.start(), "claim", m) for m in CLAIM.finditer(body)]
        events += [(m.start(), "release", m) for m in RELEASE.finditer(body)]
        for _, kind, match in sorted(events, key=lambda event: event[0]):
            if kind == "claim":
                try:
                    claims[match.group(1)] = parse_until(match.group(2), match.group(3))
                except ValueError:
                    # Same rule as the fleet repair lane: an unreadable time counts as
                    # live, so a typo never lets a push land under someone's claim.
                    claims[match.group(1)] = dt.datetime.max.replace(tzinfo=dt.timezone.utc)
            elif match.group(1):
                claims.pop(match.group(1), None)
            else:
                claims.clear()
    now = dt.datetime.now(dt.timezone.utc)
    live = [(claim_id, until) for claim_id, until in claims.items() if until > now and claim_id != own]
    if not live:
        return 0
    claim_id, until = sorted(live, key=lambda item: item[1], reverse=True)[0]
    until_text = until.strftime("%Y-%m-%dT%H:%M:%SZ")
    print(f"✗ pre-push: live repair-claim {claim_id} until {until_text} on {repo}#{pr['number']}; post repair-claim-released or wait", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--own-claim")
    args = parser.parse_args()
    own = args.own_claim or os.environ.get("PULP_REPAIR_CLAIM_ID")
    if os.environ.get("PULP_SKIP_CLAIM_CHECK") == "1":
        print("⚠ pre-push: claim check skipped by PULP_SKIP_CLAIM_CHECK=1; CI and the repair lane still check.", file=sys.stderr)
        return 0
    try:
        for pr in read_prs(args.repo, args.branch):
            if check_pr(args.repo, pr, own):
                return 1
    except RuntimeError as exc:
        return warning(f"could not read ({exc})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
