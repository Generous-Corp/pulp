#!/usr/bin/env python3
"""Classify whether one job of a workflow run was ever picked up by a runner.

A job that waits for a runner until it is cancelled finishes `cancelled` with
`runner_name` empty and no steps. A job that ran and then failed or timed out
finishes with a runner named. Watchdogs that read only `needs.<job>.result`
see `cancelled` either way, and a watchdog that files every `cancelled` as a
runner-infra flake hides a lane that has no runner at all. The native Intel
nightly did exactly that for sixteen nights after its only runner's
registration was deleted.

Prints `key=value` lines for `$GITHUB_OUTPUT`:

    verdict=served|unserved|skipped|pending
    conclusion=<job conclusion>
    labels=<comma-separated requested labels>
    waited_minutes=<created_at to completed_at, for an unserved job>

Exit status: 0 when the job was found and classified, 2 when it could not be
read. A read failure is never reported as `served`.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import subprocess
import sys
from typing import Any


def parse_ts(raw: str) -> dt.datetime | None:
    if not raw:
        return None
    return dt.datetime.fromisoformat(raw.replace("Z", "+00:00"))


def classify(job: dict[str, Any]) -> dict[str, str]:
    conclusion = str(job.get("conclusion") or "")
    labels = ",".join(str(label) for label in job.get("labels") or [])
    result = {"verdict": "", "conclusion": conclusion, "labels": labels, "waited_minutes": ""}
    if job.get("status") != "completed":
        result["verdict"] = "pending"
    elif conclusion == "skipped":
        result["verdict"] = "skipped"
    elif str(job.get("runner_name") or "").strip():
        result["verdict"] = "served"
    else:
        result["verdict"] = "unserved"
        created = parse_ts(str(job.get("created_at") or ""))
        completed = parse_ts(str(job.get("completed_at") or ""))
        if created and completed:
            result["waited_minutes"] = f"{(completed - created).total_seconds() / 60:.0f}"
    return result


def find_job(jobs: list[dict[str, Any]], name: str) -> dict[str, Any] | None:
    # A re-run lists every attempt; the latest attempt is the one that counts.
    matches = [job for job in jobs if job.get("name") == name]
    if not matches:
        return None
    return max(matches, key=lambda job: int(job.get("run_attempt") or 0))


def fetch_jobs(gh: str, repo: str, run_id: str) -> list[dict[str, Any]]:
    out = subprocess.run(
        [gh, "api", "--paginate", f"repos/{repo}/actions/runs/{run_id}/jobs?filter=all&per_page=100",
         "--jq", ".jobs[]"],
        check=True, capture_output=True, text=True, encoding="utf-8", timeout=120,
    ).stdout
    return [json.loads(line) for line in out.splitlines() if line.strip()]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--job", required=True, help="exact job name as the jobs API reports it")
    ap.add_argument("--repo", default="")
    ap.add_argument("--run-id", default="")
    ap.add_argument("--jobs-json", default="", help="read a recorded jobs listing instead of the API")
    ap.add_argument("--gh", default="gh")
    args = ap.parse_args(argv)

    try:
        if args.jobs_json:
            with open(args.jobs_json, encoding="utf-8") as fh:
                data = json.load(fh)
            jobs = data.get("jobs", data) if isinstance(data, dict) else data
        else:
            if not args.repo or not args.run_id:
                ap.error("--repo and --run-id are required without --jobs-json")
            jobs = fetch_jobs(args.gh, args.repo, args.run_id)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"unserved_job_check: could not read jobs: {exc}", file=sys.stderr)
        return 2

    job = find_job(jobs, args.job)
    if job is None:
        print(f"unserved_job_check: no job named {args.job!r} in the run", file=sys.stderr)
        return 2
    for key, value in classify(job).items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
