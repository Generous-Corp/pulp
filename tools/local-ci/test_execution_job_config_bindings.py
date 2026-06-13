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

    def test_job_config_exports_match_wrappers(self) -> None:
        expected = (
            "config_for_job_execution",
            "submission_target_state",
            "resolve_ssh_target_execution",
        )

        self.assertEqual(self.mod.EXECUTION_JOB_CONFIG_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_job_config_helpers_wires_named_exports(self) -> None:
        captured = {}

        def config_runner(*args, **kwargs):
            captured["config"] = (args, kwargs)
            return {"targets": {}}

        bindings = {
            "_execution": types.SimpleNamespace(config_for_job_execution=config_runner),
            "print": object(),
            "load_config_file": object(),
        }

        self.mod.install_execution_job_config_helpers(bindings, ("config_for_job_execution",))

        self.assertEqual(bindings["config_for_job_execution"]({"id": "job"}, {"targets": {}}), {"targets": {}})
        self.assertIs(captured["config"][1]["load_config_file_fn"], bindings["load_config_file"])
        self.assertEqual(bindings["config_for_job_execution"].__name__, "config_for_job_execution")

    def test_install_execution_job_config_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_execution_job_config_helper = lambda _bindings: "future"

        self.mod.install_execution_job_config_helpers(bindings, ("future_execution_job_config_helper",))

        self.assertEqual(bindings["future_execution_job_config_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
