#!/usr/bin/env python3
"""Tests for desktop action selector facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_action_selector_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopActionSelectorBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_selector_exports_match_wrappers(self) -> None:
        expected = ("windows_requires_pulp_app_selectors",)

        self.assertEqual(self.mod.DESKTOP_ACTION_SELECTOR_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_windows_selector_binds_facade_dependency(self) -> None:
        captured = {}

        def runner(args):
            captured["args"] = args
            return True

        bindings = {
            "_desktop_action_commands_cli": types.SimpleNamespace(windows_requires_pulp_app_selectors=runner),
        }
        args_obj = object()
        self.assertTrue(self.mod.windows_requires_pulp_app_selectors(bindings, args_obj))
        self.assertIs(captured["args"], args_obj)

    def test_install_desktop_action_selector_helpers_wires_named_exports(self) -> None:
        bindings = {
            "_desktop_action_commands_cli": types.SimpleNamespace(
                windows_requires_pulp_app_selectors=lambda args: True,
            ),
        }

        self.mod.install_desktop_action_selector_helpers(bindings)

        self.assertTrue(bindings["windows_requires_pulp_app_selectors"](object()))

    def test_install_desktop_action_selector_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_action_selector_helper = lambda _bindings: "future"

        self.mod.install_desktop_action_selector_helpers(bindings, ("future_desktop_action_selector_helper",))

        self.assertEqual(bindings["future_desktop_action_selector_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
