#!/usr/bin/env python3
"""Detect CI lanes that have stopped serving jobs, using queue age as the signal.

Why queue age and not runner labels
-----------------------------------
Pulp's macOS lanes are JIT/ephemeral: a runner registers with GitHub only while
it is serving a job, and deregisters when the job ends. So "zero runners carry
label X" is BOTH the healthy-idle state (nothing to do right now) AND the
dead-lane state (nothing can ever pick this up). From GitHub's side the two are
indistinguishable, which makes a label-satisfiability probe the wrong
instrument: it false-alarms every idle night.

Queue age is the observable that separates them. It is cause-agnostic and
symptom-level, so it catches unknown-unknowns: a wedged worker, a revoked
token, a label typo, a LaunchAgent that never came back after reboot, a cause
nobody has thought of yet. Nothing queues on an idle night, so an idle fleet is
silent by construction.

Two independent conditions, both required to alarm
--------------------------------------------------
A deep queue on a healthy pool is normal and must stay quiet. Measured baseline
on this repo under normal healthy load: median queue age 5 min, oldest 31 min,
3 runs over 30 min. A naive "age > 30 min" rule fires on that distribution
every busy afternoon. So a finding must satisfy BOTH:

1. Age. The job has waited longer than ``--alarm-minutes`` (default 45).
2. Liveness. Its lane shows no sign of life -- nothing with comparable labels
   is currently in_progress, and nothing with comparable labels has *started*
   since this job queued.

Condition 2 carries most of the false-alarm load, which is what lets condition
1 stay tight enough to keep detection latency low:

* Saturated-but-healthy pool. Runners are busy, so comparable jobs are
  in_progress -> quiet, no matter how deep the queue gets. This is the observed
  31-minute baseline.
* One runner, one long job, a queue behind it. The in_progress job proves the
  runner is alive -> quiet. (Age alone would have called this dead.)
* Idle fleet, nothing queued. No jobs to evaluate -> quiet.
* Genuinely dead lane. Nothing in_progress, nothing starting, work piling up ->
  ALARM, naming the labels the stalled jobs asked for so a human sees which
  lane is sick.

Lane comparison is deliberately loose (subset in either direction) because a
job's ``labels`` are what it *requested*, not what the serving runner carries.
Loose matching biases toward calling a lane live, i.e. toward staying quiet --
the correct direction for a monitor whose credibility dies on false alarms.

The analysis is pure: ``analyze()`` takes a snapshot dict and returns findings.
``--snapshot`` feeds it a recorded snapshot (tests, dry runs, replaying an
incident); the default path collects a live one via ``gh api``.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
from typing import Any

WARN_MINUTES = 30
ALARM_MINUTES = 45

# Bounds on the live collector's API budget. GITHUB_TOKEN allows 1000
# req/hr/repo; this workflow sweeps twice an hour and each run below costs one
# jobs call, so the caps keep a worst-case sweep well inside the budget even
# when something upstream floods the queue.
MAX_RUNS_PER_STATUS = 60
COMPLETED_LOOKBACK_HOURS = 3


def parse_ts(value: str) -> dt.datetime:
    """Parse a GitHub ISO-8601 timestamp into an aware UTC datetime."""
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    stamp = dt.datetime.fromisoformat(text)
    if stamp.tzinfo is None:
        stamp = stamp.replace(tzinfo=dt.timezone.utc)
    return stamp.astimezone(dt.timezone.utc)


def _minutes_between(later: dt.datetime, earlier: dt.datetime) -> float:
    return (later - earlier).total_seconds() / 60.0


def lanes_are_comparable(a: set[str], b: set[str]) -> bool:
    """True when one requested-label set could be served by the other's runner.

    Subset in either direction. See the module docstring: requested labels are
    not runner labels, so exact matching would invent false alarms out of
    harmless label drift between two jobs on the same physical lane.
    """
    if not a or not b:
        return False
    return a <= b or b <= a


def analyze(
    snapshot: dict[str, Any],
    now: dt.datetime,
    warn_minutes: float = WARN_MINUTES,
    alarm_minutes: float = ALARM_MINUTES,
) -> list[dict[str, Any]]:
    """Return findings for queued jobs at or past the warn threshold.

    Each finding carries ``level`` ("warn" or "alarm"). Only "alarm" findings
    are issue-worthy; "warn" exists so a human reading the run summary can see
    the queue getting deep before it is judged sick.

    A snapshot that failed to collect part of its evidence never alarms. The
    alarm means "no runner is alive here", and an unobserved lane is not an
    absent one -- claiming otherwise on partial data is how a monitor earns a
    reputation for lying. We sweep every 30 min, so the cost of waiting for
    complete evidence is one cycle.
    """
    degraded = bool(snapshot.get("errors"))

    live = []
    for entry in snapshot.get("live_jobs", []):
        labels = set(entry.get("labels") or [])
        if not labels:
            continue
        started_raw = entry.get("started_at")
        live.append(
            {
                "labels": labels,
                "status": entry.get("status", ""),
                "started_at": parse_ts(started_raw) if started_raw else None,
            }
        )

    findings: list[dict[str, Any]] = []
    for job in snapshot.get("queued_jobs", []):
        queued_at = parse_ts(job["queued_at"])
        age = _minutes_between(now, queued_at)
        if age < warn_minutes:
            continue

        labels = set(job.get("labels") or [])
        served_by = None
        for lane in live:
            if not lanes_are_comparable(lane["labels"], labels):
                continue
            # A busy runner is a live runner: an in_progress job on this lane
            # proves the lane can serve work, however long it has been running.
            if lane["status"] == "in_progress":
                served_by = "in_progress"
                break
            # Otherwise the lane must have *started* something since this job
            # queued. A start that predates our queueing says nothing about
            # whether the lane is still alive now.
            if lane["started_at"] is not None and lane["started_at"] >= queued_at:
                served_by = "recent_start"
                break

        if served_by is None and age >= alarm_minutes and not degraded:
            level = "alarm"
        else:
            level = "warn"

        findings.append(
            {
                "level": level,
                "age_minutes": round(age, 1),
                "labels": sorted(labels),
                "lane": ", ".join(sorted(labels)) or "(no labels reported)",
                "workflow": job.get("workflow", ""),
                "job": job.get("job", ""),
                "run_url": job.get("run_url", ""),
                "queued_at": job["queued_at"],
                "lane_evidence": (
                    served_by
                    or ("evidence incomplete this sweep" if degraded else "no live runner observed")
                ),
            }
        )

    findings.sort(key=lambda f: (f["level"] != "alarm", -f["age_minutes"]))
    return findings


def group_by_lane(findings: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Collapse findings into one row per lane, worst-first."""
    lanes: dict[str, dict[str, Any]] = {}
    for f in findings:
        lane = lanes.setdefault(
            f["lane"],
            {"lane": f["lane"], "count": 0, "oldest": 0.0, "workflows": set()},
        )
        lane["count"] += 1
        lane["oldest"] = max(lane["oldest"], f["age_minutes"])
        if f["workflow"]:
            lane["workflows"].add(f["workflow"])
    rows = [
        {
            "lane": v["lane"],
            "count": v["count"],
            "oldest": v["oldest"],
            "workflows": sorted(v["workflows"]),
        }
        for v in lanes.values()
    ]
    rows.sort(key=lambda r: -r["oldest"])
    return rows


# --------------------------------------------------------------------------
# Live collection
# --------------------------------------------------------------------------


def _gh_api(path: str) -> dict[str, Any]:
    # In Actions this is `gh` on GITHUB_TOKEN. Locally, PULP_GH_BIN=ghapp
    # routes through the Shipyard GitHub App's own rate-limit bucket rather
    # than burning the personal token shared with a human.
    gh_bin = os.environ.get("PULP_GH_BIN") or "gh"
    proc = subprocess.run(
        [gh_bin, "api", "-H", "Accept: application/vnd.github+json", path],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(proc.stdout)


def collect_snapshot(repo: str, now: dt.datetime) -> dict[str, Any]:
    """Build a snapshot of queued jobs + lane liveness via the Actions API."""
    snapshot: dict[str, Any] = {
        "generated_at": now.isoformat(),
        "repo": repo,
        "queued_jobs": [],
        "live_jobs": [],
        "truncated": [],
        "errors": [],
    }

    def runs(status: str, limit: int) -> list[dict[str, Any]]:
        data = _gh_api(
            f"/repos/{repo}/actions/runs?status={status}&per_page={min(limit, 100)}"
        )
        found = data.get("workflow_runs", [])
        if len(found) >= limit:
            snapshot["truncated"].append(status)
        return found[:limit]

    completed_cutoff = now - dt.timedelta(hours=COMPLETED_LOOKBACK_HOURS)

    # `queued` and `in_progress` runs both matter: a run reports as in_progress
    # while some of its jobs are still queued, so the waiting job for a dead
    # lane is just as likely to sit under an in_progress run as a queued one.
    for status in ("queued", "in_progress", "completed"):
        # Uniform cap across statuses. Truncating the liveness evidence
        # (in_progress/completed) makes a live lane look dead, which is the one
        # direction of error a monitor cannot afford, so this window is not
        # shallower than the queued one.
        for run in runs(status, MAX_RUNS_PER_STATUS):
            if status == "completed":
                updated = run.get("updated_at")
                if updated and parse_ts(updated) < completed_cutoff:
                    continue
            try:
                jobs = _gh_api(
                    f"/repos/{repo}/actions/runs/{run['id']}/jobs?per_page=100"
                ).get("jobs", [])
            except (subprocess.CalledProcessError, json.JSONDecodeError) as exc:
                # One flaky call must not take the sweep down — a watchdog that
                # reddens its own run gets ignored. But record it: a missed
                # jobs fetch can hide the liveness evidence that keeps a live
                # lane quiet, and analyze() must not alarm on partial data.
                snapshot["errors"].append(
                    {"run_id": run["id"], "status": status, "error": str(exc)[:200]}
                )
                continue
            for job in jobs:
                labels = job.get("labels") or []
                if job.get("status") == "queued":
                    # Prefer the job's own queueing time; fall back to the
                    # run's, which is the only timestamp GitHub guarantees.
                    queued_at = (
                        job.get("created_at")
                        or run.get("run_started_at")
                        or run.get("created_at")
                    )
                    snapshot["queued_jobs"].append(
                        {
                            "run_id": run["id"],
                            "run_url": run.get("html_url", ""),
                            "workflow": run.get("name", ""),
                            "job": job.get("name", ""),
                            "labels": labels,
                            "queued_at": queued_at,
                        }
                    )
                elif job.get("started_at"):
                    snapshot["live_jobs"].append(
                        {
                            "labels": labels,
                            "status": job.get("status", ""),
                            "started_at": job.get("started_at"),
                        }
                    )

    return snapshot


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------


def render_body(
    findings: list[dict[str, Any]],
    alarm_minutes: float,
    now: dt.datetime,
) -> str:
    alarms = [f for f in findings if f["level"] == "alarm"]
    lines: list[str] = []
    lines.append(
        "_Auto-generated by `.github/workflows/runner-health-check.yml` on "
        f"{now.strftime('%Y-%m-%d %H:%M UTC')}._"
    )
    lines.append("")
    lines.append(
        f"**{len(alarms)} job(s) have been queued for more than {alarm_minutes:g} "
        "minutes with no sign of life on their lane** — nothing with comparable "
        "labels is running, and nothing with comparable labels has started since "
        "they queued. A saturated-but-healthy pool does not look like this: its "
        "runners are visibly busy. This looks like a lane that cannot pick work up."
    )
    lines.append("")
    lines.append("### Sick lanes")
    lines.append("")
    for row in group_by_lane(alarms):
        workflows = ", ".join(row["workflows"]) or "—"
        lines.append(
            f"- **`{row['lane']}`** — {row['count']} job(s) stalled, oldest "
            f"{row['oldest']:g} min. Workflows: {workflows}"
        )
    lines.append("")
    lines.append("### Stalled jobs")
    lines.append("")
    for f in alarms:
        lines.append(
            f"- `{f['workflow']}` / `{f['job']}` — {f['age_minutes']:g} min "
            f"(queued {f['queued_at']})"
        )
        lines.append(f"  - wants labels: `{', '.join(f['labels']) or '(none)'}`")
        if f["run_url"]:
            lines.append(f"  - run: {f['run_url']}")
    lines.append("")
    lines.append("### Where to look")
    lines.append("")
    lines.append(
        "The labels above name the lane. This check is deliberately "
        "cause-agnostic — it reports the symptom, not the diagnosis. Usual "
        "suspects, cheapest first:"
    )
    lines.append("")
    lines.append("- A wedged `Runner.Worker` holding a lane hostage (`shipyard runner kill`).")
    lines.append("- Runner LaunchAgents not loaded on a host after a reboot.")
    lines.append("- A `runs-on` label edit that no live runner satisfies.")
    lines.append("- Registration token / GitHub App credential expiry on the host.")
    lines.append("- The host is offline, asleep, or out of disk.")
    lines.append("")
    lines.append(
        "_This tracker updates in place each sweep and closes automatically "
        "once no lane is stalled._"
    )
    return "\n".join(lines)


def render_summary(
    findings: list[dict[str, Any]],
    alarm_minutes: float,
    errors: list[dict[str, Any]] | None = None,
) -> str:
    alarms = [f for f in findings if f["level"] == "alarm"]
    warns = [f for f in findings if f["level"] == "warn"]
    lines = ["## Queue-age watchdog", ""]
    if errors:
        lines.append(
            f"> **Degraded sweep** — {len(errors)} API call(s) failed, so the "
            "lane-liveness evidence is incomplete and alarms are suppressed for "
            "this cycle. An unobserved lane is not a dead one. Next sweep in 30 min."
        )
        lines.append("")
    if not findings:
        # Never report health off a sweep that could not see. "I found nothing"
        # and "I looked and there is nothing" are different claims.
        lines.append(
            "No findings — but this sweep's evidence was incomplete, so this is "
            "not a clean bill of health."
            if errors
            else "No job has been queued past the warn threshold. Fleet looks healthy."
        )
        return "\n".join(lines)
    lines.append(f"- **{len(alarms)}** alarm (>= {alarm_minutes:g} min, no live runner)")
    lines.append(f"- **{len(warns)}** warn (deep queue, lane still serving — not alarmed)")
    lines.append("")
    lines.append("| level | lane | age (min) | workflow / job | lane evidence |")
    lines.append("| --- | --- | --- | --- | --- |")
    for f in findings:
        lines.append(
            f"| {f['level']} | `{f['lane']}` | {f['age_minutes']:g} | "
            f"{f['workflow']} / {f['job']} | {f['lane_evidence']} |"
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default="", help="owner/name (live collection)")
    ap.add_argument("--snapshot", default="", help="read a recorded snapshot instead of the API")
    ap.add_argument("--warn-minutes", type=float, default=WARN_MINUTES)
    ap.add_argument("--alarm-minutes", type=float, default=ALARM_MINUTES)
    ap.add_argument("--findings-out", default="findings.json")
    ap.add_argument("--snapshot-out", default="")
    ap.add_argument("--body-out", default="body.md")
    ap.add_argument("--summary-out", default="")
    args = ap.parse_args(argv)

    if args.alarm_minutes < args.warn_minutes:
        print(
            "queue_age_watchdog: --alarm-minutes must be >= --warn-minutes",
            file=sys.stderr,
        )
        return 2

    now = dt.datetime.now(dt.timezone.utc)
    if args.snapshot:
        snapshot = json.load(open(args.snapshot, encoding="utf-8"))
        if snapshot.get("generated_at"):
            now = parse_ts(snapshot["generated_at"])
    else:
        if not args.repo:
            print("queue_age_watchdog: --repo required without --snapshot", file=sys.stderr)
            return 2
        snapshot = collect_snapshot(args.repo, now)

    findings = analyze(snapshot, now, args.warn_minutes, args.alarm_minutes)
    alarms = [f for f in findings if f["level"] == "alarm"]

    with open(args.findings_out, "w", encoding="utf-8") as fh:
        json.dump(findings, fh, indent=2)
    if args.snapshot_out:
        with open(args.snapshot_out, "w", encoding="utf-8") as fh:
            json.dump(snapshot, fh, indent=2)
    if alarms:
        with open(args.body_out, "w", encoding="utf-8") as fh:
            fh.write(render_body(findings, args.alarm_minutes, now))

    summary = render_summary(findings, args.alarm_minutes, snapshot.get("errors"))
    print(summary)
    if args.summary_out:
        with open(args.summary_out, "a", encoding="utf-8") as fh:
            fh.write(summary + "\n")

    if snapshot.get("truncated"):
        print(
            "note: run listing truncated for status(es): "
            + ", ".join(snapshot["truncated"]),
            file=sys.stderr,
        )
    if snapshot.get("errors"):
        print(
            f"note: {len(snapshot['errors'])} API call(s) failed; alarms "
            "suppressed this sweep on incomplete evidence.",
            file=sys.stderr,
        )

    # Exit 0 regardless of findings: the workflow decides what to do with them.
    # A watchdog that fails its own run is a watchdog nobody keeps green.
    print(f"alarm_count={len(alarms)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
