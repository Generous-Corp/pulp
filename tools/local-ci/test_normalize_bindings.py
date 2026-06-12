#!/usr/bin/env python3
"""Tests for normalization facade bindings."""

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

    def test_priority_values_delegates_to_normalize_module(self):
        bindings, _calls = self._bindings()

        self.assertEqual(self.mod.priority_values(bindings), {"normal": 50, "high": 75})

    def test_scalar_normalizers_delegate_to_normalize_module(self):
        bindings, calls = self._bindings()

        self.assertEqual(self.mod.normalize_priority(bindings, "NORMAL"), "normal")
        self.assertEqual(self.mod.priority_value(bindings, "normal"), 50)
        self.assertEqual(self.mod.normalize_validation_mode(bindings, "FULL"), "full")
        self.assertEqual(self.mod.normalize_desktop_source_mode(bindings, "live"), "live")
        self.assertEqual(self.mod.default_desktop_artifact_root(bindings), Path("/runs"))
        self.assertEqual(self.mod.normalize_publish_mode(bindings, "branch"), "branch")
        self.assertTrue(self.mod.parse_config_bool(bindings, "yes"))

        self.assertEqual([call[0] for call in calls], [
            "normalize_priority",
            "priority_value",
            "normalize_validation_mode",
            "normalize_desktop_source_mode",
            "default_desktop_artifact_root",
            "normalize_publish_mode",
            "parse_config_bool",
        ])
        self.assertEqual(calls[0][1], ("NORMAL",))
        self.assertEqual(calls[4][1], ())

    def test_desktop_normalizers_delegate_to_normalize_module(self):
        bindings, calls = self._bindings()
        target_cfg = {"type": "local"}
        config = {"targets": {"mac": target_cfg}}

        self.assertEqual(self.mod.normalize_desktop_optional_config(bindings, {"webview_driver": "yes"}), {"webview_driver": True})
        self.assertEqual(self.mod.infer_desktop_adapter(bindings, "mac", target_cfg), "macos-local")
        self.assertEqual(self.mod.default_desktop_bootstrap(bindings, "macos-local"), "launchagent")
        self.assertEqual(self.mod.default_desktop_capability_tier(bindings, "macos-local"), "v2")
        self.assertEqual(self.mod.normalize_desktop_config(bindings, config), {"desktop_automation": {}})

        self.assertEqual([call[0] for call in calls], [
            "normalize_desktop_optional_config",
            "infer_desktop_adapter",
            "default_desktop_bootstrap",
            "default_desktop_capability_tier",
            "normalize_desktop_config",
        ])
        self.assertEqual(calls[1][1], ("mac", target_cfg))
        self.assertEqual(calls[4][1], (config,))

    def test_install_normalize_helpers_wires_named_exports(self):
        bindings, calls = self._bindings()
        self.mod.install_normalize_helpers(bindings, ("normalize_priority", "normalize_desktop_config"))

        self.assertEqual(bindings["normalize_priority"]("NORMAL"), "normal")
        self.assertEqual(bindings["normalize_desktop_config"]({"targets": {}}), {"desktop_automation": {}})
        self.assertEqual(bindings["normalize_priority"].__name__, "runner")
        self.assertEqual([call[0] for call in calls], ["normalize_priority", "normalize_desktop_config"])


if __name__ == "__main__":
    unittest.main()
