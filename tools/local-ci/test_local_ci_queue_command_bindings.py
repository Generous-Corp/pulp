#!/usr/bin/env python3
"""Tests for queue-oriented local-CI command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_queue_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiQueueCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_enqueue_drain_and_run_bind_facade_dependencies(self):
        cases = [
            (
                "cmd_enqueue",
                self.mod.cmd_enqueue,
                [
                    "resolve_submission_options",
                    "print_submission_metadata",
                    "enqueue_job",
                    "enqueue_command_result_line",
                ],
            ),
            (
                "cmd_drain",
                self.mod.cmd_drain,
                ["load_config", "drain_pending_jobs", "current_runner_info", "drain_runner_active_line", "notify"],
            ),
            (
                "cmd_run",
                self.mod.cmd_run,
                [
                    "resolve_submission_options",
                    "print_submission_metadata",
                    "gh_workflow_dispatch",
                    "enqueue_job",
                    "enqueue_command_result_line",
                    "wait_for_job",
                    "load_job",
                    "print_result",
                    "notify",
                ],
            ),
        ]

        for runner_name, wrapper, dependency_names in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **kwargs):
                    captured["args"] = args
                    captured["kwargs"] = kwargs
                    return 7

                bindings = {"_local_ci_commands_cli": types.SimpleNamespace(**{runner_name: runner})}
                for name in dependency_names:
                    bindings[name] = object()

                args_obj = object()
                self.assertEqual(wrapper(bindings, args_obj), 7)
                self.assertEqual(captured["args"], (args_obj,))
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
