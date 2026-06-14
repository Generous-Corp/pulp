#!/usr/bin/env python3
"""Tests for desktop recent report command bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_report_recent_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportRecentCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_recent_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_REPORT_RECENT_COMMAND_EXPORTS, ("cmd_desktop_recent",))

    def test_recent_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 4

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_recent=runner),
            "_desktop_cli": types.SimpleNamespace(desktop_recent_lines=object()),
            "load_config": object(),
            "desktop_run_manifests": object(),
            "desktop_run_summary": object(),
            "short_sha": object(),
        }

        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_recent(bindings, args_obj), 4)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(captured["kwargs"]["desktop_run_summary_fn"], bindings["desktop_run_summary"])
        self.assertIs(captured["kwargs"]["desktop_recent_lines_fn"], bindings["_desktop_cli"].desktop_recent_lines)
        self.assertIs(captured["kwargs"]["short_sha_fn"], bindings["short_sha"])


if __name__ == "__main__":
    unittest.main()
