#!/usr/bin/env python3
"""Tests for target preflight facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("target_preflight_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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
        calls = []

        def reachability_install(bindings, names):
            calls.append(("reachability", names))

        def config_install(bindings, names):
            calls.append(("config", names))

        def submission_install(bindings, names):
            calls.append(("submission", names))

        self.mod.install_target_reachability_helpers = reachability_install
        self.mod.install_target_config_preflight_helpers = config_install
        self.mod.install_target_submission_helpers = submission_install

        self.mod.install_target_preflight_helpers(
            {},
            ("ssh_probe", "config_source_name", "print_submission_metadata"),
        )

        self.assertEqual(
            calls,
            [
                ("reachability", ("ssh_probe",)),
                ("config", ("config_source_name",)),
                ("submission", ("print_submission_metadata",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
