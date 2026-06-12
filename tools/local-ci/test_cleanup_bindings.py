#!/usr/bin/env python3
"""Tests for local_ci facade cleanup binding wiring."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("cleanup_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("cleanup_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class CleanupBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.cleanup = mock.Mock()
        self.bindings = {
            "_cleanup": self.cleanup,
            "bundles_dir": mock.Mock(name="bundles_dir"),
            "logs_dir": mock.Mock(name="logs_dir"),
            "results_dir": mock.Mock(name="results_dir"),
            "prepared_dir": mock.Mock(name="prepared_dir"),
            "path_size_bytes": mock.Mock(name="path_size_bytes"),
            "format_size_bytes": mock.Mock(name="format_size_bytes"),
            "describe_path_for_cleanup": mock.Mock(name="describe_path_for_cleanup"),
            "stale_running_jobs_unlocked": mock.Mock(name="stale_running_jobs_unlocked"),
            "now_iso": mock.Mock(name="now_iso"),
            "ps_literal": mock.Mock(name="ps_literal"),
            "run_logged_command": mock.Mock(name="run_logged_command"),
            "windows_ssh_powershell_command": mock.Mock(name="windows_ssh_powershell_command"),
            "trim_line": mock.Mock(name="trim_line"),
        }

    def test_collect_cleanup_plan_wires_facade_dependencies(self) -> None:
        self.cleanup.collect_local_ci_cleanup_plan.return_value = {"categories": {}}
        queue = [{"id": "job1"}]

        result = self.mod.collect_local_ci_cleanup_plan(
            self.bindings,
            queue,
            keep_results=1,
            keep_logs=2,
            keep_bundles=3,
            include_prepared=True,
        )

        self.assertEqual(result, {"categories": {}})
        self.cleanup.collect_local_ci_cleanup_plan.assert_called_once_with(
            queue,
            keep_results=1,
            keep_logs=2,
            keep_bundles=3,
            include_prepared=True,
            bundles_dir_fn=self.bindings["bundles_dir"],
            logs_dir_fn=self.bindings["logs_dir"],
            results_dir_fn=self.bindings["results_dir"],
            prepared_dir_fn=self.bindings["prepared_dir"],
            path_size_bytes_fn=self.bindings["path_size_bytes"],
        )

    def test_cleanup_plan_lines_wires_formatters(self) -> None:
        self.cleanup.cleanup_plan_lines.return_value = ["line"]
        plan = {"categories": {}}

        result = self.mod.cleanup_plan_lines(self.bindings, plan, dry_run=False)

        self.assertEqual(result, ["line"])
        self.cleanup.cleanup_plan_lines.assert_called_once_with(
            plan,
            dry_run=False,
            format_size_fn=self.bindings["format_size_bytes"],
            describe_path_fn=self.bindings["describe_path_for_cleanup"],
        )

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
