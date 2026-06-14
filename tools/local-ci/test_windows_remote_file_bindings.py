#!/usr/bin/env python3
"""Tests for Windows SSH remote file dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("windows_remote_file_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsRemoteFileBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_remote_file_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.WINDOWS_REMOTE_FILE_WRITE_EXPORTS,
            *self.mod.WINDOWS_REMOTE_FILE_TRANSFER_EXPORTS,
        )

        self.assertEqual(self.mod.WINDOWS_REMOTE_FILE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_windows_remote_file_helpers_routes_groups_and_unknown_fallback(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_windows_remote_file_write_helpers") as write,
            mock.patch.object(self.mod, "install_windows_remote_file_transfer_helpers") as transfer,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_windows_remote_file_helpers(
                bindings,
                ("windows_ssh_write_text", "windows_ssh_fetch_file", "unknown_helper"),
            )

        write.assert_called_once_with(bindings, ("windows_ssh_write_text",))
        transfer.assert_called_once_with(bindings, ("windows_ssh_fetch_file",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
