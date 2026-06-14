#!/usr/bin/env python3
"""Tests for Linux window-driver command dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("linux_target_window_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetWindowCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_window_command_exports_match_facade_helpers(self) -> None:
        expected = ("build_linux_window_driver_remote_command",)

        self.assertEqual(self.mod.LINUX_TARGET_WINDOW_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_window_remote_command_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def build_linux_window_driver_remote_command(*args, **kwargs):
            captured["window"] = (args, kwargs)
            return "window-cmd"

        bindings = {
            "_linux_target": types.SimpleNamespace(
                build_linux_window_driver_remote_command=build_linux_window_driver_remote_command,
            ),
        }
        deps = {"parse_coordinate_pair_fn": object()}

        with mock.patch.object(self.mod, "linux_target_window_command_dependencies", return_value=deps):
            self.assertEqual(
                self.mod.build_linux_window_driver_remote_command(
                    bindings,
                    "/repo",
                    ".local/run",
                    "./app",
                    launch_backend={"mode": "display", "display": ":99"},
                    launch_cwd="/repo/build",
                    click_point="1,2",
                    capture_before=True,
                    settle_secs=0.25,
                ),
                "window-cmd",
            )
        self.assertEqual(captured["window"][0], ("/repo", ".local/run", "./app"))
        self.assertEqual(captured["window"][1]["launch_backend"], {"mode": "display", "display": ":99"})
        self.assertEqual(captured["window"][1]["parse_coordinate_pair_fn"], deps["parse_coordinate_pair_fn"])

    def test_install_linux_target_window_command_helpers_wires_named_exports(self) -> None:
        linux_target = types.SimpleNamespace(
            build_linux_window_driver_remote_command=lambda *args, **kwargs: "window-cmd",
        )
        bindings = {
            "_linux_target": linux_target,
            "parse_coordinate_pair": object(),
        }

        self.mod.install_linux_target_window_command_helpers(
            bindings,
            ("build_linux_window_driver_remote_command",),
        )

        self.assertEqual(
            bindings["build_linux_window_driver_remote_command"](
                "/repo",
                ".local/run",
                "./app",
                click_point=None,
                capture_before=False,
                settle_secs=0.0,
            ),
            "window-cmd",
        )


if __name__ == "__main__":
    unittest.main()
