#!/usr/bin/env python3
"""No-network tests for local-ci macOS desktop helpers."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import struct
import tempfile
import time
import unittest
import zlib


MODULE_PATH = Path(__file__).with_name("macos_desktop.py")


def load_module():
    spec = importlib.util.spec_from_file_location("macos_desktop_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_rgb_png(path: Path, width: int, height: int, pixels: list[tuple[int, int, int]]) -> None:
    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            row.extend(pixels[y * width + x])
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", raw)
        + chunk(b"IEND", b"")
    )


class MacOSDesktopTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_bundle_detection_and_plist_bundle_id(self) -> None:
        app_binary = self.root / "Demo.app" / "Contents" / "MacOS" / "Demo"
        app_binary.parent.mkdir(parents=True)
        app_binary.write_text("#!/bin/sh\n")

        self.assertIsNone(self.mod.detect_macos_app_bundle(None))
        self.assertIsNone(self.mod.detect_macos_app_bundle(""))
        self.assertEqual(self.mod.detect_macos_app_bundle(str(app_binary)), self.root / "Demo.app")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Missing.app"))

        info_plist = self.root / "Demo.app" / "Contents" / "Info.plist"
        info_plist.write_bytes(plistlib.dumps({"CFBundleIdentifier": "com.example.demo"}))
        self.assertEqual(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"), "com.example.demo")

        info_plist.write_text("not a plist")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"))

    def test_swift_and_osascript_helpers_parse_command_results(self) -> None:
        probe_path = self.root / "macos_window_probe.swift"
        calls: list[list[str]] = []

        def run_json(cmd: list[str], **_kwargs):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout='{"trusted":true,"windows":[{"id":9}],"activated":true}\n', stderr="")

        self.assertEqual(self.mod.macos_window_probe_path(self.root), probe_path)
        self.assertEqual(
            self.mod.macos_window_info_for_pid(42, probe_path_fn=lambda: probe_path, run_fn=run_json)["windows"][0]["id"],
            9,
        )
        self.assertEqual(calls[-1], ["swift", str(probe_path), "window-info", "--pid", "42"])
        self.assertTrue(self.mod.macos_accessibility_trusted(probe_path_fn=lambda: probe_path, run_fn=run_json))
        self.assertEqual(calls[-1], ["swift", str(probe_path), "accessibility-trusted"])
        self.assertTrue(self.mod.activate_macos_pid(42, probe_path_fn=lambda: probe_path, run_fn=run_json)["activated"])
        self.assertEqual(calls[-1], ["swift", str(probe_path), "activate", "--pid", "42"])
        self.assertTrue(self.mod.dispatch_macos_click(10.5, 20.25, probe_path_fn=lambda: probe_path, run_fn=run_json)["activated"])
        self.assertEqual(calls[-1], ["swift", str(probe_path), "click", "--x", "10.5", "--y", "20.25"])

        def run_osascript(cmd: list[str], **_kwargs):
            return subprocess.CompletedProcess(cmd, 7, stdout="out\n", stderr="err\n")

        activation = self.mod.activate_macos_bundle_id("com.example.demo", run_fn=run_osascript)
        self.assertFalse(activation["activated"])
        self.assertEqual(activation["bundle_id"], "com.example.demo")
        self.assertEqual(activation["stdout"], "out")
        self.assertEqual(activation["stderr"], "err")

        quit_calls: list[list[str]] = []

        def run_quit(cmd: list[str], **_kwargs):
            quit_calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        self.mod.quit_macos_bundle_id("com.example.demo", run_fn=run_quit)
        self.assertEqual(quit_calls[0], ["osascript", "-e", 'tell application id "com.example.demo" to quit'])

    def test_wait_helpers_retry_until_windows_are_visible(self) -> None:
        now = [0.0]

        def time_fn() -> float:
            return now[0]

        def sleep_fn(amount: float) -> None:
            now[0] += amount

        pid_payloads = [{"windows": []}, {"windows": [{"id": 7}]}]

        def info_for_pid(_pid: int) -> dict:
            return pid_payloads.pop(0)

        self.assertEqual(
            self.mod.wait_for_macos_window(123, 1.0, macos_window_info_for_pid_fn=info_for_pid, time_fn=time_fn, sleep_fn=sleep_fn),
            {"id": 7},
        )

        bundle_payloads = [{"pid": None, "windows": []}, {"pid": 456, "windows": [{"id": 8}]}]
        activations: list[str] = []

        def info_for_bundle(_bundle_id: str) -> dict:
            return bundle_payloads.pop(0)

        def activate(bundle_id: str) -> dict:
            activations.append(bundle_id)
            return {"activated": True, "stderr": ""}

        self.assertEqual(
            self.mod.wait_for_macos_bundle_window(
                "com.example.demo",
                1.0,
                macos_window_info_for_bundle_id_fn=info_for_bundle,
                activate_macos_bundle_id_fn=activate,
                time_fn=time_fn,
                sleep_fn=sleep_fn,
            ),
            (456, {"id": 8}),
        )
        self.assertEqual(activations, ["com.example.demo"])

    def test_wait_for_bundle_window_title_filters_terminal_window(self) -> None:
        now = [0.0]
        payloads = [
            {"pid": 456, "windows": [{"title": "Other", "windowId": 1}]},
            {"pid": 456, "windows": [{"title": "Pulp Video Proof abcd1234", "windowId": 2}]},
        ]

        result = self.mod.wait_for_macos_bundle_window_title(
            "com.apple.Terminal",
            "Pulp Video Proof abcd1234",
            1.0,
            macos_window_info_for_bundle_id_fn=lambda _bundle_id: payloads.pop(0),
            activate_macos_bundle_id_fn=lambda _bundle_id: {"activated": True, "stderr": ""},
            time_fn=lambda: now[0],
            sleep_fn=lambda amount: now.__setitem__(0, now[0] + amount),
        )

        self.assertEqual(result, (456, {"title": "Pulp Video Proof abcd1234", "windowId": 2}))

    def test_terminal_proof_shell_script_sets_title_and_teed_logs(self) -> None:
        script = self.mod.terminal_proof_shell_script(
            cwd=Path("/repo path"),
            command_args=["/tmp/Pulp Tone", "--flag", "two words"],
            title="Pulp Video Proof abcd1234",
            stdout_path=Path("/tmp/std out.log"),
            stderr_path=Path("/tmp/std err.log"),
            returncode_path=Path("/tmp/rc file"),
            keepalive_secs=3.0,
        )

        self.assertIn("cd '/repo path'", script)
        self.assertIn("Pulp Video Proof abcd1234", script)
        self.assertIn("tee -a '/tmp/std out.log'", script)
        self.assertIn("tee -a '/tmp/std err.log'", script)
        self.assertIn("'/tmp/Pulp Tone' --flag 'two words' &", script)
        self.assertIn("sleep 3.000", script)
        self.assertIn("'/tmp/rc file'", script)

    def test_close_terminal_windows_with_title_uses_scoped_title(self) -> None:
        calls = []
        stdout_values = ["2\n", "0\n"]

        def run_osascript(cmd: list[str], **_kwargs):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout=stdout_values.pop(0), stderr="")

        result = self.mod.close_macos_terminal_windows_with_title(
            "Pulp Video Proof abcd1234",
            run_fn=run_osascript,
        )

        self.assertEqual(result["closed_count"], 2)
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(calls[0][0], "osascript")
        self.assertIn("Pulp Video Proof abcd1234", calls[0][-1])
        self.assertIn("close w", calls[0][-1])
        self.assertEqual(len(calls), 2)

    def test_close_terminal_windows_terminates_only_scoped_proof_terminal(self) -> None:
        calls = []

        def run_osascript(cmd: list[str], **_kwargs):
            calls.append(cmd)
            if cmd[0] == "osascript" and "set closedCount" in cmd[-1]:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="User canceled")
            if cmd[0] == "osascript":
                self.assertIn("Pulp Video Proof abcd1234", cmd[-1])
                return subprocess.CompletedProcess(cmd, 0, stdout="1234\t1\t0", stderr="")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        result = self.mod.close_macos_terminal_windows_with_title(
            "Pulp Video Proof abcd1234",
            run_fn=run_osascript,
            attempts=1,
        )

        self.assertTrue(result["terminated_terminal"])
        self.assertEqual(result["terminate_returncode"], 0)
        self.assertEqual(calls[-1], ["kill", "-TERM", "1234"])

    def test_close_terminal_windows_does_not_terminate_with_other_windows(self) -> None:
        calls = []

        def run_osascript(cmd: list[str], **_kwargs):
            calls.append(cmd)
            if cmd[0] == "osascript" and "set closedCount" in cmd[-1]:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="User canceled")
            return subprocess.CompletedProcess(cmd, 0, stdout="1234\t1\t1", stderr="")

        result = self.mod.close_macos_terminal_windows_with_title(
            "Pulp Video Proof abcd1234",
            run_fn=run_osascript,
            attempts=1,
        )

        self.assertFalse(result["terminated_terminal"])
        self.assertNotIn(["kill", "-TERM", "1234"], calls)

    def test_capture_retry_and_process_termination(self) -> None:
        output_path = self.root / "screens" / "window.png"
        calls = [0]

        def run_capture(cmd: list[str], **_kwargs):
            calls[0] += 1
            if calls[0] == 2:
                Path(cmd[-1]).write_text("png")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="busy")

        self.mod.capture_macos_window(88, output_path, run_fn=run_capture, sleep_fn=lambda _amount: None)
        self.assertEqual(calls[0], 2)
        self.assertTrue(output_path.exists())

    def test_capture_falls_back_to_full_screen_when_window_capture_fails(self) -> None:
        output_path = self.root / "screens" / "window.png"
        calls: list[list[str]] = []

        def run_capture(cmd: list[str], **_kwargs):
            calls.append(cmd)
            if "-l" in cmd:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="could not create image from window")
            Path(cmd[-1]).write_text("fullscreen")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        self.mod.capture_macos_window(88, output_path, run_fn=run_capture, sleep_fn=lambda _amount: None)

        self.assertEqual(len([cmd for cmd in calls if "-l" in cmd]), 5)
        self.assertEqual(calls[-1], ["screencapture", "-x", str(output_path)])
        self.assertEqual(output_path.read_text(), "fullscreen")

    def test_capture_reports_window_error_when_full_screen_fallback_fails(self) -> None:
        output_path = self.root / "screens" / "window.png"

        def run_capture(cmd: list[str], **_kwargs):
            if "-l" in cmd:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="could not create image from window")
            return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="could not create image from display")

        with self.assertRaisesRegex(RuntimeError, "could not create image from window"):
            self.mod.capture_macos_window(88, output_path, run_fn=run_capture, sleep_fn=lambda _amount: None)

        class FakeProc:
            def __init__(self) -> None:
                self.terminated = False
                self.killed = False
                self.waits = 0

            def poll(self):
                return None if not self.terminated and not self.killed else 0

            def terminate(self) -> None:
                self.terminated = True

            def wait(self, *, timeout: float):
                self.waits += 1
                if self.waits == 1:
                    raise subprocess.TimeoutExpired("proc", timeout)
                return 0

            def kill(self) -> None:
                self.killed = True

        proc = FakeProc()
        self.mod.terminate_process(proc, timeout_secs=0.01)
        self.assertTrue(proc.terminated)
        self.assertTrue(proc.killed)

    def test_window_video_command_crops_window_region_for_h264_mp4(self) -> None:
        window = {"bounds": {"x": 10.4, "y": 20.6, "width": 321, "height": 201}}
        output_path = self.root / "video" / "proof.mp4"

        command = self.mod.macos_window_video_command(
            window,
            output_path,
            duration_secs=4.0,
            fps=15.0,
            ffmpeg_path="/opt/ffmpeg",
            input_device="5:",
        )

        self.assertEqual(self.mod.macos_window_video_bounds(window), {"x": 10, "y": 21, "width": 320, "height": 200})
        self.assertEqual(command[0], "/opt/ffmpeg")
        self.assertIn("5:", command)
        self.assertIn("nv12", command)
        self.assertIn("crop=320:200:10:21,fps=15.0", command)
        self.assertIn("libx264", command)
        self.assertEqual(command[command.index("-frames:v") + 1], "60")
        self.assertEqual(command[-1], str(output_path))

    def test_avfoundation_screen_input_device_uses_listed_capture_screen_index(self) -> None:
        def run_devices(cmd: list[str], **_kwargs):
            self.assertIn("-list_devices", cmd)
            return subprocess.CompletedProcess(
                cmd,
                1,
                stdout="",
                stderr="[AVFoundation indev @ 0x1] [3] Capture screen 0\n",
            )

        self.assertEqual(self.mod.macos_avfoundation_screen_input_device(run_fn=run_devices), "3:")

    def test_avfoundation_audio_input_device_uses_explicit_or_env(self) -> None:
        self.assertEqual(self.mod.macos_avfoundation_audio_input_device(":2"), "2")
        self.assertEqual(
            self.mod.macos_avfoundation_audio_input_device(env={"PULP_VIDEO_AUDIO_DEVICE": "BlackHole 2ch"}),
            "BlackHole 2ch",
        )
        self.assertIsNone(self.mod.macos_avfoundation_audio_input_device("", env={}))

    def test_png_visual_stats_detects_blank_and_nonblank_posters(self) -> None:
        blank = self.root / "blank.png"
        nonblank = self.root / "nonblank.png"
        write_rgb_png(blank, 2, 2, [(0, 0, 0)] * 4)
        write_rgb_png(nonblank, 2, 2, [(0, 0, 0), (255, 64, 32), (0, 0, 0), (32, 64, 255)])

        blank_stats = self.mod.png_visual_stats(blank)
        nonblank_stats = self.mod.png_visual_stats(nonblank)

        self.assertTrue(blank_stats["ok"])
        self.assertTrue(blank_stats["appears_blank"])
        self.assertTrue(nonblank_stats["ok"])
        self.assertFalse(nonblank_stats["appears_blank"])

    def test_window_video_recording_encodes_screencapture_frame_sequence(self) -> None:
        output_path = self.root / "video" / "proof.mp4"
        metadata_path = self.root / "video" / "metadata.json"
        poster_path = self.root / "video" / "poster.png"
        calls: list[list[str]] = []

        def run_capture_or_encode(cmd: list[str], **_kwargs):
            calls.append(cmd)
            if cmd[0] == "screencapture":
                Path(cmd[-1]).write_bytes(b"png")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[1:] == ["-hide_banner", "-version"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="ffmpeg version 6.0\n", stderr="")
            output_path.write_bytes(b"mp4")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="encoded")

        recording = self.mod.start_macos_window_video_recording(
            {"windowId": 88, "bounds": {"x": 0, "y": 0, "width": 320, "height": 200}},
            output_path,
            duration_secs=0.2,
            fps=5.0,
            run_fn=run_capture_or_encode,
            ffmpeg_path="/opt/ffmpeg",
        )
        time.sleep(0.25)
        def video_metadata(path: Path, **kwargs):
            metadata_kwargs["kwargs"] = kwargs
            return {
                "path": str(path),
                "fps": kwargs["fps"],
                "command": kwargs["command"],
                "encoder": kwargs["encoder"],
                "has_audio": kwargs["has_audio"],
                "audio_source": kwargs["audio_source"],
                "size": {"fits_attachment_budget": True},
            }

        metadata = self.mod.stop_macos_window_video_recording(
            recording,
            output_path=output_path,
            metadata_path=metadata_path,
            poster_path=poster_path,
            duration_secs=0.2,
            fps=5.0,
            attachment_budget_bytes=1_000_000,
            desktop_video_metadata_fn=lambda path, **kwargs: {
                "path": str(path),
                "fps": kwargs["fps"],
                "encoder": kwargs["encoder"],
                "size": {"fits_attachment_budget": True},
            },
            write_desktop_video_metadata_fn=lambda path, payload: path.write_text(str(payload)),
        )

        self.assertTrue(output_path.exists())
        self.assertTrue(metadata_path.exists())
        self.assertTrue(poster_path.exists())
        self.assertEqual(poster_path.read_bytes(), b"png")
        self.assertGreaterEqual(metadata["frame_count"], 1)
        self.assertTrue(metadata["poster"]["exists"])
        self.assertEqual(calls[-1][0], "/opt/ffmpeg")
        self.assertIn(["/opt/ffmpeg", "-hide_banner", "-version"], calls)
        self.assertEqual(metadata["encoder"]["version"], "ffmpeg version 6.0")
        self.assertIn("Could not find AVFoundation device", metadata["fallback_reason"])

    def test_window_video_recording_crops_full_screen_frames_when_window_capture_fails(self) -> None:
        output_path = self.root / "video" / "proof.mp4"
        metadata_path = self.root / "video" / "metadata.json"
        poster_path = self.root / "video" / "poster.png"
        calls: list[list[str]] = []

        def run_capture_or_encode(cmd: list[str], **_kwargs):
            calls.append(cmd)
            if cmd[0] == "screencapture":
                if "-l" in cmd:
                    return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="could not create image from window")
                Path(cmd[-1]).write_bytes(b"fullscreen-png")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[1:] == ["-hide_banner", "-version"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="ffmpeg version 6.0\n", stderr="")
            if "-frames:v" in cmd:
                poster_path.write_bytes(b"cropped-poster")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            output_path.write_bytes(b"mp4")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="encoded")

        recording = self.mod.start_macos_window_video_recording(
            {"windowId": 88, "bounds": {"x": 10, "y": 20, "width": 320, "height": 200}},
            output_path,
            duration_secs=0.2,
            fps=5.0,
            run_fn=run_capture_or_encode,
            ffmpeg_path="/opt/ffmpeg",
        )
        time.sleep(0.25)
        metadata = self.mod.stop_macos_window_video_recording(
            recording,
            output_path=output_path,
            metadata_path=metadata_path,
            poster_path=poster_path,
            duration_secs=0.2,
            fps=5.0,
            attachment_budget_bytes=1_000_000,
            desktop_video_metadata_fn=lambda path, **kwargs: {
                "path": str(path),
                "fps": kwargs["fps"],
                "command": kwargs["command"],
                "encoder": kwargs["encoder"],
                "size": {"fits_attachment_budget": True},
            },
            write_desktop_video_metadata_fn=lambda path, payload: path.write_text(str(payload)),
        )

        encode_command = metadata["command"]
        self.assertEqual(metadata["frame_capture_scope"], "screen-crop")
        self.assertIn("-vf", encode_command)
        self.assertIn("crop=320:200:10:20", encode_command)
        self.assertTrue(any(cmd[:3] == ["screencapture", "-x", "-l"] for cmd in calls))
        self.assertTrue(any(cmd[:2] == ["screencapture", "-x"] and "-l" not in cmd for cmd in calls))
        self.assertTrue(metadata["poster"]["exists"])
        self.assertEqual(poster_path.read_bytes(), b"cropped-poster")
        self.assertTrue(any("window capture failed" in error for error in metadata["capture_errors"]))

    def test_window_video_recording_can_prefer_screencapture_frame_sequence(self) -> None:
        output_path = self.root / "video" / "proof.mp4"
        metadata_path = self.root / "video" / "metadata.json"
        calls: list[list[str]] = []

        def run_capture_or_encode(cmd: list[str], **_kwargs):
            calls.append(cmd)
            if cmd[0] == "screencapture":
                Path(cmd[-1]).write_bytes(b"png")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[1:] == ["-hide_banner", "-version"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="ffmpeg version 6.0\n", stderr="")
            output_path.write_bytes(b"mp4")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="encoded")

        recording = self.mod.start_macos_window_video_recording(
            {"windowId": 88, "bounds": {"x": 10, "y": 20, "width": 320, "height": 200}},
            output_path,
            duration_secs=0.2,
            fps=5.0,
            run_fn=run_capture_or_encode,
            ffmpeg_path="/opt/ffmpeg",
            input_device_fn=lambda **_kwargs: self.fail("AVFoundation should not be probed"),
            prefer_frame_sequence=True,
        )
        time.sleep(0.25)
        metadata = self.mod.stop_macos_window_video_recording(
            recording,
            output_path=output_path,
            metadata_path=metadata_path,
            poster_path=None,
            duration_secs=0.2,
            fps=5.0,
            attachment_budget_bytes=1_000_000,
            desktop_video_metadata_fn=lambda path, **kwargs: {
                "path": str(path),
                "fps": kwargs["fps"],
                "encoder": kwargs["encoder"],
                "size": {"fits_attachment_budget": True},
            },
            write_desktop_video_metadata_fn=lambda path, payload: path.write_text(str(payload)),
        )

        self.assertEqual(recording["mode"], "screencapture-sequence")
        self.assertEqual(metadata["mode"], "screencapture-sequence")
        self.assertEqual(metadata["fallback_reason"], "window-id frame capture preferred")
        self.assertTrue(any(cmd[:3] == ["screencapture", "-x", "-l"] for cmd in calls))
        self.assertFalse(any("avfoundation" in cmd for cmd in calls))

    def test_window_video_recording_uses_ffmpeg_avfoundation_primary_path(self) -> None:
        output_path = self.root / "video" / "proof.mp4"
        metadata_path = self.root / "video" / "metadata.json"
        poster_path = self.root / "video" / "poster.png"
        popen_calls: list[list[str]] = []
        run_calls: list[list[str]] = []

        class FakeProc:
            def __init__(self, cmd: list[str]):
                self.cmd = cmd
                self.returncode = None
                self.terminated = False
                self.killed = False

            def poll(self):
                return self.returncode

            def terminate(self) -> None:
                self.terminated = True
                self.returncode = 255
                output_path.write_bytes(b"mp4")

            def kill(self) -> None:
                self.killed = True
                self.returncode = -9

            def communicate(self, timeout=None):
                self.returncode = 0
                output_path.write_bytes(b"mp4")
                return "", "recorded"

        fake_proc_holder = {}

        def fake_popen(cmd: list[str], **kwargs):
            popen_calls.append(cmd)
            fake_proc_holder["proc"] = FakeProc(cmd)
            return fake_proc_holder["proc"]

        def run_metadata_or_poster(cmd: list[str], **_kwargs):
            run_calls.append(cmd)
            if cmd[1:] == ["-hide_banner", "-version"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="ffmpeg version 6.0\n", stderr="")
            if "-frames:v" in cmd:
                poster_path.write_bytes(b"poster")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            raise AssertionError(f"unexpected command: {cmd}")

        recording = self.mod.start_macos_window_video_recording(
            {"windowId": 88, "bounds": {"x": 10, "y": 20, "width": 320, "height": 200}},
            output_path,
            duration_secs=0.2,
            fps=5.0,
            popen_fn=fake_popen,
            run_fn=run_metadata_or_poster,
            ffmpeg_path="/opt/ffmpeg",
            input_device_fn=lambda **_kwargs: "3:",
            startup_grace_secs=0,
        )
        metadata = self.mod.stop_macos_window_video_recording(
            recording,
            output_path=output_path,
            metadata_path=metadata_path,
            poster_path=poster_path,
            duration_secs=0.2,
            fps=5.0,
            attachment_budget_bytes=1_000_000,
            desktop_video_metadata_fn=lambda path, **kwargs: {
                "path": str(path),
                "fps": kwargs["fps"],
                "command": kwargs["command"],
                "encoder": kwargs["encoder"],
                "size": {"fits_attachment_budget": True},
            },
            write_desktop_video_metadata_fn=lambda path, payload: path.write_text(str(payload)),
        )

        self.assertEqual(recording["mode"], "ffmpeg-avfoundation")
        self.assertEqual(popen_calls[0][0], "/opt/ffmpeg")
        self.assertIn("avfoundation", popen_calls[0])
        self.assertIn("crop=320:200:10:20,fps=5.0", popen_calls[0])
        self.assertEqual(metadata["mode"], "ffmpeg-avfoundation")
        self.assertEqual(metadata["returncode"], 0)
        self.assertFalse(metadata["terminated"])
        self.assertEqual(metadata["encoder"]["version"], "ffmpeg version 6.0")
        self.assertTrue(poster_path.exists())
        self.assertIn(["/opt/ffmpeg", "-hide_banner", "-version"], run_calls)

    def test_window_video_recording_can_include_system_audio_device(self) -> None:
        output_path = self.root / "video" / "proof.mp4"
        metadata_path = self.root / "video" / "metadata.json"
        poster_path = self.root / "video" / "poster.png"
        popen_calls: list[list[str]] = []
        metadata_kwargs: dict = {}

        class FakeProc:
            returncode = None

            def poll(self):
                return None

            def communicate(self, timeout=None):
                self.returncode = 0
                output_path.write_bytes(b"mp4")
                return "", "recorded"

        def fake_popen(cmd: list[str], **_kwargs):
            popen_calls.append(cmd)
            return FakeProc()

        def run_metadata_or_poster(cmd: list[str], **_kwargs):
            if cmd[1:] == ["-hide_banner", "-version"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="ffmpeg version 6.0\n", stderr="")
            if "-frames:v" in cmd:
                poster_path.write_bytes(b"poster")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            raise AssertionError(f"unexpected command: {cmd}")

        def video_metadata(path: Path, **kwargs):
            metadata_kwargs["kwargs"] = kwargs
            return {
                "path": str(path),
                "fps": kwargs["fps"],
                "command": kwargs["command"],
                "encoder": kwargs["encoder"],
                "has_audio": kwargs["has_audio"],
                "audio_source": kwargs["audio_source"],
                "size": {"fits_attachment_budget": True},
            }

        recording = self.mod.start_macos_window_video_recording(
            {"windowId": 88, "bounds": {"x": 10, "y": 20, "width": 320, "height": 200}},
            output_path,
            duration_secs=0.2,
            fps=5.0,
            popen_fn=fake_popen,
            run_fn=run_metadata_or_poster,
            ffmpeg_path="/opt/ffmpeg",
            input_device_fn=lambda **_kwargs: "3:",
            audio_input_device_fn=lambda _device: "2",
            audio_source="system",
            audio_device="2",
            startup_grace_secs=0,
        )
        metadata = self.mod.stop_macos_window_video_recording(
            recording,
            output_path=output_path,
            metadata_path=metadata_path,
            poster_path=poster_path,
            duration_secs=0.2,
            fps=5.0,
            attachment_budget_bytes=1_000_000,
            desktop_video_metadata_fn=video_metadata,
            write_desktop_video_metadata_fn=lambda path, payload: path.write_text(json.dumps(payload)),
        )

        self.assertIn("3:2", popen_calls[0])
        self.assertNotIn("-an", popen_calls[0])
        self.assertIn("-c:a", popen_calls[0])
        self.assertIn("aac", popen_calls[0])
        self.assertTrue(metadata_kwargs["kwargs"]["has_audio"])
        self.assertEqual(metadata_kwargs["kwargs"]["audio_source"], "system")
        self.assertEqual(metadata["audio_device"], "2")

    def test_system_audio_requires_explicit_device_and_no_frame_sequence_fallback(self) -> None:
        output_path = self.root / "video" / "proof.mp4"
        window = {"windowId": 88, "bounds": {"x": 10, "y": 20, "width": 320, "height": 200}}

        with self.assertRaisesRegex(RuntimeError, "requires an AVFoundation audio device"):
            self.mod.start_macos_window_video_recording(
                window,
                output_path,
                duration_secs=0.2,
                fps=5.0,
                popen_fn=lambda *_args, **_kwargs: self.fail("ffmpeg should not launch"),
                run_fn=lambda *_args, **_kwargs: subprocess.CompletedProcess([], 0, stdout="", stderr=""),
                ffmpeg_path="/opt/ffmpeg",
                input_device_fn=lambda **_kwargs: "3:",
                audio_input_device_fn=lambda _device: None,
                audio_source="system",
                startup_grace_secs=0,
            )

        with self.assertRaisesRegex(RuntimeError, "cannot use frame-sequence fallback"):
            self.mod.start_macos_window_video_recording(
                window,
                output_path,
                duration_secs=0.2,
                fps=5.0,
                audio_source="system",
                audio_device="2",
                prefer_frame_sequence=True,
            )

    def test_window_video_recording_rejects_blank_ffmpeg_poster(self) -> None:
        output_path = self.root / "video" / "proof.mp4"
        metadata_path = self.root / "video" / "metadata.json"
        poster_path = self.root / "video" / "poster.png"
        written: dict = {}

        class FakeProc:
            returncode = None

            def poll(self):
                return None

            def communicate(self, timeout=None):
                self.returncode = 0
                output_path.write_bytes(b"mp4")
                return "", "recorded"

        def fake_popen(cmd: list[str], **kwargs):
            return FakeProc()

        def run_metadata_or_poster(cmd: list[str], **_kwargs):
            if cmd[1:] == ["-hide_banner", "-version"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="ffmpeg version 6.0\n", stderr="")
            if "-frames:v" in cmd:
                write_rgb_png(poster_path, 2, 2, [(0, 0, 0)] * 4)
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            raise AssertionError(f"unexpected command: {cmd}")

        def write_metadata(path: Path, payload: dict) -> None:
            written.update(payload)
            path.write_text(json.dumps(payload))

        recording = self.mod.start_macos_window_video_recording(
            {"windowId": 88, "bounds": {"x": 10, "y": 20, "width": 320, "height": 200}},
            output_path,
            duration_secs=0.2,
            fps=5.0,
            popen_fn=fake_popen,
            run_fn=run_metadata_or_poster,
            ffmpeg_path="/opt/ffmpeg",
            input_device_fn=lambda **_kwargs: "3:",
            startup_grace_secs=0,
        )

        with self.assertRaisesRegex(RuntimeError, "poster appears blank"):
            self.mod.stop_macos_window_video_recording(
                recording,
                output_path=output_path,
                metadata_path=metadata_path,
                poster_path=poster_path,
                duration_secs=0.2,
                fps=5.0,
                attachment_budget_bytes=1_000_000,
                desktop_video_metadata_fn=lambda path, **kwargs: {
                    "path": str(path),
                    "fps": kwargs["fps"],
                    "command": kwargs["command"],
                    "encoder": kwargs["encoder"],
                    "size": {"fits_attachment_budget": True},
                },
                write_desktop_video_metadata_fn=write_metadata,
            )

        self.assertTrue(metadata_path.exists())
        self.assertTrue(written["poster"]["visual"]["appears_blank"])


if __name__ == "__main__":
    unittest.main()
