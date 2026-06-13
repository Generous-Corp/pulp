#!/usr/bin/env python3
"""Tests for macOS window click dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("macos_window_click_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowClickBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_click_exports_match_wrappers(self) -> None:
        self.assertEqual(self.mod.MACOS_WINDOW_CLICK_EXPORTS, ("dispatch_macos_click",))
        self.assertTrue(callable(self.mod.dispatch_macos_click))

    def test_click_helper_binds_dependencies(self) -> None:
        captured = {}

        def dispatch_click(*args, **kwargs):
            captured["click"] = (args, kwargs)
            return {"clicked": True}

        run_fn = object()
        bindings = {
            "_macos_desktop": types.SimpleNamespace(dispatch_macos_click=dispatch_click),
            "subprocess": types.SimpleNamespace(run=run_fn),
            "macos_window_probe_path": object(),
        }

        self.assertEqual(self.mod.dispatch_macos_click(bindings, 10.0, 20.0), {"clicked": True})
        self.assertEqual(captured["click"][0], (10.0, 20.0))
        self.assertIs(captured["click"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertIs(captured["click"][1]["run_fn"], run_fn)

    def test_click_installer_wires_named_export(self) -> None:
        bindings = {
            "_macos_desktop": types.SimpleNamespace(dispatch_macos_click=lambda *args, **kwargs: {"clicked": True}),
            "subprocess": types.SimpleNamespace(run=object()),
            "macos_window_probe_path": object(),
        }

        self.mod.install_macos_window_click_helpers(bindings)

        self.assertEqual(bindings["dispatch_macos_click"](10.0, 20.0), {"clicked": True})

    def test_click_installer_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_macos_window_click_helper = lambda _bindings: "future"

        self.mod.install_macos_window_click_helpers(bindings, ("future_macos_window_click_helper",))

        self.assertEqual(bindings["future_macos_window_click_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
