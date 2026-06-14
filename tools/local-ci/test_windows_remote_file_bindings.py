#!/usr/bin/env python3
"""Tests for Windows SSH remote file dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


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

    def test_remote_file_helpers_bind_facade_dependencies(self) -> None:
        cases = [
            (
                "windows_ssh_write_text",
                self.mod.windows_ssh_write_text,
                ("win", r"%TEMP%\a.txt", "hello"),
                {},
                [
                    "run_windows_ssh_powershell",
                    "parse_windows_ssh_json",
                    "windows_contract_expand_expression",
                    "ps_literal",
                ],
            ),
            (
                "windows_ssh_fetch_file",
                self.mod.windows_ssh_fetch_file,
                ("win", r"%TEMP%\a.txt", Path("/tmp/a.txt")),
                {"optional": True, "timeout": 99},
                ["run_windows_ssh_powershell", "windows_contract_expand_expression"],
            ),
            (
                "windows_ssh_read_json",
                self.mod.windows_ssh_read_json,
                ("win", r"%TEMP%\a.json"),
                {"optional": True, "timeout": 17},
                ["run_windows_ssh_powershell", "windows_contract_expand_expression"],
            ),
            (
                "windows_ssh_remove_path",
                self.mod.windows_ssh_remove_path,
                ("win", r"%TEMP%\old"),
                {},
                ["run_windows_ssh_powershell", "windows_contract_expand_expression"],
            ),
        ]
        for runner_name, wrapper, args, kwargs, dependency_names in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*runner_args, **runner_kwargs):
                    captured["args"] = runner_args
                    captured["kwargs"] = runner_kwargs
                    return {"ok": True}

                bindings = {"_windows_probe": types.SimpleNamespace(**{runner_name: runner})}
                for name in dependency_names:
                    bindings[name] = object()

                self.assertEqual(wrapper(bindings, *args, **kwargs), {"ok": True})
                self.assertEqual(captured["args"], args)
                for key, value in kwargs.items():
                    self.assertEqual(captured["kwargs"][key], value)
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

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


    def test_install_windows_remote_file_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_windows_remote_file_helper = lambda _bindings: "future"

        self.mod.install_windows_remote_file_helpers(bindings, ("future_windows_remote_file_helper",))

        self.assertEqual(bindings["future_windows_remote_file_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
