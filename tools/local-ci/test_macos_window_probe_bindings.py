#!/usr/bin/env python3
"""Tests for macOS window probe/capture facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("macos_window_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, macos_desktop):
        return {
            "_macos_desktop": macos_desktop,
            "subprocess": types.SimpleNamespace(run=object()),
            "time": types.SimpleNamespace(time=object(), sleep=object()),
            "macos_window_probe_path": object(),
            "macos_window_info_for_pid": object(),
            "macos_window_info_for_bundle_id": object(),
            "activate_macos_bundle_id": object(),
        }

    def test_probe_wait_and_capture_bind_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        run_fn = object()
        time_fn = object()
        sleep_fn = object()
        macos_desktop = types.SimpleNamespace(
            macos_window_info_for_pid=capture("pid_info", {"windows": [{"id": 7}]}),
            macos_window_info_for_bundle_id=capture("bundle_info", {"pid": 99, "windows": [{"id": 8}]}),
            macos_accessibility_trusted=capture("trusted", True),
            wait_for_macos_window=capture("wait_pid", {"id": 7}),
            wait_for_macos_bundle_window=capture("wait_bundle", (99, {"id": 8})),
            capture_macos_window=capture("capture", None),
        )
        bindings = self._bindings(macos_desktop)
        bindings["subprocess"] = types.SimpleNamespace(run=run_fn)
        bindings["time"] = types.SimpleNamespace(time=time_fn, sleep=sleep_fn)

        self.assertEqual(self.mod.macos_window_info_for_pid(bindings, 123), {"windows": [{"id": 7}]})
        self.assertIs(captured["pid_info"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertIs(captured["pid_info"][1]["run_fn"], run_fn)
        self.assertEqual(self.mod.macos_window_info_for_bundle_id(bindings, "com.example.demo"), {"pid": 99, "windows": [{"id": 8}]})
        self.assertIs(captured["bundle_info"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertTrue(self.mod.macos_accessibility_trusted(bindings))
        self.assertIs(captured["trusted"][1]["run_fn"], run_fn)
        self.assertEqual(self.mod.wait_for_macos_window(bindings, 123, 2.0), {"id": 7})
        self.assertIs(captured["wait_pid"][1]["macos_window_info_for_pid_fn"], bindings["macos_window_info_for_pid"])
        self.assertIs(captured["wait_pid"][1]["time_fn"], time_fn)
        self.assertIs(captured["wait_pid"][1]["sleep_fn"], sleep_fn)
        self.assertEqual(self.mod.wait_for_macos_bundle_window(bindings, "com.example.demo", 2.0), (99, {"id": 8}))
        self.assertIs(captured["wait_bundle"][1]["activate_macos_bundle_id_fn"], bindings["activate_macos_bundle_id"])
        self.mod.capture_macos_window(bindings, 7, Path("/tmp/window.png"))
        self.assertIs(captured["capture"][1]["run_fn"], run_fn)
        self.assertIs(captured["capture"][1]["sleep_fn"], sleep_fn)

    def test_probe_exports_compose_focused_groups(self) -> None:
        expected = (
            *self.mod.MACOS_WINDOW_INFO_EXPORTS,
            *self.mod.MACOS_WINDOW_WAIT_EXPORTS,
            *self.mod.MACOS_WINDOW_CAPTURE_EXPORTS,
        )

        self.assertEqual(self.mod.MACOS_WINDOW_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_probe_installer_wires_selected_helpers(self) -> None:
        macos_desktop = types.SimpleNamespace(
            macos_accessibility_trusted=lambda **kwargs: True,
            capture_macos_window=lambda window_id, output_path, **kwargs: None,
        )
        bindings = self._bindings(macos_desktop)

        self.mod.install_macos_window_probe_helpers(bindings, ("macos_accessibility_trusted", "capture_macos_window"))

        self.assertTrue(bindings["macos_accessibility_trusted"]())
        self.assertIsNone(bindings["capture_macos_window"](7, Path("/tmp/window.png")))
        self.assertNotIn("wait_for_macos_window", bindings)


if __name__ == "__main__":
    unittest.main()
