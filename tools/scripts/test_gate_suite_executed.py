#!/usr/bin/env python3
"""Tests for the `macos` gate suite-execution classifier.

The fixtures are the real shapes observed on Generous-Corp/pulp: a bootstrap
job that claimed the required `macos` context with three steps and reported
success, and a matrix job with the full step list that genuinely ran the suite.
Both reported through the same check name, which is why the distinction has to
be drawn from the steps rather than the conclusion.
"""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import gate_suite_executed as gse  # noqa: E402


def step(name: str, conclusion: str = "success") -> dict:
    return {"name": name, "conclusion": conclusion}


# The receipt-reuse / skip-safe bootstrap: three steps, green, no suite.
BOOTSTRAP_JOB = [
    step("Set up job"),
    step("macOS merge-group bootstrap (required when native leg is absent)"),
    step("Complete job"),
]

# An abridged but faithful matrix job that really ran.
REAL_JOB = [
    step("Set up job"),
    step("Run actions/checkout@v5"),
    step("Bootstrap repository dependencies"),
    step("Configure"),
    step("Build"),
    step("Test (non-Windows)"),
    step("Surface ctest failures (non-Windows)"),
    step("Complete job"),
]


class ClassifyStepsTests(unittest.TestCase):
    def test_bootstrap_job_is_not_executed(self) -> None:
        self.assertEqual(gse.classify_steps(BOOTSTRAP_JOB), gse.NOT_EXECUTED)

    def test_real_job_is_executed(self) -> None:
        self.assertEqual(gse.classify_steps(REAL_JOB), gse.EXECUTED)

    def test_a_failing_suite_still_counts_as_executed(self) -> None:
        """The question is whether it RAN, not whether it passed."""
        failed = [s.copy() for s in REAL_JOB]
        for s in failed:
            if s["name"].startswith("Test ("):
                s["conclusion"] = "failure"
        self.assertEqual(gse.classify_steps(failed), gse.EXECUTED)

    def test_skipped_test_step_is_built_but_untested(self) -> None:
        """A pull-request head builds but skips the suite; that is not a pass."""
        skipped = [s.copy() for s in REAL_JOB]
        for s in skipped:
            if s["name"].startswith("Test ("):
                s["conclusion"] = "skipped"
        self.assertEqual(gse.classify_steps(skipped), gse.BUILT_BUT_UNTESTED)

    def test_cancelled_test_step_is_not_a_pass(self) -> None:
        cancelled = [s.copy() for s in REAL_JOB]
        for s in cancelled:
            if s["name"].startswith("Test ("):
                s["conclusion"] = "cancelled"
        self.assertNotEqual(gse.classify_steps(cancelled), gse.EXECUTED)

    def test_empty_step_list_is_not_executed(self) -> None:
        self.assertEqual(gse.classify_steps([]), gse.NOT_EXECUTED)

    def test_windows_test_step_also_counts(self) -> None:
        job = [step("Set up job"), step("Test (Windows — UTF-8 code page wrapped)")]
        self.assertEqual(gse.classify_steps(job), gse.EXECUTED)

    def test_a_step_merely_named_test_does_not_count(self) -> None:
        """`Test A2T structural receipt issuer` is not the suite."""
        job = [
            step("Set up job"),
            step("Test A2T structural receipt issuer"),
            step("Complete job"),
        ]
        self.assertEqual(gse.classify_steps(job), gse.NOT_EXECUTED)

    def test_missing_conclusion_is_treated_as_not_run(self) -> None:
        job = [step("Set up job"), {"name": "Test (non-Windows)", "conclusion": None}]
        self.assertEqual(gse.classify_steps(job), gse.NOT_EXECUTED)


class DescribeTests(unittest.TestCase):
    def test_every_verdict_has_a_description(self) -> None:
        for verdict in (gse.EXECUTED, gse.BUILT_BUT_UNTESTED, gse.NOT_EXECUTED):
            with self.subTest(verdict=verdict):
                self.assertTrue(gse.describe(verdict).strip())

    def test_non_executed_descriptions_refuse_to_read_as_a_pass(self) -> None:
        for verdict in (gse.BUILT_BUT_UNTESTED, gse.NOT_EXECUTED):
            with self.subTest(verdict=verdict):
                self.assertIn("NO", gse.describe(verdict))


if __name__ == "__main__":
    unittest.main()
