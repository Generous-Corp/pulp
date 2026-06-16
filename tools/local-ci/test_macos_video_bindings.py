#!/usr/bin/env python3
"""Tests for macOS video-proof facade dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest


def load_module():
    return load_local_ci_module("macos_video_bindings.py")


class MacosVideoBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_install_registers_all_exports(self):
        bindings: dict = {}
        self.mod.install_macos_video_helpers(bindings)
        for name in self.mod.MACOS_VIDEO_EXPORTS:
            self.assertTrue(callable(bindings[name]), name)

    def test_install_subset_registers_named_only(self):
        bindings: dict = {}
        self.mod.install_macos_video_helpers(bindings, ("start_macos_window_video_recording",))
        self.assertTrue(callable(bindings["start_macos_window_video_recording"]))
        self.assertNotIn("stop_macos_window_video_recording", bindings)

    def test_start_recording_threads_subprocess_and_ffmpeg(self):
        captured = {}

        def fake_start(window, output_path, **kwargs):
            captured.update(kwargs)
            captured["window"] = window
            return {"ok": True}

        bindings = {
            "_macos_desktop_video": types.SimpleNamespace(start_macos_window_video_recording=fake_start),
            "subprocess": types.SimpleNamespace(run="RUN", Popen="POPEN"),
            "resolve_ffmpeg_path": lambda: "/usr/bin/ffmpeg",
        }

        result = self.mod.start_macos_window_video_recording(
            bindings,
            {"windowId": 1},
            "/tmp/proof.mp4",
            duration_secs=4.0,
            fps=30.0,
        )

        self.assertEqual(result, {"ok": True})
        self.assertEqual(captured["popen_fn"], "POPEN")
        self.assertEqual(captured["run_fn"], "RUN")
        self.assertEqual(captured["ffmpeg_path"], "/usr/bin/ffmpeg")

    def test_terminal_proof_threads_run_fn(self):
        captured = {}

        def fake_launch(args, **kwargs):
            captured.update(kwargs)
            captured["args"] = args
            return {"pid": 5}

        bindings = {
            "_macos_terminal_proof": types.SimpleNamespace(launch_macos_terminal_proof_command=fake_launch),
            "subprocess": types.SimpleNamespace(run="RUN"),
        }
        from pathlib import Path

        result = self.mod.launch_macos_terminal_proof_command(
            bindings,
            ["./app"],
            cwd=Path("/tmp"),
            title="Pulp Video Proof",
            stdout_path=Path("/tmp/out"),
            stderr_path=Path("/tmp/err"),
            returncode_path=Path("/tmp/rc"),
            keepalive_secs=6.0,
        )

        self.assertEqual(result, {"pid": 5})
        self.assertEqual(captured["run_fn"], "RUN")
        self.assertEqual(captured["args"], ["./app"])


if __name__ == "__main__":
    unittest.main()
