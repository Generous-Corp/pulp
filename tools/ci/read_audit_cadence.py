#!/usr/bin/env python3
"""Per-night ledger of the nightly read audit on main: which nights ran clean,
which did not, and which had no counted run at all.

The read audit's Stage 0 streak counts clean nights, and only the nightly
window speaks for a night. A run counts for night N only if it completed
(`updated_at`) inside N's window, 07:00-10:00Z, and at most one run counts per
night:

    the scheduled run      normally the one that counts; if two completed in
                           the window, the first to complete
    a replacing dispatch   a run of another event counts only when it
                           completed in the window and has the same head sha
                           as a scheduled run of that night that was
                           cancelled (the dispatch cancelled it), and only when
                           no scheduled run counts
    anything else          ignored and listed with its reason: a dispatch
                           outside the window is a canary precondition or a
                           control, never a night

A counted run is clean when its report's stage0 verdict says so (--verdicts,
read from each run's read-audit.json). Without a report, its conclusion
decides, but only for a run created after the nightly began failing on any
verdict but clean (FAIL_ON_FINDINGS_SINCE); before that a `success` could carry
findings, so such a run is not clean. A cancelled or skipped run says nothing.

    streak       walking the nights in order, a clean night adds one; a
                 not-clean night and a gap both reset it to zero
    missing      every judged night since --start with no counted run (a gap):
                 GitHub dropped or deferred the schedule past the window; the
                 night is named so it is never hidden
    missing_recent
                 the missing nights of the last --flag-days days, which is what
                 the tracking issue raises (an old gap stays in the ledger but
                 does not hold the issue open forever)

A night is judged once its window has closed; before that it is `pending`.

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
# The nightly window, as offsets from UTC midnight. Only a run that completes
# inside it speaks for its night.
WINDOW_OPEN = dt.timedelta(hours=7)
WINDOW_CLOSE = dt.timedelta(hours=10)


def _stamp(text: str) -> dt.datetime:
    return dt.datetime.fromisoformat(text.replace("Z", "+00:00")).astimezone(dt.timezone.utc)


def _date(stamp: str) -> dt.date:
    return _stamp(stamp).date()


def window(night: dt.date) -> tuple[dt.datetime, dt.datetime]:
    """The nightly window of `night`: [07:00Z, 10:00Z]."""
    base = dt.datetime(night.year, night.month, night.day, tzinfo=dt.timezone.utc)
    return base + WINDOW_OPEN, base + WINDOW_CLOSE


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


def _completed(run: dict) -> dt.datetime:
    return _stamp(run.get("updated_at") or run["created_at"])


def _entry(run: dict, verdict: str | None = None, reason: str | None = None) -> dict:
    entry = {"id": run["id"], "event": run.get("event"), "created_at": run["created_at"],
             "completed_at": run.get("updated_at") or run["created_at"]}
    if verdict is not None:
        entry["verdict"] = verdict
    if reason is not None:
        entry["reason"] = reason
    return entry


def night_run(runs: list[dict], night: dt.date, verdicts: dict[str, str]) -> tuple[dict | None, list[dict]]:
    """(the run that speaks for `night` with its verdict, every other run of the night with a reason).

    `runs` are the completed runs on the ref whose creation or completion
    falls on `night`.
    """
    opens, closes = window(night)
    cancelled_schedule_shas = {r.get("head_sha") for r in runs
                               if r.get("event") == "schedule" and r.get("conclusion") == "cancelled"
                               and _date(r["created_at"]) == night}
    scheduled, replacing, ignored = [], [], []
    for r in sorted(runs, key=_completed):
        verdict = run_verdict(r, verdicts)
        if verdict is None:
            continue
        if not opens <= _completed(r) <= closes:
            ignored.append(_entry(r, verdict, "completed outside the nightly window"))
        elif r.get("event") == "schedule":
            scheduled.append((r, verdict))
        elif r.get("head_sha") and r.get("head_sha") in cancelled_schedule_shas:
            replacing.append((r, verdict))
        else:
            ignored.append(_entry(r, verdict, "in the window but not replacing a cancelled scheduled run"))
    chosen = (scheduled or replacing or [None])[0]
    for r, verdict in scheduled + replacing:
        if chosen is None or r is not chosen[0]:
            ignored.append(_entry(r, verdict, "a run already counts for this night"))
    if chosen is None:
        return None, ignored
    return _entry(chosen[0], chosen[1]), ignored


def ledger(runs: list[dict], start: dt.date, now: dt.datetime, ref: str = "main", flag_days: int = 7,
           verdicts: dict[str, str] | None = None) -> dict:
    by_night: dict[dt.date, list[dict]] = {}
    for run in runs:
        if run.get("head_branch") != ref or run.get("status") != "completed":
            continue
        nights = {_date(run["created_at"]), _completed(run).date()}
        for night in nights:
            if night >= start:
                by_night.setdefault(night, []).append(run)
    now = now.astimezone(dt.timezone.utc)
    rows, missing = [], []
    streak: list[dict] = []
    night = start
    while night <= now.date():
        counted, ignored = night_run(by_night.get(night, []), night, verdicts or {})
        if counted is not None:
            verdict = counted["verdict"]
        elif now <= window(night)[1]:
            verdict = "pending"  # the window is still open
        else:
            verdict = "gap"
            missing.append(night.isoformat())
        if verdict == "clean":
            streak.append({"date": night.isoformat(), "run_id": counted["id"]})
        elif verdict in ("not_clean", "gap"):
            streak = []
        rows.append({"date": night.isoformat(), "verdict": verdict, "run": counted, "ignored": ignored})
        night += dt.timedelta(days=1)
    recent = (now.date() - dt.timedelta(days=flag_days)).isoformat()
    return {"schema": SCHEMA, "ref": ref, "start": start.isoformat(), "now": now.isoformat(),
            "window": {"open": str(WINDOW_OPEN), "close": str(WINDOW_CLOSE)},
            "days": rows, "missing": missing, "missing_recent": [d for d in missing if d >= recent],
            "streak": {"count": len(streak), "days": streak}}


def summarize(doc: dict, needed: int = 7) -> str:
    s = doc["streak"]
    lines = [f"## Read audit cadence on {doc['ref']} since {doc['start']}", "",
             f"Clean-night streak: **{s['count']} of {needed}**. A night counts only through a run that "
             f"completed in its 07:00-10:00Z window; a gap resets the streak like a finding.", ""]
    if doc["missing"]:
        lines += [f"Nights with no counted run (gap, streak reset): **{', '.join(doc['missing'])}**", ""]
    lines += ["| night | verdict | counted run | not counted |", "|---|---|---|---|"]
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
