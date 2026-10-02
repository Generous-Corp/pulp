#!/usr/bin/env python3
"""Rank why changed-surface plans select the full suite, over every PR head.

`.github/workflows/changed-surface-shadow-plan.yml` records one shadow plan
per pull-request head (`shipyard changed-surface-plan --record`) and uploads
it as the `changed-surface-shadow-plan` artifact. This collects those records
and ranks them by planner reason.

Only records stamped `origin: shadow_plan_step` count: these plans never
execute, so they must never enter a lane proxy's denominator, and a lane plan
must never enter this one. A head whose run uploaded no record is counted as a
missing plan, never dropped, so `plans / heads` is a real coverage figure.

    python3 tools/scripts/changed_surface_shadow_plans.py rank --since 2026-10-03
"""
from __future__ import annotations

import argparse
import collections
import datetime as dt
import io
import json
import sys
import zipfile
from pathlib import Path
from typing import Iterable

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from reuse_replay_collect import Collector, GitHub, _days  # noqa: E402
from reuse_policy_replay import _parse_time  # noqa: E402

WORKFLOW = "changed-surface-shadow-plan.yml"
ARTIFACT = "changed-surface-shadow-plan"
ORIGIN = "shadow_plan_step"


def reason_of(record: dict) -> str:
    """The ranking key: the planner's reason, `bounded` when it selected a
    bounded suite, `unlabelled` for a full plan that names no reason."""
    if record.get("outcome") == "planner_error":
        return "planner_error"
    if record.get("planner_reason"):
        return str(record["planner_reason"])
    return "bounded" if record.get("planned_suite") == "bounded" else "unlabelled"


def rank(records: Iterable[dict], heads: Iterable[tuple[int, str]]) -> dict:
    """Rank shadow-plan records against the PR heads that should have one.

    `heads` are the (pull request, head SHA) pairs the workflow ran for. A
    record for a head binds by (pull request, head SHA); a `planner_error`
    record carries no head and binds to its pull request's heads."""
    expected = sorted(set(heads))
    by_head: dict[tuple[int, str], str] = {}
    errored_prs: set[int] = set()
    for record in records:
        if record.get("origin") != ORIGIN or record.get("shadow_only") is not True:
            continue
        pr = int(record.get("pull_request") or 0)
        if record.get("head_sha"):
            by_head[(pr, record["head_sha"])] = reason_of(record)
        elif record.get("outcome") == "planner_error":
            errored_prs.add(pr)
    counts: collections.Counter[str] = collections.Counter()
    examples: dict[str, int] = {}
    missing = []
    for pr, head in expected:
        reason = by_head.get((pr, head)) or ("planner_error" if pr in errored_prs else None)
        if reason is None:
            missing.append({"pull_request": pr, "head_sha": head})
            reason = "missing_record"
        counts[reason] += 1
        examples.setdefault(reason, pr)
    planned = len(expected) - len(missing)
    return {
        "heads": len(expected),
        "plans": planned,
        "plans_per_head": planned / len(expected) if expected else None,
        "reasons": [{"reason": r, "count": c, "example_pr": examples[r]}
                    for r, c in counts.most_common()],
        "missing": missing,
    }


def records_from_zip(blob: bytes) -> list[dict]:
    with zipfile.ZipFile(io.BytesIO(blob)) as zf:
        return [json.loads(zf.read(name)) for name in zf.namelist() if name.endswith(".json")]


def collect(collector: Collector, since: dt.datetime, until: dt.datetime
            ) -> tuple[list[dict], list[tuple[int, str]]]:
    """Every shadow-plan run in the window: its records, and the PR head each
    run was for (from the run itself, so a run with no artifact still counts)."""
    gh, records, heads = collector.gh, [], []
    today = dt.datetime.now(dt.timezone.utc).date().isoformat()
    for day in _days(since, until):
        def fetch_runs(day=day) -> list[dict]:
            doc = gh.json(f"repos/{gh.repository}/actions/workflows/{WORKFLOW}/runs"
                          f"?event=pull_request&created={day}&per_page=100")
            return [{"id": r["id"], "head_sha": r["head_sha"], "created_at": r["created_at"],
                     "prs": [p["number"] for p in r.get("pull_requests") or []]}
                    for r in doc.get("workflow_runs", [])]
        for run in collector._cached(f"shadow-plan/runs-{day}.json.gz", fetch_runs, keep=day < today):
            if not since <= _parse_time(run["created_at"]) <= until:
                continue
            heads += [(pr, run["head_sha"]) for pr in run["prs"]]

            def fetch_records(run_id=run["id"]) -> list[dict]:
                listing = gh.json(f"repos/{gh.repository}/actions/runs/{run_id}/artifacts?per_page=100")
                art = next((a for a in listing.get("artifacts", [])
                            if a.get("name") == ARTIFACT and not a.get("expired")), None)
                if art is None:
                    return []
                with gh._request(art["archive_download_url"], "application/vnd.github+json") as resp:
                    return records_from_zip(resp.read())
            records += collector._cached(f"shadow-plan/records-{run['id']}.json.gz", fetch_records)
    return records, heads


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    ranked = sub.add_parser("rank", help="rank shadow plans by planner reason")
    ranked.add_argument("--repo", default="Generous-Corp/pulp")
    ranked.add_argument("--since", required=True, help="ISO date or time (UTC)")
    ranked.add_argument("--until", default=None, help="ISO date or time (UTC); default now")
    ranked.add_argument("--out", type=Path, default=Path.home() / ".cache/pulp/changed-surface-shadow-plans")
    ranked.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    since = _parse_time(args.since if "T" in args.since else f"{args.since}T00:00:00Z")
    until = (_parse_time(args.until if "T" in args.until else f"{args.until}T00:00:00Z")
             if args.until else dt.datetime.now(dt.timezone.utc))
    collector = Collector(GitHub(args.repo, None), args.out, Path.cwd(), workers=1)
    result = rank(*collect(collector, since, until))
    result["window"] = [since.isoformat(), until.isoformat()]
    if args.json:
        print(json.dumps(result, indent=2))
        return 0
    print(f"window {result['window'][0]} .. {result['window'][1]}")
    share = result["plans_per_head"]
    print(f"plans / PR heads: {result['plans']}/{result['heads']}"
          + (f" ({share:.0%})" if share is not None else ""))
    for row in result["reasons"]:
        print(f"{row['count']:5d}  {row['reason']}  e.g. #{row['example_pr']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
