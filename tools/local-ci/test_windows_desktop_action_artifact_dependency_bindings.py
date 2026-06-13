#!/usr/bin/env python3
"""Tests for Windows desktop action artifact/rollup dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("windows_desktop_action_artifact_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsDesktopActionArtifactDependencyBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        desktop_actions = types.SimpleNamespace(desktop_action_artifact_paths=object())
        return {
            "_desktop_actions": desktop_actions,
            "create_desktop_run_bundle": object(),
            "atomic_write_text": object(),
            "windows_ssh_fetch_file": object(),
            "windows_ssh_remove_path": object(),
            "write_desktop_run_rollups": object(),
            "now_iso": object(),
        }, desktop_actions

    def test_artifact_dependency_exports_match_wrappers(self):
        self.assertEqual(
            self.mod.WINDOWS_DESKTOP_ACTION_ARTIFACT_DEPENDENCY_EXPORTS,
            ("windows_desktop_action_artifact_dependencies",),
        )

    def test_artifact_dependencies_bind_facade_values(self):
        bindings, desktop_actions = self._bindings()

        deps = self.mod.windows_desktop_action_artifact_dependencies(bindings)

        self.assertIs(deps["create_desktop_run_bundle_fn"], bindings["create_desktop_run_bundle"])
        self.assertIs(deps["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(deps["atomic_write_text_fn"], bindings["atomic_write_text"])
        self.assertIs(deps["windows_ssh_fetch_file_fn"], bindings["windows_ssh_fetch_file"])
        self.assertIs(deps["windows_ssh_remove_path_fn"], bindings["windows_ssh_remove_path"])
        self.assertIs(deps["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])
        self.assertIs(deps["now_iso_fn"], bindings["now_iso"])

    def test_artifact_dependency_installer_wires_named_export(self):
        bindings, desktop_actions = self._bindings()

        self.mod.install_windows_desktop_action_artifact_dependency_helpers(bindings)

        self.assertIs(
            bindings["windows_desktop_action_artifact_dependencies"]()["desktop_action_artifact_paths_fn"],
            desktop_actions.desktop_action_artifact_paths,
        )

    def test_artifact_dependency_installer_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_windows_desktop_action_artifact_helper = lambda _bindings: "future"

        self.mod.install_windows_desktop_action_artifact_dependency_helpers(
            bindings,
            ("future_windows_desktop_action_artifact_helper",),
        )

        self.assertEqual(bindings["future_windows_desktop_action_artifact_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
