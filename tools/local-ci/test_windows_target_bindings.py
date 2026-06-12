#!/usr/bin/env python3
"""Tests for Windows target facade bindings."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("windows_target_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("windows_target_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class WindowsTargetBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, windows_target):
        return {
            "_windows_target": windows_target,
            "default_desktop_label": object(),
            "WINDOWS_REQUIRED_REMOTE_TOOLS": {"git": {"required": True}},
        }

    def test_simple_wrappers_delegate_to_windows_target_module(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        windows_target = types.SimpleNamespace(
            WINDOWS_REQUIRED_REMOTE_TOOLS={"git": {"required": True}},
            WINDOWS_OPTIONAL_REMOTE_TOOLS={"gh": {"required": False}},
            WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME="pulp-validate",
            default_windows_session_task_name=capture("task", "Task-win"),
            desktop_target_contract=capture("contract", {"kind": "windows-session-agent"}),
            windows_path_join=capture("join", r"C:\Pulp"),
            windows_default_repo_checkout_path=capture("default_path", r"C:\Users\dev\pulp-validate"),
            windows_repo_path_is_unsafe=capture("unsafe", False),
            update_target_repo_path=capture("update", None),
            windows_repo_checkout_ready=capture("ready", True),
            windows_tooling_detail=capture("tool_detail", "git version"),
            windows_desktop_session_user=capture("user", "dev"),
            windows_desktop_session_state=capture("state", "Active"),
            windows_repo_checkout_detail=capture("checkout_detail", r"C:\Pulp"),
        )
        bindings = self._bindings(windows_target)
        config = {}
        probe = {"git_found": True}

        self.assertIs(self.mod.windows_required_remote_tools(bindings), windows_target.WINDOWS_REQUIRED_REMOTE_TOOLS)
        self.assertIs(self.mod.windows_optional_remote_tools(bindings), windows_target.WINDOWS_OPTIONAL_REMOTE_TOOLS)
        self.assertEqual(self.mod.windows_default_remote_repo_dirname(bindings), "pulp-validate")
        self.assertEqual(self.mod.default_windows_session_task_name(bindings, "win"), "Task-win")
        self.assertEqual(captured["task"][0], ("win",))
        self.assertEqual(self.mod.desktop_target_contract(bindings, "win", {"adapter": "windows-session-agent"}), {"kind": "windows-session-agent"})
        self.assertEqual(captured["contract"][0], ("win", {"adapter": "windows-session-agent"}))
        self.assertEqual(self.mod.windows_path_join(bindings, r"C:\Users", "dev"), r"C:\Pulp")
        self.assertEqual(captured["join"][0], (r"C:\Users", "dev"))
        self.assertEqual(self.mod.windows_default_repo_checkout_path(bindings, r"C:\Users\dev"), r"C:\Users\dev\pulp-validate")
        self.assertEqual(captured["default_path"][0], (r"C:\Users\dev",))
        self.assertFalse(self.mod.windows_repo_path_is_unsafe(bindings, r"C:\Users\dev\pulp", r"C:\Users\dev"))
        self.assertEqual(captured["unsafe"][0], (r"C:\Users\dev\pulp", r"C:\Users\dev"))
        self.mod.update_target_repo_path(bindings, config, "win", r"C:\Pulp")
        self.assertEqual(captured["update"][0], (config, "win", r"C:\Pulp"))
        self.assertTrue(self.mod.windows_repo_checkout_ready(bindings, probe))
        self.assertEqual(captured["ready"][0], (probe,))
        self.assertEqual(self.mod.windows_tooling_detail(bindings, probe, "git", missing_hint="install git"), "git version")
        self.assertEqual(captured["tool_detail"][0], (probe, "git"))
        self.assertEqual(captured["tool_detail"][1], {"missing_hint": "install git"})
        self.assertEqual(self.mod.windows_desktop_session_user(bindings, probe), "dev")
        self.assertEqual(captured["user"][0], (probe,))
        self.assertEqual(self.mod.windows_desktop_session_state(bindings, probe), "Active")
        self.assertEqual(captured["state"][0], (probe,))
        self.assertEqual(self.mod.windows_repo_checkout_detail(bindings, probe, fallback_path=r"C:\Fallback"), r"C:\Pulp")
        self.assertEqual(captured["checkout_detail"][0], (probe,))
        self.assertEqual(captured["checkout_detail"][1], {"fallback_path": r"C:\Fallback"})

    def test_build_request_and_tooling_ready_bind_facade_dependencies(self) -> None:
        captured = {}

        def build_request(*args, **kwargs):
            captured["build"] = (args, kwargs)
            return {"job_id": "abc"}

        def tooling_ready(*args, **kwargs):
            captured["ready"] = (args, kwargs)
            return True

        windows_target = types.SimpleNamespace(
            build_windows_session_agent_request=build_request,
            windows_remote_tooling_ready=tooling_ready,
        )
        bindings = self._bindings(windows_target)
        result = self.mod.build_windows_session_agent_request(
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
        )

        self.assertEqual(result, {"job_id": "abc"})
        self.assertEqual(captured["build"][0], ("win", {"results_dir": r"C:\Results"}, "build.bat"))
        build_kwargs = captured["build"][1]
        self.assertEqual(build_kwargs["repo_path"], r"C:\Pulp")
        self.assertEqual(build_kwargs["action_name"], "smoke")
        self.assertIs(build_kwargs["default_desktop_label_fn"], bindings["default_desktop_label"])

        self.assertTrue(self.mod.windows_remote_tooling_ready(bindings, {"git_found": True}))
        self.assertEqual(captured["ready"][0], ({"git_found": True},))
        self.assertIs(captured["ready"][1]["required_tools"], bindings["WINDOWS_REQUIRED_REMOTE_TOOLS"])


if __name__ == "__main__":
    unittest.main()
