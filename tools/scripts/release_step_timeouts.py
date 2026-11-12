#!/usr/bin/env python3
"""Release workflow timeouts: derive them from history, and lint that they exist.

A release leg that hangs does not fail. It sits on its runner until the job's
`timeout-minutes` (or GitHub's 6-hour default) expires, and every alarm
downstream waits with it. v1.119.0's `CLI darwin-x64` leg sat in
`Prepare SDK build dir (macOS)` for over 105 minutes, a step whose last three
successful runs took 3, 6 and 7 minutes, while the release waited for the
reconciler's multi-hour drought alarm.

So every long step of the release workflows carries its own `timeout-minutes`,
and every job carries one. The values are not guesses:

    timeout = max(floor, ceil(MULTIPLIER x p95))

over the step's (or job's) durations in the last N successful runs, taken from
the GitHub API. For a matrix job the p95 is the slowest leg's, since one value
bounds every leg. `tools/ci/release_step_timeouts.json` records N, each p95 and
each derived value, and is what `check` enforces.

    derive   fetch history and print the table (JSON) — run it to refresh
    check    fail if a policed workflow has a job without `timeout-minutes`,
             or a step listed in the policy without the recorded value

`check` needs no network and runs in the `release-step-timeouts` ctest and
workflow-lint. A step with a recorded p95 below `long_step_minutes` is not
"long": the job timeout bounds it.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import subprocess
import sys
from datetime import datetime
from typing import Iterable

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
POLICY_PATH = os.path.join(REPO_ROOT, "tools", "ci", "release_step_timeouts.json")


# ── statistics ───────────────────────────────────────────────────────────────

def p95(values: Iterable[float]) -> float:
    """Nearest-rank 95th percentile. Small samples resolve to the maximum."""
    ordered = sorted(values)
    if not ordered:
        raise ValueError("p95 of an empty sample")
    rank = math.ceil(0.95 * len(ordered))
    return ordered[rank - 1]


def derive_timeout(p95_minutes: float, *, multiplier: float, floor: int) -> int:
    return max(floor, math.ceil(multiplier * p95_minutes))


def _minutes(start: str | None, end: str | None) -> float | None:
    if not start or not end:
        return None
    a = datetime.fromisoformat(start.replace("Z", "+00:00"))
    b = datetime.fromisoformat(end.replace("Z", "+00:00"))
    return max(0.0, (b - a).total_seconds() / 60.0)


# ── workflow parsing ─────────────────────────────────────────────────────────

def load_workflow(path: str) -> dict:
    import yaml  # PyYAML; imported lazily so `p95` callers need no YAML

    with open(path, encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def job_name_pattern(job_key: str, job: dict) -> re.Pattern[str]:
    """A regex matching the API display name of a job (matrix expressions wildcarded)."""
    name = str(job.get("name", job_key))
    parts = re.split(r"\$\{\{.*?\}\}", name)
    return re.compile("^" + ".+".join(re.escape(p) for p in parts) + "$")


def has_timeout(node: dict) -> bool:
    value = node.get("timeout-minutes")
    return value is not None and value != ""


def step_timeout_value(step: dict):
    value = step.get("timeout-minutes")
    return int(value) if isinstance(value, int) or (isinstance(value, str) and value.isdigit()) else value


# ── check ────────────────────────────────────────────────────────────────────

def check(policy: dict, root: str = REPO_ROOT) -> list[str]:
    """Return every violation of the release timeout policy."""
    problems: list[str] = []
    long_floor = float(policy["long_step_minutes"])
    for wf_rel, wf_policy in policy["workflows"].items():
        path = os.path.join(root, wf_rel)
        try:
            workflow = load_workflow(path)
        except OSError as exc:
            problems.append(f"{wf_rel}: cannot read ({exc})")
            continue
        jobs = workflow.get("jobs") or {}
        for job_key, job in jobs.items():
            if "uses" in job:
                continue  # a reusable-workflow call; its jobs carry their own
            if not has_timeout(job):
                problems.append(
                    f"{wf_rel}: job `{job_key}` has no timeout-minutes; a hung leg "
                    f"would hold the release for GitHub's 6-hour default"
                )
                continue
            recorded_job = (wf_policy.get("jobs") or {}).get(job_key)
            if recorded_job and job.get("timeout-minutes") != recorded_job["timeout_minutes"]:
                problems.append(
                    f"{wf_rel}: job `{job_key}` timeout-minutes is "
                    f"{job.get('timeout-minutes')!r}, policy records "
                    f"{recorded_job['timeout_minutes']} (re-derive with "
                    f"`release_step_timeouts.py derive` and update both)"
                )
        for job_key, steps in (wf_policy.get("steps") or {}).items():
            job = jobs.get(job_key)
            if job is None:
                problems.append(f"{wf_rel}: policy names job `{job_key}`, which no longer exists")
                continue
            by_name = {s.get("name"): s for s in job.get("steps", []) if s.get("name")}
            for step_name, rec in steps.items():
                if rec["p95_minutes"] < long_floor:
                    continue
                step = by_name.get(step_name)
                if step is None:
                    problems.append(
                        f"{wf_rel}: long step `{job_key} › {step_name}` is in the policy "
                        f"but not in the workflow (renamed? re-derive the policy)"
                    )
                    continue
                if not has_timeout(step):
                    problems.append(
                        f"{wf_rel}: long step `{job_key} › {step_name}` (p95 "
                        f"{rec['p95_minutes']} min) has no timeout-minutes; expected "
                        f"{rec['timeout_minutes']}"
                    )
                elif step_timeout_value(step) != rec["timeout_minutes"]:
                    problems.append(
                        f"{wf_rel}: step `{job_key} › {step_name}` timeout-minutes is "
                        f"{step.get('timeout-minutes')!r}, policy records {rec['timeout_minutes']}"
                    )
    return problems


# ── derive ───────────────────────────────────────────────────────────────────

def _gh_json(args: list[str]) -> object:
    binary = os.environ.get("RELEASE_TIMEOUTS_GH", "ghread")
    out = subprocess.run([binary, *args], check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def collect_durations(
    jobs_per_run: Iterable[list[dict]],
    workflow: dict,
) -> tuple[dict[str, dict[str, list[float]]], dict[str, dict[str, dict[str, list[float]]]]]:
    """Map API jobs onto workflow job keys and collect job/step minutes per leg.

    Only `success` jobs and `success` steps count. A matrix job keeps one sample
    per leg (its API display name): a step's timeout applies to every leg, so it
    must fit the SLOWEST leg, and pooling the legs would let five fast platforms
    hide one slow one (darwin `Bootstrap dependencies` p95 is ~18 min while the
    pooled p95 is ~4). A non-matrix job is a single leg named by its key, since
    its display name may embed the tag.
    """
    wf_jobs = workflow.get("jobs") or {}
    patterns = {k: job_name_pattern(k, v) for k, v in wf_jobs.items()}
    job_minutes: dict[str, dict[str, list[float]]] = {}
    step_minutes: dict[str, dict[str, dict[str, list[float]]]] = {}
    for jobs in jobs_per_run:
        for job in jobs:
            if job.get("conclusion") != "success":
                continue
            name = job.get("name", "")
            key = next((k for k, p in patterns.items() if p.match(name)), None)
            if key is None:
                continue
            leg = name if (wf_jobs[key].get("strategy") or {}).get("matrix") else key
            m = _minutes(job.get("started_at"), job.get("completed_at"))
            if m is not None:
                job_minutes.setdefault(key, {}).setdefault(leg, []).append(m)
            for step in job.get("steps", []):
                if step.get("conclusion") != "success":
                    continue
                sm = _minutes(step.get("started_at"), step.get("completed_at"))
                if sm is not None:
                    step_minutes.setdefault(key, {}).setdefault(step["name"], {}).setdefault(leg, []).append(sm)
    return job_minutes, step_minutes


def _slowest_leg(legs: dict[str, list[float]]) -> dict:
    """The leg with the highest p95, which sets the timeout for all of them."""
    leg, values = max(legs.items(), key=lambda kv: p95(kv[1]))
    return {
        "n": len(values),
        "legs": len(legs),
        "slowest_leg": leg,
        "p95_minutes": round(p95(values), 1),
        "max_minutes": round(max(values), 1),
    }


def build_policy(
    *,
    workflow: dict,
    job_minutes: dict[str, dict[str, list[float]]],
    step_minutes: dict[str, dict[str, dict[str, list[float]]]],
    n_runs: int,
    run_ids: list[int],
    multiplier: float,
    step_floor: int,
    job_floor: int,
    long_step_minutes: float,
) -> dict:
    declared = {k: {s.get("name") for s in v.get("steps", [])} for k, v in (workflow.get("jobs") or {}).items()}
    jobs_out = {}
    for key, legs in sorted(job_minutes.items()):
        rec = _slowest_leg(legs)
        rec["timeout_minutes"] = derive_timeout(rec["p95_minutes"], multiplier=multiplier, floor=job_floor)
        jobs_out[key] = rec
    steps_out: dict[str, dict] = {}
    for key, steps in sorted(step_minutes.items()):
        for name, legs in sorted(steps.items()):
            rec = _slowest_leg(legs)
            if rec["p95_minutes"] < long_step_minutes or name not in declared.get(key, set()):
                continue
            rec["timeout_minutes"] = derive_timeout(rec["p95_minutes"], multiplier=multiplier, floor=step_floor)
            steps_out.setdefault(key, {})[name] = rec
    return {
        "n_runs": n_runs,
        "run_ids": run_ids,
        "jobs": jobs_out,
        "steps": steps_out,
    }


def cmd_derive(args: argparse.Namespace) -> int:
    rel = args.workflow
    workflow = load_workflow(os.path.join(REPO_ROOT, rel))
    file = os.path.basename(rel)
    runs = _gh_json([
        "api", f"repos/{args.repo}/actions/workflows/{file}/runs?status=success&per_page={args.runs}",
    ])["workflow_runs"][: args.runs]
    jobs_per_run = []
    for run in runs:
        data = _gh_json(["api", f"repos/{args.repo}/actions/runs/{run['id']}/jobs?per_page=100"])
        jobs_per_run.append(data["jobs"])
    job_minutes, step_minutes = collect_durations(jobs_per_run, workflow)
    out = build_policy(
        workflow=workflow, job_minutes=job_minutes,
        step_minutes=step_minutes, n_runs=len(runs), run_ids=[r["id"] for r in runs],
        multiplier=args.multiplier, step_floor=args.step_floor, job_floor=args.job_floor,
        long_step_minutes=args.long_step_minutes,
    )
    json.dump({rel: out}, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


def cmd_check(args: argparse.Namespace) -> int:
    with open(args.policy, encoding="utf-8") as handle:
        policy = json.load(handle)
    problems = check(policy, args.root)
    for p in problems:
        print(f"::error title=release timeout::{p}" if os.environ.get("GITHUB_ACTIONS") else f"FAIL {p}")
    if problems:
        print(f"release_step_timeouts: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    n = sum(len(s) for wf in policy["workflows"].values() for s in (wf.get("steps") or {}).values())
    print(f"release_step_timeouts: OK ({len(policy['workflows'])} workflows, {n} long steps policed)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("derive")
    d.add_argument("--repo", default="Generous-Corp/pulp")
    d.add_argument("--workflow", default=".github/workflows/release-cli.yml")
    d.add_argument("--runs", type=int, default=30)
    d.add_argument("--multiplier", type=float, default=4.0)
    d.add_argument("--step-floor", type=int, default=15)
    d.add_argument("--job-floor", type=int, default=30)
    d.add_argument("--long-step-minutes", type=float, default=2.0)
    d.set_defaults(func=cmd_derive)
    c = sub.add_parser("check")
    c.add_argument("--policy", default=POLICY_PATH)
    c.add_argument("--root", default=REPO_ROOT)
    c.set_defaults(func=cmd_check)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
