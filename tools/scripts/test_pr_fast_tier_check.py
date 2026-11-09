#!/usr/bin/env python3
"""Tests for pr_fast_tier_check.py.

Run:
    python3 tools/scripts/test_pr_fast_tier_check.py
"""
from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

THIS_DIR = Path(__file__).parent
_spec = importlib.util.spec_from_file_location(
    "pr_fast_tier_check", THIS_DIR / "pr_fast_tier_check.py"
)
tier = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tier)

WORKFLOW = THIS_DIR.parent.parent / ".github" / "workflows" / "build.yml"


def listing(*tests: tuple[str, object]) -> dict:
    return {
        "tests": [
            {"name": name, "properties": [{"name": "LABELS", "value": labels}]}
            for name, labels in tests
        ]
    }


class SelectionTests(unittest.TestCase):
    def full_selection(self) -> list[str]:
        return list(tier.REQUIRED_MEMBERS)

    def test_labels_are_read_as_list_or_semicolon_string(self) -> None:
        doc = listing(
            ("a", ["python", "pr-fast"]),
            ("b", "pr-fast;lint"),
            ("c", ["python"]),
            ("d", ["pr-fast-extended"]),
        )
        self.assertEqual(tier.labelled_tests(doc), ["a", "b"])

    def test_complete_selection_passes(self) -> None:
        names = self.full_selection()
        self.assertEqual(tier.check_selection(names, min_count=len(names)), [])

    def test_empty_selection_fails(self) -> None:
        errors = tier.check_selection([], min_count=1)
        self.assertTrue(any("selects 0 test" in error for error in errors))

    def test_each_required_member_is_enforced(self) -> None:
        for dropped in tier.REQUIRED_MEMBERS:
            with self.subTest(dropped=dropped):
                names = [n for n in self.full_selection() if n != dropped]
                errors = tier.check_selection(names, min_count=0)
                self.assertTrue(any(dropped in error for error in errors))

    def test_required_members_include_the_merge_queue_ejection_checks(self) -> None:
        for name in (
            "consumption-census-drift",
            "skip-not-pass-lint",
            "inspector-protocol-registry-complete",
            "agent-capability-manifest-check",
        ):
            self.assertIn(name, tier.REQUIRED_MEMBERS)

    def test_slow_targeted_checks_are_rejected_from_fast_tier(self) -> None:
        errors = tier.check_selection(
            self.full_selection() + ["wide-non-native-selftest", "sample-region-compat-baseline"],
            min_count=0,
        )
        self.assertTrue(any("non-subsecond" in error for error in errors))


class ConfigureSelectionTests(unittest.TestCase):
    """The pre-build members: inside pr-fast, outside needs-build, complete."""

    def fast(self) -> list[str]:
        return list(tier.REQUIRED_MEMBERS) + list(tier.REQUIRED_CONFIGURE_MEMBERS)

    def test_reads_the_configure_label_and_not_its_prefix(self) -> None:
        doc = listing(
            ("a", ["pr-fast", "pr-fast-configure"]),
            ("b", "pr-fast;pr-fast-configure-extra"),
            ("c", ["pr-fast"]),
        )
        self.assertEqual(tier.labelled_tests(doc, tier.CONFIGURE_LABEL), ["a"])
        self.assertEqual(tier.labelled_tests(doc), ["a", "b", "c"])

    def test_complete_configure_selection_passes(self) -> None:
        configure = list(tier.REQUIRED_CONFIGURE_MEMBERS)
        self.assertEqual(tier.check_configure_selection(self.fast(), configure, []), [])

    def test_member_outside_pr_fast_fails(self) -> None:
        # The post-build tier drops the label, so a member outside pr-fast
        # would run nowhere on a pull-request head.
        configure = list(tier.REQUIRED_CONFIGURE_MEMBERS) + ["orphan-drift"]
        errors = tier.check_configure_selection(self.fast(), configure, [])
        self.assertTrue(any("orphan-drift" in e and "not in pr-fast" in e for e in errors))

    def test_member_that_needs_the_build_fails(self) -> None:
        configure = list(tier.REQUIRED_CONFIGURE_MEMBERS)
        errors = tier.check_configure_selection(
            self.fast(), configure, ["consumption-census-drift"])
        self.assertTrue(any("needs-build" in e for e in errors))

    def test_each_required_configure_member_is_enforced(self) -> None:
        for dropped in tier.REQUIRED_CONFIGURE_MEMBERS:
            with self.subTest(dropped=dropped):
                configure = [n for n in tier.REQUIRED_CONFIGURE_MEMBERS if n != dropped]
                errors = tier.check_configure_selection(self.fast(), configure, [])
                self.assertTrue(any(dropped in e for e in errors))

    def test_required_configure_members_are_also_required_or_listed_fast_members(self) -> None:
        # The drift checks behind most red heads; the two census/manifest ones
        # are also pinned pr-fast members.
        for name in ("consumption-census-drift", "agent-capability-manifest-check"):
            self.assertIn(name, tier.REQUIRED_MEMBERS)
            self.assertIn(name, tier.REQUIRED_CONFIGURE_MEMBERS)


class WorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8")

    def test_live_workflow_step_passes(self) -> None:
        self.assertEqual(tier.check_workflow(self.text), [])

    def test_missing_label_selection_fails(self) -> None:
        mutated = self.text.replace("-L '^pr-fast$'", "-L '^pr-slow$'")
        self.assertNotEqual(mutated, self.text)
        self.assertTrue(tier.check_workflow(mutated))

    def test_empty_selection_tolerance_fails(self) -> None:
        start = self.text.index(tier.STEP_NAME)
        end = self.text.index("- name:", start)
        section = self.text[start:end]
        mutated = self.text[:start] + section.replace(" --no-tests=error", "") + self.text[end:]
        self.assertNotEqual(mutated, self.text)
        self.assertTrue(
            any("--no-tests=error" in e for e in tier.check_workflow(mutated))
        )

    def test_writing_the_full_suite_report_fails(self) -> None:
        mutated = self.text.replace("/ctest-pr-fast.junit.xml", "/ctest.junit.xml", 1)
        self.assertNotEqual(mutated, self.text)
        self.assertTrue(
            any("ctest.junit.xml" in e for e in tier.check_workflow(mutated))
        )

    def test_step_leaving_pull_request_scope_fails(self) -> None:
        mutated = self.text.replace(
            "if: runner.os != 'Windows' && github.event_name == 'pull_request'",
            "if: runner.os != 'Windows'",
            1,
        )
        self.assertNotEqual(mutated, self.text)
        self.assertTrue(tier.check_workflow(mutated))

    def configure_section(self) -> tuple[int, int]:
        start = self.text.index("- name: " + tier.CONFIGURE_STEP_NAME)
        end = self.text.index("- name:", start + 1)
        return start, end

    def test_live_workflow_has_the_pre_build_step_before_build(self) -> None:
        self.assertEqual(tier.check_configure_workflow(self.text), [])
        start, end = self.configure_section()
        self.assertTrue(self.text[end:].startswith("- name: Build\n"))

    def test_pre_build_step_moved_after_build_fails(self) -> None:
        start, end = self.configure_section()
        section = self.text[start:end]
        rest = self.text[:start] + self.text[end:]
        build = rest.index("- name: Build\n")
        after_build = rest.index("- name:", build + 1)
        mutated = rest[:after_build] + section + rest[after_build:]
        errors = tier.check_workflow(mutated)
        self.assertTrue(any("immediately before Build" in e for e in errors), errors)

    def test_pre_build_step_on_the_contract_lane_fails(self) -> None:
        start, end = self.configure_section()
        section = self.text[start:end]
        mutated = self.text[:start] + section.replace(
            " && matrix.lane != 'contract'", "") + self.text[end:]
        self.assertNotEqual(mutated, self.text)
        self.assertTrue(any("contract lane" in e for e in tier.check_workflow(mutated)))

    def test_pre_build_step_selecting_only_pr_fast_fails(self) -> None:
        mutated = self.text.replace(" -L '^pr-fast-configure$'", "", 1)
        self.assertNotEqual(mutated, self.text)
        self.assertTrue(any("pre-build step does not select" in e
                            for e in tier.check_workflow(mutated)))

    def test_pre_build_step_tolerating_an_empty_selection_fails(self) -> None:
        start, end = self.configure_section()
        section = self.text[start:end]
        mutated = self.text[:start] + section.replace(" --no-tests=error", "") + self.text[end:]
        self.assertNotEqual(mutated, self.text)
        self.assertTrue(any("pre-build step does not fail on an empty selection" in e
                            for e in tier.check_workflow(mutated)))

    def test_unconditional_drop_from_post_build_tier_fails(self) -> None:
        # Dropping the members whether or not they ran before the build would
        # leave a head that skipped the pre-build step without them.
        mutated = self.text.replace(
            'if [ "$PR_FAST_CONFIGURE_OUTCOME" = success ]; then', "if true; then", 1)
        self.assertNotEqual(mutated, self.text)
        self.assertTrue(any("without checking that the pre-build step passed" in e
                            for e in tier.check_workflow(mutated)))

    def test_missing_pre_build_step_while_post_build_drops_fails(self) -> None:
        mutated = self.text.replace(tier.CONFIGURE_STEP_NAME, "Renamed pre-build step")
        self.assertTrue(any("expected exactly one" in e and tier.CONFIGURE_STEP_NAME in e
                            for e in tier.check_workflow(mutated)))

    def test_missing_step_fails(self) -> None:
        mutated = self.text.replace(tier.STEP_NAME, "Renamed step")
        self.assertTrue(tier.check_workflow(mutated))


if __name__ == "__main__":
    unittest.main(verbosity=2)
