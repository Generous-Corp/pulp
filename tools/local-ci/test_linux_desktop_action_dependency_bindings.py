#!/usr/bin/env python3
"""Tests for Linux desktop action dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("linux_desktop_action_dependency_bindings.py")


class LinuxDesktopActionDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        desktop_actions = types.SimpleNamespace(
            desktop_action_artifact_paths=object(),
            desktop_interaction_requested=object(),
            view_tree_inspector_summary=object(),
            pulp_app_interaction_summary=object(),
        )
        bindings = {
            "_desktop_actions": desktop_actions,
            "subprocess": types.SimpleNamespace(run=object()),
        }
        for name in [
            "ensure_host_reachable",
            "probe_linux_launch_backend",
            "create_desktop_run_bundle",
            "prepare_linux_exact_sha_source",
            "remote_linux_bundle_relpath",
            "build_linux_xvfb_remote_command",
            "build_linux_window_driver_remote_command",
            "fetch_ssh_artifact",
            "cleanup_remote_ssh_dir",
            "default_desktop_label",
            "image_change_summary",
            "parse_coordinate_pair",
            "attach_desktop_source_to_manifest",
            "atomic_write_text",
            "write_desktop_run_rollups",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings, desktop_actions

    def test_dependency_exports_match_wrappers(self) -> None:
        expected = ("linux_desktop_action_dependencies",)

        self.assertEqual(self.mod.LINUX_DESKTOP_ACTION_DEPENDENCY_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_linux_desktop_action_dependencies_bind_facade_values(self) -> None:
        bindings, desktop_actions = self._bindings()

        deps = self.mod.linux_desktop_action_dependencies(bindings)

        self.assertIs(deps["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])
        self.assertIs(deps["probe_linux_launch_backend_fn"], bindings["probe_linux_launch_backend"])
        self.assertIs(deps["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(deps["desktop_interaction_requested_fn"], desktop_actions.desktop_interaction_requested)
        self.assertIs(deps["run_fn"], bindings["subprocess"].run)
        self.assertIs(deps["fetch_ssh_artifact_fn"], bindings["fetch_ssh_artifact"])
        self.assertIs(deps["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])
        self.assertIs(deps["view_tree_inspector_summary_fn"], desktop_actions.view_tree_inspector_summary)

if __name__ == "__main__":
    unittest.main()
