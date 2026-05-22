#!/usr/bin/env python3
"""Unit tests for diff_against_reference.py helper scoring and CLI flow."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("diff_against_reference.py")
SPEC = importlib.util.spec_from_file_location("diff_against_reference", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
diff = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = diff
SPEC.loader.exec_module(diff)


class FakeImage:
    def __init__(
        self,
        pixels: list[tuple[int, int, int]],
        size: tuple[int, int] | None = None,
        histogram: list[int] | None = None,
    ) -> None:
        self._pixels = pixels
        self.size = size or (len(pixels), 1)
        self._histogram = histogram
        self.resized_to: tuple[int, int] | None = None
        self.converted_to: str | None = None

    def getdata(self) -> list[tuple[int, int, int]]:
        return self._pixels

    def histogram(self) -> list[int]:
        if self._histogram is not None:
            return self._histogram
        hist = [0] * 768
        for r, g, b in self._pixels:
            hist[r] += 1
            hist[256 + g] += 1
            hist[512 + b] += 1
        return hist

    def convert(self, mode: str) -> "FakeImage":
        self.converted_to = mode
        return self

    def resize(self, target: tuple[int, int], _resampling: object = None) -> "FakeImage":
        self.resized_to = target
        self.size = target
        return self


class HelperScoringTests(unittest.TestCase):
    def test_image_module_imports_pillow_or_reports_install_hint(self) -> None:
        def fake_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "PIL":
                return mock.Mock(Image="fake-image-module")
            return __import__(name, *args, **kwargs)

        with mock.patch("builtins.__import__", side_effect=fake_import):
            self.assertEqual(diff._image_module(), "fake-image-module")
        with mock.patch("builtins.__import__", side_effect=ImportError):
            with self.assertRaisesRegex(RuntimeError, "PIL/Pillow required"):
                diff._image_module()

    def test_histogram_similarity_handles_identity_and_empty_norm(self) -> None:
        a = FakeImage([(0, 0, 0)], histogram=[1, 2, 3])
        self.assertAlmostEqual(diff.histogram_similarity(a, a), 1.0)
        self.assertEqual(diff.histogram_similarity(FakeImage([], histogram=[]), a), 0.0)

    def test_mean_pixel_distance_scores_identical_different_and_mismatched_lengths(self) -> None:
        white = FakeImage([(255, 255, 255)])
        black = FakeImage([(0, 0, 0)])
        self.assertEqual(diff.mean_pixel_distance(white, white), 1.0)
        self.assertLess(diff.mean_pixel_distance(white, black), 0.001)
        self.assertEqual(diff.mean_pixel_distance(white, FakeImage([])), 0.0)
        self.assertGreater(diff.mean_pixel_distance(white, FakeImage([(128, 128, 128)])), 0.49)
        self.assertLess(diff.mean_pixel_distance(white, FakeImage([(128, 128, 128)])), 0.51)

    def test_blank_detection_uses_near_black_ratio(self) -> None:
        self.assertTrue(diff.is_blank(FakeImage([(0, 0, 0)] * 96 + [(255, 255, 255)] * 4)))
        self.assertFalse(diff.is_blank(FakeImage([(0, 0, 0)] * 94 + [(255, 255, 255)] * 6)))

    def test_dominant_colors_are_quantized_and_sorted(self) -> None:
        img = FakeImage([(33, 63, 95), (34, 64, 96), (250, 250, 250)])
        colors = diff.dominant_colors(img, k=2)
        self.assertEqual(colors, [(32, 32, 64), (32, 64, 96)])
        self.assertEqual(len(colors), 2)
        self.assertNotIn((224, 224, 224), colors)

    def test_load_normalized_converts_and_resizes_when_needed(self) -> None:
        opened = FakeImage([(1, 2, 3)], size=(4, 4))
        fake_module = mock.Mock()
        fake_module.open.return_value = opened
        fake_module.Resampling.LANCZOS = object()
        with mock.patch.object(diff, "_image_module", return_value=fake_module):
            result = diff.load_normalized(Path("image.png"), (8, 8))
        self.assertIs(result, opened)
        self.assertEqual(opened.converted_to, "RGB")
        self.assertEqual(opened.resized_to, (8, 8))
        fake_module.open.assert_called_once_with(Path("image.png"))
        self.assertEqual(result.size, (8, 8))

    def test_load_normalized_skips_resize_when_size_matches(self) -> None:
        opened = FakeImage([(1, 2, 3)], size=(8, 8))
        fake_module = mock.Mock()
        fake_module.open.return_value = opened
        with mock.patch.object(diff, "_image_module", return_value=fake_module):
            result = diff.load_normalized(Path("image.png"), (8, 8))
        self.assertIs(result, opened)
        self.assertIsNone(opened.resized_to)
        self.assertEqual(opened.converted_to, "RGB")


class MainFlowTests(unittest.TestCase):
    def _run_main(
        self,
        argv: list[str],
        *,
        ref: FakeImage | None = None,
        cand: FakeImage | None = None,
    ) -> tuple[int, str, str]:
        ref_img = ref or FakeImage([(255, 255, 255)])
        cand_img = cand or FakeImage([(255, 255, 255)])
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as tdir:
            root = Path(tdir)
            ref_path = root / "ref.png"
            cand_path = root / "cand.png"
            ref_path.write_text("ref", encoding="utf-8")
            cand_path.write_text("cand", encoding="utf-8")
            args = [str(ref_path), str(cand_path), *argv]
            with mock.patch.object(sys, "argv", [str(SCRIPT), *args]), \
                 mock.patch.object(diff, "load_normalized", side_effect=[ref_img, cand_img]), \
                 contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                rc = diff.main()
        return rc, stdout.getvalue(), stderr.getvalue()

    def test_main_prints_pass_verdict_for_matching_images(self) -> None:
        rc, stdout, stderr = self._run_main(["--threshold", "0.5"])
        self.assertEqual(rc, 0)
        self.assertIn("PASS", stdout)
        self.assertEqual(stderr, "")

    def test_main_fails_blank_candidate_even_when_pixels_match(self) -> None:
        blank = FakeImage([(0, 0, 0)] * 100)
        rc, stdout, _stderr = self._run_main([], ref=blank, cand=blank)
        self.assertEqual(rc, 1)
        self.assertIn("tier=blank", stdout)
        self.assertIn("candidate is essentially blank", stdout)
        self.assertIn("FAIL", stdout)
        self.assertIn("ref dominant colors", stdout)

    def test_main_json_output_contains_metrics_and_diagnosis(self) -> None:
        rc, stdout, _stderr = self._run_main(["--json", "--threshold", "0.99"])
        payload = json.loads(stdout)
        self.assertEqual(rc, 0)
        self.assertTrue(payload["passed"])
        self.assertEqual(payload["tier"], "pass")
        self.assertEqual(payload["score"], 1.0)
        self.assertEqual(payload["threshold"], 0.99)
        self.assertFalse(payload["blank_candidate"])

    def test_main_reports_missing_file_before_loading_images(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [str(SCRIPT), "missing-ref.png", "missing-cand.png"]), \
             mock.patch.object(diff, "load_normalized") as load_normalized, \
             contextlib.redirect_stdout(stdout), \
             contextlib.redirect_stderr(stderr):
            rc = diff.main()
        self.assertEqual(rc, 2)
        self.assertIn("file not found", stderr.getvalue())
        self.assertEqual(stdout.getvalue(), "")
        load_normalized.assert_not_called()

    def test_main_rejects_malformed_size(self) -> None:
        rc, _stdout, stderr = self._run_main(["--size", "12-by-8"])
        self.assertEqual(rc, 2)
        self.assertIn("malformed --size", stderr)

    def test_main_reports_load_errors_as_compare_errors(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as tdir:
            root = Path(tdir)
            ref_path = root / "ref.png"
            cand_path = root / "cand.png"
            ref_path.write_text("ref", encoding="utf-8")
            cand_path.write_text("cand", encoding="utf-8")
            with mock.patch.object(sys, "argv", [str(SCRIPT), str(ref_path), str(cand_path)]), \
                 mock.patch.object(diff, "load_normalized", side_effect=RuntimeError("bad image")), \
                 contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                rc = diff.main()
        self.assertEqual(rc, 2)
        self.assertIn("failed to load images", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
