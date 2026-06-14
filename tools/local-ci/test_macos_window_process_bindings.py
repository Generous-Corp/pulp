#!/usr/bin/env python3
"""Tests for macOS window process dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("macos_window_process_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowProcessBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_process_exports_match_wrappers(self) -> None:
        expected = (
            "terminate_process",
            "quit_macos_bundle_id",
        )

        self.assertEqual(self.mod.MACOS_WINDOW_PROCESS_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_process_helpers_bind_dependencies(self) -> None:
        captured = {}

        def terminate(*args, **kwargs):
            captured["terminate"] = (args, kwargs)

        def quit_bundle(*args, **kwargs):
            captured["quit"] = (args, kwargs)

        run_fn = object()
        bindings = {
            "_macos_desktop": types.SimpleNamespace(
                terminate_process=terminate,
                quit_macos_bundle_id=quit_bundle,
            ),
            "subprocess": types.SimpleNamespace(run=run_fn),
        }
        proc = object()

        self.mod.terminate_process(bindings, proc, timeout_secs=1.25)
        self.assertEqual(captured["terminate"], ((proc,), {"timeout_secs": 1.25}))
        self.mod.quit_macos_bundle_id(bindings, "com.example.demo")
        self.assertEqual(captured["quit"][0], ("com.example.demo",))
        self.assertIs(captured["quit"][1]["run_fn"], run_fn)

    def test_process_installer_wires_named_exports(self) -> None:
        bindings = {
            "_macos_desktop": types.SimpleNamespace(
                terminate_process=lambda proc, timeout_secs=5.0: None,
                quit_macos_bundle_id=lambda bundle_id, **kwargs: None,
            ),
            "subprocess": types.SimpleNamespace(run=object()),
        }

        self.mod.install_macos_window_process_helpers(bindings)

        self.assertIsNone(bindings["terminate_process"](object()))
        self.assertIsNone(bindings["quit_macos_bundle_id"]("com.example.demo"))


if __name__ == "__main__":
    unittest.main()
