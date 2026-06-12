#!/usr/bin/env python3
"""Tests for validation command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_helpers_delegate_and_bind_dependencies(self):
        captured = {}

        def windows_validation_script(*args, **kwargs):
            captured["windows_script"] = (args, kwargs)
            return "script", "full"

        execution = types.SimpleNamespace(
            remote_commit_error=lambda target, host, job: f"{target}:{host}:{job['id']}",
            prepared_state_root=lambda target, validation: Path(f"/prepared/{target}/{validation}"),
            should_reuse_prepared_state=lambda job: job.get("reuse", False),
            local_validation_command=lambda job, exclude_tests="": ([job["id"], exclude_tests], job.get("validation", "full")),
            posix_ssh_validation_command=lambda *args, **kwargs: (list(args), kwargs["exclude_tests"]),
            windows_validation_script=windows_validation_script,
        )
        bindings = {"_execution": execution, "ps_literal": object()}

        self.assertEqual(self.mod.remote_commit_error(bindings, "mac", "host", {"id": "job"}), "mac:host:job")
        self.assertEqual(self.mod.prepared_state_root(bindings, "mac", "full"), Path("/prepared/mac/full"))
        self.assertTrue(self.mod.should_reuse_prepared_state(bindings, {"reuse": True}))
        self.assertEqual(self.mod.local_validation_command(bindings, {"id": "job", "validation": "smoke"}, "slow"), (["job", "slow"], "smoke"))
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
            )[1],
            "slow",
        )
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
        self.assertIs(captured["windows_script"][1]["ps_literal_fn"], bindings["ps_literal"])


if __name__ == "__main__":
    unittest.main()
