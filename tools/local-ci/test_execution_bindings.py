#!/usr/bin/env python3
"""Tests for validation execution facade composition."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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
        calls = []

        def command_install(bindings, names):
            calls.append(("command", names))

        def result_install(bindings, names):
            calls.append(("result", names))

        def logging_install(bindings, names):
            calls.append(("logging", names))

        def runner_install(bindings, names):
            calls.append(("runner", names))

        def job_install(bindings, names):
            calls.append(("job", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_execution_command_helpers = command_install
        self.mod.install_execution_result_helpers = result_install
        self.mod.install_execution_logging_helpers = logging_install
        self.mod.install_execution_runner_helpers = runner_install
        self.mod.install_execution_job_helpers = job_install
        self.mod.install_local_helpers = local_install

        self.mod.install_execution_helpers(
            {},
            (
                "local_validation_command",
                "unreachable_target_result",
                "parse_progress_marker",
                "run_local_validation",
                "config_for_job_execution",
                "custom_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("command", ("local_validation_command",)),
                ("result", ("unreachable_target_result",)),
                ("logging", ("parse_progress_marker",)),
                ("runner", ("run_local_validation",)),
                ("job", ("config_for_job_execution",)),
                ("local", ("custom_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
