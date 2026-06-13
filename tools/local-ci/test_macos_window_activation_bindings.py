#!/usr/bin/env python3
"""Tests for macOS window activation dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("macos_window_activation_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowActivationBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_activation_exports_match_wrappers(self) -> None:
        expected = (
            "activate_macos_pid",
            "activate_macos_bundle_id",
        )

        self.assertEqual(self.mod.MACOS_WINDOW_ACTIVATION_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_activation_helpers_bind_dependencies(self) -> None:
        captured = {}

        def capture(name):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return {"activated": True}

            return inner

        run_fn = object()
        bindings = {
            "_macos_desktop": types.SimpleNamespace(
                activate_macos_pid=capture("pid"),
                activate_macos_bundle_id=capture("bundle"),
            ),
            "subprocess": types.SimpleNamespace(run=run_fn),
            "macos_window_probe_path": object(),
        }

        self.assertEqual(self.mod.activate_macos_pid(bindings, 123), {"activated": True})
        self.assertEqual(captured["pid"][0], (123,))
        self.assertIs(captured["pid"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertIs(captured["pid"][1]["run_fn"], run_fn)
        self.assertEqual(self.mod.activate_macos_bundle_id(bindings, "com.example.demo"), {"activated": True})
        self.assertEqual(captured["bundle"][0], ("com.example.demo",))
        self.assertIs(captured["bundle"][1]["run_fn"], run_fn)

    def test_activation_installer_wires_named_export(self) -> None:
        bindings = {
            "_macos_desktop": types.SimpleNamespace(
                activate_macos_bundle_id=lambda bundle_id, **kwargs: {"bundle_id": bundle_id},
            ),
            "subprocess": types.SimpleNamespace(run=object()),
        }

        self.mod.install_macos_window_activation_helpers(bindings, ("activate_macos_bundle_id",))

        self.assertEqual(bindings["activate_macos_bundle_id"]("com.example.demo"), {"bundle_id": "com.example.demo"})
        self.assertNotIn("activate_macos_pid", bindings)

    def test_activation_installer_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_macos_window_activation_helper = lambda _bindings: "future"

        self.mod.install_macos_window_activation_helpers(bindings, ("future_macos_window_activation_helper",))

        self.assertEqual(bindings["future_macos_window_activation_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
