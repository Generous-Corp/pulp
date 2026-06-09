#!/usr/bin/env python3
"""No-network tests for local-ci macOS desktop helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("macos_desktop.py")


def load_module():
    spec = importlib.util.spec_from_file_location("macos_desktop_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class MacOSDesktopTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_bundle_detection_and_plist_bundle_id(self) -> None:
        app_binary = self.root / "Demo.app" / "Contents" / "MacOS" / "Demo"
        app_binary.parent.mkdir(parents=True)
        app_binary.write_text("#!/bin/sh\n")

        self.assertIsNone(self.mod.detect_macos_app_bundle(None))
        self.assertIsNone(self.mod.detect_macos_app_bundle(""))
        self.assertEqual(self.mod.detect_macos_app_bundle(str(app_binary)), self.root / "Demo.app")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Missing.app"))

        info_plist = self.root / "Demo.app" / "Contents" / "Info.plist"
        info_plist.write_bytes(plistlib.dumps({"CFBundleIdentifier": "com.example.demo"}))
        self.assertEqual(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"), "com.example.demo")

        info_plist.write_text("not a plist")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"))

    def test_swift_and_osascript_helpers_parse_command_results(self) -> None:
        probe_path = self.root / "macos_window_probe.swift"
        calls: list[list[str]] = []

        def run_json(cmd: list[str], **_kwargs):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout='{"trusted":true,"windows":[{"id":9}],"activated":true}\n', stderr="")

        self.assertEqual(self.mod.macos_window_probe_path(self.root), probe_path)
        self.assertEqual(
            self.mod.macos_window_info_for_pid(42, probe_path_fn=lambda: probe_path, run_fn=run_json)["windows"][0]["id"],
            9,
        )
        self.assertEqual(calls[-1], ["swift", str(probe_path), "window-info", "--pid", "42"])
        self.assertTrue(self.mod.macos_accessibility_trusted(probe_path_fn=lambda: probe_path, run_fn=run_json))
        self.assertEqual(calls[-1], ["swift", str(probe_path), "accessibility-trusted"])
        self.assertTrue(self.mod.activate_macos_pid(42, probe_path_fn=lambda: probe_path, run_fn=run_json)["activated"])
        self.assertEqual(calls[-1], ["swift", str(probe_path), "activate", "--pid", "42"])
        self.assertTrue(self.mod.dispatch_macos_click(10.5, 20.25, probe_path_fn=lambda: probe_path, run_fn=run_json)["activated"])
        self.assertEqual(calls[-1], ["swift", str(probe_path), "click", "--x", "10.5", "--y", "20.25"])

        def run_osascript(cmd: list[str], **_kwargs):
            return subprocess.CompletedProcess(cmd, 7, stdout="out\n", stderr="err\n")

        activation = self.mod.activate_macos_bundle_id("com.example.demo", run_fn=run_osascript)
        self.assertFalse(activation["activated"])
        self.assertEqual(activation["bundle_id"], "com.example.demo")
        self.assertEqual(activation["stdout"], "out")
        self.assertEqual(activation["stderr"], "err")

        quit_calls: list[list[str]] = []

        def run_quit(cmd: list[str], **_kwargs):
            quit_calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        self.mod.quit_macos_bundle_id("com.example.demo", run_fn=run_quit)
        self.assertEqual(quit_calls[0], ["osascript", "-e", 'tell application id "com.example.demo" to quit'])

    def test_wait_helpers_retry_until_windows_are_visible(self) -> None:
        now = [0.0]

        def time_fn() -> float:
            return now[0]

        def sleep_fn(amount: float) -> None:
            now[0] += amount

        pid_payloads = [{"windows": []}, {"windows": [{"id": 7}]}]

        def info_for_pid(_pid: int) -> dict:
            return pid_payloads.pop(0)

        self.assertEqual(
            self.mod.wait_for_macos_window(123, 1.0, macos_window_info_for_pid_fn=info_for_pid, time_fn=time_fn, sleep_fn=sleep_fn),
            {"id": 7},
        )

        bundle_payloads = [{"pid": None, "windows": []}, {"pid": 456, "windows": [{"id": 8}]}]
        activations: list[str] = []

        def info_for_bundle(_bundle_id: str) -> dict:
            return bundle_payloads.pop(0)

        def activate(bundle_id: str) -> dict:
            activations.append(bundle_id)
            return {"activated": True, "stderr": ""}

        self.assertEqual(
            self.mod.wait_for_macos_bundle_window(
                "com.example.demo",
                1.0,
                macos_window_info_for_bundle_id_fn=info_for_bundle,
                activate_macos_bundle_id_fn=activate,
                time_fn=time_fn,
                sleep_fn=sleep_fn,
            ),
            (456, {"id": 8}),
        )
        self.assertEqual(activations, ["com.example.demo"])

    def test_capture_retry_and_process_termination(self) -> None:
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
