#!/usr/bin/env python3
"""Tests for macOS desktop smoke/action facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("macos_desktop_smoke_bindings.py")


class MacosDesktopSmokeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_smoke_exports_match_wrappers(self):
        expected = ("run_macos_local_smoke",)

        self.assertEqual(self.mod.MACOS_DESKTOP_SMOKE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_run_macos_local_smoke_binds_facade_dependencies(self):
        captured = {}

        def action_runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        desktop_actions = types.SimpleNamespace(
            desktop_action_artifact_paths=object(),
            desktop_interaction_requested=object(),
            content_size_from_window=object(),
            content_size_from_view_tree=object(),
            view_tree_inspector_summary=object(),
            pulp_app_interaction_summary=object(),
            desktop_click_selector=object(),
        )
        bindings = {
            "_desktop_actions": desktop_actions,
            "_macos_desktop_action": types.SimpleNamespace(run_macos_local_smoke=action_runner),
            "subprocess": types.SimpleNamespace(run=object(), Popen=object()),
            "time": types.SimpleNamespace(sleep=object()),
            "shlex": types.SimpleNamespace(split=object()),
            "os": types.SimpleNamespace(environ=types.SimpleNamespace(copy=object())),
        }
        for name in [
            "create_desktop_run_bundle",
            "macos_accessibility_trusted",
            "now_iso",
            "prepare_macos_exact_sha_source",
            "quit_macos_bundle_id",
            "activate_macos_bundle_id",
            "wait_for_macos_bundle_window",
            "detect_macos_app_bundle",
            "macos_bundle_id_for_app_path",
            "wait_for_macos_window",
            "wait_for_path",
            "capture_macos_window",
            "parse_coordinate_pair",
            "resolve_view_tree_click_point",
            "screen_point_for_content_point",
            "activate_macos_pid",
            "dispatch_macos_click",
            "image_change_summary",
            "attach_desktop_source_to_manifest",
            "atomic_write_text",
            "write_desktop_run_rollups",
            "terminate_process",
        ]:
            bindings[name] = object()

        result = self.mod.run_macos_local_smoke(
            bindings,
            {"desktop_automation": {}},
            "/tmp/app",
            action_name="click",
            bundle_id=None,
            label="demo",
            output_path=None,
            capture_ui_snapshot=True,
            click_point="1,2",
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            pulp_app_automation=False,
            capture_before=True,
            settle_secs=0.25,
            timeout_secs=1.0,
            source_request={"mode": "current"},
        )

        self.assertEqual(result, {"ok": True})
        self.assertEqual(captured["args"], ({"desktop_automation": {}}, "/tmp/app"))
        self.assertEqual(captured["kwargs"]["action_name"], "click")
        self.assertIs(captured["kwargs"]["create_desktop_run_bundle_fn"], bindings["create_desktop_run_bundle"])
        self.assertIs(captured["kwargs"]["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(captured["kwargs"]["wait_for_macos_window_fn"], bindings["wait_for_macos_window"])
        self.assertIs(captured["kwargs"]["capture_macos_window_fn"], bindings["capture_macos_window"])
        self.assertIs(captured["kwargs"]["terminate_process_fn"], bindings["terminate_process"])


if __name__ == "__main__":
    unittest.main()
