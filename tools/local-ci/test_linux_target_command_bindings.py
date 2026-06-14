#!/usr/bin/env python3
"""Tests for Linux target command facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("linux_target_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.LINUX_TARGET_BUNDLE_EXPORTS,
            *self.mod.LINUX_TARGET_XVFB_COMMAND_EXPORTS,
            *self.mod.LINUX_TARGET_WINDOW_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.LINUX_TARGET_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_linux_target_command_helpers_routes_each_group(self) -> None:
        linux_target = types.SimpleNamespace(
            remote_linux_bundle_relpath=lambda target_name, action_name, bundle_dir: f"{target_name}/{action_name}/{bundle_dir.name}",
            build_linux_xvfb_remote_command=lambda *args, **kwargs: "xvfb-cmd",
            build_linux_window_driver_remote_command=lambda *args, **kwargs: "window-cmd",
        )
        bindings = {
            "_linux_target": linux_target,
            "parse_coordinate_pair": object(),
        }

        self.mod.install_linux_target_command_helpers(
            bindings,
            (
                "remote_linux_bundle_relpath",
                "build_linux_xvfb_remote_command",
                "build_linux_window_driver_remote_command",
            ),
        )

        self.assertEqual(
            bindings["remote_linux_bundle_relpath"]("ubuntu", "smoke", Path("/tmp/run")),
            "ubuntu/smoke/run",
        )
        self.assertEqual(
            bindings["build_linux_xvfb_remote_command"](
                "/repo",
                ".local/run",
                "./app",
                capture_ui_snapshot=False,
                click_point=None,
                click_view_id=None,
                click_view_type=None,
                click_view_text=None,
                click_view_label=None,
                capture_before=False,
                settle_secs=0.0,
            ),
            "xvfb-cmd",
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

    def test_path_and_command_builders_delegate_with_parse_coordinate_seam(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        linux_target = types.SimpleNamespace(
            remote_linux_bundle_relpath=capture("relpath", ".local/state/pulp/desktop-automation/remote/ubuntu/smoke/run"),
            build_linux_xvfb_remote_command=capture("xvfb", "xvfb-cmd"),
            build_linux_window_driver_remote_command=capture("window", "window-cmd"),
        )
        bindings = {
            "_linux_target": linux_target,
            "parse_coordinate_pair": object(),
        }
        bundle_dir = Path("/tmp/run")

        self.assertEqual(
            self.mod.remote_linux_bundle_relpath(bindings, "ubuntu", "smoke", bundle_dir),
            ".local/state/pulp/desktop-automation/remote/ubuntu/smoke/run",
        )
        self.assertEqual(captured["relpath"][0], ("ubuntu", "smoke", bundle_dir))

        self.assertEqual(
            self.mod.build_linux_xvfb_remote_command(
                bindings,
                "/repo",
                ".local/run",
                "./app",
                launch_backend={"mode": "xvfb"},
                launch_cwd="/repo/build",
                capture_ui_snapshot=True,
                click_point="1,2",
                click_view_id="gain",
                click_view_type="Slider",
                click_view_text="Gain",
                click_view_label="Gain slider",
                capture_before=True,
                settle_secs=0.25,
            ),
            "xvfb-cmd",
        )
        self.assertEqual(captured["xvfb"][0], ("/repo", ".local/run", "./app"))
        self.assertEqual(captured["xvfb"][1]["click_view_label"], "Gain slider")

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
        self.assertIs(captured["window"][1]["parse_coordinate_pair_fn"], bindings["parse_coordinate_pair"])



if __name__ == "__main__":
    unittest.main()
