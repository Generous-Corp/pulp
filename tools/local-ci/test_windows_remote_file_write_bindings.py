#!/usr/bin/env python3
"""Tests for Windows SSH remote file write dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("windows_remote_file_write_bindings.py")


class WindowsRemoteFileWriteBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_write_exports_match_wrappers(self) -> None:
        expected = ("windows_ssh_write_text",)

        self.assertEqual(self.mod.WINDOWS_REMOTE_FILE_WRITE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_windows_ssh_write_text_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        bindings = {
            "_windows_probe": types.SimpleNamespace(windows_ssh_write_text=runner),
            "run_windows_ssh_powershell": object(),
            "parse_windows_ssh_json": object(),
            "windows_contract_expand_expression": object(),
            "ps_literal": object(),
        }

        self.assertEqual(self.mod.windows_ssh_write_text(bindings, "win", r"%TEMP%\a.txt", "hello"), {"ok": True})
        self.assertEqual(captured["args"], ("win", r"%TEMP%\a.txt", "hello"))
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(captured["kwargs"]["parse_windows_ssh_json_fn"], bindings["parse_windows_ssh_json"])
        self.assertIs(captured["kwargs"]["windows_contract_expand_expression_fn"], bindings["windows_contract_expand_expression"])
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])


if __name__ == "__main__":
    unittest.main()
