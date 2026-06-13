#!/usr/bin/env python3
"""Tests for macOS window capture facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("macos_window_capture_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowCaptureBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_capture_exports_match_wrappers(self) -> None:
        expected = ("capture_macos_window",)

        self.assertEqual(self.mod.MACOS_WINDOW_CAPTURE_EXPORTS, expected)
        self.assertTrue(callable(self.mod.capture_macos_window))

    def test_capture_binding_delegates_to_macos_desktop_module(self) -> None:
        captured = {}

        def capture_macos_window(*args, **kwargs):
            captured["capture"] = (args, kwargs)

        run_fn = object()
        sleep_fn = object()
        macos_desktop = types.SimpleNamespace(capture_macos_window=capture_macos_window)
        bindings = {
            "_macos_desktop": macos_desktop,
            "subprocess": types.SimpleNamespace(run=run_fn),
            "time": types.SimpleNamespace(sleep=sleep_fn),
        }

        self.mod.capture_macos_window(bindings, 7, Path("/tmp/window.png"))
        self.assertEqual(captured["capture"][0], (7, Path("/tmp/window.png")))
        self.assertIs(captured["capture"][1]["run_fn"], run_fn)
        self.assertIs(captured["capture"][1]["sleep_fn"], sleep_fn)


if __name__ == "__main__":
    unittest.main()
