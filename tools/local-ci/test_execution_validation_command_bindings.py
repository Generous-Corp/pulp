#!/usr/bin/env python3
"""Tests for validation command construction dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_validation_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionValidationCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_validation_command_helpers_delegate_to_execution_module(self):
        captured = {}

        def posix_ssh_validation_command(*args, **kwargs):
            captured["posix"] = (args, kwargs)
            return list(args), kwargs["exclude_tests"]

        def windows_validation_script(*args, **kwargs):
            captured["windows"] = (args, kwargs)
            return "script", "full"

        execution = types.SimpleNamespace(
            local_validation_command=lambda job, exclude_tests="": ([job["id"], exclude_tests], job.get("validation", "full")),
            posix_ssh_validation_command=posix_ssh_validation_command,
            windows_validation_script=windows_validation_script,
        )
        ps_literal = object()
        bindings = {"_execution": execution, "ps_literal": ps_literal}

        self.assertEqual(
            self.mod.local_validation_command(bindings, {"id": "job", "validation": "smoke"}, "slow"),
            (["job", "slow"], "smoke"),
        )
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

        self.assertEqual(
            self.mod.windows_validation_script(
                bindings,
                "windows",
                "host",
                r"C:\Repo",
                {"id": "job"},
                bundle_name="bundle",
                bundle_ref="ref",
                exclude_tests="slow",
                cmake_generator="Ninja",
                resolved_platform="ARM64",
                resolved_generator_instance=r"C:\VS",
            ),
            ("script", "full"),
        )
        self.assertEqual(captured["windows"][0], ("windows", "host", r"C:\Repo", {"id": "job"}))
        self.assertEqual(captured["windows"][1]["cmake_generator"], "Ninja")
        self.assertEqual(captured["windows"][1]["resolved_platform"], "ARM64")
        self.assertEqual(captured["windows"][1]["resolved_generator_instance"], r"C:\VS")
        self.assertIs(captured["windows"][1]["ps_literal_fn"], ps_literal)

    def test_validation_command_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.EXECUTION_LOCAL_COMMAND_EXPORTS,
            *self.mod.EXECUTION_POSIX_COMMAND_EXPORTS,
            *self.mod.EXECUTION_WINDOWS_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_VALIDATION_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_validation_command_installer_routes_focused_groups(self):
        execution = types.SimpleNamespace(
            local_validation_command=lambda job, exclude_tests="": ([job["id"], exclude_tests], job.get("validation", "full")),
            posix_ssh_validation_command=lambda *args, **kwargs: (list(args), kwargs["exclude_tests"]),
        )
        bindings = {"_execution": execution}

        self.mod.install_execution_validation_command_helpers(
            bindings,
            ("local_validation_command", "posix_ssh_validation_command"),
        )

        self.assertEqual(bindings["local_validation_command"]({"id": "job"}, "slow"), (["job", "slow"], "full"))
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
        self.assertNotIn("windows_validation_script", bindings)


if __name__ == "__main__":
    unittest.main()
