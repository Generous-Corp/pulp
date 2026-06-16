#!/usr/bin/env python3
"""Tests for desktop video compose/design command facade bindings."""

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_video_compose_command_bindings.py")


class DesktopVideoComposeCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_and_installer(self):
        self.assertEqual(
            self.mod.DESKTOP_VIDEO_COMPOSE_COMMAND_EXPORTS,
            ("cmd_desktop_compose_video", "cmd_desktop_design_diff", "cmd_desktop_design_proof"),
        )
        bindings: dict = {}
        self.mod.install_desktop_video_compose_command_helpers(bindings)
        for name in self.mod.DESKTOP_VIDEO_COMPOSE_COMMAND_EXPORTS:
            self.assertTrue(callable(bindings[name]), name)

    def test_compose_video_threads_dependencies(self):
        captured = {}

        def runner(args, **kwargs):
            captured.update(kwargs)
            captured["args"] = args
            return 0

        bindings = {
            "_desktop_video_compose_commands_cli": types.SimpleNamespace(cmd_desktop_compose_video=runner),
            "compose_desktop_video_proof": object(),
            "create_issue_video_variant": object(),
            "atomic_write_text": object(),
        }
        result = self.mod.cmd_desktop_compose_video(bindings, "ARGS")
        self.assertEqual(result, 0)
        self.assertEqual(captured["args"], "ARGS")
        self.assertIs(captured["compose_desktop_video_proof_fn"], bindings["compose_desktop_video_proof"])
        self.assertIs(captured["create_issue_video_variant_fn"], bindings["create_issue_video_variant"])

    def test_design_diff_threads_parity_summary(self):
        captured = {}

        def runner(args, **kwargs):
            captured.update(kwargs)
            return 0

        parity = types.SimpleNamespace(design_parity_diff_summary=lambda *a, **k: {"ok": True})
        bindings = {
            "_desktop_video_compose_commands_cli": types.SimpleNamespace(cmd_desktop_design_diff=runner),
            "_io_utils_design_parity": parity,
            "atomic_write_text": object(),
        }
        self.mod.cmd_desktop_design_diff(bindings, "ARGS")
        self.assertEqual(captured["design_parity_diff_summary_fn"]("x"), {"ok": True})


if __name__ == "__main__":
    unittest.main()
