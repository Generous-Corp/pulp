#!/usr/bin/env python3
"""No-network tests for local-ci macOS desktop action execution helpers."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("macos_desktop_action.py")


def load_module():
    spec = importlib.util.spec_from_file_location("macos_desktop_action_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeProcess:
    def __init__(self, pid: int = 4242) -> None:
        self.pid = pid


class MacosDesktopActionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.bundle_dir = self.root / "bundle"
        self.bundle_dir.mkdir()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def artifact_paths(self, current_bundle: Path, _output_path: str | None) -> dict[str, Path]:
        return {
            "screenshot": current_bundle / "screenshots" / "window.png",
            "before_screenshot": current_bundle / "screenshots" / "before.png",
            "diff_screenshot": current_bundle / "screenshots" / "diff.png",
            "ui_snapshot": current_bundle / "ui-tree.json",
            "video": current_bundle / "video" / "proof.mp4",
            "video_composed": current_bundle / "video" / "proof-composed.mp4",
            "video_issue": current_bundle / "video" / "proof.issue.mp4",
            "video_metadata": current_bundle / "video" / "metadata.json",
            "video_composed_metadata": current_bundle / "video" / "composed-metadata.json",
            "video_issue_metadata": current_bundle / "video" / "issue-metadata.json",
            "video_poster": current_bundle / "video" / "poster.png",
            "stdout": current_bundle / "stdout.log",
            "stderr": current_bundle / "stderr.log",
        }

    def run_action(self, **overrides):
        rollups = overrides.pop("rollups", [])
        launched = overrides.pop("launched", [])
        terminated = overrides.pop("terminated", [])
        waited_paths = overrides.pop("waited_paths", [])
        source_context = overrides.pop("source_context", None)
        window = overrides.pop(
            "window",
            {"windowId": 88, "title": "UI Preview", "bounds": {"width": 320, "height": 200}},
        )

        def popen(args, **kwargs):
            launched.append((args, kwargs))
            return FakeProcess()

        def wait_for_path(path: Path, _timeout: float) -> None:
            waited_paths.append(path.name)
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.name == "ui-tree.json":
                path.write_text(json.dumps({"id": "root", "type": "Window"}))
            else:
                path.write_bytes(b"png")

        def prepare_source(bundle_dir, target_name, command, context):
            if source_context is None:
                self.fail("unexpected source preparation")
            return source_context

        def capture_window(_window_id: int, path: Path) -> None:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"png")

        def image_change_summary(_before_path, _after_path, *, diff_output_path=None):
            if diff_output_path is not None:
                diff_output_path.parent.mkdir(parents=True, exist_ok=True)
                diff_output_path.write_bytes(b"diff")
            return {"changed": True}

        def start_video(window_payload, output_path, *, duration_secs, fps):
            self.assertEqual(window_payload, window)
            return {"path": str(output_path), "duration_secs": duration_secs, "fps": fps}

        def stop_video(recording, *, output_path, metadata_path, poster_path, duration_secs, fps, attachment_budget_bytes):
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_bytes(b"mp4")
            poster_path.write_bytes(b"poster")
            metadata = {
                "path": str(output_path),
                "duration_secs": duration_secs,
                "fps": fps,
                "poster": {"path": str(poster_path), "exists": True},
                "size": {
                    "size_bytes": output_path.stat().st_size,
                    "attachment_budget_bytes": attachment_budget_bytes,
                    "fits_attachment_budget": True,
                },
                "recording": recording,
            }
            metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
            return metadata

        def compose_video(manifest_path: Path, output_path: Path):
            self.assertTrue(manifest_path.exists())
            output_path.write_bytes(b"composed")
            return {"output": str(output_path), "composer": "remotion", "size": {"fits_attachment_budget": True}}

        def issue_video(source_path: Path, output_path: Path, metadata_path: Path, *, attachment_budget_bytes: int):
            self.assertTrue(source_path.exists())
            output_path.write_bytes(source_path.read_bytes())
            payload = {
                "output": str(output_path),
                "source": str(source_path),
                "status": "copied",
                "size": {"fits_attachment_budget": True, "attachment_budget_bytes": attachment_budget_bytes},
            }
            metadata_path.write_text(json.dumps(payload, indent=2) + "\n")
            return payload

        kwargs = {
            "action_name": "inspect",
            "bundle_id": None,
            "label": "ui-preview",
            "output_path": None,
            "capture_ui_snapshot": True,
            "click_point": None,
            "click_view_id": None,
            "click_view_type": None,
            "click_view_text": None,
            "click_view_label": None,
            "pulp_app_automation": False,
            "capture_before": False,
            "settle_secs": 0.0,
            "timeout_secs": 5.0,
            "source_request": None,
            "create_desktop_run_bundle_fn": lambda *_args: self.bundle_dir,
            "desktop_action_artifact_paths_fn": self.artifact_paths,
            "desktop_interaction_requested_fn": lambda **kwargs: any(kwargs.values()),
            "macos_accessibility_trusted_fn": lambda: True,
            "now_iso_fn": lambda: "2026-06-11T00:00:00+00:00",
            "prepare_macos_exact_sha_source_fn": prepare_source,
            "quit_macos_bundle_id_fn": lambda _bundle_id: None,
            "sleep_fn": lambda _secs: None,
            "run_fn": lambda *_args, **_kwargs: None,
            "activate_macos_bundle_id_fn": lambda _bundle_id: None,
            "wait_for_macos_bundle_window_fn": lambda _bundle_id, _timeout: (5151, window),
            "split_command_fn": lambda command: command.split(),
            "detect_macos_app_bundle_fn": lambda _command: None,
            "macos_bundle_id_for_app_path_fn": lambda _path: None,
            "environ_copy_fn": lambda: {},
            "popen_fn": popen,
            "wait_for_macos_window_fn": lambda _pid, _timeout: window,
            "content_size_from_window_fn": lambda _window: (320.0, 200.0),
            "wait_for_path_fn": wait_for_path,
            "content_size_from_view_tree_fn": lambda _tree, fallback: fallback,
            "view_tree_inspector_summary_fn": lambda tree: {"node_count": 1, "root_type": tree["type"]},
            "pulp_app_interaction_summary_fn": lambda **kwargs: {"mode": "pulp-app", "selector": kwargs},
            "capture_macos_window_fn": capture_window,
            "parse_coordinate_pair_fn": lambda point, **_kwargs: tuple(float(part) for part in point.split(",")),
            "resolve_view_tree_click_point_fn": lambda *_args, **_kwargs: (12.0, 24.0),
            "screen_point_for_content_point_fn": lambda _window, _content_size, content_point: (content_point[0] + 10.0, content_point[1] + 20.0),
            "activate_macos_pid_fn": lambda _pid: {"activated": True},
            "dispatch_macos_click_fn": lambda x, y: {"clicked": True, "x": x, "y": y},
            "desktop_click_selector_fn": lambda **kwargs: kwargs,
            "image_change_summary_fn": image_change_summary,
            "start_macos_window_video_recording_fn": start_video,
            "stop_macos_window_video_recording_fn": stop_video,
            "compose_desktop_video_proof_fn": compose_video,
            "create_issue_video_variant_fn": issue_video,
            "attach_desktop_source_to_manifest_fn": lambda payload, context: payload.setdefault("source", context) if context else None,
            "atomic_write_text_fn": lambda path, text: path.write_text(text),
            "write_desktop_run_rollups_fn": lambda *args, **kwargs: rollups.append((args, kwargs)),
            "terminate_process_fn": lambda proc: terminated.append(proc.pid),
        }
        kwargs.update(overrides)

        manifest = self.mod.run_macos_local_smoke(
            {"defaults": {}},
            "/repo/build/ui-preview",
            **kwargs,
        )
        return manifest, launched, terminated, waited_paths, rollups

    def test_run_macos_local_smoke_writes_manifest_and_ui_snapshot(self) -> None:
        manifest, launched, terminated, waited_paths, rollups = self.run_action()

        self.assertEqual(manifest["target"], "mac")
        self.assertEqual(manifest["command"], ["/repo/build/ui-preview"])
        self.assertEqual(manifest["inspector"], {"node_count": 1, "root_type": "Window"})
        self.assertIn("PULP_VIEW_TREE_OUT", launched[0][1]["env"])
        self.assertEqual(terminated, [4242])
        self.assertIn("ui-tree.json", waited_paths)
        self.assertTrue((self.bundle_dir / "manifest.json").exists())
        self.assertEqual(len(rollups), 2)

    def test_run_macos_local_smoke_exact_sha_uses_prepared_command_and_cwd(self) -> None:
        source_context = {
            "mode": "exact-sha",
            "sha": "a" * 40,
            "launch_cwd": str(self.root / "prepared"),
            "launch_command": "/prepared/ui-preview",
        }

        manifest, launched, _terminated, _waited_paths, _rollups = self.run_action(
            source_request={"mode": "exact-sha", "sha": "a" * 40},
            source_context=source_context,
            capture_ui_snapshot=False,
        )

        self.assertEqual(launched[0][0], ["/prepared/ui-preview"])
        self.assertEqual(launched[0][1]["cwd"], source_context["launch_cwd"])
        self.assertEqual(manifest["source"], source_context)

    def test_run_macos_local_smoke_delegates_pulp_app_interaction(self) -> None:
        manifest, launched, terminated, waited_paths, _rollups = self.run_action(
            action_name="click",
            pulp_app_automation=True,
            capture_before=True,
            click_view_id="bypass-toggle",
        )

        env = launched[0][1]["env"]
        self.assertEqual(env["PULP_AUTOMATION_CLICK_VIEW_ID"], "bypass-toggle")
        self.assertEqual(env["PULP_AUTOMATION_EXIT_AFTER"], "1")
        self.assertEqual(manifest["interaction"]["mode"], "pulp-app")
        self.assertIn("before.png", waited_paths)
        self.assertEqual(terminated, [4242])

    def test_run_macos_local_smoke_dispatches_desktop_click_and_diff(self) -> None:
        manifest, _launched, _terminated, _waited_paths, _rollups = self.run_action(
            action_name="click",
            capture_ui_snapshot=True,
            click_point="20,30",
            capture_before=True,
        )

        self.assertEqual(manifest["interaction"]["mode"], "desktop-event")
        self.assertEqual(manifest["interaction"]["click"]["screen_point"], {"x": 30.0, "y": 50.0})
        self.assertIn("before_screenshot", manifest["artifacts"])
        self.assertTrue(manifest["artifacts"]["diff_screenshot"].endswith("/screenshots/diff.png"))

    def test_run_macos_local_smoke_records_video_manifest_artifacts(self) -> None:
        manifest, _launched, _terminated, _waited_paths, _rollups = self.run_action(
            record_video=True,
            video_duration_secs=3.0,
            video_fps=12.0,
            video_attachment_budget_bytes=4_000_000,
            compose_video_proof=True,
            capture_ui_snapshot=False,
        )

        self.assertTrue(manifest["artifacts"]["video"].endswith("/video/proof.mp4"))
        self.assertTrue(manifest["artifacts"]["video_metadata"].endswith("/video/metadata.json"))
        self.assertTrue(manifest["artifacts"]["video_poster"].endswith("/video/poster.png"))
        self.assertEqual(manifest["video"]["duration_secs"], 3.0)
        self.assertEqual(manifest["video"]["fps"], 12.0)
        self.assertTrue(manifest["video"]["size"]["fits_attachment_budget"])
        self.assertTrue(manifest["artifacts"]["video_composed"].endswith("/video/proof-composed.mp4"))
        self.assertTrue(manifest["artifacts"]["video_composed_metadata"].endswith("/video/composed-metadata.json"))
        self.assertEqual(manifest["video_composed"]["composer"], "remotion")
        self.assertTrue(manifest["artifacts"]["video_issue"].endswith("/video/proof.issue.mp4"))
        self.assertTrue(manifest["artifacts"]["video_issue_metadata"].endswith("/video/issue-metadata.json"))
        self.assertEqual(manifest["video_issue"]["status"], "copied")
        self.assertTrue(manifest["video_issue"]["source"].endswith("/video/proof-composed.mp4"))

    def test_run_macos_local_smoke_rejects_view_click_without_snapshot(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "View-targeted click requires"):
            self.run_action(
                capture_ui_snapshot=False,
                click_view_id="bypass-toggle",
            )


if __name__ == "__main__":
    unittest.main()
