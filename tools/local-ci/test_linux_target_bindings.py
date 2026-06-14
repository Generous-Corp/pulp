#!/usr/bin/env python3
"""Tests for Linux target facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("linux_target_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_linux_target_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.LINUX_TARGET_PROBE_EXPORTS,
            *self.mod.LINUX_TARGET_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.LINUX_TARGET_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        self.assertEqual(
            self.mod.LINUX_TARGET_CONSTANT_EXPORTS,
            (
                "linux_required_remote_tools",
                "linux_optional_remote_tools",
            ),
        )

    def test_install_linux_target_helpers_routes_each_group_and_unknown_exports(self) -> None:
        calls = []

        def constant_install(bindings, names):
            calls.append(("constant", names))

        def probe_install(bindings, names):
            calls.append(("probe", names))

        def command_install(bindings, names):
            calls.append(("command", names))

        def fallback_install(bindings, globals_obj, names):
            calls.append(("local_fallback", names))

        self.mod.install_linux_target_constant_helpers = constant_install
        self.mod.install_linux_target_probe_helpers = probe_install
        self.mod.install_linux_target_command_helpers = command_install
        self.mod.install_local_helpers = fallback_install

        self.mod.install_linux_target_helpers(
            {},
            (
                "linux_required_remote_tools",
                "probe_linux_launch_backend",
                "remote_linux_bundle_relpath",
                "custom_linux_target_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("constant", ("linux_required_remote_tools",)),
                ("probe", ("probe_linux_launch_backend",)),
                ("command", ("remote_linux_bundle_relpath",)),
                ("local_fallback", ("custom_linux_target_export",)),
            ],
        )



if __name__ == "__main__":
    unittest.main()
