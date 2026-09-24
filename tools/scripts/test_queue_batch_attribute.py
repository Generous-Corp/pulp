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
import tempfile
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


class CensusOwnershipTests(unittest.TestCase):
    """A gate that reports a COUNT is owned by a diff its name cannot match.

    `public_headers.count` is walked live from each target's exported include
    roots, so adding one header under a root a target already exports drifts the
    census. The header's path shares no token with `consumption-census-drift`,
    so path scoring alone gives the owning pull request zero and the batch reads
    as "pre-existing on main" -- a phantom main-side defect. The fixture below is
    the batch where that happened: a canvas header added under
    `core/canvas/include`, which the census records as an exported root.
    """

    ROOTS = frozenset({"core/canvas/include", "core/view/include"})

    BATCH = {
        8726: [
            qba.ChangedFile("core/canvas/include/pulp/canvas/path_measure.hpp", "added"),
            qba.ChangedFile("core/canvas/src/path_measure.cpp", "added"),
            qba.ChangedFile("test/test_canvas_path_measure.cpp", "added"),
        ],
        8754: [qba.ChangedFile("test/test_sdf_emitter_differential.cpp", "added")],
        8756: [qba.ChangedFile("tools/scripts/queue_admission_guard.py", "added")],
    }

    def test_path_scoring_alone_cannot_see_the_owner(self) -> None:
        """The false negative itself: the cause scores zero by name."""
        self.assertEqual(
            qba.score_match(
                "consumption-census-drift",
                "core/canvas/include/pulp/canvas/path_measure.hpp",
            ),
            0,
        )

    def test_a_header_added_under_an_exported_root_owns_the_drift(self) -> None:
        result = qba.attribute(
            ["consumption-census-drift"], self.BATCH, census_roots=self.ROOTS
        )
        self.assertEqual(result.verdict, qba.VERDICT_CULPRIT)
        self.assertEqual(result.culprit, 8726)

    def test_the_report_names_the_owner_instead_of_blaming_main(self) -> None:
        result = qba.attribute(
            ["consumption-census-drift"], self.BATCH, census_roots=self.ROOTS
        )
        text = "\n".join(qba.render(result, "35944327058"))
        self.assertIn("CULPRIT: #8726", text)
        self.assertNotIn("PRE-EXISTING ON MAIN", text)
        self.assertIn("core/canvas/include/pulp/canvas/path_measure.hpp", text)

    def test_the_report_explains_why_a_count_gate_has_no_matching_file(self) -> None:
        """An unexplained census attribution reads as a non sequitur."""
        result = qba.attribute(
            ["consumption-census-drift"], self.BATCH, census_roots=self.ROOTS
        )
        text = "\n".join(qba.render(result, "35944327058"))
        self.assertIn("ADDED or DELETED", text)

    def test_the_negative_contract_gate_is_owned_the_same_way(self) -> None:
        """Live drift fails the contract's unmodified-inputs control too."""
        result = qba.attribute(
            ["consumption-census-negative-contract"],
            self.BATCH,
            census_roots=self.ROOTS,
        )
        self.assertEqual(result.culprit, 8726)

    def test_a_deleted_header_owns_the_drift_as_well(self) -> None:
        result = qba.attribute(
            ["consumption-census-drift"],
            {8726: [qba.ChangedFile("core/view/include/pulp/view/gone.hpp", "removed")]},
            census_roots=self.ROOTS,
        )
        self.assertEqual(result.culprit, 8726)

    def test_editing_a_header_in_place_owns_nothing(self) -> None:
        """A modified header leaves the count exactly where it was."""
        result = qba.attribute(
            ["consumption-census-drift"],
            {
                8726: [
                    qba.ChangedFile(
                        "core/canvas/include/pulp/canvas/path.hpp", "modified"
                    )
                ]
            },
            census_roots=self.ROOTS,
        )
        self.assertIsNone(result.culprit)

    def test_a_header_outside_every_exported_root_owns_nothing(self) -> None:
        """A private header is not public header surface and is not counted."""
        result = qba.attribute(
            ["consumption-census-drift"],
            {8726: [qba.ChangedFile("core/canvas/src/path_measure.hpp", "added")]},
            census_roots=self.ROOTS,
        )
        self.assertIsNone(result.culprit)

    def test_a_non_header_under_an_exported_root_owns_nothing(self) -> None:
        result = qba.attribute(
            ["consumption-census-drift"],
            {8726: [qba.ChangedFile("core/canvas/include/README.md", "added")]},
            census_roots=self.ROOTS,
        )
        self.assertIsNone(result.culprit)

    def test_the_rule_is_scoped_to_census_gates(self) -> None:
        """A header add must not become a universal culprit for any failure."""
        result = qba.attribute(
            ["skip-not-pass-lint"], self.BATCH, census_roots=self.ROOTS
        )
        self.assertIsNone(result.culprit)

    def test_a_rename_across_a_root_boundary_owns_the_drift(self) -> None:
        result = qba.attribute(
            ["consumption-census-drift"],
            {
                8726: [
                    qba.ChangedFile(
                        "core/canvas/include/pulp/canvas/moved.hpp",
                        "renamed",
                        previous_path="core/canvas/src/moved.hpp",
                    )
                ]
            },
            census_roots=self.ROOTS,
        )
        self.assertEqual(result.culprit, 8726)

    def test_a_rename_inside_one_root_owns_nothing(self) -> None:
        """Both ends are counted, so the count did not move."""
        result = qba.attribute(
            ["consumption-census-drift"],
            {
                8726: [
                    qba.ChangedFile(
                        "core/canvas/include/pulp/canvas/b.hpp",
                        "renamed",
                        previous_path="core/canvas/include/pulp/canvas/a.hpp",
                    )
                ]
            },
            census_roots=self.ROOTS,
        )
        self.assertIsNone(result.culprit)

    def test_a_bare_path_cannot_claim_census_ownership(self) -> None:
        """Without a diff status there is no add/delete evidence to act on."""
        result = qba.attribute(
            ["consumption-census-drift"],
            {8726: ["core/canvas/include/pulp/canvas/path_measure.hpp"]},
            census_roots=self.ROOTS,
        )
        self.assertIsNone(result.culprit)

    def test_touching_the_census_itself_owns_a_census_failure(self) -> None:
        result = qba.attribute(
            ["consumption-census-drift"],
            {8726: [qba.ChangedFile("docs/status/consumption-profiles.json", "modified")]},
            census_roots=self.ROOTS,
        )
        self.assertEqual(result.culprit, 8726)


class BatchMembershipTests(unittest.TestCase):
    """Only an entry the batch CONTAINS can own the batch's failure.

    Header-count ownership makes this load-bearing. Two open branches can each
    add a counted header under the same exported root while only one of them is
    in the batch, and scoring the whole open list lets the bystander outrank the
    real owner -- a false accusation dressed as a decisive match. The fixture is
    the batch where that nearly happened: one member, one non-member, both
    adding a header under `core/canvas/include`.
    """

    ROOTS = frozenset({"core/canvas/include"})
    MEMBER = 8726
    BYSTANDER = 8761
    DIFFS = {
        8726: [
            qba.ChangedFile("core/canvas/include/pulp/canvas/path_measure.hpp", "added")
        ],
        8761: [
            qba.ChangedFile(
                "core/canvas/include/pulp/canvas/drawlist_format.hpp", "added"
            )
        ],
    }

    def test_only_the_member_is_named(self) -> None:
        result = qba.attribute(
            ["consumption-census-drift"],
            self.DIFFS,
            census_roots=self.ROOTS,
            batch_members=frozenset({self.MEMBER}),
        )
        self.assertEqual(result.culprit, self.MEMBER)
        self.assertNotIn(self.BYSTANDER, result.strength)
        self.assertEqual(result.scoped_out, [self.BYSTANDER])

    def test_without_membership_the_bystander_can_outrank_the_owner(self) -> None:
        """Unscoped, both tie -- which is why membership is not optional."""
        result = qba.attribute(
            ["consumption-census-drift"], self.DIFFS, census_roots=self.ROOTS
        )
        self.assertEqual(result.contenders, [self.MEMBER, self.BYSTANDER])
        self.assertFalse(result.batch_members_known)

    def test_unknown_membership_is_announced_not_assumed(self) -> None:
        result = qba.attribute(
            ["consumption-census-drift"], self.DIFFS, census_roots=self.ROOTS
        )
        text = "\n".join(qba.render(result, "1"))
        self.assertIn("membership could not be read", text)

    def test_equal_owners_are_both_named_rather_than_one_picked(self) -> None:
        result = qba.attribute(
            ["consumption-census-drift"],
            self.DIFFS,
            census_roots=self.ROOTS,
            batch_members=frozenset({self.MEMBER, self.BYSTANDER}),
        )
        text = "\n".join(qba.render(result, "1"))
        self.assertIn("CO-OWNERS: #8726, #8761", text)
        self.assertNotIn("CULPRIT", text)

    def test_the_ref_names_the_last_entry_and_the_merges_name_the_rest(self) -> None:
        members = qba.parse_batch_members(
            "gh-readonly-queue/main/pr-8726-"
            "3d9026b8750626493e6f0de6ba87399818b646cf",
            [
                "feat(canvas): measure and trim a Path by arc length",
                "Merge branch 'main' into feature/path-arc-length-measure",
                "Merge pull request #8726 from Generous-Corp/feature/path-arc",
                "Merge pull request #8754 from Generous-Corp/test/sdf-emitter",
            ],
        )
        self.assertEqual(members, frozenset({8726, 8754}))

    def test_a_single_entry_batch_is_read_from_the_ref_alone(self) -> None:
        """No merge subject is needed: the queue ref names the last entry."""
        members = qba.parse_batch_members(
            "gh-readonly-queue/main/pr-8726-"
            "3d9026b8750626493e6f0de6ba87399818b646cf",
            [],
        )
        self.assertEqual(members, frozenset({8726}))

    def test_an_unrecognised_ref_yields_unknown_never_an_empty_batch(self) -> None:
        """Empty would scope every candidate out and report nobody."""
        self.assertIsNone(qba.parse_batch_members("refs/heads/main", []))
        self.assertIsNone(qba.parse_batch_members("", ["Merge pull request #1 x"]))


class HeaderSuffixTests(unittest.TestCase):
    """Tie the counted-suffix list to what the census actually counts.

    The rule turns on whether a path is header surface the census counts, and
    that list lives in the generator. Restating it here is a copy that can drift
    silently, so ask the generator instead of asserting the copy is right.
    """

    def counted(self, suffix: str) -> int:
        import consumption_census

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory) / "inc"
            root.mkdir()
            (root / f"probe{suffix}").write_text("")
            return consumption_census.count_headers(pathlib.Path(directory), ["inc"])

    def test_every_suffix_the_census_counts_is_declared_and_no_others(self) -> None:
        for suffix in (".h", ".hpp", ".hxx", ".hh", ".inc", ".cpp", ".md"):
            with self.subTest(suffix=suffix):
                self.assertEqual(
                    bool(self.counted(suffix)),
                    suffix in qba.HEADER_SUFFIXES,
                    f"{suffix}: the census and this rule disagree about counting it",
                )

    def test_the_probe_can_see_a_counted_header(self) -> None:
        """Control: a probe that counts nothing would pass the test above."""
        self.assertEqual(self.counted(".hpp"), 1)


class CensusRootsTests(unittest.TestCase):
    """The roots come out of the committed census, so read the real one.

    A roots list that drifts from the published census attributes a drift to the
    wrong diff, which is the failure this rule exists to end.
    """

    REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

    def test_the_committed_census_yields_its_exported_include_roots(self) -> None:
        roots = qba.census_include_roots(self.REPO_ROOT)
        self.assertGreater(len(roots), 10)
        self.assertIn("core/canvas/include", roots)

    def test_a_checkout_without_a_census_yields_nothing(self) -> None:
        self.assertEqual(
            qba.census_include_roots(pathlib.Path("/nonexistent-checkout")),
            frozenset(),
        )

    def test_an_unreadable_census_is_reported_not_silently_passed(self) -> None:
        """No roots means the rule did not run; that is not an absence of owners."""
        result = qba.attribute(
            ["consumption-census-drift"],
            {8726: [qba.ChangedFile("core/canvas/include/a.hpp", "added")]},
            census_roots=frozenset(),
        )
        self.assertFalse(result.census_roots_read)
        text = "\n".join(qba.render(result, "1"))
        self.assertIn("header-count ownership was NOT evaluated", text)

    def test_a_non_census_batch_carries_no_census_note(self) -> None:
        result = qba.attribute(
            ["prepush-cannot-measure"],
            {8001: ["tools/scripts/test_prepush_cannot_measure.py"]},
        )
        text = "\n".join(qba.render(result, "1"))
        self.assertNotIn("census", text)


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
