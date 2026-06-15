#!/usr/bin/env python3
"""Tests for local-CI enqueue command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("local_ci_enqueue_command_dependency_bindings.py")


class LocalCiEnqueueCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_local_ci_enqueue_command_dependencies_preserve_facade_seams(self) -> None:
        bindings = {
            "resolve_submission_options": object(),
            "print_submission_metadata": object(),
            "enqueue_job": object(),
            "enqueue_command_result_line": object(),
        }

        deps = self.mod.local_ci_enqueue_command_dependencies(bindings)

        self.assertIs(deps["resolve_submission_options_fn"], bindings["resolve_submission_options"])
        self.assertIs(deps["print_submission_metadata_fn"], bindings["print_submission_metadata"])
        self.assertIs(deps["enqueue_job_fn"], bindings["enqueue_job"])
        self.assertIs(deps["enqueue_command_result_line_fn"], bindings["enqueue_command_result_line"])


if __name__ == "__main__":
    unittest.main()
