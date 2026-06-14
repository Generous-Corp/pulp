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
            "video_audio": current_bundle / "video" / "audio.wav",
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
        terminal_cleanups = overrides.pop("terminal_cleanups", [])
        source_context = overrides.pop("source_context", None)
        window = overrides.pop(
            "window",
            {"windowId": 88, "title": "UI Preview", "bounds": {"width": 320, "height": 200}},
        )
        expected_video_window = overrides.pop("expected_video_window", window)

        def popen(args, **kwargs):
            launched.append((args, kwargs))
            return FakeProcess()

        def wait_for_path(path: Path, _timeout: float) -> None:
            waited_paths.append(path.name)
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.name == "ui-tree.json":
                path.write_text(json.dumps({"id": "root", "type": "Window"}))
            elif path.name == "terminal-returncode.txt" and path.exists():
                return
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

        def start_video(
            window_payload,
            output_path,
            *,
            duration_secs,
            fps,
            audio_source="none",
            audio_device=None,
            prefer_frame_sequence=False,
        ):
            self.assertEqual(window_payload, expected_video_window)
            return {
                "path": str(output_path),
                "duration_secs": duration_secs,
                "fps": fps,
                "audio_source": audio_source,
                "audio_device": audio_device,
                "prefer_frame_sequence": prefer_frame_sequence,
            }

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

        def compose_video(manifest_path: Path, output_path: Path, **kwargs):
            self.assertTrue(manifest_path.exists())
            output_path.write_bytes(b"composed")
            serializable_kwargs = dict(kwargs)
            if serializable_kwargs.get("source_image") is not None:
                serializable_kwargs["source_image"] = str(serializable_kwargs["source_image"])
            return {"output": str(output_path), "composer": "remotion", "size": {"fits_attachment_budget": True}, "kwargs": serializable_kwargs}

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

        def mux_audio(video_path: Path, audio_path: Path, *, attachment_budget_bytes: int):
            self.assertTrue(video_path.exists())
            self.assertTrue(audio_path.exists())
            video_path.write_bytes(video_path.read_bytes() + b"+audio")
            return {
                "kind": "desktop-video-proof-audio-mux",
                "status": "muxed",
                "audio_source": "plugin",
                "audio_file": str(audio_path),
                "output": str(video_path),
                "size": {
                    "size_bytes": video_path.stat().st_size,
                    "attachment_budget_bytes": attachment_budget_bytes,
                    "fits_attachment_budget": True,
                },
            }

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
            "wait_for_macos_bundle_window_title_fn": lambda _bundle_id, _title, _timeout: (5151, window),
            "wait_for_macos_bundle_secondary_window_fn": lambda _bundle_id, _timeout: (5151, window),
            "split_command_fn": lambda command: command.split(),
            "detect_macos_app_bundle_fn": lambda _command: None,
            "macos_bundle_id_for_app_path_fn": lambda _path: None,
            "environ_copy_fn": lambda: {},
            "cwd_path_fn": lambda: self.root,
            "launch_macos_terminal_proof_command_fn": lambda args, **kwargs: {
                "bundle_id": "com.apple.Terminal",
                "title": kwargs["title"],
                "command": args,
                "returncode": str(kwargs["returncode_path"]),
            },
            "close_macos_terminal_windows_with_title_fn": lambda title: terminal_cleanups.append(title)
            or {"title_contains": title, "closed_count": 1, "returncode": 0},
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
            "mux_desktop_video_audio_fn": mux_audio,
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

    def test_run_macos_local_smoke_muxes_plugin_audio_file(self) -> None:
        source_audio = self.root / "plugin-output.wav"
        source_audio.write_bytes(b"wav")

        manifest, _launched, _terminated, _waited_paths, _rollups = self.run_action(
            record_video=True,
            video_audio_source="plugin",
            video_audio_file=str(source_audio),
            video_attachment_budget_bytes=4_000_000,
            capture_ui_snapshot=False,
        )

        self.assertTrue(manifest["artifacts"]["video_audio"].endswith("/video/audio.wav"))
        copied_audio = self.bundle_dir / "video" / "audio.wav"
        self.assertEqual(copied_audio.read_bytes(), b"wav")
        self.assertTrue(manifest["video"]["has_audio"])
        self.assertEqual(manifest["video"]["audio_source"], "plugin")
        self.assertEqual(manifest["video"]["audio_file"], str(copied_audio))
        self.assertEqual(manifest["video"]["audio_mux"]["status"], "muxed")
        self.assertEqual(manifest["video"]["recording"]["audio_source"], "none")

    def test_run_macos_local_smoke_records_terminal_capture(self) -> None:
        terminal_launches = []
        terminal_cleanups = []

        def launch_terminal(args, **kwargs):
            terminal_launches.append((args, kwargs))
            kwargs["returncode_path"].write_text("143\n")
            return {
                "bundle_id": "com.apple.Terminal",
                "title": kwargs["title"],
                "command": args,
                "returncode": str(kwargs["returncode_path"]),
            }

        window = {"windowId": 99, "title": "Pulp Video Proof abcd1234", "bounds": {"width": 640, "height": 360}}
        manifest, launched, terminated, _waited_paths, _rollups = self.run_action(
            window=window,
            capture_ui_snapshot=False,
            record_video=True,
            video_capture_target="terminal",
            video_duration_secs=3.0,
            launch_macos_terminal_proof_command_fn=launch_terminal,
            terminal_cleanups=terminal_cleanups,
        )

        self.assertEqual(launched, [])
        self.assertEqual(terminated, [])
        self.assertEqual(terminal_launches[0][0], ["/repo/build/ui-preview"])
        self.assertEqual(terminal_launches[0][1]["cwd"], self.root)
        self.assertEqual(terminal_launches[0][1]["keepalive_secs"], 5.0)
        self.assertEqual(manifest["terminal"]["returncode"], 143)
        self.assertEqual(terminal_cleanups, [terminal_launches[0][1]["title"]])
        self.assertEqual(manifest["terminal"]["cleanup"]["closed_count"], 1)
        self.assertEqual(manifest["terminal"]["returncode_path"], str(self.bundle_dir / "terminal-returncode.txt"))
        self.assertTrue(manifest["artifacts"]["terminal_returncode"].endswith("/terminal-returncode.txt"))
        self.assertEqual(manifest["window"], window)
        self.assertIn("terminal", manifest)

    def test_run_macos_local_smoke_captures_wrapper_spawned_bundle_window(self) -> None:
        activated = []
        bundle_waits = []
        quits = []
        host_window = {"windowId": 120, "title": "REAPER", "bounds": {"width": 900, "height": 520}}

        def wait_for_bundle(bundle_id, timeout):
            bundle_waits.append((bundle_id, timeout))
            return (9090, host_window)

        manifest, launched, terminated, _waited_paths, _rollups = self.run_action(
            window=host_window,
            capture_ui_snapshot=False,
            capture_bundle_id="com.cockos.reaper",
            record_video=True,
            wait_for_macos_bundle_window_fn=wait_for_bundle,
            activate_macos_bundle_id_fn=lambda bundle_id: activated.append(bundle_id),
            quit_macos_bundle_id_fn=lambda bundle_id: quits.append(bundle_id),
        )

        self.assertEqual(launched[0][0], ["/repo/build/ui-preview"])
        self.assertEqual(bundle_waits, [("com.cockos.reaper", 5.0)])
        self.assertEqual(activated, ["com.cockos.reaper"])
        self.assertEqual(manifest["window"], host_window)
        self.assertTrue(manifest["video"]["recording"]["prefer_frame_sequence"])
        self.assertEqual(terminated, [4242])
        self.assertEqual(quits, ["com.cockos.reaper"])

    def test_generated_reaper_recipe_captures_secondary_window(self) -> None:
        host_window = {"windowId": 120, "title": "", "bounds": {"width": 900, "height": 520}}
        editor_window = {"windowId": 121, "title": "", "bounds": {"width": 724, "height": 394}}
        bundle_waits = []
        secondary_waits = []

        def wait_for_bundle(bundle_id, timeout):
            bundle_waits.append((bundle_id, timeout))
            (self.bundle_dir / "stdout.log").write_text(
                "TrackFX_AddByName PulpSynth -> 0\n"
                "fx name ok=true name=CLAPi: PulpSynth (Pulp)\n"
                "TrackFX_Show floating-editor mode=3\n"
            )
            return (9090, host_window)

        def wait_for_secondary(bundle_id, timeout):
            secondary_waits.append((bundle_id, timeout))
            return (9090, editor_window)

        manifest, _launched, _terminated, _waited_paths, _rollups = self.run_action(
            window=host_window,
            expected_video_window=editor_window,
            capture_ui_snapshot=False,
            capture_bundle_id="com.cockos.reaper",
            record_video=True,
            wait_for_macos_bundle_window_fn=wait_for_bundle,
            wait_for_macos_bundle_secondary_window_fn=wait_for_secondary,
            video_context={"reaper_recipe": "generated"},
        )

        self.assertEqual(bundle_waits, [("com.cockos.reaper", 5.0)])
        self.assertEqual(secondary_waits, [("com.cockos.reaper", 5.0)])
        self.assertEqual(manifest["window"], editor_window)
        self.assertEqual(
            manifest["capture_window_refinement"]["strategy"],
            "floating-editor-secondary-window-after-reaper-marker",
        )
        self.assertEqual(manifest["capture_window_refinement"]["from"], host_window)
        self.assertEqual(manifest["capture_window_refinement"]["to"], editor_window)

    def test_generated_reaper_recipe_terminates_captured_reaper_pid(self) -> None:
        host_window = {"windowId": 120, "title": "", "bounds": {"width": 900, "height": 520}}
        editor_window = {"windowId": 121, "title": "", "bounds": {"width": 724, "height": 394}}
        terminated_pids = []

        def wait_for_bundle(_bundle_id, _timeout):
            (self.bundle_dir / "stdout.log").write_text(
                "TrackFX_AddByName PulpSynth -> 0\n"
                "fx name ok=true name=CLAPi: PulpSynth (Pulp)\n"
                "TrackFX_Show floating-editor mode=3\n"
            )
            return (9090, host_window)

        original_terminate_pid = self.mod._terminate_pid
        self.mod._terminate_pid = lambda pid, **_kwargs: terminated_pids.append(pid)
        try:
            _manifest, _launched, terminated, _waited_paths, _rollups = self.run_action(
                window=host_window,
                expected_video_window=editor_window,
                capture_ui_snapshot=False,
                capture_bundle_id="com.cockos.reaper",
                record_video=True,
                wait_for_macos_bundle_window_fn=wait_for_bundle,
                wait_for_macos_bundle_secondary_window_fn=lambda _bundle_id, _timeout: (9090, editor_window),
                video_context={"reaper_recipe": "generated"},
            )
        finally:
            self.mod._terminate_pid = original_terminate_pid

        self.assertEqual(terminated_pids, [9090])
        self.assertEqual(terminated, [4242])

    def test_run_macos_local_smoke_ignores_cleanup_quit_failures(self) -> None:
        host_window = {"windowId": 120, "title": "REAPER", "bounds": {"width": 900, "height": 520}}

        manifest, _launched, terminated, _waited_paths, _rollups = self.run_action(
            window=host_window,
            capture_ui_snapshot=False,
            capture_bundle_id="com.cockos.reaper",
            record_video=True,
            wait_for_macos_bundle_window_fn=lambda _bundle_id, _timeout: (9090, host_window),
            quit_macos_bundle_id_fn=lambda _bundle_id: (_ for _ in ()).throw(RuntimeError("connection invalid")),
        )

        self.assertEqual(manifest["window"], host_window)
        self.assertEqual(terminated, [4242])

    def test_generated_reaper_recipe_status_requires_opened_plugin(self) -> None:
        log_path = self.root / "stdout.log"
        log_path.write_text(
            "TrackFX_AddByName PulpSynth -> 0\n"
            "fx name ok=true name=CLAPi: PulpSynth (Pulp)\n"
            "TrackFX_Show floating-editor mode=3\n"
        )
        self.mod._validate_generated_reaper_recipe_status({"reaper_recipe": "generated"}, log_path)

        log_path.write_text("TrackFX_AddByName PulpSynth -> -1\nplugin not found\n")
        with self.assertRaisesRegex(RuntimeError, "did not find the requested plugin"):
            self.mod._validate_generated_reaper_recipe_status({"reaper_recipe": "generated"}, log_path)

        log_path.write_text("starting\n")
        with self.assertRaisesRegex(RuntimeError, "did not confirm that the plugin loaded"):
            self.mod._validate_generated_reaper_recipe_status({"reaper_recipe": "generated"}, log_path)

        log_path.write_text("TrackFX_AddByName PulpSynth -> 0\nfx name ok=true name=CLAPi: PulpSynth (Pulp)\n")
        with self.assertRaisesRegex(RuntimeError, "floating plugin editor"):
            self.mod._validate_generated_reaper_recipe_status({"reaper_recipe": "generated"}, log_path)

    def test_run_macos_local_smoke_rejects_capture_bundle_id_combinations(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "only valid with --command"):
            self.run_action(
                bundle_id="com.cockos.reaper",
                capture_bundle_id="com.cockos.reaper",
                capture_ui_snapshot=False,
            )

        with self.assertRaisesRegex(RuntimeError, "cannot be combined with --video-capture-target terminal"):
            self.run_action(
                capture_bundle_id="com.cockos.reaper",
                capture_ui_snapshot=False,
                video_capture_target="terminal",
            )

        with self.assertRaisesRegex(RuntimeError, "cannot inject UI snapshot"):
            self.run_action(capture_bundle_id="com.cockos.reaper")

    def test_run_macos_local_smoke_passes_video_composition_context(self) -> None:
        reference = self.root / "reference.png"
        reference.write_bytes(b"png")

        manifest, _launched, _terminated, _waited_paths, _rollups = self.run_action(
            record_video=True,
            compose_video_proof=True,
            capture_ui_snapshot=False,
            video_template="design-parity",
            video_source_image=str(reference),
            video_source_label="Figma reference",
            video_title="Design parity proof",
            video_notes=["Reference matches implementation"],
            video_context={"recipe": "design-parity", "source": "figma"},
        )

        self.assertEqual(manifest["video_composed"]["kwargs"]["template"], "design-parity")
        self.assertEqual(manifest["video_composed"]["kwargs"]["source_image"], str(reference.resolve()))
        self.assertEqual(manifest["video_composed"]["kwargs"]["source_label"], "Figma reference")
        self.assertEqual(manifest["video_composed"]["kwargs"]["title"], "Design parity proof")
        self.assertEqual(manifest["video_composed"]["kwargs"]["notes"], ["Reference matches implementation"])
        self.assertEqual(manifest["video_proof_notes"], ["Reference matches implementation"])
        self.assertEqual(manifest["video_proof_composition"]["template"], "design-parity")
        self.assertEqual(manifest["video_proof_composition"]["source_image"], str(reference.resolve()))
        self.assertEqual(manifest["video_proof_composition"]["notes"], ["Reference matches implementation"])
        self.assertEqual(manifest["video_proof_composition"]["context"], {"recipe": "design-parity", "source": "figma"})

    def test_run_macos_local_smoke_adds_component_zoom_focus_context(self) -> None:
        manifest, _launched, _terminated, _waited_paths, _rollups = self.run_action(
            action_name="click",
            record_video=True,
            compose_video_proof=True,
            capture_ui_snapshot=True,
            capture_before=True,
            click_view_id="bypass-toggle",
            video_template="component-zoom",
            video_title="Component validation",
        )

        composition = manifest["video_proof_composition"]
        self.assertEqual(composition["template"], "component-zoom")
        self.assertEqual(composition["focus"]["label"], "bypass-toggle")
        self.assertEqual(composition["focus"]["selector"]["click_view_id"], "bypass-toggle")
        self.assertEqual(composition["focus"]["content_point"], {"x": 12.0, "y": 24.0})
        self.assertAlmostEqual(composition["focus"]["normalized_center"]["x"], 12.0 / 320.0)
        self.assertAlmostEqual(composition["focus"]["normalized_center"]["y"], 24.0 / 200.0)
        self.assertEqual(composition["action_marker"]["kind"], "click")
        self.assertEqual(composition["action_marker"]["label"], "bypass-toggle")
        self.assertEqual(composition["action_marker"]["content_point"], {"x": 12.0, "y": 24.0})
        self.assertAlmostEqual(composition["action_marker"]["normalized_point"]["x"], 12.0 / 320.0)
        self.assertAlmostEqual(composition["action_marker"]["normalized_point"]["y"], 24.0 / 200.0)
        self.assertEqual(manifest["video_composed"]["kwargs"]["template"], "component-zoom")

    def test_run_macos_local_smoke_rejects_view_click_without_snapshot(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "View-targeted click requires"):
            self.run_action(
                capture_ui_snapshot=False,
                click_view_id="bypass-toggle",
            )


if __name__ == "__main__":
    unittest.main()
