#!/usr/bin/env python3
"""Tests for validation execution facade composition."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("execution_bindings.py")


class ExecutionBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_execution_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.EXECUTION_COMMAND_EXPORTS,
            *self.mod.EXECUTION_RESULT_EXPORTS,
            *self.mod.EXECUTION_LOGGING_EXPORTS,
            *self.mod.EXECUTION_RUNNER_INSTALL_EXPORTS,
            *self.mod.EXECUTION_JOB_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        self.assertEqual(
            self.mod.EXECUTION_RUNNER_INSTALL_EXPORTS,
            (
                "run_local_validation",
                "run_posix_ssh_validation",
                "run_windows_ssh_validation",
            ),
        )

    def test_install_execution_helpers_routes_focused_groups(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_execution_command_helpers") as command,
            mock.patch.object(self.mod, "install_execution_result_helpers") as result,
            mock.patch.object(self.mod, "install_execution_logging_helpers") as logging,
            mock.patch.object(self.mod, "install_execution_runner_helpers") as runner,
            mock.patch.object(self.mod, "install_execution_job_helpers") as job,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_execution_helpers(
                bindings,
                (
                    "local_validation_command",
                    "unreachable_target_result",
                    "parse_progress_marker",
                    "run_local_validation",
                    "config_for_job_execution",
                    "custom_export",
                ),
            )

        command.assert_called_once_with(bindings, ("local_validation_command",))
        result.assert_called_once_with(bindings, ("unreachable_target_result",))
        logging.assert_called_once_with(bindings, ("parse_progress_marker",))
        runner.assert_called_once_with(bindings, ("run_local_validation",))
        job.assert_called_once_with(bindings, ("config_for_job_execution",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_export",))


if __name__ == "__main__":
    unittest.main()
