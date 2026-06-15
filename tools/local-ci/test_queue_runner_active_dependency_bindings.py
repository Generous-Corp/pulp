#!/usr/bin/env python3
"""Tests for queue runner active-target dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("queue_runner_active_dependency_bindings.py")


class QueueRunnerActiveDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_runner_active_dependencies_bind_orchestrator_mutator(self) -> None:
        captured = {}

        def update_runner_info_active_targets(info, job_id, active_targets, *, now_iso_fn):
            captured["mutate"] = (info, job_id, active_targets, now_iso_fn)
            return True

        bindings = {
            "_queue_orchestrator": types.SimpleNamespace(update_runner_info_active_targets=update_runner_info_active_targets),
            "now_iso": object(),
        }

        deps = self.mod.queue_runner_active_dependencies(bindings)

        self.assertTrue(deps["update_runner_info_active_targets_fn"]({"pid": 1}, "job1", {"mac": {"status": "pass"}}))
        self.assertEqual(captured["mutate"], ({"pid": 1}, "job1", {"mac": {"status": "pass"}}, bindings["now_iso"]))


if __name__ == "__main__":
    unittest.main()
