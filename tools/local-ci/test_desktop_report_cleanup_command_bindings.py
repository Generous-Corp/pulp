#!/usr/bin/env python3
"""Tests for desktop cleanup report command bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_report_cleanup_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportCleanupCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_REPORT_CLEANUP_COMMAND_EXPORTS, ("cmd_desktop_cleanup",))

    def test_cleanup_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_cleanup=runner),
            "_desktop_cli": types.SimpleNamespace(
                desktop_cleanup_empty_line=object(),
                desktop_cleanup_lines=object(),
            ),
            "load_config": object(),
            "prune_desktop_run_manifests": object(),
            "write_desktop_run_rollups": object(),
        }

        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_cleanup(bindings, args_obj), 9)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["prune_desktop_run_manifests_fn"], bindings["prune_desktop_run_manifests"])
        self.assertIs(captured["kwargs"]["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])
        self.assertIs(captured["kwargs"]["desktop_cleanup_empty_line_fn"], bindings["_desktop_cli"].desktop_cleanup_empty_line)
        self.assertIs(captured["kwargs"]["desktop_cleanup_lines_fn"], bindings["_desktop_cli"].desktop_cleanup_lines)


if __name__ == "__main__":
    unittest.main()
