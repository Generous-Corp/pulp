#!/usr/bin/env python3
"""Tests for Windows session-agent and CMake probe dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import types
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_session_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsSessionProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_session_probe_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.WINDOWS_SESSION_AGENT_EXPORTS,
            *self.mod.WINDOWS_SESSION_CMAKE_PROBE_EXPORTS,
        )

        self.assertEqual(self.mod.WINDOWS_SESSION_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_session_probe_helpers_routes_named_exports_by_group(self) -> None:
        bindings = self._bindings(
            types.SimpleNamespace(
                start_windows_session_agent_task=lambda *args, **kwargs: {"started": True},
                probe_windows_ssh_cmake_settings=lambda *args, **kwargs: ("ARM64", "C:/VS"),
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

    def test_install_session_probe_helpers_preserves_unknown_fallback(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_windows_session_probe_helpers(bindings, ("unknown_helper",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))

    def _bindings(self, windows_probe):
        return {
            "_windows_probe": windows_probe,
            "subprocess": types.SimpleNamespace(run=object()),
        }

if __name__ == "__main__":
    unittest.main()
