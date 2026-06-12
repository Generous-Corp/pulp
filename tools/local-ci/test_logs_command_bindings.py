#!/usr/bin/env python3
"""Tests for logs command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("logs_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LogsCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        bindings = {
            "_logs_cli": types.SimpleNamespace(**{runner_name: runner}),
            "_queue_orchestrator": types.SimpleNamespace(select_job_for_logs=object()),
        }
        for name in [
            "load_queue",
            "current_runner_info",
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
        return bindings

    def test_logs_bind_facade_dependencies(self):
        captured = {}

        def resolve_runner(*args, **kwargs):
            captured["resolve_args"] = args
            captured["resolve_kwargs"] = kwargs
            return {"id": "job"}

        bindings = self._bindings("resolve_job_for_logs", resolve_runner)
        self.assertEqual(self.mod.resolve_job_for_logs(bindings, "job"), {"id": "job"})
        self.assertEqual(captured["resolve_args"], ("job",))
        self.assertIs(captured["resolve_kwargs"]["load_queue_fn"], bindings["load_queue"])
        self.assertIs(captured["resolve_kwargs"]["current_runner_info_fn"], bindings["current_runner_info"])
        self.assertIs(captured["resolve_kwargs"]["select_job_for_logs_fn"], bindings["_queue_orchestrator"].select_job_for_logs)

        def cmd_runner(*args, **kwargs):
            captured["cmd_args"] = args
            captured["cmd_kwargs"] = kwargs
            return 2

        bindings = self._bindings("cmd_logs", cmd_runner)
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


if __name__ == "__main__":
    unittest.main()
