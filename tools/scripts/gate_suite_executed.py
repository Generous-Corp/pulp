#!/usr/bin/env python3
"""Report whether a run's `macos` gate actually executed the test suite.

A green `macos` check has two very different meanings and the checks list shows
the same colour for both. It can mean the full suite built and passed, or it
can mean a bootstrap job claimed the required context without running anything
-- because the change touched no native build input, or because an earlier
run's protected receipt was reused. A reused receipt produces a three-step job
(`Set up job`, the bootstrap step, `Complete job`) reporting success, and
reading that as proof the suite passed sends an investigation in the wrong
direction for as long as the mistake survives.

This asks the run itself. It classifies the `macos` job by the steps that
actually ran:

    executed            a Test step ran, so the suite genuinely reported
    built-but-untested  the build ran but no Test step did
    not-executed        neither ran -- a bootstrap or receipt-reuse green

Read-only. Exits 0 on a successful classification; with --require-executed it
exits 1 unless the suite genuinely ran, so a script can gate on it.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys

DEFAULT_REPO = "Generous-Corp/pulp"

# The workflow's suite steps are "Test (non-Windows)" and "Test (Windows ...)".
TEST_STEP_RE = re.compile(r"^\s*Test\s*\(", re.IGNORECASE)
BUILD_STEP_RE = re.compile(r"^\s*Build\s*$", re.IGNORECASE)

# A step with any other conclusion (skipped, cancelled, null) did not run.
RAN_CONCLUSIONS = frozenset({"success", "failure"})

EXECUTED = "executed"
BUILT_BUT_UNTESTED = "built-but-untested"
NOT_EXECUTED = "not-executed"


def _name(step: dict) -> str:
    return (step.get("name") or "").strip()


def _conclusion(step: dict) -> str:
    return (step.get("conclusion") or "").strip().lower()


def _ran(step: dict) -> bool:
    return _conclusion(step) in RAN_CONCLUSIONS


def classify_steps(steps: list[dict]) -> str:
    """Classify a job's step list. Pure; the network wrapper feeds it."""
    if any(TEST_STEP_RE.match(_name(s)) and _ran(s) for s in steps):
        return EXECUTED
    if any(BUILD_STEP_RE.match(_name(s)) and _ran(s) for s in steps):
        return BUILT_BUT_UNTESTED
    return NOT_EXECUTED


def describe(verdict: str) -> str:
    return {
        EXECUTED: "the macOS test suite ran and reported",
        BUILT_BUT_UNTESTED: (
            "the macOS build ran but NO test step did -- this green says "
            "nothing about the suite"
        ),
        NOT_EXECUTED: (
            "NO build and NO test step ran -- this green is a bootstrap or a "
            "reused receipt, not evidence the suite passes"
        ),
    }[verdict]


def gh(path: str, jq: str | None = None) -> str | None:
    cmd = ["ghapp", "api", path] + (["--jq", jq] if jq else [])
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    return proc.stdout.strip()


def macos_jobs(repo: str, run_id: str) -> list[dict]:
    """Every job in the run that owns the `macos` check name.

    A skipped alias job reports its raw `name:` expression rather than the
    evaluated name, so match those by their conclusion instead of the text.
    """
    raw = gh(f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100")
    if not raw:
        return []
    try:
        jobs = json.loads(raw).get("jobs", [])
    except json.JSONDecodeError:
        return []
    return [j for j in jobs if (j.get("name") or "").strip() == "macos"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", required=True, help="Actions run id")
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument(
        "--require-executed",
        action="store_true",
        help="exit nonzero unless the suite genuinely ran",
    )
    parser.add_argument("--format", choices=("text", "json"), default="text")
    args = parser.parse_args(argv)

    jobs = macos_jobs(args.repo, args.run_id)
    if not jobs:
        print(
            f"run {args.run_id}: no job named `macos` reported. The gate may "
            "still be pending, or this event does not post that context.",
            file=sys.stderr,
        )
        return 1 if args.require_executed else 0

    results = []
    for job in jobs:
        steps = job.get("steps") or []
        verdict = classify_steps(steps)
        results.append(
            {
                "job_id": job.get("id"),
                "conclusion": job.get("conclusion"),
                "step_count": len(steps),
                "verdict": verdict,
                "detail": describe(verdict),
            }
        )

    if args.format == "json":
        print(json.dumps(results, indent=2, sort_keys=True))
    else:
        for row in results:
            print(
                f"run {args.run_id} job {row['job_id']}: "
                f"conclusion={row['conclusion']} steps={row['step_count']} "
                f"-> {row['verdict'].upper()}"
            )
            print(f"    {row['detail']}")

    if args.require_executed and not any(r["verdict"] == EXECUTED for r in results):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
