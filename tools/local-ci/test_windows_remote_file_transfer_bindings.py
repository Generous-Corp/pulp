#!/usr/bin/env python3
"""Tests for Windows SSH remote file transfer dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("windows_remote_file_transfer_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsRemoteFileTransferBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_transfer_exports_match_wrappers(self) -> None:
        expected = (
            *self.mod.WINDOWS_REMOTE_FILE_FETCH_EXPORTS,
            *self.mod.WINDOWS_REMOTE_FILE_READ_EXPORTS,
            *self.mod.WINDOWS_REMOTE_FILE_REMOVE_EXPORTS,
        )

        self.assertEqual(self.mod.WINDOWS_REMOTE_FILE_TRANSFER_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_windows_remote_file_transfer_helpers_routes_groups_and_fallback(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_windows_remote_file_fetch_helpers") as fetch,
            mock.patch.object(self.mod, "install_windows_remote_file_read_helpers") as read,
            mock.patch.object(self.mod, "install_windows_remote_file_remove_helpers") as remove,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_windows_remote_file_transfer_helpers(
                bindings,
                ("windows_ssh_fetch_file", "windows_ssh_read_json", "windows_ssh_remove_path", "unknown_helper"),
            )

        fetch.assert_called_once_with(bindings, ("windows_ssh_fetch_file",))
        read.assert_called_once_with(bindings, ("windows_ssh_read_json",))
        remove.assert_called_once_with(bindings, ("windows_ssh_remove_path",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
