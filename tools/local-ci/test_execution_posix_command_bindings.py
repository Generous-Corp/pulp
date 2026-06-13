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

    def test_posix_command_installer_wires_selected_exports(self):
        execution = types.SimpleNamespace(
            posix_ssh_validation_command=lambda *args, **kwargs: (list(args), kwargs["exclude_tests"]),
        )
        bindings = {"_execution": execution}

        self.mod.install_execution_posix_command_helpers(bindings, ("posix_ssh_validation_command",))

        self.assertEqual(
            bindings["posix_ssh_validation_command"](
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

    def test_install_execution_posix_command_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_execution_posix_command_helper = lambda _bindings: "future"

        self.mod.install_execution_posix_command_helpers(bindings, ("future_execution_posix_command_helper",))

        self.assertEqual(bindings["future_execution_posix_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
