#!/usr/bin/env python3
"""Tests for local-CI cleanup plan compatibility bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cleanup_plan_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupPlanBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.bindings = {}

    def test_cleanup_plan_exports_match_facade_helpers(self) -> None:
        expected = (
            *self.mod.CLEANUP_ARTIFACT_IDENTITY_EXPORTS,
            *self.mod.CLEANUP_PLAN_COLLECT_EXPORTS,
            *self.mod.CLEANUP_PLAN_APPLY_EXPORTS,
        )

        self.assertEqual(self.mod.CLEANUP_PLAN_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_cleanup_plan_helpers_routes_focused_groups_and_unknown_exports(self) -> None:
        with (
            mock.patch.object(self.mod, "install_cleanup_artifact_identity_helpers") as identity,
            mock.patch.object(self.mod, "install_cleanup_plan_collect_helpers") as collect,
            mock.patch.object(self.mod, "install_cleanup_plan_apply_helpers") as apply,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_cleanup_plan_helpers(
                self.bindings,
                (
                    "result_file_job_id",
                    "collect_local_ci_cleanup_plan",
                    "cleanup_plan_lines",
                    "custom_cleanup_plan",
                ),
            )

        identity.assert_called_once_with(self.bindings, ("result_file_job_id",))
        collect.assert_called_once_with(self.bindings, ("collect_local_ci_cleanup_plan",))
        apply.assert_called_once_with(self.bindings, ("cleanup_plan_lines",))
        install_local.assert_called_once_with(self.bindings, self.mod.__dict__, ("custom_cleanup_plan",))


if __name__ == "__main__":
    unittest.main()
