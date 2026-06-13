#!/usr/bin/env python3
"""Tests for Windows CMake settings probe dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("windows_session_cmake_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsSessionCmakeProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cmake_probe_exports_match_wrappers(self) -> None:
        expected = ("probe_windows_ssh_cmake_settings",)

        self.assertEqual(self.mod.WINDOWS_SESSION_CMAKE_PROBE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_probe_windows_ssh_cmake_settings_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return ("ARM64", "C:/VS")

        bindings = {
            "_windows_probe": types.SimpleNamespace(probe_windows_ssh_cmake_settings=runner),
            "subprocess": types.SimpleNamespace(run=object()),
            "windows_ssh_powershell_command": object(),
            "ps_literal": object(),
        }

        self.assertEqual(
            self.mod.probe_windows_ssh_cmake_settings(bindings, "win", "Visual Studio 17 2022", "", ""),
            ("ARM64", "C:/VS"),
        )
        self.assertEqual(captured["args"], ("win", "Visual Studio 17 2022", "", ""))
        self.assertIs(captured["kwargs"]["windows_ssh_powershell_command_fn"], bindings["windows_ssh_powershell_command"])
        self.assertIs(captured["kwargs"]["run_fn"], bindings["subprocess"].run)
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])

    def test_install_cmake_probe_helpers_wires_named_exports(self) -> None:
        bindings = {
            "_windows_probe": types.SimpleNamespace(
                probe_windows_ssh_cmake_settings=lambda *args, **kwargs: ("ARM64", "C:/VS")
            ),
            "subprocess": types.SimpleNamespace(run=object()),
            "windows_ssh_powershell_command": object(),
            "ps_literal": object(),
        }

        self.mod.install_windows_session_cmake_probe_helpers(bindings, ("probe_windows_ssh_cmake_settings",))

        self.assertEqual(
            bindings["probe_windows_ssh_cmake_settings"]("win", "Visual Studio 17 2022", "", ""),
            ("ARM64", "C:/VS"),
        )

    def test_install_cmake_probe_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_windows_cmake_probe_helper = lambda _bindings: "future"

        self.mod.install_windows_session_cmake_probe_helpers(bindings, ("future_windows_cmake_probe_helper",))

        self.assertEqual(bindings["future_windows_cmake_probe_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
