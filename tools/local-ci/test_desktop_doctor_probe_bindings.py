#!/usr/bin/env python3
"""Tests for desktop doctor probe dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from unittest import mock
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

    def test_doctor_probe_exports_and_installer_wire_named_helpers(self):
        expected = (
            *self.mod.DESKTOP_DOCTOR_CHECK_EXPORTS,
            *self.mod.DESKTOP_DOCTOR_WEBDRIVER_PROBE_EXPORTS,
        )
        self.assertEqual(self.mod.DESKTOP_DOCTOR_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

        captured = {}

        def webdriver_runner(*args, **kwargs):
            captured["webdriver"] = (args, kwargs)
            return {"ready": True}

        bindings = self._bindings()
        bindings["_desktop_doctor"].probe_webdriver_endpoint = webdriver_runner

        self.mod.install_desktop_doctor_probe_helpers(bindings, ("probe_webdriver_endpoint",))

        self.assertEqual(bindings["probe_webdriver_endpoint"]("http://driver"), {"ready": True})
        self.assertNotIn("desktop_doctor_checks", bindings)
        self.assertEqual(captured["webdriver"][0], ("http://driver",))

    def test_doctor_probe_installer_routes_selected_groups(self):
        bindings = self._bindings()
        bindings["_desktop_doctor"].desktop_doctor_checks = lambda *args, **kwargs: [{"name": "ok"}]
        bindings["_desktop_doctor"].probe_webdriver_endpoint = lambda *args, **kwargs: {"ready": True}
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

        self.mod.install_desktop_doctor_probe_helpers(
            bindings,
            ("desktop_doctor_checks", "probe_webdriver_endpoint"),
        )

        self.assertEqual(bindings["desktop_doctor_checks"]({}, "mac"), [{"name": "ok"}])
        self.assertEqual(bindings["probe_webdriver_endpoint"]("http://driver"), {"ready": True})

    def test_doctor_probe_installer_preserves_unknown_fallback(self):
        bindings = self._bindings()

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_desktop_doctor_probe_helpers(bindings, ("unknown_helper",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
