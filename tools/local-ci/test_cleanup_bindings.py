#!/usr/bin/env python3
"""Tests for local_ci facade cleanup binding wiring."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("cleanup_bindings.py")


class CleanupBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.CLEANUP_PLAN_EXPORTS,
            *self.mod.CLEANUP_STALE_WINDOWS_EXPORTS,
        )

        self.assertEqual(self.mod.CLEANUP_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_cleanup_helpers_routes_focused_groups_and_unknown_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_cleanup_plan_helpers") as plan,
            mock.patch.object(self.mod, "install_cleanup_stale_windows_helpers") as stale_windows,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_cleanup_helpers(
                bindings,
                ("result_file_job_id", "cleanup_stale_windows_validator", "custom_cleanup"),
            )

        plan.assert_called_once_with(bindings, ("result_file_job_id",))
        stale_windows.assert_called_once_with(bindings, ("cleanup_stale_windows_validator",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_cleanup",))


if __name__ == "__main__":
    unittest.main()
