#!/usr/bin/env python3
"""Tests for queue runner stale-job dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_runner_stale_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueRunnerStaleDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_runner_stale_dependencies_bind_orchestrator_helper(self) -> None:
        orchestrator = types.SimpleNamespace(stale_running_jobs_for_runner_unlocked=object())
        bindings = {"_queue_orchestrator": orchestrator}

        deps = self.mod.queue_runner_stale_dependencies(bindings)

        self.assertIs(
            deps["stale_running_jobs_for_runner_unlocked_fn"],
            orchestrator.stale_running_jobs_for_runner_unlocked,
        )


if __name__ == "__main__":
    unittest.main()
