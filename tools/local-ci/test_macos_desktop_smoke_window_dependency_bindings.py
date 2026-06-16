#!/usr/bin/env python3
"""Tests for macOS desktop smoke window dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("macos_desktop_smoke_window_dependency_bindings.py")


class MacosDesktopSmokeWindowDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_window_dependency_exports_match_wrappers(self) -> None:
        expected = ("macos_desktop_smoke_window_dependencies",)

        self.assertEqual(self.mod.MACOS_DESKTOP_SMOKE_WINDOW_DEPENDENCY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_window_dependencies_bind_facade_values(self) -> None:
        desktop_actions = types.SimpleNamespace(
            content_size_from_window=object(),
            content_size_from_view_tree=object(),
        )
        bindings = {
            "_desktop_actions": desktop_actions,
            "wait_for_macos_window": object(),
            "wait_for_path": object(),
            "capture_macos_window": object(),
        }

        deps = self.mod.macos_desktop_smoke_window_dependencies(bindings)

        self.assertIs(deps["wait_for_macos_window_fn"], bindings["wait_for_macos_window"])
        self.assertIs(deps["content_size_from_window_fn"], desktop_actions.content_size_from_window)
        self.assertIs(deps["wait_for_path_fn"], bindings["wait_for_path"])
        self.assertIs(deps["content_size_from_view_tree_fn"], desktop_actions.content_size_from_view_tree)
        self.assertIs(deps["capture_macos_window_fn"], bindings["capture_macos_window"])


if __name__ == "__main__":
    unittest.main()
