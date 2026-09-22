#!/usr/bin/env python3
"""Detect the "green PRs, but nothing is merging" stall — a wedged auto-merger.

The failure this catches
------------------------
Every required check can be green, every PR can be mergeable, nothing can be
queued waiting on a runner — and still nothing merges, for hours, because the
component that actually presses the merge button (Shipyard's per-host
queue-tick, or a future GitHub merge queue) is silently held in a state where it
reaps but never advances. The queue-age watchdog (runner-health-check.yml) does
NOT see this: that guard alarms on jobs sitting *queued* because a runner lane
died. This is the opposite shape — checks GREEN, nothing queued, yet the merge
throughput is zero. No job-level signal exists for "everything is green and
nobody is merging"; the only observable is a population of merge-ready PRs that
stays merge-ready and unmerged.

The alarm predicate (per open PR)
---------------------------------
A PR is "merge-ready and stuck" this tick when ALL hold:

1. **Required checks green.** Every check in the repo's REQUIRED set (read from
   branch protection at runtime, not hardcoded — see ``resolve_required_checks``)
   has concluded successfully. A missing or pending required check is not green,
   so it is not stuck; it is just not ready.
2. **mergeStateStatus in {CLEAN, BEHIND}.** GitHub's own merge verdict. CLEAN =
   ready to merge; BEHIND = ready but the base moved (an auto-merger updates and
   merges it). DIRTY (conflicts), BLOCKED (a required check red/missing/review
   pending), and UNSTABLE (a non-required check still moving) are all correctly
   excluded — those are waiting on something real, not on a wedged merger.
3. **Auto-merge / queue eligible.** Auto-merge is enabled on the PR (the signal
   that a machine, not a human, owns pressing merge). A green PR with no
   auto-merge is waiting on a person and must not alarm.
4. **Merge-ready age past the threshold.** The PR has been green for longer than
   ``--threshold-minutes`` (default 45), measured from the completion time of
   the last required check to go green — a real duration, independent of this
   watchdog's own cadence.

Why "two consecutive ticks"
---------------------------
A single snapshot can misread. A per-PR REST poll of merge state gets
rate-limited and returns false CLEAN/BEHIND readings under load — that literally
happened during the incident this guard is built for, which is why collection
uses one GraphQL call for every open PR's state instead. Belt-and-suspenders on
top of that: a PR must satisfy the full predicate on TWO consecutive sweeps
before it is issue-worthy. The first qualifying sweep records it as *pending*
(run-summary only); the second consecutive sweep promotes it to *alarm*. A
normal in-flight PR that merges within a tick never reaches the second
observation, so it never trips. The cross-tick memory is the set of PR numbers
that qualified last sweep, persisted as a workflow artifact (crash-safe: GitHub
holds it independently of this repo or any host).

The outcome heartbeat
---------------------
Every predicate above infers health from a component: a PR's merge state, a
queue head's age, a batch having been dispatched. Each can read healthy while
the thing that actually matters has stopped. ``analyze_merge_throughput`` asks
the outcome question instead — has anything landed on the base branch — and so
it survives causes nobody has enumerated: a routing typo, an exhausted pool, a
label no runner advertises, a host that went down.

Its denominator is what keeps it honest: zero merges is the CORRECT reading of
a quiet repository, so the alarm requires the merge queue to be non-empty.
Capacity is recorded on the finding and drives the diagnosis text but is
deliberately NOT a condition — requiring "capacity is online" would let a total
fleet outage silence the outcome alarm, which is the very substitution of a
component for the outcome this check exists to stop.

Why a blind sweep must alarm
----------------------------
A sweep whose collection failed produces zero findings, which every downstream
consumer reads identically to a sweep that looked and found nothing wrong. That
is not hypothetical: on 2026-09-21 both reads failed, this guard rendered
"Merges are flowing", and its run went green — while nothing had merged for
four hours. ``analyze_blindness`` closes that by making a failed read a
first-class finding that names the verdicts it could not reach. Silence is
evidence of health only when the instrument demonstrably ran.

``analyze()`` is pure: snapshot + previous-tick set + now -> (findings, this-tick
set). ``--snapshot`` / ``--prev-state`` feed recorded inputs (tests, replay); the
default path collects a live snapshot via ``gh api graphql``.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
import time
from typing import Any

THRESHOLD_MINUTES = 45
MERGE_QUEUE_THRESHOLD_MINUTES = 30

# Fallback only. The workflow reads the live required-check set from branch
# protection; this list is what we assume when that read is unavailable (the
# default GITHUB_TOKEN cannot always see protection rules). Kept in sync with
# the documented required set for the `main` branch.
DEFAULT_REQUIRED_CHECKS = [
    "macos",
    "Enforce version & skill sync",
    "Build + prove + (owner-gated) deploy",
    "Vellum trusted freeze",
    "Vellum freeze",
]

# A CheckRun conclusion that satisfies a required-status-check gate. SKIPPED and
# NEUTRAL count as green to branch protection, so they count here too.
GREEN_CONCLUSIONS = {"SUCCESS", "SKIPPED", "NEUTRAL"}

# The merge verdicts that mean "ready; only the merger has to act."
READY_MERGE_STATES = {"CLEAN", "BEHIND"}

# Outcome-heartbeat threshold: minutes with zero merges onto the base branch
# while the merge queue is non-empty. Merge-group validation on this repo runs
# ~40 min, so a healthy queue still lands something well inside this window.
MERGE_THROUGHPUT_THRESHOLD_MINUTES = 90

# Which alarm each collection stage feeds. A stage that FAILED is not a stage
# that found nothing: every alarm downstream of it goes silent, and that
# silence carries no information. Naming the dependency is what lets a blind
# sweep say which verdicts it was unable to reach instead of reporting calm.
BLINDING_STAGES: dict[str, tuple[str, ...]] = {
    "graphql": ("alarm",),
    "merge-queue": ("queue_alarm", "throughput_alarm"),
    "merge-throughput": ("throughput_alarm",),
}


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


def _required_check_state(
    checks: dict[str, dict[str, Any]], required: set[str]
) -> tuple[bool, dt.datetime | None, list[str]]:
    """Return (all_green, green_since, missing_or_pending).

    ``green_since`` is the completion time of the LAST required check to go
    green — the moment the PR became fully green — or None if any required
    check is not green.
    """
    missing: list[str] = []
    completions: list[dt.datetime] = []
    for name in sorted(required):
        entry = checks.get(name)
        if not entry or not entry.get("green"):
            missing.append(name)
            continue
        completed = entry.get("completed_at")
        if completed:
            completions.append(parse_ts(completed))
    if missing:
        return False, None, missing
    green_since = max(completions) if completions else None
    return True, green_since, []


def analyze(
    snapshot: dict[str, Any],
    prev_stuck: list[int] | set[int] | None,
    now: dt.datetime,
    threshold_minutes: float = THRESHOLD_MINUTES,
) -> tuple[list[dict[str, Any]], list[int]]:
    """Return (findings, stuck_now).

    ``findings`` carry ``level``: "alarm" (merge-ready and stuck for two
    consecutive sweeps — issue-worthy) or "pending" (qualified this sweep for
    the first time — run-summary only, promoted next sweep if it persists).

    ``stuck_now`` is the set of PR numbers that satisfy the full predicate
    THIS sweep; it becomes the next sweep's ``prev_stuck`` so a second
    consecutive qualification can promote pending -> alarm.
    """
    required = set(snapshot.get("required_checks") or DEFAULT_REQUIRED_CHECKS)
    required_source = snapshot.get("required_checks_source", "default")
    prev = {int(x) for x in (prev_stuck or [])}

    findings: list[dict[str, Any]] = []
    stuck_now: list[int] = []

    for pr in snapshot.get("open_prs", []):
        number = int(pr["number"])

        # A draft is not a merge candidate; a person is still editing it.
        if pr.get("is_draft"):
            continue
        # Auto-merge (or a merge-queue enqueue) is the "a machine owns the
        # merge" signal. A green PR without it is waiting on a human.
        if not pr.get("auto_merge_enabled"):
            continue
        if pr.get("merge_state_status") not in READY_MERGE_STATES:
            continue

        checks = pr.get("checks", {})
        all_green, green_since, missing = _required_check_state(checks, required)
        if not all_green:
            continue

        # Merge-ready age from the check clock, not this watchdog's cadence.
        # If GitHub reported no completion timestamp for any required check
        # (unusual), we cannot prove the age, so we do not alarm this sweep.
        if green_since is None:
            continue
        age = _minutes_between(now, green_since)
        if age < threshold_minutes:
            continue

        # Full predicate satisfied this sweep.
        stuck_now.append(number)
        level = "alarm" if number in prev else "pending"
        findings.append(
            {
                "level": level,
                "number": number,
                "title": pr.get("title", ""),
                "url": pr.get("url", ""),
                "merge_state_status": pr.get("merge_state_status"),
                "ready_minutes": round(age, 1),
                "green_since": green_since.isoformat().replace("+00:00", "Z"),
                "required_checks": sorted(required),
                "required_checks_source": required_source,
            }
        )

    # Alarms first, then longest-stuck first.
    findings.sort(key=lambda f: (f["level"] != "alarm", -f["ready_minutes"]))
    stuck_now.sort()
    return findings, stuck_now


def analyze_merge_throughput(
    snapshot: dict[str, Any],
    now: dt.datetime,
    threshold_minutes: float = MERGE_THROUGHPUT_THRESHOLD_MINUTES,
) -> list[dict[str, Any]]:
    """Alarm on the OUTCOME: nothing is merging while work is waiting to merge.

    Every other predicate in this module infers health from a component — a
    PR's merge state, a queue head's age, a batch having been dispatched. Each
    one can report healthy while the thing that actually matters, commits
    landing on the base branch, has stopped. This check asks the outcome
    question directly, so it survives causes nobody has thought of yet.

    The denominator is what makes it safe. Zero merges is the CORRECT reading
    of a quiet repo, so the alarm requires demand: the merge queue must be
    non-empty. An idle repo with an empty queue can never trip this no matter
    how long it has been since the last merge.

    Capacity is recorded on the finding and drives the diagnosis text, but is
    deliberately NOT a condition. Requiring "capacity is online" would mean a
    total fleet outage silences the outcome alarm — reintroducing exactly the
    measure-a-component-and-infer-the-outcome failure this exists to replace.
    """
    throughput = snapshot.get("merge_throughput")
    queue = snapshot.get("merge_queue")
    # Either input missing is BLINDNESS, not calm; analyze_blindness owns it.
    if not isinstance(throughput, dict) or not isinstance(queue, dict):
        return []
    last_merge_at = throughput.get("last_merge_at")
    if not last_merge_at:
        return []

    depth = int(queue.get("depth") or 0)
    if depth <= 0:
        return []  # denominator: nothing is waiting, so nothing is stuck

    minutes = _minutes_between(now, parse_ts(last_merge_at))
    if minutes < threshold_minutes:
        return []

    capacity = snapshot.get("runner_capacity") or {}
    online = capacity.get("online")
    return [
        {
            "level": "throughput_alarm",
            "minutes_since_last_merge": round(minutes, 1),
            "last_merge_at": last_merge_at,
            "last_merge_sha": throughput.get("last_merge_sha", ""),
            "queue_depth": depth,
            "runners_online": online,
            "threshold_minutes": threshold_minutes,
        }
    ]


def failed_stages(snapshot: dict[str, Any]) -> list[str]:
    """Names of collection stages that errored on this sweep."""
    return sorted(
        {
            str(err.get("stage"))
            for err in (snapshot.get("errors") or [])
            if err.get("stage")
        }
    )


def analyze_blindness(snapshot: dict[str, Any]) -> list[dict[str, Any]]:
    """Alarm when the sweep could not see the things it claims are fine.

    A watchdog whose collection failed reports zero findings, which is
    indistinguishable in every downstream consumer from a watchdog that looked
    and found nothing wrong. That is how this guard printed "merges are
    flowing" during a total merge stall: both of its reads had failed, so it
    had no observations at all, and zero findings rendered as calm.

    Silence is only evidence of health when the instrument demonstrably ran.
    """
    stages = failed_stages(snapshot)
    if not stages:
        return []
    silenced: set[str] = set()
    for stage in stages:
        silenced.update(BLINDING_STAGES.get(stage, ()))
    return [
        {
            "level": "blind_alarm",
            "failed_stages": stages,
            "silenced_alarms": sorted(silenced),
            "detail": (
                snapshot.get("errors") or []
            ),
        }
    ]


def analyze_merge_queue(
    snapshot: dict[str, Any],
    now: dt.datetime,
    threshold_minutes: float = MERGE_QUEUE_THRESHOLD_MINUTES,
) -> list[dict[str, Any]]:
    """Detect an enqueued head whose merge group has stopped making progress.

    A non-empty queue is healthy while its current merge group is young. It is
    stalled when the head has waited past the threshold and no merge-group run
    has started within that same window. This is deliberately a one-sweep
    alarm: the age window already supplies the anti-flap observation period.
    """
    queue = snapshot.get("merge_queue") or {}
    if int(queue.get("depth") or 0) <= 0:
        return []
    head = queue.get("head") or {}
    enqueued_at = head.get("enqueued_at")
    if not enqueued_at:
        return []
    age = _minutes_between(now, parse_ts(enqueued_at))
    if age < threshold_minutes:
        return []

    last_started_text = queue.get("last_merge_group_started_at")
    minutes_since_batch: float | None = None
    if last_started_text:
        minutes_since_batch = _minutes_between(now, parse_ts(last_started_text))
        if minutes_since_batch < threshold_minutes:
            return []

    return [
        {
            "level": "queue_alarm",
            "number": head.get("number"),
            "title": head.get("title", ""),
            "url": head.get("url", ""),
            "queue_state": head.get("state", "UNKNOWN"),
            "queue_depth": int(queue.get("depth") or 0),
            "queue_minutes": round(age, 1),
            "last_merge_group_started_at": last_started_text,
            "minutes_since_batch": (
                round(minutes_since_batch, 1)
                if minutes_since_batch is not None
                else None
            ),
            "blocking_checks": head.get("blocking_checks") or [],
        }
    ]


# --------------------------------------------------------------------------
# Live collection
# --------------------------------------------------------------------------


# Transient upstream failures a retry can clear. A 502/503/504 is GitHub
# declining to answer in time, not a verdict about the repository, and the
# whole guard is worthless if a slow minute blinds it. Terminal failures are
# NOT retried — only interruptions, bounded, exactly as the recovery contract
# requires.
_TRANSIENT_MARKERS = ("HTTP 502", "HTTP 503", "HTTP 504", "couldn't respond")
_GH_RETRIES = 3
_GH_RETRY_SLEEP_SECONDS = 4.0


def _is_transient(stderr: str) -> bool:
    return any(marker in stderr for marker in _TRANSIENT_MARKERS)


def _gh(args: list[str]) -> str:
    # In Actions this is `gh` on GITHUB_TOKEN. Locally, PULP_GH_BIN=ghapp routes
    # through the Shipyard GitHub App's own rate-limit bucket.
    gh_bin = os.environ.get("PULP_GH_BIN") or "gh"
    last: subprocess.CalledProcessError | None = None
    for attempt in range(_GH_RETRIES):
        proc = subprocess.run([gh_bin, *args], capture_output=True, text=True)
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


# The page size is load-bearing. At 50 this query asks GitHub for 50 check
# rollups at once and times out (HTTP 504) under normal load, which blinded
# every sweep for days. The cost is dominated by the per-PR rollup, so a
# smaller page trades round trips for a read that completes.
_PR_QUERY = """
query($owner:String!, $name:String!, $cursor:String) {
  repository(owner:$owner, name:$name) {
    pullRequests(states: OPEN, first: 25, after: $cursor,
                 orderBy: {field: CREATED_AT, direction: ASC}) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number
        title
        url
        isDraft
        mergeStateStatus
        autoMergeRequest { enabledAt }
        commits(last: 1) {
          nodes {
            commit {
              statusCheckRollup {
                contexts(first: 100) {
                  nodes {
                    __typename
                    ... on CheckRun {
                      name
                      status
                      conclusion
                      completedAt
                    }
                    ... on StatusContext {
                      context
                      state
                      createdAt
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}
"""

_MERGE_QUEUE_QUERY = """
query($owner:String!, $name:String!, $branch:String!) {
  repository(owner:$owner, name:$name) {
    mergeQueue(branch:$branch) {
      configuration { maximumEntriesToBuild }
      entries(first:20) {
        totalCount
        nodes {
          position
          enqueuedAt
          state
          pullRequest { number title url }
          headCommit {
            oid
            statusCheckRollup {
              contexts(first:100) {
                nodes {
                  __typename
                  ... on CheckRun {
                    name
                    status
                    conclusion
                    completedAt
                    detailsUrl
                  }
                  ... on StatusContext {
                    context
                    state
                    createdAt
                    targetUrl
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}
"""

_COMMIT_ROLLUP_QUERY = """
query($owner:String!, $name:String!, $oid:GitObjectID!) {
  repository(owner:$owner, name:$name) {
    object(oid:$oid) {
      ... on Commit {
        oid
        statusCheckRollup {
          contexts(first:100) {
            nodes {
              __typename
              ... on CheckRun {
                name
                status
                conclusion
                completedAt
                detailsUrl
              }
              ... on StatusContext {
                context
                state
                createdAt
                targetUrl
              }
            }
          }
        }
      }
    }
  }
}
"""


def _checks_from_rollup(commit: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Flatten a statusCheckRollup into name -> {green, completed_at}."""
    checks: dict[str, dict[str, Any]] = {}
    rollup = (commit or {}).get("statusCheckRollup") or {}
    for ctx in (rollup.get("contexts") or {}).get("nodes", []) or []:
        typename = ctx.get("__typename")
        if typename == "CheckRun":
            name = ctx.get("name")
            green = ctx.get("status") == "COMPLETED" and ctx.get(
                "conclusion"
            ) in GREEN_CONCLUSIONS
            completed = ctx.get("completedAt")
        elif typename == "StatusContext":
            name = ctx.get("context")
            green = ctx.get("state") == "SUCCESS"
            # Legacy status contexts have no completion time; createdAt is the
            # only timestamp available and is a safe lower bound on "green since".
            completed = ctx.get("createdAt")
        else:
            continue
        if not name:
            continue
        # A required gate may report multiple contexts of the same name across a
        # matrix; keep the one that is green with the latest completion so
        # green_since reflects the last leg to finish.
        prior = checks.get(name)
        if prior is None or (green and not prior["green"]):
            checks[name] = {"green": green, "completed_at": completed}
        elif green and prior["green"]:
            if completed and (
                not prior["completed_at"] or completed > prior["completed_at"]
            ):
                checks[name]["completed_at"] = completed
    return checks


def resolve_required_checks(repo: str, base: str) -> tuple[list[str], str]:
    """Read the required status checks for ``base`` from branch protection.

    Returns (checks, source) where source is "branch-protection" on a live read
    or "default" on the documented fallback. The default GITHUB_TOKEN often
    cannot read protection rules (needs admin), so a failure here is expected and
    must degrade to the documented set rather than fail the sweep.
    """
    try:
        raw = _gh(
            [
                "api",
                "-H",
                "Accept: application/vnd.github+json",
                f"/repos/{repo}/branches/{base}/protection/required_status_checks",
            ]
        )
        data = json.loads(raw)
        contexts = data.get("contexts") or [
            c.get("context") for c in (data.get("checks") or [])
        ]
        contexts = [c for c in contexts if c]
        if contexts:
            return sorted(set(contexts)), "branch-protection"
    except (subprocess.CalledProcessError, json.JSONDecodeError, KeyError):
        pass
    return list(DEFAULT_REQUIRED_CHECKS), "default"


def _blocking_contexts(commit: dict[str, Any], required: set[str]) -> list[dict[str, Any]]:
    """Return non-green required contexts from a merge-group head commit."""
    rollup = (commit or {}).get("statusCheckRollup") or {}
    contexts = (rollup.get("contexts") or {}).get("nodes", []) or []
    by_name: dict[str, dict[str, Any]] = {}
    for context in contexts:
        if context.get("__typename") == "CheckRun":
            name = context.get("name")
            green = (
                context.get("status") == "COMPLETED"
                and context.get("conclusion") in GREEN_CONCLUSIONS
            )
            state = (
                context.get("conclusion")
                if context.get("status") == "COMPLETED"
                else context.get("status")
            ) or "UNKNOWN"
            url = context.get("detailsUrl") or ""
        elif context.get("__typename") == "StatusContext":
            name = context.get("context")
            green = context.get("state") == "SUCCESS"
            state = context.get("state") or "UNKNOWN"
            url = context.get("targetUrl") or ""
        else:
            continue
        if name in required and not green:
            by_name[name] = {"name": name, "state": state, "url": url}
        elif name in required and green:
            by_name.pop(name, None)
    for name in required:
        if name not in by_name and not any(
            (c.get("name") == name or c.get("context") == name)
            for c in contexts
        ):
            by_name[name] = {"name": name, "state": "MISSING", "url": ""}
    return [by_name[name] for name in sorted(by_name)]


def _commit_rollup(repo: str, sha: str) -> dict[str, Any]:
    """Read checks from the synthetic merge-group commit identified by a run."""
    owner, _, name = repo.partition("/")
    raw = _gh(
        [
            "api",
            "graphql",
            "-f",
            f"query={_COMMIT_ROLLUP_QUERY}",
            "-f",
            f"owner={owner}",
            "-f",
            f"name={name}",
            "-f",
            f"oid={sha}",
        ]
    )
    payload = json.loads(raw)
    if payload.get("errors"):
        raise KeyError(f"GraphQL commit rollup failed: {payload['errors']!r}")
    return (
        payload
        .get("data", {})
        .get("repository", {})
        .get("object", {})
        or {}
    )


def collect_merge_throughput(repo: str, base: str) -> dict[str, Any]:
    """Read when something last landed on the base branch.

    This is the outcome the whole guard exists to protect, and it is one cheap
    REST call. Deliberately REST rather than GraphQL: the shared GraphQL quota
    is regularly exhausted by agent PR polling, and the outcome heartbeat must
    keep working when the richer reads cannot.
    """
    commits = json.loads(
        _gh(["api", f"/repos/{repo}/commits?sha={base}&per_page=1"])
    )
    if not commits:
        raise KeyError(f"no commits returned for {repo}@{base}")
    head = commits[0]
    committed = (head.get("commit") or {}).get("committer") or {}
    when = committed.get("date")
    if not when:
        raise KeyError("head commit carries no committer date")
    return {"last_merge_at": when, "last_merge_sha": head.get("sha", "")}


def collect_runner_capacity(repo: str) -> dict[str, Any]:
    """Count self-hosted runners currently registered and online.

    Context for the diagnosis only — never a condition on any alarm. A runner
    registration is not proof of usable capacity (an offline registration still
    advertises its labels), so this answers "was anything even registered",
    not "could this job have run".
    """
    payload = json.loads(_gh(["api", f"/repos/{repo}/actions/runners?per_page=100"]))
    runners = payload.get("runners") or []
    online = [r for r in runners if str(r.get("status")) == "online"]
    return {
        "registered": len(runners),
        "online": len(online),
        "busy": sum(1 for r in online if r.get("busy")),
    }


def collect_merge_queue(repo: str, base: str, required: list[str]) -> dict[str, Any]:
    """Collect queue depth/head plus the last merge-group run start time."""
    owner, _, name = repo.partition("/")
    result: dict[str, Any] = {"depth": 0, "head": None}
    raw = _gh(
        [
            "api",
            "graphql",
            "-f",
            f"query={_MERGE_QUEUE_QUERY}",
            "-f",
            f"owner={owner}",
            "-f",
            f"name={name}",
            "-f",
            f"branch={base}",
        ]
    )
    payload = json.loads(raw)
    if payload.get("errors"):
        raise KeyError(f"GraphQL merge queue query failed: {payload['errors']!r}")
    queue = payload.get("data", {}).get("repository", {}).get("mergeQueue")
    if not queue:
        return result
    entries = queue.get("entries") or {}
    result["depth"] = int(entries.get("totalCount") or 0)
    nodes = entries.get("nodes") or []
    maximum_entries_to_build = max(
        1,
        int((queue.get("configuration") or {}).get("maximumEntriesToBuild") or 1),
    )
    result["maximum_entries_to_build"] = maximum_entries_to_build
    if nodes:
        node = nodes[0]
        pull = node.get("pullRequest") or {}
        result["head"] = {
            "number": pull.get("number"),
            "title": pull.get("title", ""),
            "url": pull.get("url", ""),
            "position": node.get("position"),
            "state": node.get("state"),
            "enqueued_at": node.get("enqueuedAt"),
            "queue_head_sha": (node.get("headCommit") or {}).get("oid"),
            "merge_group_sha": None,
            "blocking_checks": [],
        }

    runs = json.loads(
        _gh(
            [
                "api",
                f"/repos/{repo}/actions/runs?event=merge_group&per_page=100",
            ]
        )
    ).get("workflow_runs", [])
    queue_ref_prefix = f"gh-readonly-queue/{base}/"
    if result.get("head"):
        number = result["head"].get("number")
        build_window_numbers = {
            (node.get("pullRequest") or {}).get("number")
            for node in nodes[:maximum_entries_to_build]
        }
        build_window_runs = [
            run
            for run in runs
            if any(
                str(run.get("head_branch") or "").startswith(
                    f"{queue_ref_prefix}pr-{candidate}-"
                )
                for candidate in build_window_numbers
                if candidate is not None
            )
            and run.get("created_at")
        ]
        if build_window_runs:
            result["last_merge_group_started_at"] = max(
                run["created_at"] for run in build_window_runs
            )
        head_runs = [
            run
            for run in runs
            if str(run.get("head_branch") or "").startswith(
                f"{queue_ref_prefix}pr-{number}-"
            )
            and run.get("head_sha")
        ]
        if head_runs:
            latest = max(
                head_runs,
                key=lambda run: run.get("created_at") or "",
            )
            merge_group_sha = latest["head_sha"]
            commit = _commit_rollup(repo, merge_group_sha)
            result["head"]["merge_group_sha"] = merge_group_sha
            result["head"]["blocking_checks"] = _blocking_contexts(
                commit, set(required)
            )
    return result


def collect_snapshot(repo: str, base: str, now: dt.datetime) -> dict[str, Any]:
    """Collect all open PRs' merge state via one paginated GraphQL query.

    GraphQL, not a per-PR REST loop: a REST poll of each PR's mergeable state
    gets rate-limited under load and returns false readings (the incident that
    motivated this guard). One GraphQL call returns every open PR's
    mergeStateStatus reliably.
    """
    owner, _, name = repo.partition("/")
    required, required_source = resolve_required_checks(repo, base)
    snapshot: dict[str, Any] = {
        "generated_at": now.isoformat(),
        "repo": repo,
        "base": base,
        "required_checks": required,
        "required_checks_source": required_source,
        "open_prs": [],
        "errors": [],
    }

    try:
        snapshot["merge_queue"] = collect_merge_queue(repo, base, required)
    except (subprocess.CalledProcessError, json.JSONDecodeError, KeyError) as exc:
        snapshot["errors"].append(
            {"stage": "merge-queue", "error": str(exc)[:200]}
        )

    try:
        snapshot["merge_throughput"] = collect_merge_throughput(repo, base)
    except (subprocess.CalledProcessError, json.JSONDecodeError, KeyError) as exc:
        snapshot["errors"].append(
            {"stage": "merge-throughput", "error": str(exc)[:200]}
        )

    try:
        snapshot["runner_capacity"] = collect_runner_capacity(repo)
    except (subprocess.CalledProcessError, json.JSONDecodeError, KeyError) as exc:
        # Diagnosis context only, so its loss blinds no alarm and is recorded
        # outside `errors` to keep the blindness verdict precise.
        snapshot["runner_capacity_error"] = str(exc)[:200]

    cursor: str | None = None
    for _ in range(100):  # hard page cap; 100 * 25 = 2500 open PRs
        args = [
            "api",
            "graphql",
            "-H",
            "Accept: application/vnd.github.merge-info-preview+json",
            "-f",
            f"query={_PR_QUERY}",
            "-f",
            f"owner={owner}",
            "-f",
            f"name={name}",
        ]
        if cursor:
            args += ["-f", f"cursor={cursor}"]
        try:
            data = json.loads(_gh(args))
        except (subprocess.CalledProcessError, json.JSONDecodeError) as exc:
            snapshot["errors"].append({"stage": "graphql", "error": str(exc)[:200]})
            break
        if data.get("errors"):
            snapshot["errors"].append(
                {
                    "stage": "graphql",
                    "error": f"GraphQL PR query failed: {data['errors']!r}"[:200],
                }
            )
            break

        prs = (
            data.get("data", {})
            .get("repository", {})
            .get("pullRequests", {})
        )
        for node in prs.get("nodes", []) or []:
            commits = (node.get("commits") or {}).get("nodes") or []
            commit = commits[0].get("commit") if commits else {}
            snapshot["open_prs"].append(
                {
                    "number": node.get("number"),
                    "title": node.get("title", ""),
                    "url": node.get("url", ""),
                    "is_draft": bool(node.get("isDraft")),
                    "merge_state_status": node.get("mergeStateStatus"),
                    "auto_merge_enabled": node.get("autoMergeRequest") is not None,
                    "checks": _checks_from_rollup(commit or {}),
                }
            )

        page = prs.get("pageInfo") or {}
        if page.get("hasNextPage"):
            cursor = page.get("endCursor")
        else:
            break

    return snapshot


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------


def render_body(
    findings: list[dict[str, Any]],
    threshold_minutes: float,
    now: dt.datetime,
) -> str:
    alarms = [f for f in findings if f["level"] == "alarm"]
    queue_alarms = [f for f in findings if f["level"] == "queue_alarm"]
    throughput_alarms = [f for f in findings if f["level"] == "throughput_alarm"]
    blind_alarms = [f for f in findings if f["level"] == "blind_alarm"]
    lines: list[str] = []
    lines.append(
        "_Auto-generated by `.github/workflows/merge-stall-check.yml` on "
        f"{now.strftime('%Y-%m-%d %H:%M UTC')}._"
    )
    lines.append("")
    if blind_alarms:
        lines.append("**This sweep could not observe the repository.**")
        lines.append("")
        for f in blind_alarms:
            lines.append(
                f"- Collection stage(s) `{'`, `'.join(f['failed_stages'])}` "
                "failed, so this sweep has no observation to report."
            )
            if f["silenced_alarms"]:
                lines.append(
                    "  - Verdicts it could NOT reach: "
                    f"`{'`, `'.join(f['silenced_alarms'])}`. Their silence is "
                    "not evidence of health."
                )
            for err in f.get("detail") or []:
                lines.append(
                    f"  - `{err.get('stage')}`: {str(err.get('error'))[:160]}"
                )
        lines.append("")
        lines.append(
            "A read that fails and a repository that is healthy produce the same "
            "zero findings. Fix the read before trusting any quiet sweep."
        )
        lines.append("")
    if throughput_alarms:
        lines.append("**Nothing is merging while the queue is non-empty.**")
        lines.append("")
        for f in throughput_alarms:
            lines.append(
                f"- No commit has landed on the base branch for "
                f"**{f['minutes_since_last_merge']:g} min** "
                f"(last: `{f['last_merge_sha'][:12]}` at {f['last_merge_at']}), "
                f"while **{f['queue_depth']}** entr"
                f"{'y is' if f['queue_depth'] == 1 else 'ies are'} queued."
            )
            online = f.get("runners_online")
            if online is not None:
                lines.append(
                    f"  - Self-hosted runners online: **{online}**."
                    + (
                        " Capacity is registered, so this is a routing or"
                        " scheduling failure rather than an empty fleet."
                        if online
                        else " No runner is registered — start with the fleet."
                    )
                )
            else:
                lines.append(
                    "  - Runner capacity could not be read; treat the cause as unknown."
                )
        lines.append("")
    if queue_alarms:
        lines.append("**The GitHub merge queue is not advancing.**")
        lines.append("")
        for f in queue_alarms:
            lines.append(
                f"- **[#{f['number']}]({f['url']})** is queue head in "
                f"`{f['queue_state']}` for {f['queue_minutes']:g} min; "
                f"queue depth is **{f['queue_depth']}**."
            )
            if f.get("last_merge_group_started_at"):
                lines.append(
                    "  - Last merge-group batch started "
                    f"{f['minutes_since_batch']:g} min ago "
                    f"({f['last_merge_group_started_at']})."
                )
            else:
                lines.append("  - No merge-group run was found.")
            blockers = f.get("blocking_checks") or []
            if blockers:
                rendered = ", ".join(
                    f"[{b['name']}]({b['url']}) `{b['state']}`"
                    if b.get("url")
                    else f"{b['name']} `{b['state']}`"
                    for b in blockers
                )
                lines.append(f"  - Required blockers: {rendered}")
        lines.append("")
    if alarms:
        lines.append(
            f"**{len(alarms)} PR(s) have been merge-ready for more than "
            f"{threshold_minutes:g} minutes across two consecutive sweeps, and "
            "are still unmerged.**"
        )
        lines.append("")
        lines.append("### Merge-ready but not merging")
        lines.append("")
        for f in alarms:
            lines.append(
                f"- **[#{f['number']}]({f['url']})** — {f['merge_state_status']}, "
                f"merge-ready {f['ready_minutes']:g} min (green since {f['green_since']})"
            )
            title = f.get("title")
            if title:
                lines.append(f"  - {title}")
    lines.append("")
    lines.append("### Where to look")
    lines.append("")
    lines.append("Start with the queue head's required blockers, then runner routing and capacity.")
    lines.append("")
    lines.append(
        "- Shipyard's per-host queue-tick stuck in reap-only mode "
        "(`shipyard runner watch` / `shipyard rescue` on the host that owns the queue)."
    )
    lines.append("- Auto-merge armed on the PRs but the merge method/branch protection changed under it.")
    lines.append("- A required check was renamed in branch protection and no PR can satisfy the new name.")
    lines.append(
        f"- Required set this sweep: `{', '.join(alarms[0]['required_checks'])}` "
        f"(source: {alarms[0]['required_checks_source']})."
        if alarms
        else ""
    )
    lines.append("")
    lines.append(
        "_This tracker updates in place each sweep and closes automatically "
        "once no PR is stuck merge-ready._"
    )
    return "\n".join(line for line in lines if line is not None)


def render_summary(
    findings: list[dict[str, Any]],
    threshold_minutes: float,
    errors: list[dict[str, Any]] | None = None,
) -> str:
    alarms = [f for f in findings if f["level"] == "alarm"]
    pendings = [f for f in findings if f["level"] == "pending"]
    queue_alarms = [f for f in findings if f["level"] == "queue_alarm"]
    throughput_alarms = [f for f in findings if f["level"] == "throughput_alarm"]
    blind_alarms = [f for f in findings if f["level"] == "blind_alarm"]
    lines = ["## Merge-stall watchdog", ""]
    if errors:
        lines.append(
            f"> **Degraded sweep** — {len(errors)} collection call(s) failed, so "
            "this snapshot may be incomplete. Next sweep in ~30 min."
        )
        lines.append("")
    if not findings:
        # Only a sweep that actually observed something may report calm. A
        # blind sweep produces a blind_alarm above and never lands here.
        lines.append(
            "No PR is merge-ready-and-stuck. Merges are flowing (or nothing is ready)."
        )
        return "\n".join(lines)
    lines.append(
        f"- **{len(alarms)}** alarm (merge-ready >= {threshold_minutes:g} min, two "
        "consecutive sweeps)"
    )
    lines.append(
        f"- **{len(pendings)}** pending (qualified this sweep only — promoted next "
        "sweep if it persists)"
    )
    lines.append(f"- **{len(queue_alarms)}** stalled merge queue")
    lines.append(f"- **{len(throughput_alarms)}** zero-merge-throughput")
    lines.append(f"- **{len(blind_alarms)}** blind sweep (could not observe)")
    lines.append("")
    lines.append("| level | subject | state | minutes |")
    lines.append("| --- | --- | --- | --- |")
    for f in findings:
        level = f["level"]
        if level == "queue_alarm":
            lines.append(
                f"| queue alarm | #{f['number']} | {f['queue_state']} | "
                f"{f['queue_minutes']:g} |"
            )
        elif level == "throughput_alarm":
            lines.append(
                f"| throughput alarm | base branch | "
                f"queue depth {f['queue_depth']} | "
                f"{f['minutes_since_last_merge']:g} |"
            )
        elif level == "blind_alarm":
            lines.append(
                f"| blind alarm | {', '.join(f['failed_stages'])} | "
                f"silenced: {', '.join(f['silenced_alarms']) or 'none'} | - |"
            )
        else:
            lines.append(
                f"| {level} | #{f['number']} | {f['merge_state_status']} | "
                f"{f['ready_minutes']:g} |"
            )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default="", help="owner/name (live collection)")
    ap.add_argument("--base", default="main", help="base branch for required-check lookup")
    ap.add_argument("--snapshot", default="", help="read a recorded snapshot instead of the API")
    ap.add_argument("--prev-state", default="", help="previous-tick stuck-PR state file")
    ap.add_argument("--threshold-minutes", type=float, default=THRESHOLD_MINUTES)
    ap.add_argument(
        "--queue-threshold-minutes",
        type=float,
        default=MERGE_QUEUE_THRESHOLD_MINUTES,
    )
    ap.add_argument(
        "--throughput-threshold-minutes",
        type=float,
        default=MERGE_THROUGHPUT_THRESHOLD_MINUTES,
    )
    ap.add_argument("--findings-out", default="findings.json")
    ap.add_argument("--snapshot-out", default="")
    ap.add_argument("--state-out", default="state.json")
    ap.add_argument("--body-out", default="body.md")
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
            print("merge_stall_watchdog: --repo required without --snapshot", file=sys.stderr)
            return 2
        snapshot = collect_snapshot(args.repo, args.base, now)

    prev_stuck: list[int] = []
    if args.prev_state and os.path.exists(args.prev_state):
        try:
            with open(args.prev_state, encoding="utf-8") as fh:
                prev_stuck = json.load(fh).get("stuck_prs", [])
        except (json.JSONDecodeError, OSError):
            prev_stuck = []

    findings, stuck_now = analyze(snapshot, prev_stuck, now, args.threshold_minutes)
    findings.extend(
        analyze_merge_queue(snapshot, now, args.queue_threshold_minutes)
    )
    findings.extend(
        analyze_merge_throughput(snapshot, now, args.throughput_threshold_minutes)
    )
    findings.extend(analyze_blindness(snapshot))
    alarms = [f for f in findings if f["level"] == "alarm"]
    queue_alarms = [f for f in findings if f["level"] == "queue_alarm"]
    throughput_alarms = [f for f in findings if f["level"] == "throughput_alarm"]
    blind_alarms = [f for f in findings if f["level"] == "blind_alarm"]
    actionable = alarms + queue_alarms + throughput_alarms + blind_alarms
    # An incomplete observation must never erase the prior sweep's memory.
    # Keep any newly observed stuck PRs too, but only a complete sweep may
    # remove a PR from the persisted two-sweep set.
    persisted_stuck = (
        sorted(set(prev_stuck) | set(stuck_now))
        if snapshot.get("errors")
        else stuck_now
    )

    with open(args.findings_out, "w", encoding="utf-8") as fh:
        json.dump(findings, fh, indent=2)
    with open(args.state_out, "w", encoding="utf-8") as fh:
        json.dump(
            {"generated_at": now.isoformat(), "stuck_prs": persisted_stuck},
            fh,
            indent=2,
        )
    if args.snapshot_out:
        with open(args.snapshot_out, "w", encoding="utf-8") as fh:
            json.dump(snapshot, fh, indent=2)
    if actionable:
        with open(args.body_out, "w", encoding="utf-8") as fh:
            fh.write(render_body(findings, args.threshold_minutes, now))

    summary = render_summary(findings, args.threshold_minutes, snapshot.get("errors"))
    print(summary)
    if args.summary_out:
        with open(args.summary_out, "a", encoding="utf-8") as fh:
            fh.write(summary + "\n")

    if snapshot.get("errors"):
        print(
            f"note: {len(snapshot['errors'])} collection call(s) failed.",
            file=sys.stderr,
        )

    # Exit 0 regardless of findings: the workflow decides what to do with them.
    # A watchdog that reddens its own run gets ignored.
    print(f"alarm_count={len(actionable)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
