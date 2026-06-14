#!/usr/bin/env python3
"""Tests for desktop doctor command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_doctor_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopDoctorCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_DOCTOR_COMMAND_EXPORTS, ("cmd_desktop_doctor",))

    def test_doctor_binds_setup_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 2

        bindings = {
            "_desktop_setup_commands_cli": types.SimpleNamespace(cmd_desktop_doctor=runner),
            "load_config": object(),
            "resolve_desktop_target": object(),
            "desktop_doctor_checks": object(),
        }
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_doctor(bindings, args_obj), 2)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["resolve_desktop_target_fn"], bindings["resolve_desktop_target"])
        self.assertIs(captured["kwargs"]["desktop_doctor_checks_fn"], bindings["desktop_doctor_checks"])

if __name__ == "__main__":
    unittest.main()
