#!/usr/bin/env python3
"""Tests for remote exact-source preparation compatibility bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("desktop_exact_source_remote_bindings.py")


class DesktopExactSourceRemoteBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_remote_facade_reexports_focused_helpers(self):
        self.assertEqual(self.mod.prepare_linux_exact_sha_source.__module__, "desktop_exact_source_linux_bindings")
        self.assertEqual(self.mod.prepare_windows_exact_sha_source.__module__, "desktop_exact_source_windows_bindings")

    def test_remote_exports_compose_platform_groups(self):
        expected = (
            *self.mod.DESKTOP_EXACT_SOURCE_LINUX_EXPORTS,
            *self.mod.DESKTOP_EXACT_SOURCE_WINDOWS_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_remote_installer_routes_selected_platform_helpers(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_exact_source_linux_helpers") as linux,
            mock.patch.object(self.mod, "install_desktop_exact_source_windows_helpers") as windows,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_exact_source_remote_helpers(
                bindings,
                ("prepare_linux_exact_sha_source", "prepare_windows_exact_sha_source", "custom"),
            )

        linux.assert_called_once_with(bindings, ("prepare_linux_exact_sha_source",))
        windows.assert_called_once_with(bindings, ("prepare_windows_exact_sha_source",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
