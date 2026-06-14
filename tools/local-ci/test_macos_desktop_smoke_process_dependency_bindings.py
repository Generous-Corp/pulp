#!/usr/bin/env python3
"""Tests for macOS desktop smoke process dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("macos_desktop_smoke_process_dependency_bindings.py")


class MacosDesktopSmokeProcessDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self) -> dict:
        bindings = {
            "subprocess": types.SimpleNamespace(run=object(), Popen=object()),
            "time": types.SimpleNamespace(sleep=object()),
            "shlex": types.SimpleNamespace(split=object()),
            "os": types.SimpleNamespace(environ=types.SimpleNamespace(copy=object())),
        }
        for name in [
            "macos_accessibility_trusted",
            "quit_macos_bundle_id",
            "activate_macos_bundle_id",
            "wait_for_macos_bundle_window",
            "detect_macos_app_bundle",
            "macos_bundle_id_for_app_path",
            "terminate_process",
        ]:
            bindings[name] = object()
        return bindings

    def test_process_dependency_exports_match_wrappers(self) -> None:
        expected = ("macos_desktop_smoke_process_dependencies",)

        self.assertEqual(self.mod.MACOS_DESKTOP_SMOKE_PROCESS_DEPENDENCY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_process_dependencies_bind_facade_values(self) -> None:
        bindings = self._bindings()

        deps = self.mod.macos_desktop_smoke_process_dependencies(bindings)

        self.assertIs(deps["macos_accessibility_trusted_fn"], bindings["macos_accessibility_trusted"])
        self.assertIs(deps["quit_macos_bundle_id_fn"], bindings["quit_macos_bundle_id"])
        self.assertIs(deps["sleep_fn"], bindings["time"].sleep)
        self.assertIs(deps["run_fn"], bindings["subprocess"].run)
        self.assertIs(deps["wait_for_macos_bundle_window_fn"], bindings["wait_for_macos_bundle_window"])
        self.assertIs(deps["split_command_fn"], bindings["shlex"].split)
        self.assertIs(deps["environ_copy_fn"], bindings["os"].environ.copy)
        self.assertIs(deps["popen_fn"], bindings["subprocess"].Popen)
        self.assertIs(deps["terminate_process_fn"], bindings["terminate_process"])


if __name__ == "__main__":
    unittest.main()
