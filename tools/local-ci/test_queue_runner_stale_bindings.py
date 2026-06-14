#!/usr/bin/env python3
"""Tests for queue runner stale-job dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_runner_stale_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueRunnerStaleBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_runner_stale_exports_match_facade_helpers(self):
        expected = ("stale_running_jobs_unlocked",)

        self.assertEqual(self.mod.QUEUE_RUNNER_STALE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_stale_running_jobs_unlocked_binds_orchestrator_helper(self):
        captured = {}

        def stale_running_jobs_for_current_runner(queue, **kwargs):
            captured["stale"] = (queue, kwargs)
            return [{"id": "old"}]

        orchestrator = types.SimpleNamespace(stale_running_jobs_for_runner_unlocked=object())
        bindings = {
            "_runner_state": types.SimpleNamespace(
                stale_running_jobs_for_current_runner=stale_running_jobs_for_current_runner,
            ),
            "_queue_orchestrator": orchestrator,
        }

        self.assertEqual(self.mod.stale_running_jobs_unlocked(bindings, [{"id": "run"}]), [{"id": "old"}])
        self.assertEqual(captured["stale"][0], [{"id": "run"}])
        self.assertIs(
            captured["stale"][1]["stale_running_jobs_for_runner_unlocked_fn"],
            orchestrator.stale_running_jobs_for_runner_unlocked,
        )


if __name__ == "__main__":
    unittest.main()
