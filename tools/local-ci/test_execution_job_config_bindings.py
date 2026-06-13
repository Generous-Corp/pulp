#!/usr/bin/env python3
"""Tests for validation job config facade bindings."""

from __future__ import annotations

import types
import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_job_config_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionJobConfigBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_config_and_ssh_resolution_bind_facade_dependencies(self) -> None:
        captured = {}

        def config_runner(*args, **kwargs):
            captured["config"] = (args, kwargs)
            return {"targets": {}}

        def resolve_runner(*args, **kwargs):
            captured["resolve"] = (args, kwargs)
            return "host", "/repo"

        bindings = {
            "_execution": types.SimpleNamespace(
                config_for_job_execution=config_runner,
                submission_target_state=lambda job, target: {"job": job["id"], "target": target},
                resolve_ssh_target_execution=resolve_runner,
            ),
            "print": object(),
            "load_config_file": object(),
            "ensure_host_reachable": object(),
        }

        self.assertEqual(self.mod.config_for_job_execution(bindings, {"id": "job"}, {"targets": {}}), {"targets": {}})
        self.assertEqual(captured["config"][0], ({"id": "job"}, {"targets": {}}))
        self.assertIs(captured["config"][1]["load_config_file_fn"], bindings["load_config_file"])
        self.assertIs(captured["config"][1]["warn_fn"], bindings["print"])
        self.assertEqual(self.mod.submission_target_state(bindings, {"id": "job"}, "mac"), {"job": "job", "target": "mac"})

        result = self.mod.resolve_ssh_target_execution(bindings, {"id": "job"}, "ubuntu", {"host": "u"}, {})
        self.assertEqual(result, ("host", "/repo"))
        self.assertEqual(captured["resolve"][0], ({"id": "job"}, "ubuntu", {"host": "u"}, {}))
        self.assertIs(captured["resolve"][1]["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])


if __name__ == "__main__":
    unittest.main()
