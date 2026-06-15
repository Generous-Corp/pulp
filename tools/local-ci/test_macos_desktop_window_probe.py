#!/usr/bin/env python3
"""No-network tests for macOS window probe helpers."""

from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("macos_desktop_window_probe.py")


class MacOSDesktopWindowProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_swift_helpers_parse_command_results(self) -> None:
        probe_path = self.root / "macos_window_probe.swift"
        calls: list[list[str]] = []

        def run_json(cmd: list[str], **_kwargs):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout='{"trusted":true,"windows":[{"id":9}]}\n', stderr="")

        self.assertEqual(
            self.mod.macos_window_info_for_pid(42, probe_path_fn=lambda: probe_path, run_fn=run_json)["windows"][0]["id"],
            9,
        )
        self.assertEqual(calls[-1], ["swift", str(probe_path), "window-info", "--pid", "42"])
        self.assertTrue(self.mod.macos_accessibility_trusted(probe_path_fn=lambda: probe_path, run_fn=run_json))
        self.assertEqual(calls[-1], ["swift", str(probe_path), "accessibility-trusted"])

    def test_wait_helpers_retry_until_windows_are_visible(self) -> None:
        now = [0.0]

        def time_fn() -> float:
            return now[0]

        def sleep_fn(amount: float) -> None:
            now[0] += amount

        pid_payloads = [{"windows": []}, {"windows": [{"id": 7}]}]
        self.assertEqual(
            self.mod.wait_for_macos_window(
                123,
                1.0,
                macos_window_info_for_pid_fn=lambda _pid: pid_payloads.pop(0),
                time_fn=time_fn,
                sleep_fn=sleep_fn,
            ),
            {"id": 7},
        )

        bundle_payloads = [{"pid": None, "windows": []}, {"pid": 456, "windows": [{"id": 8}]}]
        activations: list[str] = []
        self.assertEqual(
            self.mod.wait_for_macos_bundle_window(
                "com.example.demo",
                1.0,
                macos_window_info_for_bundle_id_fn=lambda _bundle_id: bundle_payloads.pop(0),
                activate_macos_bundle_id_fn=lambda bundle_id: activations.append(bundle_id) or {"activated": True, "stderr": ""},
                time_fn=time_fn,
                sleep_fn=sleep_fn,
            ),
            (456, {"id": 8}),
        )
        self.assertEqual(activations, ["com.example.demo"])

    def test_capture_retries_until_output_exists(self) -> None:
        output_path = self.root / "screens" / "window.png"
        calls = [0]

        def run_capture(cmd: list[str], **_kwargs):
            calls[0] += 1
            if calls[0] == 2:
                Path(cmd[-1]).write_text("png")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="busy")

        self.mod.capture_macos_window(88, output_path, run_fn=run_capture, sleep_fn=lambda _amount: None)
        self.assertEqual(calls[0], 2)
        self.assertTrue(output_path.exists())


if __name__ == "__main__":
    unittest.main()
