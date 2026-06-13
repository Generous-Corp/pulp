#!/usr/bin/env python3
"""Tests for Windows SSH remote file fetch dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("windows_remote_file_fetch_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsRemoteFileFetchBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_fetch_exports_match_wrappers(self) -> None:
        expected = ("windows_ssh_fetch_file",)

        self.assertEqual(self.mod.WINDOWS_REMOTE_FILE_FETCH_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_windows_ssh_fetch_file_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        bindings = {
            "_windows_probe": types.SimpleNamespace(windows_ssh_fetch_file=runner),
            "run_windows_ssh_powershell": object(),
            "windows_contract_expand_expression": object(),
        }

        self.assertEqual(
            self.mod.windows_ssh_fetch_file(bindings, "win", r"%TEMP%\a.txt", Path("/tmp/a.txt"), optional=True, timeout=99),
            {"ok": True},
        )
        self.assertEqual(captured["args"], ("win", r"%TEMP%\a.txt", Path("/tmp/a.txt")))
        self.assertTrue(captured["kwargs"]["optional"])
        self.assertEqual(captured["kwargs"]["timeout"], 99)
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(
            captured["kwargs"]["windows_contract_expand_expression_fn"],
            bindings["windows_contract_expand_expression"],
        )

    def test_install_windows_remote_file_fetch_helpers_wires_named_exports(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"installed": True}

        bindings = {
            "_windows_probe": types.SimpleNamespace(windows_ssh_fetch_file=runner),
            "run_windows_ssh_powershell": object(),
            "windows_contract_expand_expression": object(),
        }

        self.mod.install_windows_remote_file_fetch_helpers(bindings)

        self.assertEqual(
            bindings["windows_ssh_fetch_file"]("win", r"%TEMP%\a.txt", Path("/tmp/a.txt")),
            {"installed": True},
        )
        self.assertEqual(captured["args"], ("win", r"%TEMP%\a.txt", Path("/tmp/a.txt")))

    def test_install_windows_remote_file_fetch_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_windows_fetch_helper = lambda _bindings: "future"

        self.mod.install_windows_remote_file_fetch_helpers(bindings, ("future_windows_fetch_helper",))

        self.assertEqual(bindings["future_windows_fetch_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
