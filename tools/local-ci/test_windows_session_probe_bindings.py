#!/usr/bin/env python3
"""Tests for Windows session-agent and CMake probe dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("windows_session_probe_bindings.py")


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

    def test_install_session_probe_helpers_routes_groups_and_unknown_fallback(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_windows_session_agent_helpers") as agent,
            mock.patch.object(self.mod, "install_windows_session_cmake_probe_helpers") as cmake_probe,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_windows_session_probe_helpers(
                bindings,
                ("start_windows_session_agent_task", "probe_windows_ssh_cmake_settings", "unknown_helper"),
            )

        agent.assert_called_once_with(bindings, ("start_windows_session_agent_task",))
        cmake_probe.assert_called_once_with(bindings, ("probe_windows_ssh_cmake_settings",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))

if __name__ == "__main__":
    unittest.main()
