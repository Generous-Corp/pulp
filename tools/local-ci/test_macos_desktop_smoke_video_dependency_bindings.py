#!/usr/bin/env python3
"""Tests for macOS desktop smoke video dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest


def load_module():
    return load_local_ci_module("macos_desktop_smoke_video_dependency_bindings.py")


class MacosDesktopSmokeVideoDependencyBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_video_dependencies_resolve_expected_callables(self):
        sentinels = {
            "wait_for_macos_bundle_window_title": object(),
            "wait_for_macos_bundle_secondary_window": object(),
            "launch_macos_terminal_proof_command": object(),
            "close_macos_terminal_windows_with_title": object(),
            "start_macos_window_video_recording": object(),
            "stop_macos_window_video_recording": object(),
            "mux_desktop_video_audio": object(),
            "generate_interaction_focus": object(),
            "compose_desktop_video_proof": object(),
            "create_issue_video_variant": object(),
        }

        class _Path:
            cwd = object()

        bindings = dict(sentinels)
        bindings["Path"] = _Path

        deps = self.mod.macos_desktop_smoke_video_dependencies(bindings)

        self.assertIs(deps["wait_for_macos_bundle_window_title_fn"], sentinels["wait_for_macos_bundle_window_title"])
        self.assertIs(deps["start_macos_window_video_recording_fn"], sentinels["start_macos_window_video_recording"])
        self.assertIs(deps["compose_desktop_video_proof_fn"], sentinels["compose_desktop_video_proof"])
        self.assertIs(deps["create_issue_video_variant_fn"], sentinels["create_issue_video_variant"])
        self.assertIs(deps["cwd_path_fn"], _Path.cwd)

    def test_exports_and_installer(self):
        self.assertEqual(
            self.mod.MACOS_DESKTOP_SMOKE_VIDEO_DEPENDENCY_EXPORTS,
            ("macos_desktop_smoke_video_dependencies",),
        )
        bindings: dict = {}
        self.mod.install_macos_desktop_smoke_video_dependency_helpers(bindings)
        self.assertTrue(callable(bindings["macos_desktop_smoke_video_dependencies"]))


if __name__ == "__main__":
    unittest.main()
