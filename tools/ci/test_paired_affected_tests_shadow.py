#!/usr/bin/env python3
"""Failure and receipt tests for the observation-only paired runner."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def command(code: str) -> list[str]:
    return [sys.executable, "-c", code]


class PairedShadowTests(unittest.TestCase):
    def test_runs_selected_and_full_in_parallel_and_records_outside_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            selected_junit = root / "selected.xml"
            full_junit = root / "full.xml"
            xml = '<testsuite><testcase name="selected"/><testcase name="outside"><failure>boom</failure></testcase></testsuite>'
            write = f"from pathlib import Path; Path({str(full_junit)!r}).write_text({xml!r})"
            selected_junit.write_text('<testsuite><testcase name="selected"/></testsuite>')
            (root / "selection.json").write_text(json.dumps({"tests": ["selected"]}))
            (root / "selected.json").write_text(json.dumps(command("pass")))
            (root / "full.json").write_text(json.dumps(command(write)))
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-junit", str(selected_junit), "--full-junit", str(full_junit),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out),
                                   "--repository", "O/R", "--event", "merge_group",
                                   "--base", "base", "--head", "head", "--tree", "tree"],
                                  capture_output=True, text=True, check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            receipt = json.loads(out.read_text())
            self.assertTrue(receipt["shadow_only"])
            self.assertEqual(receipt["full_outcome"], "success")
            self.assertEqual(receipt["failed_outside_selection"], ["outside"])
            self.assertTrue(receipt["identity_complete"])
            self.assertEqual(receipt["selection_digest"], receipt["selection_digest"])

    def test_full_failure_does_not_change_runner_exit_and_missing_commands_are_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, value in (("selected.json", [sys.executable, "-c", "raise SystemExit(3)"]),
                                ("full.json", [sys.executable, "-c", "raise SystemExit(7)"])):
                (root / name).write_text(json.dumps(value))
            (root / "selection.json").write_text(json.dumps({"tests": []}))
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-junit", str(root / "s.xml"), "--full-junit", str(root / "f.xml"),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out)],
                                  capture_output=True, text=True, check=False)
            self.assertEqual(proc.returncode, 0)
            receipt = json.loads(out.read_text())
            self.assertEqual(receipt["full_outcome"], "failure")
            self.assertFalse(receipt["identity_complete"])
            self.assertEqual(receipt["selected_process"]["returncode"], 3)
            self.assertEqual(receipt["full_process"]["returncode"], 7)


if __name__ == "__main__":
    unittest.main()
