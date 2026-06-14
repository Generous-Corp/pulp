#!/usr/bin/env python3
"""Tests for macOS window probe/capture facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("macos_window_probe_bindings.py")


class MacosWindowProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_exports_compose_focused_groups(self) -> None:
        expected = (
            *self.mod.MACOS_WINDOW_INFO_EXPORTS,
            *self.mod.MACOS_WINDOW_WAIT_EXPORTS,
            *self.mod.MACOS_WINDOW_CAPTURE_EXPORTS,
        )

        self.assertEqual(self.mod.MACOS_WINDOW_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_probe_installer_routes_selected_groups_and_unknown_exports(self) -> None:
        bindings = {}
        names = ("macos_accessibility_trusted", "wait_for_macos_window", "capture_macos_window", "custom")

        with (
            mock.patch.object(self.mod, "install_macos_window_info_helpers") as info,
            mock.patch.object(self.mod, "install_macos_window_wait_helpers") as wait,
            mock.patch.object(self.mod, "install_macos_window_capture_helpers") as capture,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_macos_window_probe_helpers(bindings, names)

        info.assert_called_once_with(bindings, ("macos_accessibility_trusted",))
        wait.assert_called_once_with(bindings, ("wait_for_macos_window",))
        capture.assert_called_once_with(bindings, ("capture_macos_window",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
