#!/usr/bin/env python3
"""Facade-level config, workflow, and target-resolution integration tests."""

from __future__ import annotations

import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_config_target_resolution_integration",
        add_module_dir=True,
    )


class ConfigTargetResolutionIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_config_workflow_and_target_resolution_edge_paths(self) -> None:
        with self.assertRaisesRegex(FileNotFoundError, "Local CI config not found"):
            self.mod.load_config_file(self.root / "missing.json")
        state_paths_mod = sys.modules["state_paths"]
        with mock.patch.object(state_paths_mod, "SCRIPT_DIR", self.root):
            with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
                self.assertIsNone(self.mod.load_optional_config())

        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json({"github_actions": {"workflows": []}}, "build", "namespace"),
            "",
        )
        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json(
                {"github_actions": {"workflows": {"build": []}}},
                "build",
                "namespace",
            ),
            "",
        )
        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json(
                {"github_actions": {"workflows": {"build": {"providers": []}}}},
                "build",
                "namespace",
            ),
            "",
        )
        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json(
                {"github_actions": {"workflows": {"build": {"providers": {"namespace": []}}}}},
                "build",
                "namespace",
            ),
            "",
        )
        self.assertEqual(self.mod.resolve_workflow_dispatch_field_values(None, "build", "namespace", None), {})
        self.assertEqual(
            self.mod.resolve_workflow_dispatch_field_values(
                {"github_actions": {"workflows": {"build": {"providers": {"namespace": []}}}}},
                "build",
                "namespace",
                ("linux_runner_selector_json",),
            ),
            {},
        )
        self.assertEqual(
            self.mod.resolve_default_provider_for_workflow({}, "build", explicit_provider="namespace"),
            ("namespace", "cli"),
        )
        with self.assertRaisesRegex(ValueError, "Unknown workflow"):
            self.mod.resolve_default_provider_for_workflow({}, "unknown")
        with self.assertRaisesRegex(ValueError, "does not support provider"):
            self.mod.resolve_default_provider_for_workflow({}, "validate", explicit_provider="namespace")
        self.assertEqual(self.mod.repo_variable_name_for_workflow_field("build", "github-hosted", "runner"), "")

        config = {
            "targets": {
                "mac": {"enabled": True},
                "windows": {"enabled": False},
            },
        }
        self.assertEqual(self.mod.enabled_targets(config), ["mac"])
        self.assertIsNone(self.mod.parse_targets_arg(""))
        self.assertEqual(self.mod.parse_targets_arg("windows, mac,windows"), ["mac", "windows"])
        self.assertEqual(self.mod.resolve_targets(config, None), ["mac"])
        self.assertEqual(self.mod.resolve_targets(config, []), [])
        with self.assertRaisesRegex(ValueError, "Unknown target"):
            self.mod.resolve_targets(config, ["ubuntu"])
        with self.assertRaisesRegex(ValueError, "disabled"):
            self.mod.resolve_targets(config, ["windows"])
        self.assertEqual(
            self.mod.resolve_targets(
                {"defaults": {"targets": "mac,mac"}, "targets": {"mac": {"enabled": True}}},
                None,
            ),
            ["mac"],
        )


if __name__ == "__main__":
    unittest.main()
