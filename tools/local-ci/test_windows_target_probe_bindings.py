#!/usr/bin/env python3
"""Tests for Windows target probe/detail compatibility bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("windows_target_probe_bindings.py")


class WindowsTargetProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_probe_helpers(self) -> None:
        expected = (
            *self.mod.WINDOWS_TARGET_TOOLING_PROBE_EXPORTS,
            *self.mod.WINDOWS_TARGET_DESKTOP_DETAIL_EXPORTS,
        )

        self.assertEqual(self.mod.WINDOWS_TARGET_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_windows_target_probe_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_windows_target_tooling_probe_helpers") as install_tooling,
            mock.patch.object(self.mod, "install_windows_target_desktop_detail_helpers") as install_desktop_detail,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_windows_target_probe_helpers(
                bindings,
                (
                    "windows_tooling_detail",
                    "windows_remote_tooling_ready",
                    "windows_desktop_session_user",
                    "custom_windows_probe_export",
                ),
            )

        install_tooling.assert_called_once_with(bindings, ("windows_tooling_detail", "windows_remote_tooling_ready"))
        install_desktop_detail.assert_called_once_with(bindings, ("windows_desktop_session_user",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_windows_probe_export",))


if __name__ == "__main__":
    unittest.main()
