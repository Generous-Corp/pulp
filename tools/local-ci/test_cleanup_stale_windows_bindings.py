#!/usr/bin/env python3
"""Tests for stale Windows cleanup facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import pathlib
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("cleanup_stale_windows_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupStaleWindowsBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.cleanup = mock.Mock()
        self.bindings = {
            "_cleanup": self.cleanup,
            "stale_running_jobs_unlocked": mock.Mock(name="stale_running_jobs_unlocked"),
            "now_iso": mock.Mock(name="now_iso"),
            "ps_literal": mock.Mock(name="ps_literal"),
            "run_logged_command": mock.Mock(name="run_logged_command"),
            "windows_ssh_powershell_command": mock.Mock(name="windows_ssh_powershell_command"),
            "trim_line": mock.Mock(name="trim_line"),
        }

    def test_cleanup_stale_windows_exports_match_facade_helpers(self) -> None:
        expected = (
            "collect_stale_windows_cleanup_candidates_unlocked",
            "cleanup_stale_windows_validator",
        )

        self.assertEqual(self.mod.CLEANUP_STALE_WINDOWS_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_stale_windows_candidate_wiring_preserves_facade_seams(self) -> None:
        self.cleanup.collect_stale_windows_cleanup_candidates_unlocked.return_value = [{"job_id": "job1"}]
        queue = [{"id": "job1"}]

        result = self.mod.collect_stale_windows_cleanup_candidates_unlocked(self.bindings, queue)

        self.assertEqual(result, [{"job_id": "job1"}])
        self.cleanup.collect_stale_windows_cleanup_candidates_unlocked.assert_called_once_with(
            queue,
            stale_running_jobs_fn=self.bindings["stale_running_jobs_unlocked"],
            now_fn=self.bindings["now_iso"],
        )

    def test_cleanup_stale_windows_validator_wires_remote_helpers(self) -> None:
        self.cleanup.cleanup_stale_windows_validator.return_value = {"killed": True}

        result = self.mod.cleanup_stale_windows_validator(
            self.bindings,
            "win",
            123,
            "2026-05-01T00:00:00Z",
        )

        self.assertEqual(result, {"killed": True})
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
