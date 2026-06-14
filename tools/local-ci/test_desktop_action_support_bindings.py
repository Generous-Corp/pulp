#!/usr/bin/env python3
"""Tests for desktop action support dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("desktop_action_support_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopActionSupportBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_support_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_TARGET_SELECTION_EXPORTS,
            *self.mod.DESKTOP_VIEW_ACTION_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_ACTION_SUPPORT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_support_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_target_selection_helpers") as install_selection,
            mock.patch.object(self.mod, "install_desktop_view_action_helpers") as install_view_action,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_action_support_helpers(
                bindings,
                ("resolve_desktop_target", "count_view_tree_nodes", "unknown_helper"),
            )

        install_selection.assert_called_once_with(bindings, ("resolve_desktop_target",))
        install_view_action.assert_called_once_with(bindings, ("count_view_tree_nodes",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
