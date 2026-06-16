#!/usr/bin/env python3
"""No-network tests for io_utils_design_parity (re-home of test_io_utils.py)."""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
from unittest.mock import patch


from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("io_utils_design_parity.py", add_module_dir=True)


class IoUtilsDesignParityTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.mod = load_module()

    def tearDown(self):
        self.tmpdir.cleanup()

    def test_design_parity_diff_summary_resizes_source_and_writes_outputs(self):
        source_path = Path(self.tmpdir.name) / "source.png"
        native_path = Path(self.tmpdir.name) / "native.png"
        diff_path = Path(self.tmpdir.name) / "out" / "diff.png"
        resized_path = Path(self.tmpdir.name) / "out" / "source-resized.png"
        source_path.write_bytes(b"source")
        native_path.write_bytes(b"native")

        class FakeImage:
            def __init__(self, size):
                self.size = size

            def convert(self, _mode):
                return self

            def resize(self, size, _resampling):
                return FakeImage(size)

            def save(self, path):
                Path(path).write_text(f"image {self.size}\n")

        class FakeDiff:
            def save(self, path):
                Path(path).write_text("diff\n")

            def getbbox(self):
                return (0, 1, 2, 3)

        class FakeBrightness:
            def __init__(self, image):
                self.image = image

            def enhance(self, _value):
                return self.image

        pil_pkg = type(sys)("PIL")
        image_mod = type(sys)("PIL.Image")
        chops_mod = type(sys)("PIL.ImageChops")
        enhance_mod = type(sys)("PIL.ImageEnhance")
        image_mod.Resampling = SimpleNamespace(LANCZOS="lanczos")
        image_mod.open = mock.Mock(side_effect=[FakeImage((1040, 720)), FakeImage((588, 460))])
        chops_mod.difference = mock.Mock(return_value=FakeDiff())
        enhance_mod.Brightness = FakeBrightness
        pil_pkg.Image = image_mod
        pil_pkg.ImageChops = chops_mod
        pil_pkg.ImageEnhance = enhance_mod

        with mock.patch.dict(
            sys.modules,
            {
                "PIL": pil_pkg,
                "PIL.Image": image_mod,
                "PIL.ImageChops": chops_mod,
                "PIL.ImageEnhance": enhance_mod,
            },
        ):
            summary = self.mod.design_parity_diff_summary(
                source_path,
                native_path,
                diff_output_path=diff_path,
                resized_source_output_path=resized_path,
                enhance_brightness=2.5,
            )

        self.assertTrue(summary["changed"])
        self.assertTrue(summary["resized_source"])
        self.assertEqual(summary["source_size"], {"width": 1040, "height": 720})
        self.assertEqual(summary["native_size"], {"width": 588, "height": 460})
        self.assertEqual(summary["bbox"], {"left": 0, "top": 1, "right": 2, "bottom": 3})
        self.assertEqual(diff_path.read_text(), "diff\n")
        self.assertEqual(resized_path.read_text(), "image (588, 460)\n")

    def test_design_parity_diff_summary_falls_back_to_stdlib_png(self):
        source_path = Path(self.tmpdir.name) / "source.png"
        native_path = Path(self.tmpdir.name) / "native.png"
        diff_path = Path(self.tmpdir.name) / "out" / "diff.png"
        resized_path = Path(self.tmpdir.name) / "out" / "source-resized.png"
        try:
            from PIL import Image
        except Exception as exc:
            self.skipTest(f"Pillow unavailable for fixture generation: {exc}")
        Image.new("RGB", (2, 1), (10, 20, 30)).save(source_path)
        Image.new("RGB", (1, 1), (20, 20, 30)).save(native_path)

        with mock.patch.dict(sys.modules, {"PIL": None}):
            summary = self.mod.design_parity_diff_summary(
                source_path,
                native_path,
                diff_output_path=diff_path,
                resized_source_output_path=resized_path,
                enhance_brightness=2.0,
            )

        self.assertTrue(summary["changed"])
        self.assertTrue(summary["resized_source"])
        self.assertEqual(summary["method"], "stdlib-png-resized-source-diff")
        self.assertEqual(summary["bbox"], {"left": 0, "top": 0, "right": 1, "bottom": 1})
        self.assertTrue(diff_path.is_file())
        self.assertTrue(resized_path.is_file())
        self.assertGreater(diff_path.stat().st_size, 0)



if __name__ == "__main__":
    unittest.main()
