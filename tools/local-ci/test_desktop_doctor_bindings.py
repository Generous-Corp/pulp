#!/usr/bin/env python3
"""Tests for desktop doctor dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_doctor_bindings.py")


class DesktopDoctorBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_doctor_exports_are_declared(self) -> None:
        self.assertEqual(
            self.mod.DESKTOP_DOCTOR_EXPORTS,
            (
                "desktop_optional_capabilities",
                "desktop_capabilities_for",
                "desktop_check",
                "check_writable_dir",
                "webdriver_status_url",
            ),
        )

    def test_doctor_wrappers_delegate_arguments(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        doctor = types.SimpleNamespace(
            desktop_optional_capabilities=capture("optional", ["webview_dom"]),
            desktop_capabilities_for=capture("caps", ["launch_app"]),
            desktop_check=capture("check", {"ok": True}),
            check_writable_dir=capture("writable", (True, "/tmp")),
            webdriver_status_url=capture("webdriver", "http://host/status"),
        )
        bindings = {"_desktop_doctor": doctor}

        self.assertEqual(self.mod.desktop_optional_capabilities(bindings, {"webview_driver": True}), ["webview_dom"])
        self.assertEqual(captured["optional"][0], ({"webview_driver": True},))
        self.assertEqual(self.mod.desktop_capabilities_for(bindings, "macos-local", "v2", {"debug_attach": True}), ["launch_app"])
        self.assertEqual(captured["caps"][0], ("macos-local", "v2", {"debug_attach": True}))
        self.assertEqual(self.mod.desktop_check(bindings, "ssh", True, "ok", required=False), {"ok": True})
        self.assertEqual(captured["check"][0], ("ssh", True, "ok"))
        self.assertEqual(captured["check"][1], {"required": False})
        self.assertEqual(self.mod.check_writable_dir(bindings, Path("/tmp")), (True, "/tmp"))
        self.assertEqual(self.mod.webdriver_status_url(bindings, "http://host"), "http://host/status")

    def test_install_desktop_doctor_helpers_wires_named_exports(self) -> None:
        doctor = types.SimpleNamespace(
            desktop_check=lambda name, ok, detail, *, required=True: {
                "name": name,
                "ok": ok,
                "detail": detail,
                "required": required,
            },
        )
        bindings = {"_desktop_doctor": doctor}

        self.mod.install_desktop_doctor_helpers(bindings, ("desktop_check",))

        self.assertEqual(
            bindings["desktop_check"]("ssh", False, "missing", required=False),
            {"name": "ssh", "ok": False, "detail": "missing", "required": False},
        )
        self.assertNotIn("webdriver_status_url", bindings)


if __name__ == "__main__":
    unittest.main()
