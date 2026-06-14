#!/usr/bin/env python3
"""Tests for Windows desktop action source/session-agent request bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("windows_desktop_action_source_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsDesktopActionSourceDependencyBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        return {
            "prepare_windows_exact_sha_source": object(),
            "build_windows_session_agent_request": object(),
            "windows_path_join": object(),
            "windows_ssh_write_text": object(),
            "start_windows_session_agent_task": object(),
            "windows_ssh_read_json": object(),
            "attach_desktop_source_to_manifest": object(),
        }

    def test_source_dependency_exports_match_wrappers(self):
        self.assertEqual(
            self.mod.WINDOWS_DESKTOP_ACTION_SOURCE_DEPENDENCY_EXPORTS,
            ("windows_desktop_action_source_dependencies",),
        )

    def test_source_dependencies_bind_facade_values(self):
        bindings = self._bindings()

        deps = self.mod.windows_desktop_action_source_dependencies(bindings)

        self.assertIs(deps["prepare_windows_exact_sha_source_fn"], bindings["prepare_windows_exact_sha_source"])
        self.assertIs(deps["build_windows_session_agent_request_fn"], bindings["build_windows_session_agent_request"])
        self.assertIs(deps["windows_path_join_fn"], bindings["windows_path_join"])
        self.assertIs(deps["windows_ssh_write_text_fn"], bindings["windows_ssh_write_text"])
        self.assertIs(deps["start_windows_session_agent_task_fn"], bindings["start_windows_session_agent_task"])
        self.assertIs(deps["windows_ssh_read_json_fn"], bindings["windows_ssh_read_json"])
        self.assertIs(deps["attach_desktop_source_to_manifest_fn"], bindings["attach_desktop_source_to_manifest"])

    def test_source_dependency_installer_wires_named_export(self):
        bindings = self._bindings()

        self.mod.install_windows_desktop_action_source_dependency_helpers(bindings)

        self.assertIs(
            bindings["windows_desktop_action_source_dependencies"]()["windows_path_join_fn"],
            bindings["windows_path_join"],
        )


if __name__ == "__main__":
    unittest.main()
