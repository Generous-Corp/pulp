#!/usr/bin/env python3
"""Tests for local-CI run command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("local_ci_run_command_dependency_bindings.py")


class LocalCiRunCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_local_ci_run_command_dependencies_preserve_facade_seams(self) -> None:
        bindings = {}
        expected = {
            "resolve_submission_options": "resolve_submission_options_fn",
            "print_submission_metadata": "print_submission_metadata_fn",
            "gh_workflow_dispatch": "gh_workflow_dispatch_fn",
            "enqueue_job": "enqueue_job_fn",
            "enqueue_command_result_line": "enqueue_command_result_line_fn",
            "wait_for_job": "wait_for_job_fn",
            "load_job": "load_job_fn",
            "print_result": "print_result_fn",
            "notify": "notify_fn",
        }
        for name in expected:
            bindings[name] = object()

        deps = self.mod.local_ci_run_command_dependencies(bindings)

        self.assertEqual(set(deps), set(expected.values()))
        for binding_name, dependency_name in expected.items():
            self.assertIs(deps[dependency_name], bindings[binding_name])


if __name__ == "__main__":
    unittest.main()
