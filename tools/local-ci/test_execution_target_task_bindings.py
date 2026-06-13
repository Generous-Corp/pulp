#!/usr/bin/env python3
"""Tests for validation target task facade bindings."""

from __future__ import annotations

import types
import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_target_task_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionTargetTaskBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def bindings(self, runner_name: str, runner):
        bindings = {"_execution": types.SimpleNamespace(**{runner_name: runner}), "print": object()}
        for name in [
            "enabled_targets",
            "resolve_ssh_target_execution",
            "run_local_validation",
            "run_posix_ssh_validation",
            "run_windows_ssh_validation",
            "config_for_job_execution",
            "_build_target_tasks",
            "target_state_snapshot",
            "update_runner_active_targets",
            "update_job_active_targets",
            "updated_target_state",
            "initial_target_state",
            "completed_target_state",
            "now_iso",
            "run_target_tasks",
            "completed_job_result",
            "sorted_target_results",
            "short_sha",
        ]:
            bindings[name] = object()
        return bindings

    def test_build_target_tasks_and_process_job_bind_facade_dependencies(self) -> None:
        captured = {}

        def build_runner(*args, **kwargs):
            captured["build"] = (args, kwargs)
            return [("mac", object())]

        def process_runner(*args, **kwargs):
            captured["process"] = (args, kwargs)
            return {"overall": "pass"}

        bindings = self.bindings("build_target_tasks", build_runner)
        bindings["_execution"].process_job = process_runner
        progress_factory = object()

        self.assertEqual(len(self.mod.build_target_tasks(bindings, {"id": "job"}, {"targets": {}}, progress_factory)), 1)
        self.assertIs(captured["build"][1]["enabled_targets_fn"], bindings["enabled_targets"])
        self.assertIs(captured["build"][1]["resolve_ssh_target_execution_fn"], bindings["resolve_ssh_target_execution"])
        self.assertIs(captured["build"][1]["run_local_validation_fn"], bindings["run_local_validation"])
        self.assertIs(captured["build"][1]["run_posix_ssh_validation_fn"], bindings["run_posix_ssh_validation"])
        self.assertIs(captured["build"][1]["run_windows_ssh_validation_fn"], bindings["run_windows_ssh_validation"])
        self.assertIs(captured["build"][1]["progress_factory"], progress_factory)

        self.assertEqual(self.mod.process_job(bindings, {"id": "job"}, {"targets": {}}), {"overall": "pass"})
        self.assertEqual(captured["process"][0], ({"id": "job"}, {"targets": {}}))
        self.assertIs(captured["process"][1]["print_fn"], bindings["print"])
        self.assertIs(captured["process"][1]["short_sha_fn"], bindings["short_sha"])
        self.assertIs(captured["process"][1]["config_for_job_execution_fn"], bindings["config_for_job_execution"])
        self.assertIs(captured["process"][1]["build_target_tasks_fn"], bindings["_build_target_tasks"])
        self.assertIs(captured["process"][1]["target_state_snapshot_fn"], bindings["target_state_snapshot"])
        self.assertIs(captured["process"][1]["run_target_tasks_fn"], bindings["run_target_tasks"])
        self.assertIs(captured["process"][1]["completed_job_result_fn"], bindings["completed_job_result"])


if __name__ == "__main__":
    unittest.main()
