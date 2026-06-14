#!/usr/bin/env python3
"""Tests for desktop Windows session/tooling probe dependency bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_windows_tooling_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopWindowsToolingProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_tooling_probe_exports_match_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_WINDOWS_SESSION_AGENT_PROBE_EXPORTS,
            *self.mod.DESKTOP_WINDOWS_REMOTE_TOOLING_PROBE_EXPORTS,
        )
        self.assertEqual(self.mod.DESKTOP_WINDOWS_TOOLING_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_tooling_probe_installer_routes_selected_groups_and_unknown_fallback(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_windows_session_agent_probe_helpers") as session_agent,
            mock.patch.object(self.mod, "install_desktop_windows_remote_tooling_probe_helpers") as remote_tooling,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_windows_tooling_probe_helpers(
                bindings,
                ("probe_windows_session_agent", "ensure_windows_remote_tooling", "unknown_helper"),
            )

        session_agent.assert_called_once_with(bindings, ("probe_windows_session_agent",))
        remote_tooling.assert_called_once_with(bindings, ("ensure_windows_remote_tooling",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
