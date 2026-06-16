#!/usr/bin/env python3
"""Tests for desktop video setup/doctor command facade bindings."""

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_video_setup_command_bindings.py")


class DesktopVideoSetupCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_attr):
        names = ["load_config", "resolve_desktop_target", "desktop_doctor_checks",
                 "normalize_desktop_optional_config", "video_proof_smoke",
                 "probe_macos_avfoundation_audio", "save_config"]
        captured = {}

        def runner(args, **kwargs):
            captured.update(kwargs)
            return 0

        b = {n: object() for n in names}
        b["_desktop_video_setup_commands_cli"] = types.SimpleNamespace(**{runner_attr: runner})
        b["_desktop_video_matrix_commands_cli"] = types.SimpleNamespace(desktop_video_matrix_payload=object())
        return b, captured

    def test_exports_and_installer(self):
        self.assertEqual(
            self.mod.DESKTOP_VIDEO_SETUP_COMMAND_EXPORTS,
            ("cmd_desktop_video_doctor", "cmd_desktop_video_setup"),
        )
        bindings: dict = {}
        self.mod.install_desktop_video_setup_command_helpers(bindings)
        for name in self.mod.DESKTOP_VIDEO_SETUP_COMMAND_EXPORTS:
            self.assertTrue(callable(bindings[name]), name)

    def test_doctor_threads_deps(self):
        b, captured = self._bindings("cmd_desktop_video_doctor")
        self.mod.cmd_desktop_video_doctor(b, "ARGS")
        self.assertIs(captured["load_config_fn"], b["load_config"])
        self.assertIs(captured["desktop_doctor_checks_fn"], b["desktop_doctor_checks"])
        self.assertIs(captured["video_proof_smoke_fn"], b["video_proof_smoke"])

    def test_setup_threads_matrix_and_save_config(self):
        b, captured = self._bindings("cmd_desktop_video_setup")
        self.mod.cmd_desktop_video_setup(b, "ARGS")
        self.assertIs(captured["save_config_fn"], b["save_config"])
        self.assertIs(
            captured["desktop_video_matrix_payload_fn"],
            b["_desktop_video_matrix_commands_cli"].desktop_video_matrix_payload,
        )


if __name__ == "__main__":
    unittest.main()
