#!/usr/bin/env python3
"""Tests for logs command execution dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("logs_run_command_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LogsRunCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_logs_run_command_dependencies_bind_facade_dependencies(self) -> None:
        bindings = {}
        names = [
            "resolve_job_for_logs",
            "target_log_path",
            "job_logs_dir",
            "tail_lines",
            "missing_job_logs_line",
            "missing_log_files_line",
            "job_logs_header_line",
            "log_section_header_line",
            "empty_log_line",
        ]
        for name in names:
            bindings[name] = object()

        deps = self.mod.logs_run_command_dependencies(bindings)

        for name in names:
            self.assertIs(deps[f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
