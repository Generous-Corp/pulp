#!/usr/bin/env python3
"""Tests for desktop report command compatibility bindings."""

from __future__ import annotations

from pathlib import Path
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_report_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_REPORT_RECENT_COMMAND_EXPORTS,
            *self.mod.DESKTOP_REPORT_PROOF_COMMAND_EXPORTS,
            *self.mod.DESKTOP_REPORT_PUBLISH_COMMAND_EXPORTS,
            *self.mod.DESKTOP_REPORT_CLEANUP_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_REPORT_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_report_command_helpers_routes_focused_groups(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_report_recent_command_helpers") as recent,
            mock.patch.object(self.mod, "install_desktop_report_proof_command_helpers") as proof,
            mock.patch.object(self.mod, "install_desktop_report_publish_command_helpers") as publish,
            mock.patch.object(self.mod, "install_desktop_report_cleanup_command_helpers") as cleanup,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_report_command_helpers(
                bindings,
                (
                    "cmd_desktop_recent",
                    "cmd_desktop_proof",
                    "cmd_desktop_publish",
                    "cmd_desktop_cleanup",
                    "custom_desktop_report",
                ),
            )

        recent.assert_called_once_with(bindings, ("cmd_desktop_recent",))
        proof.assert_called_once_with(bindings, ("cmd_desktop_proof",))
        publish.assert_called_once_with(bindings, ("cmd_desktop_publish",))
        cleanup.assert_called_once_with(bindings, ("cmd_desktop_cleanup",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_desktop_report",))

if __name__ == "__main__":
    unittest.main()
