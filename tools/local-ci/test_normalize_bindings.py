#!/usr/bin/env python3
"""Tests for normalization compatibility facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("normalize_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class NormalizeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        normalize = types.SimpleNamespace(
            PRIORITY_VALUES={"normal": 50, "high": 75},
            normalize_priority=make_runner("normalize_priority", "normal"),
            priority_value=make_runner("priority_value", 50),
            normalize_validation_mode=make_runner("normalize_validation_mode", "full"),
            normalize_desktop_source_mode=make_runner("normalize_desktop_source_mode", "live"),
            default_desktop_artifact_root=make_runner("default_desktop_artifact_root", Path("/runs")),
            normalize_publish_mode=make_runner("normalize_publish_mode", "branch"),
            parse_config_bool=make_runner("parse_config_bool", True),
            normalize_desktop_optional_config=make_runner("normalize_desktop_optional_config", {"webview_driver": True}),
            infer_desktop_adapter=make_runner("infer_desktop_adapter", "macos-local"),
            default_desktop_bootstrap=make_runner("default_desktop_bootstrap", "launchagent"),
            default_desktop_capability_tier=make_runner("default_desktop_capability_tier", "v2"),
            normalize_desktop_config=make_runner("normalize_desktop_config", {"desktop_automation": {}}),
        )
        return {"_normalize": normalize}, calls

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

    def test_install_normalize_helpers_wires_named_exports(self):
        bindings, calls = self._bindings()
        self.mod.install_normalize_helpers(bindings, ("normalize_priority", "normalize_desktop_config"))

        self.assertEqual(bindings["normalize_priority"]("NORMAL"), "normal")
        self.assertEqual(bindings["normalize_desktop_config"]({"targets": {}}), {"desktop_automation": {}})
        self.assertEqual(bindings["normalize_priority"].__name__, "normalize_priority")
        self.assertEqual([call[0] for call in calls], ["normalize_priority", "normalize_desktop_config"])


if __name__ == "__main__":
    unittest.main()
