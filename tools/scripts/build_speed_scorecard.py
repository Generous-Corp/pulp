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
                     target macos-gate/merge_group/receipt-reused  reuse placeholder
  pulp-gate-steps    target macos-gate/<event>/<Step|queue>        per step
  pulp-merge-queue   target pr/enqueue-to-merged, pr/open-to-merged, merge-group-run

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

# Step display name → the gate step names it covers. The iOS compile gate runs
# inside "Build" today; a step whose name mentions it is picked up if it is
# ever split out.
STEP_NAMES: dict[str, tuple[str, ...]] = {
    "Configure": ("Configure",),
    "Build": ("Build",),
    "iOS compile gate": ("iOS compile gate",),
    "Test": ("Test (non-Windows)",),
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
            # A hosted placeholder: in a merge group it stands in for a reused
            # PR receipt; on a PR it means no native build was required.
            if event == "merge_group" and dur is not None:
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

    def runs_for_day(self, day: dt.date, event: str) -> list[dict]:
        runs: list[dict] = []
        page = 1
        while True:
            data = self.api(f"repos/{self.repo}/actions/workflows/{WORKFLOW_FILE}/runs"
                            f"?per_page=100&page={page}&event={event}&created={day.isoformat()}")
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
                 "[ADDED_TO_MERGE_QUEUE_EVENT]){nodes{__typename ... on AddedToMergeQueueEvent"
                 "{createdAt}}}}}}}")
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
    if sy.available:
        for project in (PROJECT_JOBS, PROJECT_STEPS, PROJECT_QUEUE):
            existing |= {r.get("external_id") for r in sy.list_rows(project) if r.get("external_id")}

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

    errors = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for fut in concurrent.futures.as_completed([pool.submit(fetch, r) for r in to_fetch]):
            try:
                records += fut.result()
            except RuntimeError as exc:
                errors += 1
                log(f"ingest: jobs fetch failed: {exc}")
    for day in days:
        for pr in gh.merged_prs_for_day(day):
            records += pr_latency_records(pr)

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
        L.append(f"\nMerge groups that reused a PR receipt instead of running the macOS gate: "
                 f"{pipe['receipt_reused']}.")
        drift = rep.get("drift") or []
        L += ["", "### Drift (`shipyard metrics watch`)", ""]
        if drift:
            for f in drift[:DRIFT_SHOWN]:
                L.append(f"- [{f.get('severity')}] {f['project']} `{f.get('lane')}`: {f.get('message')}")
            if len(drift) > DRIFT_SHOWN:
                L.append(f"- … {len(drift) - DRIFT_SHOWN} more (`--json` lists all)")
        else:
            L.append("No drift findings beyond 'insufficient samples'.")
    L.append("")

    L += ["## Fleet", ""]
    fleet = rep.get("fleet")
    if fleet is None:
        L.append("**NOT MEASURED**: fleet probe skipped.")
    else:
        L += ["| Host | Source | Load (1/5/15) / cores | Mem free | Gate VMs (RSS, CPU) | ccache host: hit / size / max / cleanups | Gate ccache hit | tartci (exec / checkout) | Leases used | Wheelhouse |",
              "|---|---|---|---|---|---|---|---|---|---|"]
        for h in fleet:
            if h.get("status") != "ok":
                L.append(f"| {h['host']} | **{h.get('status')}** {h.get('error', '')} | | | | | | | | |")
                continue
            b = h["build"]
            load = b.get("load") or []
            cc = b.get("ccache_host")
            ccs = (f"{cc.get('hit_pct')}% / {cc.get('size_gb')} / {cc.get('max_gb')} GB / {cc.get('cleanups')}"
                   if cc else "NOT MEASURED (no ccache)")
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
                     f"{tc.get('executing_generation') or 'n/a'} / {tc.get('checkout_head') or 'n/a'} | "
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
    p_rep.add_argument("--json", action="store_true")
    p_rep.set_defaults(func=cmd_report)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
