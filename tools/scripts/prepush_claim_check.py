#!/usr/bin/env python3
"""Fail a push when any other actor holds a live `repair-claim:` on any of the branch's open PRs.

Run by .githooks/pre-push as the last step before every allowed push. Paginated GraphQL reads;
a failed read warns and allows.

Your own claim is the one `shipyard claim post` posted from this worktree: it records
`shipyard.claim.<owner>/<repo>#<pr>.record = <id> <until> <comment-id>` in the worktree's own
git config (config.worktree). That exempts exactly one claim: the live `repair-claim:` by that id,
posted in that comment, on that pull request, not released or taken since. The same rule as
Shipyard's `shipyard claim check-push`. PULP_REPAIR_CLAIM_ID (or --own-claim), or the fleet
runner's SHIPYARD_CLAIM_OWNER, still names your own claim by id.

This is a coordination fence, not a trust boundary: the same user could write the record by
hand, as they could set the environment variable.
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


# Repair-lane IDs are either a single label (for example ``lane-a``) or the
# usual ``lane/name`` form.  Keep the grammar shared by claims and named
# releases so releasing one actor can never accidentally clear every claim.
CLAIM_ID = r"[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)?"
CLAIM = re.compile(
    rf"repair-claim:[ \t]*(?P<claim_id>{CLAIM_ID})[ \t]+until[ \t]*(?P<until>[^\r\n]*)"
)
RELEASE = re.compile(r"^repair-claim-released:(?P<tail>[^\r\n]*)", re.M)
# `repair-claim-taken: <new> from <old> reason=...` (`shipyard claim take`) moves the old
# claim, with its expiry, to the new owner, as Shipyard reads it. Both ids are read as any
# non-space run, as Shipyard does: `shipyard claim take` accepts ids such as `team@m3`.
TAKE = re.compile(r"^[ \t]*repair-claim-taken:[ \t]*(?P<owner>\S+)[ \t]+from[ \t]+(?P<source>\S+)", re.M)
RECORD_KEY = re.compile(r"^shipyard\.claim\.(?P<repo>[^#\s]+)#(?P<pr>\d+)\.record$")
# Environment that points git at another repository than the cwd's; a hook exports GIT_DIR.
GIT_REPOSITORY_ENV = ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE", "GIT_PREFIX",
                      "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT")


def warning(message: str) -> int:
    print(f"⚠ pre-push: claim check {message}; continuing (CI and the repair lane still check).", file=sys.stderr)
    return 0


def parse_until(date: str, clock: str) -> dt.datetime:
    return dt.datetime.fromisoformat(f"{date}T{clock}").replace(tzinfo=dt.timezone.utc)


PR_QUERY = """query($owner:String!,$name:String!,$branch:String!,$prsCursor:String){
  repository(owner:$owner,name:$name){
    pullRequests(headRefName:$branch,states:OPEN,first:100,after:$prsCursor){
      nodes{number comments(first:100){nodes{body fullDatabaseId} pageInfo{hasNextPage endCursor}}}
      pageInfo{hasNextPage endCursor}
    }
  }
}"""
COMMENTS_QUERY = """query($owner:String!,$name:String!,$number:Int!,$commentsCursor:String!){
  repository(owner:$owner,name:$name){
    pullRequest(number:$number){
      comments(first:100,after:$commentsCursor){nodes{body fullDatabaseId} pageInfo{hasNextPage endCursor}}
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
        if not isinstance(payload, dict):
            raise ValueError("response is not an object")
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


def worktree_records() -> dict[tuple[str, int], tuple[str, int]]:
    """The claim records in this worktree's own config.worktree: {(repo, pr): (id, comment id)}.

    A record anywhere else (the clone's shared .git/config, a global, a -c) is not this
    worktree's and does not count; --show-scope says which file each entry came from.
    """
    env = {key: value for key, value in os.environ.items() if key not in GIT_REPOSITORY_ENV}
    try:
        result = subprocess.run(
            ["git", "config", "--show-scope", "--get-regexp", r"^shipyard\.claim\..*\.record$"],
            check=False, capture_output=True, text=True, encoding="utf-8", timeout=10, env=env)
    except (OSError, subprocess.SubprocessError):
        return {}
    records = {}
    for line in result.stdout.splitlines() if result.returncode == 0 else []:
        scope, _, entry = line.partition("\t")
        key, _, value = entry.partition(" ")
        match = RECORD_KEY.match(key)
        fields = value.split()
        if scope != "worktree" or not match or len(fields) != 3 or not fields[2].isdigit():
            continue
        records[(match.group("repo").lower(), int(match.group("pr")))] = (fields[0], int(fields[2]))
    return records


def check_pr(repo: str, pr: dict, own: str | None, records=None) -> int:
    # claim id -> (expiry, the comment that put the claim in that id's hands)
    claims: dict[str, tuple[dt.datetime, object]] = {}
    for comment in pr.get("comments", {}).get("nodes", []):
        if not isinstance(comment, dict) or not isinstance(comment.get("body"), str):
            raise RuntimeError("invalid GraphQL comment: expected an object with a string body")
        body = comment["body"]
        # The REST comment id (BigInt, sent as a string), which the record stores.
        full_id = str(comment.get("fullDatabaseId") or "")
        comment_id = int(full_id) if full_id.isdigit() else None
        events = [(m.start(), "claim", m) for m in CLAIM.finditer(body)]
        events += [(m.start(), "release", m) for m in RELEASE.finditer(body)]
        events += [(m.start(), "take", m) for m in TAKE.finditer(body)]
        for _, kind, match in sorted(events, key=lambda event: event[0]):
            if kind == "take":
                taken = claims.pop(match.group("source"), None)
                if taken is not None:
                    claims[match.group("owner")] = (taken[0], comment_id)
            elif kind == "claim":
                try:
                    until = match.group("until").strip()
                    # An explanation may follow a complete timestamp, separated by
                    # whitespace. Do not accept a valid prefix of a malformed token.
                    timestamp = re.match(
                        r"(\d{4}-\d\d-\d\d)[T ](\d\d:\d\d(?::\d\d)?)\s*(?:Z|UTC)(?=\s|$)",
                        until,
                    )
                    if not timestamp:
                        raise ValueError("claim expiry has invalid shape")
                    claims[match.group("claim_id")] = (parse_until(
                        timestamp.group(1), timestamp.group(2)
                    ), comment_id)
                except ValueError:
                    # Same rule as the fleet repair lane: an unreadable time counts as
                    # live, so a typo never lets a push land under someone's claim.
                    claims[match.group("claim_id")] = (dt.datetime.max.replace(
                        tzinfo=dt.timezone.utc
                    ), comment_id)
            elif kind == "release":
                tail = match.group("tail").strip()
                if not tail:
                    # Only an explicitly bare release clears every claim.
                    claims.clear()
                    continue
                release_id = tail.split(None, 1)[0]
                if re.fullmatch(CLAIM_ID, release_id):
                    claims.pop(release_id, None)
    now = dt.datetime.now(dt.timezone.utc)
    # This worktree's record exempts one claim: same id, same comment, still held and live.
    record = (records or {}).get((repo.lower(), pr["number"]))
    mine = record is not None and record[0] in claims and claims[record[0]][1] == record[1]
    if record is not None and not (mine and claims[record[0]][0] > now):
        print(f"⚠ pre-push: this worktree's claim record for {repo}#{pr['number']} names comment "
              f"{record[1]} by {record[0]}, which is not a live claim; it counts for nothing.",
              file=sys.stderr)
    live = [(claim_id, until) for claim_id, (until, _) in claims.items()
            if until > now and claim_id != own and not (mine and claim_id == record[0])]
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
    # A fleet repair lane's own claim is `repair-claim: <signer>/<tag>`, and its runner exports
    # exactly that as SHIPYARD_CLAIM_OWNER; without it the hook refused the lane's own push
    # (m3, 2026-11-19), and lanes released their claim just to push.
    own = (args.own_claim or os.environ.get("PULP_REPAIR_CLAIM_ID")
           or os.environ.get("SHIPYARD_CLAIM_OWNER"))
    if os.environ.get("PULP_SKIP_CLAIM_CHECK") == "1":
        print("⚠ pre-push: claim check skipped by PULP_SKIP_CLAIM_CHECK=1; CI and the repair lane still check.", file=sys.stderr)
        return 0
    records = worktree_records()
    try:
        for pr in read_prs(args.repo, args.branch):
            if check_pr(args.repo, pr, own, records):
                return 1
    except RuntimeError as exc:
        return warning(f"could not read ({exc})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
