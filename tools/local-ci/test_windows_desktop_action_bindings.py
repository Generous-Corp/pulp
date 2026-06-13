#!/usr/bin/env python3
"""Tests for Windows desktop action facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_desktop_action_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsDesktopActionBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_action_exports_match_wrappers(self):
        expected = ("run_windows_session_agent_action",)

        self.assertEqual(self.mod.WINDOWS_DESKTOP_ACTION_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_run_windows_session_agent_action_binds_facade_dependencies(self):
        captured = {}

        def action_runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        desktop_actions = types.SimpleNamespace(
            desktop_action_artifact_paths=object(),
            desktop_interaction_requested=object(),
            view_tree_inspector_summary=object(),
            pulp_app_interaction_summary=object(),
        )
        time_mod = types.SimpleNamespace(time=object(), sleep=object())
        bindings = {
            "_desktop_actions": desktop_actions,
            "_windows_desktop_action": types.SimpleNamespace(run_windows_session_agent_action=action_runner),
            "time": time_mod,
        }
        for name in [
            "ensure_host_reachable",
            "desktop_receipt_for",
            "desktop_target_contract",
            "probe_windows_session_agent",
            "windows_desktop_session_user",
            "create_desktop_run_bundle",
            "prepare_windows_exact_sha_source",
            "build_windows_session_agent_request",
            "windows_path_join",
            "windows_ssh_write_text",
            "start_windows_session_agent_task",
            "windows_ssh_read_json",
            "atomic_write_text",
            "windows_ssh_fetch_file",
            "windows_ssh_remove_path",
            "default_desktop_label",
            "image_change_summary",
            "attach_desktop_source_to_manifest",
            "write_desktop_run_rollups",
            "now_iso",
        ]:
            bindings[name] = object()

        result = self.mod.run_windows_session_agent_action(
            bindings,
            {"defaults": {}},
            "windows",
            {"adapter": "windows-session-agent"},
            r".\tool.exe",
            action_name="click",
            label="demo",
            output_path=None,
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_point=None,
            click_view_id="button",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=True,
            settle_secs=0.5,
            timeout_secs=2.0,
            source_request={"mode": "exact-sha"},
        )

        self.assertEqual(result, {"ok": True})
        self.assertEqual(captured["args"], ({"defaults": {}}, "windows", {"adapter": "windows-session-agent"}, r".\tool.exe"))
        self.assertEqual(captured["kwargs"]["action_name"], "click")
        self.assertIs(captured["kwargs"]["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])
        self.assertIs(captured["kwargs"]["probe_windows_session_agent_fn"], bindings["probe_windows_session_agent"])
        self.assertIs(captured["kwargs"]["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(captured["kwargs"]["time_fn"], time_mod.time)
        self.assertIs(captured["kwargs"]["windows_ssh_fetch_file_fn"], bindings["windows_ssh_fetch_file"])
        self.assertIs(captured["kwargs"]["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])

    def test_install_windows_desktop_action_helpers_wires_named_exports(self):
        captured = {}

        def action_runner(*args, **kwargs):
            captured["args"] = args
            return {"installed": True}

        desktop_actions = types.SimpleNamespace(
            desktop_action_artifact_paths=object(),
            desktop_interaction_requested=object(),
            view_tree_inspector_summary=object(),
            pulp_app_interaction_summary=object(),
        )
        bindings = {
            "_desktop_actions": desktop_actions,
            "_windows_desktop_action": types.SimpleNamespace(run_windows_session_agent_action=action_runner),
            "time": types.SimpleNamespace(time=object(), sleep=object()),
        }
        for name in [
            "ensure_host_reachable",
            "desktop_receipt_for",
            "desktop_target_contract",
            "probe_windows_session_agent",
            "windows_desktop_session_user",
            "create_desktop_run_bundle",
            "prepare_windows_exact_sha_source",
            "build_windows_session_agent_request",
            "windows_path_join",
            "windows_ssh_write_text",
            "start_windows_session_agent_task",
            "windows_ssh_read_json",
            "atomic_write_text",
            "windows_ssh_fetch_file",
            "windows_ssh_remove_path",
            "default_desktop_label",
            "image_change_summary",
            "attach_desktop_source_to_manifest",
            "write_desktop_run_rollups",
            "now_iso",
        ]:
            bindings[name] = object()

        self.mod.install_windows_desktop_action_helpers(bindings)

        self.assertEqual(
            bindings["run_windows_session_agent_action"](
                {"defaults": {}},
                "windows",
                {"adapter": "windows-session-agent"},
                r".\tool.exe",
                action_name="smoke",
                label=None,
                output_path=None,
                pulp_app_automation=False,
                capture_ui_snapshot=False,
                click_point=None,
                click_view_id=None,
                click_view_type=None,
                click_view_text=None,
                click_view_label=None,
                capture_before=False,
                settle_secs=0.0,
                timeout_secs=1.0,
            ),
            {"installed": True},
        )
        self.assertEqual(captured["args"], ({"defaults": {}}, "windows", {"adapter": "windows-session-agent"}, r".\tool.exe"))

    def test_install_windows_desktop_action_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_windows_desktop_action_helper = lambda _bindings: "future"

        self.mod.install_windows_desktop_action_helpers(bindings, ("future_windows_desktop_action_helper",))

        self.assertEqual(bindings["future_windows_desktop_action_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
