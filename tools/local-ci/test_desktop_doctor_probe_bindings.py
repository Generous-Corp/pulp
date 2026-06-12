#!/usr/bin/env python3
"""Tests for desktop doctor probe dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_doctor_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopDoctorProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        return {
            "_desktop_doctor": types.SimpleNamespace(),
            "sys": types.SimpleNamespace(platform="darwin"),
            "shutil": types.SimpleNamespace(which=object()),
            "urllib": types.SimpleNamespace(
                request=types.SimpleNamespace(Request=object(), urlopen=object()),
            ),
        }

    def test_desktop_doctor_and_webdriver_bind_dependencies(self):
        captured = {}

        def doctor_runner(*args, **kwargs):
            captured["doctor_args"] = args
            captured["doctor_kwargs"] = kwargs
            return [{"name": "ok"}]

        def webdriver_runner(*args, **kwargs):
            captured["webdriver_args"] = args
            captured["webdriver_kwargs"] = kwargs
            return {"ready": True}

        bindings = self._bindings()
        bindings["_desktop_doctor"].desktop_doctor_checks = doctor_runner
        bindings["_desktop_doctor"].probe_webdriver_endpoint = webdriver_runner
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

        self.assertEqual(self.mod.desktop_doctor_checks(bindings, {"desktop_automation": {}}, "mac"), [{"name": "ok"}])
        self.assertEqual(captured["doctor_args"], ({"desktop_automation": {}}, "mac"))
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
            self.assertIs(captured["doctor_kwargs"][f"{name}_fn"], bindings[name])
        self.assertEqual(captured["doctor_kwargs"]["platform"], "darwin")
        self.assertIs(captured["doctor_kwargs"]["which_fn"], bindings["shutil"].which)

        self.assertEqual(self.mod.probe_webdriver_endpoint(bindings, "http://driver", timeout=1.5), {"ready": True})
        self.assertEqual(captured["webdriver_args"], ("http://driver",))
        self.assertEqual(captured["webdriver_kwargs"]["timeout"], 1.5)
        self.assertIs(captured["webdriver_kwargs"]["request_cls"], bindings["urllib"].request.Request)
        self.assertIs(captured["webdriver_kwargs"]["urlopen_fn"], bindings["urllib"].request.urlopen)


if __name__ == "__main__":
    unittest.main()
