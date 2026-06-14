#!/usr/bin/env python3
"""Tests for local_ci facade cleanup binding wiring."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import pathlib
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("cleanup_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.cleanup = mock.Mock()
        self.bindings = {
            "_cleanup": self.cleanup,
            "ps_literal": mock.Mock(name="ps_literal"),
            "run_logged_command": mock.Mock(name="run_logged_command"),
            "windows_ssh_powershell_command": mock.Mock(name="windows_ssh_powershell_command"),
            "trim_line": mock.Mock(name="trim_line"),
        }

    def test_cleanup_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.CLEANUP_PLAN_EXPORTS,
            *self.mod.CLEANUP_STALE_WINDOWS_EXPORTS,
        )

        self.assertEqual(self.mod.CLEANUP_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_cleanup_helpers_wires_named_exports(self) -> None:
        self.cleanup.result_file_job_id.return_value = "job1"
        self.cleanup.cleanup_stale_windows_validator.return_value = {"killed": True}

        self.mod.install_cleanup_helpers(
            self.bindings,
            ("result_file_job_id", "cleanup_stale_windows_validator"),
        )

        self.assertEqual(self.bindings["result_file_job_id"](pathlib.Path("/state/results/job1.json")), "job1")
        self.assertEqual(
            self.bindings["cleanup_stale_windows_validator"]("win", 123, "2026-05-01T00:00:00Z"),
            {"killed": True},
        )
        self.cleanup.cleanup_stale_windows_validator.assert_called_once_with(
            "win",
            123,
            "2026-05-01T00:00:00Z",
            ps_literal_fn=self.bindings["ps_literal"],
            run_logged_command_fn=self.bindings["run_logged_command"],
            windows_ssh_powershell_command_fn=self.bindings["windows_ssh_powershell_command"],
            trim_line_fn=self.bindings["trim_line"],
        )


if __name__ == "__main__":
    unittest.main()
