#!/usr/bin/env python3
"""Tests for desktop recent report command bindings."""

from __future__ import annotations

import types
import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_report_recent_command_bindings.py")


class DesktopReportRecentCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_recent_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_REPORT_RECENT_COMMAND_EXPORTS, ("cmd_desktop_recent",))

    def test_recent_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 4

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_recent=runner),
        }
        deps = {
            "load_config_fn": object(),
            "desktop_run_manifests_fn": object(),
            "desktop_run_summary_fn": object(),
            "desktop_recent_lines_fn": object(),
            "short_sha_fn": object(),
        }

        args_obj = object()
        with mock.patch.object(self.mod, "desktop_report_recent_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_desktop_recent(bindings, args_obj), 4)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)


if __name__ == "__main__":
    unittest.main()
