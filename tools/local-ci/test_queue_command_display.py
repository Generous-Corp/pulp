#!/usr/bin/env python3
"""Tests for queue command display helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("queue_command_display.py", module_name="pulp_queue_command_display")


class QueueCommandDisplayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_summarize_job_formats_core_job_fields(self) -> None:
        job = {
            "id": "abc123",
            "branch": "feature/q",
            "sha": "1234567890abcdef",
            "priority": "high",
            "targets": ["mac", "windows"],
            "validation": "smoke",
        }

        self.assertEqual(
            self.mod.summarize_job(job),
            "[abc123] feature/q @ 1234567890ab priority=high targets=mac,windows validation=smoke",
        )

    def test_queue_command_result_lines_cover_success_and_errors(self) -> None:
        self.assertEqual(self.mod.bump_queue_command_result_line({"status": "missing"}, "abc"), (1, "No active job matches 'abc'."))
        self.assertEqual(
            self.mod.cancel_queue_command_result_line({"status": "not_pending", "job_status": "running"}, "abc"),
            (1, "Job is already running; only pending jobs can be canceled safely."),
        )
        self.assertEqual(
            self.mod.bump_queue_command_result_line({"status": "updated", "summary": "job summary"}, "abc"),
            (0, "Updated priority: job summary"),
        )

    def test_drain_runner_active_line_includes_active_job_when_available(self) -> None:
        self.assertEqual(
            self.mod.drain_runner_active_line({"active_job_id": "job1", "active_branch": "feature/q"}),
            "Another local CI runner is active [job1] feature/q.",
        )
        self.assertEqual(self.mod.drain_runner_active_line({}), "Another local CI runner is active.")


if __name__ == "__main__":
    unittest.main()
