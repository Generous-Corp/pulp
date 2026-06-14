#!/usr/bin/env python3
"""Tests for validation command state dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from pathlib import Path



def load_module():
    return load_local_ci_module("execution_command_state_bindings.py")


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

if __name__ == "__main__":
    unittest.main()
