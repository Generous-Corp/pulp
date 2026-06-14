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

    def test_doctor_probe_installer_routes_selected_groups_and_unknown_fallback(self):
        bindings = self._bindings()

        with (
            mock.patch.object(self.mod, "install_desktop_doctor_check_helpers") as checks,
            mock.patch.object(self.mod, "install_desktop_doctor_webdriver_probe_helpers") as webdriver,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_doctor_probe_helpers(
                bindings,
                ("desktop_doctor_checks", "probe_webdriver_endpoint", "unknown_helper"),
            )

        checks.assert_called_once_with(bindings, ("desktop_doctor_checks",))
        webdriver.assert_called_once_with(bindings, ("probe_webdriver_endpoint",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
