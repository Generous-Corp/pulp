#!/usr/bin/env python3
"""Tests for cleanup plan apply/display facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("cleanup_plan_apply_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupPlanApplyBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.cleanup = mock.Mock()
        self.bindings = {
            "_cleanup": self.cleanup,
            "format_size_bytes": mock.Mock(name="format_size_bytes"),
            "describe_path_for_cleanup": mock.Mock(name="describe_path_for_cleanup"),
        }

    def test_cleanup_plan_apply_exports_match_facade_helpers(self) -> None:
        expected = (
            "apply_local_ci_cleanup_plan",
            "cleanup_plan_lines",
        )

        self.assertEqual(self.mod.CLEANUP_PLAN_APPLY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_apply_cleanup_plan_delegates_to_cleanup_module(self) -> None:
        self.cleanup.apply_local_ci_cleanup_plan.return_value = {"removed": []}

        self.assertEqual(
            self.mod.apply_local_ci_cleanup_plan(self.bindings, {"categories": {}}),
            {"removed": []},
        )
        self.cleanup.apply_local_ci_cleanup_plan.assert_called_once_with({"categories": {}})

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

    def test_install_cleanup_plan_apply_helpers_wires_named_exports(self) -> None:
        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cleanup_plan_apply_helpers(
                self.bindings,
                ("cleanup_plan_lines", "custom_cleanup_apply"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(self.bindings, self.mod.__dict__, ("cleanup_plan_lines",)),
                mock.call(self.bindings, self.mod.__dict__, ("custom_cleanup_apply",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
