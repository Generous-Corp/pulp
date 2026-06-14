#!/usr/bin/env python3
"""Tests for Windows target facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("windows_target_bindings.py")


class WindowsTargetBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_windows_target_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.WINDOWS_TARGET_SESSION_EXPORTS,
            *self.mod.WINDOWS_TARGET_PATH_EXPORTS,
            *self.mod.WINDOWS_TARGET_PROBE_EXPORTS,
        )

        self.assertEqual(self.mod.WINDOWS_TARGET_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        self.assertEqual(
            self.mod.WINDOWS_TARGET_CONSTANT_EXPORTS,
            (
                "windows_required_remote_tools",
                "windows_optional_remote_tools",
                "windows_default_remote_repo_dirname",
            ),
        )

    def test_install_windows_target_helpers_routes_each_group_and_unknown_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_windows_target_constant_helpers") as constant,
            mock.patch.object(self.mod, "install_windows_target_session_helpers") as session,
            mock.patch.object(self.mod, "install_windows_target_path_helpers") as path,
            mock.patch.object(self.mod, "install_windows_target_probe_helpers") as probe,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_windows_target_helpers(
                bindings,
                (
                    "windows_required_remote_tools",
                    "default_windows_session_task_name",
                    "windows_path_join",
                    "windows_desktop_session_user",
                    "custom_windows_target_export",
                ),
            )

        constant.assert_called_once_with(bindings, ("windows_required_remote_tools",))
        session.assert_called_once_with(bindings, ("default_windows_session_task_name",))
        path.assert_called_once_with(bindings, ("windows_path_join",))
        probe.assert_called_once_with(bindings, ("windows_desktop_session_user",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_windows_target_export",))


if __name__ == "__main__":
    unittest.main()
