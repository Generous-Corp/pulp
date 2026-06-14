#!/usr/bin/env python3
"""Tests for desktop publish report command bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_report_publish_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportPublishCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_publish_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_REPORT_PUBLISH_COMMAND_EXPORTS, ("cmd_desktop_publish",))

    def test_publish_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 8

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_publish=runner),
            "_desktop_cli": types.SimpleNamespace(desktop_publish_lines=object()),
            "load_config": object(),
            "desktop_run_manifests": object(),
            "stage_desktop_publish_report": object(),
        }

        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_publish(bindings, args_obj), 8)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(captured["kwargs"]["stage_desktop_publish_report_fn"], bindings["stage_desktop_publish_report"])
        self.assertIs(captured["kwargs"]["desktop_publish_lines_fn"], bindings["_desktop_cli"].desktop_publish_lines)


if __name__ == "__main__":
    unittest.main()
