#!/usr/bin/env python3
"""Unit tests for pr_flow_audit classification, reachability, and --fix safety.

Every guarantee is asserted in both directions. A test that only shows the
flagged case passes just as happily when the classifier flags everything, so
each positive cell is paired with a control that must NOT be flagged — and for
reachability the control is the same PR judged against a workflow whose
condition lacks the event exclusion, which is the distinction the tool exists
to make.

No test performs a live or mutating call.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "pr_flow_audit", HERE / "pr_flow_audit.py")
mod = importlib.util.module_from_spec(_spec)
# Register before exec so the module's dataclasses resolve their __module__.
sys.modules[_spec.name] = mod
_spec.loader.exec_module(mod)

NOW = datetime(2026, 9, 22, 12, 0, 0, tzinfo=timezone.utc)

# The step condition as it stands on base: the test step is excluded from the
# pull_request event, so a fresh run skips it.
WORKFLOW_EXCLUDED = """
name: Build and Test
on:
  pull_request:
  merge_group:
jobs:
  build:
    steps:
      - name: Build
        run: cmake --build build
      - name: Test (non-Windows)
        if: runner.os != 'Windows' && github.event_name != 'pull_request'
        run: ctest
      - name: Surface ctest failures (non-Windows)
        if: failure() && runner.os != 'Windows'
        run: cat log
"""

# The SAME workflow before the exclusion landed. The only difference is the one
# clause, so any test run against both isolates the condition evaluation.
WORKFLOW_REACHABLE = WORKFLOW_EXCLUDED.replace(
    "if: runner.os != 'Windows' && github.event_name != 'pull_request'",
    "if: runner.os != 'Windows'",
)


def make_pr(number=1, **over):
    """A PR that is clean, armed, green, and therefore uninteresting."""
    pr = {
        "number": number,
        "title": f"pr {number}",
        "mergeable_state": "clean",
        "auto_merge": True,
        "draft": False,
        "head_sha": "a" * 40,
        "updated_at": NOW.isoformat(),
        "errors": [],
        "checks": [
            {"name": "macos", "status": "completed", "conclusion": "success",
             "started_at": "2026-09-22T10:00:00Z", "run_id": 99},
        ],
    }
    pr.update(over)
    return pr


def failed_gate(steps, run_id=99):
    """A PR whose macos gate failed at the named steps."""
    return {
        "checks": [
            {"name": "macos", "status": "completed", "conclusion": "failure",
             "started_at": "2026-09-22T10:00:00Z", "run_id": run_id},
        ],
        "gate_failed_steps": steps,
    }


def classify(pr, workflow=WORKFLOW_EXCLUDED, now=NOW, **kw):
    steps = mod.parse_workflow_steps(workflow)
    return mod.classify_pr(pr, steps, "", now, **kw)


class ExpressionTest(unittest.TestCase):
    """The evaluator must distinguish the two workflows, or nothing else holds."""

    def test_event_exclusion_makes_step_unreachable(self):
        self.assertFalse(mod.evaluate_condition(
            "runner.os != 'Windows' && github.event_name != 'pull_request'",
            event_name="pull_request", runner_os="macOS"))

    def test_same_condition_without_exclusion_is_reachable(self):
        self.assertTrue(mod.evaluate_condition(
            "runner.os != 'Windows'",
            event_name="pull_request", runner_os="macOS"))

    def test_absent_condition_always_runs(self):
        self.assertTrue(mod.evaluate_condition(
            None, event_name="pull_request", runner_os="macOS"))

    def test_unknown_reference_is_refused_rather_than_guessed(self):
        with self.assertRaises(mod.WorkflowError):
            mod.evaluate_condition("github.actor == 'nobody'",
                                   event_name="pull_request", runner_os="macOS")

    def test_consequence_condition_detected(self):
        self.assertTrue(mod.is_consequence_condition("failure() && runner.os != 'Windows'"))

    def test_plain_condition_is_not_a_consequence(self):
        self.assertFalse(mod.is_consequence_condition("runner.os != 'Windows'"))


class UnreachableStepTest(unittest.TestCase):
    """A failure at a step a fresh run would skip, and the control that it would not."""

    def test_failure_at_now_unreachable_step_is_auto_fixable(self):
        pr = make_pr(mergeable_state="behind",
                     **failed_gate(["Test (non-Windows)"]))
        finding = classify(pr, WORKFLOW_EXCLUDED)
        self.assertEqual(finding.bucket, mod.AUTO_FIXABLE)
        # The fix must be a fresh merge commit: re-running replays the same
        # workflow version that reached the step in the first place.
        self.assertEqual(finding.action, "update-branch")

    def test_control_same_pr_when_the_step_is_still_reachable(self):
        # Identical PR, identical failing step — only the workflow condition
        # differs. This is what proves the verdict comes from evaluation.
        pr = make_pr(mergeable_state="behind",
                     **failed_gate(["Test (non-Windows)"]))
        finding = classify(pr, WORKFLOW_REACHABLE)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertNotEqual(finding.action, "update-branch")
        self.assertIn("Test (non-Windows)", finding.detail["blocking_steps"])

    def test_consequence_step_alongside_unreachable_step_still_auto_fixable(self):
        # The reporting step only ran because the test step failed; clearing
        # the cause clears it too.
        pr = make_pr(mergeable_state="behind", **failed_gate(
            ["Test (non-Windows)", "Surface ctest failures (non-Windows)"]))
        self.assertEqual(classify(pr, WORKFLOW_EXCLUDED).bucket, mod.AUTO_FIXABLE)

    def test_a_genuinely_blocking_step_beats_an_unreachable_one(self):
        pr = make_pr(mergeable_state="behind",
                     **failed_gate(["Test (non-Windows)", "Build"]))
        finding = classify(pr, WORKFLOW_EXCLUDED)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertEqual(finding.detail["blocking_steps"], ["Build"])


class CheapWinTest(unittest.TestCase):
    """Conflicted PRs holding a green gate are the cheapest wins in a backlog."""

    def test_green_gate_plus_dirty_is_surfaced_as_cheap_win(self):
        finding = classify(make_pr(mergeable_state="dirty"))
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertTrue(finding.cheap_win)
        self.assertEqual(finding.action, "resolve the conflict")

    def test_control_green_gate_plus_clean_is_not_surfaced(self):
        finding = classify(make_pr(mergeable_state="clean"))
        self.assertEqual(finding.bucket, mod.MOVING)
        self.assertFalse(finding.cheap_win)

    def test_dirty_without_a_green_gate_is_not_a_cheap_win(self):
        pr = make_pr(mergeable_state="dirty", **failed_gate(["Build"]))
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertFalse(finding.cheap_win)

    def test_cheap_wins_sort_ahead_of_everything_else(self):
        snapshot = {
            "workflow_text": WORKFLOW_EXCLUDED,
            "prs": [make_pr(10, mergeable_state="clean"),
                    make_pr(20, mergeable_state="dirty")],
        }
        self.assertEqual([f.number for f in mod.analyze(snapshot, NOW)], [20, 10])


class AutoMergeArmingTest(unittest.TestCase):
    def test_mergeable_and_unarmed_is_flagged(self):
        finding = classify(make_pr(mergeable_state="clean", auto_merge=False))
        self.assertEqual(finding.bucket, mod.AUTO_FIXABLE)
        self.assertEqual(finding.action, "enable-auto-merge")

    def test_control_mergeable_and_armed_is_not_flagged(self):
        finding = classify(make_pr(mergeable_state="clean", auto_merge=True))
        self.assertEqual(finding.bucket, mod.MOVING)
        self.assertEqual(finding.action, "")

    def test_behind_and_unarmed_is_flagged(self):
        finding = classify(make_pr(mergeable_state="behind", auto_merge=False))
        self.assertEqual(finding.bucket, mod.AUTO_FIXABLE)

    def test_a_draft_is_never_armed_automatically(self):
        finding = classify(make_pr(mergeable_state="clean", auto_merge=False, draft=True))
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)


class InfraLossTest(unittest.TestCase):
    def test_failure_with_no_failing_step_is_infra_loss(self):
        pr = make_pr(mergeable_state="behind", **failed_gate([]))
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.AUTO_FIXABLE)
        self.assertEqual(finding.action, "rerun-failed-jobs")
        self.assertEqual(finding.detail["run_id"], 99)

    def test_control_failure_with_a_failing_step_is_not_infra_loss(self):
        pr = make_pr(mergeable_state="behind", **failed_gate(["Build"]))
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertNotEqual(finding.action, "rerun-failed-jobs")


class FailClosedTest(unittest.TestCase):
    """Unreadable state must become UNKNOWN, and --fix must never touch it."""

    def test_collection_error_is_unknown(self):
        pr = make_pr(errors=["check runs: HTTP 502"])
        self.assertEqual(classify(pr).bucket, mod.UNKNOWN)

    def test_absent_checks_are_unknown_not_green(self):
        pr = make_pr(checks=None)
        self.assertEqual(classify(pr).bucket, mod.UNKNOWN)

    def test_uncomputed_mergeable_state_is_unknown(self):
        self.assertEqual(classify(make_pr(mergeable_state="unknown")).bucket, mod.UNKNOWN)
        self.assertEqual(classify(make_pr(mergeable_state=None)).bucket, mod.UNKNOWN)

    def test_unparseable_workflow_makes_a_gate_failure_unknown(self):
        pr = make_pr(mergeable_state="behind", **failed_gate(["Test (non-Windows)"]))
        snapshot = {"workflow_text": "{{ not yaml", "prs": [pr]}
        self.assertEqual(mod.analyze(snapshot, NOW)[0].bucket, mod.UNKNOWN)

    def test_uncollected_workflow_makes_a_gate_failure_unknown(self):
        pr = make_pr(mergeable_state="behind", **failed_gate(["Test (non-Windows)"]))
        snapshot = {"workflow_text": None, "workflow_error": "404", "prs": [pr]}
        self.assertEqual(mod.analyze(snapshot, NOW)[0].bucket, mod.UNKNOWN)

    def test_unreadable_failing_steps_are_unknown(self):
        pr = make_pr(mergeable_state="behind")
        pr["checks"] = [{"name": "macos", "status": "completed",
                         "conclusion": "failure", "started_at": "x", "run_id": 99}]
        # gate_failed_steps absent entirely: the collector could not read them.
        self.assertEqual(classify(pr).bucket, mod.UNKNOWN)

    def test_step_absent_from_base_workflow_is_undecidable(self):
        # Absence cannot distinguish "base removed it" from "this PR adds it".
        pr = make_pr(mergeable_state="behind", **failed_gate(["Install new deps"]))
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.UNKNOWN)

    def test_ambiguous_step_name_is_undecidable(self):
        workflow = """
jobs:
  a:
    steps:
      - name: Shared
        if: github.event_name != 'pull_request'
  b:
    steps:
      - name: Shared
        run: echo
"""
        pr = make_pr(mergeable_state="behind", **failed_gate(["Shared"]))
        self.assertEqual(classify(pr, workflow).bucket, mod.UNKNOWN)

    def test_fix_selection_excludes_unknown_and_needs_human(self):
        findings = [
            mod.Finding(1, "unknown", mod.UNKNOWN, "unreadable"),
            mod.Finding(2, "human", mod.NEEDS_HUMAN, "conflict"),
            mod.Finding(3, "moving", mod.MOVING, "running"),
            mod.Finding(4, "fixable", mod.AUTO_FIXABLE, "unarmed",
                        action="enable-auto-merge"),
        ]
        self.assertEqual([f.number for f in mod.select_fixable(findings)], [4])

    def test_apply_fix_refuses_a_non_auto_fixable_finding(self):
        # The independent second guard: even called directly, a fix must not
        # run against a PR that was not classified auto-fixable.
        for bucket in (mod.UNKNOWN, mod.NEEDS_HUMAN, mod.MOVING):
            with self.assertRaises(ValueError):
                mod.apply_fix(
                    mod.Finding(7, "t", bucket, "r", action="enable-auto-merge"),
                    "o/r", "ghapp", dry_run=True)

    def test_dry_run_mutates_nothing(self):
        # A dry run must not invoke the CLI at all.
        def explode(*_a, **_k):
            raise AssertionError("dry run invoked the GitHub CLI")

        original = mod._gh
        mod._gh = explode
        try:
            finding = mod.Finding(8, "t", mod.AUTO_FIXABLE, "r", action="update-branch")
            self.assertIn("would update", mod.apply_fix(finding, "o/r", "ghapp", True))
        finally:
            mod._gh = original


class MovingTest(unittest.TestCase):
    def test_a_running_check_is_moving(self):
        pr = make_pr(mergeable_state="dirty")
        pr["checks"] = [{"name": "linux", "status": "in_progress",
                         "conclusion": None, "started_at": "x", "run_id": 1}]
        self.assertEqual(classify(pr).bucket, mod.MOVING)

    def test_merge_queue_membership_is_moving(self):
        self.assertEqual(
            classify(make_pr(mergeable_state="dirty", in_merge_queue=True)).bucket,
            mod.MOVING)

    def test_absent_required_gate_is_a_leak_not_health(self):
        pr = make_pr(mergeable_state="clean", checks=[
            {"name": "linux", "status": "completed", "conclusion": "success",
             "started_at": "x", "run_id": 1}])
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertIn("never registered", finding.reason)


class UnstableTest(unittest.TestCase):
    def test_unstable_beyond_threshold_needs_a_human(self):
        pr = make_pr(mergeable_state="unstable",
                     updated_at=(NOW - timedelta(hours=9)).isoformat())
        pr["checks"].append({"name": "codex", "status": "completed",
                             "conclusion": "failure", "started_at": "x", "run_id": 2})
        finding = classify(pr, unstable_hours=6.0)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertIn("codex", finding.reason)

    def test_unstable_with_an_unreadable_age_is_unknown(self):
        pr = make_pr(mergeable_state="unstable", updated_at="not-a-date")
        self.assertEqual(classify(pr).bucket, mod.UNKNOWN)


class ReportTest(unittest.TestCase):
    def test_a_degraded_sweep_never_reports_health(self):
        snapshot = {"workflow_text": WORKFLOW_EXCLUDED, "prs": [],
                    "errors": [{"stage": "pulls", "error": "502"}]}
        text = mod.render_report(mod.analyze(snapshot, NOW), snapshot)
        self.assertIn("not a clean bill of health", text)
        self.assertNotIn("Every open pull request has work in flight", text)

    def test_a_clean_sweep_with_nothing_stalled_says_so(self):
        snapshot = {"workflow_text": WORKFLOW_EXCLUDED,
                    "prs": [make_pr(1, mergeable_state="clean")], "errors": []}
        text = mod.render_report(mod.analyze(snapshot, NOW), snapshot)
        self.assertIn("Every open pull request has work in flight", text)


class RunIdTest(unittest.TestCase):
    def test_run_id_comes_from_details_url_not_the_check_id(self):
        # A check-run id is not a job id and must never be used as one.
        check = {"id": 106527743343,
                 "details_url": "https://github.com/o/r/actions/runs/35657266004/job/106527743343"}
        self.assertEqual(mod._run_id_from(check), 35657266004)

    def test_missing_details_url_yields_no_run_id(self):
        self.assertIsNone(mod._run_id_from({"id": 5}))


if __name__ == "__main__":
    unittest.main()
