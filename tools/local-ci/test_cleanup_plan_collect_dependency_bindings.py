#!/usr/bin/env python3
"""Tests for cleanup plan collection dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("cleanup_plan_collect_dependency_bindings.py")


class CleanupPlanCollectDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_plan_retention_values_use_defaults(self) -> None:
        bindings = {"KEEP_COMPLETED_JOBS": 17}

        deps = self.mod.cleanup_plan_retention_values(bindings, keep_results=None, keep_logs=None)

        self.assertEqual(deps, {"keep_results": 17, "keep_logs": 17})

    def test_cleanup_plan_retention_values_preserve_explicit_values(self) -> None:
        bindings = {"KEEP_COMPLETED_JOBS": 17}

        deps = self.mod.cleanup_plan_retention_values(bindings, keep_results=3, keep_logs=4)

        self.assertEqual(deps, {"keep_results": 3, "keep_logs": 4})

    def test_cleanup_plan_collect_dependencies_preserve_path_seams(self) -> None:
        bindings = {
            "bundles_dir": object(),
            "logs_dir": object(),
            "results_dir": object(),
            "prepared_dir": object(),
            "path_size_bytes": object(),
        }

        deps = self.mod.cleanup_plan_collect_dependencies(bindings)

        self.assertIs(deps["bundles_dir_fn"], bindings["bundles_dir"])
        self.assertIs(deps["logs_dir_fn"], bindings["logs_dir"])
        self.assertIs(deps["results_dir_fn"], bindings["results_dir"])
        self.assertIs(deps["prepared_dir_fn"], bindings["prepared_dir"])
        self.assertIs(deps["path_size_bytes_fn"], bindings["path_size_bytes"])


if __name__ == "__main__":
    unittest.main()
