#!/usr/bin/env python3
"""Tests for logs job-resolution command dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("logs_resolution_command_dependency_bindings.py")


class LogsResolutionCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_logs_resolution_command_dependencies_bind_facade_dependencies(self) -> None:
        bindings = {
            "_queue_orchestrator": types.SimpleNamespace(select_job_for_logs=object()),
            "load_queue": object(),
            "current_runner_info": object(),
        }

        deps = self.mod.logs_resolution_command_dependencies(bindings)

        self.assertIs(deps["load_queue_fn"], bindings["load_queue"])
        self.assertIs(deps["current_runner_info_fn"], bindings["current_runner_info"])
        self.assertIs(deps["select_job_for_logs_fn"], bindings["_queue_orchestrator"].select_job_for_logs)


if __name__ == "__main__":
    unittest.main()
