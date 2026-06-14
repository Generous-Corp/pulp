#!/usr/bin/env python3
"""Tests for stale Windows cleanup dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("cleanup_stale_windows_dependency_bindings.py")


class CleanupStaleWindowsDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_stale_windows_candidate_dependencies_preserve_queue_seams(self) -> None:
        bindings = {
            "stale_running_jobs_unlocked": object(),
            "now_iso": object(),
        }

        deps = self.mod.stale_windows_candidate_dependencies(bindings)

        self.assertIs(deps["stale_running_jobs_fn"], bindings["stale_running_jobs_unlocked"])
        self.assertIs(deps["now_fn"], bindings["now_iso"])

    def test_cleanup_stale_windows_validator_dependencies_preserve_remote_seams(self) -> None:
        bindings = {
            "ps_literal": object(),
            "run_logged_command": object(),
            "windows_ssh_powershell_command": object(),
            "trim_line": object(),
        }

        deps = self.mod.cleanup_stale_windows_validator_dependencies(bindings)

        self.assertIs(deps["ps_literal_fn"], bindings["ps_literal"])
        self.assertIs(deps["run_logged_command_fn"], bindings["run_logged_command"])
        self.assertIs(deps["windows_ssh_powershell_command_fn"], bindings["windows_ssh_powershell_command"])
        self.assertIs(deps["trim_line_fn"], bindings["trim_line"])


if __name__ == "__main__":
    unittest.main()
