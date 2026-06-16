#!/usr/bin/env python3
"""Tests for desktop view/action facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("desktop_view_action_bindings.py")


class DesktopViewActionBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_view_action_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_VIEW_TREE_EXPORTS,
            *self.mod.DESKTOP_ACTION_GEOMETRY_EXPORTS,
            *self.mod.DESKTOP_ACTION_LABEL_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_VIEW_ACTION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_view_action_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_view_tree_helpers") as install_view_tree,
            mock.patch.object(self.mod, "install_desktop_action_geometry_helpers") as install_geometry,
            mock.patch.object(self.mod, "install_desktop_action_label_helpers") as install_label,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_view_action_helpers(
                bindings,
                ("count_view_tree_nodes", "parse_coordinate_pair", "default_desktop_label", "custom_view_action"),
            )

        install_view_tree.assert_called_once_with(bindings, ("count_view_tree_nodes",))
        install_geometry.assert_called_once_with(bindings, ("parse_coordinate_pair",))
        install_label.assert_called_once_with(bindings, ("default_desktop_label",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_view_action",))


if __name__ == "__main__":
    unittest.main()
