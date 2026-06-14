#!/usr/bin/env python3
"""Tests for desktop WebDriver probe dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_doctor_webdriver_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopDoctorWebdriverProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_webdriver_probe_exports_match_wrappers(self):
        self.assertEqual(self.mod.DESKTOP_DOCTOR_WEBDRIVER_PROBE_EXPORTS, ("probe_webdriver_endpoint",))
        self.assertTrue(callable(self.mod.probe_webdriver_endpoint))

    def test_webdriver_probe_binds_dependencies(self):
        captured = {}

        def webdriver_runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ready": True}

        bindings = {
            "_desktop_doctor": types.SimpleNamespace(probe_webdriver_endpoint=webdriver_runner),
            "urllib": types.SimpleNamespace(
                request=types.SimpleNamespace(Request=object(), urlopen=object()),
            ),
        }

        self.assertEqual(self.mod.probe_webdriver_endpoint(bindings, "http://driver", timeout=1.5), {"ready": True})
        self.assertEqual(captured["args"], ("http://driver",))
        self.assertEqual(captured["kwargs"]["timeout"], 1.5)
        self.assertIs(captured["kwargs"]["request_cls"], bindings["urllib"].request.Request)
        self.assertIs(captured["kwargs"]["urlopen_fn"], bindings["urllib"].request.urlopen)

    def test_webdriver_probe_installer_wires_named_export(self):
        bindings = {
            "_desktop_doctor": types.SimpleNamespace(probe_webdriver_endpoint=lambda *args, **kwargs: {"ready": True}),
            "urllib": types.SimpleNamespace(
                request=types.SimpleNamespace(Request=object(), urlopen=object()),
            ),
        }

        self.mod.install_desktop_doctor_webdriver_probe_helpers(bindings)

        self.assertEqual(bindings["probe_webdriver_endpoint"]("http://driver"), {"ready": True})


if __name__ == "__main__":
    unittest.main()
