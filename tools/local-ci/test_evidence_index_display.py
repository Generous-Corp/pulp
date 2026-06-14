#!/usr/bin/env python3
"""Tests for evidence index display helpers."""

from __future__ import annotations

from contextlib import redirect_stdout
import io
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("evidence_index_display.py", module_name="pulp_evidence_index_display", add_module_dir=True)


class EvidenceIndexDisplayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_evidence_command_line_fragments(self) -> None:
        self.assertEqual(
            self.mod.evidence_scope_header_line("feature/evidence", None),
            "Evidence for branch `feature/evidence`:",
        )
        self.assertEqual(
            self.mod.evidence_scope_header_line(None, "f" * 40),
            "Evidence for sha `ffffffffffff`:",
        )
        self.assertIsNone(self.mod.evidence_scope_header_line(None, None))
        self.assertEqual(self.mod.evidence_empty_line(has_header=True), "  (none)")
        self.assertEqual(self.mod.evidence_empty_line(has_header=False), "No local CI evidence recorded.")

    def test_print_summary_groups_branch_results(self) -> None:
        groups = {
            "smoke": [
                {
                    "sha": "2" * 40,
                    "completed_at": "2026-04-01T01:10:00+00:00",
                    "provenance": {"execution_kind": "direct"},
                    "targets": {"mac": {"target": "mac"}, "windows": {"target": "windows"}},
                }
            ]
        }

        buf = io.StringIO()
        with redirect_stdout(buf):
            found = self.mod.print_evidence_summary_from_groups(groups, limit=5)

        output = buf.getvalue()
        self.assertTrue(found)
        self.assertIn("smoke:", output)
        self.assertIn("mac=pass, windows=pass", output)
        self.assertIn("222222222222", output)


if __name__ == "__main__":
    unittest.main()
