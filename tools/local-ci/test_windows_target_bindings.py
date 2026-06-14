#!/usr/bin/env python3
"""Tests for Windows target facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("windows_target_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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
        calls = []

        def constant_install(bindings, names):
            calls.append(("constant", names))

        def session_install(bindings, names):
            calls.append(("session", names))

        def path_install(bindings, names):
            calls.append(("path", names))

        def probe_install(bindings, names):
            calls.append(("probe", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_windows_target_constant_helpers = constant_install
        self.mod.install_windows_target_session_helpers = session_install
        self.mod.install_windows_target_path_helpers = path_install
        self.mod.install_windows_target_probe_helpers = probe_install
        self.mod.install_local_helpers = local_install

        self.mod.install_windows_target_helpers(
            {},
            (
                "windows_required_remote_tools",
                "default_windows_session_task_name",
                "windows_path_join",
                "windows_desktop_session_user",
                "custom_windows_target_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("constant", ("windows_required_remote_tools",)),
                ("session", ("default_windows_session_task_name",)),
                ("path", ("windows_path_join",)),
                ("probe", ("windows_desktop_session_user",)),
                ("local", ("custom_windows_target_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
