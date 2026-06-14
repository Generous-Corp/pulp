#!/usr/bin/env python3
"""Tests for queue status display helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_status_display.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_queue_status_display", add_module_dir=True)


class QueueStatusDisplayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_status_active_targets_prefers_job_then_runner(self) -> None:
        self.assertEqual(
            self.mod.status_active_targets({"id": "job1", "active_targets": {"mac": {"status": "running"}}}),
            {"mac": {"status": "running"}},
        )
        self.assertEqual(
            self.mod.status_active_targets(
                {"id": "job1"},
                {"active_job_id": "job1", "active_targets": {"windows": {"status": "pending"}}},
            ),
            {"windows": {"status": "pending"}},
        )

    def test_target_detail_lines_include_details_last_line_and_cleanup(self) -> None:
        job = {"targets": ["mac", "windows"]}
        active_targets = {
            "mac": {
                "phase": "test",
                "quiet_for_secs": 4,
                "log_path": "/tmp/mac.log",
                "last_line": "ran tests",
                "cleanup_result": "deleted 1 file",
            }
        }

        self.assertEqual(
            self.mod.status_target_detail_lines(job, active_targets),
            ["mac: phase=test, idle=4s, log=mac.log", "  ran tests", "  cleanup: deleted 1 file"],
        )

    def test_recent_completed_lines_format_result_and_missing_result(self) -> None:
        job = {"id": "job1", "branch": "feature/q", "sha": "abcdef123456", "targets": ["mac"]}
        result = {"overall": "pass", "results": [{"target": "mac", "status": "pass"}], "provenance": {"kind": "local"}}

        self.assertIn("[job1] feature/q @ abcdef123456 PASS [mac=pass]", self.mod.recent_completed_status_line(job, result))
        self.assertEqual(
            self.mod.recent_completed_missing_result_line(job),
            "[job1] feature/q @ abcdef123456 priority=normal targets=mac (result file missing)",
        )


if __name__ == "__main__":
    unittest.main()
