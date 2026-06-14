#!/usr/bin/env python3
"""Tests for Windows target session-agent request bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_target_session_request_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetSessionRequestBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_request_helpers(self) -> None:
        self.assertEqual(
            self.mod.WINDOWS_TARGET_SESSION_REQUEST_EXPORTS,
            ("build_windows_session_agent_request",),
        )

    def test_request_wrapper_delegates_and_binds_desktop_label(self) -> None:
        captured = {}

        def build_request(*args, **kwargs):
            captured["build"] = (args, kwargs)
            return {"job_id": "abc"}

        windows_target = types.SimpleNamespace(build_windows_session_agent_request=build_request)
        bindings = {
            "_windows_target": windows_target,
            "default_desktop_label": object(),
        }

        self.assertEqual(
            self.mod.build_windows_session_agent_request(
                bindings,
                "win",
                {"results_dir": r"C:\Results"},
                "build.bat",
                repo_path=r"C:\Pulp",
                action_name="smoke",
                label=None,
                pulp_app_automation=True,
                capture_ui_snapshot=True,
                click_point="1,2",
                click_view_id="gain",
                click_view_type="Slider",
                click_view_text="Gain",
                click_view_label="Gain slider",
                capture_before=True,
                settle_secs=0.5,
                timeout_secs=30.0,
            ),
            {"job_id": "abc"},
        )
        self.assertEqual(captured["build"][0], ("win", {"results_dir": r"C:\Results"}, "build.bat"))
        self.assertEqual(captured["build"][1]["repo_path"], r"C:\Pulp")
        self.assertIs(captured["build"][1]["default_desktop_label_fn"], bindings["default_desktop_label"])


if __name__ == "__main__":
    unittest.main()
