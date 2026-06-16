#!/usr/bin/env python3
"""Tests for queue job construction dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("queue_job_factory_dependency_bindings.py")


class QueueJobFactoryDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_job_factory_dependencies_bind_time_uuid_root_and_branch_validation(self) -> None:
        bindings = {
            "ROOT": Path("/repo"),
            "now_iso": object(),
            "uuid": types.SimpleNamespace(uuid4=lambda: types.SimpleNamespace(hex="uuidhex")),
            "validate_ci_branch_name": object(),
        }

        deps = self.mod.queue_job_factory_dependencies(bindings)

        self.assertIs(deps["now_iso_fn"], bindings["now_iso"])
        self.assertEqual(deps["uuid_hex_fn"](), "uuidhex")
        self.assertIs(deps["root"], bindings["ROOT"])
        self.assertIs(deps["validate_branch_fn"], bindings["validate_ci_branch_name"])


if __name__ == "__main__":
    unittest.main()
