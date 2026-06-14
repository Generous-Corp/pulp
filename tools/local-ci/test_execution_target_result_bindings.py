#!/usr/bin/env python3
"""Tests for validation target result compatibility bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_target_result_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionTargetResultBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_target_result_exports_match_helpers(self):
        expected = (
            *self.mod.EXECUTION_TARGET_RUN_RESULT_EXPORTS,
            *self.mod.EXECUTION_TARGET_FAILURE_RESULT_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_TARGET_RESULT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_target_result_installer_routes_each_group(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_execution_target_run_result_helpers") as install_run,
            mock.patch.object(self.mod, "install_execution_target_failure_result_helpers") as install_failure,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_execution_target_result_helpers(
                bindings,
                ("validation_result_from_run", "target_exception_result", "custom_target_result"),
            )

        install_run.assert_called_once_with(bindings, ("validation_result_from_run",))
        install_failure.assert_called_once_with(bindings, ("target_exception_result",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_target_result",))

if __name__ == "__main__":
    unittest.main()
