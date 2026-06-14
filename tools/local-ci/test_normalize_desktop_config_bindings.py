#!/usr/bin/env python3
"""Tests for desktop normalization dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("normalize_desktop_config_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class NormalizeDesktopConfigBindingsTests(unittest.TestCase):
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
            normalize_desktop_optional_config=make_runner("normalize_desktop_optional_config", {"webview_driver": True}),
            infer_desktop_adapter=make_runner("infer_desktop_adapter", "macos-local"),
            default_desktop_bootstrap=make_runner("default_desktop_bootstrap", "launchagent"),
            default_desktop_capability_tier=make_runner("default_desktop_capability_tier", "v2"),
            normalize_desktop_config=make_runner("normalize_desktop_config", {"desktop_automation": {}}),
        )
        return {"_normalize": normalize}, calls

    def test_desktop_normalizers_delegate_to_normalize_module(self):
        bindings, calls = self._bindings()
        target_cfg = {"type": "local"}
        config = {"targets": {"mac": target_cfg}}

        self.assertEqual(self.mod.normalize_desktop_optional_config(bindings, {"webview_driver": "yes"}), {"webview_driver": True})
        self.assertEqual(self.mod.infer_desktop_adapter(bindings, "mac", target_cfg), "macos-local")
        self.assertEqual(self.mod.default_desktop_bootstrap(bindings, "macos-local"), "launchagent")
        self.assertEqual(self.mod.default_desktop_capability_tier(bindings, "macos-local"), "v2")
        self.assertEqual(self.mod.normalize_desktop_config(bindings, config), {"desktop_automation": {}})

        self.assertEqual(
            [call[0] for call in calls],
            [
                "normalize_desktop_optional_config",
                "infer_desktop_adapter",
                "default_desktop_bootstrap",
                "default_desktop_capability_tier",
                "normalize_desktop_config",
            ],
        )
        self.assertEqual(calls[1][1], ("mac", target_cfg))
        self.assertEqual(calls[4][1], (config,))

    def test_install_normalize_desktop_config_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_normalize_desktop_config_helpers(
                bindings,
                ("normalize_desktop_config", "custom_desktop_normalizer"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("normalize_desktop_config",)),
                mock.call(bindings, self.mod.__dict__, ("custom_desktop_normalizer",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
