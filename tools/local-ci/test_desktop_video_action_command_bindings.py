#!/usr/bin/env python3
"""Tests for desktop video action command facade bindings."""

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_video_action_command_bindings.py")


class DesktopVideoActionCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_and_installer(self):
        self.assertEqual(self.mod.DESKTOP_VIDEO_ACTION_COMMAND_EXPORTS, ("cmd_desktop_video",))
        bindings: dict = {}
        self.mod.install_desktop_video_action_command_helpers(bindings)
        self.assertTrue(callable(bindings["cmd_desktop_video"]))

    def test_video_dispatches_to_action_without_terminal(self):
        seen = {}

        def video_runner(args, **kwargs):
            seen["kwargs"] = kwargs
            return 7

        bindings = {
            "_desktop_video_action_commands_cli": types.SimpleNamespace(cmd_desktop_video=video_runner),
            "cmd_desktop_smoke": lambda a: 1,
            "cmd_desktop_click": lambda a: 2,
            "cmd_desktop_inspect": lambda a: 3,
        }
        # args without run_in_terminal -> no terminal re-entry
        from argparse import Namespace
        rc = self.mod.cmd_desktop_video(bindings, Namespace(run_in_terminal=False))
        self.assertEqual(rc, 7)
        self.assertIn("cmd_desktop_smoke_fn", seen["kwargs"])
        self.assertIn("cmd_desktop_click_fn", seen["kwargs"])

    def test_terminal_stdout_passthrough_without_cleanup(self):
        self.assertEqual(self.mod._terminal_stdout({"stdout": "plain"}), "plain")


if __name__ == "__main__":
    unittest.main()
