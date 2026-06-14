#!/usr/bin/env python3
"""Tests for Linux target facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("linux_target_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_linux_target_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.LINUX_TARGET_PROBE_EXPORTS,
            *self.mod.LINUX_TARGET_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.LINUX_TARGET_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        self.assertEqual(
            self.mod.LINUX_TARGET_CONSTANT_EXPORTS,
            (
                "linux_required_remote_tools",
                "linux_optional_remote_tools",
            ),
        )

    def test_install_linux_target_helpers_routes_each_group_and_unknown_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_linux_target_constant_helpers") as constant,
            mock.patch.object(self.mod, "install_linux_target_probe_helpers") as probe,
            mock.patch.object(self.mod, "install_linux_target_command_helpers") as command,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_linux_target_helpers(
                bindings,
                (
                    "linux_required_remote_tools",
                    "probe_linux_launch_backend",
                    "remote_linux_bundle_relpath",
                    "custom_linux_target_export",
                ),
            )

        constant.assert_called_once_with(bindings, ("linux_required_remote_tools",))
        probe.assert_called_once_with(bindings, ("probe_linux_launch_backend",))
        command.assert_called_once_with(bindings, ("remote_linux_bundle_relpath",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_linux_target_export",))



if __name__ == "__main__":
    unittest.main()
