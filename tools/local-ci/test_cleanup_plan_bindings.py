#!/usr/bin/env python3
"""Tests for local-CI cleanup plan facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import pathlib
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("cleanup_plan_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupPlanBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.cleanup = mock.Mock()
        self.bindings = {
            "_cleanup": self.cleanup,
            "bundles_dir": mock.Mock(name="bundles_dir"),
            "logs_dir": mock.Mock(name="logs_dir"),
            "results_dir": mock.Mock(name="results_dir"),
            "prepared_dir": mock.Mock(name="prepared_dir"),
            "path_size_bytes": mock.Mock(name="path_size_bytes"),
            "format_size_bytes": mock.Mock(name="format_size_bytes"),
            "describe_path_for_cleanup": mock.Mock(name="describe_path_for_cleanup"),
        }

    def test_cleanup_plan_exports_match_facade_helpers(self) -> None:
        expected = (
            "result_file_job_id",
            "artifact_entry_sort_key",
            "collect_local_ci_cleanup_plan",
            "apply_local_ci_cleanup_plan",
            "cleanup_plan_lines",
        )

        self.assertEqual(self.mod.CLEANUP_PLAN_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_simple_cleanup_helpers_delegate_to_cleanup_module(self) -> None:
        path = pathlib.Path("/state/results/job1.json")
        entry = {"mtime": 1.0}
        self.cleanup.result_file_job_id.return_value = "job1"
        self.cleanup.artifact_entry_sort_key.return_value = (1.0, "job1")
        self.cleanup.apply_local_ci_cleanup_plan.return_value = {"removed": []}

        self.assertEqual(self.mod.result_file_job_id(self.bindings, path), "job1")
        self.assertEqual(self.mod.artifact_entry_sort_key(self.bindings, entry), (1.0, "job1"))
        self.assertEqual(self.mod.apply_local_ci_cleanup_plan(self.bindings, {"categories": {}}), {"removed": []})
        self.cleanup.result_file_job_id.assert_called_once_with(path)
        self.cleanup.artifact_entry_sort_key.assert_called_once_with(entry)
        self.cleanup.apply_local_ci_cleanup_plan.assert_called_once_with({"categories": {}})

    def test_collect_cleanup_plan_wires_facade_dependencies(self) -> None:
        self.cleanup.collect_local_ci_cleanup_plan.return_value = {"categories": {}}
        queue = [{"id": "job1"}]

        result = self.mod.collect_local_ci_cleanup_plan(
            self.bindings,
            queue,
            keep_results=1,
            keep_logs=2,
            keep_bundles=3,
            include_prepared=True,
        )

        self.assertEqual(result, {"categories": {}})
        self.cleanup.collect_local_ci_cleanup_plan.assert_called_once_with(
            queue,
            keep_results=1,
            keep_logs=2,
            keep_bundles=3,
            include_prepared=True,
            bundles_dir_fn=self.bindings["bundles_dir"],
            logs_dir_fn=self.bindings["logs_dir"],
            results_dir_fn=self.bindings["results_dir"],
            prepared_dir_fn=self.bindings["prepared_dir"],
            path_size_bytes_fn=self.bindings["path_size_bytes"],
        )

    def test_collect_cleanup_plan_uses_default_retention(self) -> None:
        self.cleanup.collect_local_ci_cleanup_plan.return_value = {"categories": {}}
        self.bindings["KEEP_COMPLETED_JOBS"] = 17

        self.assertEqual(self.mod.collect_local_ci_cleanup_plan(self.bindings, [{"id": "job1"}]), {"categories": {}})
        self.cleanup.collect_local_ci_cleanup_plan.assert_called_once_with(
            [{"id": "job1"}],
            keep_results=17,
            keep_logs=17,
            keep_bundles=0,
            include_prepared=False,
            bundles_dir_fn=self.bindings["bundles_dir"],
            logs_dir_fn=self.bindings["logs_dir"],
            results_dir_fn=self.bindings["results_dir"],
            prepared_dir_fn=self.bindings["prepared_dir"],
            path_size_bytes_fn=self.bindings["path_size_bytes"],
        )

    def test_cleanup_plan_lines_wires_formatters(self) -> None:
        self.cleanup.cleanup_plan_lines.return_value = ["line"]
        plan = {"categories": {}}

        result = self.mod.cleanup_plan_lines(self.bindings, plan, dry_run=False)

        self.assertEqual(result, ["line"])
        self.cleanup.cleanup_plan_lines.assert_called_once_with(
            plan,
            dry_run=False,
            format_size_fn=self.bindings["format_size_bytes"],
            describe_path_fn=self.bindings["describe_path_for_cleanup"],
        )

    def test_install_cleanup_plan_helpers_wires_named_exports(self) -> None:
        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cleanup_plan_helpers(self.bindings, ("result_file_job_id", "custom_cleanup_plan"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(self.bindings, self.mod.__dict__, ("result_file_job_id",)),
                mock.call(self.bindings, self.mod.__dict__, ("custom_cleanup_plan",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
