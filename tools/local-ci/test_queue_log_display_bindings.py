#!/usr/bin/env python3
"""Tests for queue log display facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_log_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueLogDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_log_display_exports_match_facade_helpers(self):
        expected = (
            "missing_job_logs_line",
            "missing_log_files_line",
            "job_logs_header_line",
            "log_section_header_line",
            "empty_log_line",
        )

        self.assertEqual(self.mod.QUEUE_LOG_DISPLAY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

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

    def test_install_queue_log_display_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_log_display_helpers(bindings, ("empty_log_line", "custom_log_display"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("empty_log_line",)),
                mock.call(bindings, self.mod.__dict__, ("custom_log_display",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
