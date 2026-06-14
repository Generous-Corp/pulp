#!/usr/bin/env python3
"""Tests for Windows desktop action host/session dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("windows_desktop_action_host_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsDesktopActionHostDependencyBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        return {
            "ensure_host_reachable": object(),
            "desktop_receipt_for": object(),
            "desktop_target_contract": object(),
            "probe_windows_session_agent": object(),
            "windows_desktop_session_user": object(),
            "time": types.SimpleNamespace(time=object(), sleep=object()),
        }

    def test_host_dependency_exports_match_wrappers(self):
        self.assertEqual(
            self.mod.WINDOWS_DESKTOP_ACTION_HOST_DEPENDENCY_EXPORTS,
            ("windows_desktop_action_host_dependencies",),
        )

    def test_host_dependencies_bind_facade_values(self):
        bindings = self._bindings()

        deps = self.mod.windows_desktop_action_host_dependencies(bindings)

        self.assertIs(deps["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])
        self.assertIs(deps["desktop_receipt_for_fn"], bindings["desktop_receipt_for"])
        self.assertIs(deps["desktop_target_contract_fn"], bindings["desktop_target_contract"])
        self.assertIs(deps["probe_windows_session_agent_fn"], bindings["probe_windows_session_agent"])
        self.assertIs(deps["windows_desktop_session_user_fn"], bindings["windows_desktop_session_user"])
        self.assertIs(deps["time_fn"], bindings["time"].time)
        self.assertIs(deps["sleep_fn"], bindings["time"].sleep)

if __name__ == "__main__":
    unittest.main()
