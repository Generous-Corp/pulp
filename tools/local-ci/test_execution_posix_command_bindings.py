#!/usr/bin/env python3
"""Tests for POSIX SSH validation command dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("execution_posix_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionPosixCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_posix_command_exports_match_facade_helpers(self):
        expected = ("posix_ssh_validation_command",)

        self.assertEqual(self.mod.EXECUTION_POSIX_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_posix_ssh_validation_command_delegates_to_execution_module(self):
        captured = {}

        def posix_ssh_validation_command(*args, **kwargs):
            captured["posix"] = (args, kwargs)
            return list(args), kwargs["exclude_tests"]

        execution = types.SimpleNamespace(posix_ssh_validation_command=posix_ssh_validation_command)
        bindings = {"_execution": execution}

        self.assertEqual(
            self.mod.posix_ssh_validation_command(
                bindings,
                "ubuntu",
                "host",
                "/repo",
                {"id": "job"},
                bundle_name="bundle",
                bundle_ref="ref",
                exclude_tests="slow",
            ),
            (["ubuntu", "host", "/repo", {"id": "job"}], "slow"),
        )
        self.assertEqual(captured["posix"][1]["bundle_name"], "bundle")
        self.assertEqual(captured["posix"][1]["bundle_ref"], "ref")

if __name__ == "__main__":
    unittest.main()
