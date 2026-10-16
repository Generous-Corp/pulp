#!/usr/bin/env python3
"""Tests for unserved_job_check.py and the nightly-intel wiring that uses it."""
from __future__ import annotations

import copy
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import unserved_job_check as ujc  # noqa: E402

FIXTURE = HERE / "fixtures" / "nightly_intel_unserved_jobs.json"
NATIVE = "Native Intel build + test"
WORKFLOW = HERE.parents[1] / ".github" / "workflows" / "nightly-intel.yml"


def jobs() -> list[dict]:
    return copy.deepcopy(json.loads(FIXTURE.read_text(encoding="utf-8"))["jobs"])


def native(rows: list[dict]) -> dict:
    return next(job for job in rows if job["name"] == NATIVE)


def run_main(*argv: str) -> tuple[int, dict[str, str]]:
    out = io.StringIO()
    with redirect_stdout(out):
        rc = ujc.main(list(argv))
    pairs = dict(line.split("=", 1) for line in out.getvalue().splitlines() if "=" in line)
    return rc, pairs


class ClassifyTests(unittest.TestCase):
    def test_recorded_2026_10_14_run_is_unserved(self) -> None:
        # The run that waited 8 h for a deleted runner, then was cancelled.
        rc, out = run_main("--job", NATIVE, "--jobs-json", str(FIXTURE))
        self.assertEqual(rc, 0)
        self.assertEqual(out["verdict"], "unserved")
        self.assertEqual(out["conclusion"], "cancelled")
        self.assertEqual(out["waited_minutes"], "490")
        self.assertIn("pulp-host-macmini", out["labels"])

    def test_a_job_that_ran_and_was_cancelled_is_served(self) -> None:
        # Negative control: a job-level timeout also concludes `cancelled`,
        # but a runner picked it up, so the lane is alive.
        job = native(jobs())
        job.update(runner_name="pulp-intel-macmini", runner_id=36284)
        self.assertEqual(ujc.classify(job)["verdict"], "served")

    def test_skipped_and_pending_jobs_are_not_unserved(self) -> None:
        job = native(jobs())
        job.update(conclusion="skipped")
        self.assertEqual(ujc.classify(job)["verdict"], "skipped")
        job = native(jobs())
        job.update(status="queued", conclusion=None)
        self.assertEqual(ujc.classify(job)["verdict"], "pending")

    def test_latest_attempt_wins(self) -> None:
        rows = jobs()
        rerun = copy.deepcopy(native(rows))
        rerun.update(run_attempt=2, runner_name="pulp-intel-macmini", conclusion="success")
        rows.append(rerun)
        self.assertEqual(ujc.classify(ujc.find_job(rows, NATIVE))["verdict"], "served")

    def test_missing_job_or_unreadable_listing_is_an_error_never_served(self) -> None:
        rc, out = run_main("--job", "No such job", "--jobs-json", str(FIXTURE))
        self.assertEqual((rc, out), (2, {}))
        with tempfile.TemporaryDirectory() as td:
            bad = Path(td) / "bad.json"
            bad.write_text("{not json", encoding="utf-8")
            rc, out = run_main("--job", NATIVE, "--jobs-json", str(bad))
        self.assertEqual((rc, out), (2, {}))


class NightlyIntelWiringTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = WORKFLOW.read_text(encoding="utf-8")
        self.watchdog = self.workflow.split("\n  watchdog:\n", 1)[1]

    def test_watchdog_reads_the_native_job_by_its_exact_name(self) -> None:
        self.assertIn("name: Native Intel build + test\n", self.workflow)
        self.assertIn(f'--job "{NATIVE}"', self.watchdog)
        self.assertIn("actions: read", self.watchdog)

    def test_an_unserved_native_job_fails_the_watchdog(self) -> None:
        step = self.watchdog.split("- name: Native Intel lane served", 1)[1]
        self.assertIn("if: always()", step)
        ok, rest = step.split('if [ "$VERDICT" != "unserved" ]; then', 1)
        # Only served and skipped exit 0. Everything else, including an empty
        # verdict from a failed read, exits 1.
        self.assertIn('[ "$VERDICT" = "served" ] || [ "$VERDICT" = "skipped" ]', ok)
        self.assertEqual(ok.count("exit 0"), 1)
        self.assertIn("exit 1", rest.split("\n          fi\n", 1)[0])
        self.assertTrue(rest.rstrip().endswith("exit 1"))
        # The detection step turns a read failure into an empty verdict.
        detect = self.watchdog.split("- name: Detect an unserved native Intel job", 1)[1]
        self.assertIn('echo "verdict=" >> "$GITHUB_OUTPUT"', detect)


if __name__ == "__main__":
    unittest.main()
