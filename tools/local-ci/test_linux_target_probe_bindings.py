#!/usr/bin/env python3
"""Tests for Linux target probe/tooling compatibility bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("linux_target_probe_bindings.py")


class LinuxTargetProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_constant_and_probe_helpers(self) -> None:
        probe_expected = (
            *self.mod.LINUX_TARGET_PROBE_COMMAND_EXPORTS,
            *self.mod.LINUX_TARGET_TOOLING_STATUS_EXPORTS,
        )

        self.assertEqual(
            self.mod.LINUX_TARGET_CONSTANT_EXPORTS,
            (
                "linux_required_remote_tools",
                "linux_optional_remote_tools",
            ),
        )
        self.assertEqual(self.mod.LINUX_TARGET_PROBE_EXPORTS, probe_expected)
        self.assertEqual(len(probe_expected), len(set(probe_expected)))

    def test_install_linux_target_probe_helpers_routes_focused_groups_and_unknown_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_linux_target_probe_command_helpers") as probe_command,
            mock.patch.object(self.mod, "install_linux_target_tooling_status_helpers") as tooling_status,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_linux_target_probe_helpers(
                bindings,
                (
                    "probe_linux_launch_backend",
                    "linux_tooling_detail",
                    "custom_probe_export",
                ),
            )

        probe_command.assert_called_once_with(bindings, ("probe_linux_launch_backend",))
        tooling_status.assert_called_once_with(bindings, ("linux_tooling_detail",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_probe_export",))


if __name__ == "__main__":
    unittest.main()
