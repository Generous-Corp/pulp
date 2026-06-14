#!/usr/bin/env python3
"""Tests for Windows session-agent and CMake probe dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_session_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsSessionProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, windows_probe):
        return {
            "_windows_probe": windows_probe,
            "subprocess": types.SimpleNamespace(run=object()),
        }

    def test_session_probe_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.WINDOWS_SESSION_AGENT_EXPORTS,
            *self.mod.WINDOWS_SESSION_CMAKE_PROBE_EXPORTS,
        )

        self.assertEqual(self.mod.WINDOWS_SESSION_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
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

    def test_probe_windows_ssh_cmake_settings_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return ("ARM64", "C:/VS")

        bindings = self._bindings(types.SimpleNamespace(probe_windows_ssh_cmake_settings=runner))
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

    def test_install_session_probe_helpers_routes_named_exports_by_group(self) -> None:
        captured = {}

        def start_runner(*args, **kwargs):
            captured["start"] = (args, kwargs)
            return {"started": True}

        def cmake_runner(*args, **kwargs):
            captured["cmake"] = (args, kwargs)
            return ("ARM64", "C:/VS")

        bindings = self._bindings(
            types.SimpleNamespace(
                start_windows_session_agent_task=start_runner,
                probe_windows_ssh_cmake_settings=cmake_runner,
            )
        )
        for name in [
            "run_windows_ssh_powershell",
            "parse_windows_ssh_json",
            "ps_literal",
            "windows_ssh_powershell_command",
        ]:
            bindings[name] = object()

        self.mod.install_windows_session_probe_helpers(
            bindings,
            ("start_windows_session_agent_task", "probe_windows_ssh_cmake_settings"),
        )

        self.assertEqual(bindings["start_windows_session_agent_task"]("win", {"task_name": "Pulp"}), {"started": True})
        self.assertEqual(
            bindings["probe_windows_ssh_cmake_settings"]("win", "Visual Studio 17 2022", "", ""),
            ("ARM64", "C:/VS"),
        )
        self.assertEqual(captured["start"][0], ("win", {"task_name": "Pulp"}))
        self.assertEqual(captured["cmake"][0], ("win", "Visual Studio 17 2022", "", ""))



if __name__ == "__main__":
    unittest.main()
