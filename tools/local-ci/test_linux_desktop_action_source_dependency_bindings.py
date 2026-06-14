#!/usr/bin/env python3
"""Tests for Linux desktop action source/remote-command dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("linux_desktop_action_source_dependency_bindings.py")


class LinuxDesktopActionSourceDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        return {
            "prepare_linux_exact_sha_source": object(),
            "remote_linux_bundle_relpath": object(),
            "build_linux_xvfb_remote_command": object(),
            "build_linux_window_driver_remote_command": object(),
            "attach_desktop_source_to_manifest": object(),
        }

    def test_source_dependency_exports_match_wrappers(self) -> None:
        self.assertEqual(
            self.mod.LINUX_DESKTOP_ACTION_SOURCE_DEPENDENCY_EXPORTS,
            ("linux_desktop_action_source_dependencies",),
        )

    def test_source_dependencies_bind_facade_values(self) -> None:
        bindings = self._bindings()

        deps = self.mod.linux_desktop_action_source_dependencies(bindings)

        self.assertIs(deps["prepare_linux_exact_sha_source_fn"], bindings["prepare_linux_exact_sha_source"])
        self.assertIs(deps["remote_linux_bundle_relpath_fn"], bindings["remote_linux_bundle_relpath"])
        self.assertIs(deps["build_linux_xvfb_remote_command_fn"], bindings["build_linux_xvfb_remote_command"])
        self.assertIs(
            deps["build_linux_window_driver_remote_command_fn"],
            bindings["build_linux_window_driver_remote_command"],
        )
        self.assertIs(deps["attach_desktop_source_to_manifest_fn"], bindings["attach_desktop_source_to_manifest"])

if __name__ == "__main__":
    unittest.main()
