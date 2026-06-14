#!/usr/bin/env python3
"""Tests for local_ci facade cleanup binding wiring."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import pathlib
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("cleanup_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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

    def test_install_cleanup_helpers_routes_known_and_unknown_exports(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cleanup_helpers(
                bindings,
                ("result_file_job_id", "cleanup_stale_windows_validator", "custom_cleanup"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(
                    bindings,
                    self.mod.__dict__,
                    ("result_file_job_id", "cleanup_stale_windows_validator"),
                ),
                mock.call(bindings, self.mod.__dict__, ("custom_cleanup",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
