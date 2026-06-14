#!/usr/bin/env python3
"""Tests for desktop Windows probe dependency bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_windows_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopWindowsProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_windows_probe_exports_compose_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_WINDOWS_REPO_PROBE_EXPORTS,
            *self.mod.DESKTOP_WINDOWS_TOOLING_PROBE_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_WINDOWS_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_windows_probe_helpers_routes_selected_groups_and_unknown_exports(self):
        calls = []

        def repo_install(bindings, names):
            calls.append(("repo", names))

        def tooling_install(bindings, names):
            calls.append(("tooling", names))

        def fallback_install(bindings, globals_obj, names):
            calls.append(("local_fallback", names))

        self.mod.install_desktop_windows_repo_probe_helpers = repo_install
        self.mod.install_desktop_windows_tooling_probe_helpers = tooling_install
        self.mod.install_local_helpers = fallback_install

        self.mod.install_desktop_windows_probe_helpers(
            {},
            (
                "probe_windows_repo_checkout",
                "probe_windows_remote_tooling",
                "custom_desktop_windows_probe_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("repo", ("probe_windows_repo_checkout",)),
                ("tooling", ("probe_windows_remote_tooling",)),
                ("local_fallback", ("custom_desktop_windows_probe_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
