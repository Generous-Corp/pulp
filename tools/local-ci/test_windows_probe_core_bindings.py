#!/usr/bin/env python3
"""Tests for Windows PowerShell probe core dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("windows_probe_core_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsProbeCoreBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_core_exports_are_declared(self) -> None:
        self.assertEqual(
            self.mod.WINDOWS_PROBE_CORE_EXPORTS,
            (
                "ps_literal",
                "windows_ssh_powershell_command",
                "run_windows_ssh_powershell",
                "parse_windows_ssh_json",
                "windows_contract_expand_expression",
                "windows_session_agent_template_path",
            ),
        )

    def _bindings(self, windows_probe):
        return {
            "_windows_probe": windows_probe,
            "SCRIPT_DIR": Path("/repo/tools/local-ci"),
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

    def test_run_windows_ssh_powershell_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return "completed"

        windows_probe = types.SimpleNamespace(run_windows_ssh_powershell=runner)
        bindings = self._bindings(windows_probe)
        deps = {"run_ssh_subprocess_fn": object()}

        with mock.patch.object(self.mod, "run_windows_ssh_powershell_dependencies", return_value=deps):
            self.assertEqual(self.mod.run_windows_ssh_powershell(bindings, "win", "Get-Date", timeout=42), "completed")
        self.assertEqual(captured["args"], ("win", "Get-Date"))
        self.assertEqual(captured["kwargs"]["timeout"], 42)
        self.assertIs(captured["kwargs"]["run_ssh_subprocess_fn"], deps["run_ssh_subprocess_fn"])

    def test_windows_contract_expand_expression_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return "$env:TEMP"

        windows_probe = types.SimpleNamespace(windows_contract_expand_expression=runner)
        bindings = self._bindings(windows_probe)
        deps = {"ps_literal_fn": object()}

        with mock.patch.object(self.mod, "windows_contract_expand_expression_dependencies", return_value=deps):
            self.assertEqual(self.mod.windows_contract_expand_expression(bindings, "%TEMP%"), "$env:TEMP")
        self.assertEqual(captured["args"], ("%TEMP%",))
        self.assertIs(captured["kwargs"]["ps_literal_fn"], deps["ps_literal_fn"])

    def test_windows_session_agent_template_path_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return Path("/repo/tools/local-ci/windows-session-agent.ps1")

        windows_probe = types.SimpleNamespace(windows_session_agent_template_path=runner)
        bindings = self._bindings(windows_probe)
        deps = {"script_dir": Path("/custom/tools/local-ci")}

        with mock.patch.object(self.mod, "windows_session_agent_template_path_dependencies", return_value=deps):
            self.assertEqual(
                self.mod.windows_session_agent_template_path(bindings),
                Path("/repo/tools/local-ci/windows-session-agent.ps1"),
            )
        self.assertEqual(captured["args"], (deps["script_dir"],))
        self.assertEqual(captured["kwargs"], {})

    def test_install_windows_probe_core_helpers_wires_named_exports(self) -> None:
        captured = {}

        def run_powershell(host, script, **kwargs):
            captured["run"] = (host, script, kwargs)
            return "completed"

        bindings = self._bindings(types.SimpleNamespace(run_windows_ssh_powershell=run_powershell))
        bindings["run_ssh_subprocess"] = object()

        self.mod.install_windows_probe_core_helpers(bindings, ("run_windows_ssh_powershell",))

        self.assertEqual(bindings["run_windows_ssh_powershell"]("win", "Get-Date", timeout=42), "completed")
        self.assertEqual(captured["run"][0:2], ("win", "Get-Date"))
        self.assertEqual(captured["run"][2]["timeout"], 42)
        self.assertNotIn("ps_literal", bindings)


if __name__ == "__main__":
    unittest.main()
