#!/usr/bin/env python3
"""Tests for Linux Xvfb remote command dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("linux_target_xvfb_command_bindings.py")


class LinuxTargetXvfbCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_xvfb_command_exports_match_facade_helpers(self) -> None:
        expected = ("build_linux_xvfb_remote_command",)

        self.assertEqual(self.mod.LINUX_TARGET_XVFB_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_xvfb_remote_command_delegates_to_linux_target_module(self) -> None:
        captured = {}

        def build_linux_xvfb_remote_command(*args, **kwargs):
            captured["xvfb"] = (args, kwargs)
            return "xvfb-cmd"

        bindings = {
            "_linux_target": types.SimpleNamespace(build_linux_xvfb_remote_command=build_linux_xvfb_remote_command),
        }

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
        self.assertEqual(captured["xvfb"][1]["launch_backend"], {"mode": "xvfb"})
        self.assertEqual(captured["xvfb"][1]["click_view_label"], "Gain slider")
        self.assertTrue(captured["xvfb"][1]["capture_ui_snapshot"])

    def test_install_linux_target_xvfb_command_helpers_wires_named_exports(self) -> None:
        linux_target = types.SimpleNamespace(
            build_linux_xvfb_remote_command=lambda *args, **kwargs: "xvfb-cmd",
        )
        bindings = {"_linux_target": linux_target}

        self.mod.install_linux_target_xvfb_command_helpers(bindings, ("build_linux_xvfb_remote_command",))

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


if __name__ == "__main__":
    unittest.main()
