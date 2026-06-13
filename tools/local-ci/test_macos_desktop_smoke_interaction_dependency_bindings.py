#!/usr/bin/env python3
"""Tests for macOS desktop smoke interaction dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("macos_desktop_smoke_interaction_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosDesktopSmokeInteractionDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_interaction_dependency_exports_match_wrappers(self) -> None:
        expected = ("macos_desktop_smoke_interaction_dependencies",)

        self.assertEqual(self.mod.MACOS_DESKTOP_SMOKE_INTERACTION_DEPENDENCY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_interaction_dependencies_bind_facade_values(self) -> None:
        desktop_actions = types.SimpleNamespace(
            view_tree_inspector_summary=object(),
            pulp_app_interaction_summary=object(),
            desktop_click_selector=object(),
        )
        bindings = {"_desktop_actions": desktop_actions}
        for name in [
            "parse_coordinate_pair",
            "resolve_view_tree_click_point",
            "screen_point_for_content_point",
            "activate_macos_pid",
            "dispatch_macos_click",
        ]:
            bindings[name] = object()

        deps = self.mod.macos_desktop_smoke_interaction_dependencies(bindings)

        self.assertIs(deps["view_tree_inspector_summary_fn"], desktop_actions.view_tree_inspector_summary)
        self.assertIs(deps["pulp_app_interaction_summary_fn"], desktop_actions.pulp_app_interaction_summary)
        self.assertIs(deps["parse_coordinate_pair_fn"], bindings["parse_coordinate_pair"])
        self.assertIs(deps["resolve_view_tree_click_point_fn"], bindings["resolve_view_tree_click_point"])
        self.assertIs(deps["screen_point_for_content_point_fn"], bindings["screen_point_for_content_point"])
        self.assertIs(deps["activate_macos_pid_fn"], bindings["activate_macos_pid"])
        self.assertIs(deps["dispatch_macos_click_fn"], bindings["dispatch_macos_click"])
        self.assertIs(deps["desktop_click_selector_fn"], desktop_actions.desktop_click_selector)

    def test_install_interaction_dependency_helpers_wires_named_exports(self) -> None:
        desktop_actions = types.SimpleNamespace(
            view_tree_inspector_summary=object(),
            pulp_app_interaction_summary=object(),
            desktop_click_selector=object(),
        )
        bindings = {"_desktop_actions": desktop_actions}
        for name in [
            "parse_coordinate_pair",
            "resolve_view_tree_click_point",
            "screen_point_for_content_point",
            "activate_macos_pid",
            "dispatch_macos_click",
        ]:
            bindings[name] = object()

        self.mod.install_macos_desktop_smoke_interaction_dependency_helpers(bindings)

        self.assertIs(
            bindings["macos_desktop_smoke_interaction_dependencies"]()["dispatch_macos_click_fn"],
            bindings["dispatch_macos_click"],
        )


if __name__ == "__main__":
    unittest.main()
