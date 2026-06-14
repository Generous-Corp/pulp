#!/usr/bin/env python3
"""Tests for local-CI run command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_run_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiRunCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_run_export_matches_wrapper(self):
        self.assertEqual(self.mod.LOCAL_CI_RUN_COMMAND_EXPORTS, ("cmd_run",))

    def test_cmd_run_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(cmd_run=runner)}
        for name in [
            "resolve_submission_options",
            "print_submission_metadata",
            "gh_workflow_dispatch",
            "enqueue_job",
            "enqueue_command_result_line",
            "wait_for_job",
            "load_job",
            "print_result",
            "notify",
        ]:
            bindings[name] = object()

        args_obj = object()
        self.assertEqual(self.mod.cmd_run(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
        for name in [
            "resolve_submission_options",
            "print_submission_metadata",
            "gh_workflow_dispatch",
            "enqueue_job",
            "enqueue_command_result_line",
            "wait_for_job",
            "load_job",
            "print_result",
            "notify",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
