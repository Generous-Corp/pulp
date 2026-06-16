#!/usr/bin/env python3
"""No-network tests for macos_terminal_proof.py (video-proof re-home of test_macos_desktop.py)."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest


MODULE_PATH = Path(__file__).with_name("macos_terminal_proof.py")


def load_module():
    spec = importlib.util.spec_from_file_location("macos_terminal_proof_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module



class MacosTerminalProofTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_terminal_proof_shell_script_sets_title_and_teed_logs(self) -> None:
        script = self.mod.terminal_proof_shell_script(
            cwd=Path("/repo path"),
            command_args=["/tmp/Pulp Tone", "--flag", "two words"],
            title="Pulp Video Proof abcd1234",
            stdout_path=Path("/tmp/std out.log"),
            stderr_path=Path("/tmp/std err.log"),
            returncode_path=Path("/tmp/rc file"),
            keepalive_secs=3.0,
        )

        self.assertIn("cd '/repo path'", script)
        self.assertIn("Pulp Video Proof abcd1234", script)
        self.assertIn("tee -a '/tmp/std out.log'", script)
        self.assertIn("tee -a '/tmp/std err.log'", script)
        self.assertIn("'/tmp/Pulp Tone' --flag 'two words' &", script)
        self.assertIn("sleep 3.000", script)
        self.assertIn("'/tmp/rc file'", script)

    def test_close_terminal_windows_with_title_uses_scoped_title(self) -> None:
        calls = []
        stdout_values = ["2\n", "0\n"]

        def run_osascript(cmd: list[str], **_kwargs):
            calls.append(cmd)
            script = cmd[-1] if cmd[0] == "osascript" else ""
            if "set ttyList" in script:
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            return subprocess.CompletedProcess(cmd, 0, stdout=stdout_values.pop(0), stderr="")

        result = self.mod.close_macos_terminal_windows_with_title(
            "Pulp Video Proof abcd1234",
            run_fn=run_osascript,
        )

        self.assertEqual(result["closed_count"], 2)
        self.assertEqual(result["returncode"], 0)
        close_calls = [c for c in calls if c[0] == "osascript" and "close w" in c[-1]]
        self.assertTrue(close_calls)
        self.assertIn("Pulp Video Proof abcd1234", close_calls[0][-1])

    def test_close_terminal_windows_kills_proof_tty_before_close(self) -> None:
        calls = []
        stdout_values = ["1\n", "0\n"]

        def run_fn(cmd: list[str], **_kwargs):
            calls.append(cmd)
            if cmd[0] == "osascript" and "set ttyList" in cmd[-1]:
                self.assertIn("Pulp Video Proof abcd1234", cmd[-1])
                return subprocess.CompletedProcess(cmd, 0, stdout="/dev/ttys012\n", stderr="")
            if cmd[0] == "osascript":
                return subprocess.CompletedProcess(cmd, 0, stdout=stdout_values.pop(0), stderr="")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        result = self.mod.close_macos_terminal_windows_with_title(
            "Pulp Video Proof abcd1234",
            run_fn=run_fn,
            sleep_fn=lambda _s: None,
        )

        self.assertIn("ttys012", result["killed_ttys"])
        self.assertIn(["pkill", "-t", "ttys012"], calls)
        # The tty kill happens before the close script runs.
        pkill_idx = calls.index(["pkill", "-t", "ttys012"])
        close_idx = next(i for i, c in enumerate(calls) if c[0] == "osascript" and "close w" in c[-1])
        self.assertLess(pkill_idx, close_idx)

    def test_close_terminal_windows_terminates_only_scoped_proof_terminal(self) -> None:
        calls = []

        def run_osascript(cmd: list[str], **_kwargs):
            calls.append(cmd)
            if cmd[0] == "osascript" and "set ttyList" in cmd[-1]:
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[0] == "osascript" and "set closedCount" in cmd[-1]:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="User canceled")
            if cmd[0] == "osascript":
                self.assertIn("Pulp Video Proof abcd1234", cmd[-1])
                return subprocess.CompletedProcess(cmd, 0, stdout="1234\t1\t0", stderr="")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        result = self.mod.close_macos_terminal_windows_with_title(
            "Pulp Video Proof abcd1234",
            run_fn=run_osascript,
            attempts=1,
        )

        self.assertTrue(result["terminated_terminal"])
        self.assertEqual(result["terminate_returncode"], 0)
        self.assertEqual(calls[-1], ["kill", "-TERM", "1234"])

    def test_close_terminal_windows_does_not_terminate_with_other_windows(self) -> None:
        calls = []

        def run_osascript(cmd: list[str], **_kwargs):
            calls.append(cmd)
            if cmd[0] == "osascript" and "set ttyList" in cmd[-1]:
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[0] == "osascript" and "set closedCount" in cmd[-1]:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="User canceled")
            return subprocess.CompletedProcess(cmd, 0, stdout="1234\t1\t1", stderr="")

        result = self.mod.close_macos_terminal_windows_with_title(
            "Pulp Video Proof abcd1234",
            run_fn=run_osascript,
            attempts=1,
        )

        self.assertFalse(result["terminated_terminal"])
        self.assertNotIn(["kill", "-TERM", "1234"], calls)


if __name__ == "__main__":
    unittest.main()
