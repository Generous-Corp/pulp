#!/usr/bin/env python3
"""Tests for desktop source command rewrite dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("desktop_source_rewrite_bindings.py")


class DesktopSourceRewriteBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_source_rewrite_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REWRITE_ROOT_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_SOURCE_REWRITE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_source_rewrite_installer_routes_selected_groups_and_unknown_fallback(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_source_rewrite_command_helpers") as command,
            mock.patch.object(self.mod, "install_desktop_source_rewrite_root_helpers") as root,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_source_rewrite_helpers(
                bindings,
                ("command_path_rewrite_candidate", "rewrite_launch_command_for_posix_root", "unknown_helper"),
            )

        command.assert_called_once_with(bindings, ("command_path_rewrite_candidate",))
        root.assert_called_once_with(bindings, ("rewrite_launch_command_for_posix_root",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
