#!/usr/bin/env python3
"""Tests for Linux target window command dependency assembly."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("linux_target_window_command_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetWindowCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_window_dependencies_preserve_coordinate_parser_seam(self) -> None:
        bindings = {"parse_coordinate_pair": object()}

        deps = self.mod.linux_target_window_command_dependencies(bindings)

        self.assertIs(deps["parse_coordinate_pair_fn"], bindings["parse_coordinate_pair"])


if __name__ == "__main__":
    unittest.main()
