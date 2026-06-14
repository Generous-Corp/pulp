#!/usr/bin/env python3
"""Tests for POSIX SSH validation runner facade bindings."""

from __future__ import annotations

import types
import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_runner_ssh_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionRunnerSshBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_ssh_runner_exports_match_wrappers(self) -> None:
        expected = ("run_posix_ssh_validation",)

        self.assertEqual(self.mod.EXECUTION_RUNNER_SSH_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def _bindings(self, runner):
        execution = types.SimpleNamespace(run_posix_ssh_validation=runner)
        bindings = {"_execution": execution, "print": object()}
        for name in [
            "short_sha",
            "prepare_target_log",
            "now_iso",
            "sync_job_bundle_to_ssh_host",
            "posix_ssh_validation_command",
            "run_logged_command",
            "validation_result_from_run",
            "validation_error_result",
        ]:
            bindings[name] = object()
        return bindings

    def test_run_posix_ssh_validation_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"target": "ubuntu"}

        bindings = self._bindings(runner)
        progress = object()
        config = {"ssh": {}}

        result = self.mod.run_posix_ssh_validation(
            bindings,
            "ubuntu",
            "ubuntu.example.com",
            "/repo",
            {"id": "job"},
            "slow",
            config,
            progress,
        )

        self.assertEqual(result, {"target": "ubuntu"})
        self.assertEqual(captured["args"], ("ubuntu", "ubuntu.example.com", "/repo", {"id": "job"}, "slow", config, progress))
        self.assertIs(captured["kwargs"]["print_fn"], bindings["print"])
        self.assertIs(captured["kwargs"]["sync_job_bundle_to_ssh_host_fn"], bindings["sync_job_bundle_to_ssh_host"])
        self.assertIs(captured["kwargs"]["posix_ssh_validation_command_fn"], bindings["posix_ssh_validation_command"])
        self.assertIs(captured["kwargs"]["run_logged_command_fn"], bindings["run_logged_command"])
        self.assertIs(captured["kwargs"]["validation_error_result_fn"], bindings["validation_error_result"])

    def test_install_execution_runner_ssh_helpers_wires_named_exports(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"target": "ubuntu"}

        bindings = self._bindings(runner)
        self.mod.install_execution_runner_ssh_helpers(bindings)

        self.assertEqual(
            bindings["run_posix_ssh_validation"]("ubuntu", "host", "/repo", {"id": "job"}),
            {"target": "ubuntu"},
        )
        self.assertEqual(captured["args"], ("ubuntu", "host", "/repo", {"id": "job"}, "", None, None))
        self.assertIs(captured["kwargs"]["print_fn"], bindings["print"])


if __name__ == "__main__":
    unittest.main()
