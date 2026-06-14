#!/usr/bin/env python3
"""Tests for Linux target tool constant facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("linux_target_constant_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetConstantBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_tool_constant_helpers(self) -> None:
        expected = (
            "linux_required_remote_tools",
            "linux_optional_remote_tools",
        )

        self.assertEqual(self.mod.LINUX_TARGET_CONSTANT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_constant_helpers_bind_linux_target_tool_maps(self) -> None:
        linux_target = types.SimpleNamespace(
            LINUX_REQUIRED_REMOTE_TOOLS={"git": {"required": True}},
            LINUX_OPTIONAL_REMOTE_TOOLS={"xvfb": {"required": False}},
        )
        bindings = {"_linux_target": linux_target}

        self.assertIs(
            self.mod.linux_required_remote_tools(bindings),
            linux_target.LINUX_REQUIRED_REMOTE_TOOLS,
        )
        self.assertIs(
            self.mod.linux_optional_remote_tools(bindings),
            linux_target.LINUX_OPTIONAL_REMOTE_TOOLS,
        )

    def test_install_linux_target_constant_helpers_wires_named_exports(self) -> None:
        linux_target = types.SimpleNamespace(
            LINUX_REQUIRED_REMOTE_TOOLS={"git": {"required": True}},
            LINUX_OPTIONAL_REMOTE_TOOLS={"xvfb": {"required": False}},
        )
        bindings = {"_linux_target": linux_target}

        self.mod.install_linux_target_constant_helpers(bindings, ("linux_required_remote_tools",))

        self.assertIs(
            bindings["linux_required_remote_tools"](),
            linux_target.LINUX_REQUIRED_REMOTE_TOOLS,
        )

    def test_install_linux_target_constant_helpers_keeps_unknown_fallback(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_linux_target_constant_helpers(bindings, ("custom_constant_export",))

        install_local.assert_any_call(bindings, self.mod.__dict__, ())
        install_local.assert_any_call(bindings, self.mod.__dict__, ("custom_constant_export",))


if __name__ == "__main__":
    unittest.main()
