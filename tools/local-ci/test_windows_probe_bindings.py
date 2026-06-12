#!/usr/bin/env python3
"""Tests for Windows probe facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("windows_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, windows_probe):
        return {
            "_windows_probe": windows_probe,
            "SCRIPT_DIR": Path("/repo/tools/local-ci"),
            "subprocess": types.SimpleNamespace(run=object()),
        }

    def test_simple_wrappers_delegate_to_windows_probe_module(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        windows_probe = types.SimpleNamespace(
            ps_literal=capture("literal", "escaped"),
            windows_ssh_powershell_command=capture("command", ["ssh", "win", "powershell"]),
            parse_windows_ssh_json=capture("json", {"ok": True}),
            windows_session_agent_template_path=capture("template", Path("/repo/tools/local-ci/windows-session-agent.ps1")),
        )
        bindings = self._bindings(windows_probe)

        self.assertEqual(self.mod.ps_literal(bindings, "value"), "escaped")
        self.assertEqual(captured["literal"][0], ("value",))
        self.assertEqual(self.mod.windows_ssh_powershell_command(bindings, "win"), ["ssh", "win", "powershell"])
        self.assertEqual(captured["command"][0], ("win",))
        self.assertEqual(self.mod.parse_windows_ssh_json(bindings, "{}"), {"ok": True})
        self.assertEqual(captured["json"][0], ("{}",))
        self.assertEqual(
            self.mod.windows_session_agent_template_path(bindings),
            Path("/repo/tools/local-ci/windows-session-agent.ps1"),
        )
        self.assertEqual(captured["template"][0], (bindings["SCRIPT_DIR"],))

    def test_run_windows_ssh_powershell_binds_ssh_runner(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return "completed"

        windows_probe = types.SimpleNamespace(run_windows_ssh_powershell=runner)
        bindings = self._bindings(windows_probe)
        bindings["run_ssh_subprocess"] = object()

        self.assertEqual(self.mod.run_windows_ssh_powershell(bindings, "win", "Get-Date", timeout=42), "completed")
        self.assertEqual(captured["args"], ("win", "Get-Date"))
        self.assertEqual(captured["kwargs"]["timeout"], 42)
        self.assertIs(captured["kwargs"]["run_ssh_subprocess_fn"], bindings["run_ssh_subprocess"])

    def test_windows_contract_expand_expression_binds_facade_literal(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return "$env:TEMP"

        windows_probe = types.SimpleNamespace(windows_contract_expand_expression=runner)
        bindings = self._bindings(windows_probe)
        bindings["ps_literal"] = object()

        self.assertEqual(self.mod.windows_contract_expand_expression(bindings, "%TEMP%"), "$env:TEMP")
        self.assertEqual(captured["args"], ("%TEMP%",))
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])

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

                windows_probe = types.SimpleNamespace(**{runner_name: runner})
                bindings = self._bindings(windows_probe)
                for name in dependency_names:
                    bindings[name] = object()

                self.assertEqual(wrapper(bindings, *args, **kwargs), {"ok": True})
                self.assertEqual(captured["args"], args)
                for key, value in kwargs.items():
                    self.assertEqual(captured["kwargs"][key], value)
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_session_agent_helpers_bind_facade_dependencies(self) -> None:
        cases = [
            (
                "bootstrap_windows_session_agent",
                self.mod.bootstrap_windows_session_agent,
                ("win", {"task_name": "Pulp"}),
                [
                    "windows_session_agent_template_path",
                    "windows_ssh_write_text",
                    "run_windows_ssh_powershell",
                    "parse_windows_ssh_json",
                    "windows_contract_expand_expression",
                    "ps_literal",
                ],
            ),
            (
                "start_windows_session_agent_task",
                self.mod.start_windows_session_agent_task,
                ("win", {"task_name": "Pulp"}),
                ["run_windows_ssh_powershell", "parse_windows_ssh_json", "ps_literal"],
            ),
        ]
        for runner_name, wrapper, args, dependency_names in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*runner_args, **kwargs):
                    captured["args"] = runner_args
                    captured["kwargs"] = kwargs
                    return {"started": True}

                windows_probe = types.SimpleNamespace(**{runner_name: runner})
                bindings = self._bindings(windows_probe)
                for name in dependency_names:
                    bindings[name] = object()

                self.assertEqual(wrapper(bindings, *args), {"started": True})
                self.assertEqual(captured["args"], args)
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_probe_windows_ssh_cmake_settings_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return ("ARM64", "C:/VS")

        windows_probe = types.SimpleNamespace(probe_windows_ssh_cmake_settings=runner)
        bindings = self._bindings(windows_probe)
        bindings["windows_ssh_powershell_command"] = object()
        bindings["ps_literal"] = object()

        self.assertEqual(
            self.mod.probe_windows_ssh_cmake_settings(bindings, "win", "Visual Studio 17 2022", "", ""),
            ("ARM64", "C:/VS"),
        )
        self.assertEqual(captured["args"], ("win", "Visual Studio 17 2022", "", ""))
        self.assertIs(captured["kwargs"]["windows_ssh_powershell_command_fn"], bindings["windows_ssh_powershell_command"])
        self.assertIs(captured["kwargs"]["run_fn"], bindings["subprocess"].run)
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])

    def test_install_windows_probe_helpers_wires_named_exports(self) -> None:
        captured = {}

        def run_powershell(host, script, **kwargs):
            captured["run"] = (host, script, kwargs)
            return "completed"

        def expand(raw_value, **kwargs):
            captured["expand"] = (raw_value, kwargs)
            return "$env:TEMP"

        windows_probe = types.SimpleNamespace(
            run_windows_ssh_powershell=run_powershell,
            windows_contract_expand_expression=expand,
        )
        bindings = self._bindings(windows_probe)
        bindings["run_ssh_subprocess"] = object()
        bindings["ps_literal"] = object()

        self.mod.install_windows_probe_helpers(
            bindings,
            ("run_windows_ssh_powershell", "windows_contract_expand_expression"),
        )

        self.assertEqual(bindings["run_windows_ssh_powershell"]("win", "Get-Date", timeout=42), "completed")
        self.assertEqual(bindings["windows_contract_expand_expression"]("%TEMP%"), "$env:TEMP")
        self.assertEqual(captured["run"][0:2], ("win", "Get-Date"))
        self.assertEqual(captured["run"][2]["timeout"], 42)
        self.assertEqual(captured["expand"][0], "%TEMP%")


if __name__ == "__main__":
    unittest.main()
