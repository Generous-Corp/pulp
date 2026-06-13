#!/usr/bin/env python3
"""Tests for macOS desktop facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("macos_desktop_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("macos_desktop_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class MacosDesktopBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

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
        subprocess_mod = types.SimpleNamespace(run=object(), Popen=object())
        time_mod = types.SimpleNamespace(sleep=object())
        shlex_mod = types.SimpleNamespace(split=object())
        os_mod = types.SimpleNamespace(environ=types.SimpleNamespace(copy=object()))
        bindings = {
            "_desktop_actions": desktop_actions,
            "_macos_desktop_action": types.SimpleNamespace(run_macos_local_smoke=action_runner),
            "subprocess": subprocess_mod,
            "time": time_mod,
            "shlex": shlex_mod,
            "os": os_mod,
            "Path": Path,
        }
        for name in [
            "create_desktop_run_bundle",
            "macos_accessibility_trusted",
            "now_iso",
            "prepare_macos_exact_sha_source",
            "quit_macos_bundle_id",
            "activate_macos_bundle_id",
            "wait_for_macos_bundle_window",
            "wait_for_macos_bundle_window_title",
            "detect_macos_app_bundle",
            "macos_bundle_id_for_app_path",
            "launch_macos_terminal_proof_command",
            "close_macos_terminal_windows_with_title",
            "wait_for_macos_window",
            "wait_for_path",
            "capture_macos_window",
            "parse_coordinate_pair",
            "resolve_view_tree_click_point",
            "screen_point_for_content_point",
            "activate_macos_pid",
            "dispatch_macos_click",
            "image_change_summary",
            "start_macos_window_video_recording",
            "stop_macos_window_video_recording",
            "compose_desktop_video_proof",
            "create_issue_video_variant",
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
            record_video=True,
            video_duration_secs=2.0,
            video_fps=15.0,
            video_capture_target="terminal",
            capture_bundle_id="com.cockos.reaper",
            video_attachment_budget_bytes=50,
            compose_video_proof=True,
            video_template="design-parity",
            video_source_image="/tmp/reference.png",
            video_source_label="Figma reference",
            video_title="Design parity proof",
            video_notes=["Reference matches implementation"],
        )

        self.assertEqual(result, {"ok": True})
        self.assertEqual(captured["args"], ({"desktop_automation": {}}, "/tmp/app"))
        self.assertEqual(captured["kwargs"]["action_name"], "click")
        self.assertIs(captured["kwargs"]["create_desktop_run_bundle_fn"], bindings["create_desktop_run_bundle"])
        self.assertIs(captured["kwargs"]["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(captured["kwargs"]["sleep_fn"], time_mod.sleep)
        self.assertIs(captured["kwargs"]["run_fn"], subprocess_mod.run)
        self.assertIs(captured["kwargs"]["popen_fn"], subprocess_mod.Popen)
        self.assertIs(captured["kwargs"]["split_command_fn"], shlex_mod.split)
        self.assertIs(captured["kwargs"]["environ_copy_fn"], os_mod.environ.copy)
        self.assertIs(captured["kwargs"]["wait_for_macos_window_fn"], bindings["wait_for_macos_window"])
        self.assertIs(captured["kwargs"]["wait_for_macos_bundle_window_title_fn"], bindings["wait_for_macos_bundle_window_title"])
        self.assertIs(captured["kwargs"]["launch_macos_terminal_proof_command_fn"], bindings["launch_macos_terminal_proof_command"])
        self.assertIs(
            captured["kwargs"]["close_macos_terminal_windows_with_title_fn"],
            bindings["close_macos_terminal_windows_with_title"],
        )
        self.assertTrue(callable(captured["kwargs"]["cwd_path_fn"]))
        self.assertIs(captured["kwargs"]["capture_macos_window_fn"], bindings["capture_macos_window"])
        self.assertIs(captured["kwargs"]["start_macos_window_video_recording_fn"], bindings["start_macos_window_video_recording"])
        self.assertIs(captured["kwargs"]["stop_macos_window_video_recording_fn"], bindings["stop_macos_window_video_recording"])
        self.assertIs(captured["kwargs"]["compose_desktop_video_proof_fn"], bindings["compose_desktop_video_proof"])
        self.assertIs(captured["kwargs"]["create_issue_video_variant_fn"], bindings["create_issue_video_variant"])
        self.assertTrue(captured["kwargs"]["record_video"])
        self.assertTrue(captured["kwargs"]["compose_video_proof"])
        self.assertEqual(captured["kwargs"]["video_duration_secs"], 2.0)
        self.assertEqual(captured["kwargs"]["video_fps"], 15.0)
        self.assertEqual(captured["kwargs"]["video_capture_target"], "terminal")
        self.assertEqual(captured["kwargs"]["capture_bundle_id"], "com.cockos.reaper")
        self.assertEqual(captured["kwargs"]["video_attachment_budget_bytes"], 50)
        self.assertEqual(captured["kwargs"]["video_template"], "design-parity")
        self.assertEqual(captured["kwargs"]["video_source_image"], "/tmp/reference.png")
        self.assertEqual(captured["kwargs"]["video_source_label"], "Figma reference")
        self.assertEqual(captured["kwargs"]["video_title"], "Design parity proof")
        self.assertEqual(captured["kwargs"]["video_notes"], ["Reference matches implementation"])
        self.assertIs(captured["kwargs"]["terminate_process_fn"], bindings["terminate_process"])


if __name__ == "__main__":
    unittest.main()
