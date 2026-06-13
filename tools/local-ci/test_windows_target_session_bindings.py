#!/usr/bin/env python3
"""Tests for Windows target session facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_target_session_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetSessionBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_session_helpers(self) -> None:
        expected = (
            "default_windows_session_task_name",
            "desktop_target_contract",
            "build_windows_session_agent_request",
        )

        self.assertEqual(self.mod.WINDOWS_TARGET_SESSION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_session_wrappers_delegate_and_bind_desktop_label(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        windows_target = types.SimpleNamespace(
            default_windows_session_task_name=capture("task", "Task-win"),
            desktop_target_contract=capture("contract", {"kind": "windows-session-agent"}),
            build_windows_session_agent_request=capture("build", {"job_id": "abc"}),
        )
        bindings = {
            "_windows_target": windows_target,
            "default_desktop_label": object(),
        }

        self.assertEqual(self.mod.default_windows_session_task_name(bindings, "win"), "Task-win")
        self.assertEqual(captured["task"][0], ("win",))
        self.assertEqual(self.mod.desktop_target_contract(bindings, "win", {"adapter": "windows-session-agent"}), {"kind": "windows-session-agent"})
        self.assertEqual(captured["contract"][0], ("win", {"adapter": "windows-session-agent"}))

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

    def test_install_windows_target_session_helpers_wires_named_exports(self) -> None:
        windows_target = types.SimpleNamespace(
            default_windows_session_task_name=lambda target_name: f"Task-{target_name}",
            desktop_target_contract=lambda target_name, target: {"target": target_name, **target},
        )
        bindings = {"_windows_target": windows_target}

        self.mod.install_windows_target_session_helpers(
            bindings,
            ("default_windows_session_task_name", "desktop_target_contract"),
        )

        self.assertEqual(bindings["default_windows_session_task_name"]("win"), "Task-win")
        self.assertEqual(
            bindings["desktop_target_contract"]("win", {"adapter": "windows-session-agent"}),
            {"target": "win", "adapter": "windows-session-agent"},
        )

    def test_install_windows_target_session_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_windows_session_helper = lambda _bindings: "future"

        self.mod.install_windows_target_session_helpers(bindings, ("future_windows_session_helper",))

        self.assertEqual(bindings["future_windows_session_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
