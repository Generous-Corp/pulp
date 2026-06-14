#!/usr/bin/env python3
"""Tests for queue log display helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_log_display.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_queue_log_display")


class QueueLogDisplayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_log_display_lines(self) -> None:
        job = {"id": "job1", "branch": "feature/q", "sha": "abcdef123456"}

        self.assertEqual(self.mod.missing_job_logs_line(), "No matching job logs found.")
        self.assertEqual(self.mod.missing_log_files_line(job), "No logs found for job [job1] feature/q.")
        self.assertEqual(self.mod.job_logs_header_line(job), "Logs for [job1] feature/q @ abcdef123456")
        self.assertEqual(self.mod.log_section_header_line("mac"), "== mac ==")
        self.assertEqual(self.mod.empty_log_line(), "(empty)")


if __name__ == "__main__":
    unittest.main()
