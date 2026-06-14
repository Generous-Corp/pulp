#!/usr/bin/env python3
"""Tests for Linux desktop action interaction/summary dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("linux_desktop_action_interaction_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopActionInteractionDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        desktop_actions = types.SimpleNamespace(
            desktop_interaction_requested=object(),
            view_tree_inspector_summary=object(),
            pulp_app_interaction_summary=object(),
        )
        return {
            "_desktop_actions": desktop_actions,
            "default_desktop_label": object(),
            "image_change_summary": object(),
            "parse_coordinate_pair": object(),
        }, desktop_actions

    def test_interaction_dependency_exports_match_wrappers(self) -> None:
        self.assertEqual(
            self.mod.LINUX_DESKTOP_ACTION_INTERACTION_DEPENDENCY_EXPORTS,
            ("linux_desktop_action_interaction_dependencies",),
        )

    def test_interaction_dependencies_bind_facade_values(self) -> None:
        bindings, desktop_actions = self._bindings()

        deps = self.mod.linux_desktop_action_interaction_dependencies(bindings)

        self.assertIs(deps["desktop_interaction_requested_fn"], desktop_actions.desktop_interaction_requested)
        self.assertIs(deps["default_desktop_label_fn"], bindings["default_desktop_label"])
        self.assertIs(deps["image_change_summary_fn"], bindings["image_change_summary"])
        self.assertIs(deps["parse_coordinate_pair_fn"], bindings["parse_coordinate_pair"])
        self.assertIs(deps["view_tree_inspector_summary_fn"], desktop_actions.view_tree_inspector_summary)
        self.assertIs(deps["pulp_app_interaction_summary_fn"], desktop_actions.pulp_app_interaction_summary)

    def test_interaction_dependency_installer_wires_named_export(self) -> None:
        bindings, desktop_actions = self._bindings()

        self.mod.install_linux_desktop_action_interaction_dependency_helpers(bindings)

        self.assertIs(
            bindings["linux_desktop_action_interaction_dependencies"]()["desktop_interaction_requested_fn"],
            desktop_actions.desktop_interaction_requested,
        )


if __name__ == "__main__":
    unittest.main()
