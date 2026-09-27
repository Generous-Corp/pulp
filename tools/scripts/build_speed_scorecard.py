#!/usr/bin/env python3
"""Build-speed scorecard: local build cost, PR pipeline timing, fleet state.

Two subcommands:

  ingest   Backfill the required `macos` gate's job and step timings, merge-queue
           latency and merge_group outcomes from the GitHub API into Shipyard's
           metrics store (`shipyard metrics record`). Idempotent: rows already in
           the store (by external id) are skipped. Run it before `report`, or on a
           schedule, so `shipyard metrics watch` has history to judge drift.
  report   Render the scorecard. Pipeline numbers are READ FROM SHIPYARD, not
           recomputed from GitHub; fleet state is read from each host's published
           host_vitals.json; the local section uses build_time_report.py.

Where the data lands in Shipyard (one project per shape, so step and latency
rows never inflate a project's worker-minute totals):

  pulp               job "macos", target macos-gate/<event>       whole gate job
                     target macos-gate/merge_group/receipt-reused  hosted placeholder
                     target macos-gate/merge_group/placeholder/<kind>  what it stood for:
                            receipt-reused, skip-safe (no native input) or unknown
                     target macos-gate/<event>/no-runner-cancel  cancelled before any
                            runner took it (duration = the queue time it waited)
                     target release/<workflow>  a release-lane job on a gate runner
  pulp-gate-steps    target macos-gate/<event>/<Step|queue>        per step
  pulp-merge-queue   target pr/enqueue-to-merged, pr/open-to-merged, merge-group-run
                     target pr/gate-minutes      self-hosted gate minutes one merged PR cost
                     target pr/ejected/<reason>  one removal from the queue before merging

Host is the PHYSICAL host (m1/m3/m5) derived from the ephemeral runner name, so
`summary`/`watch` lanes are per machine rather than per throwaway runner.

`report --json` emits the section format `bench_diff.py` diffs, and `--baseline
FILE` renders the before→after delta through bench_diff.

Every probe that cannot answer says so (NOT MEASURED / UNREACHABLE); nothing
unmeasured is ever printed as a zero.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable, Iterable

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
DEFAULT_REPO = "Generous-Corp/pulp"
WORKFLOW_FILE = "build.yml"
WORKFLOW_NAME = "Build and Test"
GATE_JOB = "macos"
EVENTS = ("pull_request", "merge_group")
PROJECT_JOBS = "pulp"
PROJECT_STEPS = "pulp-gate-steps"
PROJECT_QUEUE = "pulp-merge-queue"

# Workflows whose macOS legs run on the self-hosted gate runners and so compete
# with the required gate for the same VMs.
RELEASE_WORKFLOWS = ("release-cli.yml", "sign-and-release.yml", "release-path-pr-gate.yml")

# Step display name → the gate step names it covers. The iOS compile gate runs
# inside "Build" today; a step whose name mentions it is picked up if it is
# ever split out.
STEP_NAMES: dict[str, tuple[str, ...]] = {
    "Configure": ("Configure",),
    "Build": ("Build",),
    "iOS compile gate": ("iOS compile gate",),
    "Test": ("Test (non-Windows)",),
    # Pull-request heads run only this label tier; the full suite runs in the
    # merge queue, so a pull_request "Test" row predates the split.
    "Fast tier": ("Test fast deterministic tier (pull request head)",),
    "SDK contract": ("Test installed SDK capability contract (affected changes)",),
}

# The standard blast-radius set the build-speed baseline was measured with.
STANDARD_FILES = (
    "core/view/src/widgets.cpp",
    "core/view/include/pulp/view/view.hpp",
    "core/view/include/pulp/view/widgets.hpp",
    "core/canvas/include/pulp/canvas/canvas.hpp",
    "core/format/include/pulp/format/processor.hpp",
    "core/runtime/include/pulp/runtime/log.hpp",
    "core/state/include/pulp/state/parameter.hpp",
    "core/audio/include/pulp/audio/buffer.hpp",
    "core/signal/include/pulp/signal/biquad.hpp",
    "core/timeline/include/pulp/timeline/model.hpp",
    "core/view/platform/mac/window_host_mac.mm",
    "core/host/src/signal_graph.cpp",
    "test/test_biquad.cpp",
)

DEFAULT_HOSTS = ("m3=local", "m5", "m1")

# A tree is "up to date" for blast-radius purposes when the control plans no
# more than the always-run steps (cargo, staging): 3 on the baseline tree.
BLAST_CLEAN_CONTROL_MAX = 10
DRIFT_SHOWN = 20


def _load_sibling(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules.setdefault(name, mod)
    spec.loader.exec_module(mod)
    return mod


# --------------------------------------------------------------------------
# small pure helpers
# --------------------------------------------------------------------------

def percentile(values: Iterable[float], q: float) -> float | None:
    """Linear-interpolated percentile (q in 0..100); None for no data."""
    xs = sorted(v for v in values if v is not None)
    if not xs:
        return None
    if len(xs) == 1:
        return float(xs[0])
    pos = (len(xs) - 1) * q / 100.0
    lo = int(pos)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (pos - lo)


def parse_ts(s: str | None) -> dt.datetime | None:
    if not s:
        return None
    try:
        return dt.datetime.fromisoformat(s.replace("Z", "+00:00"))
    except ValueError:
        return None


def iso(t: dt.datetime) -> str:
    return t.astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_since(spec: str, now: dt.datetime | None = None) -> dt.datetime:
    """`14d`, `36h`, or an ISO date → an aware UTC datetime."""
    now = now or dt.datetime.now(dt.timezone.utc)
    spec = spec.strip()
    if spec.endswith("d") and spec[:-1].isdigit():
        return now - dt.timedelta(days=int(spec[:-1]))
    if spec.endswith("h") and spec[:-1].isdigit():
        return now - dt.timedelta(hours=int(spec[:-1]))
    t = parse_ts(spec if "T" in spec else spec + "T00:00:00+00:00")
    if t is None:
        raise ValueError(f"cannot parse --since {spec!r} (use 14d, 36h or YYYY-MM-DD)")
    return t if t.tzinfo else t.replace(tzinfo=dt.timezone.utc)


def physical_host(runner_name: str | None) -> str | None:
    """Ephemeral runner name → physical host (studio-… is m3)."""
    if not runner_name or runner_name.startswith("GitHub Actions"):
        return None
    head = runner_name.split("-", 1)[0].lower()
    return {"studio": "m3"}.get(head, head)


def minutes(a: str | None, b: str | None) -> float | None:
    ta, tb = parse_ts(a), parse_ts(b)
    if not ta or not tb:
        return None
    return (tb - ta).total_seconds() / 60.0


# --------------------------------------------------------------------------
# GitHub → Shipyard records (pure: JSON in, record dicts out)
# --------------------------------------------------------------------------

def gate_job_records(run: dict, jobs: list[dict]) -> list[dict]:
    """Shipyard records for one workflow run's required `macos` gate job."""
    event = run.get("event")
    if event not in EVENTS:
        return []
    out: list[dict] = []
    pr = None
    prs = run.get("pull_requests") or []
    if prs:
        pr = prs[0].get("number")
    else:
        # A merge_group run carries no pull_requests; its queue branch names
        # the PR at the head of the group (gh-readonly-queue/main/pr-8760-<sha>).
        m = re.search(r"gh-readonly-queue/.+/pr-(\d+)-", run.get("head_branch") or "")
        if m:
            pr = int(m.group(1))
    common = {"repo": run.get("repository", {}).get("full_name") or DEFAULT_REPO,
              "workflow": WORKFLOW_NAME, "branch": run.get("head_branch"),
              "sha": run.get("head_sha"), "pr": pr}
    for job in jobs:
        if job.get("name") != GATE_JOB or job.get("status") != "completed":
            continue
        conclusion = job.get("conclusion") or "unknown"
        if conclusion == "skipped":
            continue
        base_id = f"github:{run['id']}/{job['id']}/{job.get('run_attempt', 1)}"
        start, end = job.get("started_at"), job.get("completed_at")
        dur = minutes(start, end)
        host = physical_host(job.get("runner_name"))
        if host is None:
            if not job.get("runner_name") and conclusion == "cancelled":
                # Cancelled while still queued: no gate minutes, but it is the
                # starvation signal (a newer push gave up waiting for a VM).
                waited = minutes(job.get("created_at"), end)
                if waited is not None and waited >= 0:
                    out.append({**common, "project": PROJECT_JOBS, "job": GATE_JOB,
                                "target": f"macos-gate/{event}/no-runner-cancel",
                                "platform": "macos", "backend": "vm", "provider": "tart-macos",
                                "host": "none", "duration_ms": round(waited * 60000),
                                "status": "cancelled", "started_at": job.get("created_at"),
                                "completed_at": end, "external_id": base_id})
                continue
            # A hosted placeholder: in a merge group it stands in for a reused
            # PR receipt or a skip-safe group; on a PR it means no native build
            # was required.
            if (event == "merge_group" and dur is not None
                    and str(job.get("runner_name") or "").startswith("GitHub Actions")):
                out.append({**common, "project": PROJECT_JOBS, "job": GATE_JOB,
                            "target": "macos-gate/merge_group/receipt-reused",
                            "platform": "macos", "backend": "github",
                            "provider": "github-hosted", "host": "github",
                            "runner": job.get("runner_name"), "duration_ms": round(dur * 60000),
                            "status": conclusion, "started_at": start, "completed_at": end,
                            "external_id": base_id})
            continue
        if dur is None:
            continue
        gate = {**common, "platform": "macos", "backend": "vm", "provider": "tart-macos",
                "host": host, "runner": job.get("runner_name"), "job": GATE_JOB}
        out.append({**gate, "project": PROJECT_JOBS, "target": f"macos-gate/{event}",
                    "duration_ms": round(dur * 60000), "status": conclusion,
                    "started_at": start, "completed_at": end, "external_id": base_id})
        queue = minutes(job.get("created_at"), start)
        if queue is not None and queue >= 0:
            out.append({**gate, "project": PROJECT_STEPS, "target": f"macos-gate/{event}/queue",
                        "duration_ms": round(queue * 60000), "status": "success",
                        "started_at": job.get("created_at"), "completed_at": start,
                        "external_id": base_id + "/queue"})
        for label, names in STEP_NAMES.items():
            for step in job.get("steps") or []:
                if step.get("name") not in names and not (
                        label == "iOS compile gate" and "ios compile" in step.get("name", "").lower()):
                    continue
                if step.get("conclusion") in (None, "skipped"):
                    continue
                sd = minutes(step.get("started_at"), step.get("completed_at"))
                if sd is None:
                    continue
                out.append({**gate, "project": PROJECT_STEPS,
                            "target": f"macos-gate/{event}/{label}",
                            "duration_ms": round(sd * 60000), "status": step.get("conclusion"),
                            "started_at": step.get("started_at"),
                            "completed_at": step.get("completed_at"),
                            "external_id": f"{base_id}/step/{label}"})
                break
    return out


def release_job_records(run: dict, jobs: list[dict], workflow: str) -> list[dict]:
    """Release-lane jobs that ran on a self-hosted gate runner: run time and queue."""
    out = []
    lane = workflow.rsplit(".", 1)[0]
    for job in jobs:
        host = physical_host(job.get("runner_name"))
        if host is None or job.get("status") != "completed":
            continue
        dur = minutes(job.get("started_at"), job.get("completed_at"))
        if dur is None:
            continue
        base_id = f"github:{run['id']}/{job['id']}/{job.get('run_attempt', 1)}"
        common = {"repo": DEFAULT_REPO, "workflow": run.get("name") or lane,
                  "branch": run.get("head_branch"), "sha": run.get("head_sha"),
                  "platform": "macos", "backend": "vm", "provider": "tart-macos",
                  "host": host, "runner": job.get("runner_name"), "job": job.get("name") or lane}
        out.append({**common, "project": PROJECT_JOBS, "target": f"release/{lane}",
                    "duration_ms": round(dur * 60000), "status": job.get("conclusion") or "unknown",
                    "started_at": job.get("started_at"), "completed_at": job.get("completed_at"),
                    "external_id": base_id})
        queue = minutes(job.get("created_at"), job.get("started_at"))
        if queue is not None and queue >= 0:
            out.append({**common, "project": PROJECT_STEPS, "target": f"release/{lane}/queue",
                        "duration_ms": round(queue * 60000), "status": "success",
                        "started_at": job.get("created_at"), "completed_at": job.get("started_at"),
                        "external_id": base_id + "/queue"})
    return out


def merge_group_run_record(run: dict) -> dict | None:
    if run.get("event") != "merge_group" or run.get("status") != "completed":
        return None
    dur = minutes(run.get("run_started_at") or run.get("created_at"), run.get("updated_at"))
    if dur is None:
        return None
    return {"project": PROJECT_QUEUE, "repo": DEFAULT_REPO, "workflow": WORKFLOW_NAME,
            "job": "merge_group", "target": "merge-group-run", "platform": "github",
            "backend": "github", "provider": "github-actions-run", "host": "github",
            "branch": run.get("head_branch"), "sha": run.get("head_sha"),
            "duration_ms": round(dur * 60000), "status": run.get("conclusion") or "unknown",
            "started_at": run.get("run_started_at"), "completed_at": run.get("updated_at"),
            "external_id": f"github-run:{run['id']}/{run.get('run_attempt', 1)}"}


def pr_latency_records(pr: dict) -> list[dict]:
    """Merge-queue latency rows for one merged PR (GraphQL node)."""
    n = pr.get("number")
    merged = pr.get("mergedAt")
    if not n or not merged:
        return []
    enq = sorted(e["createdAt"] for e in (pr.get("timelineItems") or {}).get("nodes", [])
                 if e.get("__typename") == "AddedToMergeQueueEvent" and e.get("createdAt")
                 and e["createdAt"] <= merged)
    common = {"project": PROJECT_QUEUE, "repo": DEFAULT_REPO, "job": "pr", "platform": "github",
              "backend": "github", "provider": "github-pr", "host": "github", "pr": n,
              "status": "success", "completed_at": merged}
    out = []
    opened = minutes(pr.get("createdAt"), merged)
    if opened is not None:
        out.append({**common, "target": "pr/open-to-merged", "started_at": pr.get("createdAt"),
                    "duration_ms": round(opened * 60000), "external_id": f"github-pr:{n}/open"})
    if enq:
        first = minutes(enq[0], merged)
        last = minutes(enq[-1], merged)
        out.append({**common, "target": "pr/enqueue-to-merged", "started_at": enq[0],
                    "duration_ms": round(first * 60000), "external_id": f"github-pr:{n}/enqueue"})
        out.append({**common, "target": "pr/last-enqueue-to-merged", "started_at": enq[-1],
                    "duration_ms": round(last * 60000),
                    "external_id": f"github-pr:{n}/last-enqueue"})
    out += pr_ejection_records(pr)
    return out


def pr_ejection_records(pr: dict) -> list[dict]:
    """One row per removal from the merge queue before the PR merged.

    Duration is the queue time the removal threw away (the latest enqueue
    before it → the removal); the reason (failed_checks, merge_conflict,
    manual, ...) is part of the target so it survives `metrics list`.
    """
    n, merged = pr.get("number"), pr.get("mergedAt")
    if not n or not merged:
        return []
    items = sorted((e for e in (pr.get("timelineItems") or {}).get("nodes", [])
                    if e.get("createdAt") and e["createdAt"] <= merged),
                   key=lambda e: e["createdAt"])
    out, enq = [], None
    for e in items:
        kind = e.get("__typename")
        if kind == "AddedToMergeQueueEvent":
            enq = e["createdAt"]
        elif kind == "RemovedFromMergeQueueEvent":
            reason = str(e.get("reason") or "unknown").lower()
            if reason == "merged":
                continue
            lost = minutes(enq, e["createdAt"]) if enq else None
            out.append({"project": PROJECT_QUEUE, "repo": DEFAULT_REPO, "job": "pr",
                        "platform": "github", "backend": "github", "provider": "github-pr",
                        "host": "github", "pr": n, "status": "failure",
                        "target": f"pr/ejected/{reason}", "started_at": enq,
                        "completed_at": e["createdAt"],
                        "duration_ms": round((lost or 0) * 60000),
                        "external_id": f"github-pr:{n}/ejected/{e['createdAt']}"})
            enq = None
    return out


def run_pr(run: dict) -> int | None:
    """The PR a Build and Test run validated: the PR head, or the merge-queue entry."""
    prs = run.get("pull_requests") or []
    if prs:
        return prs[0].get("number")
    m = re.search(r"gh-readonly-queue/.+/pr-(\d+)-", run.get("head_branch") or "")
    return int(m.group(1)) if m else None


GATE_SETTLE = dt.timedelta(hours=1)
PLACEHOLDER = "macos-gate/merge_group/receipt-reused"

# The hosted `macos` placeholder names its reason in an annotation: either a
# protected PR receipt was reused, or the merge group changed no native build
# input and the matrix was skipped. Only the first is receipt reuse.
PLACEHOLDER_KINDS = (("receipt from an earlier run was reused", "receipt-reused"),
                     ("changed no native build input", "skip-safe"))


def placeholder_kind(annotations: list[dict]) -> str:
    for a in annotations or []:
        msg = str(a.get("message") or "")
        for needle, kind in PLACEHOLDER_KINDS:
            if needle in msg:
                return kind
    return "unknown"


def placeholder_kind_record(row: dict, kind: str) -> dict:
    """Classify one placeholder row; its own external id + '/kind' keeps it idempotent."""
    return {"project": PROJECT_JOBS, "repo": DEFAULT_REPO, "workflow": WORKFLOW_NAME,
            "job": "macos-placeholder", "target": f"macos-gate/merge_group/placeholder/{kind}",
            "platform": "macos", "backend": "github", "provider": "github-hosted",
            "host": "github", "duration_ms": 0, "status": "success",
            "started_at": row.get("completed_at"), "completed_at": row.get("completed_at"),
            "external_id": f"{row['external_id']}/kind"}


def pr_gate_minutes_records(prs: list[dict], pr_of_run: dict[str, int],
                            gate_ms_of_run: dict[str, int], since: dt.datetime,
                            now: dt.datetime) -> list[dict]:
    """Self-hosted `macos` gate minutes each merged PR cost (PR heads + merge groups).

    Every attempt counts, cancelled and failed included: a gate job cancelled
    by a newer push or a refresh is a cost of landing that PR. Only PRs opened
    inside the ingested window (older runs are not listed) and merged at least
    GATE_SETTLE ago (a cancelled run may still be completing) get a row, so a
    row is final when written and the external id never needs rewriting.
    """
    by_pr: dict[int, int] = {}
    for run_id, pr in pr_of_run.items():
        if run_id in gate_ms_of_run:
            by_pr[pr] = by_pr.get(pr, 0) + gate_ms_of_run[run_id]
    out = []
    for pr in prs:
        n, merged, opened = pr.get("number"), parse_ts(pr.get("mergedAt")), parse_ts(pr.get("createdAt"))
        if not n or not merged or not opened or opened < since or now - merged < GATE_SETTLE:
            continue
        out.append({"project": PROJECT_QUEUE, "repo": DEFAULT_REPO, "job": "pr",
                    "platform": "github", "backend": "github", "provider": "github-pr",
                    "host": "github", "pr": n, "status": "success", "target": "pr/gate-minutes",
                    "started_at": pr.get("createdAt"), "completed_at": pr.get("mergedAt"),
                    "duration_ms": by_pr.get(n, 0), "external_id": f"github-pr:{n}/gate-minutes"})
    return out


def record_argv(rec: dict, shipyard: str = "shipyard") -> list[str]:
    argv = [shipyard, "metrics", "record", "--project", rec["project"], "--job", rec["job"]]
    flags = {"repo": "--repo", "workflow": "--workflow", "branch": "--branch", "sha": "--sha",
             "pr": "--pr", "target": "--target", "platform": "--platform", "backend": "--backend",
             "provider": "--provider", "runner": "--runner", "host": "--host",
             "duration_ms": "--duration-ms", "status": "--status", "started_at": "--started-at",
             "completed_at": "--completed-at", "external_id": "--external-id"}
    for key, flag in flags.items():
        v = rec.get(key)
        if v is None or v == "":
            continue
        argv += [flag, str(v)]
    return argv


# --------------------------------------------------------------------------
# I/O: GitHub and Shipyard
# --------------------------------------------------------------------------

class GitHub:
    """`ghapp api` from a Pulp checkout (ghapp derives its identity from cwd)."""

    def __init__(self, repo: str, gh: str = "ghapp", cwd: Path = REPO_ROOT):
        self.repo, self.gh, self.cwd = repo, gh, cwd

    def api(self, path: str, *extra: str) -> Any:
        proc = subprocess.run([self.gh, "api", path, *extra], cwd=self.cwd,
                              capture_output=True, text=True, timeout=120)
        if proc.returncode != 0:
            raise RuntimeError(f"{self.gh} api {path}: {proc.stderr.strip()[:300]}")
        return json.loads(proc.stdout or "null")

    def runs_for_day(self, day: dt.date, event: str | None,
                     workflow: str = WORKFLOW_FILE) -> list[dict]:
        runs: list[dict] = []
        page = 1
        ev = f"&event={event}" if event else ""
        while True:
            data = self.api(f"repos/{self.repo}/actions/workflows/{workflow}/runs"
                            f"?per_page=100&page={page}{ev}&created={day.isoformat()}")
            batch = data.get("workflow_runs", [])
            runs += batch
            if len(batch) < 100 or len(runs) >= data.get("total_count", 0) or page >= 10:
                return runs
            page += 1

    def jobs(self, run_id: int) -> list[dict]:
        # Job ids come from the run's own jobs listing; a check-run id handed to
        # actions/jobs returns an unrelated job without an error.
        return self.api(f"repos/{self.repo}/actions/runs/{run_id}/jobs?per_page=100").get("jobs", [])

    def merged_prs_for_day(self, day: dt.date) -> list[dict]:
        query = ("query($q:String!,$after:String){search(query:$q,type:ISSUE,first:100,"
                 "after:$after){pageInfo{hasNextPage endCursor} nodes{... on PullRequest{"
                 "number createdAt mergedAt timelineItems(first:100,itemTypes:"
                 "[ADDED_TO_MERGE_QUEUE_EVENT,REMOVED_FROM_MERGE_QUEUE_EVENT]){nodes{__typename"
                 " ... on AddedToMergeQueueEvent{createdAt}"
                 " ... on RemovedFromMergeQueueEvent{createdAt reason}}}}}}}")
        q = f"repo:{self.repo} is:pr is:merged merged:{day.isoformat()}"
        out: list[dict] = []
        after = None
        while True:
            args = ["-f", f"q={q}", "-f", f"query={query}"]
            if after:
                args += ["-f", f"after={after}"]
            data = self.api("graphql", *args)["data"]["search"]
            out += [n for n in data["nodes"] if n]
            if not data["pageInfo"]["hasNextPage"]:
                return out
            after = data["pageInfo"]["endCursor"]


class Shipyard:
    def __init__(self, binary: str | None = None, mode: str | None = None):
        self.bin = binary or shutil.which("shipyard")
        self.mode = mode

    @property
    def available(self) -> bool:
        return bool(self.bin)

    def _args(self, *a: str) -> list[str]:
        argv = [self.bin, "metrics", *a]
        if self.mode:
            argv += ["--mode", self.mode]
        return argv

    def list_rows(self, project: str, limit: int = 1_000_000) -> list[dict]:
        proc = subprocess.run(self._args("list", "--project", project, "--limit", str(limit),
                                         "--json"), capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"shipyard metrics list: {proc.stderr.strip()[:300]}")
        return json.loads(proc.stdout).get("rows", [])

    def watch(self, project: str, since: str) -> list[dict]:
        proc = subprocess.run(self._args("watch", "--project", project, "--since", since, "--json"),
                              capture_output=True, text=True)
        if proc.returncode != 0:
            return []
        return json.loads(proc.stdout).get("findings", [])

    def record(self, rec: dict) -> bool:
        argv = record_argv(rec, self.bin)
        if self.mode:
            argv += ["--mode", self.mode]
        return subprocess.run(argv, capture_output=True, text=True).returncode == 0


def gate_ms_by_run(rows: list[dict]) -> dict[str, int]:
    """Self-hosted gate job milliseconds per workflow run id, any conclusion.

    Accepts both shapes: `metrics list` rows (total_ms) and fresh records
    (duration_ms). The run id is the first segment of the gate external id.
    """
    out: dict[str, int] = {}
    seen: set[str] = set()
    for r in rows:
        ext = str(r.get("external_id") or "")
        if (not ext.startswith("github:") or ext in seen or r.get("host") in (None, "github")
                or r.get("target") not in {f"macos-gate/{e}" for e in EVENTS}):
            continue
        seen.add(ext)
        ms = r.get("total_ms", r.get("duration_ms"))
        if ms is None:
            continue
        run_id = ext[len("github:"):].split("/", 1)[0]
        out[run_id] = out.get(run_id, 0) + int(ms)
    return out


def ingest(gh: GitHub, sy: Shipyard, since: dt.datetime, dry_run: bool = False,
           workers: int = 8, log: Callable[[str], None] = print) -> dict:
    now = dt.datetime.now(dt.timezone.utc)
    days = [(since + dt.timedelta(days=i)).date()
            for i in range((now.date() - since.date()).days + 1)]
    runs: list[dict] = []
    for day in days:
        for event in EVENTS:
            runs += [r for r in gh.runs_for_day(day, event) if r.get("status") == "completed"]
    log(f"ingest: {len(runs)} completed {WORKFLOW_FILE} runs since {since.date()}")

    existing: set[str] = set()
    job_rows: list[dict] = []
    if sy.available:
        for project in (PROJECT_JOBS, PROJECT_STEPS, PROJECT_QUEUE):
            rows = sy.list_rows(project)
            if project == PROJECT_JOBS:
                job_rows = rows
            existing |= {r.get("external_id") for r in rows if r.get("external_id")}

    records: list[dict] = []
    for r in runs:
        rec = merge_group_run_record(r)
        if rec:
            records.append(rec)

    # A run whose gate job is already recorded needs no jobs call; this is what
    # keeps a scheduled re-ingest cheap.
    seen_runs = {e.split("/", 1)[0][len("github:"):] for e in existing if e.startswith("github:")}
    to_fetch = [r for r in runs if str(r["id"]) not in seen_runs]

    def fetch(run: dict) -> list[dict]:
        return gate_job_records(run, gh.jobs(run["id"]))

    release_runs = [(wf, r) for day in days for wf in RELEASE_WORKFLOWS
                    for r in gh.runs_for_day(day, None, wf)
                    if r.get("status") == "completed" and str(r["id"]) not in seen_runs]

    def fetch_release(item: tuple[str, dict]) -> list[dict]:
        wf, run = item
        return release_job_records(run, gh.jobs(run["id"]), wf)

    errors = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for fut in concurrent.futures.as_completed(
                [pool.submit(fetch_release, it) for it in release_runs]):
            try:
                records += fut.result()
            except RuntimeError as exc:
                errors += 1
                log(f"ingest: release jobs fetch failed: {exc}")
        for fut in concurrent.futures.as_completed([pool.submit(fetch, r) for r in to_fetch]):
            try:
                records += fut.result()
            except RuntimeError as exc:
                errors += 1
                log(f"ingest: jobs fetch failed: {exc}")
    merged: list[dict] = []
    for day in days:
        merged += gh.merged_prs_for_day(day)
    for pr in merged:
        records += pr_latency_records(pr)
    pr_of_run = {str(r["id"]): p for r in runs if (p := run_pr(r)) is not None}
    records += pr_gate_minutes_records(merged, pr_of_run,
                                       gate_ms_by_run(job_rows + records), since, now)

    # Classify every placeholder not yet classified, history included: the
    # annotation outlives the run, so old rows gain a kind on the next ingest.
    known = existing | {r["external_id"] for r in records}
    todo = {r["external_id"]: r for r in job_rows + records
            if r.get("target") == PLACEHOLDER and r.get("status") == "success"
            and str(r.get("external_id", "")).startswith("github:")
            and f"{r['external_id']}/kind" not in known}

    def classify(row: dict) -> dict:
        job_id = row["external_id"][len("github:"):].split("/")[1]
        return placeholder_kind_record(
            row, placeholder_kind(gh.api(f"repos/{gh.repo}/check-runs/{job_id}/annotations")))

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for fut in concurrent.futures.as_completed([pool.submit(classify, r) for r in todo.values()]):
            try:
                records.append(fut.result())
            except RuntimeError as exc:
                errors += 1
                log(f"ingest: placeholder annotations failed: {exc}")

    new = [r for r in records if r["external_id"] not in existing]
    written = 0
    if not dry_run:
        if not sy.available:
            raise RuntimeError("shipyard is not on PATH; nothing can be recorded")
        for rec in new:
            written += sy.record(rec)
    summary = {"runs": len(runs), "jobs_fetched": len(to_fetch), "records": len(records), "already_recorded": len(records) - len(new),
               "new": len(new), "written": written, "fetch_errors": errors, "dry_run": dry_run}
    log("ingest: " + json.dumps(summary))
    return summary


# --------------------------------------------------------------------------
# report: pipeline (from Shipyard rows)
# --------------------------------------------------------------------------

def _in_window(rows: list[dict], since: dt.datetime) -> list[dict]:
    out = []
    for r in rows:
        t = parse_ts(r.get("completed_at"))
        if t and t >= since:
            out.append(r)
    return out


def _mins(rows: list[dict]) -> list[float]:
    return [r["total_ms"] / 60000.0 for r in rows if r.get("total_ms") is not None]


def pipeline_stats(job_rows: list[dict], step_rows: list[dict], queue_rows: list[dict],
                   since: dt.datetime) -> dict:
    """Windowed pipeline numbers from Shipyard `metrics list` rows."""
    jobs = [r for r in _in_window(job_rows, since) if str(r.get("target", "")).startswith("macos-gate/")]
    steps = _in_window(step_rows, since)
    queue = _in_window(queue_rows, since)
    ok = [r for r in jobs if r.get("status") == "success"]
    gate: dict[str, Any] = {}
    for ev in EVENTS:
        xs = _mins([r for r in ok if r.get("target") == f"macos-gate/{ev}"])
        gate[ev] = {"n": len(xs), "p50": percentile(xs, 50), "p90": percentile(xs, 90)}
    hosts: dict[str, Any] = {}
    for r in ok:
        if r.get("target") in (f"macos-gate/{e}" for e in EVENTS):
            hosts.setdefault(r.get("host"), []).append(r["total_ms"] / 60000.0)
    by_host = {h: {"n": len(v), "p50": percentile(v, 50), "p90": percentile(v, 90)}
               for h, v in sorted(hosts.items())}
    step_stats: dict[str, Any] = {}
    for r in steps:
        if r.get("status") not in ("success", None) and not str(r.get("target", "")).endswith("/queue"):
            continue
        step_stats.setdefault(r.get("target"), []).append(r["total_ms"] / 60000.0)
    step_p50 = {t: {"n": len(v), "p50": percentile(v, 50), "p90": percentile(v, 90)}
                for t, v in sorted(step_stats.items())}
    reused = [r for r in jobs if r.get("target") == "macos-gate/merge_group/receipt-reused"]
    mg_runs = [r for r in queue if r.get("target") == "merge-group-run"
               and r.get("status") in ("success", "failure")]
    per_day: dict[str, list[int]] = {}
    for r in mg_runs:
        day = r["completed_at"][:10]
        tally = per_day.setdefault(day, [0, 0])
        tally[0] += r.get("status") == "failure"
        tally[1] += 1
    fail_total = sum(v[0] for v in per_day.values())
    runs_total = sum(v[1] for v in per_day.values())
    lat = {}
    for tgt, scale in (("pr/enqueue-to-merged", 1.0), ("pr/last-enqueue-to-merged", 1.0),
                       ("pr/open-to-merged", 1 / 60.0)):
        xs = [m * scale for m in _mins([r for r in queue if r.get("target") == tgt])]
        lat[tgt] = {"n": len(xs), "p50": percentile(xs, 50), "p90": percentile(xs, 90),
                    "unit": "h" if scale != 1.0 else "min"}
    return {
        "since": iso(since),
        "gate": gate,
        "gate_by_host": by_host,
        "gate_failures": {ev: sum(1 for r in jobs if r.get("target") == f"macos-gate/{ev}"
                                  and r.get("status") == "failure") for ev in EVENTS},
        "steps": step_p50,
        "receipt_reused": len(reused),
        "merge_group_failure": {
            "per_day": {d: {"failed": v[0], "runs": v[1], "rate": v[0] / v[1]}
                        for d, v in sorted(per_day.items())},
            "window_rate": (fail_total / runs_total) if runs_total else None,
            "runs": runs_total,
        },
        "latency": lat,
    }


def split_halves(since: dt.datetime, split: dt.datetime, after_from: dt.datetime | None,
                 until: dt.datetime | None) -> dict[str, tuple[dt.datetime, dt.datetime | None]]:
    """before = [since, split); after = [after_from or split, until or now).

    A later `--after-from` leaves a gap (a rollout, a confounded day) out of
    both sides instead of charging it to either regime.
    """
    if split <= since:
        raise ValueError(f"--split {iso(split)} must be after the window start {iso(since)}")
    lo = after_from or split
    if lo < split:
        raise ValueError(f"--after-from {iso(lo)} must not be before --split {iso(split)}")
    if until is not None and until <= lo:
        raise ValueError(f"--until {iso(until)} must be after the after-window start {iso(lo)}")
    return {"before": (since, split), "after": (lo, until)}


def _between(rows: list[dict], lo: dt.datetime, hi: dt.datetime | None) -> list[dict]:
    out = []
    for r in rows:
        t = parse_ts(r.get("completed_at"))
        if t and t >= lo and (hi is None or t < hi):
            out.append(r)
    return out


def _p(rows: list[dict]) -> dict:
    xs = _mins(rows)
    return {"n": len(xs), "p50": percentile(xs, 50), "p90": percentile(xs, 90)}


def split_stats(job_rows: list[dict], step_rows: list[dict], since: dt.datetime,
                split: dt.datetime, after_from: dt.datetime | None = None,
                until: dt.datetime | None = None) -> dict:
    """Gate job and step timings before vs after one instant.

    `shipyard metrics watch` judges drift by halving a fixed window, which
    straddles whatever changed mid-window; an explicit split point (a merge, an
    incident's end) compares the two regimes instead. `shipyard metrics
    compare` splits only on whole days ago and cannot filter by target, so this
    reads the same `metrics list` rows the pipeline section already fetched.
    Successful runs only: a failure's duration measures where it stopped.
    """
    halves = split_halves(since, split, after_from, until)
    out: dict[str, Any] = {"since": iso(since), "split": iso(split), "rows": [],
                           "windows": {k: [iso(lo), iso(hi) if hi else None]
                                       for k, (lo, hi) in halves.items()}}
    ok_jobs = [r for r in job_rows if r.get("status") == "success"]
    ok_steps = [r for r in step_rows if r.get("status") in ("success", None)
                or str(r.get("target", "")).endswith("/queue")]
    for ev in EVENTS:
        labels = [("gate job", [r for r in ok_jobs if r.get("target") == f"macos-gate/{ev}"])]
        for label in ("queue", *STEP_NAMES):
            labels.append((label, [r for r in ok_steps
                                   if r.get("target") == f"macos-gate/{ev}/{label}"]))
        for label, rows in labels:
            row = {"event": ev, "stage": label}
            for side, (lo, hi) in halves.items():
                row[side] = _p(_between(rows, lo, hi))
            b, a = row["before"]["p50"], row["after"]["p50"]
            row["p50_change_pct"] = (None if b in (None, 0) or a is None
                                     else (a - b) / b * 100.0)
            out["rows"].append(row)
    return out


def render_split_markdown(sp: dict) -> list[str]:
    L = [f"### Before / after {sp['split']} (window from {sp['since']})", "",
         "| Event | Stage | before n | before p50 | before p90 | after n | after p50 | after p90 | p50 change |",
         "|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    for row in sp["rows"]:
        b, a = row["before"], row["after"]
        if not b["n"] and not a["n"]:
            continue
        ch = row["p50_change_pct"]
        L.append(f"| {row['event']} | {row['stage']} | {b['n']} | {_fmt(_r(b['p50']), 'min')} | "
                 f"{_fmt(_r(b['p90']), 'min')} | {a['n']} | {_fmt(_r(a['p50']), 'min')} | "
                 f"{_fmt(_r(a['p90']), 'min')} | {'NOT MEASURED' if ch is None else f'{ch:+.0f}%'} |")
    L.append("\nSuccessful runs only. A side with n < 5 is anecdote, not a trend.")
    return L


def _batch_sizes(merge_times: list[str]) -> list[int]:
    """PRs per merge-queue push, one entry per PR: PRs a single push merged share
    a mergedAt to the second, so a group is the set with one timestamp."""
    counts: dict[str, int] = {}
    for t in merge_times:
        counts[t] = counts.get(t, 0) + 1
    return [counts[t] for t in merge_times]


def merge_split_stats(job_rows: list[dict], step_rows: list[dict], queue_rows: list[dict],
                      since: dt.datetime, split: dt.datetime,
                      after_from: dt.datetime | None = None,
                      until: dt.datetime | None = None,
                      now: dt.datetime | None = None) -> dict:
    """Time-to-merge and gate cost before vs after the split.

    Everything is keyed on when it completed: a PR on its merge time, a gate
    job on its completion. Rates are normalised per merged PR and per day so
    windows of different lengths compare.
    """
    now = now or dt.datetime.now(dt.timezone.utc)
    out: dict[str, Any] = {"sides": {}}
    gate_targets = {f"macos-gate/{e}" for e in EVENTS}
    for side, (lo, hi) in split_halves(since, split, after_from, until).items():
        days = ((hi or now) - lo).total_seconds() / 86400.0
        q = _between(queue_rows, lo, hi)
        jobs = _between(job_rows, lo, hi)
        steps = _between(step_rows, lo, hi)
        opened = [r for r in q if r.get("target") == "pr/open-to-merged"]
        merged_n = len(opened)
        lat = {t: _p([r for r in q if r.get("target") == t])
               for t in ("pr/last-enqueue-to-merged", "pr/enqueue-to-merged")}
        o = _p(opened)
        lat["pr/open-to-merged"] = {"n": o["n"], "p50": o["p50"] and o["p50"] / 60.0,
                                    "p90": o["p90"] and o["p90"] / 60.0}
        batches = _batch_sizes([r["completed_at"] for r in opened])
        gm = [r["total_ms"] / 60000.0 for r in q if r.get("target") == "pr/gate-minutes"]
        own = [r for r in jobs if r.get("target") in gate_targets and r.get("host") != "github"]
        cancelled = sum(r["total_ms"] for r in own if r.get("status") == "cancelled") / 60000.0
        selfhosted_mg = [r for r in own if r.get("target") == "macos-gate/merge_group"
                         and r.get("status") in ("success", "failure")]
        kinds: dict[str, int] = {}
        for r in jobs:
            t = str(r.get("target", ""))
            if t.startswith("macos-gate/merge_group/placeholder/"):
                k = t.rsplit("/", 1)[1]
                kinds[k] = kinds.get(k, 0) + 1
        placeholders = sum(1 for r in jobs if r.get("target") == PLACEHOLDER
                           and r.get("status") == "success")
        reused = kinds.get("receipt-reused", 0)
        mg = [r for r in q if r.get("target") == "merge-group-run"]
        mg_done = [r for r in mg if r.get("status") in ("success", "failure")]
        ejected = [r for r in q if str(r.get("target", "")).startswith("pr/ejected/")]
        reasons: dict[str, int] = {}
        for r in ejected:
            k = r["target"].split("/", 2)[2]
            reasons[k] = reasons.get(k, 0) + 1
        starved = {ev: [r for r in jobs if r.get("target") == f"macos-gate/{ev}/no-runner-cancel"]
                   for ev in EVENTS}
        release = [r for r in jobs if str(r.get("target", "")).startswith("release/")]
        release_q = [r for r in steps if str(r.get("target", "")).startswith("release/")
                     and str(r.get("target")).endswith("/queue")]
        hosts: dict[str, dict] = {}
        for ev in EVENTS:
            per: dict[str, list[dict]] = {}
            for r in steps:
                if r.get("target") == f"macos-gate/{ev}/queue":
                    per.setdefault(r.get("host") or "?", []).append(r)
            hosts[ev] = {h: _p(v) for h, v in sorted(per.items())}
        out["sides"][side] = {
            "window": [iso(lo), iso(hi or now)], "days": days,
            "merged": merged_n, "merged_per_day": merged_n / days if days > 0 else None,
            "no_runner_cancels": {ev: {"n": len(v), **{k: x for k, x in _p(v).items() if k != "n"},
                                       "per_day": len(v) / days if days > 0 else None}
                                  for ev, v in starved.items()},
            "release_on_gate": {"jobs": len(release),
                                "minutes": sum(r["total_ms"] for r in release) / 60000.0,
                                "queue": _p(release_q)},
            "latency": lat,
            "batch": {"n": len(batches),
                      "mean": (sum(batches) / len(batches)) if batches else None},
            "gate_minutes_per_pr": {"n": len(gm), "p50": percentile(gm, 50),
                                    "mean": (sum(gm) / len(gm)) if gm else None},
            "cancelled_gate_minutes": {
                "total": cancelled, "jobs": sum(1 for r in own if r.get("status") == "cancelled"),
                "per_day": cancelled / days if days > 0 else None,
                "per_merged_pr": cancelled / merged_n if merged_n else None},
            # Reuse counts only placeholders whose annotation says a receipt
            # was reused; skip-safe groups never needed the native gate.
            "receipt_reuse": {"reused": reused, "merge_groups": reused + len(selfhosted_mg),
                              "rate": (reused / (reused + len(selfhosted_mg))
                                       if reused or selfhosted_mg else None),
                              # Placeholders from before the gate annotated its
                              # reason could be either kind: the upper bound
                              # counts every one of them as a reuse.
                              "rate_upper": ((reused + kinds.get("unknown", 0))
                                             / (reused + kinds.get("unknown", 0) + len(selfhosted_mg))
                                             if reused or selfhosted_mg or kinds.get("unknown")
                                             else None),
                              "skip_safe": kinds.get("skip-safe", 0),
                              "unclassified": max(0, placeholders - sum(kinds.values())),
                              "unknown": kinds.get("unknown", 0)},
            # The required check itself: a failed Build and Test run can be an
            # advisory leg (hosted Linux) that ejects nothing.
            "merge_group_gate": {"completed": len(selfhosted_mg),
                                 "failed": sum(1 for r in selfhosted_mg if r["status"] == "failure"),
                                 "failure_rate": (sum(1 for r in selfhosted_mg
                                                      if r["status"] == "failure") / len(selfhosted_mg)
                                                  if selfhosted_mg else None)},
            "merge_group_runs": {
                "completed": len(mg), "failed": sum(1 for r in mg_done if r["status"] == "failure"),
                "cancelled": sum(1 for r in mg if r.get("status") == "cancelled"),
                "failure_rate": (sum(1 for r in mg_done if r["status"] == "failure") / len(mg_done)
                                 if mg_done else None)},
            "ejections": {"total": len(ejected), "by_reason": dict(sorted(reasons.items())),
                          "per_merged_pr": len(ejected) / merged_n if merged_n else None},
            "queue_by_host": hosts,
        }
    return out


def _pct_change(b: float | None, a: float | None) -> str:
    if b in (None, 0) or a is None:
        return "NOT MEASURED"
    return f"{(a - b) / b * 100:+.0f}%"


def render_merge_split_markdown(ms: dict) -> list[str]:
    b, a = ms["sides"]["before"], ms["sides"]["after"]
    L = ["", f"#### Time to merge and gate cost (before {b['window'][0]} → {b['window'][1]}, "
             f"after {a['window'][0]} → {a['window'][1]})", "",
         "| Metric | before n | before | after n | after | change |",
         "|---|---:|---:|---:|---:|---:|"]

    def line(label: str, bn: Any, bv: float | None, an: Any, av: float | None, unit: str,
             nd: int = 1) -> None:
        # A rate's change is in percentage points; a relative change of a
        # rate that starts at 0% has no meaning.
        ch = (f"{av - bv:+.0f} pp" if unit == "%" and None not in (av, bv)
              else "NOT MEASURED" if unit == "%" else _pct_change(bv, av))
        L.append(f"| {label} | {bn} | {_fmt(_r(bv, nd), unit)} | {an} | {_fmt(_r(av, nd), unit)} | "
                 f"{ch} |")

    for key, label, unit in (("pr/open-to-merged", "PR open→merged", "h"),
                             ("pr/last-enqueue-to-merged", "last enqueue→merged", "min"),
                             ("pr/enqueue-to-merged", "first enqueue→merged", "min")):
        for q in ("p50", "p90"):
            line(f"{label} {q}", b["latency"][key]["n"], b["latency"][key][q],
                 a["latency"][key]["n"], a["latency"][key][q], unit)
    line("merged PRs per day", b["merged"], b["merged_per_day"], a["merged"], a["merged_per_day"], "")
    line("merged PRs per hour", b["merged"], b["merged_per_day"] and b["merged_per_day"] / 24,
         a["merged"], a["merged_per_day"] and a["merged_per_day"] / 24, "")
    line("PRs per merge-queue push (mean)", b["batch"]["n"], b["batch"]["mean"],
         a["batch"]["n"], a["batch"]["mean"], "", 2)
    g_b, g_a = b["gate_minutes_per_pr"], a["gate_minutes_per_pr"]
    line("gate-minutes per merged PR (mean)", g_b["n"], g_b["mean"], g_a["n"], g_a["mean"], "min")
    line("gate-minutes per merged PR (p50)", g_b["n"], g_b["p50"], g_a["n"], g_a["p50"], "min")
    c_b, c_a = b["cancelled_gate_minutes"], a["cancelled_gate_minutes"]
    line("cancelled gate-minutes per day", c_b["jobs"], c_b["per_day"], c_a["jobs"], c_a["per_day"], "min")
    line("cancelled gate-minutes per merged PR", c_b["jobs"], c_b["per_merged_pr"], c_a["jobs"],
         c_a["per_merged_pr"], "min")
    r_b, r_a = b["receipt_reuse"], a["receipt_reuse"]
    line("receipt reuse rate", r_b["merge_groups"], r_b["rate"] and r_b["rate"] * 100,
         r_a["merge_groups"], r_a["rate"] and r_a["rate"] * 100, "%", 0)
    if r_b["unknown"] or r_a["unknown"]:
        line("receipt reuse rate, upper bound (unannotated placeholders as reuse)",
             r_b["merge_groups"] + r_b["unknown"], r_b["rate_upper"] and r_b["rate_upper"] * 100,
             r_a["merge_groups"] + r_a["unknown"], r_a["rate_upper"] and r_a["rate_upper"] * 100,
             "%", 0)
    m_b, m_a = b["merge_group_runs"], a["merge_group_runs"]
    line("merge_group run failure rate", m_b["completed"], m_b["failure_rate"] and m_b["failure_rate"] * 100,
         m_a["completed"], m_a["failure_rate"] and m_a["failure_rate"] * 100, "%", 0)
    g_b2, g_a2 = b["merge_group_gate"], a["merge_group_gate"]
    line("merge_group `macos` gate failure rate", g_b2["completed"],
         g_b2["failure_rate"] and g_b2["failure_rate"] * 100, g_a2["completed"],
         g_a2["failure_rate"] and g_a2["failure_rate"] * 100, "%", 0)
    e_b, e_a = b["ejections"], a["ejections"]
    line("ejections per 100 merged PRs", e_b["total"],
         e_b["per_merged_pr"] and e_b["per_merged_pr"] * 100, e_a["total"],
         e_a["per_merged_pr"] and e_a["per_merged_pr"] * 100, "")
    for ev in EVENTS:
        sb, sa = b["no_runner_cancels"][ev], a["no_runner_cancels"][ev]
        line(f"{ev} gate jobs cancelled with no runner, per day", sb["n"], sb["per_day"],
             sa["n"], sa["per_day"], "")
        line(f"{ev} no-runner cancel: wait before giving up p50", sb["n"], sb["p50"],
             sa["n"], sa["p50"], "min")
    rb, ra = b["release_on_gate"], a["release_on_gate"]
    line("release-lane jobs on gate runners: queue p50", rb["queue"]["n"], rb["queue"]["p50"],
         ra["queue"]["n"], ra["queue"]["p50"], "min")
    line("release-lane gate-runner minutes per day", rb["jobs"], rb["minutes"] / b["days"],
         ra["jobs"], ra["minutes"] / a["days"], "min")
    for ev in EVENTS:
        for h in sorted(set(b["queue_by_host"][ev]) | set(a["queue_by_host"][ev])):
            hb = b["queue_by_host"][ev].get(h) or {"n": 0, "p50": None}
            ha = a["queue_by_host"][ev].get(h) or {"n": 0, "p50": None}
            line(f"{ev} gate queue p50, {h}", hb["n"], hb["p50"], ha["n"], ha["p50"], "min")
    L.append(f"\nHosted `macos` placeholders that were not receipt reuse (skip-safe: no native "
             f"build input): before {r_b['skip_safe']}, after {r_a['skip_safe']}; unclassified "
             f"(run `ingest` to classify) {r_b['unclassified']} / {r_a['unclassified']}; "
             f"annotation unknown {r_b['unknown']} / {r_a['unknown']}.")
    reasons = sorted(set(e_b["by_reason"]) | set(e_a["by_reason"]))
    if reasons:
        L.append("\nEjections by reason (before → after): " + ", ".join(
            f"{k} {e_b['by_reason'].get(k, 0)} → {e_a['by_reason'].get(k, 0)}" for k in reasons) + ".")
    L.append("\nn is merged PRs for latency rows, gate-minute rows for per-PR cost, cancelled "
             "self-hosted gate jobs for cancelled minutes, merge groups for reuse, completed "
             "merge_group runs for the failure rate (cancelled runs count toward n, not failures), "
             "and ejections for the ejection row. Gate-minutes cover PRs opened inside the "
             "ingested window only; a side with n < 5 is anecdote.")
    return L


def drift_findings(sy: Shipyard, since_spec: str) -> list[dict]:
    """`shipyard metrics watch` findings that say something (not 'keep collecting')."""
    out = []
    for project in (PROJECT_JOBS, PROJECT_STEPS, PROJECT_QUEUE):
        for f in sy.watch(project, since_spec):
            if f.get("signal") == "insufficient_samples":
                continue
            lane = str(f.get("lane", ""))
            if project == PROJECT_JOBS and not lane.startswith("macos-gate"):
                continue
            out.append({"project": project, **f})
    return out


# --------------------------------------------------------------------------
# report: fleet (host_vitals.json, published by the host-vitals sensor)
# --------------------------------------------------------------------------

VITALS_PATH = ".local/state/pulp/host_vitals.json"


def _ssh(host: str, *cmd: str, stdin: str | None = None, timeout: int = 30) -> subprocess.CompletedProcess:
    return subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", host, *cmd],
                          input=stdin, capture_output=True, text=True, timeout=timeout)


def read_host_state(alias: str, target: str, vitals_script: Path) -> dict:
    """One host's published vitals, with the build snapshot.

    Reads the sensor's state file (one `cat` per host). A host whose installed
    sensor predates the build snapshot gets a live read-only probe by the same
    in-repo script instead, and is labelled so; it is never reported as zeros.
    """
    state: dict[str, Any] = {"host": alias, "source": None}
    try:
        if target == "local":
            text = (Path.home() / VITALS_PATH).read_text() if (Path.home() / VITALS_PATH).exists() else ""
        else:
            proc = _ssh(target, "cat", VITALS_PATH)
            if proc.returncode == 255:
                return {**state, "status": "UNREACHABLE", "error": proc.stderr.strip()[:200]}
            text = proc.stdout if proc.returncode == 0 else ""
    except (subprocess.TimeoutExpired, OSError) as exc:
        return {**state, "status": "UNREACHABLE", "error": str(exc)[:200]}
    vitals = {}
    if text.strip():
        try:
            vitals = json.loads(text)
        except ValueError:
            vitals = {}
    state["vitals"] = {k: vitals.get(k) for k in ("level", "reason", "load1", "ncpu",
                                                   "pressure_level", "sampled_at")} if vitals else None
    build = vitals.get("build") if vitals else None
    if build:
        state.update(status="ok", source="sensor", build=build)
        return state
    try:
        if target == "local":
            proc = subprocess.run(["bash", str(vitals_script), "--build-json"],
                                  capture_output=True, text=True, timeout=60)
        else:
            proc = _ssh(target, "bash", "-s", "--", "--build-json",
                        stdin=vitals_script.read_text(), timeout=60)
        if proc.returncode == 255 and target != "local":
            return {**state, "status": "UNREACHABLE", "error": proc.stderr.strip()[:200]}
        state.update(status="ok", source="live probe (sensor lacks build snapshot)",
                     build=json.loads(proc.stdout))
    except (subprocess.TimeoutExpired, OSError, ValueError) as exc:
        state.update(status="NOT MEASURED", error=str(exc)[:200])
    return state


def fleet_state(hosts: Iterable[str], workers: int = 4) -> list[dict]:
    script = HERE / "host_vitals.sh"
    specs = []
    for h in hosts:
        alias, _, target = h.partition("=")
        specs.append((alias, target or alias))
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(lambda s: read_host_state(s[0], s[1], script), specs))


# --------------------------------------------------------------------------
# report: local build
# --------------------------------------------------------------------------

def local_build(build_dir: Path | None, runs: str, blast: bool, files: Iterable[str],
                log_is_clean_build: bool = False) -> dict:
    btr = _load_sibling("build_time_report")
    if build_dir is None:
        return {"status": "NOT MEASURED", "reason": "no --build-dir given"}
    if not (build_dir / "build.ninja").is_file():
        return {"status": "NOT MEASURED",
                "reason": f"{build_dir} is not a Ninja build dir (no build.ninja); the local "
                          "section needs one and this tool never builds one"}
    out: dict[str, Any] = {"status": "ok", "build_dir": str(build_dir),
                           "log_is_clean_build": log_is_clean_build}
    try:
        out["ninja_log"] = btr.ninja_log_report(build_dir, runs)
    except (FileNotFoundError, ValueError) as exc:
        out["ninja_log"] = {"status": "NOT MEASURED", "reason": str(exc)}
    if blast:
        try:
            out["blast_radius"] = btr.blast_radius(build_dir, list(files),
                                                   btr._guess_src_root(build_dir))
        except btr.InstrumentBroken as exc:
            out["blast_radius"] = {"status": "INSTRUMENT BROKEN", "reason": str(exc)}
    return out


# --------------------------------------------------------------------------
# sections (bench_diff format) and markdown
# --------------------------------------------------------------------------

def _r(v: float | None, nd: int = 1) -> float | None:
    return None if v is None else round(v, nd)


def to_sections(pipe: dict | None, fleet: list[dict] | None, local: dict | None) -> list[dict]:
    secs: list[dict] = []
    if pipe:
        g = pipe["gate"]
        vals = {}
        for ev in EVENTS:
            vals[f"{ev} p50"] = _r(g[ev]["p50"])
            vals[f"{ev} p90"] = _r(g[ev]["p90"])
        for h in ("m3", "m5", "m1"):
            vals[f"host {h} p50"] = _r((pipe["gate_by_host"].get(h) or {}).get("p50"))
        secs.append({"title": "Required macos gate job (min)", "unit": "min",
                     "lower_is_better": True, "values": vals})
        sv = {}
        for label in ("queue", *STEP_NAMES):
            for ev in EVENTS:
                sv[f"{label} ({ev})"] = _r((pipe["steps"].get(f"macos-gate/{ev}/{label}") or {}).get("p50"))
        secs.append({"title": "Gate stage p50 (min)", "unit": "min", "lower_is_better": True,
                     "values": sv})
        lat = pipe["latency"]
        secs.append({"title": "Merge queue latency (min)", "unit": "min", "lower_is_better": True,
                     "values": {"enqueue→merged p50": _r(lat["pr/enqueue-to-merged"]["p50"]),
                                "enqueue→merged p90": _r(lat["pr/enqueue-to-merged"]["p90"]),
                                "last enqueue→merged p50": _r(lat["pr/last-enqueue-to-merged"]["p50"])}})
        secs.append({"title": "PR opened→merged (h)", "unit": "h", "lower_is_better": True,
                     "values": {"p50": _r(lat["pr/open-to-merged"]["p50"]),
                                "p90": _r(lat["pr/open-to-merged"]["p90"])}})
        mg = pipe["merge_group_failure"]
        secs.append({"title": "merge_group failure rate (%)", "unit": "%", "lower_is_better": True,
                     "values": {"window": _r(None if mg["window_rate"] is None else mg["window_rate"] * 100),
                                **{d: _r(v["rate"] * 100) for d, v in mg["per_day"].items()}}})
        secs.append({"title": "Receipt reuse (count)", "unit": "count", "lower_is_better": None,
                     "notes": ["No good direction until a receipt proves its ctest ran: more reuse "
                               "is only a win once reuse cannot skip tests."],
                     "values": {"merge groups reusing a macos receipt": pipe["receipt_reused"],
                                "merge_group runs": mg["runs"]}})
    if local and local.get("status") == "ok":
        nl = local.get("ninja_log") or {}
        # A .ninja_log holds whatever was built last; only a log the caller
        # declares to be a clean build is comparable with a clean-build baseline.
        if "selected" in nl and local.get("log_is_clean_build"):
            cats = nl["selected"]["categories"]
            secs.append({"title": "Local build edge-seconds (s)", "unit": "s", "lower_is_better": True,
                         "values": {"total": nl["selected"]["edge_seconds"],
                                    **{c: v["edge_seconds"] for c, v in cats.items()}}})
            secs.append({"title": "Local build edges (count)", "unit": "count", "lower_is_better": True,
                         "values": {c: v["count"] for c, v in cats.items()}})
        br = local.get("blast_radius") or {}
        # Counts from a tree with pending work are lower bounds; diffing them
        # against a full-tree baseline would report a fake improvement.
        if "files" in br and br["control"]["total"] <= BLAST_CLEAN_CONTROL_MAX:
            vals = {}
            for row in br["files"]:
                if row.get("status") in ("ok", "no-dependents"):
                    for k, label in (("compiles", "compiles"), ("exe_links", "exe relinks"),
                                     ("bundle_links", "bundle relinks")):
                        vals[f"{row['file']} {label}"] = row[k]
            secs.append({"title": "Blast radius (edges)", "unit": "count", "lower_is_better": True,
                         "values": vals})
    for h in fleet or []:
        b = h.get("build")
        if h.get("status") != "ok" or not b:
            continue
        cc = b.get("ccache_host") or {}
        vms = b.get("gate_vms") or {}
        mem = b.get("mem_bytes")
        free = b.get("memory_free_pct")
        fill = (cc["size_gb"] / cc["max_gb"] * 100) if cc.get("size_gb") is not None and cc.get("max_gb") else None
        load = b.get("load") or [None]
        secs.append({"title": f"Fleet {h['host']}", "unit": "", "lower_is_better": True,
                     "higher_is_better_keys": ["ccache hit %", "cores", "memory GB",
                                              "memory free %"],
                     "values": {"ccache hit %": cc.get("hit_pct"), "ccache fill %": _r(fill),
                                "ccache max GB": cc.get("max_gb"),
                                "ccache uncacheable %": cc.get("uncacheable_pct"),
                                "ccache cleanups": cc.get("cleanups"), "load1": load[0],
                                "cores": b.get("ncpu"), "gate VMs": vms.get("count"),
                                "gate VM RSS GB": _r(vms["rss_mb"] / 1024) if vms.get("rss_mb") is not None else None,
                                # memory_pressure's free percentage; not comparable with
                                # a vm_stat/Activity Monitor "used" reading.
                                "memory free %": free,
                                "memory GB": _r(mem / 2**30) if mem else None}})
    return secs


def _fmt(v: Any, unit: str = "") -> str:
    if v is None:
        return "NOT MEASURED"
    if isinstance(v, float):
        return f"{v:,.1f}{(' ' + unit) if unit and unit != 'count' else ''}"
    return f"{v:,}{(' ' + unit) if unit and unit not in ('count',) else ''}"


def tartci_cell(tc: dict) -> str:
    """tartci's version on a host: installed / executing / checkout.

    `installed` is what `tartci fleet-macos self-update` calls installed (the
    sealed launcher bundle's commit), so a sealed host with no checkout still
    names its version. A sensor older than that field reads `not published`,
    which is a stale install, not an unknown version.
    """
    if "installed_generation" in tc:
        installed = tc.get("installed_generation") or "n/a"
    else:
        installed = "not published"
    return (f"{installed} / {tc.get('executing_generation') or 'n/a'} / "
            f"{tc.get('checkout_head') or 'n/a'}")


def render_markdown(rep: dict) -> str:
    L: list[str] = [f"# Build-speed scorecard — {rep['generated_at']}", ""]
    btr = _load_sibling("build_time_report")

    L += ["## Local build", ""]
    loc = rep.get("local") or {}
    if loc.get("status") != "ok":
        L.append(f"**{loc.get('status', 'NOT MEASURED')}**: {loc.get('reason', '')}")
    else:
        nl = loc.get("ninja_log") or {}
        L.append(btr.render_log_markdown(nl) if "selected" in nl
                 else f"`.ninja_log`: **{nl.get('status')}** — {nl.get('reason')}")
        br = loc.get("blast_radius")
        if br and "files" in br:
            L += ["", btr.render_blast_markdown(br)]
        elif br:
            L += ["", f"Blast radius: **{br['status']}** — {br['reason']}"]
    L.append("")

    L += ["## PR pipeline (from Shipyard metrics)", ""]
    pipe = rep.get("pipeline")
    if not pipe or pipe.get("status"):
        L.append(f"**{(pipe or {}).get('status', 'NOT MEASURED')}**: {(pipe or {}).get('reason', '')}")
    else:
        L.append(f"Window: since {pipe['since']}.")
        L += ["", "| Required `macos` gate job | n | p50 | p90 | failures |", "|---|---:|---:|---:|---:|"]
        for ev in EVENTS:
            g = pipe["gate"][ev]
            L.append(f"| {ev} | {g['n']} | {_fmt(_r(g['p50']), 'min')} | {_fmt(_r(g['p90']), 'min')} | "
                     f"{pipe['gate_failures'][ev]} |")
        L += ["", "| Host | n | p50 | p90 |", "|---|---:|---:|---:|"]
        for h, v in pipe["gate_by_host"].items():
            L.append(f"| {h} | {v['n']} | {_fmt(_r(v['p50']), 'min')} | {_fmt(_r(v['p90']), 'min')} |")
        L += ["", "| Stage | pull_request p50 | merge_group p50 |", "|---|---:|---:|"]
        for label in ("queue", *STEP_NAMES):
            cells = [_fmt(_r((pipe["steps"].get(f"macos-gate/{ev}/{label}") or {}).get("p50")), "min")
                     for ev in EVENTS]
            L.append(f"| {label} | {cells[0]} | {cells[1]} |")
        if not any(f"/iOS compile gate" in t for t in pipe["steps"]):
            L.append("\nThe iOS compile gate runs inside the Build step, so it has no step timing "
                     "of its own; its cost is part of Build.")
        lat = pipe["latency"]
        L += ["", "| Merge queue | n | p50 | p90 |", "|---|---:|---:|---:|"]
        for tgt, label in (("pr/enqueue-to-merged", "first enqueue → merged"),
                           ("pr/last-enqueue-to-merged", "last enqueue → merged"),
                           ("pr/open-to-merged", "PR opened → merged")):
            v = lat[tgt]
            L.append(f"| {label} | {v['n']} | {_fmt(_r(v['p50']), v['unit'])} | {_fmt(_r(v['p90']), v['unit'])} |")
        mg = pipe["merge_group_failure"]
        L += ["", "| merge_group runs (by day) | failed / completed | rate |", "|---|---:|---:|"]
        for d, v in mg["per_day"].items():
            L.append(f"| {d} | {v['failed']} / {v['runs']} | {v['rate'] * 100:.0f}% |")
        wr = mg["window_rate"]
        L.append(f"| window | | {'NOT MEASURED' if wr is None else f'{wr * 100:.0f}%'} |")
        L.append(f"\nMerge groups whose `macos` was a hosted placeholder (a reused receipt or a "
                 f"skip-safe group; `--split` separates them): {pipe['receipt_reused']}.")
        drift = rep.get("drift") or []
        L += ["", "### Drift (`shipyard metrics watch`)", ""]
        if drift:
            for f in drift[:DRIFT_SHOWN]:
                L.append(f"- [{f.get('severity')}] {f['project']} `{f.get('lane')}`: {f.get('message')}")
            if len(drift) > DRIFT_SHOWN:
                L.append(f"- … {len(drift) - DRIFT_SHOWN} more (`--json` lists all)")
        else:
            L.append("No drift findings beyond 'insufficient samples'.")
        L.append("\nDrift compares the two halves of the window, so a window that spans a change "
                 "mixes both regimes; `--split <ISO time>` compares either side of it.")
        if rep.get("split"):
            L += [""] + render_split_markdown(rep["split"])
        if rep.get("merge_split"):
            L += render_merge_split_markdown(rep["merge_split"])
    L.append("")

    L += ["## Fleet", ""]
    fleet = rep.get("fleet")
    if fleet is None:
        L.append("**NOT MEASURED**: fleet probe skipped.")
    else:
        L += ["| Host | Source | Load (1/5/15) / cores | Mem free | Gate VMs (RSS, CPU) | ccache host: hit / size / max / cleanups | Gate ccache hit | tartci (installed / exec / checkout) | Leases used | Wheelhouse |",
              "|---|---|---|---|---|---|---|---|---|---|"]
        for h in fleet:
            if h.get("status") != "ok":
                L.append(f"| {h['host']} | **{h.get('status')}** {h.get('error', '')} | | | | | | | | |")
                continue
            b = h["build"]
            load = b.get("load") or []
            cc = b.get("ccache_host")
            ccs = (f"{cc.get('hit_pct')}% / {cc.get('size_gb')} / {cc.get('max_gb')} GB / {cc.get('cleanups')}"
                   if cc else "NOT MEASURED (ccache absent, or its stats timed out)")
            gcc = b.get("ccache_gate")
            vms = b.get("gate_vms") or {}
            tc = b.get("tartci") or {}
            le = tc.get("leases")
            leases = (f"{le.get('used_cores')}/{le.get('total_cores')} cores, "
                      f"{le.get('used_mem_mb')}/{le.get('total_mem_mb')} MB" if le else "NOT MEASURED")
            wh = b.get("wheelhouse_wheels")
            L.append(f"| {h['host']} | {h['source']} | {'/'.join(str(x) for x in load) or 'NOT MEASURED'} / "
                     f"{b.get('ncpu')} | {_fmt(b.get('memory_free_pct'), '%')} | "
                     f"{vms.get('count')} ({_fmt(vms.get('rss_mb'), 'MB')}, {vms.get('cpu_pct')}%) | {ccs} | "
                     f"{(str(gcc.get('hit_pct')) + '%') if gcc else 'n/a'} | "
                     f"{tartci_cell(tc)} | "
                     f"{leases} | {'absent' if wh is None else f'{wh} wheels'} |")
    return "\n".join(L)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def cmd_ingest(args) -> int:
    since = parse_since(args.since)
    gh = GitHub(args.repo, args.gh)
    sy = Shipyard(args.shipyard, args.shipyard_mode)
    try:
        s = ingest(gh, sy, since, dry_run=args.dry_run, workers=args.workers,
                   log=lambda m: print(m, file=sys.stderr))
    except RuntimeError as exc:
        print(f"ingest failed: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(s))
    return 1 if s["fetch_errors"] else 0


def cmd_report(args) -> int:
    since = parse_since(args.since)
    rep: dict[str, Any] = {"generated_at": iso(dt.datetime.now(dt.timezone.utc)),
                           "host": os.uname().nodename.split(".")[0]}
    rc = 0
    if args.no_local:
        rep["local"] = {"status": "NOT MEASURED", "reason": "--no-local"}
    else:
        rep["local"] = local_build(args.build_dir, args.runs, not args.no_blast_radius,
                                   args.files or STANDARD_FILES, args.log_is_clean_build)
        if (rep["local"].get("blast_radius") or {}).get("status") == "INSTRUMENT BROKEN":
            rc = 3
    if args.no_pipeline:
        rep["pipeline"] = {"status": "NOT MEASURED", "reason": "--no-pipeline"}
    else:
        sy = Shipyard(args.shipyard, args.shipyard_mode)
        if not sy.available:
            rep["pipeline"] = {"status": "NOT MEASURED", "reason": "shipyard is not on PATH"}
        else:
            rows = {p: sy.list_rows(p) for p in (PROJECT_JOBS, PROJECT_STEPS, PROJECT_QUEUE)}
            stats = pipeline_stats(rows[PROJECT_JOBS], rows[PROJECT_STEPS], rows[PROJECT_QUEUE], since)
            if not any(stats["gate"][ev]["n"] for ev in EVENTS):
                rep["pipeline"] = {"status": "NOT MEASURED",
                                   "reason": "no macos-gate rows in Shipyard for the window; run "
                                             "`build_speed_scorecard.py ingest` first"}
            else:
                rep["pipeline"] = stats
                rep["drift"] = drift_findings(sy, args.since)
                if args.split:
                    rep["split"] = split_stats(rows[PROJECT_JOBS], rows[PROJECT_STEPS], since,
                                               args.split, args.after_from, args.until)
                    rep["merge_split"] = merge_split_stats(
                        rows[PROJECT_JOBS], rows[PROJECT_STEPS], rows[PROJECT_QUEUE], since,
                        args.split, args.after_from, args.until)
    rep["fleet"] = None if args.no_fleet else fleet_state(args.hosts or DEFAULT_HOSTS)
    pipe = rep["pipeline"] if not rep["pipeline"].get("status") else None
    current = {"schema": "pulp-bench-sections/1", "title": "Build speed",
               "host": rep["host"], "date": rep["generated_at"], "pulp_commit": _head(),
               "sections": to_sections(pipe, rep["fleet"], rep["local"])}
    if args.json:
        print(json.dumps({"report": rep, "bench": current}, indent=2))
    else:
        print(render_markdown(rep))
    if args.baseline:
        bd = _load_sibling("bench_diff")
        base = json.loads(Path(args.baseline).read_text())
        print("\n" + bd.render_sections(base, current, args.threshold))
    if args.write_current:
        Path(args.write_current).write_text(json.dumps(current, indent=2) + "\n")
    return rc


def _parse_split(text: str) -> dt.datetime:
    t = parse_ts(text)
    if t is None:
        raise argparse.ArgumentTypeError(f"not an ISO time: {text!r}")
    return t if t.tzinfo else t.replace(tzinfo=dt.timezone.utc)


def _head() -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip() or "?"
    except OSError:
        return "?"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--shipyard", help="shipyard binary (default: on PATH)")
    ap.add_argument("--shipyard-mode", choices=("isolated", "shipyard"),
                    help="pass --mode to shipyard (isolated = sandboxed store)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_in = sub.add_parser("ingest", help="backfill GitHub gate/queue timings into Shipyard")
    p_in.add_argument("--since", default="14d")
    p_in.add_argument("--repo", default=DEFAULT_REPO)
    p_in.add_argument("--gh", default=os.environ.get("PULP_GH_BIN", "ghapp"))
    p_in.add_argument("--workers", type=int, default=8)
    p_in.add_argument("--dry-run", action="store_true")
    p_in.set_defaults(func=cmd_ingest)

    p_rep = sub.add_parser("report", help="render the scorecard")
    p_rep.add_argument("--since", default="7d")
    p_rep.add_argument("--build-dir", type=Path)
    p_rep.add_argument("--runs", default="all", help=".ninja_log runs to count (all | last | 0-1)")
    p_rep.add_argument("--log-is-clean-build", action="store_true",
                       help="the selected .ninja_log runs are a clean build; only then are "
                            "they compared with the clean-build baseline")
    p_rep.add_argument("--no-blast-radius", action="store_true")
    p_rep.add_argument("--files", nargs="*", help="blast-radius files (default: the standard set)")
    p_rep.add_argument("--hosts", nargs="*", help="alias[=ssh-target|local] (default: m3=local m5 m1)")
    p_rep.add_argument("--no-local", action="store_true")
    p_rep.add_argument("--no-pipeline", action="store_true")
    p_rep.add_argument("--no-fleet", action="store_true")
    p_rep.add_argument("--baseline", type=Path, help="bench-sections JSON to diff against")
    p_rep.add_argument("--threshold", type=float, default=0.05,
                       help="relative change flagged as a regression in the baseline diff")
    p_rep.add_argument("--write-current", type=Path, help="also write the bench-sections JSON here")
    p_rep.add_argument("--split", type=_parse_split, metavar="ISO-TIME",
                       help="also compare gate job and step timings before vs after this "
                            "instant (e.g. 2026-09-24T13:12Z), within the --since window; "
                            "also time-to-merge, gate-minutes per merged PR, cancelled "
                            "gate-minutes, receipt reuse and merge_group failure/ejection rates")
    p_rep.add_argument("--after-from", type=_parse_split, metavar="ISO-TIME",
                       help="start the after side here instead of at --split (skip a rollout)")
    p_rep.add_argument("--until", type=_parse_split, metavar="ISO-TIME",
                       help="end the after side here instead of now (a fixed 48 h window)")
    p_rep.add_argument("--json", action="store_true")
    p_rep.set_defaults(func=cmd_report)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
