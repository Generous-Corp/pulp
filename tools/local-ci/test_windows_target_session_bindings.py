#!/usr/bin/env python3
"""Tests for Windows target session compatibility bindings."""

from pathlib import Path
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_target_session_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetSessionBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_session_helpers(self) -> None:
        expected = (
            *self.mod.WINDOWS_TARGET_SESSION_IDENTITY_EXPORTS,
            *self.mod.WINDOWS_TARGET_SESSION_REQUEST_EXPORTS,
        )

        self.assertEqual(self.mod.WINDOWS_TARGET_SESSION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_windows_target_session_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_windows_target_session_identity_helpers") as install_identity,
            mock.patch.object(self.mod, "install_windows_target_session_request_helpers") as install_request,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_windows_target_session_helpers(
                bindings,
                (
                    "default_windows_session_task_name",
                    "desktop_target_contract",
                    "build_windows_session_agent_request",
                    "custom_windows_session_export",
                ),
            )

        install_identity.assert_called_once_with(
            bindings,
            ("default_windows_session_task_name", "desktop_target_contract"),
        )
        install_request.assert_called_once_with(bindings, ("build_windows_session_agent_request",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_windows_session_export",))


if __name__ == "__main__":
    unittest.main()
