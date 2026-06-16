#!/usr/bin/env python3
"""Tests for desktop review command facade bindings."""

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_review_command_bindings.py")


class DesktopReviewCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_and_installer(self):
        self.assertEqual(
            self.mod.DESKTOP_REVIEW_COMMAND_EXPORTS,
            ("cmd_desktop_verdict", "cmd_desktop_review_issue", "cmd_desktop_review_status", "cmd_desktop_review_watch"),
        )
        bindings: dict = {}
        self.mod.install_desktop_review_command_helpers(bindings)
        for name in self.mod.DESKTOP_REVIEW_COMMAND_EXPORTS:
            self.assertTrue(callable(bindings[name]), name)

    def test_verdict_binding_threads_dependencies(self):
        captured = {}

        def runner(args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 0

        bindings = {
            "_desktop_review_commands_cli": types.SimpleNamespace(cmd_desktop_verdict=runner),
            "now_iso": object(),
            "atomic_write_text": object(),
            "subprocess": types.SimpleNamespace(run=object()),
        }
        result = self.mod.cmd_desktop_verdict(bindings, "ARGS")
        self.assertEqual(result, 0)
        self.assertEqual(captured["args"], "ARGS")
        self.assertIs(captured["kwargs"]["now_iso_fn"], bindings["now_iso"])
        self.assertIs(captured["kwargs"]["atomic_write_text_fn"], bindings["atomic_write_text"])
        self.assertIs(captured["kwargs"]["run_fn"], bindings["subprocess"].run)


if __name__ == "__main__":
    unittest.main()
