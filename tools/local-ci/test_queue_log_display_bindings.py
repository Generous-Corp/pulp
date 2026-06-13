#!/usr/bin/env python3
"""Tests for queue log display facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_log_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueLogDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_log_display_bindings_delegate_to_orchestrator(self):
        orchestrator = types.SimpleNamespace(
            missing_job_logs_line=lambda: "missing logs",
            missing_log_files_line=lambda job: "missing files",
            job_logs_header_line=lambda job: "header",
            log_section_header_line=lambda target: "section",
            empty_log_line=lambda: "empty",
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.missing_job_logs_line(bindings), "missing logs")
        self.assertEqual(self.mod.missing_log_files_line(bindings, {"id": "job"}), "missing files")
        self.assertEqual(self.mod.job_logs_header_line(bindings, {"id": "job"}), "header")
        self.assertEqual(self.mod.log_section_header_line(bindings, "mac"), "section")
        self.assertEqual(self.mod.empty_log_line(bindings), "empty")


if __name__ == "__main__":
    unittest.main()
