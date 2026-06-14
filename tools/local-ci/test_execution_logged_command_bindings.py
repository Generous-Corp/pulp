#!/usr/bin/env python3
"""Tests for validation logged command dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("execution_logged_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionLoggedCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_logged_command_exports_match_helpers(self):
        expected = ("run_logged_command",)

        self.assertEqual(self.mod.EXECUTION_LOGGED_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_run_logged_command_delegates_and_binds_default_timing(self):
        captured = {}

        def run_logged_command(cmd, **kwargs):
            captured["logged"] = (cmd, kwargs)
            return {"exit_code": 0}

        execution = types.SimpleNamespace(
            HEARTBEAT_INTERVAL_SECS=15.0,
            STUCK_IDLE_SECS=90.0,
            run_logged_command=run_logged_command,
        )
        bindings = {"_execution": execution}

        self.assertEqual(self.mod.run_logged_command(bindings, ["cmd"]), {"exit_code": 0})
        self.assertEqual(captured["logged"][1]["heartbeat_interval_secs"], 15.0)
        self.assertEqual(captured["logged"][1]["stuck_idle_secs"], 90.0)
        self.assertEqual(
            self.mod.run_logged_command(bindings, ["cmd"], heartbeat_interval_secs=1.5, stuck_idle_secs=2.5),
            {"exit_code": 0},
        )
        self.assertEqual(captured["logged"][1]["heartbeat_interval_secs"], 1.5)
        self.assertEqual(captured["logged"][1]["stuck_idle_secs"], 2.5)

if __name__ == "__main__":
    unittest.main()
