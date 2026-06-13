#!/usr/bin/env python3
"""Tests for validation logging/progress dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("execution_logging_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionLoggingBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_logging_helpers_delegate_and_bind_default_timing(self):
        captured = {}

        def run_logged_command(cmd, **kwargs):
            captured["logged"] = (cmd, kwargs)
            return {"exit_code": 0}

        execution = types.SimpleNamespace(
            HEARTBEAT_INTERVAL_SECS=15.0,
            STUCK_IDLE_SECS=90.0,
            parse_progress_marker=lambda line: {"line": line},
            run_logged_command=run_logged_command,
        )
        bindings = {"_execution": execution}

        self.assertEqual(self.mod.heartbeat_interval_secs(bindings), 15.0)
        self.assertEqual(self.mod.stuck_idle_secs(bindings), 90.0)
        self.assertEqual(self.mod.parse_progress_marker(bindings, "line"), {"line": "line"})
        self.assertEqual(self.mod.run_logged_command(bindings, ["cmd"]), {"exit_code": 0})
        self.assertEqual(captured["logged"][1]["heartbeat_interval_secs"], 15.0)
        self.assertEqual(captured["logged"][1]["stuck_idle_secs"], 90.0)
        self.assertEqual(
            self.mod.run_logged_command(bindings, ["cmd"], heartbeat_interval_secs=1.5, stuck_idle_secs=2.5),
            {"exit_code": 0},
        )
        self.assertEqual(captured["logged"][1]["heartbeat_interval_secs"], 1.5)
        self.assertEqual(captured["logged"][1]["stuck_idle_secs"], 2.5)

    def test_logging_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.EXECUTION_LOGGING_TIMING_EXPORTS,
            *self.mod.EXECUTION_PROGRESS_MARKER_EXPORTS,
            *self.mod.EXECUTION_LOGGED_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_LOGGING_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_logging_installer_wires_selected_exports(self):
        execution = types.SimpleNamespace(
            HEARTBEAT_INTERVAL_SECS=15.0,
            STUCK_IDLE_SECS=90.0,
            parse_progress_marker=lambda line: {"line": line},
            run_logged_command=lambda cmd, **kwargs: {"cmd": cmd, **kwargs},
        )
        bindings = {"_execution": execution}

        self.mod.install_execution_logging_helpers(
            bindings,
            ("heartbeat_interval_secs", "parse_progress_marker", "run_logged_command"),
        )

        self.assertEqual(bindings["heartbeat_interval_secs"](), 15.0)
        self.assertEqual(bindings["parse_progress_marker"]("line"), {"line": "line"})
        self.assertEqual(bindings["run_logged_command"](["cmd"])["cmd"], ["cmd"])
        self.assertNotIn("stuck_idle_secs", bindings)

    def test_install_execution_logging_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_execution_logging_helper = lambda _bindings: "future"

        self.mod.install_execution_logging_helpers(bindings, ("future_execution_logging_helper",))

        self.assertEqual(bindings["future_execution_logging_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
