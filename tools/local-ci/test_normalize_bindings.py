#!/usr/bin/env python3
"""Tests for normalization compatibility facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("normalize_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class NormalizeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_facade_reexports_scalar_and_desktop_normalizers(self):
        expected_exports = (
            "normalize_priority",
            "priority_value",
            "normalize_validation_mode",
            "normalize_desktop_source_mode",
            "default_desktop_artifact_root",
            "normalize_publish_mode",
            "parse_config_bool",
            "normalize_desktop_optional_config",
            "infer_desktop_adapter",
            "default_desktop_bootstrap",
            "default_desktop_capability_tier",
            "normalize_desktop_config",
        )

        self.assertEqual(self.mod.NORMALIZE_EXPORTS, expected_exports)
        for name in ("priority_values", *expected_exports):
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_normalize_helpers_routes_known_and_unknown_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install:
            self.mod.install_normalize_helpers(bindings, ("normalize_priority", "normalize_desktop_config", "external"))

        self.assertEqual(
            install.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("normalize_priority", "normalize_desktop_config")),
                mock.call(bindings, self.mod.__dict__, ("external",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
