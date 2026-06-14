#!/usr/bin/env python3
"""Tests for Windows probe facade composition."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("windows_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_windows_probe_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.WINDOWS_PROBE_CORE_EXPORTS,
            *self.mod.WINDOWS_REMOTE_FILE_EXPORTS,
            *self.mod.WINDOWS_SESSION_PROBE_EXPORTS,
        )

        self.assertEqual(self.mod.WINDOWS_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_windows_probe_helpers_routes_each_group(self) -> None:
        calls = []

        def core_install(bindings, names):
            calls.append(("core", names))

        def remote_file_install(bindings, names):
            calls.append(("remote_file", names))

        def session_install(bindings, names):
            calls.append(("session", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_windows_probe_core_helpers = core_install
        self.mod.install_windows_remote_file_helpers = remote_file_install
        self.mod.install_windows_session_probe_helpers = session_install
        self.mod.install_local_helpers = local_install

        self.mod.install_windows_probe_helpers(
            {},
            (
                "run_windows_ssh_powershell",
                "windows_ssh_write_text",
                "start_windows_session_agent_task",
                "custom_windows_probe_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("core", ("run_windows_ssh_powershell",)),
                ("remote_file", ("windows_ssh_write_text",)),
                ("session", ("start_windows_session_agent_task",)),
                ("local", ("custom_windows_probe_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
