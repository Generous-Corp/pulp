#!/usr/bin/env python3
"""Tests for desktop Windows session-agent probe dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_windows_session_agent_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopWindowsSessionAgentProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_session_agent_probe_exports_match_wrappers(self):
        expected = ("probe_windows_session_agent",)

        self.assertEqual(self.mod.DESKTOP_WINDOWS_SESSION_AGENT_PROBE_EXPORTS, expected)
        self.assertTrue(callable(self.mod.probe_windows_session_agent))

    def test_session_agent_probe_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        bindings = {
            "_windows_probe": types.SimpleNamespace(probe_windows_session_agent=runner),
            "run_windows_ssh_powershell": object(),
            "parse_windows_ssh_json": object(),
            "windows_contract_expand_expression": object(),
            "ps_literal": object(),
        }

        self.assertEqual(self.mod.probe_windows_session_agent(bindings, "win", {"task_name": "task"}), {"ok": True})
        self.assertEqual(captured["args"], ("win", {"task_name": "task"}))
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(captured["kwargs"]["parse_windows_ssh_json_fn"], bindings["parse_windows_ssh_json"])
        self.assertIs(
            captured["kwargs"]["windows_contract_expand_expression_fn"],
            bindings["windows_contract_expand_expression"],
        )
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])

    def test_session_agent_probe_installer_wires_named_export(self):
        bindings = {
            "_windows_probe": types.SimpleNamespace(probe_windows_session_agent=lambda *args, **kwargs: {"ok": True}),
            "run_windows_ssh_powershell": object(),
            "parse_windows_ssh_json": object(),
            "windows_contract_expand_expression": object(),
            "ps_literal": object(),
        }

        self.mod.install_desktop_windows_session_agent_probe_helpers(bindings)

        self.assertEqual(bindings["probe_windows_session_agent"]("win", {}), {"ok": True})

    def test_install_desktop_windows_session_agent_probe_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_windows_session_agent_probe_helper = lambda _bindings: "future"

        self.mod.install_desktop_windows_session_agent_probe_helpers(
            bindings,
            ("future_desktop_windows_session_agent_probe_helper",),
        )

        self.assertEqual(bindings["future_desktop_windows_session_agent_probe_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
