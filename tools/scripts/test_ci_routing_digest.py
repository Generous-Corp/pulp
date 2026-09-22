#!/usr/bin/env python3
"""Tests for the generated CI routing digest in CLAUDE.md."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
GENERATOR = HERE / "ci_routing_digest.py"
DOC = ROOT / "CLAUDE.md"
START = "<!-- generated:start id=ci-routing-digest -->"
END = "<!-- generated:end id=ci-routing-digest -->"


def _run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(GENERATOR), *args],
                          capture_output=True, text=True)


def _block(text: str) -> str:
    return text[text.index(START):text.index(END)]


class Digest(unittest.TestCase):
    def test_shipped_block_is_in_sync(self):
        proc = _run("--check")
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_block_states_the_required_gate_and_the_override(self):
        block = _block(DOC.read_text())
        self.assertIn("`PULP_LOCAL_MACOS_RUNS_ON_JSON`: REACHABLE", block)
        self.assertIn("DECLARED supply", block)
        self.assertIn("macos-overflow-local-only", block)
        self.assertIn("mergeQueueEntry", block)
        self.assertIn("rulesets", block)

    def test_a_hand_edit_inside_the_block_fails_check(self):
        text = DOC.read_text()
        edited = text.replace("`PULP_LOCAL_MACOS_RUNS_ON_JSON`: REACHABLE",
                              "`PULP_LOCAL_MACOS_RUNS_ON_JSON`: UNSERVED", 1)
        self.assertNotEqual(edited, text)
        with tempfile.TemporaryDirectory() as tmp:
            doc = Path(tmp) / "CLAUDE.md"
            doc.write_text(edited)
            proc = _run("--check", "--doc", str(doc))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("stale or hand-edited", proc.stderr)
            self.assertEqual(_run("--write", "--doc", str(doc)).returncode, 0)
            self.assertEqual(doc.read_text(), text)

    def test_missing_markers_fail_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            doc = Path(tmp) / "CLAUDE.md"
            doc.write_text("# no markers\n")
            proc = _run("--check", "--doc", str(doc))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("missing", proc.stderr)


if __name__ == "__main__":
    unittest.main()
