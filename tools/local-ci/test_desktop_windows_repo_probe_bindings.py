#!/usr/bin/env python3
"""Tests for desktop Windows repo probe dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("desktop_windows_repo_probe_bindings.py")


class DesktopWindowsRepoProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_repo_probe_exports_match_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_WINDOWS_REPO_CHECKOUT_PROBE_EXPORTS,
            *self.mod.DESKTOP_WINDOWS_REPO_CHECKOUT_ENSURE_EXPORTS,
        )
        self.assertEqual(self.mod.DESKTOP_WINDOWS_REPO_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_repo_probe_installer_routes_selected_groups_and_unknown_fallback(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_windows_repo_checkout_probe_helpers") as probe,
            mock.patch.object(self.mod, "install_desktop_windows_repo_checkout_ensure_helpers") as ensure,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_windows_repo_probe_helpers(
                bindings,
                ("probe_windows_repo_checkout", "ensure_windows_remote_repo_checkout", "unknown_helper"),
            )

        probe.assert_called_once_with(bindings, ("probe_windows_repo_checkout",))
        ensure.assert_called_once_with(bindings, ("ensure_windows_remote_repo_checkout",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
