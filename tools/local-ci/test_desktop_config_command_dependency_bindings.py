#!/usr/bin/env python3
"""Tests for desktop config command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_config_command_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopConfigCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_show_dependencies_preserve_config_display_seams(self) -> None:
        bindings = {
            "_desktop_cli": types.SimpleNamespace(desktop_config_show_lines=object()),
            "load_config": object(),
        }

        deps = self.mod.desktop_config_show_command_dependencies(bindings)

        self.assertIs(deps["load_config_fn"], bindings["load_config"])
        self.assertIs(deps["desktop_config_show_lines_fn"], bindings["_desktop_cli"].desktop_config_show_lines)

    def test_set_dependencies_preserve_config_update_seams(self) -> None:
        bindings = {
            "_desktop_cli": types.SimpleNamespace(desktop_config_update_lines=object()),
            "load_config": object(),
            "save_config": object(),
            "config_path": object(),
            "normalize_publish_mode": object(),
            "parse_config_bool": object(),
            "normalize_desktop_config": object(),
        }

        deps = self.mod.desktop_config_set_command_dependencies(bindings)

        self.assertIs(deps["load_config_fn"], bindings["load_config"])
        self.assertIs(deps["save_config_fn"], bindings["save_config"])
        self.assertIs(deps["config_path_fn"], bindings["config_path"])
        self.assertIs(deps["normalize_publish_mode_fn"], bindings["normalize_publish_mode"])
        self.assertIs(deps["parse_config_bool_fn"], bindings["parse_config_bool"])
        self.assertIs(deps["normalize_desktop_config_fn"], bindings["normalize_desktop_config"])
        self.assertIs(deps["desktop_config_update_lines_fn"], bindings["_desktop_cli"].desktop_config_update_lines)


if __name__ == "__main__":
    unittest.main()
