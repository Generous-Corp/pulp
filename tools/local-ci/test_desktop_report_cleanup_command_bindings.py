#!/usr/bin/env python3
"""Tests for desktop cleanup report command bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_report_cleanup_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportCleanupCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_REPORT_CLEANUP_COMMAND_EXPORTS, ("cmd_desktop_cleanup",))

    def test_cleanup_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_cleanup=runner),
        }
        deps = {
            "load_config_fn": object(),
            "prune_desktop_run_manifests_fn": object(),
            "write_desktop_run_rollups_fn": object(),
            "desktop_cleanup_empty_line_fn": object(),
            "desktop_cleanup_lines_fn": object(),
        }

        args_obj = object()
        with mock.patch.object(self.mod, "desktop_report_cleanup_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_desktop_cleanup(bindings, args_obj), 9)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)


if __name__ == "__main__":
    unittest.main()
