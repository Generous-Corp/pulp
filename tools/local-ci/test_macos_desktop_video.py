#!/usr/bin/env python3
"""No-network tests for macos_desktop_video.py (video-proof re-home of test_macos_desktop.py)."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import struct
import tempfile
import time
import unittest
import zlib


from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("macos_desktop_video.py", add_module_dir=True)


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


class MacosDesktopVideoTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

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

    def test_window_video_command_scales_crop_for_retina_capture(self) -> None:
        # AVFoundation captures Retina displays at 2x native pixels, so the crop
        # rect (point-space window bounds) must be doubled or it grabs the wrong
        # region. The probe reports the window's screen backing scale.
        window = {"bounds": {"x": 100, "y": 174, "width": 360, "height": 512}, "scale": 2.0}
        command = self.mod.macos_window_video_command(
            window,
            self.root / "video" / "proof.mp4",
            duration_secs=4.0,
            fps=15.0,
            ffmpeg_path="/opt/ffmpeg",
            input_device="1:",
        )
        self.assertIn("crop=720:1024:200:348,fps=15.0", command)

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
                "command": kwargs["command"],
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

    def test_frame_sequence_raises_target_window_to_keep_it_rendering(self) -> None:
        # An occluded window's render loop is paused by macOS, freezing the
        # capture on one frame; the recorder must raise the target window so it
        # keeps animating during a frame-sequence capture.
        output_path = self.root / "video" / "proof.mp4"
        activate_calls: list[int] = []

        def run_capture(cmd: list[str], **_kwargs):
            if cmd[0] == "screencapture":
                Path(cmd[-1]).write_bytes(b"png")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        def activate() -> None:
            activate_calls.append(1)

        recording = self.mod.start_macos_window_video_recording(
            {"windowId": 91, "bounds": {"x": 0, "y": 0, "width": 320, "height": 200}},
            output_path,
            duration_secs=0.2,
            fps=10.0,
            run_fn=run_capture,
            ffmpeg_path="/opt/ffmpeg",
            prefer_frame_sequence=True,
            activate_fn=activate,
        )
        time.sleep(0.25)
        recording["stop_event"].set()
        recording["thread"].join(timeout=2.0)
        # Raised at least once (before the first frame) and recorded in state.
        self.assertGreaterEqual(len(activate_calls), 1)
        self.assertGreaterEqual(int(recording["state"].get("activations", 0)), 1)

    def test_frame_sequence_without_activate_fn_still_captures(self) -> None:
        # Activation is optional; absence must not break capture.
        output_path = self.root / "video" / "proof.mp4"

        def run_capture(cmd: list[str], **_kwargs):
            if cmd[0] == "screencapture":
                Path(cmd[-1]).write_bytes(b"png")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        recording = self.mod.start_macos_window_video_recording(
            {"windowId": 92, "bounds": {"x": 0, "y": 0, "width": 320, "height": 200}},
            output_path,
            duration_secs=0.2,
            fps=10.0,
            run_fn=run_capture,
            ffmpeg_path="/opt/ffmpeg",
            prefer_frame_sequence=True,
        )
        time.sleep(0.25)
        recording["stop_event"].set()
        recording["thread"].join(timeout=2.0)
        self.assertGreaterEqual(int(recording["state"].get("frames", 0)), 1)
        self.assertEqual(int(recording["state"].get("activations", 0)), 0)

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
        self.assertIn("crop=320:200:10:20,scale=trunc(iw/2)*2:trunc(ih/2)*2", encode_command)
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
            {"windowId": 88, "bounds": {"x": 10, "y": 20, "width": 321, "height": 201}},
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
                "command": kwargs["command"],
                "encoder": kwargs["encoder"],
                "size": {"fits_attachment_budget": True},
            },
            write_desktop_video_metadata_fn=lambda path, payload: path.write_text(str(payload)),
        )

        self.assertEqual(recording["mode"], "screencapture-sequence")
        self.assertEqual(metadata["mode"], "screencapture-sequence")
        self.assertEqual(metadata["fallback_reason"], "window-id frame capture preferred")
        self.assertIn("-vf", metadata["command"])
        self.assertIn("scale=trunc(iw/2)*2:trunc(ih/2)*2", metadata["command"])
        self.assertTrue(any(cmd[:3] == ["screencapture", "-x", "-l"] for cmd in calls))
        self.assertFalse(any("avfoundation" in cmd for cmd in calls))

    def test_frame_sequence_failure_points_to_terminal_reentry(self) -> None:
        output_path = self.root / "video" / "proof.mp4"
        metadata_path = self.root / "video" / "metadata.json"

        def fail_screencapture(cmd: list[str], **_kwargs):
            if cmd[0] == "screencapture":
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="could not create image from display")
            raise AssertionError(f"unexpected command: {cmd}")

        recording = self.mod.start_macos_window_video_recording(
            {"windowId": 88, "bounds": {"x": 10, "y": 20, "width": 321, "height": 201}},
            output_path,
            duration_secs=0.05,
            fps=1.0,
            run_fn=fail_screencapture,
            ffmpeg_path="/opt/ffmpeg",
            input_device_fn=lambda **_kwargs: self.fail("AVFoundation should not be probed"),
            prefer_frame_sequence=True,
        )
        with self.assertRaisesRegex(RuntimeError, "--run-in-terminal"):
            self.mod.stop_macos_window_video_recording(
                recording,
                output_path=output_path,
                metadata_path=metadata_path,
                poster_path=None,
                duration_secs=0.05,
                fps=1.0,
                attachment_budget_bytes=1_000_000,
                desktop_video_metadata_fn=lambda path, **kwargs: {"path": str(path), **kwargs},
                write_desktop_video_metadata_fn=lambda path, payload: path.write_text(str(payload)),
            )

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
