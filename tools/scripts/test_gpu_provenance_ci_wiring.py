#!/usr/bin/env python3
"""Keep GPU-provenance history reachable in every CI job that runs a broad ctest.

The GPU provenance selftests (`gpu-recipe-catalog-selftest`,
`gpu-handoff-provenance-selftest`, and the probe/trace acceptance suites) read
real per-path git history. A shallow Actions checkout has none of it, so a job
that runs ctest WITHOUT narrowing the selection away from those tests is a
latent red the moment it is scheduled.

The sanctioned remedy is `tools/scripts/hydrate_gpu_provenance_commits.py`,
which unshallows a shallow checkout and self-verifies. A checkout that already
clones full history (`fetch-depth: 0`) is equally sufficient. This test asserts
one of the two is present in every job that runs a non-narrowing ctest.

This runs in `workflow-lint.yml`, which installs PyYAML, and is deliberately
NOT a ctest: the required macOS CTest hosts have no PyYAML, so registering it
there buys either a hard ImportError on every run or a skip that reports a
green result proving nothing. Given a lane that carries the parser, the import
stays hard on purpose -- a missing parser there is a broken lane, not a reason
to degrade into a pass.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
WORKFLOW_DIR = REPO_ROOT / ".github" / "workflows"

HYDRATE_SCRIPT = "hydrate_gpu_provenance_commits.py"

# A ctest invocation, as opposed to the word "ctest" inside a comment, an echo,
# or a log filename such as `ctest-nightly.log`. Every real invocation in this
# repo passes `--test-dir` immediately, so requiring whitespace before a dash
# is both sufficient and cheap.
CTEST_INVOCATION = re.compile(r"\bctest\s+-")

# Narrowing selectors pick a SUBSET of tests by name or label, which can drop
# the GPU provenance selftests out of the run entirely -- such a job needs no
# history. `-LE` and `--exclude-regex` are deliberately NOT narrowing: they only
# REMOVE named tests from an otherwise-complete selection, so the GPU provenance
# selftests stay selected and their history is still required.
NARROWING_SELECTORS = ("-R", "--tests-regex", "-L")


def _logical_lines(run_body: str) -> list[str]:
    """Join backslash continuations and drop whole-line shell comments."""
    joined: list[str] = []
    buffer = ""
    for raw in run_body.splitlines():
        stripped = raw.strip()
        if not buffer and stripped.startswith("#"):
            continue
        if stripped.endswith("\\"):
            buffer += stripped[:-1].rstrip() + " "
            continue
        joined.append((buffer + stripped).strip())
        buffer = ""
    if buffer:
        joined.append(buffer.strip())
    return [line for line in joined if line]


def _is_narrowing(line: str) -> bool:
    for selector in NARROWING_SELECTORS:
        # Match the selector as its own token so `-L` does not match inside
        # `-LE` and `-R` does not match inside `--repeat`.
        if re.search(rf"(?<![\w-]){re.escape(selector)}(?=[\s=]|$)", line):
            return True
    return False


def _steps(job: dict) -> list[dict]:
    steps = job.get("steps")
    if not isinstance(steps, list):
        return []
    return [step for step in steps if isinstance(step, dict)]


def _job_hydrates(job: dict) -> bool:
    for step in _steps(job):
        run = step.get("run")
        if isinstance(run, str) and HYDRATE_SCRIPT in run:
            return True
    return False


def _job_clones_full_history(job: dict) -> bool:
    for step in _steps(job):
        uses = step.get("uses")
        if not isinstance(uses, str) or "actions/checkout" not in uses:
            continue
        with_block = step.get("with")
        if not isinstance(with_block, dict):
            continue
        if str(with_block.get("fetch-depth")) == "0":
            return True
    return False


def _non_narrowing_ctest_lines(job: dict) -> list[str]:
    offenders: list[str] = []
    for step in _steps(job):
        run = step.get("run")
        if not isinstance(run, str):
            continue
        for line in _logical_lines(run):
            if not CTEST_INVOCATION.search(line):
                continue
            if not _is_narrowing(line):
                offenders.append(line)
    return offenders


def _iter_jobs():
    for path in sorted(WORKFLOW_DIR.glob("*.yml")):
        document = yaml.safe_load(path.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            continue
        jobs = document.get("jobs")
        if not isinstance(jobs, dict):
            continue
        for job_name, job in jobs.items():
            if isinstance(job, dict):
                yield path, job_name, job


class GpuProvenanceCiWiringTest(unittest.TestCase):
    def test_workflow_directory_is_present(self) -> None:
        self.assertTrue(
            WORKFLOW_DIR.is_dir(),
            f"workflow directory not found at {WORKFLOW_DIR} -- the scanner is "
            "pointed at the wrong repository root",
        )

    def test_broad_ctest_jobs_keep_gpu_provenance_history(self) -> None:
        total_ctest_jobs = 0
        failures: list[str] = []

        for path, job_name, job in _iter_jobs():
            offenders = _non_narrowing_ctest_lines(job)
            if not offenders:
                continue
            total_ctest_jobs += 1
            if _job_hydrates(job) or _job_clones_full_history(job):
                continue
            failures.append(
                f"{path.relative_to(REPO_ROOT)} job '{job_name}' runs a "
                f"non-narrowing ctest without GPU provenance history:\n"
                f"    {offenders[0]}\n"
                f"  Add the canonical hydrate step after the checkout:\n"
                f"    - name: Hydrate bounded GPU provenance commits\n"
                f"      shell: bash\n"
                f"      run: python3 tools/scripts/{HYDRATE_SCRIPT}\n"
                f"  (or `fetch-depth: 0` when the checkout cannot fetch)"
            )

        # Control: this suite can only be trusted while the scanner still SEES
        # the broad-ctest jobs. A count below the floor means the scanner
        # stopped recognizing ctest invocations (a parse change, a selector
        # rename, a refactor into a composite action) -- not that the repo
        # became safer. Treat a drop as a broken instrument, not a pass.
        self.assertGreaterEqual(
            total_ctest_jobs,
            12,
            f"only {total_ctest_jobs} jobs with a non-narrowing ctest were "
            "found; the scanner has gone blind rather than the repo having "
            "gotten safer -- re-check _logical_lines/CTEST_INVOCATION before "
            "lowering this floor",
        )

        self.assertEqual([], failures, "\n\n".join(failures))


if __name__ == "__main__":
    unittest.main()
