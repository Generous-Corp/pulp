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


EXECUTED_STEPS = [
    {"name": "Set up job", "conclusion": "success"},
    {"name": "Build", "conclusion": "success"},
    {"name": "Test (non-Windows)", "conclusion": "success"},
]
FAILED_TEST_STEPS = [
    {"name": "Set up job", "conclusion": "success"},
    {"name": "Build", "conclusion": "success"},
    {"name": "Test (non-Windows)", "conclusion": "failure"},
]
REQUIRED = ("macos", "Enforce version & skill sync")
TIP = "6e95d5cabf8ea7fe440b145923ddbab79b0ff7c1"


def job(name: str, conclusion: str, steps: list[dict] | None = None) -> dict:
    return {"name": name, "conclusion": conclusion, "steps": steps or EXECUTED_STEPS}


# The shape of merge_group run 36523704313 (main 6e95d5ca, 2026-09-29): the
# required `macos` job passed, advisory hosted Linux failed, and the RUN
# concluded failure because of Linux.
GREEN_GATE_RED_ADVISORY = [
    job("macos", "success"),
    job("Linux (x64) [github-hosted]", "failure"),
    job("macos-pr-unused", "skipped", []),
]


class RequiredGateTests(unittest.TestCase):
    def test_advisory_red_does_not_redden_a_green_gate(self) -> None:
        self.assertEqual(
            bp.judge_gate(GREEN_GATE_RED_ADVISORY, REQUIRED), (bp.EXECUTED, "success")
        )

    def test_a_red_gate_is_red(self) -> None:
        jobs = [job("macos", "failure", FAILED_TEST_STEPS), job("Linux (x64) [github-hosted]", "success")]
        self.assertEqual(bp.judge_gate(jobs, REQUIRED), (bp.EXECUTED, "failure"))

    def test_a_running_gate_is_not_evidence(self) -> None:
        """The queue lands on the required gate alone, so the tip's run is often
        still in progress; a job with no conclusion yet is pending, not green."""
        execution, conclusion = bp.judge_gate([job("macos", "")], REQUIRED)
        self.assertEqual(conclusion, "")

    def test_advisory_legs_are_never_gate_jobs(self) -> None:
        names = [j["name"] for j in bp.gate_jobs(GREEN_GATE_RED_ADVISORY, REQUIRED)]
        self.assertEqual(names, ["macos"])

    def test_the_descriptive_macos_leg_is_the_fallback_gate(self) -> None:
        jobs = [job("macOS (ARM64) [local]", "failure", FAILED_TEST_STEPS), job("Linux (x64) [github-hosted]", "success")]
        self.assertEqual(bp.judge_gate(jobs, REQUIRED), (bp.EXECUTED, "failure"))

    def test_required_contexts_come_from_the_shipyard_config(self) -> None:
        self.assertIn("macos", bp.required_contexts())
        self.assertNotIn("linux", bp.required_contexts())

    def test_an_unreadable_config_falls_back_to_macos(self) -> None:
        missing = pathlib.Path(__file__).with_name("no-such-config.toml")
        self.assertEqual(bp.required_contexts(missing), bp.FALLBACK_REQUIRED_CONTEXTS)


RULESET = pathlib.Path(__file__).resolve().parents[2] / ".github" / "rulesets" / "main-protection.json"
# The contexts other workflows produce. None of them is a job in build.yml.
NON_BUILD_CONTEXTS = (
    "Enforce version & skill sync",
    "Build + prove + (owner-gated) deploy",
    "Vellum freeze",
    "Vellum trusted freeze",
    "drift-fast",
)
FULL_CONTRACT = ("macos", *NON_BUILD_CONTEXTS)


def ruleset_contexts() -> set[str]:
    data = json.loads(RULESET.read_text(encoding="utf-8"))
    return {
        check["context"]
        for rule in data.get("rules", [])
        if rule.get("type") == "required_status_checks"
        for check in rule.get("parameters", {}).get("required_status_checks", [])
        if check.get("context")
    }


class GovernanceContractTests(unittest.TestCase):
    """`[governance] required_status_checks` is the full required-check contract.

    `shipyard governance apply` pushes that list to branch protection, so a list
    shorter than the ruleset mirror would silently drop required checks. And the
    detector reads the same list, so it must stay correct when the list names
    contexts build.yml does not produce.
    """

    def test_governance_equals_the_checked_in_ruleset(self) -> None:
        try:
            import tomllib  # noqa: F401
        except ImportError:  # Python < 3.11
            self.skipTest("tomllib unavailable; cannot read .shipyard/config.toml")
        governance = bp.required_contexts()
        self.assertNotEqual(governance, bp.FALLBACK_REQUIRED_CONTEXTS, "config was not read")
        self.assertEqual(len(governance), len(set(governance)), governance)
        self.assertEqual(
            set(governance),
            ruleset_contexts(),
            ".shipyard/config.toml [governance] required_status_checks must equal "
            "the contexts in .github/rulesets/main-protection.json",
        )

    def test_the_ruleset_reader_sees_the_required_contexts(self) -> None:
        # Positive control for the equality above: a reader returning nothing
        # would make an empty governance list look aligned.
        self.assertIn("macos", ruleset_contexts())
        self.assertGreater(len(ruleset_contexts()), 1)

    def test_contexts_from_other_workflows_select_no_build_job(self) -> None:
        names = [j["name"] for j in bp.gate_jobs(GREEN_GATE_RED_ADVISORY, FULL_CONTRACT)]
        self.assertEqual(names, ["macos"])

    def test_the_full_contract_judges_a_green_gate_green(self) -> None:
        self.assertEqual(
            bp.judge_gate(GREEN_GATE_RED_ADVISORY, FULL_CONTRACT), (bp.EXECUTED, "success")
        )

    def test_the_full_contract_judges_a_red_gate_red(self) -> None:
        jobs = [job("macos", "failure", FAILED_TEST_STEPS), job("Linux (x64) [github-hosted]", "success")]
        self.assertEqual(bp.judge_gate(jobs, FULL_CONTRACT), (bp.EXECUTED, "failure"))

    def test_the_full_contract_keeps_the_descriptive_macos_fallback(self) -> None:
        jobs = [job("macOS (ARM64) [local]", "failure", FAILED_TEST_STEPS)]
        self.assertEqual(bp.judge_gate(jobs, FULL_CONTRACT), (bp.EXECUTED, "failure"))


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import queue_batch_attribute as attributor  # noqa: E402


# Which workflow produces each required context (`[landability] workflows`).
CONTEXT_WORKFLOW = {
    "macos": ".github/workflows/build.yml",
    "Enforce version & skill sync": ".github/workflows/version-skill-check.yml",
    "Build + prove + (owner-gated) deploy": ".github/workflows/wclap-cloudflare.yml",
    "Vellum freeze": ".github/workflows/vellum-freeze-check.yml",
    "Vellum trusted freeze": ".github/workflows/vellum-trusted-gate.yml",
    "drift-fast": ".github/workflows/drift-fast.yml",
}
WORKFLOWS = tuple(dict.fromkeys(CONTEXT_WORKFLOW.values()))
ALL_GREEN = {name: "success" for name in FULL_CONTRACT}

# A real `ctest-logs-macos` member: merge_group run 36545528395 (2026-09-29),
# artifact 11023836286, Testing/Temporary/LastTestsFailed.log, byte for byte.
REAL_MACOS_LAST_TESTS_FAILED = (
    "18273:materialized paint keeps the full host frame around an authored panel\n"
)

# A real excerpt of the `drift-fast` job log from merge_group run 36590722705
# (main 4e0e8354, 2026-09-29, the fourteen-hour required-context red). The job
# uploads no ctest artifact, so this block is the only record of what failed.
REAL_DRIFT_FAST_LOG = """\
2026-09-29T15:35:33.6196453Z  40/104 Test #1333: gpu-probe-historical-v1-acceptance ..........................***Failed    0.14 sec
2026-09-29T15:35:33.6328426Z 104/104 Test #1617: wide-non-native-selftest ....................................   Passed   30.75 sec
2026-09-29T15:35:33.6328776Z 
2026-09-29T15:35:33.6328885Z 98% tests passed, 2 tests failed out of 104
2026-09-29T15:35:33.6339022Z \t1604 - consumption-census-negative-contract (Skipped)
2026-09-29T15:35:33.6339670Z 
2026-09-29T15:35:33.6339760Z The following tests FAILED:
2026-09-29T15:35:33.6340081Z \t1333 - gpu-probe-historical-v1-acceptance (Failed)       pr-fast
2026-09-29T15:35:33.6340529Z \t1603 - consumption-census-drift-description (Failed)     pr-fast
2026-09-29T15:35:33.6340959Z drift-fast: NOT CHECKED (skipped): consumption-census-drift
2026-09-29T15:35:33.6342888Z drift-fast: FAILED
"""
DRIFT_TESTS = ("gpu-probe-historical-v1-acceptance", "consumption-census-drift-description")


def _zip(members: dict[str, str]) -> bytes:
    import io
    import zipfile

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, text in members.items():
            archive.writestr(name, text)
    return buffer.getvalue()


class FakeGitHub:
    """Serves `gh api` reads for merge-group heads from per-head fixtures.

    Each head gets one merge_group run per producing workflow. build.yml's jobs
    come from the jobs endpoint (with steps); every other context is a check run
    in its workflow run's check suite; statuses come from the combined status.
    """

    def __init__(self, tip: str = TIP) -> None:
        self.tip = tip
        self.runs: list[dict] = []
        self.jobs: dict[str, list[dict]] = {}
        self.check_runs: dict[str, list[dict]] = {}
        self.statuses: dict[str, list[dict]] = {}
        self.artifacts: dict[str, str] = {}  # run id -> LastTestsFailed.log text
        self.logs: dict[str, str] = {}  # run id -> drift-fast job log text
        self.paths: list[str] = []
        self._next = 1000

    def head(
        self,
        sha: str,
        contexts: dict[str, str],
        macos_steps: list[dict] | None = None,
        created_at: str = "2026-09-29T04:59:00Z",
        statuses: dict[str, str] | None = None,
        extra_build_jobs: list[dict] | None = None,
        push_check_runs: list[dict] | None = None,
    ) -> dict[str, str]:
        """Add a head; returns context -> run id."""
        run_ids: dict[str, str] = {}
        by_workflow: dict[str, dict] = {}
        for context, conclusion in contexts.items():
            path = CONTEXT_WORKFLOW[context]
            run = by_workflow.get(path)
            if run is None:
                self._next += 1
                run = {
                    "id": self._next,
                    "path": path,
                    "head_sha": sha,
                    "head_branch": f"gh-readonly-queue/main/pr-9{self._next}-{'b' * 40}",
                    "status": "completed" if conclusion else "in_progress",
                    "conclusion": conclusion or None,
                    "check_suite_id": 50_000 + self._next,
                    "created_at": created_at,
                }
                by_workflow[path] = run
                self.runs.append(run)
            run_ids[context] = str(run["id"])
            if path == bp.BUILD_WORKFLOW:
                steps = macos_steps
                if steps is None:
                    steps = FAILED_TEST_STEPS if conclusion == "failure" else EXECUTED_STEPS
                self.jobs.setdefault(str(run["id"]), []).append(
                    job(context, conclusion, steps)
                )
            else:
                self.check_runs.setdefault(sha, []).append(
                    {
                        "id": self._next * 10,
                        "name": context,
                        "status": "completed" if conclusion else "in_progress",
                        "conclusion": conclusion or None,
                        "check_suite": {"id": run["check_suite_id"]},
                    }
                )
        build = by_workflow.get(bp.BUILD_WORKFLOW)
        if build is not None:
            self.jobs[str(build["id"])].extend(
                extra_build_jobs
                if extra_build_jobs is not None
                else [job("Linux (x64) [github-hosted]", "failure")]
            )
        self.check_runs.setdefault(sha, []).extend(push_check_runs or [])
        self.statuses[sha] = [
            {"context": name, "state": state} for name, state in (statuses or {}).items()
        ]
        return run_ids

    def gh(self, path: str, jq: str | None = None) -> str | None:
        self.paths.append(path)
        if path.endswith("/commits/main"):
            return f"{self.tip}\ttree-{self.tip}"
        match = re.search(r"/commits/([0-9a-f]+)/check-runs", path)
        if match:
            return json.dumps({"check_runs": self.check_runs.get(match.group(1), [])})
        match = re.search(r"/commits/([0-9a-f]+)/status", path)
        if match:
            return json.dumps({"statuses": self.statuses.get(match.group(1), [])})
        match = re.search(r"/commits/([0-9a-f]+)$", path)
        if match:
            return f"tree-{match.group(1)}"
        match = re.search(r"/actions/runs/(\d+)/jobs", path)
        if match:
            return json.dumps({"jobs": self.jobs.get(match.group(1), [])})
        match = re.search(r"/actions/runs/(\d+)/artifacts", path)
        if match:
            return "7" + match.group(1) if match.group(1) in self.artifacts else ""
        if "/actions/runs?" in path:
            head = re.search(r"head_sha=([0-9a-f]+)", path)
            runs = [r for r in self.runs if not head or r["head_sha"] == head.group(1)]
            runs = sorted(runs, key=lambda r: (r["created_at"], r["id"]), reverse=True)
            if "page=2" in path:
                runs = []
            return json.dumps({"workflow_runs": runs})
        return None

    def gh_bytes(self, path: str) -> bytes | None:
        match = re.search(r"/artifacts/7(\d+)/zip", path)
        if not match or match.group(1) not in self.artifacts:
            return None
        return _zip({bp.LAST_TESTS_FAILED_MEMBER: self.artifacts[match.group(1)]})

    def run_log_zip(self, repo: str, run_id: str) -> bytes | None:
        text = self.logs.get(str(run_id))
        return _zip({"0_drift-fast.txt": text, "drift-fast/system.txt": ""}) if text else None

    def patched(self):
        from contextlib import ExitStack
        from unittest import mock

        stack = ExitStack()
        stack.enter_context(mock.patch.object(bp, "gh", self.gh))
        stack.enter_context(mock.patch.object(bp, "gh_bytes", self.gh_bytes))
        stack.enter_context(mock.patch.object(attributor, "run_log_zip", self.run_log_zip))
        return stack


def detect_live(fake: FakeGitHub, required=FULL_CONTRACT, batch_limit: int = 12,
                min_streak: int = 2) -> bp.Verdict:
    with fake.patched():
        main, tip = bp.observe_main("o/r", required=required, workflows=WORKFLOWS)
        batches = bp.observe_batches("o/r", batch_limit, required, WORKFLOWS)
    return bp.detect(main, batches, min_streak=min_streak, tip_sha=tip)


class MainTipObservationTests(unittest.TestCase):
    """main's tip is judged by EVERY required context on its own merge group."""

    def test_all_green_is_healthy(self) -> None:
        """Control: every required context green and the suite ran."""
        fake = FakeGitHub()
        fake.head(TIP, ALL_GREEN)
        verdict = detect_live(fake)
        self.assertEqual(verdict.status, bp.STATUS_HEALTHY, verdict.reason)
        self.assertEqual(len(verdict.main_contexts), len(FULL_CONTRACT))
        self.assertEqual(verdict.main_evidence_source, bp.SOURCE_HEAD_SHA)
        self.assertIn(TIP[:12], verdict.reason)

    def test_advisory_red_does_not_redden_a_green_tip(self) -> None:
        fake = FakeGitHub()
        fake.head(TIP, ALL_GREEN)  # the build run also carries a red hosted Linux leg
        self.assertEqual(detect_live(fake).status, bp.STATUS_HEALTHY)

    def test_drift_fast_red_with_macos_green_is_red_and_names_drift_fast(self) -> None:
        """The 2026-09-29 shape: `macos` green on main's tip, the required
        `drift-fast` red for fourteen hours. Judging macos alone read healthy."""
        fake = FakeGitHub()
        runs = fake.head(TIP, {**ALL_GREEN, "drift-fast": "failure"})
        fake.logs[runs["drift-fast"]] = REAL_DRIFT_FAST_LOG
        verdict = detect_live(fake)
        self.assertEqual(verdict.status, bp.STATUS_SUSPECTED, verdict.reason)
        self.assertTrue(verdict.main_observed)
        self.assertEqual(verdict.main_failing_contexts, ("drift-fast",))
        self.assertEqual(verdict.main_run_id, runs["drift-fast"])
        self.assertEqual(set(verdict.tests), set(DRIFT_TESTS))
        self.assertIn("drift-fast", verdict.reason)
        payload = bp.signal(verdict)
        self.assertEqual(payload["main_failing_contexts"], ["drift-fast"])
        states = {c["context"]: c["state"] for c in payload["main_contexts"]}
        self.assertEqual(states["macos"], "success")
        self.assertEqual(states["drift-fast"], "failure")

    def test_a_red_context_is_red_even_when_macos_reused_a_receipt(self) -> None:
        fake = FakeGitHub()
        fake.head(TIP, {**ALL_GREEN, "Vellum freeze": "failure"},
                  macos_steps=RECEIPT_REUSE_JOB)
        verdict = detect_live(fake)
        self.assertTrue(verdict.main_observed)
        self.assertEqual(verdict.main_failing_contexts, ("Vellum freeze",))
        self.assertIn("named no ctest", verdict.reason)

    def test_vellum_trusted_freeze_is_read_from_a_commit_status(self) -> None:
        """`Vellum trusted freeze` can reach a commit as a STATUS, not a job."""
        contexts = {k: v for k, v in ALL_GREEN.items() if k != "Vellum trusted freeze"}
        fake = FakeGitHub()
        fake.head(TIP, contexts, statuses={"Vellum trusted freeze": "failure"})
        verdict = detect_live(fake)
        self.assertEqual(verdict.main_failing_contexts, ("Vellum trusted freeze",))
        source = {c.context: c.source for c in verdict.main_contexts}
        self.assertEqual(source["Vellum trusted freeze"], bp.SOURCE_STATUS)

        green = FakeGitHub()
        green.head(TIP, contexts, statuses={"Vellum trusted freeze": "success"})
        self.assertEqual(detect_live(green).status, bp.STATUS_HEALTHY)

    def test_a_missing_required_context_is_unproven_never_healthy(self) -> None:
        contexts = {k: v for k, v in ALL_GREEN.items() if k != "drift-fast"}
        fake = FakeGitHub()
        fake.head(TIP, contexts)
        verdict = detect_live(fake)
        self.assertEqual(verdict.status, bp.STATUS_UNPROVEN, verdict.reason)
        self.assertIn("drift-fast missing", verdict.reason)

    def test_a_still_running_context_is_unproven(self) -> None:
        fake = FakeGitHub()
        fake.head(TIP, {**ALL_GREEN, "Build + prove + (owner-gated) deploy": ""})
        verdict = detect_live(fake)
        self.assertEqual(verdict.status, bp.STATUS_UNPROVEN, verdict.reason)
        self.assertIn("pending", verdict.reason)

    def test_a_push_check_run_on_the_landed_commit_is_not_the_gate(self) -> None:
        """After landing, push runs add check runs to the same commit. Only the
        merge_group runs' check suites are read."""
        contexts = {k: v for k, v in ALL_GREEN.items() if k != "drift-fast"}
        fake = FakeGitHub()
        fake.head(TIP, contexts, push_check_runs=[{
            "id": 1, "name": "drift-fast", "status": "completed",
            "conclusion": "success", "check_suite": {"id": 1}}])
        self.assertEqual(detect_live(fake).status, bp.STATUS_UNPROVEN)

    def test_red_macos_names_its_tests_from_the_real_artifact(self) -> None:
        fake = FakeGitHub()
        runs = fake.head(TIP, {**ALL_GREEN, "macos": "failure"})
        fake.artifacts[runs["macos"]] = REAL_MACOS_LAST_TESTS_FAILED
        verdict = detect_live(fake)
        self.assertEqual(verdict.main_failing_contexts, ("macos",))
        self.assertEqual(
            verdict.tests,
            ("materialized paint keeps the full host frame around an authored panel",),
        )

    def test_no_merge_group_run_for_the_tip_is_unproven(self) -> None:
        """An admin or direct push lands no merge group: nothing tested the tip."""
        verdict = detect_live(FakeGitHub())
        self.assertEqual(verdict.status, bp.STATUS_UNPROVEN)
        self.assertFalse(verdict.main_observed)
        self.assertEqual(verdict.main_head_sha, TIP)
        self.assertIn("no merge_group run", verdict.reason)

    def test_a_receipt_reuse_green_on_the_tip_is_unproven(self) -> None:
        fake = FakeGitHub()
        fake.head(TIP, ALL_GREEN, macos_steps=RECEIPT_REUSE_JOB)
        verdict = detect_live(fake)
        self.assertEqual(verdict.status, bp.STATUS_UNPROVEN)
        self.assertIn("macos success-but-not-executed", verdict.reason)

    def test_the_tip_is_looked_up_by_sha_on_merge_group_for_every_workflow(self) -> None:
        fake = FakeGitHub()
        fake.head(TIP, ALL_GREEN)
        detect_live(fake)
        lookups = [p for p in fake.paths if f"head_sha={TIP}" in p]
        self.assertTrue(lookups, fake.paths)
        self.assertIn("event=merge_group", lookups[0])
        self.assertNotIn("/workflows/", lookups[0])

    def test_macos_only_fallback_contract_keeps_working(self) -> None:
        fake = FakeGitHub()
        fake.head(TIP, {"macos": "success"})
        verdict = detect_live(fake, required=bp.FALLBACK_REQUIRED_CONTEXTS)
        self.assertEqual(verdict.status, bp.STATUS_HEALTHY, verdict.reason)

    def test_the_configured_contract_and_workflows_are_all_six(self) -> None:
        try:
            import tomllib  # noqa: F401
        except ImportError:  # Python < 3.11
            self.skipTest("tomllib unavailable")
        self.assertEqual(set(bp.required_contexts()), set(CONTEXT_WORKFLOW))
        self.assertEqual(set(bp.required_workflows()), set(WORKFLOWS))


# Merge-group heads on 2026-09-29, newest first, while main carried the
# `drift-fast` red: each batch's `macos` passed and its `drift-fast` failed.
STREAK_HEADS = [
    "3c4646ca8d943e8665b02db23aa2d880f538a5be",
    "f8f01a01ddfd15dc1b45ae3f2bdb71aff6e38571",
    "4e0e8354bfb26f8ade335364dbff9ff2b3be10be",
    "f8a1ca5a6e7eb4ba8e200d48dd9d50ac27fdeec4",
    "99b92cd5a4dfe985e354bce5a5f9cf1bfe0bfdd7",
]
GREEN_HEAD = "365c6baa06e49bc9e741c24fcf4d31e772e3141f"


class BatchStreakTests(unittest.TestCase):
    """The streak is measured over every required context of each batch."""

    def _fake(self, tip_red: bool = True) -> FakeGitHub:
        fake = FakeGitHub(tip=STREAK_HEADS[2])
        for index, sha in enumerate(STREAK_HEADS):
            runs = fake.head(sha, {**ALL_GREEN, "drift-fast": "failure"},
                             created_at=f"2026-09-29T15:{40 - index:02d}:00Z")
            fake.logs[runs["drift-fast"]] = REAL_DRIFT_FAST_LOG
        if not tip_red:
            fake.tip = GREEN_HEAD
        fake.head(GREEN_HEAD, ALL_GREEN, created_at="2026-09-29T11:44:00Z")
        return fake

    def test_the_0929_drift_fast_streak_is_measured(self) -> None:
        verdict = detect_live(self._fake())
        self.assertEqual(verdict.streak.length, len(STREAK_HEADS), verdict.reason)
        self.assertEqual(verdict.streak.heads, tuple(STREAK_HEADS))
        self.assertEqual(verdict.streak.shared_contexts, ("drift-fast",))
        self.assertEqual(set(verdict.streak.shared_tests), set(DRIFT_TESTS))
        self.assertEqual(verdict.status, bp.STATUS_POISONED, verdict.reason)
        payload = bp.signal(verdict)
        self.assertEqual(payload["batch_streak"], len(STREAK_HEADS))
        self.assertEqual(payload["batch_streak_contexts"], ["drift-fast"])

    def test_judged_by_macos_alone_the_same_history_has_no_streak(self) -> None:
        """The negative control: the old macos-only reading of this exact
        history sees every batch pass, which is how 09-29 read streak 0."""
        verdict = detect_live(self._fake(), required=bp.FALLBACK_REQUIRED_CONTEXTS)
        self.assertEqual(verdict.streak.length, 0)
        self.assertEqual(verdict.status, bp.STATUS_HEALTHY)

    def test_a_green_batch_breaks_the_streak(self) -> None:
        fake = self._fake()
        fake.head("a" * 40, ALL_GREEN, created_at="2026-09-29T15:38:30Z")
        verdict = detect_live(fake)
        self.assertEqual(verdict.streak.length, 2)

    def test_a_streak_needs_min_streak_batches(self) -> None:
        verdict = detect_live(self._fake(), batch_limit=2, min_streak=3)
        self.assertEqual(verdict.streak.length, 2)
        self.assertEqual(verdict.status, bp.STATUS_SUSPECTED)
        self.assertEqual(verdict.proof, bp.PROOF_MAIN_ONLY)

    def test_a_batch_with_a_green_gate_breaks_the_streak(self) -> None:
        """The old run-conclusion reading made every batch a failure because
        advisory Linux failed, so the "streak" was a Linux artifact."""
        from unittest import mock

        with mock.patch.object(
            bp, "_jobs", return_value=GREEN_GATE_RED_ADVISORY
        ), mock.patch.object(bp, "artifact_failing_tests", return_value=()):
            seen = bp.observe("o/r", tip_run("12", conclusion="failure"), "batch", REQUIRED)
        self.assertTrue(seen.passed)


def tip_run(run_id: str, conclusion: str = "failure", status: str = "completed") -> dict:
    return {
        "id": int(run_id),
        "head_sha": TIP,
        "head_branch": f"gh-readonly-queue/main/pr-9039-{'1' * 40}",
        "status": status,
        "conclusion": conclusion,
        "created_at": "2026-09-29T04:59:00Z",
    }


class LogFailingTestsTests(unittest.TestCase):
    def test_the_real_drift_fast_log_names_its_failures(self) -> None:
        fake = FakeGitHub()
        fake.logs["5"] = REAL_DRIFT_FAST_LOG
        with fake.patched():
            self.assertEqual(bp.log_failing_tests("o/r", "5", "drift-fast"), DRIFT_TESTS)
            self.assertEqual(bp.log_failing_tests("o/r", "5", "another-job"), ())
            self.assertEqual(bp.log_failing_tests("o/r", "6", "drift-fast"), ())


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

    def test_every_completed_merge_group_run_triggers_the_detector(self) -> None:
        """The schedule is throttled to about one run in four hours, so the read
        that catches a red base within a batch's lifetime is the one a merge
        group triggers -- and a GREEN one too, or a healthy tip is never
        recorded and the last published verdict goes stale. The workflow name
        must be build.yml's own, or the trigger silently never fires."""
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
        self.assertNotIn("workflow_run.conclusion", guard,
                         "a green merge group must also record the tip's health")
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
                         {"actions": "read", "checks": "read", "contents": "read",
                          "pull-requests": "read", "statuses": "read"})
        self.assertNotRegex(self.detector_code, r"(?m)^[ \t]+permissions:")  # no job widening

    def test_the_detector_group_is_shared_and_never_cancels_in_progress(self) -> None:
        """One at a time, and a superseded tick must lose nothing.

        A per-sha group would let every merge start its own detector, and a
        superseded read pinned to a sha loses its observation for good, because
        that sha never comes again.
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
        """Pins why no push-run job may wait on a self-hosted runner.

        Scoped to the top-level `concurrency:` block, not the file: the tokens
        it checks appear six other times in build.yml, so a whole-file scan
        stayed green when the block's own push exemption was deleted.

        One push run holds main's group with cancel-in-progress false, so a job
        queued for a runner nobody serves gets every later push run cancelled
        while pending. If this stops holding, revisit the push-lane routing
        test in tools/scripts/test_fork_pr_runner_routing.py.
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

    def test_build_yml_push_carries_no_macos_leg_to_read(self) -> None:
        """The detector reads the merge group on main's tip, not a push run.

        If build.yml's push matrix gains a macOS leg again, re-decide whether
        that lane is an observation this detector should read.
        """
        self.assertIn('if EVENT_NAME != "push":\n              include.append({\n'
                      '                  "key": "macos"', self.build)

    def test_the_failing_test_source_is_the_artifact_not_the_job_log(self) -> None:
        """`ghapp api .../jobs/<id>/logs` refuses escape sequences and returns
        99 bytes, so a log scrape silently yields NO failing tests -- which
        reads as "the failure was not a test failure". A context with no
        artifact is read through the attributor's RUN-log zip instead.

        Comment text is stripped first: the prohibition has to be explained in
        the module, and a raw substring scan would trip over the explanation.
        """
        source = pathlib.Path(bp.__file__).read_text(encoding="utf-8")
        self.assertIn("/artifacts/", source)
        code = "\n".join(
            line.split("#", 1)[0] for line in source.splitlines()
        )
        self.assertNotIn("/logs", code, "the detector reads a job log again")
        self.assertIn("attributor.run_log_zip(", code)


if __name__ == "__main__":
    unittest.main()
