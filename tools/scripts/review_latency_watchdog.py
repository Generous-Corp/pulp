#!/usr/bin/env python3
"""Detect ready pull requests that have waited too long for a review verdict.

The failure this catches
------------------------
Every reviewer here is an agent, and an agent session can stop without saying
so: out of quota, rate limited, closed, or wedged on another task. When that
happens nothing turns red. Checks pass, nothing is queued, and a ready pull
request simply waits, because nothing lands on Pulp without a verdict naming
its exact head. The merge-stall guard in the same workflow does not see this:
its predicate needs auto-merge armed, and arming waits on the verdict.

The predicate (per open pull request on the base branch)
--------------------------------------------------------
A pull request is waiting too long when ALL hold:

1. **Not a draft.** A draft is not asking for review.
2. **No verdict on the current head.** No comment carries a
   ``verdict: <WORD> head:<sha>`` line whose sha names the current head (a
   full sha or a prefix of at least 7 characters), and no submitted review
   that approves or requests changes was made on the current head. A verdict
   on an earlier head does not count: a push restarts the wait.
3. **Waited past the threshold** (default 60 minutes), measured from the
   latest of: the pull request opening, it leaving draft, and its current
   head arriving. Head arrival is the creation time of the head commit's
   first check suite, which GitHub creates on push, falling back to the
   commit's committed date when no suite exists.

Who wrote the verdict comment is not checked. This is an alert, not a gate:
the merge guards decide whether a verdict counts, and a forged comment here
only silences a page for one pull request.

A failed read is itself an alarm (``blind_alarm``), as in
merge_stall_watchdog.py: a sweep that could not look must not read as "every
pull request has its verdict", and must not close the tracker.

``analyze()`` is pure: snapshot + now + threshold -> findings. ``--snapshot``
replays a recorded snapshot (tests, debugging); the default path collects one
via ``gh api graphql``.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import subprocess
import sys
import time
from typing import Any

THRESHOLD_MINUTES = 60

# `verdict: APPROVE head:<sha>`, `verdict: HOLD head:<sha>`, and any other
# verdict word. Matched per line, case-insensitively.
VERDICT_LINE = re.compile(
    r"^\s*verdict:\s*[A-Za-z_-]+\b.*?\bhead:\s*([0-9a-fA-F]{7,40})\b", re.IGNORECASE
)

# Submitted review states that are a verdict. COMMENTED is not one.
VERDICT_REVIEW_STATES = {"APPROVED", "CHANGES_REQUESTED"}

_TRANSIENT_MARKERS = ("HTTP 502", "HTTP 503", "HTTP 504", "couldn't respond")
_GH_RETRIES = 3
_GH_RETRY_SLEEP_SECONDS = 4.0


def parse_ts(value: str) -> dt.datetime:
    return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))


def verdict_heads_in(body: str) -> list[str]:
    """Every head sha a ``verdict: ... head:<sha>`` line in ``body`` names."""
    heads = []
    for line in (body or "").splitlines():
        match = VERDICT_LINE.match(line)
        if match:
            heads.append(match.group(1).lower())
    return heads


def names_head(verdict_head: str, head_oid: str) -> bool:
    return len(verdict_head) >= 7 and head_oid.lower().startswith(verdict_head.lower())


def waiting_since(pr: dict[str, Any]) -> dt.datetime:
    """When the current wait started: the latest of open, ready and head push."""
    stamps = [pr.get("created_at"), pr.get("ready_at"), pr.get("head_at")]
    return max(parse_ts(stamp) for stamp in stamps if stamp)


def analyze(
    snapshot: dict[str, Any], now: dt.datetime, threshold_minutes: float
) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    for pr in snapshot.get("prs", []):
        if pr.get("is_draft"):
            continue
        head = pr.get("head_oid") or ""
        if any(names_head(verdict, head) for verdict in pr.get("verdict_heads", [])):
            continue
        since = waiting_since(pr)
        waited = (now - since).total_seconds() / 60.0
        if waited <= threshold_minutes:
            continue
        findings.append(
            {
                "level": "review_latency_alarm",
                "number": pr["number"],
                "title": pr.get("title", ""),
                "url": pr.get("url", ""),
                "author": pr.get("author", ""),
                "head": head,
                "since": since.isoformat(),
                "waited_minutes": round(waited),
                "earlier_verdict": bool(pr.get("verdict_heads")),
            }
        )
    findings.sort(key=lambda finding: -finding["waited_minutes"])
    for error in snapshot.get("errors", []):
        findings.append({"level": "blind_alarm", "error": error})
    return findings


def _is_transient(stderr: str) -> bool:
    return any(marker in stderr for marker in _TRANSIENT_MARKERS)


def _gh(args: list[str]) -> str:
    # In Actions this is `gh` on GITHUB_TOKEN. Locally, PULP_GH_BIN=ghapp routes
    # through the Shipyard GitHub App's own rate-limit bucket.
    gh_bin = os.environ.get("PULP_GH_BIN") or "gh"
    last: subprocess.CalledProcessError | None = None
    for attempt in range(_GH_RETRIES):
        proc = subprocess.run([gh_bin, *args], capture_output=True, text=True, encoding="utf-8")
        if proc.returncode == 0:
            return proc.stdout
        last = subprocess.CalledProcessError(
            proc.returncode, [gh_bin, *args], proc.stdout, proc.stderr
        )
        if not _is_transient(proc.stderr or ""):
            break
        if attempt < _GH_RETRIES - 1:
            time.sleep(_GH_RETRY_SLEEP_SECONDS * (attempt + 1))
    assert last is not None
    raise last


# Small pages: each node carries 50 comment bodies.
_PR_QUERY = """
query($owner:String!, $name:String!, $base:String!, $cursor:String) {
  repository(owner:$owner, name:$name) {
    pullRequests(states: OPEN, baseRefName: $base, first: 20, after: $cursor,
                 orderBy: {field: CREATED_AT, direction: ASC}) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number
        title
        url
        isDraft
        createdAt
        headRefOid
        author { login }
        timelineItems(last: 1, itemTypes: [READY_FOR_REVIEW_EVENT]) {
          nodes { ... on ReadyForReviewEvent { createdAt } }
        }
        commits(last: 1) {
          nodes {
            commit {
              oid
              committedDate
              checkSuites(first: 1) { nodes { createdAt } }
            }
          }
        }
        comments(last: 50) { nodes { body } }
        reviews(last: 30) { nodes { state commit { oid } } }
      }
    }
  }
}
"""


def normalize_pr(node: dict[str, Any]) -> dict[str, Any]:
    """Reduce one GraphQL node to the fields ``analyze`` reads."""
    head = node.get("headRefOid") or ""
    commit_nodes = (node.get("commits") or {}).get("nodes") or []
    commit = (commit_nodes[0] or {}).get("commit") or {} if commit_nodes else {}
    suites = (commit.get("checkSuites") or {}).get("nodes") or []
    head_at = (suites[0] or {}).get("createdAt") if suites else None
    ready_nodes = (node.get("timelineItems") or {}).get("nodes") or []
    ready_at = (ready_nodes[-1] or {}).get("createdAt") if ready_nodes else None

    verdict_heads: list[str] = []
    for comment in (node.get("comments") or {}).get("nodes") or []:
        verdict_heads.extend(verdict_heads_in((comment or {}).get("body", "")))
    for review in (node.get("reviews") or {}).get("nodes") or []:
        review = review or {}
        oid = ((review.get("commit") or {}).get("oid") or "").lower()
        if review.get("state") in VERDICT_REVIEW_STATES and oid:
            verdict_heads.append(oid)

    return {
        "number": node.get("number"),
        "title": node.get("title", ""),
        "url": node.get("url", ""),
        "author": (node.get("author") or {}).get("login", ""),
        "is_draft": bool(node.get("isDraft")),
        "created_at": node.get("createdAt"),
        "ready_at": ready_at,
        "head_oid": head,
        "head_at": head_at or commit.get("committedDate"),
        "verdict_heads": verdict_heads,
    }


def collect_snapshot(repo: str, base: str, now: dt.datetime) -> dict[str, Any]:
    owner, name = repo.split("/", 1)
    prs: list[dict[str, Any]] = []
    errors: list[str] = []
    cursor: str | None = None
    while True:
        args = [
            "api", "graphql",
            "-f", f"query={_PR_QUERY}",
            "-f", f"owner={owner}",
            "-f", f"name={name}",
            "-f", f"base={base}",
        ]
        if cursor:
            args += ["-f", f"cursor={cursor}"]
        try:
            data = json.loads(_gh(args))
            page = data["data"]["repository"]["pullRequests"]
        except (subprocess.CalledProcessError, json.JSONDecodeError, KeyError, TypeError) as exc:
            detail = getattr(exc, "stderr", "") or str(exc)
            errors.append(f"graphql: {detail.strip()[:300]}")
            break
        prs.extend(normalize_pr(node) for node in page.get("nodes") or [] if node)
        info = page.get("pageInfo") or {}
        if not info.get("hasNextPage"):
            break
        cursor = info.get("endCursor")
    return {
        "generated_at": now.isoformat(),
        "repo": repo,
        "base": base,
        "prs": prs,
        "errors": errors,
    }


def _hours(minutes: int) -> str:
    return f"{minutes // 60} h {minutes % 60:02d} min" if minutes >= 60 else f"{minutes} min"


def render_body(
    findings: list[dict[str, Any]], threshold_minutes: float, now: dt.datetime
) -> str:
    waiting = [f for f in findings if f["level"] == "review_latency_alarm"]
    blind = [f for f in findings if f["level"] == "blind_alarm"]
    lines = [
        f"Sweep at {now.strftime('%Y-%m-%d %H:%M UTC')}.",
        "",
    ]
    if waiting:
        lines += [
            f"{len(waiting)} ready pull request(s) have waited more than "
            f"{threshold_minutes:g} min with no verdict on their current head. "
            "Check that a reviewer session is alive and has these in its queue.",
            "",
            "| PR | Author | Head | Waiting | Earlier verdict |",
            "|---|---|---|---|---|",
        ]
        for finding in waiting:
            title = finding["title"].replace("|", "\\|")
            lines.append(
                f"| [#{finding['number']}]({finding['url']}) {title} "
                f"| {finding['author']} | `{finding['head'][:12]}` "
                f"| {_hours(finding['waited_minutes'])} "
                f"| {'yes, on an older head' if finding['earlier_verdict'] else 'no'} |"
            )
        lines.append("")
    if blind:
        lines += [
            "**The sweep could not read every pull request**, so the list above may be "
            "incomplete and this issue stays open until a complete sweep runs:",
            "",
        ]
        lines += [f"- {finding['error']}" for finding in blind]
        lines.append("")
    lines += [
        "This issue is updated in place every sweep and closed when no ready pull "
        "request is waiting past the threshold. Predicate and tests: "
        "`tools/scripts/review_latency_watchdog.py`.",
    ]
    return "\n".join(lines) + "\n"


def render_summary(findings: list[dict[str, Any]], threshold_minutes: float) -> str:
    waiting = [f for f in findings if f["level"] == "review_latency_alarm"]
    blind = [f for f in findings if f["level"] == "blind_alarm"]
    lines = ["## Review latency", ""]
    if blind:
        lines.append(f"Collection failed ({len(blind)} error(s)); the result is incomplete.")
    if waiting:
        lines.append(
            f"{len(waiting)} ready pull request(s) waiting more than "
            f"{threshold_minutes:g} min for a verdict on their current head:"
        )
        lines += [
            f"- #{f['number']} ({_hours(f['waited_minutes'])}) {f['title']}" for f in waiting
        ]
    elif not blind:
        lines.append(f"Every ready pull request has a verdict or has waited under {threshold_minutes:g} min.")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default="", help="owner/name (live collection)")
    ap.add_argument("--base", default="main", help="base branch of the pull requests to watch")
    ap.add_argument("--snapshot", default="", help="read a recorded snapshot instead of the API")
    ap.add_argument("--threshold-minutes", type=float, default=THRESHOLD_MINUTES)
    ap.add_argument("--findings-out", default="review-findings.json")
    ap.add_argument("--snapshot-out", default="")
    ap.add_argument("--body-out", default="review-body.md")
    ap.add_argument("--summary-out", default="")
    args = ap.parse_args(argv)

    now = dt.datetime.now(dt.timezone.utc)
    if args.snapshot:
        with open(args.snapshot, encoding="utf-8") as fh:
            snapshot = json.load(fh)
        if snapshot.get("generated_at"):
            now = parse_ts(snapshot["generated_at"])
    else:
        if not args.repo:
            print("review_latency_watchdog: --repo required without --snapshot", file=sys.stderr)
            return 2
        snapshot = collect_snapshot(args.repo, args.base, now)

    findings = analyze(snapshot, now, args.threshold_minutes)

    with open(args.findings_out, "w", encoding="utf-8") as fh:
        json.dump(findings, fh, indent=2)
    if args.snapshot_out:
        with open(args.snapshot_out, "w", encoding="utf-8") as fh:
            json.dump(snapshot, fh, indent=2)
    if findings:
        with open(args.body_out, "w", encoding="utf-8") as fh:
            fh.write(render_body(findings, args.threshold_minutes, now))

    summary = render_summary(findings, args.threshold_minutes)
    print(summary)
    if args.summary_out:
        with open(args.summary_out, "a", encoding="utf-8") as fh:
            fh.write(summary + "\n")

    # Exit 0 regardless of findings: the workflow decides what to do with them.
    print(f"alarm_count={len(findings)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
