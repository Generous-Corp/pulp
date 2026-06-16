#!/usr/bin/env python3
"""Tests for video-proof setup/doctor prerequisite helpers."""

import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_video_prerequisites.py", add_module_dir=True)


class DesktopVideoPrerequisitesTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_prerequisite_checks_flag_missing_tools(self):
        checks = self.mod.desktop_video_setup_prerequisite_checks(
            which_fn=lambda name: "/usr/bin/" + name if name in {"node", "npm"} else None
        )
        by_name = {c["name"]: c for c in checks}
        self.assertTrue(by_name)
        # node/npm present -> ok; a missing tool -> not ok
        self.assertTrue(any(c["ok"] for c in checks))
        self.assertTrue(any(not c["ok"] for c in checks))

    def test_install_model_describes_tool_addon(self):
        model = self.mod.desktop_video_install_model(pulp_command="pulp")
        self.assertIsInstance(model, dict)
        self.assertTrue(model)

    def test_recorder_backend_check_returns_status(self):
        result = self.mod.desktop_video_recorder_backend_check({"adapter": "macos-local"})
        self.assertIn("ok", result)
        self.assertIn("name", result)

    def test_doctor_remediations_for_failed_checks(self):
        remediations = self.mod.desktop_video_doctor_remediations(
            [{"name": "screen_recording", "ok": False, "detail": "not granted"}],
            target_name="mac",
        )
        self.assertIsInstance(remediations, list)


if __name__ == "__main__":
    unittest.main()
