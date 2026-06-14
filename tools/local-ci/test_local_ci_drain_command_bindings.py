#!/usr/bin/env python3
"""Tests for local-CI drain command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_drain_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiDrainCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_drain_export_matches_wrapper(self):
        self.assertEqual(self.mod.LOCAL_CI_DRAIN_COMMAND_EXPORTS, ("cmd_drain",))

    def test_cmd_drain_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(cmd_drain=runner)}
        for name in [
            "load_config",
            "drain_pending_jobs",
            "current_runner_info",
            "drain_runner_active_line",
            "notify",
        ]:
            bindings[name] = object()

        args_obj = object()
        self.assertEqual(self.mod.cmd_drain(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
        for name in [
            "load_config",
            "drain_pending_jobs",
            "current_runner_info",
            "drain_runner_active_line",
            "notify",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
