#!/usr/bin/env python3
"""Tests for target selection facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("target_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_target_exports_are_named_helpers(self):
        expected = (
            "enabled_targets",
            "parse_targets_arg",
            "resolve_targets",
        )

        self.assertEqual(self.mod.TARGET_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_target_helpers_routes_known_and_unknown_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_target_helpers(
                bindings,
                ("enabled_targets", "custom_target_export", "resolve_targets"),
            )

        install_local.assert_has_calls(
            [
                mock.call(bindings, self.mod.__dict__, ("enabled_targets", "resolve_targets")),
                mock.call(bindings, self.mod.__dict__, ("custom_target_export",)),
            ]
        )


if __name__ == "__main__":
    unittest.main()
