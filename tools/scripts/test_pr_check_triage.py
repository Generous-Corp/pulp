#!/usr/bin/env python3
"""Unit tests for pr_check_triage.triage (the pure comparison logic)."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "pr_check_triage", HERE / "pr_check_triage.py")
mod = importlib.util.module_from_spec(_spec)
# Register before exec so the module's own dataclasses can resolve their
# __module__ during introspection (frozen dataclass eq/repr, unittest).
sys.modules[_spec.name] = mod
_spec.loader.exec_module(mod)
triage = mod.triage


def by_name(rows):
    return {r.name: r for r in rows}


class TriageTest(unittest.TestCase):
    def test_green_checks_are_dropped(self):
        rows = triage({"macos": "SUCCESS", "lint": "NEUTRAL"}, {}, {"macos"})
        self.assertEqual(rows, [])

    def test_pre_existing_when_also_red_on_main(self):
        # The from_chars case: red on the PR AND red on main → not your change.
        rows = by_name(triage(
            {"UBSan": "FAILURE"}, {"UBSan": "FAILURE"}, required=set()))
        self.assertEqual(rows["UBSan"].verdict, "PRE-EXISTING")
        self.assertFalse(rows["UBSan"].required)

    def test_regressed_when_green_on_main(self):
        rows = by_name(triage(
            {"macos": "FAILURE"}, {"macos": "SUCCESS"}, required={"macos"}))
        self.assertEqual(rows["macos"].verdict, "REGRESSED")
        self.assertTrue(rows["macos"].required)

    def test_absent_on_main_is_new_not_blamed(self):
        rows = by_name(triage(
            {"merge-group-only": "FAILURE"}, {"macos": "SUCCESS"}, set()))
        self.assertEqual(rows["merge-group-only"].verdict, "NEW")
        self.assertEqual(rows["merge-group-only"].main_state, "ABSENT")

    def test_pending_is_reported_separately(self):
        rows = by_name(triage({"macos": "QUEUED"}, {"macos": "SUCCESS"}, {"macos"}))
        self.assertEqual(rows["macos"].verdict, "PENDING")

    def test_required_regressed_sorts_first(self):
        rows = triage(
            {"adv": "FAILURE", "macos": "FAILURE", "ubsan": "FAILURE"},
            {"adv": "FAILURE", "macos": "SUCCESS", "ubsan": "FAILURE"},
            required={"macos"})
        # required + REGRESSED (macos) must be first.
        self.assertEqual(rows[0].name, "macos")
        self.assertEqual(rows[0].verdict, "REGRESSED")

    def test_format_flags_required_regression(self):
        rows = triage({"macos": "FAILURE"}, {"macos": "SUCCESS"}, {"macos"})
        out = mod.format_rows(rows)
        self.assertIn("REQUIRED check(s) regressed", out)

    def test_format_clears_when_only_preexisting(self):
        rows = triage({"ubsan": "FAILURE"}, {"ubsan": "FAILURE"}, set())
        out = mod.format_rows(rows)
        self.assertIn("No required check was regressed", out)


if __name__ == "__main__":
    unittest.main()
