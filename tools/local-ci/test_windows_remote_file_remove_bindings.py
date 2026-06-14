#!/usr/bin/env python3
"""Tests for Windows SSH remote path removal dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("windows_remote_file_remove_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsRemoteFileRemoveBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_remove_exports_match_wrappers(self) -> None:
        expected = ("windows_ssh_remove_path",)

        self.assertEqual(self.mod.WINDOWS_REMOTE_FILE_REMOVE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_windows_ssh_remove_path_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        bindings = {
            "_windows_probe": types.SimpleNamespace(windows_ssh_remove_path=runner),
            "run_windows_ssh_powershell": object(),
            "windows_contract_expand_expression": object(),
        }

        self.assertEqual(self.mod.windows_ssh_remove_path(bindings, "win", r"%TEMP%\old"), {"ok": True})
        self.assertEqual(captured["args"], ("win", r"%TEMP%\old"))
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(
            captured["kwargs"]["windows_contract_expand_expression_fn"],
            bindings["windows_contract_expand_expression"],
        )


if __name__ == "__main__":
    unittest.main()
