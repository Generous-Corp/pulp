#!/usr/bin/env python3
"""Tests for validation command facade dependency bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_facade_reexports_command_state_and_validation_command_helpers(self):
        expected_exports = (
            "remote_commit_error",
            "prepared_state_root",
            "should_reuse_prepared_state",
            "local_validation_command",
            "posix_ssh_validation_command",
            "windows_validation_script",
        )

        self.assertEqual(self.mod.EXECUTION_COMMAND_EXPORTS, expected_exports)
        for name in expected_exports:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_command_installer_wires_selected_exports(self):
        execution = type(
            "Execution",
            (),
            {
                "remote_commit_error": staticmethod(lambda target, host, job: f"{target}:{host}:{job['id']}"),
                "local_validation_command": staticmethod(lambda job, exclude_tests="": ([job["id"]], exclude_tests)),
            },
        )
        bindings = {"_execution": execution}

        self.mod.install_execution_command_helpers(bindings, ("remote_commit_error", "local_validation_command"))

        self.assertEqual(bindings["remote_commit_error"]("mac", "host", {"id": "job"}), "mac:host:job")
        self.assertEqual(bindings["local_validation_command"]({"id": "job"}, "slow"), (["job"], "slow"))
        self.assertNotIn("windows_validation_script", bindings)
        self.assertNotIn("prepared_state_root", bindings)


if __name__ == "__main__":
    unittest.main()
