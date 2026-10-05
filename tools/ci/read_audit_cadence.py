#!/usr/bin/env python3
"""Per-day ledger of the nightly read audit on main: which days ran clean,
which did not, and which had no run at all.

The read audit's Stage 0 streak counts clean days, not runs. A completed run
of read-audit-nightly.yml on main (any event: the schedule, a dispatch, the
schedule backstop) is clean when its report's stage0 verdict says so
(--verdicts, read from each run's read-audit.json). Without a report, its
conclusion decides, but only for a run created after the nightly began
failing on any verdict but clean (FAIL_ON_FINDINGS_SINCE); before that a
`success` could carry findings, so such a run is not clean. A cancelled or
skipped run says nothing and is ignored. The day is the UTC date the run was
created.

    streak       walking the days in order, a day with a non-clean run resets
                 it to zero (a clean run later the same day starts it again);
                 a day whose runs were all clean adds one, however many there
                 were; a day with no run neither adds nor resets
    missing      every completed UTC day since --start with no counted run on
                 main: GitHub dropped or deferred the schedule, and the
                 backstop did not fill the gap; the day is named so it is
                 never hidden
    missing_recent
                 the missing days of the last --flag-days days, which is what
                 the tracking issue raises (an old gap stays in the ledger but
                 does not hold the issue open forever)

    read_audit_cadence.py --runs runs.json --start 2026-10-03 [--now ISO] \\
        --out ledger.json [--summary ledger.md]

runs.json is the workflow's runs listing (`actions/workflows/<file>/runs`),
read without the branch filter; the ref is selected here.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

SCHEMA = "pulp-read-audit-cadence/v1"
COUNTED = {"success": "clean", "failure": "not_clean"}
# When read-audit-nightly.yml started passing --fail-on-findings: from here a
# `success` conclusion means a clean verdict.
FAIL_ON_FINDINGS_SINCE = "2026-10-04T00:00:00Z"


def _date(stamp: str) -> dt.date:
    return dt.datetime.fromisoformat(stamp.replace("Z", "+00:00")).astimezone(dt.timezone.utc).date()


def run_verdict(run: dict, verdicts: dict[str, str]) -> str | None:
    """clean, not_clean, or None for a run that says nothing."""
    by_conclusion = COUNTED.get(run.get("conclusion") or "")
    if by_conclusion is None:
        return None
    reported = verdicts.get(str(run["id"]))
    if reported is not None:
        return "clean" if reported == "clean" else "not_clean"
    if by_conclusion == "clean" and run["created_at"] < FAIL_ON_FINDINGS_SINCE:
        return "not_clean"  # a success then could carry findings, and no report says otherwise
    return by_conclusion


def ledger(runs: list[dict], start: dt.date, now: dt.datetime, ref: str = "main", flag_days: int = 7,
           verdicts: dict[str, str] | None = None) -> dict:
    days: dict[dt.date, list[dict]] = {}
    for run in runs:
        if run.get("head_branch") != ref or run.get("status") != "completed":
            continue
        verdict = run_verdict(run, verdicts or {})
        if verdict is None:
            continue
        day = _date(run["created_at"])
        if day < start:
            continue
        days.setdefault(day, []).append({"id": run["id"], "event": run.get("event"),
                                         "created_at": run["created_at"], "verdict": verdict})
    today = now.astimezone(dt.timezone.utc).date()
    rows, missing = [], []
    streak: list[dict] = []
    day = start
    while day <= today:
        day_runs = sorted(days.get(day, []), key=lambda r: r["created_at"])
        if not day_runs:
            if day < today:  # today may still run
                missing.append(day.isoformat())
            rows.append({"date": day.isoformat(), "verdict": "none", "runs": []})
        else:
            counted = False
            for r in day_runs:
                if r["verdict"] == "not_clean":
                    streak, counted = [], False
                elif not counted:
                    streak.append({"date": day.isoformat(), "run_id": r["id"]})
                    counted = True
            clean = all(r["verdict"] == "clean" for r in day_runs)
            rows.append({"date": day.isoformat(), "verdict": "clean" if clean else "not_clean",
                         "runs": day_runs})
        day += dt.timedelta(days=1)
    recent = (today - dt.timedelta(days=flag_days)).isoformat()
    return {"schema": SCHEMA, "ref": ref, "start": start.isoformat(), "now": now.isoformat(),
            "days": rows, "missing": missing, "missing_recent": [d for d in missing if d >= recent],
            "streak": {"count": len(streak), "days": streak}}


def summarize(doc: dict, needed: int = 7) -> str:
    s = doc["streak"]
    lines = [f"## Read audit cadence on {doc['ref']} since {doc['start']}", "",
             f"Clean-day streak: **{s['count']} of {needed}**.", ""]
    if doc["missing"]:
        lines += [f"Days with no counted run (schedule dropped, not backstopped): **{', '.join(doc['missing'])}**", ""]
    lines += ["| day | verdict | runs |", "|---|---|---|"]
    for row in doc["days"]:
        runs = ", ".join(f"{r['id']} ({r['event']}, {r['verdict']})" for r in row["runs"]) or "none"
        lines.append(f"| {row['date']} | {row['verdict']} | {runs} |")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--runs", required=True, type=Path)
    ap.add_argument("--start", required=True, type=dt.date.fromisoformat)
    ap.add_argument("--now", type=dt.datetime.fromisoformat)
    ap.add_argument("--ref", default="main")
    ap.add_argument("--flag-days", type=int, default=7)
    ap.add_argument("--verdicts", type=Path, help="{run id: stage0 verdict} read from each run's report")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--summary", type=Path)
    a = ap.parse_args(argv[1:])
    listing = json.loads(a.runs.read_text(encoding="utf-8"))
    runs = listing.get("workflow_runs") if isinstance(listing, dict) else listing
    if not isinstance(runs, list):
        print("read-audit-cadence: runs listing is malformed", file=sys.stderr)
        return 2
    now = a.now or dt.datetime.now(dt.timezone.utc)
    if now.tzinfo is None:
        now = now.replace(tzinfo=dt.timezone.utc)
    verdicts = json.loads(a.verdicts.read_text(encoding="utf-8")) if a.verdicts else {}
    doc = ledger(runs, a.start, now, a.ref, a.flag_days, verdicts)
    a.out.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    text = summarize(doc)
    if a.summary:
        a.summary.write_text(text, encoding="utf-8")
    print(text)
    print(f"read-audit-cadence: streak={doc['streak']['count']} missing={','.join(doc['missing']) or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
