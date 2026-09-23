#!/usr/bin/env python3
"""Tests for merge_group batch attribution.

The behaviour worth protecting is the REFUSAL. Naming a culprit is useful, but
naming the wrong one is actively harmful: someone goes and "fixes" an innocent
branch while the real break sits on main, unexamined, failing every batch that
forms. So the weak-match cases are tested at least as hard as the strong ones,
including the exact incidental overlap observed in a real batch.
"""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import queue_batch_attribute as qba  # noqa: E402


class ScoreMatchTests(unittest.TestCase):
    def test_exact_test_name_to_file_stem_is_decisive(self) -> None:
        self.assertEqual(
            qba.score_match(
                "prepush-cannot-measure",
                "tools/scripts/test_prepush_cannot_measure.py",
            ),
            qba.WEIGHT_EXACT_STEM,
        )

    def test_test_name_contained_in_the_path_is_strong(self) -> None:
        self.assertEqual(
            qba.score_match(
                "widget-bridge",
                "core/view/src/widget_bridge/widget_assets_api.cpp",
            ),
            qba.WEIGHT_PATH_CONTAINS,
        )

    def test_a_single_shared_token_scores_nothing(self) -> None:
        """One generic word in common is noise, not ownership."""
        self.assertEqual(
            qba.score_match("consumption-census-drift", "tools/ci/census.py"),
            0,
        )

    def test_incidental_overlap_scores_low_not_decisive(self) -> None:
        """The real weak match from a failed batch must stay far below threshold."""
        score = qba.score_match(
            "cmake-control-sdk-consumer",
            "test/cmake/test_gpu_audio_sdk_consumer.cmake",
        )
        self.assertGreater(score, 0)
        self.assertLess(score, qba.CONFIDENT)

    def test_unrelated_path_scores_zero(self) -> None:
        self.assertEqual(
            qba.score_match("skip-not-pass-lint", "core/audio/src/buffer_view.cpp"),
            0,
        )


class AttributeVerdictTests(unittest.TestCase):
    def test_strong_match_names_the_owning_pr(self) -> None:
        result = qba.attribute(
            ["prepush-cannot-measure"],
            {
                8000: ["docs/guides/local-ci.md"],
                8001: ["tools/scripts/test_prepush_cannot_measure.py"],
            },
        )
        self.assertEqual(result.verdict, qba.VERDICT_CULPRIT)
        self.assertEqual(result.culprit, 8001)

    def test_only_weak_matches_refuse_to_accuse(self) -> None:
        """The guard against a false accusation, in its exact observed form."""
        result = qba.attribute(
            ["cmake-control-sdk-consumer"],
            {8672: ["test/cmake/test_gpu_audio_sdk_consumer.cmake"]},
        )
        self.assertEqual(result.verdict, qba.VERDICT_PRE_EXISTING)
        self.assertIsNone(result.culprit)

    def test_no_match_at_all_is_unowned(self) -> None:
        result = qba.attribute(
            ["skip-not-pass-lint"], {8000: ["core/audio/src/buffer_view.cpp"]}
        )
        self.assertEqual(result.verdict, qba.VERDICT_UNOWNED)
        self.assertIsNone(result.culprit)

    def test_no_open_prs_is_unowned(self) -> None:
        result = qba.attribute(["anything-at-all"], {})
        self.assertEqual(result.verdict, qba.VERDICT_UNOWNED)

    def test_the_strongest_pr_wins_when_several_match(self) -> None:
        result = qba.attribute(
            ["prepush-cannot-measure", "widget-bridge"],
            {
                8001: ["tools/scripts/test_prepush_cannot_measure.py"],
                8002: ["core/view/src/widget_bridge/a.cpp"],
            },
        )
        self.assertEqual(result.verdict, qba.VERDICT_CULPRIT)
        self.assertEqual(result.culprit, 8001)

    def test_a_cluster_of_unowned_failures_reads_as_pre_existing(self) -> None:
        """A whole batch failing with no owner is the broken-main signature."""
        tests = [
            "inspector-protocol-registry-complete",
            "skip-not-pass-lint",
            "catch-discover-timeout-guard",
            "consumption-census-drift",
        ]
        result = qba.attribute(tests, {8672: ["docs/guides/local-ci.md"]})
        self.assertIn(
            result.verdict, (qba.VERDICT_UNOWNED, qba.VERDICT_PRE_EXISTING)
        )
        self.assertIsNone(result.culprit)


class ThresholdTests(unittest.TestCase):
    def test_threshold_boundary_is_inclusive_of_a_containment_match(self) -> None:
        """A containment match sits exactly at the threshold and must count."""
        self.assertEqual(qba.WEIGHT_PATH_CONTAINS, qba.CONFIDENT)
        result = qba.attribute(
            ["widget-bridge"], {9: ["core/view/src/widget_bridge/a.cpp"]}
        )
        self.assertEqual(result.verdict, qba.VERDICT_CULPRIT)


class ParseFailingTestsTests(unittest.TestCase):
    LOG = """
2026-09-23T08:40:01Z 99% tests passed, 3 tests failed out of 21764
2026-09-23T08:40:01Z
2026-09-23T08:40:01Z The following tests FAILED:
2026-09-23T08:40:01Z \t 101 - skip-not-pass-lint (Failed)
2026-09-23T08:40:01Z \t 340 - catch-discover-timeout-guard (Timeout)
2026-09-23T08:40:01Z \t 902 - consumption-census-drift (Subprocess aborted)
2026-09-23T08:40:01Z Errors while running CTest
2026-09-23T08:40:02Z \t 999 - never-reached (Failed)
"""

    def test_parses_each_failure_kind(self) -> None:
        self.assertEqual(
            qba.parse_failing_tests(self.LOG),
            [
                "skip-not-pass-lint",
                "catch-discover-timeout-guard",
                "consumption-census-drift",
            ],
        )

    def test_stops_at_the_end_of_the_failure_block(self) -> None:
        self.assertNotIn("never-reached", qba.parse_failing_tests(self.LOG))

    def test_a_log_without_a_failure_block_yields_nothing(self) -> None:
        self.assertEqual(qba.parse_failing_tests("build succeeded\n"), [])


class RenderTests(unittest.TestCase):
    def test_culprit_output_names_the_pr(self) -> None:
        result = qba.attribute(
            ["prepush-cannot-measure"],
            {8001: ["tools/scripts/test_prepush_cannot_measure.py"]},
        )
        text = "\n".join(qba.render(result, "123"))
        self.assertIn("CULPRIT: #8001", text)

    def test_weak_output_says_pre_existing_and_names_nobody_as_culprit(self) -> None:
        result = qba.attribute(
            ["cmake-control-sdk-consumer"],
            {8672: ["test/cmake/test_gpu_audio_sdk_consumer.cmake"]},
        )
        text = "\n".join(qba.render(result, "123"))
        self.assertIn("LIKELY PRE-EXISTING ON MAIN", text)
        self.assertNotIn("CULPRIT", text)


if __name__ == "__main__":
    unittest.main()
