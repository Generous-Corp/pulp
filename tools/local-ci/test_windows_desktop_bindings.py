#!/usr/bin/env python3
"""Tests for Windows desktop action facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_desktop_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("windows_desktop_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class WindowsDesktopBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

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
        self.assertEqual(
            captured["args"],
            ({"defaults": {}}, "windows", {"adapter": "windows-session-agent"}, r".\tool.exe"),
        )
        self.assertEqual(captured["kwargs"]["action_name"], "click")
        self.assertIs(captured["kwargs"]["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])
        self.assertIs(captured["kwargs"]["desktop_receipt_for_fn"], bindings["desktop_receipt_for"])
        self.assertIs(captured["kwargs"]["desktop_target_contract_fn"], bindings["desktop_target_contract"])
        self.assertIs(captured["kwargs"]["probe_windows_session_agent_fn"], bindings["probe_windows_session_agent"])
        self.assertIs(captured["kwargs"]["windows_desktop_session_user_fn"], bindings["windows_desktop_session_user"])
        self.assertIs(captured["kwargs"]["create_desktop_run_bundle_fn"], bindings["create_desktop_run_bundle"])
        self.assertIs(captured["kwargs"]["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(captured["kwargs"]["desktop_interaction_requested_fn"], desktop_actions.desktop_interaction_requested)
        self.assertIs(captured["kwargs"]["prepare_windows_exact_sha_source_fn"], bindings["prepare_windows_exact_sha_source"])
        self.assertIs(captured["kwargs"]["build_windows_session_agent_request_fn"], bindings["build_windows_session_agent_request"])
        self.assertIs(captured["kwargs"]["windows_path_join_fn"], bindings["windows_path_join"])
        self.assertIs(captured["kwargs"]["windows_ssh_write_text_fn"], bindings["windows_ssh_write_text"])
        self.assertIs(captured["kwargs"]["start_windows_session_agent_task_fn"], bindings["start_windows_session_agent_task"])
        self.assertIs(captured["kwargs"]["time_fn"], time_mod.time)
        self.assertIs(captured["kwargs"]["sleep_fn"], time_mod.sleep)
        self.assertIs(captured["kwargs"]["windows_ssh_read_json_fn"], bindings["windows_ssh_read_json"])
        self.assertIs(captured["kwargs"]["windows_ssh_fetch_file_fn"], bindings["windows_ssh_fetch_file"])
        self.assertIs(captured["kwargs"]["windows_ssh_remove_path_fn"], bindings["windows_ssh_remove_path"])
        self.assertIs(captured["kwargs"]["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])
        self.assertIs(captured["kwargs"]["view_tree_inspector_summary_fn"], desktop_actions.view_tree_inspector_summary)


if __name__ == "__main__":
    unittest.main()
