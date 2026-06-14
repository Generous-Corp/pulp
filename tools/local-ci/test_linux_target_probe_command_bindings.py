#!/usr/bin/env python3
"""Tests for Linux target probe execution facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("linux_target_probe_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetProbeCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_probe_command_helpers(self) -> None:
        expected = (
            "probe_linux_launch_backend",
            "probe_linux_remote_tooling",
        )

        self.assertEqual(self.mod.LINUX_TARGET_PROBE_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_probe_commands_delegate_with_assembled_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        linux_target = types.SimpleNamespace(
            probe_linux_launch_backend=capture("launch", {"mode": "xvfb"}),
            probe_linux_remote_tooling=capture("tooling", {"git_found": True}),
        )
        bindings = {
            "_linux_target": linux_target,
        }
        deps = {"ssh_command_result_fn": object()}

        with mock.patch.object(self.mod, "linux_target_probe_command_dependencies", return_value=deps):
            self.assertEqual(
                self.mod.probe_linux_launch_backend(bindings, "ubuntu"),
                {"mode": "xvfb"},
            )
            self.assertEqual(
                self.mod.probe_linux_remote_tooling(bindings, "ubuntu"),
                {"git_found": True},
            )
        self.assertEqual(captured["launch"][1], deps)
        self.assertEqual(captured["tooling"][1], deps)

    def test_install_linux_target_probe_command_helpers_wires_named_exports(self) -> None:
        linux_target = types.SimpleNamespace(
            probe_linux_launch_backend=lambda host, *, ssh_command_result_fn: {"host": host},
        )
        bindings = {
            "_linux_target": linux_target,
            "ssh_command_result": object(),
        }

        self.mod.install_linux_target_probe_command_helpers(bindings, ("probe_linux_launch_backend",))

        self.assertEqual(bindings["probe_linux_launch_backend"]("ubuntu"), {"host": "ubuntu"})

    def test_install_linux_target_probe_command_helpers_keeps_unknown_fallback(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_linux_target_probe_command_helpers(bindings, ("custom_probe_export",))

        install_local.assert_any_call(bindings, self.mod.__dict__, ())
        install_local.assert_any_call(bindings, self.mod.__dict__, ("custom_probe_export",))


if __name__ == "__main__":
    unittest.main()
