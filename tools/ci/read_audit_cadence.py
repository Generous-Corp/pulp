#!/usr/bin/env python3
"""Per-day ledger of the nightly read audit on main: which cron days ran clean,
which did not, and which had no counted run at all.

The read audit's Stage 0 streak counts clean cron days, and only the schedule
speaks for a day. GitHub fires the nightly's cron hours late, so the day is
the UTC date the scheduled run was created, not the cron hour. At most one run
counts per day:

    the scheduled run      the run with event `schedule` created that day; if
                           there were two, the first to complete
    a replacing dispatch   counts in its place only when that day's scheduled
                           run was cancelled and the dispatch ran on the same
                           head sha after it on the same day (the per-ref
                           cancel-in-progress replacement); the first such
                           dispatch
    anything else          ignored and listed with its reason: any other
                           dispatch is a canary precondition or a control,
                           never a day

A counted run is clean when its report's stage0 verdict says so (--verdicts,
read from each run's read-audit.json). Without a report, its conclusion
decides, but only for a run created after the nightly began failing on any
verdict but clean (FAIL_ON_FINDINGS_SINCE); before that a `success` could carry
findings, so such a run is not clean. A cancelled or skipped run says nothing.

    streak       walking the days in order, a clean day adds one; a
                 not-clean day and a gap both reset it to zero
    missing      every completed UTC day since --start with no counted run (a
                 gap): GitHub dropped the schedule, or its run was cancelled
                 with no same-sha replacement; the day is named so it is never
                 hidden
    missing_recent
                 the missing days of the last --flag-days days, which is what
                 the tracking issue raises (an old gap stays in the ledger but
                 does not hold the issue open forever)

Today, and a day whose run is still in progress, is `pending` until a counted
run exists.

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

SCHEMA = "pulp-read-audit-cadence/v2"
COUNTED = {"success": "clean", "failure": "not_clean"}
# When read-audit-nightly.yml started passing --fail-on-findings: from here a
# `success` conclusion means a clean verdict.
FAIL_ON_FINDINGS_SINCE = "2026-10-04T00:00:00Z"


def _stamp(text: str) -> dt.datetime:
    return dt.datetime.fromisoformat(text.replace("Z", "+00:00")).astimezone(dt.timezone.utc)


def _date(stamp: str) -> dt.date:
    return _stamp(stamp).date()


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


def _entry(run: dict, verdict: str | None = None, reason: str | None = None) -> dict:
    entry = {"id": run["id"], "event": run.get("event"), "created_at": run["created_at"],
             "head_sha": run.get("head_sha"), "conclusion": run.get("conclusion")}
    if verdict is not None:
        entry["verdict"] = verdict
    if reason is not None:
        entry["reason"] = reason
    return entry


def assign(runs: list[dict], verdicts: dict[str, str]) -> tuple[dict[dt.date, dict], dict[dt.date, list[dict]]]:
    """({day: the counted run with its verdict}, {day: the runs not counted, with reasons}).

    `runs` are the completed runs on the ref.
    """
    counted: dict[dt.date, dict] = {}
    ignored: dict[dt.date, list[dict]] = {}
    used: set = set()
    ordered = sorted(runs, key=lambda r: r["created_at"])
    # If GitHub ever creates two scheduled runs on one day, the first to
    # complete counts.
    schedules = sorted((r for r in ordered if r.get("event") == "schedule"),
                       key=lambda r: (r.get("updated_at") or r["created_at"], r["created_at"]))
    for sched in schedules:
        day = _date(sched["created_at"])
        if day in counted:
            ignored.setdefault(day, []).append(
                _entry(sched, run_verdict(sched, verdicts), "a run already counts for this day"))
            continue
        verdict = run_verdict(sched, verdicts)
        if verdict is not None:
            counted[day] = _entry(sched, verdict)
            used.add(sched["id"])
            continue
        if sched.get("conclusion") != "cancelled":
            continue  # skipped or otherwise silent: the day may still have another scheduled run
        for r in ordered:
            if (r.get("event") != "schedule" and r["id"] not in used and r.get("head_sha")
                    and r.get("head_sha") == sched.get("head_sha") and r["created_at"] >= sched["created_at"]
                    and _date(r["created_at"]) == day):
                verdict = run_verdict(r, verdicts)
                if verdict is not None:
                    counted[day] = _entry(r, verdict, f"replaces cancelled scheduled run {sched['id']}")
                    used.add(r["id"])
                    break
    for r in ordered:
        if r["id"] in used or r.get("event") == "schedule" or run_verdict(r, verdicts) is None:
            continue
        ignored.setdefault(_date(r["created_at"]), []).append(
            _entry(r, run_verdict(r, verdicts), "not a scheduled run and not replacing a cancelled one"))
    return counted, ignored


def ledger(runs: list[dict], start: dt.date, now: dt.datetime, ref: str = "main", flag_days: int = 7,
           verdicts: dict[str, str] | None = None) -> dict:
    mine = [r for r in runs if r.get("head_branch") == ref and r.get("status") == "completed"]
    counted, ignored = assign(mine, verdicts or {})
    # A day whose run is still going is not judged yet.
    running = {_date(r["created_at"]) for r in runs
               if r.get("head_branch") == ref and r.get("status") != "completed"}
    today = now.astimezone(dt.timezone.utc).date()
    rows, missing = [], []
    streak: list[dict] = []
    day = start
    while day <= today:
        run = counted.get(day)
        if run is not None:
            verdict = run["verdict"]
        elif day == today or day in running:
            verdict = "pending"  # the schedule may still fire today, or its run is still going
        else:
            verdict = "gap"
            missing.append(day.isoformat())
        if verdict == "clean":
            streak.append({"date": day.isoformat(), "run_id": run["id"]})
        elif verdict in ("not_clean", "gap"):
            streak = []
        rows.append({"date": day.isoformat(), "verdict": verdict, "run": run,
                     "ignored": ignored.get(day, [])})
        day += dt.timedelta(days=1)
    recent = (today - dt.timedelta(days=flag_days)).isoformat()
    return {"schema": SCHEMA, "ref": ref, "start": start.isoformat(), "now": now.isoformat(),
            "days": rows, "missing": missing, "missing_recent": [d for d in missing if d >= recent],
            "streak": {"count": len(streak), "days": streak}}


def summarize(doc: dict, needed: int = 7) -> str:
    s = doc["streak"]
    lines = [f"## Read audit cadence on {doc['ref']} since {doc['start']}", "",
             f"Clean-day streak: **{s['count']} of {needed}**. A day counts only through its scheduled run "
             f"(or a same-sha dispatch that replaced a cancelled one); a gap resets the streak like a finding.", ""]
    if doc["missing"]:
        lines += [f"Days with no counted run (gap, streak reset): **{', '.join(doc['missing'])}**", ""]
    lines += ["| day | verdict | counted run | not counted |", "|---|---|---|---|"]
    for row in doc["days"]:
        run = row["run"]
        counted = f"{run['id']} ({run['event']})" if run else "none"
        ignored = "; ".join(f"{r['id']} ({r['event']}): {r['reason']}" for r in row["ignored"]) or ""
        lines.append(f"| {row['date']} | {row['verdict']} | {counted} | {ignored} |")
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
