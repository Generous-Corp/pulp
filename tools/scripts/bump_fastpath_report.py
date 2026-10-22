#!/usr/bin/env python3
"""Count how often the release bot's version bump took the gate's fast path.

`build.yml`'s classify job runs the protected-base verifier
(`generated_version_bump_check.py`) on every `release/version-bump` pull
request and merge group, and leaves one `bump-fast-path` notice on the job:
`taken`, or `refused: <reason>`. This report reads those notices for the
`Build and Test` runs of the last N days and prints how many bump runs were
taken and refused, the refusal reasons, and the gate minutes the refused runs
spent. A run whose classify job carries no notice is counted as `unreported`
(a run from before the notice existed, or one the classifier never reached),
never as taken.

    bump_fastpath_report.py [--repo Generous-Corp/pulp] [--days 7] [--json]

It reads through `gh` (or `$GH_CLI`), read-only. Exit 0 always prints a
report; exit 2 means the GitHub reads failed.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from collections import Counter
from datetime import datetime, timedelta, timezone
from typing import Any, Callable, Iterable

WORKFLOW = "build.yml"
BUMP_BRANCH = "release/version-bump"
NOTICE_TITLE = "bump-fast-path"
CLASSIFY_JOB = "classify"
# Merge groups label their branch with the pull request; pull-request runs
# carry the head branch itself.
QUEUE_PREFIX = "gh-readonly-queue/"

Fetch = Callable[[str], Any]


def gh_fetch(gh: str) -> Fetch:
    def fetch(endpoint: str) -> Any:
        out = subprocess.run([gh, "api", endpoint], capture_output=True, text=True,
                             encoding="utf-8", check=True, timeout=120).stdout
        return json.loads(out)
    return fetch


def _minutes(start: str | None, end: str | None) -> float:
    if not start or not end:
        return 0.0
    parse = lambda s: datetime.fromisoformat(s.replace("Z", "+00:00"))  # noqa: E731
    return max(0.0, (parse(end) - parse(start)).total_seconds() / 60)


def outcome_of(annotations: Iterable[dict[str, Any]]) -> str:
    """`taken`, `refused: <reason>` or `unreported`, from a classify job's
    annotations; the last bump-fast-path notice wins."""
    found = [a.get("message", "") for a in annotations
             if isinstance(a, dict) and a.get("title") == NOTICE_TITLE]
    return found[-1].strip() if found else "unreported"


def bump_runs(fetch: Fetch, repo: str, since: datetime,
              is_bump: Callable[[dict[str, Any]], bool]) -> list[dict[str, Any]]:
    """One record per Build and Test run on a bump since `since`."""
    records = []
    created = since.strftime("%Y-%m-%dT%H:%M:%SZ")
    for event in ("merge_group", "pull_request"):
        page = 1
        while True:
            body = fetch(f"repos/{repo}/actions/workflows/{WORKFLOW}/runs"
                         f"?event={event}&created=>={created}&per_page=100&page={page}")
            runs = body.get("workflow_runs", []) if isinstance(body, dict) else []
            for run in runs:
                if not is_bump(run):
                    continue
                jobs = fetch(f"repos/{repo}/actions/runs/{run['id']}/jobs?per_page=100")
                jobs = jobs.get("jobs", []) if isinstance(jobs, dict) else []
                classify = next((j for j in jobs if j.get("name") == CLASSIFY_JOB), None)
                outcome = "unreported"
                if classify is not None:
                    outcome = outcome_of(fetch(
                        f"repos/{repo}/check-runs/{classify['id']}/annotations?per_page=100"))
                macos = [j for j in jobs if j.get("name") == "macos"
                         and j.get("conclusion") not in (None, "skipped")]
                records.append({
                    "run": run["id"], "event": event, "outcome": outcome,
                    "conclusion": run.get("conclusion"),
                    "macos_minutes": round(sum(_minutes(j.get("started_at"),
                                                        j.get("completed_at"))
                                               for j in macos), 1),
                })
            if len(runs) < 100:
                break
            page += 1
    return records


def pull_is_bump(fetch: Fetch, repo: str) -> Callable[[dict[str, Any]], bool]:
    """Whether a run is on the bot's bump branch. A merge group names its pull
    request (`.../pr-<n>-<sha>`), whose head branch is looked up once."""
    cache: dict[str, bool] = {}

    def check(run: dict[str, Any]) -> bool:
        branch = run.get("head_branch") or ""
        if not branch.startswith(QUEUE_PREFIX):
            return branch == BUMP_BRANCH
        tail = branch.rsplit("/", 1)[-1]
        number = tail.split("-")[1] if tail.startswith("pr-") and tail.count("-") >= 2 else ""
        if not number.isdigit():
            return False
        if number not in cache:
            pull = fetch(f"repos/{repo}/pulls/{number}")
            cache[number] = (isinstance(pull, dict)
                             and (pull.get("head") or {}).get("ref") == BUMP_BRANCH)
        return cache[number]
    return check


def summarize(records: list[dict[str, Any]]) -> dict[str, Any]:
    outcomes = Counter("taken" if r["outcome"] == "taken" else
                       "unreported" if r["outcome"] == "unreported" else "refused"
                       for r in records)
    reasons = Counter(r["outcome"][len("refused: "):] for r in records
                      if r["outcome"].startswith("refused: "))
    by_event = Counter((r["event"], "taken" if r["outcome"] == "taken" else "full")
                       for r in records)
    return {
        "runs": len(records),
        "taken": outcomes["taken"],
        "refused": outcomes["refused"],
        "unreported": outcomes["unreported"],
        "refusal_reasons": dict(reasons.most_common()),
        "by_event": {f"{event}:{kind}": n for (event, kind), n in sorted(by_event.items())},
        "macos_minutes_on_refused": round(sum(r["macos_minutes"] for r in records
                                              if r["outcome"] != "taken"), 1),
    }


def render(summary: dict[str, Any], days: int) -> str:
    lines = [f"bump fast path, last {days} day(s): {summary['runs']} bump run(s), "
             f"{summary['taken']} taken, {summary['refused']} refused, "
             f"{summary['unreported']} unreported; "
             f"{summary['macos_minutes_on_refused']} macos gate-minutes on runs not taken"]
    for key, n in summary["by_event"].items():
        lines.append(f"  {key}: {n}")
    for reason, n in summary["refusal_reasons"].items():
        lines.append(f"  refused {n}x: {reason}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--repo", default="Generous-Corp/pulp")
    parser.add_argument("--days", type=int, default=7)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    fetch = gh_fetch(os.environ.get("GH_CLI") or "gh")
    since = datetime.now(timezone.utc) - timedelta(days=args.days)
    try:
        records = bump_runs(fetch, args.repo, since, pull_is_bump(fetch, args.repo))
    except (subprocess.SubprocessError, OSError, ValueError, KeyError) as exc:
        print(f"bump-fastpath-report: GitHub reads failed: {exc}", file=sys.stderr)
        return 2
    summary = summarize(records)
    print(json.dumps(summary, sort_keys=True) if args.json else render(summary, args.days))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
