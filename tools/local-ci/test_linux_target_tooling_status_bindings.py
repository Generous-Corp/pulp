#!/usr/bin/env python3
"""Tests for Linux target tooling status facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("linux_target_tooling_status_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetToolingStatusBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_tooling_status_helpers(self) -> None:
        expected = (
            "linux_tooling_detail",
            "linux_remote_tooling_ready",
        )

        self.assertEqual(self.mod.LINUX_TARGET_TOOLING_STATUS_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_tooling_status_helpers_bind_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        linux_target = types.SimpleNamespace(
            linux_tooling_detail=capture("detail", "git version"),
            linux_remote_tooling_ready=capture("ready", True),
        )
        bindings = {
            "_linux_target": linux_target,
            "LINUX_REQUIRED_REMOTE_TOOLS": {"git": {"required": True}},
        }
        probe = {"git_found": True}

        self.assertEqual(
            self.mod.linux_tooling_detail(bindings, probe, "git", missing_hint="install git"),
            "git version",
        )
        self.assertEqual(captured["detail"][1], {"missing_hint": "install git"})
        self.assertTrue(self.mod.linux_remote_tooling_ready(bindings, probe))
        self.assertIs(captured["ready"][1]["required_tools"], bindings["LINUX_REQUIRED_REMOTE_TOOLS"])

    def test_install_linux_target_tooling_status_helpers_wires_named_exports(self) -> None:
        linux_target = types.SimpleNamespace(
            linux_tooling_detail=lambda probe, tool_name, *, missing_hint=None: missing_hint,
        )
        bindings = {"_linux_target": linux_target}

        self.mod.install_linux_target_tooling_status_helpers(bindings, ("linux_tooling_detail",))

        self.assertEqual(
            bindings["linux_tooling_detail"]({"git_found": False}, "git", missing_hint="install git"),
            "install git",
        )

    def test_install_linux_target_tooling_status_helpers_keeps_unknown_fallback(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_linux_target_tooling_status_helpers(bindings, ("custom_status_export",))

        install_local.assert_any_call(bindings, self.mod.__dict__, ())
        install_local.assert_any_call(bindings, self.mod.__dict__, ("custom_status_export",))


if __name__ == "__main__":
    unittest.main()
