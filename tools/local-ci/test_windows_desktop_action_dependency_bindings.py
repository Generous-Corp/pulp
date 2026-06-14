#!/usr/bin/env python3
"""Tests for Windows desktop action dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_desktop_action_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsDesktopActionDependencyBindingsTests(unittest.TestCase):
    def setUp(self):
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
            "time": types.SimpleNamespace(time=object(), sleep=object()),
        }
        for name in [
            "ensure_host_reachable",
            "desktop_receipt_for",
            "desktop_target_contract",
            "probe_windows_session_agent",
            "windows_desktop_session_user",
            "create_desktop_run_bundle",
            "prepare_windows_exact_sha_source",
            "build_windows_session_agent_request",
            "windows_path_join",
            "windows_ssh_write_text",
            "start_windows_session_agent_task",
            "windows_ssh_read_json",
            "atomic_write_text",
            "windows_ssh_fetch_file",
            "windows_ssh_remove_path",
            "default_desktop_label",
            "image_change_summary",
            "attach_desktop_source_to_manifest",
            "write_desktop_run_rollups",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings, desktop_actions

    def test_dependency_exports_match_wrappers(self):
        expected = ("windows_desktop_action_dependencies",)

        self.assertEqual(self.mod.WINDOWS_DESKTOP_ACTION_DEPENDENCY_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_windows_desktop_action_dependencies_bind_facade_values(self):
        bindings, desktop_actions = self._bindings()

        deps = self.mod.windows_desktop_action_dependencies(bindings)

        self.assertIs(deps["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])
        self.assertIs(deps["desktop_target_contract_fn"], bindings["desktop_target_contract"])
        self.assertIs(deps["probe_windows_session_agent_fn"], bindings["probe_windows_session_agent"])
        self.assertIs(deps["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(deps["desktop_interaction_requested_fn"], desktop_actions.desktop_interaction_requested)
        self.assertIs(deps["time_fn"], bindings["time"].time)
        self.assertIs(deps["sleep_fn"], bindings["time"].sleep)
        self.assertIs(deps["windows_ssh_fetch_file_fn"], bindings["windows_ssh_fetch_file"])
        self.assertIs(deps["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])

    def test_install_dependency_helpers_wires_named_exports(self):
        bindings, _desktop_actions = self._bindings()

        self.mod.install_windows_desktop_action_dependency_helpers(bindings)

        self.assertIs(
            bindings["windows_desktop_action_dependencies"]()["windows_path_join_fn"],
            bindings["windows_path_join"],
        )


if __name__ == "__main__":
    unittest.main()
