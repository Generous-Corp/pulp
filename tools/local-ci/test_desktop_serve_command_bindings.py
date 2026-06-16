#!/usr/bin/env python3
"""Tests for desktop serve command facade bindings."""

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_serve_command_bindings.py")


class DesktopServeCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_and_installer(self):
        self.assertEqual(self.mod.DESKTOP_SERVE_COMMAND_EXPORTS, ("cmd_desktop_serve",))
        bindings: dict = {}
        self.mod.install_desktop_serve_command_helpers(bindings)
        self.assertTrue(callable(bindings["cmd_desktop_serve"]))

    def test_serve_threads_dependencies(self):
        captured = {}

        def runner(args, **kwargs):
            captured.update(kwargs)
            return 0

        bindings = {
            "_desktop_serve_commands_cli": types.SimpleNamespace(cmd_desktop_serve=runner),
            "load_config": object(),
            "desktop_publish_reports": object(),
        }
        self.mod.cmd_desktop_serve(bindings, "ARGS")
        self.assertIs(captured["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["desktop_publish_reports_fn"], bindings["desktop_publish_reports"])


if __name__ == "__main__":
    unittest.main()
