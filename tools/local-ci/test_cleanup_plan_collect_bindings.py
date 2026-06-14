#!/usr/bin/env python3
"""Tests for cleanup plan collection facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("cleanup_plan_collect_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupPlanCollectBindingTests(unittest.TestCase):
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
        }

    def test_cleanup_plan_collect_exports_match_facade_helpers(self) -> None:
        self.assertEqual(self.mod.CLEANUP_PLAN_COLLECT_EXPORTS, ("collect_local_ci_cleanup_plan",))

    def test_collect_cleanup_plan_delegates_with_assembled_dependencies(self) -> None:
        self.cleanup.collect_local_ci_cleanup_plan.return_value = {"categories": {}}
        queue = [{"id": "job1"}]
        retention = {"keep_results": 1, "keep_logs": 2}
        deps = {
            "bundles_dir_fn": object(),
            "logs_dir_fn": object(),
            "results_dir_fn": object(),
            "prepared_dir_fn": object(),
            "path_size_bytes_fn": object(),
        }

        with (
            mock.patch.object(self.mod, "cleanup_plan_retention_values", return_value=retention),
            mock.patch.object(self.mod, "cleanup_plan_collect_dependencies", return_value=deps),
        ):
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
            **deps,
        )

    def test_collect_cleanup_plan_uses_default_retention(self) -> None:
        self.cleanup.collect_local_ci_cleanup_plan.return_value = {"categories": {}}
        retention = {"keep_results": 17, "keep_logs": 17}

        with mock.patch.object(self.mod, "cleanup_plan_retention_values", return_value=retention) as retention_values:
            self.assertEqual(
                self.mod.collect_local_ci_cleanup_plan(self.bindings, [{"id": "job1"}]),
                {"categories": {}},
            )
        retention_values.assert_called_once_with(self.bindings, keep_results=None, keep_logs=None)
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

    def test_install_cleanup_plan_collect_helpers_wires_named_exports(self) -> None:
        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cleanup_plan_collect_helpers(
                self.bindings,
                ("collect_local_ci_cleanup_plan", "custom_cleanup_collect"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(self.bindings, self.mod.__dict__, ("collect_local_ci_cleanup_plan",)),
                mock.call(self.bindings, self.mod.__dict__, ("custom_cleanup_collect",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
