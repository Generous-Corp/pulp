#!/usr/bin/env python3
"""Tests for evidence-index display helpers."""

import unittest

import evidence_index


class EvidenceIndexTests(unittest.TestCase):
    def setUp(self):
        self.mod = evidence_index

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
        self.assertEqual(
            self.mod.evidence_empty_line(has_header=False),
            "No local CI evidence recorded.",
        )


if __name__ == "__main__":
    unittest.main()
