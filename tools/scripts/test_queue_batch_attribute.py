#!/usr/bin/env python3
"""Tests for merge_group batch attribution.

The behaviour worth protecting is the REFUSAL. Naming a culprit is useful, but
naming the wrong one is actively harmful: someone goes and "fixes" an innocent
branch while the real break sits on main, unexamined, failing every batch that
forms. So the weak-match cases are tested at least as hard as the strong ones,
including the exact incidental overlap observed in a real batch.
"""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import tomllib
import unittest
import zipfile
from unittest import mock

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


# ---------------------------------------------------------------------------
# Certification: the verdict Shipyard's queue-arm-guard reads.
#
# The behaviour worth protecting is again the REFUSAL, and harder here than in
# attribution: a wrong refusal costs a stuck pull request, but a wrong
# CERTIFICATION re-queues a head that ejects its innocent batch-mates -- the
# exact harm the guard exists to prevent. So every fixture below that must not
# certify is drawn from a real ejecting batch, and the one that must certify is
# too.
# ---------------------------------------------------------------------------

# Recorded from runs 36245658496 and 36093055057, the two batches that ejected
# pull requests 8896 and 8811. A merge_group run is mostly `skipped` jobs, and
# the failing step names are exactly these.
JOBS_LINK_ERROR = [
    {"name": "windows", "conclusion": "skipped", "steps": []},
    {"name": "linux", "conclusion": "skipped", "steps": []},
    {
        "name": "macos",
        "conclusion": "failure",
        "steps": [
            {"name": "Configure", "conclusion": "success"},
            {"name": "Build", "conclusion": "failure"},
        ],
    },
]
JOBS_CCACHE_PLUS_BUILD = [
    {
        "name": "macos",
        "conclusion": "failure",
        "steps": [{"name": "Install ccache (macOS)", "conclusion": "failure"}],
    },
    {
        "name": "Linux (x64) [github-hosted]",
        "conclusion": "failure",
        "steps": [{"name": "Build", "conclusion": "failure"}],
    },
]
# Recorded from run 35973715485, where a curl of a pinned wasi-sdk release --
# and nothing else -- ejected pull request 8773.
JOBS_WASI_SDK = [
    {
        "name": "Build + prove + (owner-gated) deploy",
        "conclusion": "failure",
        "steps": [{"name": "Set up wasi-sdk (pinned)", "conclusion": "failure"}],
    },
]


class NonContentStepTests(unittest.TestCase):
    def test_observed_non_content_failures_are_recognised(self) -> None:
        for step in (
            "Install ccache (macOS)",
            "Install visual-analysis Python dependencies",
            "Install Linux dependencies",
            "Upload exact GPU-audio SDK (macOS ARM64)",
            "Upload ctest logs and JUnit report",
            "Set up wasi-sdk (pinned)",
            "Set up job",
            "Checkout",
        ):
            with self.subTest(step=step):
                self.assertTrue(qba.non_content_step(step))

    def test_a_step_that_compiles_or_runs_the_tree_is_content(self) -> None:
        for step in (
            "Build",
            "Configure",
            "Test (non-Windows)",
            "Surface ctest failures (non-Windows)",
            "Skill-sync check",
            "Validate durable source-authority events",
        ):
            with self.subTest(step=step):
                self.assertFalse(qba.non_content_step(step))

    def test_a_step_running_a_repository_script_is_content(self) -> None:
        # `Hydrate bounded GPU provenance commits` runs
        # tools/scripts/hydrate_gpu_provenance_commits.py over inputs this module
        # cannot enumerate, so no name pattern could tell whether a head broke it.
        self.assertFalse(qba.non_content_step("Hydrate bounded GPU provenance commits"))

    def test_the_allowlist_is_anchored_so_install_does_not_swallow_a_build(self) -> None:
        # `cmake --install` builds and installs the tree, and an unanchored
        # "install" would read that as infrastructure.
        for step in ("Install and test the SDK", "Build then upload the bundle"):
            with self.subTest(step=step):
                self.assertFalse(qba.non_content_step(step))

    def test_an_empty_step_name_is_never_non_content(self) -> None:
        # A failing job with no failing step is an absence, not a cause.
        self.assertFalse(qba.non_content_step(""))


class HeadReachesStepTests(unittest.TestCase):
    def test_a_workflow_change_reaches_every_allowlisted_step(self) -> None:
        changes = [qba.ChangedFile(path=".github/workflows/build.yml")]
        for step in ("Install ccache (macOS)", "Upload ctest logs and JUnit report"):
            with self.subTest(step=step):
                self.assertEqual(
                    qba.head_reaches_step(step, changes), ".github/workflows/build.yml"
                )

    def test_a_composite_action_change_reaches_the_step_it_defines(self) -> None:
        changes = [qba.ChangedFile(path=".github/actions/install-linux-build-deps/action.yml")]
        self.assertIsNotNone(qba.head_reaches_step("Install Linux dependencies", changes))

    def test_a_build_system_change_reaches_only_a_dependency_install(self) -> None:
        changes = [qba.ChangedFile(path="CMakeLists.txt")]
        # It reads $PULP_BUILD_DIR/CMakeCache.txt, so a configure change can break it.
        self.assertEqual(
            qba.head_reaches_step("Install visual-analysis Python dependencies", changes),
            "CMakeLists.txt",
        )
        # A curl of a pinned external release cannot fail because a version moved.
        # Refusing here would make the rule fire on every version bump, which is
        # most of them, and certify nothing ever.
        self.assertIsNone(qba.head_reaches_step("Set up wasi-sdk (pinned)", changes))
        self.assertIsNone(qba.head_reaches_step("Upload ctest logs", changes))

    def test_a_cmake_module_change_reaches_a_dependency_install(self) -> None:
        self.assertEqual(
            qba.head_reaches_step(
                "Install visual-analysis Python dependencies",
                [qba.ChangedFile(path="test/cmake/quality_tests.cmake")],
            ),
            "test/cmake/quality_tests.cmake",
        )

    def test_declarative_ci_data_reaches_nothing(self) -> None:
        # A watch-event JSON is read by its own gate job. If a head broke that,
        # THAT job's step fails and no allowlist pattern matches it, so it is
        # refused on its own account. Counting it here would refuse nearly every
        # Pulp pull request.
        self.assertIsNone(
            qba.head_reaches_step(
                "Install ccache (macOS)",
                [
                    qba.ChangedFile(
                        path=".github/vellum-expansion-watch-events/20260926-x.json"
                    ),
                    qba.ChangedFile(path="core/view/src/widgets.cpp"),
                ],
            )
        )


class ParseFailingStepsTests(unittest.TestCase):
    def test_only_failing_jobs_contribute(self) -> None:
        failures = qba.parse_failing_steps(JOBS_LINK_ERROR)
        self.assertEqual(failures, [qba.StepFailure(job="macos", step="Build")])

    def test_a_failing_job_with_no_failing_step_is_recorded_steplessly(self) -> None:
        failures = qba.parse_failing_steps(
            [{"name": "macos", "conclusion": "failure", "steps": []}]
        )
        self.assertEqual(failures, [qba.StepFailure(job="macos", step="")])

    def test_every_failing_step_of_every_failing_job_is_kept(self) -> None:
        self.assertEqual(
            qba.parse_failing_steps(JOBS_CCACHE_PLUS_BUILD),
            [
                qba.StepFailure(job="macos", step="Install ccache (macOS)"),
                qba.StepFailure(job="Linux (x64) [github-hosted]", step="Build"),
            ],
        )


class CertifyTests(unittest.TestCase):
    SOURCE = [qba.ChangedFile(path="core/view/src/widgets.cpp")]

    def rule(self, pr, jobs, changes=None, ctest=None):
        return qba.certify(
            99,
            pr,
            qba.explain_failures(
                pr,
                qba.parse_failing_steps(jobs),
                ctest,
                self.SOURCE if changes is None else changes,
            ),
        )

    def test_a_toolchain_fetch_out_of_the_head_s_reach_certifies(self) -> None:
        verdict = self.rule(8773, JOBS_WASI_SDK)
        self.assertEqual(verdict.verdict, qba.VERDICT_INFRASTRUCTURE)
        self.assertIs(verdict.implicates_head, False)

    def test_one_content_step_beside_an_infrastructure_one_refuses(self) -> None:
        # Run 36093055057: `Install ccache (macOS)` is accountable, the Linux
        # `Build` is not, and that head's own later commit admits it was broken.
        # Certifying on the accountable half is the whole failure mode.
        verdict = self.rule(8811, JOBS_CCACHE_PLUS_BUILD)
        self.assertEqual(verdict.verdict, qba.VERDICT_UNEXPLAINED)
        self.assertIsNone(verdict.implicates_head)
        self.assertIn("Build", verdict.evidence)

    def test_a_link_error_with_no_test_failure_refuses(self) -> None:
        # Run 36245658496. "No ctest block, therefore infrastructure" is the
        # tempting rule and the wrong one: a link error is how a head most often
        # breaks a batch, and it produces no ctest block at all.
        verdict = self.rule(8896, JOBS_LINK_ERROR)
        self.assertEqual(verdict.verdict, qba.VERDICT_UNEXPLAINED)
        self.assertIsNone(verdict.implicates_head)

    def test_an_unreadable_head_diff_refuses(self) -> None:
        # A queued pull request always changes something, so an empty diff is a
        # failed read. Scoring it as "reaches nothing" would certify on a
        # measurement that never happened.
        verdict = self.rule(8773, JOBS_WASI_SDK, changes=[])
        self.assertEqual(verdict.verdict, qba.VERDICT_UNEXPLAINED)
        self.assertIsNone(verdict.implicates_head)

    def test_a_workflow_change_makes_its_own_step_failure_unexplained(self) -> None:
        verdict = self.rule(
            8773,
            JOBS_WASI_SDK,
            changes=[qba.ChangedFile(path=".github/workflows/wclap-cloudflare.yml")],
        )
        self.assertEqual(verdict.verdict, qba.VERDICT_UNEXPLAINED)

    def test_no_failing_job_read_refuses_rather_than_certifying(self) -> None:
        # A run that ejected a pull request failed. A reading that finds no
        # failing step measured the wrong thing.
        verdict = self.rule(8773, [])
        self.assertEqual(verdict.verdict, qba.VERDICT_UNEXPLAINED)
        self.assertIsNone(verdict.implicates_head)

    def test_a_stepless_job_failure_is_an_absence_not_a_cause(self) -> None:
        verdict = self.rule(
            8773, [{"name": "macos", "conclusion": "failure", "steps": []}]
        )
        self.assertEqual(verdict.verdict, qba.VERDICT_UNEXPLAINED)
        # The refusal must name the job that failed. Dropping a stepless failure
        # would also refuse -- via the "no failing job was read" path -- so the
        # verdict alone cannot tell the two apart, and the one that silently
        # discards a real failing job is the dangerous one.
        self.assertIn("macos", verdict.evidence)
        self.assertNotIn("no failing job was read", verdict.evidence)


TEST_JOBS = [
    {
        "name": "macos",
        "conclusion": "failure",
        "steps": [
            {"name": "Test (non-Windows)", "conclusion": "failure"},
            {"name": "Surface ctest failures (non-Windows)", "conclusion": "failure"},
        ],
    },
]


class CertifyAgainstTestOwnershipTests(unittest.TestCase):
    SOURCE = [qba.ChangedFile(path="core/view/src/widgets.cpp")]

    def rule(self, pr, ctest):
        return qba.certify(
            99,
            pr,
            qba.explain_failures(pr, qba.parse_failing_steps(TEST_JOBS), ctest, self.SOURCE),
        )

    def test_a_batch_mate_owning_the_failing_tests_certifies_and_names_it(self) -> None:
        ctest = qba.attribute(
            ["prepush-cannot-measure"],
            {8001: ["tools/scripts/test_prepush_cannot_measure.py"]},
        )
        verdict = self.rule(8002, ctest)
        self.assertEqual(verdict.verdict, qba.VERDICT_OTHER_PR)
        self.assertIs(verdict.implicates_head, False)
        self.assertEqual(verdict.implicated_pr, 8001)
        # The guard rejects an `other_pull_request` verdict that names this same
        # pull request, so an unattributed one certifies nothing.
        payload = verdict.as_json()
        self.assertIsInstance(payload["implicated_pr"], int)
        self.assertNotEqual(payload["implicated_pr"], payload["pr"])
        self.assertIn("prepush-cannot-measure", verdict.evidence)

    def test_the_head_owning_a_failing_test_is_positively_implicated(self) -> None:
        ctest = qba.attribute(
            ["prepush-cannot-measure"],
            {8001: ["tools/scripts/test_prepush_cannot_measure.py"]},
        )
        verdict = self.rule(8001, ctest)
        self.assertEqual(verdict.verdict, qba.VERDICT_IMPLICATED)
        self.assertIs(verdict.implicates_head, True)

    def test_co_owners_at_equal_strength_refuse_rather_than_pick_one(self) -> None:
        # The module never picks between equals in its human output either.
        # Naming one of them to Shipyard would be a coin toss dressed as evidence.
        ctest = qba.attribute(
            ["prepush-cannot-measure"],
            {
                8001: ["tools/scripts/test_prepush_cannot_measure.py"],
                8002: ["tools/other/test_prepush_cannot_measure.py"],
            },
        )
        verdict = self.rule(8003, ctest)
        self.assertEqual(verdict.verdict, qba.VERDICT_UNEXPLAINED)
        self.assertIsNone(verdict.implicates_head)

    def test_a_weak_match_naming_nobody_refuses(self) -> None:
        # "No open PR strongly matches" records what was NOT found, which
        # certifies nothing.
        ctest = qba.attribute(
            ["cmake-control-sdk-consumer"],
            {8672: ["test/cmake/test_gpu_audio_sdk_consumer.cmake"]},
        )
        verdict = self.rule(8673, ctest)
        self.assertEqual(verdict.verdict, qba.VERDICT_UNEXPLAINED)

    def test_a_test_failure_with_no_ctest_block_read_refuses(self) -> None:
        self.assertEqual(self.rule(8673, None).verdict, qba.VERDICT_UNEXPLAINED)


class GuardContractTests(unittest.TestCase):
    """The shape Shipyard's read_attributor_verdict accepts, field by field."""

    CERTIFYING = ("infrastructure", "other_pull_request")

    def test_only_the_guard_s_two_verdicts_can_certify(self) -> None:
        self.assertEqual(
            sorted((qba.VERDICT_INFRASTRUCTURE, qba.VERDICT_OTHER_PR)),
            sorted(self.CERTIFYING),
        )
        for label in (qba.VERDICT_UNEXPLAINED, qba.VERDICT_IMPLICATED):
            with self.subTest(label=label):
                self.assertNotIn(label, self.CERTIFYING)

    def test_run_id_is_an_int_because_the_guard_compares_it_exactly(self) -> None:
        # The guard rejects a verdict whose run_id != the run it resolved, and it
        # holds an int. A stringified id would silently never match.
        payload = qba.Certification(
            run_id=36245658496,
            pr=8896,
            verdict=qba.VERDICT_INFRASTRUCTURE,
            implicates_head=False,
            evidence="because",
        ).as_json()
        self.assertIsInstance(payload["run_id"], int)
        self.assertEqual(payload["run_id"], 36245658496)

    def test_a_non_certifying_verdict_carries_a_null_not_a_false(self) -> None:
        # Routed through `certify` rather than built by hand: the rule under test
        # is that a refusal reaches null, and a hand-built Certification would
        # only restate the value the test itself passed in.
        payload = qba.certify(
            1, 2, qba.Explanation(unexplained=[qba.StepFailure("macos", "Build")])
        ).as_json()
        self.assertIsNone(payload["implicates_head"])
        # `false` here would be a positive claim that the head is innocent, from a
        # reading that established nothing.
        self.assertIsNot(payload["implicates_head"], False)

    def test_other_pull_request_always_names_a_different_int(self) -> None:
        verdict = qba.certify(
            7,
            8002,
            qba.Explanation(reasons=["because"], implicated_pr=8001),
        )
        payload = verdict.as_json()
        self.assertIsInstance(payload["implicated_pr"], int)
        self.assertNotEqual(payload["implicated_pr"], payload["pr"])

    def test_infrastructure_omits_implicated_pr(self) -> None:
        payload = qba.certify(7, 8002, qba.Explanation(reasons=["because"])).as_json()
        self.assertNotIn("implicated_pr", payload)
        self.assertEqual(payload["verdict"], qba.VERDICT_INFRASTRUCTURE)

    def test_every_verdict_carries_evidence_the_guard_can_quote(self) -> None:
        for verdict in (
            qba.certify(7, 1, qba.Explanation(reasons=["r"])),
            qba.certify(7, 1, qba.Explanation(unexplained=[qba.StepFailure("j", "s")])),
            qba.certify(7, 1, qba.Explanation(head_owns_a_test=True)),
        ):
            with self.subTest(verdict=verdict.verdict):
                self.assertTrue(verdict.evidence.strip())


class UnpackJobLogsTests(unittest.TestCase):
    def archive(self, entries: dict[str, str]) -> bytes:
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as bundle:
            for name, body in entries.items():
                bundle.writestr(name, body)
        return buffer.getvalue()

    def test_the_ordinal_prefix_is_stripped_so_jobs_key_by_name(self) -> None:
        # The ordinal is the job's position in THIS run, so the same job is
        # `2_macos.txt` in one run and `3_macos.txt` in the next. Keying on it
        # would read the wrong job's log.
        logs = qba.unpack_job_logs(
            self.archive({"3_macos.txt": "hello", "macos/system.txt": "noise"})
        )
        self.assertEqual(logs, {"macos": "hello"})

    def test_a_job_name_with_spaces_and_brackets_survives(self) -> None:
        logs = qba.unpack_job_logs(
            self.archive({"2_Linux (x64) [github-hosted].txt": "body"})
        )
        self.assertIn("Linux (x64) [github-hosted]", logs)

    def test_a_corrupt_archive_reads_as_no_logs_rather_than_raising(self) -> None:
        self.assertEqual(qba.unpack_job_logs(b"not a zip"), {})


class CertifyCliTests(unittest.TestCase):
    """--certify must put exactly one JSON object on stdout and exit 0."""

    def run_cli(self, argv, jobs, changes):
        with mock.patch.object(qba, "failing_jobs", return_value=jobs), mock.patch.object(
            qba, "pr_files", return_value=changes
        ), contextlib.redirect_stdout(io.StringIO()) as out:
            code = qba.main(argv)
        return code, out.getvalue()

    def test_a_certifying_run_prints_one_object_and_exits_zero(self) -> None:
        code, text = self.run_cli(
            ["--certify", "--repo", "o/n", "--pr", "8773", "--run-id", "35973715485"],
            JOBS_WASI_SDK,
            [qba.ChangedFile(path="core/view/src/widgets.cpp")],
        )
        self.assertEqual(code, 0)
        payload = json.loads(text)
        self.assertEqual(payload["run_id"], 35973715485)
        self.assertIs(payload["implicates_head"], False)
        self.assertEqual(payload["verdict"], "infrastructure")

    def test_a_refusing_run_still_exits_zero_so_the_guard_reads_the_reason(self) -> None:
        # A non-zero exit reads to the guard as "did not rule", which discards
        # the evidence a refusal should carry into its message.
        code, text = self.run_cli(
            ["--certify", "--repo", "o/n", "--pr", "8896", "--run-id", "36245658496"],
            JOBS_LINK_ERROR,
            [qba.ChangedFile(path="core/view/src/widgets.cpp")],
        )
        self.assertEqual(code, 0)
        self.assertIsNone(json.loads(text)["implicates_head"])

    def test_certify_without_a_pr_is_a_usage_error_not_a_verdict(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(
            io.StringIO()
        ):
            code = qba.main(["--certify", "--run-id", "1"])
        self.assertEqual(code, 2)
        self.assertEqual(out.getvalue().strip(), "")

    def test_a_non_numeric_run_id_is_refused_before_any_api_call(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
            io.StringIO()
        ):
            self.assertEqual(qba.main(["--certify", "--pr", "1", "--run-id", "abc"]), 2)

    def test_the_run_id_flag_and_the_positional_must_agree(self) -> None:
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            qba.main(["111", "--run-id", "222", "--certify", "--pr", "1"])


class DeclarationTests(unittest.TestCase):
    """The declaration is the whole feature: undeclared, the guard reads nothing."""

    def config(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        return tomllib.loads((root / ".shipyard" / "config.toml").read_text())

    def test_pulp_declares_this_module_as_its_batch_attributor(self) -> None:
        command = self.config()["queue"]["attribution"]["command"]
        # An argv list, never a shell string: Shipyard rejects a string by design
        # rather than quoting a command it runs on an operator's machine.
        self.assertIsInstance(command, list)
        self.assertTrue(all(isinstance(part, str) and part for part in command))
        self.assertIn("tools/scripts/queue_batch_attribute.py", command)
        self.assertIn("--certify", command)

    def test_the_declared_command_exists_and_runs(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        command = self.config()["queue"]["attribution"]["command"]
        script = next(part for part in command if part.endswith(".py"))
        self.assertTrue((root / script).is_file(), f"{script} is declared but missing")
        # The guard appends --repo/--pr/--run-id to this argv, so the script must
        # accept all three alongside whatever the declaration already carries.
        completed = subprocess.run(
            [*command, "--help"], cwd=root, capture_output=True, text=True
        )
        self.assertEqual(completed.returncode, 0)
        for flag in ("--repo", "--pr", "--run-id", "--certify"):
            self.assertIn(flag, completed.stdout)


if __name__ == "__main__":
    unittest.main()
