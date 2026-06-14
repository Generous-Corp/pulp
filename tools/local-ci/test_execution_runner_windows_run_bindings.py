#!/usr/bin/env python3
"""Tests for Windows SSH validation runner facade bindings."""

from __future__ import annotations

import types
import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_runner_windows_run_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionRunnerWindowsRunBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_windows_runner_exports_match_wrappers(self) -> None:
        expected = ("run_windows_ssh_validation",)

        self.assertEqual(self.mod.EXECUTION_RUNNER_WINDOWS_RUN_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def _bindings(self, runner):
        execution = types.SimpleNamespace(run_windows_ssh_validation=runner)
        bindings = {"_execution": execution, "ROOT": Path("/repo"), "print": object()}
        for name in [
            "short_sha",
            "prepare_target_log",
            "now_iso",
            "run_logged_command",
            "validation_result_from_run",
            "sync_job_bundle_to_ssh_host",
            "validation_error_result",
            "ensure_windows_remote_repo_checkout",
            "git_origin_clone_url",
            "probe_windows_ssh_cmake_settings",
            "windows_validation_script",
            "windows_ssh_powershell_command",
        ]:
            bindings[name] = object()
        return bindings

    def test_run_windows_ssh_validation_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"target": "windows"}

        bindings = self._bindings(runner)
        progress = object()
        config = {"targets": {}}

        result = self.mod.run_windows_ssh_validation(
            bindings,
            "windows",
            "win.example.com",
            r"C:\Pulp",
            {"id": "job"},
            "slow",
            "Ninja",
            "ARM64",
            r"C:\VS",
            config,
            progress,
        )

        self.assertEqual(result, {"target": "windows"})
        self.assertEqual(
            captured["args"],
            ("windows", "win.example.com", r"C:\Pulp", {"id": "job"}, "slow", "Ninja", "ARM64", r"C:\VS", config, progress),
        )
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["ensure_windows_remote_repo_checkout_fn"], bindings["ensure_windows_remote_repo_checkout"])
        self.assertIs(captured["kwargs"]["git_origin_clone_url_fn"], bindings["git_origin_clone_url"])
        self.assertIs(captured["kwargs"]["probe_windows_ssh_cmake_settings_fn"], bindings["probe_windows_ssh_cmake_settings"])
        self.assertIs(captured["kwargs"]["windows_validation_script_fn"], bindings["windows_validation_script"])
        self.assertIs(captured["kwargs"]["windows_ssh_powershell_command_fn"], bindings["windows_ssh_powershell_command"])

if __name__ == "__main__":
    unittest.main()
