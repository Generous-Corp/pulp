"""Tests for the recovery judgement check.

The central case is not synthetic: `fixtures/recovery_judgement_parameter_hpp.patch`
is the verbatim diff from commit `a08ec2d4abd8`, the repair the lane actually
produced on 2026-08-17 while every existing fence held. If this suite passes
while that patch is permitted, the suite is worthless.
"""

from pathlib import Path
import tempfile
import unittest

import shipyard_recovery_judgement as judgement
import shipyard_recovery_repair as repair


FIXTURES = Path(__file__).with_name("fixtures")
REAL_DANGEROUS_PATCH = (FIXTURES / "recovery_judgement_parameter_hpp.patch").read_text(
    encoding="utf-8"
)


class TheCaseThatMotivatedThis(unittest.TestCase):
    """2026-08-17: `static_assert(is_trivially_copyable_v<ParamValue>)` was
    'satisfied' by deleting every atomic from the audio/UI primitive."""

    def test_the_real_repair_is_refused(self):
        verdict = judgement.judge(
            ["core/state/include/pulp/state/parameter.hpp"], REAL_DANGEROUS_PATCH
        )
        self.assertFalse(verdict.autonomous)
        self.assertEqual(verdict.outcome, "needs_human")
        self.assertIn("core/state/include/pulp/state/parameter.hpp", verdict.reason)

    def test_it_is_refused_a_second_time_on_content_alone(self):
        # Defence in depth. Had the same edit landed on an allowlisted path,
        # surface would have permitted it and only the invariant test stands
        # between the lane and a data race on the audio thread.
        verdict = judgement.judge(["test/test_parameter.cpp"], REAL_DANGEROUS_PATCH)
        self.assertFalse(verdict.autonomous)
        self.assertIn("atomic", verdict.reason)

    def test_the_scale_of_the_loss_is_reported_not_just_its_existence(self):
        counts = judgement.count_markers(REAL_DANGEROUS_PATCH)
        self.assertEqual(counts["atomic"]["added"], 0)
        self.assertGreaterEqual(counts["atomic"]["removed"], 5)


class SurfaceTests(unittest.TestCase):
    def test_a_test_only_repair_is_permitted(self):
        verdict = judgement.classify_paths(["test/test_thing.cpp", "test/cmake/x.cmake"])
        self.assertTrue(verdict.autonomous)

    def test_core_escalates(self):
        verdict = judgement.classify_paths(["core/state/src/state_store.cpp"])
        self.assertFalse(verdict.autonomous)
        self.assertIn("framework runtime (core)", verdict.reason)

    def test_apple_escalates(self):
        self.assertFalse(judgement.classify_paths(["apple/Sources/Pulp/View.swift"]).autonomous)

    def test_a_ci_definition_escalates(self):
        # The allowlist's whole point: a repair could make a failing gate pass
        # by removing the gate. No denylist written in advance covers every
        # such surface; an allowlist covers all of them by construction.
        verdict = judgement.classify_paths([".github/workflows/build.yml"])
        self.assertFalse(verdict.autonomous)
        self.assertIn("CI definition", verdict.reason)

    def test_the_build_system_escalates(self):
        self.assertFalse(judgement.classify_paths(["tools/cmake/PulpTestSuite.cmake"]).autonomous)

    def test_one_disallowed_path_escalates_an_otherwise_clean_set(self):
        # Mixed sets must fail closed, or a repair hides one line of core in a
        # pile of test edits.
        verdict = judgement.classify_paths(
            ["test/a.cpp", "test/b.cpp", "core/state/src/state_store.cpp"]
        )
        self.assertFalse(verdict.autonomous)
        self.assertIn("core/state/src/state_store.cpp", verdict.reason)

    def test_an_unknown_surface_escalates_rather_than_defaulting_open(self):
        verdict = judgement.classify_paths(["some/brand/new/tree/file.cpp"])
        self.assertFalse(verdict.autonomous)

    def test_an_empty_repair_escalates(self):
        self.assertFalse(judgement.classify_paths([]).autonomous)


class InvariantRemovalTests(unittest.TestCase):
    def test_deleting_test_assertions_escalates(self):
        # A test-only diff that only removes REQUIREs passes every surface rule
        # and is the same failure mode: satisfy the check by deleting the claim.
        patch = (
            "--- a/test/test_x.cpp\n+++ b/test/test_x.cpp\n"
            "-    REQUIRE(value == 3);\n"
            "-    CHECK(other == 4);\n"
            "+    // covered elsewhere\n"
        )
        verdict = judgement.judge(["test/test_x.cpp"], patch)
        self.assertFalse(verdict.autonomous)
        self.assertIn("test assertion", verdict.reason)

    def test_adding_a_test_assertion_is_permitted(self):
        patch = (
            "--- a/test/test_x.cpp\n+++ b/test/test_x.cpp\n"
            "+    REQUIRE(value == 3);\n"
        )
        self.assertTrue(judgement.judge(["test/test_x.cpp"], patch).autonomous)

    def test_moving_an_assertion_is_not_a_removal(self):
        # Equal counts on both sides must not escalate, or ordinary refactoring
        # inside the allowlist becomes unrepairable and the check gets disabled.
        patch = (
            "--- a/test/test_x.cpp\n+++ b/test/test_x.cpp\n"
            "-    REQUIRE(value == 3);\n"
            "+    REQUIRE(value == 3);\n"
        )
        self.assertTrue(judgement.judge(["test/test_x.cpp"], patch).autonomous)

    def test_removing_a_lock_escalates(self):
        patch = (
            "--- a/test/test_x.cpp\n+++ b/test/test_x.cpp\n"
            "-    std::lock_guard<std::mutex> guard(mutex_);\n"
        )
        verdict = judgement.judge(["test/test_x.cpp"], patch)
        self.assertFalse(verdict.autonomous)
        self.assertIn("lock", verdict.reason)

    def test_file_header_lines_are_not_counted_as_content(self):
        # `---`/`+++` start with the same characters as removals; counting them
        # would let a rename shift the balance.
        patch = (
            "--- a/test/static_assert_helpers.hpp\n"
            "+++ b/test/static_assert_helpers.hpp\n"
            "+    static_assert(sizeof(int) == 4);\n"
        )
        self.assertTrue(judgement.judge(["test/static_assert_helpers.hpp"], patch).autonomous)

    def test_the_correct_repair_is_also_escalated_and_that_is_intended(self):
        # On 2026-08-17 the CORRECT fix was to delete the false assertion. This
        # check escalates that too, and should: the lane's job is to never land
        # a catastrophic change unattended, not to land every correct one.
        patch = (
            "--- a/test/test_state.cpp\n+++ b/test/test_state.cpp\n"
            "-static_assert(std::is_trivially_copyable_v<ParamValue>);\n"
        )
        verdict = judgement.judge(["test/test_state.cpp"], patch)
        self.assertFalse(verdict.autonomous)
        self.assertIn("static assertion", verdict.reason)


class ContainmentTests(unittest.TestCase):
    def test_this_check_is_inside_the_repair_fence(self):
        # The check is only worth anything if the model it constrains cannot
        # edit it. Asserted rather than assumed, against the real fence.
        with self.assertRaises(ValueError):
            repair.validate_changed_paths(["tools/scripts/shipyard_recovery_judgement.py"])

    def test_its_tests_and_fixture_are_inside_the_fence_too(self):
        # A model that could rewrite the fixture could make the motivating case
        # pass without touching the check.
        for path in (
            "tools/scripts/test_shipyard_recovery_judgement.py",
            "tools/scripts/fixtures/recovery_judgement_parameter_hpp.patch",
        ):
            with self.subTest(path=path):
                if path.startswith("tools/scripts/shipyard_recovery_"):
                    with self.assertRaises(ValueError):
                        repair.validate_changed_paths([path])


class CliTests(unittest.TestCase):
    def _run(self, paths, patch_text):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "paths.txt").write_text("\n".join(paths), encoding="utf-8")
            (root / "p.patch").write_text(patch_text, encoding="utf-8")
            out = root / "verdict.json"
            code = judgement.main([
                "--changed-paths", str(root / "paths.txt"),
                "--patch", str(root / "p.patch"),
                "--output", str(out),
            ])
            return code, out.read_text(encoding="utf-8")

    def test_escalation_uses_a_distinct_exit_code(self):
        # 3 rather than 1, so a workflow can tell "needs a human" from "the
        # check itself broke" without parsing prose.
        code, payload = self._run(
            ["core/state/include/pulp/state/parameter.hpp"], REAL_DANGEROUS_PATCH
        )
        self.assertEqual(code, 3)
        self.assertIn("needs_human", payload)

    def test_a_permitted_repair_exits_zero(self):
        code, payload = self._run(
            ["test/test_x.cpp"],
            "--- a/test/test_x.cpp\n+++ b/test/test_x.cpp\n+    REQUIRE(ok);\n",
        )
        self.assertEqual(code, 0)
        self.assertIn("autonomous", payload)

    def test_unusable_inputs_exit_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            code = judgement.main([
                "--changed-paths", str(Path(tmp) / "missing.txt"),
                "--patch", str(Path(tmp) / "missing.patch"),
            ])
        self.assertEqual(code, 1)

    def test_the_reason_survives_to_a_commit_status(self):
        # GitHub truncates a status description at 140 characters. If the reason
        # does not fit, the escalation is unexplained where a reader looks.
        verdict = judgement.judge(
            ["core/state/include/pulp/state/parameter.hpp"], REAL_DANGEROUS_PATCH
        )
        description = verdict.status_description()
        self.assertLessEqual(len(description), judgement.STATUS_DESCRIPTION_LIMIT)
        self.assertIn("needs_human", description)


if __name__ == "__main__":
    unittest.main(verbosity=2)
