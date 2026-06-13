#!/usr/bin/env python3
"""Tests for validation command state dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_command_state_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionCommandStateBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_state_helpers_delegate_to_execution_module(self):
        execution = types.SimpleNamespace(
            remote_commit_error=lambda target, host, job: f"{target}:{host}:{job['id']}",
            prepared_state_root=lambda target, validation: Path(f"/prepared/{target}/{validation}"),
            should_reuse_prepared_state=lambda job: job.get("reuse", False),
        )
        bindings = {"_execution": execution}

        self.assertEqual(self.mod.remote_commit_error(bindings, "mac", "host", {"id": "job"}), "mac:host:job")
        self.assertEqual(self.mod.prepared_state_root(bindings, "mac", "full"), Path("/prepared/mac/full"))
        self.assertTrue(self.mod.should_reuse_prepared_state(bindings, {"reuse": True}))
        self.assertFalse(self.mod.should_reuse_prepared_state(bindings, {}))

    def test_command_state_installer_wires_selected_exports(self):
        execution = types.SimpleNamespace(
            remote_commit_error=lambda target, host, job: f"{target}:{host}:{job['id']}",
        )
        bindings = {"_execution": execution}

        self.mod.install_execution_command_state_helpers(bindings, ("remote_commit_error",))

        self.assertEqual(bindings["remote_commit_error"]("mac", "host", {"id": "job"}), "mac:host:job")
        self.assertNotIn("prepared_state_root", bindings)

    def test_install_execution_command_state_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_execution_command_state_helper = lambda _bindings: "future"

        self.mod.install_execution_command_state_helpers(bindings, ("future_execution_command_state_helper",))

        self.assertEqual(bindings["future_execution_command_state_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
