#!/usr/bin/env python3
"""Tests for Windows SSH remote JSON read dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("windows_remote_file_read_bindings.py")


class WindowsRemoteFileReadBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_read_exports_match_wrappers(self) -> None:
        expected = ("windows_ssh_read_json",)

        self.assertEqual(self.mod.WINDOWS_REMOTE_FILE_READ_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_windows_ssh_read_json_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        bindings = {
            "_windows_probe": types.SimpleNamespace(windows_ssh_read_json=runner),
            "run_windows_ssh_powershell": object(),
            "windows_contract_expand_expression": object(),
        }

        self.assertEqual(
            self.mod.windows_ssh_read_json(bindings, "win", r"%TEMP%\a.json", optional=True, timeout=17),
            {"ok": True},
        )
        self.assertEqual(captured["args"], ("win", r"%TEMP%\a.json"))
        self.assertTrue(captured["kwargs"]["optional"])
        self.assertEqual(captured["kwargs"]["timeout"], 17)
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(
            captured["kwargs"]["windows_contract_expand_expression_fn"],
            bindings["windows_contract_expand_expression"],
        )


if __name__ == "__main__":
    unittest.main()
