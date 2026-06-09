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


class MacosDesktopTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_app_bundle_detection_and_bundle_id_parsing(self) -> None:
        app_binary = self.root / "Demo.app" / "Contents" / "MacOS" / "Demo"
        app_binary.parent.mkdir(parents=True)
        app_binary.write_text("")

        self.assertIsNone(self.mod.detect_macos_app_bundle(None))
        self.assertIsNone(self.mod.detect_macos_app_bundle(""))
        self.assertEqual(self.mod.detect_macos_app_bundle(str(app_binary)), self.root / "Demo.app")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Missing.app"))

        info_plist = self.root / "Demo.app" / "Contents" / "Info.plist"
        info_plist.write_text("not plist")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"))
        info_plist.write_bytes(plistlib.dumps({"CFBundleIdentifier": "com.example.demo"}))
        self.assertEqual(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"), "com.example.demo")
        info_plist.write_bytes(plistlib.dumps({"CFBundleIdentifier": ""}))
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"))

    def test_swift_probe_wrappers_parse_json_and_use_injected_runner(self) -> None:
        calls: list[tuple[list[str], dict]] = []

        def fake_run(command, **kwargs):
            calls.append((command, kwargs))
            if "accessibility-trusted" in command:
                return subprocess.CompletedProcess(command, 0, stdout='{"trusted": true}', stderr="")
            if "activate" in command:
                return subprocess.CompletedProcess(command, 0, stdout='{"activated": true}', stderr="")
            if "click" in command:
                return subprocess.CompletedProcess(command, 0, stdout='{"ok": true}', stderr="")
            return subprocess.CompletedProcess(command, 0, stdout='{"pid": 123, "windows": [{"windowId": 7}]}', stderr="")

        probe_path = self.root / "macos_window_probe.swift"
        probe_path.write_text("// probe")
        probe_path_fn = lambda: probe_path

        self.assertEqual(self.mod.macos_window_probe_path(script_dir=self.root), probe_path)
        self.assertEqual(
            self.mod.macos_window_info_for_pid(123, macos_window_probe_path_fn=probe_path_fn, run_fn=fake_run)["pid"],
            123,
        )
        self.assertEqual(
            self.mod.macos_window_info_for_bundle_id(
                "com.example",
                macos_window_probe_path_fn=probe_path_fn,
                run_fn=fake_run,
            )["windows"][0]["windowId"],
            7,
        )
        self.assertTrue(
            self.mod.macos_accessibility_trusted(
                macos_window_probe_path_fn=probe_path_fn,
                run_fn=fake_run,
            )
        )
        self.assertTrue(
            self.mod.activate_macos_pid(
                123,
                macos_window_probe_path_fn=probe_path_fn,
                run_fn=fake_run,
            )["activated"]
        )
        self.assertTrue(
            self.mod.dispatch_macos_click(
                10.5,
                20,
                macos_window_probe_path_fn=probe_path_fn,
                run_fn=fake_run,
            )["ok"]
        )
        self.assertEqual(calls[0][0], ["swift", str(probe_path), "window-info", "--pid", "123"])
        self.assertEqual(calls[1][0], ["swift", str(probe_path), "window-info", "--bundle-id", "com.example"])
        self.assertEqual(calls[-1][0], ["swift", str(probe_path), "click", "--x", "10.5", "--y", "20"])
        self.assertTrue(all(call_kwargs["check"] for _command, call_kwargs in calls))

    def test_wait_for_windows_and_capture_retry_edges(self) -> None:
        sleeps: list[float] = []
        payloads = iter([
            subprocess.SubprocessError("boom"),
            {"windows": []},
            {"windows": [{"windowId": 88}]},
        ])

        def window_info(_pid: int):
            value = next(payloads)
            if isinstance(value, BaseException):
                raise value
            return value

        window = self.mod.wait_for_macos_window(
            5151,
            1.0,
            macos_window_info_for_pid_fn=window_info,
            time_fn=iter([0.0, 0.0, 0.1, 0.2]).__next__,
            sleep_fn=sleeps.append,
        )
        self.assertEqual(window["windowId"], 88)
        self.assertEqual(sleeps, [0.2, 0.2])

        activations: list[str] = []
        bundle_payloads = iter([
            {"pid": 5151, "windows": []},
            {"pid": 5151, "windows": [{"windowId": 89}]},
        ])
        pid, bundle_window = self.mod.wait_for_macos_bundle_window(
            "com.example.demo",
            1.0,
            macos_window_info_for_bundle_id_fn=lambda _bundle_id: next(bundle_payloads),
            activate_macos_bundle_id_fn=lambda bundle_id: activations.append(bundle_id) or {"activated": True},
            time_fn=iter([0.0, 0.0, 0.1]).__next__,
            sleep_fn=lambda _secs: None,
        )
        self.assertEqual(pid, 5151)
        self.assertEqual(bundle_window["windowId"], 89)
        self.assertEqual(activations, ["com.example.demo"])

        output_path = self.root / "captures" / "window.png"
        attempts = {"count": 0}

        def capture_run(command, **_kwargs):
            attempts["count"] += 1
            if attempts["count"] == 1:
                return subprocess.CompletedProcess(command, 1, stdout="not ready", stderr="")
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_bytes(b"png")
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

        capture_sleeps: list[float] = []
        self.mod.capture_macos_window(88, output_path, run_fn=capture_run, sleep_fn=capture_sleeps.append)
        self.assertEqual(attempts["count"], 2)
        self.assertEqual(output_path.read_bytes(), b"png")
        self.assertEqual(capture_sleeps, [0.2])

    def test_bundle_activation_and_quit_use_injected_runner(self) -> None:
        calls: list[tuple[list[str], dict]] = []

        def fake_run(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 1, stdout="out\n", stderr="err\n")

        activation = self.mod.activate_macos_bundle_id("com.example.demo", run_fn=fake_run)
        self.assertFalse(activation["activated"])
        self.assertEqual(activation["stdout"], "out")
        self.assertEqual(activation["stderr"], "err")

        self.mod.quit_macos_bundle_id("com.example.demo", run_fn=fake_run)
        self.assertEqual(
            calls[0][0],
            ["osascript", "-e", 'tell application id "com.example.demo" to activate'],
        )
        self.assertEqual(calls[-1][0][0], "osascript")
        self.assertIn("quit", calls[-1][0][-1])
        self.assertFalse(calls[-1][1]["check"])


if __name__ == "__main__":
    unittest.main()
