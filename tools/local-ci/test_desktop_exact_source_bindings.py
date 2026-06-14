#!/usr/bin/env python3
"""Tests for desktop exact-SHA source dependency bindings."""

from module_test_utils import load_local_ci_module
from unittest import mock
import unittest



def load_module():
    return load_local_ci_module("desktop_exact_source_bindings.py")


class DesktopExactSourceBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exact_source_exports_compose_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_EXACT_SOURCE_LOCAL_EXPORTS,
            *self.mod.DESKTOP_EXACT_SOURCE_MACOS_EXPORTS,
            *self.mod.DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_EXACT_SOURCE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_exact_source_installer_routes_selected_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_exact_source_local_helpers") as install_local_source,
            mock.patch.object(self.mod, "install_desktop_exact_source_macos_helpers") as install_macos,
            mock.patch.object(self.mod, "install_desktop_exact_source_remote_helpers") as install_remote,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_exact_source_helpers(
                bindings,
                (
                    "local_worktree_matches",
                    "prepare_macos_exact_sha_source",
                    "prepare_linux_exact_sha_source",
                    "custom_exact_source_export",
                ),
            )

        install_local_source.assert_called_once_with(bindings, ("local_worktree_matches",))
        install_macos.assert_called_once_with(bindings, ("prepare_macos_exact_sha_source",))
        install_remote.assert_called_once_with(bindings, ("prepare_linux_exact_sha_source",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_exact_source_export",))


if __name__ == "__main__":
    unittest.main()
