#!/usr/bin/env python3
"""Tests for macOS desktop smoke window dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("macos_desktop_smoke_window_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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

    def test_install_window_dependency_helpers_wires_named_exports(self) -> None:
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

        self.mod.install_macos_desktop_smoke_window_dependency_helpers(bindings)

        self.assertIs(
            bindings["macos_desktop_smoke_window_dependencies"]()["capture_macos_window_fn"],
            bindings["capture_macos_window"],
        )

    def test_install_window_dependency_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_macos_desktop_smoke_window_helper = lambda _bindings: "future"

        self.mod.install_macos_desktop_smoke_window_dependency_helpers(
            bindings,
            ("future_macos_desktop_smoke_window_helper",),
        )

        self.assertEqual(bindings["future_macos_desktop_smoke_window_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
