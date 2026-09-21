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

Queue age answers "is the lane alive"; it cannot answer "why is it not"
----------------------------------------------------------------------
GitHub schedules a job only when ONE runner carries every label the job
requested. Reconciling those two sets -- but only for a label set some job has
already been waiting on past the alarm threshold -- names the missing label
instead of leaving a reader to guess which of the usual suspects it was this
time.

That is not the label-satisfiability census argued against above, and the
difference is the whole safety argument. The census interrogates the fleet on
a schedule, so a healthy idle JIT lane answers "nothing carries X" and the
alarm gets muted. This check has nothing to interrogate unless a real job is
stalled on a real label set right now. See analyze_label_reconciliation().

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

# --------------------------------------------------------------------------
# Contribution: a host that served nothing while its peers served
# --------------------------------------------------------------------------
# Queue age above answers "is the lane alive?". It cannot answer "is every host
# still in it?", and on 2026-09-15 that gap cost 7h06m: one macOS host stopped
# serving at 10:02Z, its two peers absorbed the load, every queue-age sweep was
# correctly quiet because comparable jobs were in progress the whole time, and
# nothing noticed until a human did.
#
# The instrument is NOT a runner-label census. These runners are JIT: they
# register only while serving, so "zero runners carry label X" is the healthy
# idle state, and a census-based alarm fires every quiet night until someone
# mutes it. The jobs API keeps the history a census snapshot throws away, so
# the observable is per-host SERVED-JOB RECENCY measured against fleet
# activity: a host is only judged silent while its peers are demonstrably busy.
#
# Computed off-host, on GitHub-hosted infrastructure, on purpose. Three of the
# six monitors that read green during that incident died of the same cause as
# the thing they were watching; a check that runs on the fleet cannot report
# the fleet being down.
CONTRIBUTION_WINDOW_HOURS = 3.0
# m3's slots each served a job every 35-60 min that morning, so 3 h of silence
# is more than 3x the normal gap and well under the 7 h actually lost.
CONTRIBUTION_MIN_FLEET_JOBS = 3
# Below this much observed history there is not enough to judge a host on, so
# the check stays silent rather than guessing from a sliver.
CONTRIBUTION_MIN_WINDOW_HOURS = 1.5
# The one job whose routing every merge depends on (the required macOS gate).
CONTRIBUTION_WATCHED_JOB = "macos"
# TARTCI_RUNNER_NAME_PREFIX per host. Verified against the live jobs API on
# 2026-09-15: `studio-pulp-gate-01-11393-1` (m3), `m5-pulp-gate-slot2-02-2226-2`.
DEFAULT_EXPECTED_HOST_PREFIXES = ("m1-", "m5-", "studio-")
# GitHub names its own ephemeral runners "GitHub Actions <n>". A hosted runner
# is not an unknown fleet host and must never be reported as one.
HOSTED_RUNNER_NAME_PREFIX = "GitHub Actions"
# Class labels this fleet routes by, mirroring runner_topology.json's
# event_class_v2.classes[].label. Used only for sole-host reporting.
DEFAULT_FLEET_CLASS_LABELS = ("pulp-build-merge-group", "pulp-build-pr-head")

# --------------------------------------------------------------------------
# The guard's own cadence
# --------------------------------------------------------------------------
# This workflow asks GitHub for `*/30` and has been getting roughly one sweep
# every four hours, which silently multiplies every detection latency below by
# eight. Nothing noticed that either. Each sweep therefore measures the gap
# since the PREVIOUS sweep and reports it, so the degradation is visible in the
# same place the findings are. See analyze_sweep_cadence() for exactly what
# this can and cannot catch.
SWEEP_WORKFLOW_FILE = "runner-health-check.yml"
SWEEP_CADENCE_PROMISED_MINUTES = 30.0
SWEEP_CADENCE_ALARM_MINUTES = 150.0

# --------------------------------------------------------------------------
# Label reconciliation: why a stalled job cannot be scheduled
# --------------------------------------------------------------------------
# Queue age above answers "is the lane alive?". It does not answer "why is
# nothing picking this up?", and on 2026-09-21 that gap cost 5h30m of zero
# merges: three queued `macos` jobs each asked for `pulp-build-merge-group`
# while every online runner advertised `pulp-build-pr-head` instead. Those
# jobs were unschedulable from the moment they queued, the queue head sat in
# AWAITING_CHECKS behind them, and nothing anywhere said which label was
# missing.
#
# This is NOT the label-satisfiability census the module docstring argues
# against, and the difference is the only thing that makes it safe. That
# census asks "does any runner advertise label X?" of the whole fleet on a
# schedule; because these runners are JIT and register only while serving, the
# honest answer at 3am on a healthy lane is "no", and the alarm is muted within
# a week. This check never asks that question. It only ever evaluates a label
# set that a real job is RIGHT NOW queued on and has already waited past the
# stall threshold. Nothing queued means nothing to evaluate, so an idle fleet
# is silent by construction rather than by tuning.
#
# The JIT objection does not vanish, it is bounded: a healthy lane mints a
# runner for a queued job in seconds to minutes. Gating on the same threshold
# the queue-age alarm uses gives a missing label 45 minutes to show up, which
# is far outside mint latency.
#
# The verdict is deliberately narrow, because conflating its two failure modes
# would make it useless. GitHub requires ONE runner to carry every requested
# label, so "schedulable" means some online runner's label set is a superset of
# the request. A superset that is busy is SATURATION -- the lane works, the
# queue is just deep -- and stays silent at any age. Only "no online runner
# carries this set, busy or idle" is unschedulable.
#
# Value here is attribution, not detection: it upgrades a generic "this lane
# looks dead" into "these jobs ask for `X` and nothing online advertises `X`".
#
# Both runner scopes are read. A repo-scoped listing is structurally blind to
# runners registered in an ORG runner group, and this org has online ones, so a
# repo-only census would call their labels unserved. An org read needs a token
# with Administration: Read and can legitimately refuse; when it does, the
# census is incomplete and this check must not claim anything is missing.
UNSCHEDULABLE_KIND = "unschedulable_labels"
RUNNER_CENSUS_BLIND_KIND = "runner_census_blind"
LABEL_RECONCILIATION_KINDS = {UNSCHEDULABLE_KIND, RUNNER_CENSUS_BLIND_KIND}
# Two extra API calls per sweep against a ~245-call budget, plus pagination
# only if a scope ever reports more runners than one page holds.
RUNNER_CENSUS_PER_PAGE = 100
MAX_RUNNER_PAGES = 5


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


def runner_census_gaps(snapshot: dict[str, Any]) -> list[dict[str, Any]]:
    """Return the reasons this sweep cannot prove a requested label is unserved.

    Deliberately NOT evidence_gaps(). That predicate covers the run and jobs
    listings, and neither a truncated listing nor a failed jobs call can
    falsify either input this check actually uses: a queued job's own requested
    labels, and the runner census. Sharing the predicate would leave this
    permanently degraded on a repo busy enough to truncate -- i.e. permanently
    unable to fire, which is the shape of guard it exists to replace.

    An EMPTY census counts as a gap rather than as "nothing advertises
    anything". A zero here is far more often the instrument than the world: an
    unauthorized scope, a listing that paginated away, a scope nobody asked.
    The one reading that would license an absence claim is the same reading a
    broken census produces, so it is never allowed to license one.
    """
    census = snapshot.get("runner_census")
    if not isinstance(census, dict):
        return [{"scope": "(none)", "error": "no runner census was collected"}]
    gaps = [dict(gap) for gap in census.get("errors") or []]
    runners = census.get("runners") or []
    if not any(r.get("status") == "online" for r in runners):
        gaps.append(
            {
                "scope": ", ".join(census.get("scopes_read") or []) or "(none)",
                "error": (
                    f"census returned {len(runners)} runner(s) and none online; "
                    "an empty census cannot tell an unserved label from an "
                    "unread one"
                ),
            }
        )
    return gaps


def _stalled_label_sets(
    snapshot: dict[str, Any], now: dt.datetime, alarm_minutes: float
) -> list[dict[str, Any]]:
    """Group jobs queued past ``alarm_minutes`` by the label set they asked for.

    One row per distinct request, because a lane that loses a label loses it
    for every job at once and three findings saying the same sentence is how a
    tracker gets skimmed.
    """
    rows: dict[frozenset[str], dict[str, Any]] = {}
    for job in snapshot.get("queued_jobs", []):
        labels = set(job.get("labels") or [])
        if not labels:
            # Nothing to reconcile: the request is unknown, not unserved.
            continue
        age = _minutes_between(now, parse_ts(job["queued_at"]))
        if age < alarm_minutes:
            continue
        row = rows.setdefault(
            frozenset(labels),
            {
                "labels": sorted(labels),
                "count": 0,
                "oldest": 0.0,
                "queued_at": job["queued_at"],
                "run_url": job.get("run_url", ""),
                "workflows": set(),
            },
        )
        row["count"] += 1
        if age > row["oldest"]:
            row["oldest"] = age
            row["queued_at"] = job["queued_at"]
            row["run_url"] = job.get("run_url", "")
        if job.get("workflow"):
            row["workflows"].add(job["workflow"])
    return sorted(rows.values(), key=lambda r: -r["oldest"])


def _label_finding(
    kind: str, level: str, row: dict[str, Any], evidence: str
) -> dict[str, Any]:
    return {
        "kind": kind,
        "level": level,
        "age_minutes": round(row["oldest"], 1),
        "labels": list(row["labels"]),
        "lane": ", ".join(row["labels"]),
        "workflow": ", ".join(sorted(row["workflows"])),
        "job": f"{row['count']} queued job(s)",
        "run_url": row["run_url"],
        "queued_at": row["queued_at"],
        "stalled_jobs": row["count"],
        "lane_evidence": evidence,
    }


def analyze_label_reconciliation(
    snapshot: dict[str, Any],
    now: dt.datetime,
    alarm_minutes: float = ALARM_MINUTES,
) -> list[dict[str, Any]]:
    """Findings for stalled jobs whose requested labels nothing online serves.

    Demand-gated: the census is consulted only for a label set some job has
    already waited on past ``alarm_minutes``. No such job, no finding -- not
    even an evidence-gap one, so an unreadable scope stays quiet on an idle
    repo and speaks up only when it is withholding a verdict somebody needs.
    """
    stalled = _stalled_label_sets(snapshot, now, alarm_minutes)
    if not stalled:
        return []

    census = snapshot.get("runner_census") or {}
    # Offline registrations keep advertising their labels for as long as they
    # stay registered, so counting one would answer "was this label ever
    # configured" rather than "can anything serve it now".
    online = [
        r for r in census.get("runners") or [] if r.get("status") == "online"
    ]
    advertised: set[str] = set()
    for runner in online:
        advertised |= set(runner.get("labels") or [])
    busy = sum(1 for r in online if r.get("busy"))

    gaps = runner_census_gaps(snapshot)
    if gaps:
        # Blind, not healthy, and not unschedulable either. Reporting the
        # partial diff is still worth doing -- it is the actionable half -- as
        # long as it is labelled as the unconfirmed reading it is.
        reasons = "; ".join(
            f"{gap.get('scope', '?')}: {gap.get('error', 'unknown')}"
            for gap in gaps
        )
        findings = []
        for row in stalled:
            unconfirmed = sorted(set(row["labels"]) - advertised)
            suffix = (
                " the partial census does not advertise "
                + ", ".join(f"`{x}`" for x in unconfirmed)
                + ", which is a lead, not a verdict"
                if unconfirmed
                else " the partial census advertises every requested label"
            )
            finding = _label_finding(
                RUNNER_CENSUS_BLIND_KIND,
                "warn",
                row,
                f"runner census incomplete ({reasons}), so no label can be "
                f"called unserved this sweep;{suffix}",
            )
            finding["census_gaps"] = gaps
            finding["unconfirmed_missing_labels"] = unconfirmed
            finding["online_runners"] = len(online)
            findings.append(finding)
        return findings

    findings = []
    for row in stalled:
        requested = set(row["labels"])
        # GitHub needs ONE runner to carry the whole set, so a superset is the
        # only thing that makes this schedulable. A busy superset is a deep
        # queue on a working lane; that is saturation, and it stays silent.
        if any(requested <= set(r.get("labels") or []) for r in online):
            continue
        missing = sorted(requested - advertised)
        census_note = (
            f"{row['count']} job(s) waiting, oldest {row['oldest']:.0f} min; "
            f"{len(online)} online self-hosted runner(s) observed, {busy} busy"
        )
        if missing:
            evidence = (
                "no online self-hosted runner advertises "
                + ", ".join(f"`{x}`" for x in missing)
                + f" — {census_note}"
            )
        else:
            evidence = (
                "every requested label is advertised somewhere, but no single "
                "online runner carries the whole set, which is what GitHub "
                f"requires — {census_note}"
            )
        finding = _label_finding(UNSCHEDULABLE_KIND, "alarm", row, evidence)
        finding["missing_labels"] = missing
        finding["online_runners"] = len(online)
        finding["online_busy"] = busy
        finding["advertised_labels"] = sorted(advertised)
        findings.append(finding)
    return findings


CONTRIBUTION_KINDS = {
    "host_stopped_contributing",
    "unknown_fleet_host",
    "sole_host_for_class",
    "contribution_guard_unconfigured",
    "sweep_cadence",
}


def classify_runner_host(
    runner_name: str, expected_prefixes: tuple[str, ...]
) -> tuple[str, str]:
    """Return ``(kind, host)`` for a runner name.

    ``kind`` is one of ``expected`` (a configured fleet host), ``hosted``
    (GitHub's own ephemeral runner), or ``unknown`` (a self-hosted runner whose
    prefix nobody declared). The longest matching prefix wins so that a future
    ``m5-alt-`` cannot be swallowed by ``m5-``.
    """
    name = (runner_name or "").strip()
    if not name:
        return ("unknown", "")
    if name.startswith(HOSTED_RUNNER_NAME_PREFIX):
        return ("hosted", HOSTED_RUNNER_NAME_PREFIX)
    matches = [p for p in expected_prefixes if p and name.startswith(p)]
    if matches:
        return ("expected", max(matches, key=len))
    return ("unknown", name)


def analyze_contribution(
    snapshot: dict[str, Any],
    now: dt.datetime,
    expected_prefixes: tuple[str, ...] = DEFAULT_EXPECTED_HOST_PREFIXES,
    window_hours: float = CONTRIBUTION_WINDOW_HOURS,
    min_fleet_jobs: int = CONTRIBUTION_MIN_FLEET_JOBS,
    watched_job: str = CONTRIBUTION_WATCHED_JOB,
    class_labels: tuple[str, ...] = DEFAULT_FLEET_CLASS_LABELS,
) -> list[dict[str, Any]]:
    """Findings about which fleet hosts served the watched job, and which did not.

    The alarm is conditional on peer activity in BOTH directions. A host that
    served nothing is only reported while the rest of the fleet served at least
    ``min_fleet_jobs`` in the same window; below that there was no demand to
    distinguish an idle host from a dead one, and saying otherwise is the
    label-census mistake in a different costume.
    """
    # This finding's own evidence predicate, deliberately not evidence_gaps().
    # A truncated run listing no longer disqualifies it -- the window above
    # shrinks to the span actually covered, so truncation costs reach, not
    # correctness. A FAILED jobs call is different: it hides served jobs that
    # exist, which is the one error that could invent a silent host out of a
    # busy one. Using the shared predicate here made this check permanently
    # degraded on any repo busy enough to truncate, i.e. permanently unable to
    # fire, which is the failure mode it was written to end.
    degraded = bool(snapshot.get("errors"))
    findings: list[dict[str, Any]] = []

    if not expected_prefixes:
        # An empty expected set can never alarm. That is this guard's own
        # silent-failure mode -- an unset or mistyped repo variable disarms it
        # completely -- so it reports itself instead of going quiet.
        findings.append(
            {
                "kind": "contribution_guard_unconfigured",
                "level": "warn",
                "age_minutes": 0.0,
                "labels": [],
                "lane": "(fleet contribution)",
                "workflow": "(watchdog configuration)",
                "job": watched_job,
                "run_url": "",
                "queued_at": now.isoformat(),
                "lane_evidence": (
                    "no expected host prefixes configured, so no host can be "
                    "reported as silent: set PULP_FLEET_EXPECTED_MACOS_HOSTS"
                ),
            }
        )
        return findings

    requested_cutoff = now - dt.timedelta(hours=window_hours)
    coverage_raw = snapshot.get("coverage_since") or ""
    coverage_since = parse_ts(coverage_raw) if coverage_raw else None
    # Never claim to have looked further back than the evidence reaches.
    cutoff = (
        max(requested_cutoff, coverage_since)
        if coverage_since is not None
        else requested_cutoff
    )
    effective_hours = _minutes_between(now, cutoff) / 60.0
    if effective_hours < CONTRIBUTION_MIN_WINDOW_HOURS:
        return findings

    served: dict[str, list[dict[str, Any]]] = {p: [] for p in expected_prefixes}
    unknown: dict[str, list[dict[str, Any]]] = {}
    hosted: list[dict[str, Any]] = []

    for entry in snapshot.get("served_jobs", []):
        if entry.get("job") != watched_job:
            continue
        started_raw = entry.get("started_at")
        if not started_raw:
            continue
        started = parse_ts(started_raw)
        if started < cutoff:
            continue
        kind, host = classify_runner_host(
            entry.get("runner_name", ""), expected_prefixes
        )
        record = {
            "runner_name": entry.get("runner_name", ""),
            "labels": sorted(set(entry.get("labels") or [])),
            "started_at": started_raw,
        }
        if kind == "expected":
            served[host].append(record)
        elif kind == "hosted":
            hosted.append(record)
        elif host:
            unknown.setdefault(host, []).append(record)

    self_hosted_total = sum(len(v) for v in served.values()) + sum(
        len(v) for v in unknown.values()
    )
    silent = sorted(p for p, rows in served.items() if not rows)
    active = sorted(p for p, rows in served.items() if rows)

    def base(kind: str, level: str, lane: str, evidence: str) -> dict[str, Any]:
        return {
            "kind": kind,
            "level": "warn" if degraded else level,
            "age_minutes": round(effective_hours * 60.0, 1),
            "labels": [],
            "lane": lane,
            "workflow": "Build and Test",
            "job": watched_job,
            "run_url": "",
            "queued_at": cutoff.isoformat(),
            "window_hours": round(effective_hours, 2),
            "window_requested_hours": window_hours,
            "window_truncated_by_coverage": bool(
                coverage_since is not None and coverage_since > requested_cutoff
            ),
            "fleet_served": self_hosted_total,
            "min_fleet_jobs": min_fleet_jobs,
            "served_by_host": {p: len(rows) for p, rows in sorted(served.items())},
            "lane_evidence": (
                "evidence incomplete this sweep" if degraded else evidence
            ),
        }

    if self_hosted_total >= min_fleet_jobs:
        for host in silent:
            peer_labels = sorted(
                {
                    label
                    for peer in active
                    for row in served[peer]
                    for label in row["labels"]
                    if label in class_labels
                }
            )
            finding = base(
                "host_stopped_contributing",
                "alarm",
                host,
                f"served 0 `{watched_job}` jobs in {effective_hours:.1f}h while the "
                f"fleet served {self_hosted_total}",
            )
            finding["labels"] = peer_labels
            finding["peers_active"] = active
            findings.append(finding)

        if class_labels and len(expected_prefixes) > 1:
            for label in class_labels:
                carriers = sorted(
                    p
                    for p in active
                    if any(label in row["labels"] for row in served[p])
                )
                if len(carriers) == 1:
                    finding = base(
                        "sole_host_for_class",
                        "warn",
                        carriers[0],
                        f"only host serving `{label}` in {effective_hours:.1f}h — the "
                        "next silent-host alarm on it is an outage, not a degradation",
                    )
                    finding["level"] = "warn"
                    finding["labels"] = [label]
                    findings.append(finding)

    for host, rows in sorted(unknown.items()):
        # A renamed host must be loud in BOTH directions: it silently drops out
        # of the expected set (no alarm when it dies) while still doing work.
        finding = base(
            "unknown_fleet_host",
            "alarm",
            host,
            f"served {len(rows)} `{watched_job}` job(s) under a runner name no "
            "expected prefix matches; it is outside this guard's coverage",
        )
        finding["observed_runner_names"] = sorted({r["runner_name"] for r in rows})
        findings.append(finding)

    if hosted:
        findings.append(
            base(
                "unknown_fleet_host",
                "warn",
                HOSTED_RUNNER_NAME_PREFIX,
                f"{len(hosted)} `{watched_job}` job(s) ran on GitHub-hosted "
                "runners; overflow is contracted off (`local-only`)",
            )
        )

    return findings


def analyze_sweep_cadence(
    snapshot: dict[str, Any],
    now: dt.datetime,
    promised_minutes: float = SWEEP_CADENCE_PROMISED_MINUTES,
    alarm_minutes: float = SWEEP_CADENCE_ALARM_MINUTES,
) -> list[dict[str, Any]]:
    """Report the gap since the previous sweep of this workflow.

    What this catches: GitHub delivering `*/30` as one sweep every few hours,
    which multiplies every detection latency here without changing a line of
    code or reddening a run. That degradation was live and invisible for a week.

    What it CANNOT catch, stated plainly because a guard with an unstated blind
    spot is worse than none: a sweep measures its own predecessor, so if the
    workflow stops running entirely, nothing here fires -- there is no sweep to
    do the measuring. That residual case is covered from a different substrate:
    the `workflow_run` trigger on `Build and Test`, which makes a gate
    completion pull a sweep even when cron is asleep. If BOTH cron and every
    gate run stop, this repo has a louder problem than a stale watchdog.
    """
    previous = snapshot.get("previous_sweep_at")
    if not previous:
        return []
    gap = _minutes_between(now, parse_ts(previous))
    if gap < max(promised_minutes * 2.0, 1.0):
        return []
    level = "warn" if gap < alarm_minutes else "alarm"
    return [
        {
            "kind": "sweep_cadence",
            # Never an issue-opening alarm: a slow watchdog is a real defect but
            # it is not a fleet outage, and mixing the two teaches readers to
            # skim the tracker. `cadence_level` carries the real severity for
            # the run summary; `level` stays "warn" so no tracker opens.
            "level": "warn",
            "cadence_level": level,
            "age_minutes": round(gap, 1),
            "labels": [],
            "lane": "(this watchdog)",
            "workflow": "Runner health check",
            "job": "(sweep cadence)",
            "run_url": "",
            "queued_at": previous,
            "lane_evidence": (
                f"{gap:.0f} min since the previous sweep; the schedule promises "
                f"{promised_minutes:g} min. Detection latency for every finding "
                "here is bounded by this number, not by the cron expression."
            ),
        }
    ]


def group_by_lane(findings: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Collapse findings into one row per lane, worst-first."""
    lanes: dict[str, dict[str, Any]] = {}
    for f in findings:
        if f.get("kind") == "unexpanded_workflow_run":
            continue
        if f.get("kind") in CONTRIBUTION_KINDS:
            continue
        if f.get("kind") in LABEL_RECONCILIATION_KINDS:
            # Already reported as its own lane row, with the missing label
            # named. Counting it here too would double the "N job(s) stalled"
            # figure for jobs this sweep only observed once.
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


def _read_runner_scope(path: str) -> tuple[list[dict[str, Any]], str]:
    """Read one runner scope. Returns ``(rows, error)`` — never both populated.

    Partial is an error, not a result. A listing that renders fewer rows than
    its own ``total_count`` may have paginated away exactly the runner that
    carries the label in question, and a short read is indistinguishable from
    a small fleet once the rows are in hand.
    """
    rows: list[dict[str, Any]] = []
    total: int | None = None
    for page in range(1, MAX_RUNNER_PAGES + 1):
        try:
            payload = _gh_api(
                f"/{path}?per_page={RUNNER_CENSUS_PER_PAGE}&page={page}"
            )
        except (subprocess.CalledProcessError, json.JSONDecodeError) as exc:
            return ([], str(exc)[:200])
        if not isinstance(payload, dict):
            return ([], "runner listing response is not an object")
        batch = payload.get("runners")
        if not isinstance(batch, list):
            return ([], "runner listing has no runners array")
        total = payload.get("total_count")
        if type(total) is not int:
            return ([], "runner listing has no integer total_count")
        rows.extend(batch)
        if len(rows) >= total or not batch:
            break
    if total is not None and len(rows) < total:
        return ([], f"received {len(rows)} of {total} runner rows")
    return (rows, "")


def collect_runner_census(repo: str) -> dict[str, Any]:
    """Census every self-hosted runner that could serve this repo, both scopes.

    The org scope is not optional cover: runners in an org runner group are
    invisible to the repo endpoint, and this org keeps online ones. Reading
    only the repo scope would report their labels as served by nothing.
    """
    census: dict[str, Any] = {"runners": [], "scopes_read": [], "errors": []}
    scopes = [f"repos/{repo}/actions/runners"]
    org = repo.split("/", 1)[0] if "/" in repo else ""
    if org:
        scopes.append(f"orgs/{org}/actions/runners")
    seen: set[str] = set()
    for scope in scopes:
        rows, error = _read_runner_scope(scope)
        if error:
            # Usually a token without Administration: Read on the org. Record
            # it; a silent fallback to repo-only is how a partial inventory
            # gets read as a complete one.
            census["errors"].append({"scope": scope, "error": error})
            continue
        census["scopes_read"].append(scope)
        for row in rows:
            name = row.get("name") or ""
            if name in seen:
                continue
            seen.add(name)
            census["runners"].append(
                {
                    "name": name,
                    "scope": scope,
                    "status": row.get("status", "offline"),
                    "busy": bool(row.get("busy")),
                    "labels": sorted(
                        {
                            label["name"] if isinstance(label, dict) else label
                            for label in row.get("labels") or []
                        }
                    ),
                }
            )
    return census


def collect_snapshot(repo: str, now: dt.datetime) -> dict[str, Any]:
    """Build a snapshot of queued jobs + lane liveness via the Actions API."""
    snapshot: dict[str, Any] = {
        "generated_at": now.isoformat(),
        "repo": repo,
        "queued_jobs": [],
        "live_jobs": [],
        "served_jobs": [],
        "unexpanded_runs": [],
        "previous_sweep_at": "",
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
                    # Which HOST served it. The liveness record above answers
                    # "is this lane alive"; only the runner name answers "is
                    # every host still in it".
                    snapshot["served_jobs"].append(
                        {
                            "job": job.get("name", ""),
                            "runner_name": job.get("runner_name") or "",
                            "labels": labels,
                            "status": job.get("status", ""),
                            "conclusion": job.get("conclusion") or "",
                            "started_at": job.get("started_at"),
                        }
                    )

    # How far back the served-job evidence actually reaches. MAX_RUNS_PER_STATUS
    # caps the completed listing, and on a busy repo that listing is ALWAYS
    # truncated -- measured on Generous-Corp/pulp on 2026-09-15: 60 completed
    # runs spanned 2.35 h. A fixed window wider than that is a window this
    # collector can never fill, and a contribution check that degrades on it is
    # a check that can never fire. So the window adapts to the coverage and
    # every finding reports the span it was actually computed over.
    starts = [
        parse_ts(e["started_at"])
        for e in snapshot["served_jobs"]
        if e.get("started_at")
    ]
    snapshot["coverage_since"] = min(starts).isoformat() if starts else ""
    # Kept out of snapshot["errors"] on purpose: a failure of this census
    # must not suppress the queue-age or contribution alarms, which do not
    # depend on it. runner_census_gaps() is its own evidence predicate.
    snapshot["runner_census"] = collect_runner_census(repo)
    snapshot["previous_sweep_at"] = _previous_sweep_at(repo, snapshot)
    return snapshot


def _previous_sweep_at(repo: str, snapshot: dict[str, Any]) -> str:
    """Start time of the most recent earlier run of this same workflow.

    One extra API call. A failure here is recorded but never degrades the
    sweep's other evidence: not knowing the cadence must not suppress a real
    contribution alarm.
    """
    current = os.environ.get("GITHUB_RUN_ID", "")
    try:
        data = _gh_api(
            f"/repos/{repo}/actions/workflows/{SWEEP_WORKFLOW_FILE}"
            "/runs?per_page=10"
        )
        runs = data.get("workflow_runs") or []
    except (subprocess.CalledProcessError, json.JSONDecodeError, TypeError) as exc:
        snapshot.setdefault("cadence_error", str(exc)[:200])
        return ""
    stamps = []
    for run in runs:
        if current and str(run.get("id")) == current:
            continue
        started = run.get("run_started_at") or run.get("created_at")
        if started:
            stamps.append(started)
    if not stamps:
        return ""
    return max(stamps, key=parse_ts)


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------


def render_body(
    findings: list[dict[str, Any]],
    alarm_minutes: float,
    now: dt.datetime,
) -> str:
    alarms = [f for f in findings if f["level"] == "alarm"]
    lane_alarms = [
        f
        for f in alarms
        if f.get("kind") != "unexpanded_workflow_run"
        and f.get("kind") not in LABEL_RECONCILIATION_KINDS
    ]
    run_alarms = [f for f in alarms if f.get("kind") == "unexpanded_workflow_run"]
    label_alarms = [f for f in alarms if f.get("kind") in LABEL_RECONCILIATION_KINDS]
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
    if label_alarms:
        lines.append("")
        lines.append("### Unschedulable label sets")
        lines.append("")
        lines.append(
            "No online self-hosted runner — busy or idle — advertises the full "
            "label set these jobs requested, so waiting longer cannot place "
            "them. This is reported only for label sets a job is already "
            "stalled on, which is why an idle fleet never produces it."
        )
        lines.append("")
        for f in label_alarms:
            lines.append(f"- **`{f['lane']}`** — {f['lane_evidence']}")
            if f.get("missing_labels"):
                lines.append(
                    "  - advertised by nothing online: "
                    + ", ".join(f"`{x}`" for x in f["missing_labels"])
                )
            if f.get("run_url"):
                lines.append(f"  - oldest run: {f['run_url']}")
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


def render_contribution_body(
    findings: list[dict[str, Any]], now: dt.datetime
) -> str:
    rows = [
        f
        for f in findings
        if f.get("kind") in CONTRIBUTION_KINDS and f["level"] == "alarm"
    ]
    warns = [
        f
        for f in findings
        if f.get("kind") in CONTRIBUTION_KINDS and f["level"] == "warn"
    ]
    lines: list[str] = []
    lines.append(
        "_Auto-generated by `.github/workflows/runner-health-check.yml` on "
        f"{now.strftime('%Y-%m-%d %H:%M UTC')}._"
    )
    lines.append("")
    lines.append(
        "**A macOS fleet host served nothing while its peers served.** This is "
        "not a queue stall: the lane is alive, jobs are being picked up, and "
        "every age-based check is correctly quiet. What was lost is redundancy "
        "— the remaining hosts absorb the load until one of them also goes, and "
        "queue latency rises in the meantime."
    )
    for f in rows:
        lines.append("")
        lines.append(f"### `{f['lane']}`")
        lines.append("")
        lines.append(f"- {f['lane_evidence']}")
        served = f.get("served_by_host") or {}
        if served:
            census = ", ".join(f"`{k}`={v}" for k, v in served.items())
            lines.append(f"- `{f['job']}` jobs served in the window: {census}")
        if f.get("labels"):
            lines.append(
                "- class labels its active peers carried: "
                + ", ".join(f"`{x}`" for x in f["labels"])
            )
        if f.get("observed_runner_names"):
            lines.append(
                "- runner names observed: "
                + ", ".join(f"`{x}`" for x in f["observed_runner_names"])
            )
    if warns:
        lines.append("")
        lines.append("### Also noted")
        lines.append("")
        for f in warns:
            lines.append(f"- `{f['lane']}` — {f['lane_evidence']}")
    lines.append("")
    lines.append("### Where to look")
    lines.append("")
    lines.append(
        "The host is reachable and its processes are running — that is what "
        "makes this invisible to liveness probes. Check what the supervisor "
        "itself recorded, not whether it is up:"
    )
    lines.append("")
    lines.append(
        "- `~/.tartci/state/macos-fleet/pulp-gate*/events.jsonl` on the named "
        "host: `job_assigned` stopping, and `assignment_scan_error`'s `detail=`."
    )
    lines.append(
        "- A `*.scan-blind-escalated` marker in the same state directory "
        "(tartci bounds its restart loop and leaves one when restarting cannot help)."
    )
    lines.append(
        "- `/usr/bin/python3 -c 0` and `/usr/bin/git --version` on the host. An "
        "unattended Xcode update invalidates the accepted-licence record and "
        "both exit 69 while still passing every `-x` test that selects them."
    )
    lines.append(
        "- `launchctl print gui/$(id -u)/<label>` for the supervisor: a "
        "`KeepAlive` respawn loop keeps `state = running` while serving nothing."
    )
    lines.append("")
    lines.append(
        "_This tracker updates in place each sweep and closes automatically "
        "once every expected host is serving again._"
    )
    return "\n".join(lines)


def render_summary(
    findings: list[dict[str, Any]],
    alarm_minutes: float,
    gaps: list[dict[str, Any]] | None = None,
) -> str:
    # Queue-age counts describe queue age only. A contribution finding is a
    # different observation with a different remedy, and folding it into the
    # "stalled before or at job pickup" line would attribute to the queue
    # something the queue never did.
    queue_findings = [f for f in findings if f.get("kind") not in CONTRIBUTION_KINDS]
    contribution_rows = [f for f in findings if f.get("kind") in CONTRIBUTION_KINDS]
    alarms = [f for f in queue_findings if f["level"] == "alarm"]
    warns = [f for f in queue_findings if f["level"] == "warn"]
    lines = ["## Queue-age watchdog", ""]
    if gaps:
        lines.append(
            f"> **Degraded sweep** — {len(gaps)} collection gap(s), so the "
            "lane-liveness evidence is incomplete and alarms are suppressed for "
            "this cycle. An unobserved lane is not a dead one. Next sweep in 30 min."
        )
        lines.append("")
    if not queue_findings and not contribution_rows:
        # Never report health off a sweep that could not see. "I found nothing"
        # and "I looked and there is nothing" are different claims.
        lines.append(
            "No findings — but this sweep's evidence was incomplete, so this is "
            "not a clean bill of health."
            if gaps
            else "No job or Build and Test run is past the warn threshold. Fleet looks healthy."
        )
        return "\n".join(lines)
    if queue_findings:
        lines.append(
            f"- **{len(alarms)}** alarm (>= {alarm_minutes:g} min, stalled before or at job pickup)"
        )
        lines.append(f"- **{len(warns)}** warn (aged but below the alarm contract)")
    else:
        lines.append("- No job or run is past the queue-age warn threshold.")
    lines.append("")
    contribution = contribution_rows
    if contribution:
        lines.append("### Fleet contribution")
        lines.append("")
        lines.append("| level | kind | host | evidence |")
        lines.append("| --- | --- | --- | --- |")
        for f in contribution:
            lines.append(
                f"| {f.get('cadence_level', f['level'])} | {f['kind']} | "
                f"`{f['lane']}` | {f['lane_evidence']} |"
            )
        lines.append("")
        if not queue_findings:
            return "\n".join(lines).rstrip()
        lines.append("### Queue age")
        lines.append("")
    lines.append("| level | kind | lane | age (min) | workflow / job | evidence |")
    lines.append("| --- | --- | --- | --- | --- | --- |")
    for f in queue_findings:
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
    ap.add_argument("--contribution-body-out", default="")
    ap.add_argument("--summary-out", default="")
    ap.add_argument(
        "--expected-macos-hosts",
        default=os.environ.get("PULP_FLEET_EXPECTED_MACOS_HOSTS", ""),
        help=(
            "comma-separated runner-name prefixes expected to serve the "
            f"required macOS job (default: {','.join(DEFAULT_EXPECTED_HOST_PREFIXES)})"
        ),
    )
    ap.add_argument(
        "--contribution-window-hours", type=float, default=CONTRIBUTION_WINDOW_HOURS
    )
    ap.add_argument(
        "--contribution-min-fleet-jobs", type=int, default=CONTRIBUTION_MIN_FLEET_JOBS
    )
    args = ap.parse_args(argv)

    # An unset repo variable arrives as an empty string. Falling back to the
    # built-in list keeps the guard armed; a value that is set but unparseable
    # is passed through as empty on purpose, so analyze_contribution() reports
    # itself as unconfigured rather than silently never alarming.
    if args.expected_macos_hosts.strip():
        expected_prefixes = tuple(
            part.strip()
            for part in args.expected_macos_hosts.split(",")
            if part.strip()
        )
    else:
        expected_prefixes = DEFAULT_EXPECTED_HOST_PREFIXES

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
    contribution = analyze_contribution(
        snapshot,
        now,
        expected_prefixes=expected_prefixes,
        window_hours=args.contribution_window_hours,
        min_fleet_jobs=args.contribution_min_fleet_jobs,
    )
    contribution += analyze_sweep_cadence(snapshot, now)
    findings = (
        contribution
        + findings
        + analyze_label_reconciliation(snapshot, now, args.alarm_minutes)
    )
    queue_alarms = [
        f
        for f in findings
        if f["level"] == "alarm" and f.get("kind") not in CONTRIBUTION_KINDS
    ]
    contribution_alarms = [
        f
        for f in findings
        if f["level"] == "alarm" and f.get("kind") in CONTRIBUTION_KINDS
    ]
    alarms = queue_alarms

    with open(args.findings_out, "w", encoding="utf-8") as fh:
        json.dump(findings, fh, indent=2)
    if args.snapshot_out:
        with open(args.snapshot_out, "w", encoding="utf-8") as fh:
            json.dump(snapshot, fh, indent=2)
    if queue_alarms:
        with open(args.body_out, "w", encoding="utf-8") as fh:
            fh.write(render_body(findings, args.alarm_minutes, now))
    if contribution_alarms and args.contribution_body_out:
        # A separate tracker with its own title. Folding a silent host into the
        # queue-stall issue would name a symptom that was never observed --
        # the queue was not stalled -- which is the misattribution that sent
        # readers to audit a healthy `gh` CLI for seven hours.
        with open(args.contribution_body_out, "w", encoding="utf-8") as fh:
            fh.write(render_contribution_body(findings, now))

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
    print(f"alarm_count={len(queue_alarms)}")
    print(f"contribution_alarm_count={len(contribution_alarms)}")
    print(f"degraded={'true' if gaps else 'false'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
