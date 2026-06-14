#!/usr/bin/env python3
"""Tests for queue/result/evidence state path facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("state_path_store_bindings.py")


class StatePathStoreBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        paths = types.SimpleNamespace(
            queue_path=make_runner("queue_path", Path("/state/queue.json")),
            results_dir=make_runner("results_dir", Path("/state/results")),
            cloud_runs_dir=make_runner("cloud_runs_dir", Path("/state/cloud-runs")),
            evidence_path=make_runner("evidence_path", Path("/state/evidence.json")),
            logs_dir=make_runner("logs_dir", Path("/state/logs")),
            ensure_state_dirs=make_runner("ensure_state_dirs", None),
        )
        return {"_state_paths": paths}, calls

    def test_store_path_exports_match_facade_helpers(self) -> None:
        expected = (
            "queue_path",
            "results_dir",
            "cloud_runs_dir",
            "evidence_path",
            "logs_dir",
            "ensure_state_dirs",
        )

        self.assertEqual(self.mod.STATE_PATH_STORE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_store_path_helpers_delegate_to_state_paths_module(self) -> None:
        bindings, calls = self._bindings()

        self.assertEqual(self.mod.queue_path(bindings), Path("/state/queue.json"))
        self.assertEqual(self.mod.results_dir(bindings), Path("/state/results"))
        self.assertEqual(self.mod.cloud_runs_dir(bindings), Path("/state/cloud-runs"))
        self.assertEqual(self.mod.evidence_path(bindings), Path("/state/evidence.json"))
        self.assertEqual(self.mod.logs_dir(bindings), Path("/state/logs"))
        self.mod.ensure_state_dirs(bindings)

        self.assertEqual(
            [call[0] for call in calls],
            ["queue_path", "results_dir", "cloud_runs_dir", "evidence_path", "logs_dir", "ensure_state_dirs"],
        )
        self.assertTrue(all(call[1] == () and call[2] == {} for call in calls))


if __name__ == "__main__":
    unittest.main()
