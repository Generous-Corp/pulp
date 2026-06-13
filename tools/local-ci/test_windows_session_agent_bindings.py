#!/usr/bin/env python3
"""Tests for Windows session-agent dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("windows_session_agent_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsSessionAgentBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, windows_probe):
        return {
            "_windows_probe": windows_probe,
            "subprocess": types.SimpleNamespace(run=object()),
        }

    def test_session_agent_exports_match_wrappers(self) -> None:
        expected = (
            "bootstrap_windows_session_agent",
            "start_windows_session_agent_task",
        )

        self.assertEqual(self.mod.WINDOWS_SESSION_AGENT_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

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

                bindings = self._bindings(types.SimpleNamespace(**{runner_name: runner}))
                for name in dependency_names:
                    bindings[name] = object()

                self.assertEqual(wrapper(bindings, *args), {"started": True})
                self.assertEqual(captured["args"], args)
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_install_session_agent_helpers_wires_named_exports(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"started": True}

        bindings = self._bindings(types.SimpleNamespace(start_windows_session_agent_task=runner))
        for name in ["run_windows_ssh_powershell", "parse_windows_ssh_json", "ps_literal"]:
            bindings[name] = object()

        self.mod.install_windows_session_agent_helpers(bindings, ("start_windows_session_agent_task",))

        self.assertEqual(bindings["start_windows_session_agent_task"]("win", {"task_name": "Pulp"}), {"started": True})
        self.assertEqual(captured["args"], ("win", {"task_name": "Pulp"}))


if __name__ == "__main__":
    unittest.main()
