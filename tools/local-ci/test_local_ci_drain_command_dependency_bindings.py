#!/usr/bin/env python3
"""Tests for local-CI drain command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("local_ci_drain_command_dependency_bindings.py")


class LocalCiDrainCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_local_ci_drain_command_dependencies_preserve_facade_seams(self) -> None:
        bindings = {
            "load_config": object(),
            "drain_pending_jobs": object(),
            "current_runner_info": object(),
            "drain_runner_active_line": object(),
            "notify": object(),
        }

        deps = self.mod.local_ci_drain_command_dependencies(bindings)

        self.assertIs(deps["load_config_fn"], bindings["load_config"])
        self.assertIs(deps["drain_pending_jobs_fn"], bindings["drain_pending_jobs"])
        self.assertIs(deps["current_runner_info_fn"], bindings["current_runner_info"])
        self.assertIs(deps["drain_runner_active_line_fn"], bindings["drain_runner_active_line"])
        self.assertIs(deps["notify_fn"], bindings["notify"])


if __name__ == "__main__":
    unittest.main()
