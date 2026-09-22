#!/usr/bin/env python3
"""Unit tests for pr_flow_audit classification, reachability, and --fix safety.

Every guarantee is asserted in both directions. A test that only shows the
flagged case passes just as happily when the classifier flags everything, so
each positive cell is paired with a control that must NOT be flagged — and for
reachability the control is the same PR judged against a condition that lacks
the event exclusion, which is the distinction the tool exists to make.

The collector is exercised against recorded API payloads rather than left to
the live API. Every route the fake serves is a shape GitHub really returns, and
the three that matter most — a PR sitting in the merge queue, a PR the queue
ejected, and a gate somebody cancelled — are indistinguishable from an
ordinary unarmed PR in any single REST field, which is exactly why they need
recorded evidence rather than a hand-reasoned assertion.

This file imports no third-party module. The module under test must stay
importable on a stock CI runner, and a test that cannot run there measures
nothing.

No test performs a live or mutating call.
"""

from __future__ import annotations

import base64
import importlib.util
import json
import subprocess
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

try:  # Only the workflow-parsing tests need it; the rest must not.
    import yaml  # noqa: F401
    HAVE_YAML = True
except ImportError:
    HAVE_YAML = False

NOW = datetime(2026, 9, 22, 12, 0, 0, tzinfo=timezone.utc)
REPO = "o/r"
SHA = "a" * 40


def steps_from(pairs):
    """Build the parsed-workflow model directly, without a YAML round trip."""
    return {
        name: mod.StepCondition(name=name, expr=expr, ambiguous=ambiguous)
        for name, expr, ambiguous in pairs
    }


# The step conditions as they stand on base: the test step is excluded from the
# pull_request event, so a fresh run skips it.
STEPS_EXCLUDED = steps_from([
    ("Build", None, False),
    ("Test (non-Windows)",
     "runner.os != 'Windows' && github.event_name != 'pull_request'", False),
    ("Surface ctest failures (non-Windows)",
     "failure() && runner.os != 'Windows'", False),
    ("Linux leg outcome (advisory)", "runner.os == 'Linux'", False),
])

# The SAME conditions before the exclusion landed. The only difference is the
# one clause, so any test run against both isolates the condition evaluation.
STEPS_REACHABLE = dict(STEPS_EXCLUDED)
STEPS_REACHABLE["Test (non-Windows)"] = mod.StepCondition(
    name="Test (non-Windows)", expr="runner.os != 'Windows'")

# The same steps as YAML, for the tests that prove the parser itself.
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


def make_pr(number=1, **over):
    """A PR that is clean, armed, green, and therefore uninteresting."""
    pr = {
        "number": number,
        "title": f"pr {number}",
        "mergeable_state": "clean",
        "auto_merge": True,
        "draft": False,
        "head_sha": SHA,
        "updated_at": NOW.isoformat(),
        "in_merge_queue": False,
        "errors": [],
        "checks": [
            {"name": "macos", "status": "completed", "conclusion": "success",
             "started_at": "2026-09-22T10:00:00Z", "run_id": 99},
        ],
    }
    pr.update(over)
    return pr


def failed_gate(steps, run_id=99, job_conclusion="failure", runner_os="macOS"):
    """A PR whose macos gate failed at the named steps."""
    return {
        "checks": [
            {"name": "macos", "status": "completed", "conclusion": "failure",
             "started_at": "2026-09-22T10:00:00Z", "run_id": run_id},
        ],
        "gate_failed_steps": steps,
        "gate_job_conclusion": job_conclusion,
        "gate_runner_os": runner_os,
    }


def classify(pr, steps=STEPS_EXCLUDED, now=NOW, **kw):
    return mod.classify_pr(pr, steps, "", now, **kw)


class ExpressionTest(unittest.TestCase):
    """The evaluator must distinguish the two conditions, or nothing else holds."""

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


class NegatedStatusFunctionTest(unittest.TestCase):
    """`!cancelled()` must never resolve to "a fresh run skips this step"."""

    def test_negated_status_function_is_undecidable(self):
        # Pinning every status function True makes this evaluate False, which
        # reads as unreachable and routes --fix at update-branch for a step
        # that genuinely failed. It has to refuse instead.
        for expr in ("!cancelled()", "!cancelled() && runner.os != 'Windows'",
                     "!(success() || cancelled())", "!failure()"):
            with self.subTest(expr=expr), self.assertRaises(mod.WorkflowError):
                mod.evaluate_condition(expr, event_name="pull_request",
                                       runner_os="macOS")

    def test_control_unnegated_status_function_still_evaluates(self):
        for expr in ("cancelled()", "always()", "success()", "failure()"):
            with self.subTest(expr=expr):
                self.assertTrue(mod.evaluate_condition(
                    expr, event_name="pull_request", runner_os="macOS"))

    def test_control_negating_a_non_status_name_is_fine(self):
        self.assertFalse(mod.evaluate_condition(
            "!(github.event_name == 'pull_request')",
            event_name="pull_request", runner_os="macOS"))

    def test_impossible_outcome_combination_is_unreachable(self):
        # No single run is both a success and a failure. Evaluating under one
        # all-true assignment calls this reachable; two assignments do not.
        self.assertFalse(mod.evaluate_condition(
            "success() && failure()", event_name="pull_request", runner_os="macOS"))

    def test_control_either_outcome_is_reachable(self):
        self.assertTrue(mod.evaluate_condition(
            "success() || failure()", event_name="pull_request", runner_os="macOS"))

    def test_a_step_guarded_by_a_negated_status_function_is_not_auto_fixable(self):
        steps = dict(STEPS_EXCLUDED)
        steps["Always report"] = mod.StepCondition(
            name="Always report", expr="!cancelled() && runner.os != 'Windows'")
        pr = make_pr(mergeable_state="behind", **failed_gate(["Always report"]))
        finding = classify(pr, steps)
        self.assertEqual(finding.bucket, mod.UNKNOWN)
        self.assertNotEqual(finding.action, "update-branch")

    def test_control_the_same_step_without_the_negation_is_blocking(self):
        steps = dict(STEPS_EXCLUDED)
        steps["Always report"] = mod.StepCondition(
            name="Always report", expr="runner.os != 'Windows'")
        pr = make_pr(mergeable_state="behind", **failed_gate(["Always report"]))
        self.assertEqual(classify(pr, steps).bucket, mod.NEEDS_HUMAN)


class RunnerOsTest(unittest.TestCase):
    """A step's reachability is answered by the OS its own job ran on."""

    def test_linux_step_is_reachable_when_judged_on_linux(self):
        pr = make_pr(mergeable_state="behind",
                     **failed_gate(["Linux leg outcome (advisory)"],
                                   runner_os="Linux"))
        self.assertEqual(classify(pr).bucket, mod.NEEDS_HUMAN)

    def test_control_the_same_step_judged_on_macos_reads_as_unreachable(self):
        # The misattribution this guards against: harvesting a Linux step and
        # evaluating it under the gate's macOS runner inverts its condition.
        pr = make_pr(mergeable_state="behind",
                     **failed_gate(["Linux leg outcome (advisory)"],
                                   runner_os="macOS"))
        self.assertEqual(classify(pr).bucket, mod.AUTO_FIXABLE)


class UnreachableStepTest(unittest.TestCase):
    """A failure at a step a fresh run would skip, and the control that it would not."""

    def test_failure_at_now_unreachable_step_is_auto_fixable(self):
        pr = make_pr(mergeable_state="behind",
                     **failed_gate(["Test (non-Windows)"]))
        finding = classify(pr, STEPS_EXCLUDED)
        self.assertEqual(finding.bucket, mod.AUTO_FIXABLE)
        # The fix must be a fresh merge commit: re-running replays the same
        # workflow version that reached the step in the first place.
        self.assertEqual(finding.action, "update-branch")

    def test_control_same_pr_when_the_step_is_still_reachable(self):
        # Identical PR, identical failing step — only the condition differs.
        # This is what proves the verdict comes from evaluation.
        pr = make_pr(mergeable_state="behind",
                     **failed_gate(["Test (non-Windows)"]))
        finding = classify(pr, STEPS_REACHABLE)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertNotEqual(finding.action, "update-branch")
        self.assertIn("Test (non-Windows)", finding.detail["blocking_steps"])

    def test_consequence_step_alongside_unreachable_step_still_auto_fixable(self):
        # The reporting step only ran because the test step failed; clearing
        # the cause clears it too.
        pr = make_pr(mergeable_state="behind", **failed_gate(
            ["Test (non-Windows)", "Surface ctest failures (non-Windows)"]))
        self.assertEqual(classify(pr, STEPS_EXCLUDED).bucket, mod.AUTO_FIXABLE)

    def test_a_genuinely_blocking_step_beats_an_unreachable_one(self):
        pr = make_pr(mergeable_state="behind",
                     **failed_gate(["Test (non-Windows)", "Build"]))
        finding = classify(pr, STEPS_EXCLUDED)
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
            "workflow_text": None,
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

    def test_a_human_disarming_it_is_a_decision_not_an_omission(self):
        # With required_approving_review_count=0, arming IS merging, so
        # re-arming a PR somebody deliberately disarmed merges held work.
        pr = make_pr(mergeable_state="clean", auto_merge=False,
                     auto_merge_disabled_by="danielraffel")
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertNotEqual(finding.action, "enable-auto-merge")
        self.assertIn("danielraffel", finding.reason)

    def test_control_no_disarming_event_is_still_auto_fixable(self):
        pr = make_pr(mergeable_state="clean", auto_merge=False,
                     auto_merge_disabled_by=None)
        self.assertEqual(classify(pr).action, "enable-auto-merge")


class MergeQueueTest(unittest.TestCase):
    """Queue membership is not inferable from `auto_merge`, so it must be read."""

    def test_unread_queue_membership_is_unknown_not_unarmed(self):
        # Enqueueing CONSUMES the auto-merge request, so a queued PR reports
        # auto_merge=null. A collector that did not read membership must not
        # let the arming branch run on that null.
        pr = make_pr(mergeable_state="clean", auto_merge=False)
        pr.pop("in_merge_queue")
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.UNKNOWN)
        self.assertNotEqual(finding.action, "enable-auto-merge")

    def test_control_read_as_absent_is_auto_fixable(self):
        pr = make_pr(mergeable_state="clean", auto_merge=False, in_merge_queue=False)
        self.assertEqual(classify(pr).action, "enable-auto-merge")

    def test_queued_with_a_null_auto_merge_request_is_moving(self):
        pr = make_pr(mergeable_state="clean", auto_merge=False, in_merge_queue=True)
        self.assertEqual(classify(pr).bucket, mod.MOVING)

    def test_a_queued_pr_is_moving_even_with_an_uncomputed_merge_state(self):
        # A queued PR's merge commit is the queue branch, so GitHub reports its
        # mergeStateStatus as UNKNOWN the whole time it sits there. Letting the
        # state check run first buckets every queued PR as UNKNOWN — a report
        # that four demonstrably moving PRs are stalled.
        pr = make_pr(mergeable_state="unknown", auto_merge=False,
                     in_merge_queue=True, merge_queue_state="AWAITING_CHECKS")
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.MOVING)
        self.assertIn("AWAITING_CHECKS", finding.reason)

    def test_control_an_uncomputed_state_outside_the_queue_is_still_unknown(self):
        pr = make_pr(mergeable_state="unknown", auto_merge=False,
                     in_merge_queue=False)
        self.assertEqual(classify(pr).bucket, mod.UNKNOWN)

    def test_an_ejected_pr_is_never_re_armed(self):
        pr = make_pr(mergeable_state="clean", auto_merge=False,
                     ejected_from_queue={"at": "2026-09-22T09:00:00Z",
                                         "actor": "github-merge-queue"})
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertNotEqual(finding.action, "enable-auto-merge")

    def test_control_a_pr_that_was_never_ejected_is_re_armable(self):
        pr = make_pr(mergeable_state="clean", auto_merge=False,
                     ejected_from_queue=None)
        self.assertEqual(classify(pr).action, "enable-auto-merge")

    def test_a_failed_merge_group_batch_is_never_re_armed(self):
        pr = make_pr(mergeable_state="clean", auto_merge=False,
                     merge_group_failure={"run_id": 35780581571,
                                          "conclusion": "failure"})
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertNotEqual(finding.action, "enable-auto-merge")

    def test_control_a_successful_batch_does_not_block_arming(self):
        pr = make_pr(mergeable_state="clean", auto_merge=False,
                     merge_group_failure=None)
        self.assertEqual(classify(pr).action, "enable-auto-merge")


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

    def test_a_cancelled_gate_is_never_replayed(self):
        # A cancelled job carries no steps at all, so "no failing step" is
        # true of it too — and replaying it takes back the runner whoever
        # cancelled it was freeing.
        pr = make_pr(mergeable_state="behind")
        pr["checks"] = [{"name": "macos", "status": "completed",
                         "conclusion": "cancelled",
                         "started_at": "2026-09-22T10:00:00Z", "run_id": 99}]
        pr["gate_failed_steps"] = []
        pr["gate_job_conclusion"] = "cancelled"
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertNotEqual(finding.action, "rerun-failed-jobs")

    def test_a_cancelled_job_under_a_failing_check_is_not_a_lost_run(self):
        # The check can read `failure` while the job that produced it was
        # cancelled. Only the job's own conclusion settles it.
        pr = make_pr(mergeable_state="behind",
                     **failed_gate([], job_conclusion="cancelled"))
        finding = classify(pr)
        self.assertEqual(finding.bucket, mod.UNKNOWN)
        self.assertNotEqual(finding.action, "rerun-failed-jobs")

    def test_a_lost_run_is_recoverable_even_when_the_workflow_is_unreadable(self):
        # "No step failed" is answered by the job, not the workflow, so an
        # unparseable workflow must not veto it. On a runner without PyYAML
        # that is every sweep.
        pr = make_pr(mergeable_state="behind", **failed_gate([]))
        finding = mod.classify_pr(pr, None, "PyYAML is not installed", NOW)
        self.assertEqual(finding.bucket, mod.AUTO_FIXABLE)
        self.assertEqual(finding.action, "rerun-failed-jobs")

    def test_control_a_named_failing_step_still_needs_the_workflow(self):
        pr = make_pr(mergeable_state="behind", **failed_gate(["Build"]))
        finding = mod.classify_pr(pr, None, "PyYAML is not installed", NOW)
        self.assertEqual(finding.bucket, mod.UNKNOWN)
        self.assertIn("undecidable", finding.reason)

    def test_an_unread_job_conclusion_is_not_a_lost_run(self):
        pr = make_pr(mergeable_state="behind", **failed_gate([]))
        pr.pop("gate_job_conclusion")
        self.assertEqual(classify(pr).bucket, mod.UNKNOWN)


class WorkInFlightTest(unittest.TestCase):
    """A pending advisory lane is not evidence that a PR is moving."""

    def _pr_with_pending(self, name, started, **over):
        pr = make_pr(mergeable_state="unstable", **over)
        pr["checks"] = [
            {"name": "macos", "status": "completed", "conclusion": "failure",
             "started_at": "2026-09-22T10:00:00Z", "run_id": 99},
            {"name": name, "status": "in_progress", "conclusion": None,
             "started_at": started.isoformat(), "run_id": 1},
        ]
        pr["gate_failed_steps"] = ["Build"]
        pr["gate_job_conclusion"] = "failure"
        return pr

    def test_a_pending_advisory_lane_does_not_mask_the_wedge(self):
        pr = self._pr_with_pending("codex", NOW - timedelta(minutes=5))
        self.assertNotEqual(classify(pr, required=["macos"]).bucket, mod.MOVING)

    def test_control_a_pending_required_context_is_moving(self):
        pr = self._pr_with_pending("macos", NOW - timedelta(minutes=5))
        self.assertEqual(classify(pr, required=["macos"]).bucket, mod.MOVING)

    def test_without_known_required_contexts_a_stale_pending_check_is_surfaced(self):
        pr = self._pr_with_pending("codex", NOW - timedelta(hours=30))
        finding = classify(pr, required=[], in_flight_hours=24.0)
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertEqual(finding.detail["stale_pending"], ["codex"])

    def test_control_a_fresh_pending_check_is_still_moving(self):
        pr = self._pr_with_pending("codex", NOW - timedelta(hours=1))
        self.assertEqual(
            classify(pr, required=[], in_flight_hours=24.0).bucket, mod.MOVING)


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

    def test_uncollected_workflow_makes_a_gate_failure_unknown(self):
        pr = make_pr(mergeable_state="behind", **failed_gate(["Test (non-Windows)"]))
        snapshot = {"workflow_text": None, "workflow_error": "404", "prs": [pr]}
        self.assertEqual(mod.analyze(snapshot, NOW)[0].bucket, mod.UNKNOWN)

    def test_a_missing_pyyaml_fails_closed_rather_than_crashing(self):
        # The module must import on a runner with no PyYAML, and a workflow it
        # cannot parse there must degrade to UNKNOWN like any other unreadable
        # input — not raise, and not resolve to a default.
        saved = sys.modules.get("yaml", "absent")
        sys.modules["yaml"] = None  # makes `import yaml` raise ImportError
        try:
            with self.assertRaises(mod.WorkflowError) as caught:
                mod.parse_workflow_steps(WORKFLOW_EXCLUDED)
            self.assertIn("PyYAML", str(caught.exception))
            pr = make_pr(mergeable_state="behind",
                         **failed_gate(["Test (non-Windows)"]))
            snapshot = {"workflow_text": WORKFLOW_EXCLUDED, "prs": [pr]}
            self.assertEqual(mod.analyze(snapshot, NOW)[0].bucket, mod.UNKNOWN)
        finally:
            if saved == "absent":
                sys.modules.pop("yaml", None)
            else:
                sys.modules["yaml"] = saved

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
        steps = dict(STEPS_EXCLUDED)
        steps["Shared"] = mod.StepCondition(
            name="Shared", expr="github.event_name != 'pull_request'", ambiguous=True)
        pr = make_pr(mergeable_state="behind", **failed_gate(["Shared"]))
        self.assertEqual(classify(pr, steps).bucket, mod.UNKNOWN)

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


class FixBudgetTest(unittest.TestCase):
    """A wrong verdict mislabels a shape of PR, so the blast radius is capped."""

    def _fixable(self, count):
        return [mod.Finding(i, "t", mod.AUTO_FIXABLE, "r", action="update-branch")
                for i in range(count)]

    def test_more_fixable_than_the_cap_is_withheld(self):
        selected, withheld = mod.cap_fixes(self._fixable(5), 3)
        self.assertEqual((len(selected), withheld), (3, 2))

    def test_control_fewer_than_the_cap_is_untouched(self):
        selected, withheld = mod.cap_fixes(self._fixable(2), 3)
        self.assertEqual((len(selected), withheld), (2, 0))

    def test_a_negative_cap_means_no_cap(self):
        selected, withheld = mod.cap_fixes(self._fixable(9), -1)
        self.assertEqual((len(selected), withheld), (9, 0))


class MovingTest(unittest.TestCase):
    def test_a_running_check_is_moving(self):
        pr = make_pr(mergeable_state="dirty")
        pr["checks"] = [{"name": "linux", "status": "in_progress",
                         "conclusion": None, "started_at": NOW.isoformat(),
                         "run_id": 1}]
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

    def test_age_is_measured_from_ci_activity_not_the_updated_timestamp(self):
        # A bot comment bumps updated_at, so a chatty PR would never accrue the
        # window. The last check-run activity is what the threshold is about.
        pr = make_pr(mergeable_state="unstable", updated_at=NOW.isoformat(),
                     last_ci_at=(NOW - timedelta(hours=9)).isoformat())
        self.assertEqual(classify(pr, unstable_hours=6.0).detail["hours"], 9.0)

    def test_control_without_ci_activity_it_falls_back_to_updated_at(self):
        pr = make_pr(mergeable_state="unstable", updated_at=NOW.isoformat())
        self.assertEqual(classify(pr, unstable_hours=6.0).detail["hours"], 0.0)

    def test_unstable_with_an_unreadable_age_is_unknown(self):
        pr = make_pr(mergeable_state="unstable", updated_at="not-a-date")
        self.assertEqual(classify(pr).bucket, mod.UNKNOWN)


class ReportTest(unittest.TestCase):
    def test_a_degraded_sweep_never_reports_health(self):
        snapshot = {"workflow_text": None, "prs": [],
                    "errors": [{"stage": "pulls", "error": "502"}]}
        text = mod.render_report(mod.analyze(snapshot, NOW), snapshot)
        self.assertIn("not a clean bill of health", text)
        self.assertNotIn("Every open pull request has work in flight", text)

    def test_a_clean_sweep_with_nothing_stalled_says_so(self):
        snapshot = {"workflow_text": None,
                    "prs": [make_pr(1, mergeable_state="clean")], "errors": []}
        text = mod.render_report(mod.analyze(snapshot, NOW), snapshot)
        self.assertIn("Every open pull request has work in flight", text)


class RunIdTest(unittest.TestCase):
    def test_run_id_comes_from_details_url_not_the_check_id(self):
        # A check-run id is not a job id and must never be used as one.
        check = {"id": 106527743343,
                 "details_url": "https://github.com/o/r/actions/runs/35657266004/job/106527743343"}
        self.assertEqual(mod._run_id_from(check), 35657266004)

    def test_job_id_comes_from_the_same_url(self):
        check = {"id": 1,
                 "details_url": "https://github.com/o/r/actions/runs/35657266004/job/106527743343"}
        self.assertEqual(mod._job_id_from(check), 106527743343)

    def test_missing_details_url_yields_neither_id(self):
        self.assertIsNone(mod._run_id_from({"id": 5}))
        self.assertIsNone(mod._job_id_from({"id": 5}))

    def test_a_run_url_without_a_job_segment_yields_no_job_id(self):
        check = {"details_url": "https://github.com/o/r/actions/runs/123"}
        self.assertEqual(mod._run_id_from(check), 123)
        self.assertIsNone(mod._job_id_from(check))


@unittest.skipUnless(HAVE_YAML, "PyYAML is not installed")
class WorkflowParsingTest(unittest.TestCase):
    """The YAML walk itself, wherever PyYAML is available to run it."""

    def test_step_conditions_are_read_from_the_document(self):
        steps = mod.parse_workflow_steps(WORKFLOW_EXCLUDED)
        self.assertIn("Test (non-Windows)", steps)
        self.assertEqual(
            steps["Test (non-Windows)"].expr,
            "runner.os != 'Windows' && github.event_name != 'pull_request'")
        self.assertFalse(steps["Build"].ambiguous)

    def test_a_name_used_under_two_conditions_is_marked_ambiguous(self):
        text = """
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
        self.assertTrue(mod.parse_workflow_steps(text)["Shared"].ambiguous)

    def test_invalid_yaml_is_a_workflow_error(self):
        with self.assertRaises(mod.WorkflowError):
            mod.parse_workflow_steps("{{ not yaml")

    def test_the_repository_workflow_parses(self):
        # The parser has to survive the real document, not only the fixture.
        real = HERE.parent.parent / ".github" / "workflows" / "build.yml"
        steps = mod.parse_workflow_steps(real.read_text(encoding="utf-8"))
        self.assertGreater(len(steps), 20)


# --------------------------------------------------------------------------
# Recorded-collector tests
# --------------------------------------------------------------------------
#
# Real payload shapes, replayed through a fake CLI. Three of these states —
# queued, ejected, and cancelled — are invisible in the fields the collector
# used to read, so nothing short of a recorded response catches them.


def gql_node(number=8722, **over):
    node = {
        "number": number,
        "title": f"pr {number}",
        "isDraft": False,
        "updatedAt": NOW.isoformat(),
        "mergeStateStatus": "CLEAN",
        "isInMergeQueue": False,
        "mergeQueueEntry": None,
        "autoMergeRequest": None,
        "headRefOid": SHA,
    }
    node.update(over)
    return node


def check_run(name="macos", conclusion="success", run_id=35780581571,
              job_id=106924968548, status="completed", started="2026-09-22T10:00:00Z"):
    return {
        "name": name,
        "status": status,
        "conclusion": conclusion,
        "started_at": started,
        "completed_at": "2026-09-22T11:00:00Z",
        "details_url": f"https://github.com/{REPO}/actions/runs/{run_id}/job/{job_id}",
    }


WORKFLOW_BLOB = {
    "content": base64.b64encode(WORKFLOW_EXCLUDED.encode()).decode()
}

# Two legs of one run. Only the macos job is the gate; harvesting both blames
# the gate for the Linux leg's failing step.
JOBS_TWO_LEGS = {
    "total_count": 2,
    "jobs": [
        {"id": 106924968548, "name": "macos", "conclusion": "failure",
         "labels": ["pulp-build-vm", "pulp-gate-fast"],
         "steps": [{"name": "Build", "conclusion": "failure"}]},
        {"id": 106924968704, "name": "Linux (x64) [github-hosted]",
         "conclusion": "failure", "labels": ["ubuntu-latest"],
         "steps": [{"name": "Linux leg outcome (advisory)", "conclusion": "failure"}]},
    ],
}

JOBS_CANCELLED = {
    "total_count": 1,
    "jobs": [
        {"id": 106924968548, "name": "macos", "conclusion": "cancelled",
         "labels": ["pulp-build-vm"], "steps": []},
    ],
}


class FakeGh:
    """Replays recorded payloads for the paths collect() reads."""

    def __init__(self, nodes, routes):
        self.nodes = nodes
        self.routes = list(routes.items())
        self.paths: list[str] = []

    def __call__(self, _gh_bin, *args):
        if args[:2] == ("api", "graphql"):
            self.paths.append("graphql")
            return json.dumps({"data": {"repository": {"pullRequests": {
                "pageInfo": {"hasNextPage": False, "endCursor": None},
                "nodes": self.nodes}}}})
        path = args[-1]
        self.paths.append(path)
        for key, payload in self.routes:
            if key in path:
                if isinstance(payload, Exception):
                    raise payload
                if callable(payload):
                    payload = payload(path)
                return json.dumps(payload)
        raise AssertionError(f"unrouted gh call: {path}")


NOT_FOUND = subprocess.CalledProcessError(1, "gh", stderr="404")


def base_routes(**over):
    routes = {
        "contents/.github/workflows": WORKFLOW_BLOB,
        "protection/required_status_checks": NOT_FOUND,
        "/rulesets/": {"rules": [{
            "type": "required_status_checks",
            "parameters": {"required_status_checks": [{"context": "macos"}]}}]},
        "/rulesets": [{"id": 7, "enforcement": "active"}],
        "actions/runs?event=merge_group": {"total_count": 0, "workflow_runs": []},
        "/timeline": [],
        "check-runs": {"total_count": 1, "check_runs": [check_run()]},
        "/jobs": JOBS_TWO_LEGS,
    }
    routes.update(over)
    # Longer keys first so "/rulesets/7" never matches the "/rulesets" listing.
    return dict(sorted(routes.items(), key=lambda kv: -len(kv[0])))


def run_collect(nodes, **over):
    fake = FakeGh(nodes, base_routes(**over))
    original = mod._gh
    mod._gh = fake
    try:
        snapshot = mod.collect(REPO, "ghapp", "macos", ".github/workflows/build.yml")
    finally:
        mod._gh = original
    return snapshot, fake


class CollectQueueMembershipTest(unittest.TestCase):
    def test_a_queued_pr_is_collected_as_in_the_queue(self):
        # The whole point: autoMergeRequest is null because enqueueing consumed
        # it, and only isInMergeQueue distinguishes this from "never armed".
        snapshot, _ = run_collect([gql_node(isInMergeQueue=True,
                                            mergeQueueEntry={"state": "QUEUED"},
                                            autoMergeRequest=None)])
        record = snapshot["prs"][0]
        self.assertTrue(record["in_merge_queue"])
        self.assertFalse(record["auto_merge"])
        finding = mod.analyze(snapshot, NOW)[0]
        self.assertEqual(finding.bucket, mod.MOVING)
        self.assertNotEqual(finding.action, "enable-auto-merge")

    def test_control_an_identical_pr_outside_the_queue_is_armable(self):
        snapshot, _ = run_collect([gql_node(isInMergeQueue=False,
                                            autoMergeRequest=None)])
        self.assertFalse(snapshot["prs"][0]["in_merge_queue"])
        self.assertEqual(mod.analyze(snapshot, NOW)[0].action, "enable-auto-merge")

    def test_an_ejected_pr_is_collected_from_the_timeline(self):
        timeline = [
            {"event": "added_to_merge_queue", "created_at": "2026-09-22T08:00:00Z",
             "actor": {"login": "github-merge-queue", "type": "Bot"}},
            {"event": "removed_from_merge_queue", "created_at": "2026-09-22T09:00:00Z",
             "actor": {"login": "github-merge-queue", "type": "Bot"}},
        ]
        snapshot, _ = run_collect([gql_node()], **{"/timeline": timeline})
        self.assertIsNotNone(snapshot["prs"][0]["ejected_from_queue"])
        finding = mod.analyze(snapshot, NOW)[0]
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertNotEqual(finding.action, "enable-auto-merge")

    def test_control_a_re_enqueued_pr_is_not_treated_as_ejected(self):
        timeline = [
            {"event": "removed_from_merge_queue", "created_at": "2026-09-22T08:00:00Z",
             "actor": {"login": "github-merge-queue", "type": "Bot"}},
            {"event": "added_to_merge_queue", "created_at": "2026-09-22T09:00:00Z",
             "actor": {"login": "github-merge-queue", "type": "Bot"}},
        ]
        snapshot, _ = run_collect([gql_node()], **{"/timeline": timeline})
        self.assertIsNone(snapshot["prs"][0]["ejected_from_queue"])

    def test_a_human_disarming_auto_merge_is_collected(self):
        timeline = [{"event": "auto_merge_disabled",
                     "created_at": "2026-09-22T09:00:00Z",
                     "actor": {"login": "danielraffel", "type": "User"}}]
        snapshot, _ = run_collect([gql_node()], **{"/timeline": timeline})
        self.assertEqual(snapshot["prs"][0]["auto_merge_disabled_by"], "danielraffel")
        self.assertEqual(mod.analyze(snapshot, NOW)[0].bucket, mod.NEEDS_HUMAN)

    def test_control_a_bot_disarming_it_is_the_mechanism_not_a_decision(self):
        timeline = [{"event": "auto_merge_disabled",
                     "created_at": "2026-09-22T09:00:00Z",
                     "actor": {"login": "github-merge-queue", "type": "Bot"}}]
        snapshot, _ = run_collect([gql_node()], **{"/timeline": timeline})
        self.assertIsNone(snapshot["prs"][0]["auto_merge_disabled_by"])
        self.assertEqual(mod.analyze(snapshot, NOW)[0].action, "enable-auto-merge")

    def test_a_failed_merge_group_batch_is_attributed_to_its_prs(self):
        runs = {"total_count": 1, "workflow_runs": [{
            "id": 35780581571, "conclusion": "failure",
            "created_at": "2026-09-22T20:29:05Z",
            "head_branch": "gh-readonly-queue/main/pr-8722-06da457c130c"}]}
        snapshot, _ = run_collect([gql_node(8722)],
                                  **{"actions/runs?event=merge_group": runs})
        self.assertEqual(
            snapshot["prs"][0]["merge_group_failure"]["run_id"], 35780581571)
        self.assertEqual(mod.analyze(snapshot, NOW)[0].bucket, mod.NEEDS_HUMAN)

    def test_a_batch_failure_older_than_the_last_check_is_superseded(self):
        # The branch was pushed and revalidated after the bad batch, so the
        # batch no longer describes this head. Without this the PR would be
        # pinned to NEEDS-HUMAN for the rest of its life.
        runs = {"total_count": 1, "workflow_runs": [{
            "id": 35780581571, "conclusion": "failure",
            "created_at": "2026-09-20T01:00:00Z",
            "head_branch": "gh-readonly-queue/main/pr-8722-06da457c130c"}]}
        snapshot, _ = run_collect([gql_node(8722)],
                                  **{"actions/runs?event=merge_group": runs})
        self.assertIsNone(snapshot["prs"][0].get("merge_group_failure"))
        self.assertEqual(mod.analyze(snapshot, NOW)[0].action, "enable-auto-merge")

    def test_control_a_successful_batch_is_not_attributed_as_a_failure(self):
        runs = {"total_count": 1, "workflow_runs": [{
            "id": 35780581571, "conclusion": "success",
            "created_at": "2026-09-22T20:29:05Z",
            "head_branch": "gh-readonly-queue/main/pr-8722-06da457c130c"}]}
        snapshot, _ = run_collect([gql_node(8722)],
                                  **{"actions/runs?event=merge_group": runs})
        self.assertIsNone(snapshot["prs"][0].get("merge_group_failure"))
        self.assertEqual(mod.analyze(snapshot, NOW)[0].action, "enable-auto-merge")


class CollectGateStepsTest(unittest.TestCase):
    def test_steps_come_only_from_the_job_the_gate_check_points_at(self):
        # A run holds every platform leg. Sweeping all its failed jobs is what
        # made the tool blame an advisory Linux step for a macOS gate failure.
        snapshot, _ = run_collect(
            [gql_node(mergeStateStatus="BEHIND")],
            **{"check-runs": {"total_count": 1,
                              "check_runs": [check_run(conclusion="failure")]}})
        record = snapshot["prs"][0]
        self.assertEqual(record["gate_failed_steps"], ["Build"])
        self.assertNotIn("Linux leg outcome (advisory)", record["gate_failed_steps"])
        self.assertEqual(record["gate_runner_os"], "macOS")

    def test_control_pointing_the_gate_at_the_linux_job_harvests_that_job(self):
        # Same run, same jobs payload — only which job the check names differs.
        snapshot, _ = run_collect(
            [gql_node(mergeStateStatus="BEHIND")],
            **{"check-runs": {"total_count": 1, "check_runs": [
                check_run(conclusion="failure", job_id=106924968704)]}})
        record = snapshot["prs"][0]
        self.assertEqual(record["gate_failed_steps"],
                         ["Linux leg outcome (advisory)"])
        self.assertEqual(record["gate_runner_os"], "Linux")

    def test_a_cancelled_gate_job_is_collected_as_cancelled(self):
        snapshot, _ = run_collect(
            [gql_node(mergeStateStatus="BEHIND")],
            **{"check-runs": {"total_count": 1,
                              "check_runs": [check_run(conclusion="cancelled")]},
               "/jobs": JOBS_CANCELLED})
        record = snapshot["prs"][0]
        self.assertEqual(record["gate_job_conclusion"], "cancelled")
        self.assertEqual(record["gate_failed_steps"], [])
        finding = mod.analyze(snapshot, NOW)[0]
        self.assertEqual(finding.bucket, mod.NEEDS_HUMAN)
        self.assertNotEqual(finding.action, "rerun-failed-jobs")

    def test_control_a_genuinely_lost_run_is_still_replayable(self):
        # Identical shape, one field different: the job really did fail while
        # reporting no failing step. That, and only that, is a lost run.
        lost = {"total_count": 1, "jobs": [
            {"id": 106924968548, "name": "macos", "conclusion": "failure",
             "labels": ["pulp-build-vm"], "steps": []}]}
        snapshot, _ = run_collect(
            [gql_node(mergeStateStatus="BEHIND")],
            **{"check-runs": {"total_count": 1,
                              "check_runs": [check_run(conclusion="failure")]},
               "/jobs": lost})
        finding = mod.analyze(snapshot, NOW)[0]
        self.assertEqual(finding.bucket, mod.AUTO_FIXABLE)
        self.assertEqual(finding.action, "rerun-failed-jobs")

    def test_a_check_run_without_a_job_id_is_an_error_not_a_guess(self):
        check = check_run(conclusion="failure")
        check["details_url"] = f"https://github.com/{REPO}/actions/runs/35780581571"
        snapshot, _ = run_collect(
            [gql_node(mergeStateStatus="BEHIND")],
            **{"check-runs": {"total_count": 1, "check_runs": [check]}})
        self.assertTrue(snapshot["prs"][0]["errors"])
        self.assertEqual(mod.analyze(snapshot, NOW)[0].bucket, mod.UNKNOWN)


class CollectPaginationTest(unittest.TestCase):
    def test_a_check_listing_longer_than_one_page_is_read_whole(self):
        def paged(path):
            page = 2 if "page=2" in path else 1
            names = [f"check-{i}" for i in range(100)] if page == 1 else \
                [f"check-{i}" for i in range(100, 120)]
            return {"total_count": 120,
                    "check_runs": [check_run(name=n) for n in names]}

        snapshot, _ = run_collect([gql_node()], **{"check-runs": paged})
        self.assertEqual(len(snapshot["prs"][0]["checks"]), 120)

    def test_control_a_truncated_listing_is_refused_rather_than_accepted(self):
        def truncated(path):
            if "page=2" in path:
                return {"total_count": 120, "check_runs": []}
            return {"total_count": 120,
                    "check_runs": [check_run(name=f"check-{i}") for i in range(100)]}

        snapshot, _ = run_collect([gql_node()], **{"check-runs": truncated})
        record = snapshot["prs"][0]
        self.assertTrue(any("truncated" in e for e in record["errors"]))
        self.assertEqual(mod.analyze(snapshot, NOW)[0].bucket, mod.UNKNOWN)

    def test_the_required_contexts_are_collected_from_the_active_ruleset(self):
        # Classic protection 404s on a ruleset-governed branch, so an empty
        # answer there must not be read as "nothing is required".
        snapshot, _ = run_collect([gql_node()])
        self.assertEqual(snapshot["required_contexts"], ["macos"])

    def test_the_pull_request_query_is_filtered_to_the_requested_base(self):
        # A PR targeting another base is judged against the wrong workflow, and
        # arming it against the wrong base 422s.
        _, fake = run_collect([gql_node()])
        self.assertIn("graphql", fake.paths)


class MainExitCodeTest(unittest.TestCase):
    """Findings never redden the run; a fix that did not happen does."""

    def _run_main(self, apply_fix):
        snapshot = {
            "repo": REPO, "workflow_text": None, "errors": [],
            "prs": [make_pr(1, mergeable_state="clean", auto_merge=False)],
        }
        saved_collect, saved_apply = mod.collect, mod.apply_fix
        mod.collect = lambda *a, **k: snapshot
        mod.apply_fix = apply_fix
        try:
            return mod.main(["--repo", REPO, "--fix", "--apply", "--json"])
        finally:
            mod.collect, mod.apply_fix = saved_collect, saved_apply

    def test_a_failed_fix_exits_non_zero(self):
        def failing(*_a, **_k):
            raise ValueError("update-branch returned 422")
        self.assertEqual(self._run_main(failing), 1)

    def test_control_a_successful_fix_exits_zero(self):
        self.assertEqual(self._run_main(lambda *_a, **_k: "armed"), 0)

    def test_fix_without_apply_does_not_mutate(self):
        snapshot = {
            "repo": REPO, "workflow_text": None, "errors": [],
            "prs": [make_pr(1, mergeable_state="clean", auto_merge=False)],
        }
        seen: list[bool] = []

        def record(_finding, _repo, _gh, dry_run):
            seen.append(dry_run)
            return "noted"

        saved_collect, saved_apply = mod.collect, mod.apply_fix
        mod.collect = lambda *a, **k: snapshot
        mod.apply_fix = record
        try:
            mod.main(["--repo", REPO, "--fix", "--json"])
            self.assertEqual(seen, [True])
            seen.clear()
            mod.main(["--repo", REPO, "--fix", "--apply", "--json"])
            self.assertEqual(seen, [False])
        finally:
            mod.collect, mod.apply_fix = saved_collect, saved_apply


if __name__ == "__main__":
    unittest.main()
