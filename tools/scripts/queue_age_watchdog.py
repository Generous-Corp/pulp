#!/usr/bin/env python3
"""Detect CI dispatch or runner lanes that have stopped advancing work.

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

Two stall shapes
----------------
The watchdog covers both layers where work can stop:

* A queued job whose requested lane shows no sign of life.
* A pull-request ``Build and Test`` run from ``.github/workflows/build.yml``
  that stays ``pending`` or ``queued`` and never expands into jobs. This
  happens before runner labels exist, so lane liveness cannot detect it; a
  stable exact-run reread and successful empty jobs-API response are the
  evidence.

For a queued job, two independent conditions are required to alarm
-------------------------------------------------------------------
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
UNEXPANDED_WORKFLOW = "Build and Test"
UNEXPANDED_WORKFLOW_PATH = ".github/workflows/build.yml"
UNEXPANDED_EVENT = "pull_request"
UNEXPANDED_STATUSES = {"pending", "queued"}


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


def evidence_gaps(snapshot: dict[str, Any]) -> list[dict[str, Any]]:
    """Return collection gaps that make an absence claim unsafe."""
    gaps = list(snapshot.get("errors") or [])
    for status in snapshot.get("truncated") or []:
        gaps.append({"status": status, "error": "run listing truncated"})
    return gaps


def is_target_unexpanded_run(run: dict[str, Any]) -> bool:
    """Whether a recorded run belongs to the one pre-expansion alert surface."""
    return (
        run.get("workflow") == UNEXPANDED_WORKFLOW
        and run.get("workflow_path") == UNEXPANDED_WORKFLOW_PATH
        and run.get("event") == UNEXPANDED_EVENT
        and run.get("status") in UNEXPANDED_STATUSES
    )


def analyze(
    snapshot: dict[str, Any],
    now: dt.datetime,
    warn_minutes: float = WARN_MINUTES,
    alarm_minutes: float = ALARM_MINUTES,
) -> list[dict[str, Any]]:
    """Return findings for queued jobs/runs at or past the warn threshold.

    Each finding carries ``level`` ("warn" or "alarm"). Only "alarm" findings
    are issue-worthy; "warn" exists so a human reading the run summary can see
    the queue getting deep before it is judged sick.

    A snapshot that failed to collect part of its evidence never alarms. An
    unobserved lane or workflow run is not an absent one -- claiming otherwise
    on partial data is how a monitor earns a reputation for lying. We sweep
    every 30 min, so the cost of waiting for complete evidence is one cycle.
    """
    degraded = bool(evidence_gaps(snapshot))

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
    for run in snapshot.get("unexpanded_runs", []):
        if not is_target_unexpanded_run(run):
            continue
        queued_at = parse_ts(run["queued_at"])
        age = _minutes_between(now, queued_at)
        if age < warn_minutes:
            continue
        if age >= alarm_minutes and not degraded:
            level = "alarm"
            evidence = "workflow run has zero jobs"
        else:
            level = "warn"
            evidence = (
                "evidence incomplete this sweep"
                if degraded
                else "workflow run has zero jobs"
            )
        findings.append(
            {
                "kind": "unexpanded_workflow_run",
                "level": level,
                "age_minutes": round(age, 1),
                "labels": [],
                "lane": "workflow dispatch",
                "workflow": run["workflow"],
                "job": "(zero jobs)",
                "run_id": run.get("run_id"),
                "run_url": run.get("run_url", ""),
                "queued_at": run["queued_at"],
                "run_status": run["status"],
                "event": run["event"],
                "workflow_path": run["workflow_path"],
                "head_sha": run.get("head_sha", ""),
                "head_branch": run.get("head_branch", ""),
                "lane_evidence": evidence,
            }
        )

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
                "kind": "queued_job",
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
        if f.get("kind") == "unexpanded_workflow_run":
            continue
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
        "unexpanded_runs": [],
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
    seen_run_statuses: dict[int, str] = {}

    # `queued` and `in_progress` runs both matter: a run reports as in_progress
    # while some of its jobs are still queued, so the waiting job for a dead
    # lane is just as likely to sit under an in_progress run as a queued one.
    for status in ("pending", "queued", "in_progress", "completed"):
        # Uniform cap across statuses. Truncating the liveness evidence
        # (in_progress/completed) makes a live lane look dead, which is the one
        # direction of error a monitor cannot afford, so this window is not
        # shallower than the queued one.
        for run in runs(status, MAX_RUNS_PER_STATUS):
            if run.get("status") != status:
                continue
            run_id = run.get("id")
            if not isinstance(run_id, int) or isinstance(run_id, bool):
                snapshot["errors"].append(
                    {"run_id": run_id, "status": status, "error": "invalid run id"}
                )
                continue
            if run_id in seen_run_statuses:
                if seen_run_statuses[run_id] != status:
                    snapshot["errors"].append(
                        {
                            "run_id": run_id,
                            "status": status,
                            "error": "run appeared in multiple status listings",
                        }
                    )
                continue
            seen_run_statuses[run_id] = status
            if status == "completed":
                updated = run.get("updated_at")
                if updated and parse_ts(updated) < completed_cutoff:
                    continue
            try:
                jobs_payload = _gh_api(
                    f"/repos/{repo}/actions/runs/{run_id}/jobs?per_page=100"
                )
                jobs = jobs_payload["jobs"]
                if not isinstance(jobs, list):
                    raise TypeError("jobs response field is not a list")
            except (
                KeyError,
                TypeError,
                subprocess.CalledProcessError,
                json.JSONDecodeError,
            ) as exc:
                # One flaky call must not take the sweep down — a watchdog that
                # reddens its own run gets ignored. But record it: a missed
                # jobs fetch can hide the liveness evidence that keeps a live
                # lane quiet, and analyze() must not alarm on partial data.
                snapshot["errors"].append(
                    {"run_id": run["id"], "status": status, "error": str(exc)[:200]}
                )
                continue
            if (
                status in UNEXPANDED_STATUSES
                and run.get("name") == UNEXPANDED_WORKFLOW
                and run.get("path") == UNEXPANDED_WORKFLOW_PATH
                and run.get("event") == UNEXPANDED_EVENT
                and not jobs
            ):
                total_count = jobs_payload.get("total_count")
                if not (type(total_count) is int and total_count == 0):
                    snapshot["errors"].append(
                        {
                            "run_id": run_id,
                            "status": status,
                            "error": "empty jobs response lacks exact total_count=0",
                        }
                    )
                    continue
                try:
                    exact = _gh_api(f"/repos/{repo}/actions/runs/{run_id}")
                except (subprocess.CalledProcessError, json.JSONDecodeError) as exc:
                    snapshot["errors"].append(
                        {"run_id": run_id, "status": status, "error": str(exc)[:200]}
                    )
                    continue
                head_sha = run.get("head_sha")
                if not isinstance(exact, dict):
                    snapshot["errors"].append(
                        {"run_id": run_id, "status": status, "error": "invalid exact-run response"}
                    )
                    continue
                exact_status = exact.get("status")
                if exact_status != status:
                    if exact_status in UNEXPANDED_STATUSES:
                        snapshot["errors"].append(
                            {
                                "run_id": run_id,
                                "status": status,
                                "error": "run changed between target statuses during observation",
                            }
                        )
                    # A run that reached a non-target status has advanced. A
                    # pending/queued transition remains ambiguous for this
                    # sweep and therefore degrades all absence claims.
                    continue
                if (
                    not head_sha
                    or exact.get("id") != run_id
                    or exact.get("head_sha") != head_sha
                    or exact.get("event") != UNEXPANDED_EVENT
                    or exact.get("path") != UNEXPANDED_WORKFLOW_PATH
                    or exact.get("name") != UNEXPANDED_WORKFLOW
                ):
                    snapshot["errors"].append(
                        {
                            "run_id": run_id,
                            "status": status,
                            "error": "exact-run identity differs from list evidence",
                        }
                    )
                    continue
                try:
                    control_payload = _gh_api(
                        f"/repos/{repo}/actions/runs/{run_id}/jobs?per_page=100"
                    )
                    control_jobs = control_payload["jobs"]
                    control_count = control_payload.get("total_count")
                    if not isinstance(control_jobs, list):
                        raise TypeError("control jobs response field is not a list")
                except (
                    KeyError,
                    TypeError,
                    subprocess.CalledProcessError,
                    json.JSONDecodeError,
                ) as exc:
                    snapshot["errors"].append(
                        {"run_id": run_id, "status": status, "error": str(exc)[:200]}
                    )
                    continue
                if control_jobs or control_count != 0:
                    # Job expansion began after the first read, so this run is
                    # advancing and is not zero-job evidence for this sweep.
                    continue
                queued_at = run.get("created_at") or run.get("run_started_at")
                if not queued_at:
                    snapshot["errors"].append(
                        {
                            "run_id": run_id,
                            "status": status,
                            "error": "unexpanded run lacks a queue timestamp",
                        }
                    )
                    continue
                snapshot["unexpanded_runs"].append(
                    {
                        "run_id": run_id,
                        "run_url": run.get("html_url", ""),
                        "workflow": run.get("name", ""),
                        "workflow_path": run.get("path", ""),
                        "event": run.get("event", ""),
                        "status": status,
                        "head_sha": head_sha,
                        "head_branch": run.get("head_branch", ""),
                        "queued_at": queued_at,
                    }
                )
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
    lane_alarms = [f for f in alarms if f.get("kind") != "unexpanded_workflow_run"]
    run_alarms = [f for f in alarms if f.get("kind") == "unexpanded_workflow_run"]
    lines: list[str] = []
    lines.append(
        "_Auto-generated by `.github/workflows/runner-health-check.yml` on "
        f"{now.strftime('%Y-%m-%d %H:%M UTC')}._"
    )
    lines.append("")
    lines.append(
        f"**{len(alarms)} job(s) or workflow run(s) have been stalled for more than "
        f"{alarm_minutes:g} minutes.** A queued job alarms only when its lane shows "
        "no sign of life. A workflow run with zero jobs alarms from its own "
        "successful jobs-API response because it has not reached runner routing."
    )
    if run_alarms:
        lines.append("")
        lines.append("### Workflow runs with zero jobs")
        lines.append("")
        for f in run_alarms:
            branch = f" on `{f['head_branch']}`" if f["head_branch"] else ""
            lines.append(
                f"- `{f['workflow']}` run `{f['run_id']}`{branch} — "
                f"{f['age_minutes']:g} min in `{f['run_status']}` with zero jobs "
                f"(created {f['queued_at']})"
            )
            if f["run_url"]:
                lines.append(f"  - run: {f['run_url']}")
    if lane_alarms:
        lines.append("")
        lines.append("### Sick lanes")
        lines.append("")
        for row in group_by_lane(lane_alarms):
            workflows = ", ".join(row["workflows"]) or "—"
            lines.append(
                f"- **`{row['lane']}`** — {row['count']} job(s) stalled, oldest "
                f"{row['oldest']:g} min. Workflows: {workflows}"
            )
        lines.append("")
        lines.append("### Stalled jobs")
        lines.append("")
        for f in lane_alarms:
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
        "The sections above distinguish runner-lane stalls from workflow runs "
        "that never reached runner routing. This check deliberately reports the "
        "symptom, not a guessed diagnosis. Usual suspects, cheapest first:"
    )
    lines.append("")
    lines.append("- A wedged `Runner.Worker` holding a lane hostage (`shipyard runner kill`).")
    lines.append("- Runner LaunchAgents not loaded on a host after a reboot.")
    lines.append("- A `runs-on` label edit that no live runner satisfies.")
    lines.append("- Registration token / GitHub App credential expiry on the host.")
    lines.append("- The host is offline, asleep, or out of disk.")
    lines.append(
        "- For a zero-job run, inspect older non-terminal `Build and Test` runs "
        "on the same ref that may still hold its concurrency group."
    )
    lines.append("")
    lines.append(
        "_This tracker updates in place each sweep and closes automatically "
        "once neither dispatch nor a runner lane is stalled._"
    )
    return "\n".join(lines)


def render_summary(
    findings: list[dict[str, Any]],
    alarm_minutes: float,
    gaps: list[dict[str, Any]] | None = None,
) -> str:
    alarms = [f for f in findings if f["level"] == "alarm"]
    warns = [f for f in findings if f["level"] == "warn"]
    lines = ["## Queue-age watchdog", ""]
    if gaps:
        lines.append(
            f"> **Degraded sweep** — {len(gaps)} collection gap(s), so the "
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
            if gaps
            else "No job or Build and Test run is past the warn threshold. Fleet looks healthy."
        )
        return "\n".join(lines)
    lines.append(
        f"- **{len(alarms)}** alarm (>= {alarm_minutes:g} min, stalled before or at job pickup)"
    )
    lines.append(f"- **{len(warns)}** warn (aged but below the alarm contract)")
    lines.append("")
    lines.append("| level | kind | lane | age (min) | workflow / job | evidence |")
    lines.append("| --- | --- | --- | --- | --- | --- |")
    for f in findings:
        lines.append(
            f"| {f['level']} | {f.get('kind', 'queued_job')} | `{f['lane']}` | "
            f"{f['age_minutes']:g} | "
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

    gaps = evidence_gaps(snapshot)
    summary = render_summary(findings, args.alarm_minutes, gaps)
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
    print(f"degraded={'true' if gaps else 'false'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
