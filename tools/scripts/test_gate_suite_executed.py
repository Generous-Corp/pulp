#!/usr/bin/env python3
"""Tests for the `macos` gate suite-execution classifier.

The fixtures are the real shapes observed on Generous-Corp/pulp: a bootstrap
job that claimed the required `macos` context with three steps and reported
success, and a matrix job with the full step list that genuinely ran the suite.
Both reported through the same check name, which is why the distinction has to
be drawn from the steps rather than the conclusion.
"""

from __future__ import annotations

import contextlib
import io
import pathlib
import sys
import unittest
import unittest.mock

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


def pr_head_job(ran_tier: str | None) -> list[dict]:
    """A pull-request head: the full suite skipped, at most one tier ran."""
    tiers = ["Test fast deterministic tier (pull request head)",
             "Test what this pull request reaches (pull request head)"]
    job = [step("Set up job"), step("Build")]
    job += [step(name, "success" if name == ran_tier else "skipped") for name in tiers]
    job += [step("Test (non-Windows)", "skipped"), step("Complete job")]
    return job


class ClassifyStepsTests(unittest.TestCase):
    def test_bootstrap_job_is_not_executed(self) -> None:
        self.assertEqual(gse.classify_steps(BOOTSTRAP_JOB), gse.NOT_EXECUTED)

    def test_real_job_is_executed(self) -> None:
        self.assertEqual(gse.classify_steps(REAL_JOB), gse.EXECUTED)

    def test_a_failing_suite_still_counts_as_executed(self) -> None:
        """The question is whether it RAN, not whether it passed."""
        failed = [s.copy() for s in REAL_JOB]
        for s in failed:
            if gse.TEST_STEP_RE.match(s["name"]):
                s["conclusion"] = "failure"
        self.assertEqual(gse.classify_steps(failed), gse.EXECUTED)

    def test_skipped_test_step_is_built_but_untested(self) -> None:
        """A pull-request head builds but skips the suite; that is not a pass."""
        skipped = [s.copy() for s in REAL_JOB]
        for s in skipped:
            if gse.TEST_STEP_RE.match(s["name"]):
                s["conclusion"] = "skipped"
        self.assertEqual(gse.classify_steps(skipped), gse.BUILT_BUT_UNTESTED)

    def test_cancelled_test_step_is_not_a_pass(self) -> None:
        cancelled = [s.copy() for s in REAL_JOB]
        for s in cancelled:
            if gse.TEST_STEP_RE.match(s["name"]):
                s["conclusion"] = "cancelled"
        self.assertNotEqual(gse.classify_steps(cancelled), gse.EXECUTED)

    def test_empty_step_list_is_not_executed(self) -> None:
        self.assertEqual(gse.classify_steps([]), gse.NOT_EXECUTED)

    def test_windows_test_step_also_counts(self) -> None:
        job = [step("Set up job"), step("Test (Windows — UTF-8 code page wrapped)")]
        self.assertEqual(gse.classify_steps(job), gse.EXECUTED)

    def test_each_pull_request_tier_alone_is_tier_only(self) -> None:
        # One fixture per tier step name, so deleting either alternative from
        # TIER_STEP_RE is caught on its own.
        for name in ("Test fast deterministic tier (pull request head)",
                     "Test what this pull request reaches (pull request head)"):
            with self.subTest(tier=name):
                self.assertEqual(gse.classify_steps(pr_head_job(name)), gse.TIER_ONLY)

    def test_a_tier_is_never_the_suite(self) -> None:
        # A pull-request head that ran a tier but skipped `Test (...)` must
        # not read as the suite having run, even when the tier failed.
        job = pr_head_job("Test fast deterministic tier (pull request head)")
        for s in job:
            if s["name"].startswith("Test fast"):
                s["conclusion"] = "failure"
        self.assertEqual(gse.classify_steps(job), gse.TIER_ONLY)
        self.assertNotIn("the macOS test suite ran", gse.describe(gse.TIER_ONLY))

    def test_no_tier_and_no_suite_is_built_but_untested(self) -> None:
        self.assertEqual(gse.classify_steps(pr_head_job(None)), gse.BUILT_BUT_UNTESTED)

    def test_the_full_suite_outranks_a_tier(self) -> None:
        job = pr_head_job("Test what this pull request reaches (pull request head)")
        for s in job:
            if s["name"] == "Test (non-Windows)":
                s["conclusion"] = "success"
        self.assertEqual(gse.classify_steps(job), gse.EXECUTED)

    def test_build_with_non_suite_test_step_is_not_executed(self) -> None:
        """A real build plus an issuer/check step is still not suite evidence."""
        job = [
            step("Set up job"),
            step("Build"),
            step("Test A2T structural receipt issuer"),
            step("Complete job"),
        ]
        self.assertEqual(gse.classify_steps(job), gse.BUILT_BUT_UNTESTED)

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
        for verdict in (gse.EXECUTED, gse.TIER_ONLY, gse.BUILT_BUT_UNTESTED, gse.NOT_EXECUTED):
            with self.subTest(verdict=verdict):
                self.assertTrue(gse.describe(verdict).strip())

    def test_non_executed_descriptions_refuse_to_read_as_a_pass(self) -> None:
        for verdict in (gse.TIER_ONLY, gse.BUILT_BUT_UNTESTED, gse.NOT_EXECUTED):
            with self.subTest(verdict=verdict):
                self.assertIn("NO", gse.describe(verdict))


class RequireExecutedTests(unittest.TestCase):
    def _main(self, steps: list[dict]) -> int:
        job = {"id": 1, "name": "macos", "conclusion": "success", "steps": steps}
        with unittest.mock.patch.object(gse, "macos_jobs", return_value=[job]), \
                contextlib.redirect_stdout(io.StringIO()):
            return gse.main(["--run-id", "1", "--require-executed"])

    def test_a_tier_only_run_does_not_satisfy_require_executed(self) -> None:
        tier_only = pr_head_job("Test fast deterministic tier (pull request head)")
        self.assertEqual(self._main(tier_only), 1)

    def test_control_a_full_suite_run_satisfies_it(self) -> None:
        self.assertEqual(self._main(REAL_JOB), 0)


if __name__ == "__main__":
    unittest.main()
