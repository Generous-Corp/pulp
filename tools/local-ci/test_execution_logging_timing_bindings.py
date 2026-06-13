#!/usr/bin/env python3
"""Tests for validation logging timing dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("execution_logging_timing_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionLoggingTimingBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_timing_exports_match_helpers(self):
        expected = (
            "heartbeat_interval_secs",
            "stuck_idle_secs",
        )

        self.assertEqual(self.mod.EXECUTION_LOGGING_TIMING_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_timing_helpers_delegate_to_execution_constants(self):
        bindings = {
            "_execution": types.SimpleNamespace(
                HEARTBEAT_INTERVAL_SECS=15.0,
                STUCK_IDLE_SECS=90.0,
            ),
        }

        self.assertEqual(self.mod.heartbeat_interval_secs(bindings), 15.0)
        self.assertEqual(self.mod.stuck_idle_secs(bindings), 90.0)

    def test_timing_installer_wires_selected_exports(self):
        bindings = {"_execution": types.SimpleNamespace(HEARTBEAT_INTERVAL_SECS=15.0)}

        self.mod.install_execution_logging_timing_helpers(bindings, ("heartbeat_interval_secs",))

        self.assertEqual(bindings["heartbeat_interval_secs"](), 15.0)
        self.assertNotIn("stuck_idle_secs", bindings)


if __name__ == "__main__":
    unittest.main()
