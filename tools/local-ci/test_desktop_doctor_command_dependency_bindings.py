#!/usr/bin/env python3
"""Tests for desktop doctor command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("desktop_doctor_command_dependency_bindings.py")


class DesktopDoctorCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_desktop_doctor_command_dependencies_preserve_setup_seams(self) -> None:
        bindings = {
            "load_config": object(),
            "resolve_desktop_target": object(),
            "desktop_doctor_checks": object(),
        }

        deps = self.mod.desktop_doctor_command_dependencies(bindings)

        self.assertIs(deps["load_config_fn"], bindings["load_config"])
        self.assertIs(deps["resolve_desktop_target_fn"], bindings["resolve_desktop_target"])
        self.assertIs(deps["desktop_doctor_checks_fn"], bindings["desktop_doctor_checks"])


if __name__ == "__main__":
    unittest.main()
