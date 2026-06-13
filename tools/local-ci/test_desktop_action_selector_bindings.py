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


if __name__ == "__main__":
    unittest.main()
