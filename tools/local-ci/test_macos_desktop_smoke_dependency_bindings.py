#!/usr/bin/env python3
"""Tests for macOS desktop smoke/action dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("macos_desktop_smoke_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosDesktopSmokeDependencyBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        desktop_actions = types.SimpleNamespace(
            desktop_action_artifact_paths=object(),
            desktop_interaction_requested=object(),
            content_size_from_window=object(),
            content_size_from_view_tree=object(),
            view_tree_inspector_summary=object(),
            pulp_app_interaction_summary=object(),
            desktop_click_selector=object(),
        )
        bindings = {
            "_desktop_actions": desktop_actions,
            "subprocess": types.SimpleNamespace(run=object(), Popen=object()),
            "time": types.SimpleNamespace(sleep=object()),
            "shlex": types.SimpleNamespace(split=object()),
            "os": types.SimpleNamespace(environ=types.SimpleNamespace(copy=object())),
        }
        for name in [
            "create_desktop_run_bundle",
            "macos_accessibility_trusted",
            "now_iso",
            "prepare_macos_exact_sha_source",
            "quit_macos_bundle_id",
            "activate_macos_bundle_id",
            "wait_for_macos_bundle_window",
            "detect_macos_app_bundle",
            "macos_bundle_id_for_app_path",
            "wait_for_macos_window",
            "wait_for_path",
            "capture_macos_window",
            "parse_coordinate_pair",
            "resolve_view_tree_click_point",
            "screen_point_for_content_point",
            "activate_macos_pid",
            "dispatch_macos_click",
            "image_change_summary",
            "attach_desktop_source_to_manifest",
            "atomic_write_text",
            "write_desktop_run_rollups",
            "terminate_process",
        ]:
            bindings[name] = object()
        return bindings, desktop_actions

    def test_dependency_exports_match_wrappers(self):
        expected = ("macos_desktop_smoke_dependencies",)

        self.assertEqual(self.mod.MACOS_DESKTOP_SMOKE_DEPENDENCY_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_macos_desktop_smoke_dependencies_bind_facade_values(self):
        bindings, desktop_actions = self._bindings()

        deps = self.mod.macos_desktop_smoke_dependencies(bindings)

        self.assertIs(deps["create_desktop_run_bundle_fn"], bindings["create_desktop_run_bundle"])
        self.assertIs(deps["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(deps["desktop_interaction_requested_fn"], desktop_actions.desktop_interaction_requested)
        self.assertIs(deps["content_size_from_window_fn"], desktop_actions.content_size_from_window)
        self.assertIs(deps["desktop_click_selector_fn"], desktop_actions.desktop_click_selector)
        self.assertIs(deps["run_fn"], bindings["subprocess"].run)
        self.assertIs(deps["popen_fn"], bindings["subprocess"].Popen)
        self.assertIs(deps["sleep_fn"], bindings["time"].sleep)
        self.assertIs(deps["split_command_fn"], bindings["shlex"].split)
        self.assertIs(deps["environ_copy_fn"], bindings["os"].environ.copy)
        self.assertIs(deps["terminate_process_fn"], bindings["terminate_process"])

    def test_install_dependency_helpers_wires_named_exports(self):
        bindings, _desktop_actions = self._bindings()

        self.mod.install_macos_desktop_smoke_dependency_helpers(bindings)

        self.assertIs(
            bindings["macos_desktop_smoke_dependencies"]()["wait_for_macos_window_fn"],
            bindings["wait_for_macos_window"],
        )


if __name__ == "__main__":
    unittest.main()
