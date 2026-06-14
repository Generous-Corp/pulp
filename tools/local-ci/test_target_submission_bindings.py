#!/usr/bin/env python3
"""Tests for target submission metadata compatibility bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("target_submission_bindings.py")


class TargetSubmissionBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_submission_exports_are_declared(self) -> None:
        expected = (
            *self.mod.TARGET_SUBMISSION_BUILD_EXPORTS,
            *self.mod.TARGET_SUBMISSION_PRINT_EXPORTS,
        )

        self.assertEqual(self.mod.TARGET_SUBMISSION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_target_submission_helpers_routes_focused_groups_and_unknown_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_target_submission_build_helpers") as build,
            mock.patch.object(self.mod, "install_target_submission_print_helpers") as print_,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_target_submission_helpers(
                bindings,
                ("build_submission_metadata", "print_submission_metadata", "custom_submission"),
            )

        build.assert_called_once_with(bindings, ("build_submission_metadata",))
        print_.assert_called_once_with(bindings, ("print_submission_metadata",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_submission",))


if __name__ == "__main__":
    unittest.main()
