#!/usr/bin/env python3
"""Tests for the base-poison detector's decision rules.

The fixtures are the shapes observed on Generous-Corp/pulp on 2026-09-27:
three consecutive genuinely-executed merge_group batches failing the same three
tests at three DIFFERENT bases, and a landing batch whose `macos` check reported
success in three steps because a protected receipt was reused, so main's own
tree had never had its suite run.

Those two together are the whole difficulty. The streak looks like proof and is
not; the green landing batch looks like health and measured nothing. Every test
here pins a refusal as hard as a finding, because a wrong `poisoned` pauses the
entire merge queue.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import base_poison_detector as bp  # noqa: E402


def batch(
    run_id: str,
    conclusion: str = "failure",
    tests: tuple[str, ...] = (),
    execution: str = bp.EXECUTED,
    base_sha: str = "",
) -> bp.SuiteObservation:
    return bp.SuiteObservation(
        run_id=run_id,
        lane="batch",
        conclusion=conclusion,
        execution=execution,
        failing_tests=tests,
        base_sha=base_sha or f"base-{run_id}",
    )


def main_obs(
    conclusion: str = "failure",
    tests: tuple[str, ...] = (),
    execution: str = bp.EXECUTED,
    source: str = bp.SOURCE_TREE_IDENTITY,
) -> bp.SuiteObservation:
    return bp.SuiteObservation(
        run_id="main-run",
        lane="main",
        conclusion=conclusion,
        execution=execution,
        failing_tests=tests,
        source=source,
    )


# The live streak: same three tests, three consecutive executed batches, three
# different bases. Marking this poisoned would exonerate an entry whose own
# follow-up commit admitted its head was broken.
SHARED = ("consumption-census-drift", "rack-generator-safety")
LIVE_STREAK = [
    batch("36293686606", tests=SHARED + ("pulp-browser-capture-node-unit",)),
    batch("36292427299", tests=SHARED),
    batch("36292117312", tests=SHARED + ("pulp-browser-capture-node-unit",)),
]

# The landing batch that produced main's current tree: green, three steps, no
# suite. `gate_suite_executed` classifies it NOT_EXECUTED.
RECEIPT_REUSE_JOB = [
    {"name": "Set up job", "conclusion": "success"},
    {"name": "macOS merge-group bootstrap", "conclusion": "success"},
    {"name": "Complete job", "conclusion": "success"},
]


class EvidenceTests(unittest.TestCase):
    def test_receipt_reuse_green_is_not_evidence(self) -> None:
        """A three-step green reports success having run nothing."""
        self.assertEqual(bp.classify_steps(RECEIPT_REUSE_JOB), bp.NOT_EXECUTED)
        seen = main_obs(conclusion="success", execution=bp.NOT_EXECUTED)
        self.assertFalse(seen.is_evidence)
        self.assertFalse(seen.passed)

    def test_cancelled_executed_run_is_not_evidence(self) -> None:
        """Cancelling doomed batches must not be able to erase a red base."""
        self.assertFalse(batch("1", conclusion="cancelled").is_evidence)

    def test_built_but_untested_is_not_evidence(self) -> None:
        self.assertFalse(
            batch("1", conclusion="success", execution=bp.BUILT_BUT_UNTESTED).is_evidence
        )

    def test_executed_failure_is_evidence(self) -> None:
        seen = batch("1", tests=("t",))
        self.assertTrue(seen.is_evidence)
        self.assertTrue(seen.failed)


class StreakTests(unittest.TestCase):
    def test_consecutive_executed_failures_share_their_tests(self) -> None:
        streak = bp.failure_streak(LIVE_STREAK)
        self.assertEqual(streak.length, 3)
        self.assertEqual(streak.shared_tests, tuple(sorted(SHARED)))
        self.assertEqual(len(streak.distinct_bases), 3)

    def test_an_executed_pass_breaks_the_streak(self) -> None:
        runs = [batch("1", tests=("t",)), batch("2", conclusion="success"), batch("3", tests=("t",))]
        self.assertEqual(bp.failure_streak(runs).length, 1)

    def test_non_evidence_runs_are_skipped_not_treated_as_a_break(self) -> None:
        """Otherwise cancelling a batch would hide the streak it belongs to."""
        runs = [
            batch("1", tests=("t",)),
            batch("2", conclusion="cancelled"),
            batch("3", tests=("t",)),
        ]
        streak = bp.failure_streak(runs)
        self.assertEqual(streak.length, 2)
        self.assertEqual(streak.skipped_non_evidence, 1)
        self.assertEqual(streak.shared_tests, ("t",))

    def test_a_failure_naming_no_test_empties_the_shared_set(self) -> None:
        """A compile or link error names nothing, so nothing may be named for it."""
        runs = [batch("1", tests=("t",)), batch("2", tests=())]
        streak = bp.failure_streak(runs)
        self.assertEqual(streak.length, 2)
        self.assertEqual(streak.shared_tests, ())


class DetectTests(unittest.TestCase):
    def test_streak_alone_is_suspected_and_must_not_pause_the_queue(self) -> None:
        verdict = bp.detect(None, LIVE_STREAK)
        self.assertEqual(verdict.status, bp.STATUS_SUSPECTED)
        self.assertEqual(verdict.proof, bp.PROOF_STREAK_ONLY)
        self.assertFalse(verdict.safe_to_pause_queue)
        self.assertEqual(verdict.tests, tuple(sorted(SHARED)))

    def test_main_failure_plus_streak_is_poisoned(self) -> None:
        verdict = bp.detect(main_obs(tests=SHARED), LIVE_STREAK)
        self.assertEqual(verdict.status, bp.STATUS_POISONED)
        self.assertEqual(verdict.proof, bp.PROOF_MAIN_AND_STREAK)
        self.assertTrue(verdict.safe_to_pause_queue)
        self.assertEqual(verdict.tests, tuple(sorted(SHARED)))

    def test_poisoned_names_only_tests_main_and_the_streak_agree_on(self) -> None:
        verdict = bp.detect(main_obs(tests=("rack-generator-safety", "other")), LIVE_STREAK)
        self.assertEqual(verdict.status, bp.STATUS_POISONED)
        self.assertEqual(verdict.tests, ("rack-generator-safety",))

    def test_main_failure_without_a_streak_is_suspected_not_poisoned(self) -> None:
        verdict = bp.detect(main_obs(tests=SHARED), [batch("1", tests=SHARED)])
        self.assertEqual(verdict.status, bp.STATUS_SUSPECTED)
        self.assertEqual(verdict.proof, bp.PROOF_MAIN_ONLY)
        self.assertFalse(verdict.safe_to_pause_queue)

    def test_main_failure_naming_no_test_cannot_be_poisoned(self) -> None:
        verdict = bp.detect(main_obs(tests=()), LIVE_STREAK)
        self.assertEqual(verdict.status, bp.STATUS_SUSPECTED)
        self.assertFalse(verdict.safe_to_pause_queue)
        self.assertEqual(verdict.tests, ())

    def test_a_receipt_reuse_green_on_main_is_unproven_never_healthy(self) -> None:
        verdict = bp.detect(
            main_obs(conclusion="success", execution=bp.NOT_EXECUTED), []
        )
        self.assertEqual(verdict.status, bp.STATUS_UNPROVEN)
        self.assertFalse(verdict.main_observed)

    def test_no_evidence_at_all_is_unproven_never_healthy(self) -> None:
        self.assertEqual(bp.detect(None, []).status, bp.STATUS_UNPROVEN)

    def test_executed_pass_on_main_is_healthy(self) -> None:
        verdict = bp.detect(main_obs(conclusion="success"), LIVE_STREAK)
        self.assertEqual(verdict.status, bp.STATUS_HEALTHY)
        self.assertFalse(verdict.safe_to_pause_queue)

    def test_min_streak_below_two_is_refused(self) -> None:
        """A threshold of one would call every ordinary red batch a streak."""
        with self.assertRaises(ValueError):
            bp.detect(main_obs(tests=SHARED), LIVE_STREAK, min_streak=1)

    def test_only_poisoned_authorises_pausing_the_queue(self) -> None:
        for status in (bp.STATUS_HEALTHY, bp.STATUS_UNPROVEN, bp.STATUS_SUSPECTED):
            with self.subTest(status=status):
                self.assertFalse(bp.Verdict(status=status).safe_to_pause_queue)
        self.assertTrue(bp.Verdict(status=bp.STATUS_POISONED).safe_to_pause_queue)


class SignalTests(unittest.TestCase):
    def test_signal_is_json_serialisable_and_carries_the_contract_fields(self) -> None:
        payload = bp.signal(bp.detect(main_obs(tests=SHARED), LIVE_STREAK), 8913)
        json.dumps(payload)
        self.assertEqual(payload["schema"], bp.SIGNAL_SCHEMA)
        self.assertTrue(payload["safe_to_pause_queue"])
        self.assertEqual(payload["candidate_fix_pr"], 8913)
        self.assertEqual(payload["batch_streak"], 3)
        self.assertEqual(payload["main_evidence_source"], bp.SOURCE_TREE_IDENTITY)
        for key in ("status", "proof", "tests", "reason", "min_streak"):
            self.assertIn(key, payload)

    def test_membership_culprits_are_carried_apart_from_the_fix_pr(self) -> None:
        payload = bp.signal(
            bp.detect(None, LIVE_STREAK),
            None,
            {8933: ["wavenet", "abi-baseline"]},
        )
        self.assertIsNone(payload["candidate_fix_pr"])
        self.assertEqual(
            payload["likely_culprits"],
            [{"pr": 8933, "tests": ["abi-baseline", "wavenet"]}],
        )
        lines, markdown = bp.render(payload)
        self.assertIn("| likely culprit PR | #8933 (abi-baseline, wavenet) |", markdown)
        self.assertEqual(json.loads(lines[0].split("::", 2)[2])["likely_culprits"][0]["pr"], 8933)

    def test_no_membership_finding_is_an_empty_list_and_a_dash(self) -> None:
        payload = bp.signal(bp.detect(None, LIVE_STREAK))
        self.assertEqual(payload["likely_culprits"], [])
        self.assertIn("| likely culprit PR | — |", bp.render(payload)[1])

    def test_a_macos_only_fallback_is_announced_not_passed_off_as_complete(self) -> None:
        payload = bp.signal(
            bp.detect(None, LIVE_STREAK), None, {}, bp.REQUIRED_SOURCE_FALLBACK
        )
        self.assertEqual(payload["required_contexts_source"], "macos-only-fallback")
        lines, markdown = bp.render(payload)
        self.assertIn("**macos only**", markdown)
        self.assertEqual(
            json.loads(lines[0].split("::", 2)[2])["required_contexts_source"],
            "macos-only-fallback",
        )

    def test_a_full_required_set_says_where_it_came_from(self) -> None:
        payload = bp.signal(
            bp.detect(None, LIVE_STREAK), None, {}, bp.REQUIRED_SOURCE_PROTECTION
        )
        self.assertIn("every required context", bp.render(payload)[1])
        self.assertNotIn("macos only", bp.render(payload)[1])

    def test_the_fallback_reaches_the_signal_from_an_unreadable_protection(self) -> None:
        from unittest import mock

        import queue_batch_attribute as attributor

        read = mock.Mock(observations=[], required_unread=True)
        with mock.patch.object(attributor, "observe_history", return_value=read):
            culprits, source = bp.membership_culprits("o/r", "24h", 60)
        self.assertEqual((culprits, source), ({}, bp.REQUIRED_SOURCE_FALLBACK))
        read.required_unread = False
        with mock.patch.object(attributor, "observe_history", return_value=read):
            self.assertEqual(
                bp.membership_culprits("o/r", "24h", 60)[1],
                bp.REQUIRED_SOURCE_PROTECTION,
            )

    def test_annotation_is_one_line_and_titled_for_its_reader(self) -> None:
        lines, markdown = bp.render(bp.signal(bp.detect(None, LIVE_STREAK)))
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith(f"::notice title={bp.SIGNAL_TITLE}::"))
        self.assertNotIn("\n", lines[0])
        payload = json.loads(lines[0].split("::", 2)[2])
        self.assertEqual(payload["status"], bp.STATUS_SUSPECTED)
        self.assertIn("suspected", markdown)

    def test_a_suspected_summary_says_it_is_not_proof(self) -> None:
        _, markdown = bp.render(bp.signal(bp.detect(None, LIVE_STREAK)))
        self.assertIn("not proof", markdown)

    def test_annotation_escapes_newlines_in_the_reason(self) -> None:
        verdict = bp.detect(None, LIVE_STREAK)
        verdict.reason = "line one\nline two"
        lines, _ = bp.render(bp.signal(verdict))
        self.assertNotIn("\n", lines[0])


class LastTestsFailedTests(unittest.TestCase):
    # The real file from run 36292117312's ctest-logs-macos artifact.
    LIVE = (
        "21273:pulp-browser-capture-node-unit\n"
        "21302:rack-generator-safety\n"
        "21314:consumption-census-drift\n"
        "21316:consumption-census-negative-contract\n"
    )

    def test_parses_ctest_index_colon_name(self) -> None:
        self.assertEqual(
            bp.parse_last_tests_failed(self.LIVE),
            (
                "pulp-browser-capture-node-unit",
                "rack-generator-safety",
                "consumption-census-drift",
                "consumption-census-negative-contract",
            ),
        )

    def test_empty_log_names_nothing(self) -> None:
        """A failure before ctest ran names no test, and must not invent one."""
        self.assertEqual(bp.parse_last_tests_failed(""), ())
        self.assertEqual(bp.parse_last_tests_failed("\n  \n"), ())

    def test_names_with_spaces_and_dashes_survive(self) -> None:
        self.assertEqual(
            bp.parse_last_tests_failed("7:applies_if - empty expression\n"),
            ("applies_if - empty expression",),
        )

    def test_duplicate_names_are_reported_once(self) -> None:
        self.assertEqual(bp.parse_last_tests_failed("1:t\n2:t\n"), ("t",))


class MacosJobNameTests(unittest.TestCase):
    def test_both_spellings_of_the_macos_leg_match(self) -> None:
        """`push` keeps the descriptive matrix name; the gate events rename it."""
        for name in ("macos", "macOS (ARM64) [local]", "macOS (arm64) [github-hosted]"):
            with self.subTest(name=name):
                self.assertTrue(bp.MACOS_JOB_RE.match(name))

    def test_unrelated_legs_do_not_match(self) -> None:
        for name in ("linux", "windows", "Linux (x64) [github-hosted]", "macos-pr-unused"):
            with self.subTest(name=name):
                self.assertFalse(bp.MACOS_JOB_RE.match(name))


class BatchBaseShaTests(unittest.TestCase):
    def test_a_queue_ref_names_the_base_it_was_built_on(self) -> None:
        run = {
            "head_branch": "gh-readonly-queue/main/pr-8919-"
            "2c59049881b78980c7483010947bfb855f4f22ba",
            "head_sha": "385d2ad4dee5a044fb99d2142d885af1f752bc8a",
        }
        self.assertEqual(bp._base_sha(run), "2c59049881b78980c7483010947bfb855f4f22ba")

    def test_a_push_run_is_its_own_base(self) -> None:
        run = {"head_branch": "main", "head_sha": "abc"}
        self.assertEqual(bp._base_sha(run), "abc")


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))

import queue_batch_attribute as attributor  # noqa: E402


class DecisiveFixPrTests(unittest.TestCase):
    """Naming a fix tells a queue which branch to prioritise, so the bar is high.

    The fixtures run the real attributor rather than hand-built scores, so the
    weights cannot drift from the ones it actually produces.
    """

    def test_an_exact_test_name_to_file_stem_match_names_the_pull_request(self) -> None:
        result = attributor.attribute(
            ["rack-generator-safety"],
            {8913: ["test/test_rack_generator_safety.py"]},
        )
        self.assertEqual(result.best_strength, attributor.WEIGHT_EXACT_STEM)
        self.assertEqual(bp.decisive_fix_pr(result), 8913)

    def test_an_incidental_token_overlap_names_nobody(self) -> None:
        """The attributor's own confidence threshold is lower than this one."""
        result = attributor.attribute(
            ["cmake-control-sdk-consumer"],
            {8913: ["test/cmake/test_gpu_audio_sdk_consumer.cmake"]},
        )
        self.assertLess(result.best_strength, bp._FIX_PR_MIN_STRENGTH)
        self.assertIsNone(bp.decisive_fix_pr(result))

    def test_a_tie_at_the_top_strength_names_nobody(self) -> None:
        """Two branches owning the same test is a coin toss, not a finding."""
        result = attributor.attribute(
            ["rack-generator-safety"],
            {
                8913: ["test/test_rack_generator_safety.py"],
                8914: ["tools/test_rack_generator_safety.py"],
            },
        )
        self.assertEqual(sorted(result.contenders), [8913, 8914])
        self.assertIsNone(bp.decisive_fix_pr(result))

    def test_no_attribution_names_nobody(self) -> None:
        self.assertIsNone(bp.decisive_fix_pr(None))
        self.assertIsNone(bp.decisive_fix_pr(attributor.attribute([], {})))

    def test_the_bar_is_the_attributors_decisive_weight(self) -> None:
        """Pinned to the attributor's constant so a re-weighting cannot silently
        lower this consumer's bar to ordinary confidence."""
        self.assertEqual(bp._FIX_PR_MIN_STRENGTH, attributor.WEIGHT_EXACT_STEM)
        self.assertGreater(bp._FIX_PR_MIN_STRENGTH, attributor.CONFIDENT)


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DETECTOR_WORKFLOW = REPO_ROOT / ".github/workflows/main-health-detector.yml"
BUILD_WORKFLOW = REPO_ROOT / ".github/workflows/build.yml"


def strip_comments(text: str) -> str:
    """Drop `#` comment text.

    Every one of these files has to EXPLAIN the thing it wires, so a raw
    substring scan finds the explanation and reads as wiring. Replacing the real
    invocation with an `echo` left an assertion on the bare path green, because
    the rationale comment names the same path.
    """
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


class WorkflowWiringTests(unittest.TestCase):
    """A decision module that is correct but unreferenced reads like one that works."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.detector = DETECTOR_WORKFLOW.read_text(encoding="utf-8")
        cls.detector_code = strip_comments(cls.detector)
        cls.build = BUILD_WORKFLOW.read_text(encoding="utf-8")

    def test_a_workflow_actually_invokes_the_detector(self) -> None:
        self.assertTrue(DETECTOR_WORKFLOW.is_file(), DETECTOR_WORKFLOW)
        self.assertIn(
            "python3 tools/ci/base_poison_detector.py",
            self.detector_code,
            "the detector path appears only in prose; nothing runs it",
        )

    def test_the_detector_lane_is_scheduled_so_it_runs_unprompted(self) -> None:
        self.assertIn("schedule:", self.detector_code)
        self.assertIn("cron:", self.detector_code)

    def test_a_failed_merge_group_run_triggers_the_detector(self) -> None:
        """The schedule is throttled to about one run in four hours, so the read
        that catches a red base within a batch's lifetime is the one a failed
        merge group triggers. The workflow name must be build.yml's own, or the
        trigger silently never fires."""
        build_name = re.search(r"^name:\s*(.+?)\s*$", self.build, re.MULTILINE)
        self.assertIsNotNone(build_name)
        on = re.search(r"^on:\n((?:[ \t]+\S.*\n|[ \t]*\n)+)", self.detector_code,
                       re.MULTILINE)
        self.assertIsNotNone(on, "the detector declares no top-level on:")
        block = on.group(1)
        self.assertRegex(block, r"(?m)^  workflow_run:\s*$")
        self.assertIn(f'workflows: ["{build_name.group(1)}"]', block)
        self.assertIn("types: [completed]", block)
        job_if = re.search(r"^    if: >-\n((?:      .*\n)+)", self.detector_code, re.MULTILINE)
        self.assertIsNotNone(job_if, "the triage job has no folded if: guard")
        guard = job_if.group(1)
        self.assertIn("github.event.workflow_run.event == 'merge_group'", guard)
        self.assertIn("github.event.workflow_run.conclusion == 'failure'", guard)
        self.assertIn("github.event_name == 'workflow_run' &&", guard)

    def test_a_workflow_run_read_never_runs_the_triggering_code(self) -> None:
        """`workflow_run` carries base-repository permissions; checking out the
        triggering run's head would hand that token to queued code."""
        self.assertNotIn("workflow_run.head", self.detector_code)
        for step in re.findall(r"uses: actions/checkout@\S+\n((?:\s{8,}.*\n)*)",
                               self.detector_code):
            self.assertNotIn("ref:", step, step)

    def test_the_detector_token_is_read_only(self) -> None:
        perms = re.search(r"^permissions:\n((?:[ \t]+\S.*\n)+)", self.detector_code,
                          re.MULTILINE)
        self.assertIsNotNone(perms, "no top-level permissions: the token defaults wide")
        grants = dict(line.strip().split(":", 1) for line in perms.group(1).splitlines())
        self.assertEqual({k: v.strip() for k, v in grants.items()},
                         {"actions": "read", "contents": "read", "pull-requests": "read"})
        self.assertNotRegex(self.detector_code, r"(?m)^[ \t]+permissions:")  # no job widening

    def test_the_detector_group_is_shared_and_never_cancels_in_progress(self) -> None:
        """One at a time, and a superseded tick must lose nothing.

        A per-sha group would let every merge start its own detector; keying on
        the sha is exactly what makes build.yml's push lane lose an observation
        when it is superseded, because the sha it pinned never comes again.
        """
        self.assertIn("group: main-health-detector", self.detector_code)
        self.assertIn("cancel-in-progress: false", self.detector_code)
        self.assertNotIn("main-health-detector-${{", self.detector_code)

    def test_the_detector_reports_only(self) -> None:
        """Acting on the signal is Shipyard's side; this lane must not gate."""
        self.assertNotIn("--fail-on-poisoned", self.detector_code)

    def test_the_detector_does_not_draw_a_macos_gate_host(self) -> None:
        """The whole point of tree identity is costing no gate lane.

        Reads the `runs-on` selectors rather than the whole file: the rationale
        comment names the self-hosted Studios it declines to draw, and a
        whole-file scan would fail on the explanation of its own restraint.
        """
        selectors = re.findall(r"^\s*runs-on:.*$", self.detector_code, re.MULTILINE)
        self.assertTrue(selectors, "the detector declares no runs-on at all")
        joined = " ".join(selectors)
        for token in ("pulp-build-vm", "macos-15", "macos-26", "self-hosted", "macOS"):
            with self.subTest(token=token):
                self.assertNotIn(token, joined, joined)

    def test_build_yml_push_still_shares_one_concurrency_domain(self) -> None:
        """Pins the fact the detector's design is a response to.

        Scoped to the top-level `concurrency:` block, not the file: the tokens
        it checks appear six other times in build.yml, so a whole-file scan
        stayed green when the block's own push exemption was deleted.

        If this stops holding, re-measure whether the push lane now carries an
        observation and update the detector's docstring: the tree-identity route
        would become a fallback rather than the primary source.
        """
        match = re.search(
            r"^concurrency:\n((?:[ \t]+\S.*\n|[ \t]*\n)+)",
            self.build,
            re.MULTILINE,
        )
        self.assertIsNotNone(match, "build.yml declares no top-level concurrency")
        block = strip_comments(match.group(1))
        self.assertIn("group: build-${{", block, block)
        self.assertIn("github.ref", block, block)
        self.assertIn("github.event_name != 'push'", block, block)

    def test_observe_main_falls_back_past_the_push_lane(self) -> None:
        """The detector must not depend on the lane measured to report nothing."""
        source = pathlib.Path(bp.__file__).read_text(encoding="utf-8")
        self.assertIn("observe_main_push_lane(repo, limit) or observe_main_by_tree", source)

    def test_the_failing_test_source_is_the_artifact_not_the_job_log(self) -> None:
        """`ghapp api .../jobs/<id>/logs` refuses escape sequences and returns
        99 bytes, so a log scrape silently yields NO failing tests -- which
        reads as "the failure was not a test failure".

        Comment text is stripped first: the prohibition has to be explained in
        the module, and a raw substring scan would trip over the explanation.
        """
        source = pathlib.Path(bp.__file__).read_text(encoding="utf-8")
        self.assertIn("/artifacts/", source)
        code = "\n".join(
            line.split("#", 1)[0] for line in source.splitlines()
        )
        self.assertNotIn("/logs", code, "the detector reads a job log again")


if __name__ == "__main__":
    unittest.main()
