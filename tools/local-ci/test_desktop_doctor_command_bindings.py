#!/usr/bin/env python3
"""Tests for desktop doctor command facade bindings."""

from __future__ import annotations

import types
import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_doctor_command_bindings.py")


class DesktopDoctorCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_DOCTOR_COMMAND_EXPORTS, ("cmd_desktop_doctor",))

    def test_doctor_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 2

        bindings = {
            "_desktop_setup_commands_cli": types.SimpleNamespace(cmd_desktop_doctor=runner),
        }
        deps = {
            "load_config_fn": object(),
            "resolve_desktop_target_fn": object(),
            "desktop_doctor_checks_fn": object(),
        }
        args_obj = object()
        with mock.patch.object(self.mod, "desktop_doctor_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_desktop_doctor(bindings, args_obj), 2)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)

if __name__ == "__main__":
    unittest.main()
