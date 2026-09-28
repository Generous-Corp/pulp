#!/usr/bin/env python3
"""Tests for tools/scripts/ctest_unique_names_check.py.

What must hold:
- a name registered by two different commands is reported once, with both
  commands, and the run exits 1;
- a unique inventory exits 0 and names its size;
- an empty inventory or an unreadable file exits 2, never 0 (an empty listing
  is the instrument pointed at the wrong directory, not a clean suite).

Run:
    python3 tools/scripts/test_ctest_unique_names_check.py
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ctest_unique_names_check as chk  # noqa: E402

SCRIPT = HERE / "ctest_unique_names_check.py"


def inventory(*entries: tuple[str, str]) -> dict:
    return {"tests": [{"name": n, "command": [c, n]} for n, c in entries]}


class DuplicateTests(unittest.TestCase):
    def test_same_name_from_two_commands_is_a_duplicate(self) -> None:
        doc = inventory(("Grid: column gap", "/b/test/a"), ("Grid: column gap", "/b/test/b"),
                        ("unique", "/b/test/a"))
        dups = chk.duplicates(doc)
        self.assertEqual(list(dups), ["Grid: column gap"])
        self.assertEqual(dups["Grid: column gap"], ["/b/test/a Grid: column gap", "/b/test/b Grid: column gap"])

    def test_unique_inventory_has_no_duplicates(self) -> None:
        self.assertEqual(chk.duplicates(inventory(("a", "/x"), ("b", "/x"))), {})


class CliTests(unittest.TestCase):
    def run_on(self, doc) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp, "inv.json")
            p.write_text(json.dumps(doc), encoding="utf-8")
            return subprocess.run([sys.executable, str(SCRIPT), "--show-only-json", str(p)],
                                  capture_output=True, text=True, timeout=60)

    def test_duplicates_exit_1_and_are_listed(self) -> None:
        proc = self.run_on(inventory(("dup", "/b/one"), ("dup", "/b/two"), ("ok", "/b/one")))
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("1 ctest name(s) registered more than once (of 3 entries)", proc.stdout)
        self.assertIn("'dup'", proc.stdout)
        self.assertIn("/b/two dup", proc.stdout)

    def test_unique_exits_0(self) -> None:
        proc = self.run_on(inventory(("a", "/b/one"), ("b", "/b/one")))
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertIn("OK: 2 ctest entries, all names unique", proc.stdout)

    def test_empty_inventory_exits_2(self) -> None:
        proc = self.run_on({"tests": []})
        self.assertEqual(proc.returncode, 2)
        self.assertIn("lists no tests", proc.stderr)

    def test_unreadable_inventory_exits_2(self) -> None:
        proc = subprocess.run([sys.executable, str(SCRIPT), "--show-only-json", "/nonexistent/inv.json"],
                              capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 2)


if __name__ == "__main__":
    unittest.main()
