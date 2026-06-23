#!/usr/bin/env python3
"""Tests for the io_utils text/file/image helpers."""

import os
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

import io_utils


class IoUtilsDirectTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.mod = io_utils

    def tearDown(self):
        self.tmpdir.cleanup()

    def test_wait_for_path_returns_ready_path(self):
        ready = Path(self.tmpdir.name) / "ready.txt"
        ready.write_text("ok")

        self.assertEqual(self.mod.wait_for_path(ready, 0.1), ready)

    def test_wait_for_path_polls_until_ready(self):
        ready = Path(self.tmpdir.name) / "eventual.txt"
        current_time = [0.0]

        def time_fn():
            return current_time[0]

        def sleep_fn(_interval):
            current_time[0] += 0.1
            ready.write_text("ok")

        self.assertEqual(
            self.mod.wait_for_path(ready, 0.5, time_fn=time_fn, sleep_fn=sleep_fn),
            ready,
        )

    def test_wait_for_path_reports_timeout(self):
        missing = Path(self.tmpdir.name) / "missing.txt"
        current_time = [0.0]

        def time_fn():
            return current_time[0]

        def sleep_fn(interval):
            current_time[0] += interval

        with self.assertRaisesRegex(RuntimeError, "timed out waiting for artifact"):
            self.mod.wait_for_path(missing, 0.2, time_fn=time_fn, sleep_fn=sleep_fn)



class IoUtilsTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        root = Path(self.tmpdir.name)
        self.state_dir = root / "state"

        self.prev_home = os.environ.get("PULP_LOCAL_CI_HOME")
        os.environ["PULP_LOCAL_CI_HOME"] = str(self.state_dir)
        self.mod = io_utils

    def tearDown(self):
        if self.prev_home is None:
            os.environ.pop("PULP_LOCAL_CI_HOME", None)
        else:
            os.environ["PULP_LOCAL_CI_HOME"] = self.prev_home

        self.tmpdir.cleanup()


    def test_text_file_helpers_tail_trim_and_atomic_write(self):
        log_path = self.state_dir / "logs" / "job" / "mac.log"
        log_path.parent.mkdir(parents=True)
        log_path.write_text("one\ntwo\nthree\n")

        self.assertEqual(self.mod.tail_lines(self.state_dir / "missing.log"), [])
        self.assertEqual(self.mod.tail_lines(log_path, limit=2), ["two\n", "three\n"])

        target_path = self.state_dir / "results" / "result.txt"
        self.mod.atomic_write_text(target_path, "first\n")
        self.mod.atomic_write_text(target_path, "second\n")
        self.assertEqual(target_path.read_text(), "second\n")

        trimmed = self.mod.trim_line("  " + ("x" * 170) + "tail  ", max_len=12)
        self.assertEqual(len(trimmed), 12)
        self.assertTrue(trimmed.endswith("tail"))


    def test_image_change_summary_falls_back_to_hash_for_non_images(self):
        before_path = Path(self.tmpdir.name) / "before.bin"
        after_path = Path(self.tmpdir.name) / "after.bin"
        diff_path = Path(self.tmpdir.name) / "diff" / "image.png"
        before_path.write_bytes(b"same")
        after_path.write_bytes(b"same")

        unchanged = self.mod.image_change_summary(before_path, after_path, diff_output_path=diff_path)
        self.assertFalse(unchanged["changed"])
        self.assertEqual(unchanged["method"], "file-hash")
        self.assertFalse(diff_path.exists())

        after_path.write_bytes(b"different")
        changed = self.mod.image_change_summary(before_path, after_path, diff_output_path=diff_path)
        self.assertTrue(changed["changed"])
        self.assertEqual(changed["method"], "file-hash")
        self.assertFalse(diff_path.exists())

    def test_image_change_summary_uses_pixel_bbox_when_pillow_is_available(self):
        before_path = Path(self.tmpdir.name) / "before.png"
        after_path = Path(self.tmpdir.name) / "after.png"
        diff_path = Path(self.tmpdir.name) / "diff" / "image.png"
        before_path.write_bytes(b"before")
        after_path.write_bytes(b"after")

        class FakeImage:
            def convert(self, mode):
                self.mode = mode
                return self

        class FakeDiff:
            def __init__(self, bbox):
                self.bbox = bbox

            def save(self, path):
                path.write_text("diff\n")

            def getbbox(self):
                return self.bbox

        pil_pkg = type(sys)("PIL")
        image_mod = type(sys)("PIL.Image")
        chops_mod = type(sys)("PIL.ImageChops")
        image_mod.open = mock.Mock(return_value=FakeImage())
        chops_mod.difference = mock.Mock(return_value=FakeDiff((1, 2, 3, 4)))
        pil_pkg.Image = image_mod
        pil_pkg.ImageChops = chops_mod

        with mock.patch.dict(
            sys.modules,
            {
                "PIL": pil_pkg,
                "PIL.Image": image_mod,
                "PIL.ImageChops": chops_mod,
            },
        ):
            changed = self.mod.image_change_summary(
                before_path, after_path, diff_output_path=diff_path
            )

        self.assertTrue(changed["changed"])
        self.assertEqual(changed["method"], "pixel-bbox")
        self.assertEqual(
            changed["bbox"],
            {"left": 1, "top": 2, "right": 3, "bottom": 4},
        )
        self.assertEqual(diff_path.read_text(), "diff\n")

        chops_mod.difference = mock.Mock(return_value=FakeDiff(None))
        with mock.patch.dict(
            sys.modules,
            {
                "PIL": pil_pkg,
                "PIL.Image": image_mod,
                "PIL.ImageChops": chops_mod,
            },
        ):
            unchanged = self.mod.image_change_summary(before_path, after_path)

        self.assertFalse(unchanged["changed"])
        self.assertEqual(unchanged["method"], "pixel-bbox")
        self.assertNotIn("bbox", unchanged)

    def test_tail_lines_replaces_invalid_bytes_and_trim_exact_boundary(self):
        log_path = self.state_dir / "logs" / "invalid.log"
        log_path.parent.mkdir(parents=True)
        log_path.write_bytes(b"good\nbad:\xff\n")

        self.assertEqual(self.mod.tail_lines(log_path), ["good\n", "bad:\ufffd\n"])
        self.assertEqual(self.mod.trim_line(" abc ", max_len=3), "abc")
        self.assertEqual(self.mod.trim_line("abcdef", max_len=2), "…f")

    def test_file_lock_raises_busy_when_nonblocking_flock_fails(self):
        lock_path = self.state_dir / "queue.lock"

        with mock.patch.object(io_utils.fcntl, "flock", side_effect=BlockingIOError("busy")):
            with self.assertRaises(self.mod.LockBusyError):
                with self.mod.file_lock(lock_path, blocking=False):
                    pass

    def test_file_lock_yields_handle_and_releases(self):
        lock_path = self.state_dir / "queue.lock"

        with self.mod.file_lock(lock_path, blocking=True) as handle:
            self.assertFalse(handle.closed)
            handle.write("held")

        self.assertTrue(handle.closed)



if __name__ == "__main__":
    unittest.main()
