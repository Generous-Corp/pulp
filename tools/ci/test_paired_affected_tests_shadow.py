#!/usr/bin/env python3
"""Failure and receipt tests for the observation-only paired runner."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import paired_affected_tests_shadow as _runner  # noqa: E402,F401


def command(code: str) -> list[str]:
    return [sys.executable, "-c", code]


class PairedShadowTests(unittest.TestCase):
    def test_runs_selected_and_full_in_parallel_and_records_outside_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            selected_junit = root / "selected.xml"
            full_junit = root / "full.xml"
            xml = '<testsuite><testcase name="selected"/><testcase name="outside"><failure>boom</failure></testcase></testsuite>'
            write = f"from pathlib import Path; Path({str(full_junit)!r}).write_text({xml!r}, encoding=\'utf-8\')"
            selected_junit.write_text('<testsuite><testcase name="selected"/></testsuite>', encoding="utf-8")
            (root / "selected-cwd").mkdir()
            (root / "full-cwd").mkdir()
            (root / "selection.json").write_text(json.dumps({"tests": ["selected"]}), encoding="utf-8")
            (root / "selected.json").write_text(json.dumps(command("pass")), encoding="utf-8")
            (root / "full.json").write_text(json.dumps(command(write)), encoding="utf-8")
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-cwd", str(root / "selected-cwd"),
                                   "--full-cwd", str(root / "full-cwd"),
                                   "--selected-junit", str(selected_junit), "--full-junit", str(full_junit),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out),
                                   "--repository", "O/R", "--event", "merge_group",
                                   "--base", "base", "--head", "head", "--tree", "tree"],
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            receipt = json.loads(out.read_text(encoding="utf-8"))
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
                (root / name).write_text(json.dumps(value), encoding="utf-8")
            (root / "selected-cwd").mkdir()
            (root / "full-cwd").mkdir()
            (root / "selection.json").write_text(json.dumps({"tests": []}), encoding="utf-8")
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-cwd", str(root / "selected-cwd"),
                                   "--full-cwd", str(root / "full-cwd"),
                                   "--selected-junit", str(root / "s.xml"), "--full-junit", str(root / "f.xml"),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out)],
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 7)
            receipt = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(receipt["full_outcome"], "failure")
            self.assertFalse(receipt["full_success"])
            self.assertEqual(receipt["outcome_comparison"], {
                "selected": "failure", "full": "failure", "same": True, "full_success": False,
            })
            self.assertFalse(receipt["identity_complete"])
            self.assertEqual(receipt["selected_process"]["returncode"], 3)
            self.assertEqual(receipt["full_process"]["returncode"], 7)

    def test_selected_failure_does_not_change_successful_full_exit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, value in (("selected.json", [sys.executable, "-c", "raise SystemExit(3)"]),
                                ("full.json", [sys.executable, "-c", "pass"])):
                (root / name).write_text(json.dumps(value), encoding="utf-8")
            (root / "selected-cwd").mkdir()
            (root / "full-cwd").mkdir()
            (root / "selection.json").write_text(json.dumps({"tests": []}), encoding="utf-8")
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-cwd", str(root / "selected-cwd"),
                                   "--full-cwd", str(root / "full-cwd"),
                                   "--selected-junit", str(root / "s.xml"), "--full-junit", str(root / "f.xml"),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out)],
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0)
            receipt = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(receipt["selected_process"]["returncode"], 3)
            self.assertEqual(receipt["full_process"]["returncode"], 0)

    def test_selected_timeout_terminates_process_group_and_full_still_finishes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "selected-cwd").mkdir()
            (root / "full-cwd").mkdir()
            child_pid = root / "child.pid"
            child_code = "import time; time.sleep(30)"
            selected_code = (
                "import pathlib, subprocess, sys, time; "
                f"p=subprocess.Popen([sys.executable, '-c', {child_code!r}]); "
                f"pathlib.Path({str(child_pid)!r}).write_text(str(p.pid)); time.sleep(30)"
            )
            (root / "selected.json").write_text(json.dumps(command(selected_code)), encoding="utf-8")
            (root / "full.json").write_text(json.dumps(command("pass")), encoding="utf-8")
            (root / "selection.json").write_text(json.dumps({"tests": []}), encoding="utf-8")
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-cwd", str(root / "selected-cwd"),
                                   "--full-cwd", str(root / "full-cwd"),
                                   "--selected-timeout-seconds", "0.2",
                                   "--selected-junit", str(root / "s.xml"), "--full-junit", str(root / "f.xml"),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out)],
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            receipt = json.loads(out.read_text(encoding="utf-8"))
            self.assertTrue(receipt["selected_process"]["timed_out"])
            self.assertIn(receipt["selected_process"]["termination"], {"terminated", "killed"})
            self.assertEqual(receipt["full_process"]["returncode"], 0)
            child = int(child_pid.read_text(encoding="utf-8"))
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                try:
                    os.kill(child, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.05)
            else:
                self.fail(f"selected timeout left descendant process {child} alive")
            self.assertTrue(receipt["full_success"])

    def test_shared_working_directory_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "shared").mkdir()
            for name, value in (("selected.json", command("pass")), ("full.json", command("pass"))):
                (root / name).write_text(json.dumps(value), encoding="utf-8")
            (root / "selection.json").write_text(json.dumps({"tests": []}), encoding="utf-8")
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-cwd", str(root / "shared"), "--full-cwd", str(root / "shared"),
                                   "--selected-junit", str(root / "s.xml"), "--full-junit", str(root / "f.xml"),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out)],
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0)
            receipt = json.loads(out.read_text(encoding="utf-8"))
            self.assertIn("different", receipt["selection"].get("selection_error", ""))
            self.assertEqual(receipt["full_outcome"], "success")

    def test_invalid_selected_input_does_not_suppress_authoritative_full_run(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "selected-cwd").mkdir()
            (root / "full-cwd").mkdir()
            (root / "selected.json").write_text("not json", encoding="utf-8")
            (root / "full.json").write_text(json.dumps(command("pass")), encoding="utf-8")
            (root / "selection.json").write_text(json.dumps({"tests": []}), encoding="utf-8")
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-cwd", str(root / "selected-cwd"),
                                   "--full-cwd", str(root / "full-cwd"),
                                   "--selected-junit", str(root / "s.xml"), "--full-junit", str(root / "f.xml"),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out)],
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            receipt = json.loads(out.read_text(encoding="utf-8"))
            self.assertIn("selected command unavailable", receipt["selected_process"]["error"])
            self.assertEqual(receipt["full_outcome"], "success")
            self.assertTrue(receipt["full_success"])
            self.assertEqual(receipt["full_process"]["returncode"], 0)

    def test_invalid_selection_disables_only_selected_run(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "selected-cwd").mkdir()
            (root / "full-cwd").mkdir()
            (root / "selected.json").write_text(json.dumps(command("raise SystemExit(9)")), encoding="utf-8")
            (root / "full.json").write_text(json.dumps(command("pass")), encoding="utf-8")
            (root / "selection.json").write_text("not json", encoding="utf-8")
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-cwd", str(root / "selected-cwd"),
                                   "--full-cwd", str(root / "full-cwd"),
                                   "--selected-junit", str(root / "s.xml"), "--full-junit", str(root / "f.xml"),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out)],
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            receipt = json.loads(out.read_text(encoding="utf-8"))
            self.assertIn("selection unavailable", receipt["selected_process"]["error"])
            self.assertEqual(receipt["full_outcome"], "success")
            self.assertTrue(receipt["full_success"])
            self.assertEqual(receipt["full_process"]["returncode"], 0)

    def test_unavailable_full_run_is_not_successful(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "selected-cwd").mkdir()
            (root / "full-cwd").mkdir()
            (root / "selected.json").write_text(json.dumps(command("pass")), encoding="utf-8")
            (root / "full.json").write_text("not json", encoding="utf-8")
            (root / "selection.json").write_text(json.dumps({"tests": []}), encoding="utf-8")
            out = root / "receipt.json"
            proc = subprocess.run([sys.executable, str(HERE / "paired_affected_tests_shadow.py"),
                                   "--selected-command", str(root / "selected.json"),
                                   "--full-command", str(root / "full.json"),
                                   "--selected-cwd", str(root / "selected-cwd"),
                                   "--full-cwd", str(root / "full-cwd"),
                                   "--selected-junit", str(root / "s.xml"), "--full-junit", str(root / "f.xml"),
                                   "--selection", str(root / "selection.json"), "--receipt-out", str(out)],
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            receipt = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(receipt["full_outcome"], "unavailable")
            self.assertFalse(receipt["full_success"])
            self.assertEqual(receipt["outcome_comparison"]["full"], "unavailable")
            self.assertFalse(receipt["outcome_comparison"]["same"])


if __name__ == "__main__":
    unittest.main()
