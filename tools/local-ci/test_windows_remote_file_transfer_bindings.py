#!/usr/bin/env python3
"""Tests for Windows SSH remote file transfer dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


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

    def test_transfer_helpers_bind_facade_dependencies(self) -> None:
        cases = [
            (
                "windows_ssh_fetch_file",
                self.mod.windows_ssh_fetch_file,
                ("win", r"%TEMP%\a.txt", Path("/tmp/a.txt")),
                {"optional": True, "timeout": 99},
            ),
            (
                "windows_ssh_read_json",
                self.mod.windows_ssh_read_json,
                ("win", r"%TEMP%\a.json"),
                {"optional": True, "timeout": 17},
            ),
            (
                "windows_ssh_remove_path",
                self.mod.windows_ssh_remove_path,
                ("win", r"%TEMP%\old"),
                {},
            ),
        ]
        for runner_name, wrapper, args, kwargs in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*runner_args, **runner_kwargs):
                    captured["args"] = runner_args
                    captured["kwargs"] = runner_kwargs
                    return {"ok": True}

                bindings = {
                    "_windows_probe": types.SimpleNamespace(**{runner_name: runner}),
                    "run_windows_ssh_powershell": object(),
                    "windows_contract_expand_expression": object(),
                }

                self.assertEqual(wrapper(bindings, *args, **kwargs), {"ok": True})
                self.assertEqual(captured["args"], args)
                for key, value in kwargs.items():
                    self.assertEqual(captured["kwargs"][key], value)
                self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
                self.assertIs(
                    captured["kwargs"]["windows_contract_expand_expression_fn"],
                    bindings["windows_contract_expand_expression"],
                )

    def test_install_windows_remote_file_transfer_helpers_wires_named_exports(self) -> None:
        captured = {}

        def capture(name):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return {"name": name}

            return inner

        bindings = {
            "_windows_probe": types.SimpleNamespace(
                windows_ssh_fetch_file=capture("fetch"),
                windows_ssh_read_json=capture("read"),
                windows_ssh_remove_path=capture("remove"),
            ),
            "run_windows_ssh_powershell": object(),
            "windows_contract_expand_expression": object(),
        }

        self.mod.install_windows_remote_file_transfer_helpers(bindings)

        self.assertEqual(
            bindings["windows_ssh_fetch_file"]("win", r"%TEMP%\a.txt", Path("/tmp/a.txt")),
            {"name": "fetch"},
        )
        self.assertEqual(bindings["windows_ssh_read_json"]("win", r"%TEMP%\a.json"), {"name": "read"})
        self.assertEqual(bindings["windows_ssh_remove_path"]("win", r"%TEMP%\old"), {"name": "remove"})
        self.assertEqual(captured["fetch"][0], ("win", r"%TEMP%\a.txt", Path("/tmp/a.txt")))



if __name__ == "__main__":
    unittest.main()
