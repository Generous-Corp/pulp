#!/usr/bin/env python3
"""Tests for logs command execution facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("logs_run_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LogsRunCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_logs_run_helpers(self):
        self.assertEqual(self.mod.LOGS_RUN_COMMAND_EXPORTS, ("cmd_logs",))

    def test_cmd_logs_binds_facade_dependencies(self):
        captured = {}

        def cmd_runner(*args, **kwargs):
            captured["cmd_args"] = args
            captured["cmd_kwargs"] = kwargs
            return 2

        bindings = {"_logs_cli": types.SimpleNamespace(cmd_logs=cmd_runner)}
        for name in [
            "resolve_job_for_logs",
            "target_log_path",
            "job_logs_dir",
            "tail_lines",
            "missing_job_logs_line",
            "missing_log_files_line",
            "job_logs_header_line",
            "log_section_header_line",
            "empty_log_line",
        ]:
            bindings[name] = object()

        args_obj = object()
        self.assertEqual(self.mod.cmd_logs(bindings, args_obj), 2)
        self.assertEqual(captured["cmd_args"], (args_obj,))
        for name in [
            "resolve_job_for_logs",
            "target_log_path",
            "job_logs_dir",
            "tail_lines",
            "missing_job_logs_line",
            "missing_log_files_line",
            "job_logs_header_line",
            "log_section_header_line",
            "empty_log_line",
        ]:
            self.assertIs(captured["cmd_kwargs"][f"{name}_fn"], bindings[name])

    def test_install_logs_run_command_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_logs_run_command_helpers(bindings, ("cmd_logs", "custom_logs_run"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("cmd_logs",)),
                mock.call(bindings, self.mod.__dict__, ("custom_logs_run",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
