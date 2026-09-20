#!/usr/bin/env python3
"""Unit tests for pr_check_triage comparison and check-run decoding."""

from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

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

    # ── cancelled / timed out: absence of evidence, not failure ─────────────

    def test_cancelled_is_no_evidence_not_regressed(self):
        # Green on main, cancelled here. Reading that as a regression sends
        # someone to debug a run that never produced a verdict.
        rows = by_name(triage(
            {"macos": "CANCELLED"}, {"macos": "SUCCESS"}, required={"macos"}))
        self.assertEqual(rows["macos"].verdict, "NO-EVIDENCE")
        self.assertEqual(rows["macos"].pr_state, "CANCELLED")

    def test_cancelled_is_no_evidence_not_pre_existing(self):
        rows = by_name(triage(
            {"UBSan": "CANCELLED"}, {"UBSan": "FAILURE"}, required=set()))
        self.assertEqual(rows["UBSan"].verdict, "NO-EVIDENCE")

    def test_timed_out_is_no_evidence(self):
        rows = by_name(triage(
            {"linux": "TIMED_OUT"}, {"linux": "SUCCESS"}, required=set()))
        self.assertEqual(rows["linux"].verdict, "NO-EVIDENCE")

    def test_genuine_failures_stay_red(self):
        # The control for the two tests above: real verdicts must not drift
        # into NO-EVIDENCE when the cancelled states leave RED.
        rows = by_name(triage(
            {"macos": "FAILURE", "ubsan": "FAILURE", "acted": "ACTION_REQUIRED",
             "errd": "ERROR"},
            {"macos": "SUCCESS", "ubsan": "FAILURE", "acted": "SUCCESS",
             "errd": "SUCCESS"},
            required={"macos"}))
        self.assertEqual(rows["macos"].verdict, "REGRESSED")
        self.assertEqual(rows["ubsan"].verdict, "PRE-EXISTING")
        self.assertEqual(rows["acted"].verdict, "REGRESSED")
        self.assertEqual(rows["errd"].verdict, "REGRESSED")

    def test_cancelled_main_run_is_not_a_baseline(self):
        # Main's run was cancelled, so it cannot establish "also red on main".
        rows = by_name(triage(
            {"macos": "FAILURE"}, {"macos": "CANCELLED"}, required={"macos"}))
        self.assertEqual(rows["macos"].verdict, "NEW")
        self.assertNotEqual(rows["macos"].verdict, "PRE-EXISTING")

    def test_no_evidence_sorts_above_pending_and_pre_existing(self):
        rows = triage(
            {"cancelled": "CANCELLED", "queued": "QUEUED", "old": "FAILURE"},
            {"cancelled": "SUCCESS", "queued": "SUCCESS", "old": "FAILURE"},
            required=set())
        self.assertEqual([r.verdict for r in rows],
                         ["NO-EVIDENCE", "PENDING", "PRE-EXISTING"])

    def test_format_flags_no_evidence_and_withholds_the_all_clear(self):
        rows = triage({"macos": "CANCELLED"}, {"macos": "SUCCESS"}, {"macos"})
        out = mod.format_rows(rows)
        self.assertIn("NO-EVIDENCE", out)
        self.assertIn("rerun", out)
        self.assertIn("not an all-clear", out)
        self.assertNotIn("REQUIRED check(s) regressed", out)

    def test_format_all_clear_survives_an_advisory_no_evidence(self):
        # An advisory lane with no verdict is still reported, but it does not
        # withdraw the all-clear about required checks.
        rows = triage({"adv": "CANCELLED"}, {"adv": "SUCCESS"}, set())
        out = mod.format_rows(rows)
        self.assertIn("No required check was regressed", out)
        self.assertNotIn("not an all-clear", out)
        self.assertIn("produced NO evidence", out)


class CheckRunDecodeTest(unittest.TestCase):
    def test_checks_for_sha_slurps_more_than_one_hundred_runs(self):
        first_page = {
            "check_runs": [
                {"name": f"check-{index}", "conclusion": "success"}
                for index in range(100)
            ]
        }
        second_page = {
            "check_runs": [
                {"name": "check-100", "conclusion": "failure"}
            ]
        }

        def fake_gh(_gh, *args):
            if "--paginate" in args:
                self.assertIn("--slurp", args)
                self.assertEqual(
                    args[-1],
                    "repos/owner/repo/commits/deadbeef/check-runs"
                    "?filter=latest&per_page=100")
                return json.dumps([first_page, second_page])
            return json.dumps({
                "statuses": [{"context": "legacy", "state": "success"}]
            })

        with patch.object(mod, "_gh", side_effect=fake_gh):
            checks = mod._checks_for_sha("ghapp", "owner/repo", "deadbeef")

        self.assertEqual(len(checks), 102)
        self.assertEqual(checks["check-0"], "success")
        self.assertEqual(checks["check-100"], "failure")
        self.assertEqual(checks["legacy"], "success")

    def test_checks_for_sha_rejects_malformed_paginated_output(self):
        with patch.object(mod, "_gh", return_value='[{"check_runs": []},'):
            with self.assertRaises(json.JSONDecodeError):
                mod._checks_for_sha("ghapp", "owner/repo", "deadbeef")


if __name__ == "__main__":
    unittest.main()
