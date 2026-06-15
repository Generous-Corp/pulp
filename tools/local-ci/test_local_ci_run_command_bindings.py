#!/usr/bin/env python3
"""Tests for local-CI run command dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("local_ci_run_command_bindings.py")


class LocalCiRunCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_run_export_matches_wrapper(self):
        self.assertEqual(self.mod.LOCAL_CI_RUN_COMMAND_EXPORTS, ("cmd_run",))

    def test_cmd_run_delegates_with_assembled_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(cmd_run=runner)}
        deps = {
            "resolve_submission_options_fn": object(),
            "print_submission_metadata_fn": object(),
            "gh_workflow_dispatch_fn": object(),
            "enqueue_job_fn": object(),
            "enqueue_command_result_line_fn": object(),
            "wait_for_job_fn": object(),
            "load_job_fn": object(),
            "print_result_fn": object(),
            "notify_fn": object(),
        }

        args_obj = object()
        with mock.patch.object(self.mod, "local_ci_run_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_run(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)


if __name__ == "__main__":
    unittest.main()
