#!/usr/bin/env python3
"""Tests for target preflight facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("target_preflight_bindings.py")


class TargetPreflightBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_preflight_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.TARGET_REACHABILITY_EXPORTS,
            *self.mod.TARGET_CONFIG_PREFLIGHT_EXPORTS,
            *self.mod.TARGET_SUBMISSION_EXPORTS,
        )

        self.assertEqual(self.mod.TARGET_PREFLIGHT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_target_preflight_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_target_reachability_helpers") as reachability,
            mock.patch.object(self.mod, "install_target_config_preflight_helpers") as config,
            mock.patch.object(self.mod, "install_target_submission_helpers") as submission,
        ):
            self.mod.install_target_preflight_helpers(
                bindings,
                ("ssh_probe", "config_source_name", "print_submission_metadata"),
            )

        reachability.assert_called_once_with(bindings, ("ssh_probe",))
        config.assert_called_once_with(bindings, ("config_source_name",))
        submission.assert_called_once_with(bindings, ("print_submission_metadata",))


if __name__ == "__main__":
    unittest.main()
