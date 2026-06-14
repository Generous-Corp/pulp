#!/usr/bin/env python3
"""Tests for stale Windows cleanup facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("cleanup_stale_windows_bindings.py")


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

    def test_stale_windows_candidate_wiring_delegates_with_assembled_dependencies(self) -> None:
        self.cleanup.collect_stale_windows_cleanup_candidates_unlocked.return_value = [{"job_id": "job1"}]
        queue = [{"id": "job1"}]
        deps = {
            "stale_running_jobs_fn": object(),
            "now_fn": object(),
        }

        with mock.patch.object(self.mod, "stale_windows_candidate_dependencies", return_value=deps):
            result = self.mod.collect_stale_windows_cleanup_candidates_unlocked(self.bindings, queue)

        self.assertEqual(result, [{"job_id": "job1"}])
        self.cleanup.collect_stale_windows_cleanup_candidates_unlocked.assert_called_once_with(
            queue,
            **deps,
        )

    def test_cleanup_stale_windows_validator_delegates_with_assembled_dependencies(self) -> None:
        self.cleanup.cleanup_stale_windows_validator.return_value = {"killed": True}
        deps = {
            "ps_literal_fn": object(),
            "run_logged_command_fn": object(),
            "windows_ssh_powershell_command_fn": object(),
            "trim_line_fn": object(),
        }

        with mock.patch.object(self.mod, "cleanup_stale_windows_validator_dependencies", return_value=deps):
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
            **deps,
        )

    def test_install_cleanup_stale_windows_helpers_wires_named_exports(self) -> None:
        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cleanup_stale_windows_helpers(
                self.bindings,
                ("cleanup_stale_windows_validator", "custom_stale_cleanup"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(self.bindings, self.mod.__dict__, ("cleanup_stale_windows_validator",)),
                mock.call(self.bindings, self.mod.__dict__, ("custom_stale_cleanup",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
