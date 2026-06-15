#!/usr/bin/env python3
"""Tests for queue runner stale-job dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_runner_stale_bindings.py")


class QueueRunnerStaleBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_runner_stale_exports_match_facade_helpers(self):
        expected = ("stale_running_jobs_unlocked",)

        self.assertEqual(self.mod.QUEUE_RUNNER_STALE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_stale_running_jobs_unlocked_delegates_with_assembled_dependencies(self):
        captured = {}

        def stale_running_jobs_for_current_runner(queue, **kwargs):
            captured["stale"] = (queue, kwargs)
            return [{"id": "old"}]

        bindings = {
            "_runner_state": types.SimpleNamespace(
                stale_running_jobs_for_current_runner=stale_running_jobs_for_current_runner,
            ),
        }
        deps = {"stale_running_jobs_for_runner_unlocked_fn": object()}

        with mock.patch.object(self.mod, "queue_runner_stale_dependencies", return_value=deps):
            self.assertEqual(self.mod.stale_running_jobs_unlocked(bindings, [{"id": "run"}]), [{"id": "old"}])
        self.assertEqual(captured["stale"][0], [{"id": "run"}])
        self.assertIs(captured["stale"][1]["stale_running_jobs_for_runner_unlocked_fn"], deps["stale_running_jobs_for_runner_unlocked_fn"])

    def test_install_queue_runner_stale_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_runner_stale_helpers(bindings, ("stale_running_jobs_unlocked", "custom_runner_stale"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("stale_running_jobs_unlocked",)),
                mock.call(bindings, self.mod.__dict__, ("custom_runner_stale",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
