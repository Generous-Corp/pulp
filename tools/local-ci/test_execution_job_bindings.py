#!/usr/bin/env python3
"""Tests for validation job/result facade bindings."""

from __future__ import annotations

import types
import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_job_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionJobBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        execution = types.SimpleNamespace(**{runner_name: runner})
        bindings = {"_execution": execution, "print": object()}
        for name in [
            "load_config_file",
            "ensure_host_reachable",
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
            "ensure_state_dirs",
            "results_dir",
            "update_evidence_index",
            "normalize_result",
            "result_validation_line",
            "result_execution_line",
            "result_target_lines",
            "result_overall_line",
        ]:
            bindings[name] = object()
        bindings["datetime"] = types.SimpleNamespace(now=object())
        return bindings

    def test_config_and_ssh_resolution_bind_facade_dependencies(self) -> None:
        captured = {}

        def config_runner(*args, **kwargs):
            captured["config"] = (args, kwargs)
            return {"targets": {}}

        def resolve_runner(*args, **kwargs):
            captured["resolve"] = (args, kwargs)
            return "host", "/repo"

        bindings = self._bindings("config_for_job_execution", config_runner)
        bindings["_execution"].submission_target_state = lambda job, target: {"job": job["id"], "target": target}
        bindings["_execution"].resolve_ssh_target_execution = resolve_runner

        self.assertEqual(self.mod.config_for_job_execution(bindings, {"id": "job"}, {"targets": {}}), {"targets": {}})
        self.assertEqual(captured["config"][0], ({"id": "job"}, {"targets": {}}))
        self.assertIs(captured["config"][1]["load_config_file_fn"], bindings["load_config_file"])
        self.assertIs(captured["config"][1]["warn_fn"], bindings["print"])
        self.assertEqual(self.mod.submission_target_state(bindings, {"id": "job"}, "mac"), {"job": "job", "target": "mac"})

        result = self.mod.resolve_ssh_target_execution(bindings, {"id": "job"}, "ubuntu", {"host": "u"}, {})
        self.assertEqual(result, ("host", "/repo"))
        self.assertEqual(captured["resolve"][0], ({"id": "job"}, "ubuntu", {"host": "u"}, {}))
        self.assertIs(captured["resolve"][1]["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])

    def test_execution_job_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.EXECUTION_JOB_CONFIG_EXPORTS,
            *self.mod.EXECUTION_TARGET_TASK_EXPORTS,
            *self.mod.EXECUTION_RESULT_IO_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_JOB_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_build_target_tasks_and_process_job_bind_facade_dependencies(self) -> None:
        captured = {}

        def build_runner(*args, **kwargs):
            captured["build"] = (args, kwargs)
            return [("mac", object())]

        def process_runner(*args, **kwargs):
            captured["process"] = (args, kwargs)
            return {"overall": "pass"}

        bindings = self._bindings("build_target_tasks", build_runner)
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

    def test_save_and_print_result_bind_facade_dependencies(self) -> None:
        captured = {}

        def save_runner(*args, **kwargs):
            captured["save"] = (args, kwargs)
            return Path("/state/result.json")

        def print_runner(*args, **kwargs):
            captured["print"] = (args, kwargs)

        bindings = self._bindings("save_result", save_runner)
        bindings["_execution"].print_result = print_runner
        result_payload = {"job_id": "job"}

        self.assertEqual(self.mod.save_result(bindings, result_payload), Path("/state/result.json"))
        self.assertEqual(captured["save"][0], (result_payload,))
        self.assertIs(captured["save"][1]["ensure_state_dirs_fn"], bindings["ensure_state_dirs"])
        self.assertIs(captured["save"][1]["results_dir_fn"], bindings["results_dir"])
        self.assertIs(captured["save"][1]["update_evidence_index_fn"], bindings["update_evidence_index"])
        self.assertIs(captured["save"][1]["now_fn"], bindings["datetime"].now)

        result_path = Path("/state/result.json")
        self.mod.print_result(bindings, result_payload, result_path)

        self.assertEqual(captured["print"][0], (result_payload, result_path))
        self.assertIs(captured["print"][1]["normalize_result_fn"], bindings["normalize_result"])
        self.assertIs(captured["print"][1]["result_validation_line_fn"], bindings["result_validation_line"])
        self.assertIs(captured["print"][1]["result_execution_line_fn"], bindings["result_execution_line"])
        self.assertIs(captured["print"][1]["result_target_lines_fn"], bindings["result_target_lines"])
        self.assertIs(captured["print"][1]["result_overall_line_fn"], bindings["result_overall_line"])
        self.assertIs(captured["print"][1]["print_fn"], bindings["print"])

    def test_install_execution_job_helpers_routes_each_group(self) -> None:
        captured = {}

        def config_runner(*args, **kwargs):
            captured["config"] = (args, kwargs)
            return {"targets": {}}

        def process_runner(*args, **kwargs):
            captured["process"] = (args, kwargs)
            return {"overall": "pass"}

        def save_runner(*args, **kwargs):
            captured["save"] = (args, kwargs)
            return Path("/state/result.json")

        bindings = self._bindings("config_for_job_execution", config_runner)
        bindings["_execution"].process_job = process_runner
        bindings["_execution"].save_result = save_runner

        self.mod.install_execution_job_helpers(
            bindings,
            (
                "config_for_job_execution",
                "process_job",
                "save_result",
            ),
        )

        self.assertEqual(bindings["config_for_job_execution"]({"id": "job"}, {"targets": {}}), {"targets": {}})
        self.assertEqual(bindings["process_job"]({"id": "job"}, {"targets": {}}), {"overall": "pass"})
        self.assertEqual(bindings["save_result"]({"job_id": "job"}), Path("/state/result.json"))
        self.assertEqual(list(captured), ["config", "process", "save"])

    def test_install_execution_job_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_execution_job_helper = lambda _bindings: "future"

        self.mod.install_execution_job_helpers(bindings, ("future_execution_job_helper",))

        self.assertEqual(bindings["future_execution_job_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
