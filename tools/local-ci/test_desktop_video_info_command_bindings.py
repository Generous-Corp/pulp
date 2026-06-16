#!/usr/bin/env python3
"""Tests for desktop video info command facade bindings."""

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_video_info_command_bindings.py")


class DesktopVideoInfoCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_and_installer(self):
        self.assertEqual(self.mod.DESKTOP_VIDEO_INFO_COMMAND_EXPORTS, ("cmd_desktop_video_matrix",))
        bindings: dict = {}
        self.mod.install_desktop_video_info_command_helpers(bindings)
        self.assertTrue(callable(bindings["cmd_desktop_video_matrix"]))

    def test_video_matrix_threads_load_config(self):
        captured = {}

        def runner(args, **kwargs):
            captured.update(kwargs)
            return 0

        bindings = {
            "_desktop_video_matrix_commands_cli": types.SimpleNamespace(cmd_desktop_video_matrix=runner),
            "load_config": object(),
        }
        self.mod.cmd_desktop_video_matrix(bindings, "ARGS")
        self.assertIs(captured["load_config_fn"], bindings["load_config"])


if __name__ == "__main__":
    unittest.main()
