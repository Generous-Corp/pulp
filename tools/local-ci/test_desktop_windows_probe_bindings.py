#!/usr/bin/env python3
"""Tests for desktop Windows probe dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("desktop_windows_probe_bindings.py")


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
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_windows_repo_probe_helpers") as repo,
            mock.patch.object(self.mod, "install_desktop_windows_tooling_probe_helpers") as tooling,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_windows_probe_helpers(
                bindings,
                (
                    "probe_windows_repo_checkout",
                    "probe_windows_remote_tooling",
                    "custom_desktop_windows_probe_export",
                ),
            )

        repo.assert_called_once_with(bindings, ("probe_windows_repo_checkout",))
        tooling.assert_called_once_with(bindings, ("probe_windows_remote_tooling",))
        install_local.assert_called_once_with(
            bindings,
            self.mod.__dict__,
            ("custom_desktop_windows_probe_export",),
        )


if __name__ == "__main__":
    unittest.main()
