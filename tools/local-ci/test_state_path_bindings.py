#!/usr/bin/env python3
"""Tests for state path facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("state_path_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("state_path_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class StatePathBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        paths = types.SimpleNamespace(
            state_dir=make_runner("state_dir", Path("/state")),
            config_path=make_runner("config_path", Path("/state/config.json")),
            worktree_config_path=make_runner("worktree_config_path", Path("/repo/config.json")),
            shared_config_path=make_runner("shared_config_path", Path("/state/config.json")),
            queue_path=make_runner("queue_path", Path("/state/queue.json")),
            results_dir=make_runner("results_dir", Path("/state/results")),
            cloud_runs_dir=make_runner("cloud_runs_dir", Path("/state/cloud-runs")),
            evidence_path=make_runner("evidence_path", Path("/state/evidence.json")),
            logs_dir=make_runner("logs_dir", Path("/state/logs")),
            bundles_dir=make_runner("bundles_dir", Path("/state/bundles")),
            prepared_dir=make_runner("prepared_dir", Path("/state/prepared")),
            desktop_state_dir=make_runner("desktop_state_dir", Path("/state/desktop-automation")),
            desktop_receipts_dir=make_runner("desktop_receipts_dir", Path("/state/desktop-automation/receipts")),
            queue_lock_path=make_runner("queue_lock_path", Path("/state/queue.lock")),
            evidence_lock_path=make_runner("evidence_lock_path", Path("/state/evidence.lock")),
            drain_lock_path=make_runner("drain_lock_path", Path("/state/drain.lock")),
            runner_info_path=make_runner("runner_info_path", Path("/state/runner.json")),
            ensure_state_dirs=make_runner("ensure_state_dirs", None),
            job_logs_dir=make_runner("job_logs_dir", Path("/state/logs/job-1")),
            target_log_path=make_runner("target_log_path", Path("/state/logs/job-1/mac.log")),
            prepare_target_log=make_runner("prepare_target_log", Path("/state/logs/job-1/mac.log")),
        )
        return {"_state_paths": paths}, calls

    def test_nullary_path_helpers_delegate_to_state_paths_module(self):
        bindings, calls = self._bindings()
        helpers = [
            "state_dir",
            "config_path",
            "worktree_config_path",
            "shared_config_path",
            "queue_path",
            "results_dir",
            "cloud_runs_dir",
            "evidence_path",
            "logs_dir",
            "bundles_dir",
            "prepared_dir",
            "desktop_state_dir",
            "desktop_receipts_dir",
            "queue_lock_path",
            "evidence_lock_path",
            "drain_lock_path",
            "runner_info_path",
        ]

        for name in helpers:
            with self.subTest(name=name):
                self.assertIsInstance(getattr(self.mod, name)(bindings), Path)

        self.mod.ensure_state_dirs(bindings)
        self.assertEqual([call[0] for call in calls], [*helpers, "ensure_state_dirs"])
        self.assertTrue(all(call[1] == () and call[2] == {} for call in calls))

    def test_log_path_helpers_delegate_with_job_and_target_arguments(self):
        bindings, calls = self._bindings()

        self.assertEqual(self.mod.job_logs_dir(bindings, "job-1"), Path("/state/logs/job-1"))
        self.assertEqual(self.mod.target_log_path(bindings, "job-1", "mac"), Path("/state/logs/job-1/mac.log"))
        self.assertEqual(self.mod.prepare_target_log(bindings, "job-1", "mac"), Path("/state/logs/job-1/mac.log"))

        self.assertEqual([call[0] for call in calls], ["job_logs_dir", "target_log_path", "prepare_target_log"])
        self.assertEqual(calls[0][1], ("job-1",))
        self.assertEqual(calls[1][1], ("job-1", "mac"))
        self.assertEqual(calls[2][1], ("job-1", "mac"))


if __name__ == "__main__":
    unittest.main()
