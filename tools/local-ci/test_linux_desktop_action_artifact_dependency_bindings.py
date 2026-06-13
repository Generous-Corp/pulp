#!/usr/bin/env python3
"""Tests for Linux desktop action artifact/rollup dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("linux_desktop_action_artifact_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopActionArtifactDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        desktop_actions = types.SimpleNamespace(desktop_action_artifact_paths=object())
        return {
            "_desktop_actions": desktop_actions,
            "create_desktop_run_bundle": object(),
            "fetch_ssh_artifact": object(),
            "cleanup_remote_ssh_dir": object(),
            "atomic_write_text": object(),
            "write_desktop_run_rollups": object(),
            "now_iso": object(),
        }, desktop_actions

    def test_artifact_dependency_exports_match_wrappers(self) -> None:
        self.assertEqual(
            self.mod.LINUX_DESKTOP_ACTION_ARTIFACT_DEPENDENCY_EXPORTS,
            ("linux_desktop_action_artifact_dependencies",),
        )

    def test_artifact_dependencies_bind_facade_values(self) -> None:
        bindings, desktop_actions = self._bindings()

        deps = self.mod.linux_desktop_action_artifact_dependencies(bindings)

        self.assertIs(deps["create_desktop_run_bundle_fn"], bindings["create_desktop_run_bundle"])
        self.assertIs(deps["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(deps["fetch_ssh_artifact_fn"], bindings["fetch_ssh_artifact"])
        self.assertIs(deps["cleanup_remote_ssh_dir_fn"], bindings["cleanup_remote_ssh_dir"])
        self.assertIs(deps["atomic_write_text_fn"], bindings["atomic_write_text"])
        self.assertIs(deps["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])
        self.assertIs(deps["now_iso_fn"], bindings["now_iso"])

    def test_artifact_dependency_installer_wires_named_export(self) -> None:
        bindings, desktop_actions = self._bindings()

        self.mod.install_linux_desktop_action_artifact_dependency_helpers(bindings)

        self.assertIs(
            bindings["linux_desktop_action_artifact_dependencies"]()["desktop_action_artifact_paths_fn"],
            desktop_actions.desktop_action_artifact_paths,
        )


if __name__ == "__main__":
    unittest.main()
