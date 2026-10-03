#!/usr/bin/env python3
"""Withhold an SDK tag while main's MSVC CLI compile is known to be broken.

Used by .github/workflows/auto-release.yml before it creates a `vX.Y.Z` tag.
A tag starts release-cli.yml, whose Windows legs compile the CLI with MSVC; if
that compile is already known to fail, the tag can only produce a release whose
Windows legs fail and whose `release` job is skipped. Withholding the tag keeps
the version untagged instead, which release-cadence-check.yml reports, and the
next push to main re-evaluates: auto-release tags the then-current version once
this check clears.

Evidence: the push runs of .github/workflows/windows-cli-compile.yml on main.
Most of them skip compiling (their range could not reach the MSVC CLI build), so
the verdict is the newest run whose `Build CLI targets (MSVC)` step actually ran,
restricted to runs whose commit is an ancestor of the commit being tagged.

  * That step FAILED  -> block.
  * That step PASSED  -> allow.
  * No such run, an unreadable API, or a run that failed before compiling
    (runner or bootstrap trouble) -> allow, with the reason printed. This check
    narrows when a tag is cut; release-cli.yml stays the authority, so missing
    evidence must not withhold a release.

`PULP_RELEASE_WINDOWS_PRECHECK=off` disables the check.

The decision logic is `decide()`, which is pure; `main()` is the I/O shell.
Output: one JSON line on stdout, and `block=1|0` plus `reason=` in
$GITHUB_OUTPUT when running under GitHub Actions. Exit status is always 0.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from dataclasses import dataclass
from typing import Callable, Iterable, Optional

WORKFLOW_FILE = "windows-cli-compile.yml"
COMPILE_JOB = "Windows CLI compile (MSVC)"
COMPILE_STEP = "Build CLI targets (MSVC)"
# Each run that skipped compiling costs one jobs call; this bounds a sweep.
MAX_RUNS_INSPECTED = 30

BLOCK = "block"
ALLOW = "allow"


@dataclass(frozen=True)
class Verdict:
    action: str
    reason: str
    run_url: str = ""

    def as_json(self) -> str:
        return json.dumps(
            {"action": self.action, "reason": self.reason, "run_url": self.run_url}
        )


def compile_step_outcome(jobs: Iterable[dict]) -> Optional[str]:
    """Return the compile step's conclusion, or None if it never ran.

    "failure" means the compiler rejected the code. A job that failed in an
    earlier step (bootstrap, toolchain install) leaves the compile step skipped,
    which is reported as "infrastructure" so the caller does not block on it.
    """
    for job in jobs:
        if job.get("name") != COMPILE_JOB:
            continue
        if job.get("conclusion") in (None, "skipped"):
            return None
        for step in job.get("steps", []):
            if step.get("name") == COMPILE_STEP:
                conclusion = step.get("conclusion")
                if conclusion in ("success", "failure"):
                    return conclusion
                return "infrastructure"
        return "infrastructure"
    return None


def decide(
    runs: Iterable[dict],
    *,
    is_ancestor: Callable[[str], bool],
    jobs_for: Callable[[int], list[dict]],
    max_runs: int = MAX_RUNS_INSPECTED,
) -> Verdict:
    """Pick the newest compiled verdict among ancestor push runs. Pure."""
    ordered = sorted(
        (r for r in runs if r.get("status") == "completed"),
        key=lambda r: r.get("created_at", ""),
        reverse=True,
    )
    inspected = 0
    for run in ordered:
        if run.get("conclusion") in ("cancelled", "skipped", "stale"):
            continue
        sha = run.get("head_sha", "")
        if not sha or not is_ancestor(sha):
            continue
        if inspected >= max_runs:
            break
        inspected += 1
        outcome = compile_step_outcome(jobs_for(run["id"]))
        url = run.get("html_url", "")
        if outcome == "failure":
            return Verdict(
                BLOCK,
                f"the MSVC CLI compile failed at {sha[:12]} and no later ancestor "
                "compiled cleanly; a tag now would fail release-cli.yml's Windows legs",
                url,
            )
        if outcome == "success":
            return Verdict(ALLOW, f"the MSVC CLI compile passed at {sha[:12]}", url)
        if outcome == "infrastructure":
            return Verdict(
                ALLOW,
                f"the newest compile at {sha[:12]} failed before compiling "
                "(runner or bootstrap), which says nothing about the code",
                url,
            )
    return Verdict(
        ALLOW,
        f"no compiled verdict among the last {inspected} ancestor run(s); "
        "nothing to hold the tag on",
    )


def _gh_json(args: list[str]) -> object:
    out = subprocess.run(
        ["gh", *args], capture_output=True, text=True, check=True
    ).stdout
    return json.loads(out) if out.strip() else {}


def _is_ancestor(head: str) -> Callable[[str], bool]:
    def check(sha: str) -> bool:
        result = subprocess.run(
            ["git", "merge-base", "--is-ancestor", sha, head],
            capture_output=True,
        )
        return result.returncode == 0

    return check


def _write_output(verdict: Verdict) -> None:
    target = os.environ.get("GITHUB_OUTPUT")
    if not target:
        return
    reason = verdict.reason.replace("\n", " ")
    with open(target, "a", encoding="utf-8") as handle:
        handle.write(f"block={'1' if verdict.action == BLOCK else '0'}\n")
        handle.write(f"reason={reason}\n")
        handle.write(f"run_url={verdict.run_url}\n")


def main() -> int:
    if os.environ.get("PULP_RELEASE_WINDOWS_PRECHECK", "").strip().lower() == "off":
        verdict = Verdict(ALLOW, "disabled by PULP_RELEASE_WINDOWS_PRECHECK=off")
    else:
        repo = os.environ["REPO"]
        head = os.environ.get("HEAD_SHA", "HEAD")
        try:
            listing = _gh_json(
                [
                    "api",
                    f"repos/{repo}/actions/workflows/{WORKFLOW_FILE}/runs"
                    "?branch=main&event=push&status=completed&per_page=100",
                ]
            )
            runs = listing.get("workflow_runs", []) if isinstance(listing, dict) else []

            def jobs_for(run_id: int) -> list[dict]:
                data = _gh_json(
                    ["api", f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100"]
                )
                return data.get("jobs", []) if isinstance(data, dict) else []

            verdict = decide(runs, is_ancestor=_is_ancestor(head), jobs_for=jobs_for)
        except (subprocess.CalledProcessError, ValueError, KeyError) as error:
            detail = getattr(error, "stderr", "") or str(error)
            verdict = Verdict(
                ALLOW,
                f"could not read {WORKFLOW_FILE} runs ({detail.strip()[:200]}); "
                "not holding the tag on missing evidence",
            )
    print(verdict.as_json())
    _write_output(verdict)
    return 0


if __name__ == "__main__":
    sys.exit(main())
