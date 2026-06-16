#!/usr/bin/env python3
"""Tests for local-CI enqueue command dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("local_ci_enqueue_command_bindings.py")


class LocalCiEnqueueCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_enqueue_export_matches_wrapper(self):
        self.assertEqual(self.mod.LOCAL_CI_ENQUEUE_COMMAND_EXPORTS, ("cmd_enqueue",))

    def test_cmd_enqueue_delegates_with_assembled_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(cmd_enqueue=runner)}
        deps = {
            "resolve_submission_options_fn": object(),
            "print_submission_metadata_fn": object(),
            "enqueue_job_fn": object(),
            "enqueue_command_result_line_fn": object(),
        }

        args_obj = object()
        with mock.patch.object(self.mod, "local_ci_enqueue_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_enqueue(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)


if __name__ == "__main__":
    unittest.main()
