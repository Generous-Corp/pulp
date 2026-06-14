#!/usr/bin/env python3
"""Tests for desktop doctor check dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_doctor_check_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopDoctorCheckBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        bindings = {
            "_desktop_doctor": types.SimpleNamespace(),
            "sys": types.SimpleNamespace(platform="darwin"),
            "shutil": types.SimpleNamespace(which=object()),
        }
        for name in [
            "resolve_desktop_target",
            "desktop_target_contract",
            "desktop_receipt_for",
            "macos_accessibility_trusted",
            "ssh_reachable",
            "ssh_failure_detail",
            "probe_linux_launch_backend",
            "probe_linux_remote_tooling",
            "probe_windows_session_agent",
            "probe_windows_remote_tooling",
            "probe_windows_repo_checkout",
            "probe_webdriver_endpoint",
        ]:
            bindings[name] = object()
        return bindings

    def test_doctor_check_exports_match_wrappers(self):
        self.assertEqual(self.mod.DESKTOP_DOCTOR_CHECK_EXPORTS, ("desktop_doctor_checks",))
        self.assertTrue(callable(self.mod.desktop_doctor_checks))

    def test_doctor_checks_bind_dependencies(self):
        captured = {}

        def doctor_runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return [{"name": "ok"}]

        bindings = self._bindings()
        bindings["_desktop_doctor"].desktop_doctor_checks = doctor_runner

        self.assertEqual(self.mod.desktop_doctor_checks(bindings, {"desktop_automation": {}}, "mac"), [{"name": "ok"}])
        self.assertEqual(captured["args"], ({"desktop_automation": {}}, "mac"))
        for name in [
            "resolve_desktop_target",
            "desktop_target_contract",
            "desktop_receipt_for",
            "macos_accessibility_trusted",
            "ssh_reachable",
            "ssh_failure_detail",
            "probe_linux_launch_backend",
            "probe_linux_remote_tooling",
            "probe_windows_session_agent",
            "probe_windows_remote_tooling",
            "probe_windows_repo_checkout",
            "probe_webdriver_endpoint",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])
        self.assertEqual(captured["kwargs"]["platform"], "darwin")
        self.assertIs(captured["kwargs"]["which_fn"], bindings["shutil"].which)

    def test_doctor_check_installer_wires_named_export(self):
        bindings = self._bindings()
        bindings["_desktop_doctor"].desktop_doctor_checks = lambda *args, **kwargs: [{"name": "ok"}]

        self.mod.install_desktop_doctor_check_helpers(bindings)

        self.assertEqual(bindings["desktop_doctor_checks"]({}, "mac"), [{"name": "ok"}])


if __name__ == "__main__":
    unittest.main()
