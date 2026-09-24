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

    def test_missing_step_fails(self) -> None:
        mutated = self.text.replace(tier.STEP_NAME, "Renamed step")
        self.assertTrue(tier.check_workflow(mutated))


if __name__ == "__main__":
    unittest.main(verbosity=2)
