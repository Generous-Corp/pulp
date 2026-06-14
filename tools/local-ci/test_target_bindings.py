#!/usr/bin/env python3
"""Tests for target selection facade bindings."""

from module_test_utils import load_module_from_path
import unittest
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
        calls = []

        def local_install(bindings, globals_obj, names):
            calls.append(names)

        self.mod.install_local_helpers = local_install

        self.mod.install_target_helpers(
            {},
            ("enabled_targets", "custom_target_export", "resolve_targets"),
        )

        self.assertEqual(
            calls,
            [
                ("enabled_targets", "resolve_targets"),
                ("custom_target_export",),
            ],
        )


if __name__ == "__main__":
    unittest.main()
