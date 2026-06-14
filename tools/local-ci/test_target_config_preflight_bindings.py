#!/usr/bin/env python3
"""Tests for target config preflight facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("target_config_preflight_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetConfigPreflightBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_config_preflight_exports_are_declared(self) -> None:
        self.assertEqual(
            self.mod.TARGET_CONFIG_PREFLIGHT_EXPORTS,
            (
                "config_source_name",
                "config_material_for_targets",
                "find_material_config_drift",
            ),
        )

    def test_config_helpers_delegate_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        preflight = types.SimpleNamespace(
            config_source_name=capture("source", "env-override"),
            config_material_for_targets=lambda *args: captured.setdefault("material", args) and {"mac": {}},
            find_material_config_drift=capture("drift", ["drift"]),
        )
        bindings = {
            "_target_preflight": preflight,
            "os": types.SimpleNamespace(environ={"PULP_LOCAL_CI_CONFIG": "/config"}),
            "shared_config_path": object(),
            "worktree_config_path": object(),
            "config_material_for_targets": object(),
        }

        self.assertEqual(self.mod.config_source_name(bindings, Path("/config")), "env-override")
        self.assertIs(captured["source"][1]["environ"], bindings["os"].environ)
        self.assertIs(captured["source"][1]["shared_config_path_fn"], bindings["shared_config_path"])
        self.assertEqual(self.mod.config_material_for_targets(bindings, {"targets": {}}, ["mac"]), {"mac": {}})
        self.assertEqual(captured["material"], ({"targets": {}}, ["mac"]))
        self.assertEqual(self.mod.find_material_config_drift(bindings, ["mac"]), ["drift"])
        self.assertIs(captured["drift"][1]["shared_config_path_fn"], bindings["shared_config_path"])
        self.assertIs(captured["drift"][1]["worktree_config_path_fn"], bindings["worktree_config_path"])
        self.assertIs(captured["drift"][1]["config_material_for_targets_fn"], bindings["config_material_for_targets"])

    def test_install_target_config_preflight_helpers_wires_named_exports(self) -> None:
        preflight = types.SimpleNamespace(config_material_for_targets=lambda config, targets: {"targets": targets})
        bindings = {"_target_preflight": preflight}

        self.mod.install_target_config_preflight_helpers(bindings, ("config_material_for_targets",))

        self.assertEqual(bindings["config_material_for_targets"]({}, ["mac"]), {"targets": ["mac"]})
        self.assertEqual(bindings["config_material_for_targets"].__name__, "config_material_for_targets")
        self.assertNotIn("config_source_name", bindings)


if __name__ == "__main__":
    unittest.main()
