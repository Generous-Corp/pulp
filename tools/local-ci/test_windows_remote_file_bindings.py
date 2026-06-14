#!/usr/bin/env python3
"""Tests for Windows SSH remote file dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
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

    def test_install_windows_remote_file_helpers_wires_named_exports(self) -> None:
        captured = {}

        def capture(name):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return {"name": name}

            return inner

        bindings = {
            "_windows_probe": types.SimpleNamespace(
                windows_ssh_write_text=capture("write"),
                windows_ssh_fetch_file=capture("fetch"),
                windows_ssh_read_json=capture("read"),
                windows_ssh_remove_path=capture("remove"),
            ),
            "run_windows_ssh_powershell": object(),
            "parse_windows_ssh_json": object(),
            "windows_contract_expand_expression": object(),
            "ps_literal": object(),
        }

        self.mod.install_windows_remote_file_helpers(
            bindings,
            ("windows_ssh_write_text", "windows_ssh_fetch_file", "windows_ssh_read_json", "windows_ssh_remove_path"),
        )

        self.assertEqual(bindings["windows_ssh_write_text"]("win", r"%TEMP%\a.txt", "hello"), {"name": "write"})
        self.assertEqual(
            bindings["windows_ssh_fetch_file"]("win", r"%TEMP%\a.txt", Path("/tmp/a.txt")),
            {"name": "fetch"},
        )
        self.assertEqual(bindings["windows_ssh_read_json"]("win", r"%TEMP%\a.json"), {"name": "read"})
        self.assertEqual(bindings["windows_ssh_remove_path"]("win", r"%TEMP%\old"), {"name": "remove"})
        self.assertEqual(captured["write"][0], ("win", r"%TEMP%\a.txt", "hello"))
        self.assertEqual(captured["fetch"][0], ("win", r"%TEMP%\a.txt", Path("/tmp/a.txt")))

    def test_install_windows_remote_file_helpers_preserves_unknown_fallback(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_windows_remote_file_helpers(bindings, ("unknown_helper",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
