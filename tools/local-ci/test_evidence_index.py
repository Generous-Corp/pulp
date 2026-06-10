#!/usr/bin/env python3
"""Tests for evidence-index display helpers."""

import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci.py")


def load_module():
    spec = importlib.util.spec_from_file_location("pulp_local_ci", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class EvidenceIndexTests(unittest.TestCase):
    def setUp(self):
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
        self.assertEqual(
            self.mod.evidence_empty_line(has_header=False),
            "No local CI evidence recorded.",
        )


if __name__ == "__main__":
    unittest.main()
