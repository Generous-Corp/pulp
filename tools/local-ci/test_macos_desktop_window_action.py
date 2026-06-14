#!/usr/bin/env python3
"""No-network tests for macOS window action helpers."""

from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("macos_desktop_window_action.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacOSDesktopWindowActionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_action_helpers_parse_command_results(self) -> None:
        probe_path = self.root / "macos_window_probe.swift"
        calls: list[list[str]] = []

        def run_json(cmd: list[str], **_kwargs):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout='{"activated":true}\n', stderr="")

        self.assertTrue(self.mod.activate_macos_pid(42, probe_path_fn=lambda: probe_path, run_fn=run_json)["activated"])
        self.assertEqual(calls[-1], ["swift", str(probe_path), "activate", "--pid", "42"])
        self.assertTrue(self.mod.dispatch_macos_click(10.5, 20.25, probe_path_fn=lambda: probe_path, run_fn=run_json)["activated"])
        self.assertEqual(calls[-1], ["swift", str(probe_path), "click", "--x", "10.5", "--y", "20.25"])

        activation = self.mod.activate_macos_bundle_id(
            "com.example.demo",
            run_fn=lambda cmd, **_kwargs: subprocess.CompletedProcess(cmd, 7, stdout="out\n", stderr="err\n"),
        )
        self.assertFalse(activation["activated"])
        self.assertEqual(activation["bundle_id"], "com.example.demo")
        self.assertEqual(activation["stdout"], "out")
        self.assertEqual(activation["stderr"], "err")

        quit_calls: list[list[str]] = []
        self.mod.quit_macos_bundle_id(
            "com.example.demo",
            run_fn=lambda cmd, **_kwargs: quit_calls.append(cmd) or subprocess.CompletedProcess(cmd, 0, stdout="", stderr=""),
        )
        self.assertEqual(quit_calls[0], ["osascript", "-e", 'tell application id "com.example.demo" to quit'])

    def test_process_termination_kills_after_timeout(self) -> None:
        class FakeProc:
            def __init__(self) -> None:
                self.terminated = False
                self.killed = False
                self.waits = 0

            def poll(self):
                return None if not self.terminated and not self.killed else 0

            def terminate(self) -> None:
                self.terminated = True

            def wait(self, *, timeout: float):
                self.waits += 1
                if self.waits == 1:
                    raise subprocess.TimeoutExpired("proc", timeout)
                return 0

            def kill(self) -> None:
                self.killed = True

        proc = FakeProc()
        self.mod.terminate_process(proc, timeout_secs=0.01)
        self.assertTrue(proc.terminated)
        self.assertTrue(proc.killed)


if __name__ == "__main__":
    unittest.main()
