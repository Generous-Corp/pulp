#!/usr/bin/env python3
"""Pure helper tests for tools/motion/visual/capture_sim_frames.py."""

from __future__ import annotations

import contextlib
import io
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.motion.visual import capture_sim_frames as cap


class FakeDiff:
    def __init__(self, values: list[float]) -> None:
        self.values = values

    def mean(self) -> float:
        return sum(self.values) / len(self.values)


class FakeArray:
    def __init__(self, rows: list[list[list[float]]]) -> None:
        self.rows = rows
        self.shape = (len(rows), len(rows[0]), len(rows[0][0]))

    def astype(self, dtype: str) -> "FakeArray":
        self.dtype = dtype
        return self

    def __getitem__(self, key: tuple[slice, slice]) -> "FakeArray":
        row_slice, col_slice = key
        return FakeArray([row[col_slice] for row in self.rows[row_slice]])

    def __sub__(self, other: "FakeArray") -> FakeDiff:
        values: list[float] = []
        for left_row, right_row in zip(self.rows, other.rows):
            for left_px, right_px in zip(left_row, right_row):
                for left, right in zip(left_px, right_px):
                    values.append(left - right)
        return FakeDiff(values)


class FakeNumpy:
    @staticmethod
    def abs(diff: FakeDiff) -> FakeDiff:
        return FakeDiff([abs(value) for value in diff.values])

    @staticmethod
    def asarray(image: object) -> tuple[str, object]:
        return "array", image


class FakeOpenedImage:
    def convert(self, mode: str) -> "FakeOpenedImage":
        self.mode = mode
        return self


class FakeImageModule:
    opened: list[str] = []
    fromarray_calls: list[object] = []
    saved: list[tuple[str, str | None]] = []

    @classmethod
    def open(cls, path: str) -> FakeOpenedImage:
        cls.opened.append(path)
        return FakeOpenedImage()

    @classmethod
    def fromarray(cls, array: object) -> "FakeImageModule":
        cls.fromarray_calls.append(array)
        return cls()

    def save(self, path: str, format: str | None = None) -> None:
        self.saved.append((path, format))
        Path(path).write_bytes(b"png")


class MotionVisualCaptureSimFramesTests(unittest.TestCase):
    def setUp(self) -> None:
        FakeImageModule.opened = []
        FakeImageModule.fromarray_calls = []
        FakeImageModule.saved = []

    def test_load_deps_reports_missing_optional_dependency(self) -> None:
        real_import = __import__

        def fake_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "numpy":
                raise ImportError("numpy unavailable")
            return real_import(name, *args, **kwargs)

        stderr = io.StringIO()
        with mock.patch("builtins.__import__", side_effect=fake_import):
            with contextlib.redirect_stderr(stderr):
                self.assertIsNone(cap._load_deps())
        self.assertIn("missing dependency", stderr.getvalue())
        self.assertIn("tools/motion/visual/requirements.txt", stderr.getvalue())

    def test_load_deps_returns_numpy_and_image_on_success(self) -> None:
        real_import = __import__
        fake_np = object()
        fake_image = object()

        def fake_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "numpy":
                return fake_np
            if name == "PIL":
                return types.SimpleNamespace(Image=fake_image)
            return real_import(name, *args, **kwargs)

        with mock.patch("builtins.__import__", side_effect=fake_import):
            self.assertEqual(cap._load_deps(), (fake_np, fake_image))

    def test_which_and_source_availability_cover_supported_backends(self) -> None:
        with mock.patch.object(cap.shutil, "which", side_effect=lambda name: f"/bin/{name}"):
            self.assertEqual(cap._which_or_none("xcrun"), "/bin/xcrun")
            self.assertTrue(cap._source_available("macos", "1,2,3,4"))
            self.assertTrue(cap._source_available("simulator", None))

        with mock.patch.object(cap.shutil, "which", return_value=None):
            self.assertIsNone(cap._which_or_none("missing"))
            self.assertFalse(cap._source_available("macos", "1,2,3,4"))
            self.assertFalse(cap._source_available("simulator", None))

        with mock.patch.object(cap.shutil, "which", return_value="/bin/screencapture"):
            self.assertFalse(cap._source_available("macos", None))
            self.assertFalse(cap._source_available("other", "1,2,3,4"))

    def test_capture_macos_requires_tool_and_nonempty_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "frame.png"
            with mock.patch.object(cap, "_which_or_none", return_value=None):
                self.assertFalse(cap._capture_macos(dest, "1,2,3,4"))

            def write_png(args: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
                Path(args[-1]).write_bytes(b"png")
                return subprocess.CompletedProcess(args, 0)

            with mock.patch.object(cap, "_which_or_none", return_value="/usr/sbin/screencapture"):
                with mock.patch.object(cap.subprocess, "run", side_effect=write_png) as run:
                    self.assertTrue(cap._capture_macos(dest, "1,2,3,4"))
            run.assert_called_once()
            self.assertEqual(run.call_args.args[0][:4], ["/usr/sbin/screencapture", "-x", "-R", "1,2,3,4"])

    def test_capture_simulator_requires_tool_and_nonempty_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "sim.png"
            with mock.patch.object(cap, "_which_or_none", return_value=None):
                self.assertFalse(cap._capture_simulator(dest))

            def write_png(args: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
                Path(args[-1]).write_bytes(b"png")
                return subprocess.CompletedProcess(args, 0)

            with mock.patch.object(cap, "_which_or_none", return_value="/usr/bin/xcrun"):
                with mock.patch.object(cap.subprocess, "run", side_effect=write_png) as run:
                    self.assertTrue(cap._capture_simulator(dest))
            self.assertEqual(
                run.call_args.args[0][:5],
                ["/usr/bin/xcrun", "simctl", "io", "booted", "screenshot"],
            )

    def test_mean_diff_crops_mismatched_shapes_before_abs_mean(self) -> None:
        prev_arr = FakeArray([
            [[10, 20, 30], [40, 50, 60]],
            [[70, 80, 90], [100, 110, 120]],
        ])
        next_arr = FakeArray([
            [[5, 15, 25], [30, 45, 55], [99, 99, 99]],
        ])

        self.assertAlmostEqual(cap._mean_diff(prev_arr, next_arr, FakeNumpy), 35.0 / 6.0)

    def test_load_array_converts_image_to_rgb_before_numpy_array(self) -> None:
        result = cap._load_array(Path("frame.png"), FakeNumpy, FakeImageModule)

        self.assertEqual(FakeImageModule.opened, ["frame.png"])
        self.assertEqual(result[0], "array")
        self.assertEqual(result[1].mode, "RGB")

    def test_capture_returns_dependency_and_unavailable_source_statuses(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stderr = io.StringIO()
            with mock.patch.object(cap, "_load_deps", return_value=None):
                self.assertEqual(cap.capture("macos", Path(tmp), bounds="1,2,3,4"), 3)

            with mock.patch.object(cap, "_load_deps", return_value=(FakeNumpy, FakeImageModule)):
                with mock.patch.object(cap, "_source_available", return_value=False):
                    with contextlib.redirect_stderr(stderr):
                        self.assertEqual(cap.capture("macos", Path(tmp), bounds=None), 3)
            self.assertIn("source `macos` unavailable", stderr.getvalue())

    def test_capture_reports_initial_grab_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stderr = io.StringIO()
            with mock.patch.object(cap, "_load_deps", return_value=(FakeNumpy, FakeImageModule)):
                with mock.patch.object(cap, "_source_available", return_value=True):
                    with mock.patch.object(cap, "_capture_macos", return_value=False):
                        with contextlib.redirect_stderr(stderr):
                            status = cap.capture("macos", Path(tmp), bounds="1,2,3,4", idle_timeout_s=0.01)

        self.assertEqual(status, 3)
        self.assertIn("before any frames captured", stderr.getvalue())

    def test_capture_reports_partial_mid_run_failure_after_saved_frame(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stderr = io.StringIO()
            with mock.patch.object(cap, "_load_deps", return_value=(FakeNumpy, FakeImageModule)):
                with mock.patch.object(cap, "_source_available", return_value=True):
                    with mock.patch.object(cap, "_capture_macos", side_effect=[True, True, False]):
                        with mock.patch.object(cap, "_load_array", side_effect=["first", "moving"]):
                            with mock.patch.object(cap, "_mean_diff", return_value=9.0):
                                with mock.patch.object(cap.time, "sleep"):
                                    with contextlib.redirect_stderr(stderr):
                                        status = cap.capture(
                                            "macos",
                                            Path(tmp),
                                            frame_count=2,
                                            gate_threshold=4.0,
                                            bounds="1,2,3,4",
                                        )

        self.assertEqual(status, 4)
        self.assertIn("mid-run", stderr.getvalue())
        self.assertEqual(FakeImageModule.saved, [(str(Path(tmp) / "frame_0000.png"), "PNG")])

    def test_capture_saves_motion_frames_and_skips_exact_duplicates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stdout = io.StringIO()
            with mock.patch.object(cap, "_load_deps", return_value=(FakeNumpy, FakeImageModule)):
                with mock.patch.object(cap, "_source_available", return_value=True):
                    with mock.patch.object(cap, "_capture_simulator", side_effect=[True, True, True]):
                        with mock.patch.object(cap, "_load_array", side_effect=["base", "move", "still"]):
                            with mock.patch.object(cap, "_mean_diff", side_effect=[8.0, 0.0, 8.0, 8.0]):
                                with mock.patch.object(cap.time, "sleep"):
                                    with contextlib.redirect_stdout(stdout):
                                        status = cap.capture(
                                            "simulator",
                                            Path(tmp),
                                            frame_count=2,
                                            gate_threshold=4.0,
                                        )

        self.assertEqual(status, 0)
        self.assertIn("saved 2 frames", stdout.getvalue())
        self.assertEqual(
            FakeImageModule.saved,
            [
                (str(Path(tmp) / "frame_0000.png"), "PNG"),
                (str(Path(tmp) / "frame_0001.png"), "PNG"),
            ],
        )

    def test_capture_times_out_when_motion_gate_never_opens(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stderr = io.StringIO()
            with mock.patch.object(cap, "_load_deps", return_value=(FakeNumpy, FakeImageModule)):
                with mock.patch.object(cap, "_source_available", return_value=True):
                    with mock.patch.object(cap, "_capture_simulator", return_value=True):
                        with mock.patch.object(cap, "_load_array", side_effect=["a", "b"]):
                            with mock.patch.object(cap, "_mean_diff", return_value=0.5):
                                with mock.patch.object(cap.time, "monotonic", side_effect=[0.0, 0.0, 2.0]):
                                    with mock.patch.object(cap.time, "sleep"):
                                        with contextlib.redirect_stderr(stderr):
                                            status = cap.capture(
                                                "simulator",
                                                Path(tmp),
                                                frame_count=3,
                                                gate_threshold=4.0,
                                                idle_timeout_s=1.0,
                                            )

        self.assertEqual(status, 3)
        self.assertIn("motion gate never opened", stderr.getvalue())

    def test_main_validates_macos_bounds_before_capture(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            status = cap.main(["--source", "macos", "--output-dir", "frames"])

        self.assertEqual(status, 2)
        self.assertIn("--bounds X,Y,W,H is required", stderr.getvalue())

    def test_main_forwards_cli_options_to_capture(self) -> None:
        with mock.patch.object(cap, "capture", return_value=0) as capture:
            self.assertEqual(
                cap.main([
                    "--source", "simulator",
                    "--output-dir", "frames",
                    "--fps", "24",
                    "--frame-count", "12",
                    "--gate-threshold", "3.5",
                    "--gate-consecutive", "2",
                    "--idle-timeout", "7",
                ]),
                0,
            )

        capture.assert_called_once_with(
            source="simulator",
            output_dir=Path("frames"),
            fps=24.0,
            frame_count=12,
            gate_threshold=3.5,
            gate_consecutive=2,
            idle_timeout_s=7.0,
            bounds=None,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
