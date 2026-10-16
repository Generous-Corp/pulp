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

BACKGROUND_SCRIPT = Path(__file__).with_name("viewport_background_diagnostic.py")
BACKGROUND_SPEC = importlib.util.spec_from_file_location(
    "viewport_background_diagnostic", BACKGROUND_SCRIPT)
assert BACKGROUND_SPEC is not None and BACKGROUND_SPEC.loader is not None
background = importlib.util.module_from_spec(BACKGROUND_SPEC)
BACKGROUND_SPEC.loader.exec_module(background)


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


class FakeRGBAImage:
    def __init__(self, pixels: list[tuple[int, int, int, int]], size: tuple[int, int]) -> None:
        self._pixels = pixels
        self.size = size

    def getdata(self) -> list[tuple[int, int, int, int]]:
        return self._pixels


class FakeViewportImage:
    def __init__(self, pixels: list[tuple[int, int, int, int]], size: tuple[int, int]) -> None:
        self._pixels = pixels
        self.size = size
        self.width, self.height = size

    def convert(self, _mode: str) -> "FakeViewportImage":
        return self

    def getdata(self) -> list[tuple[int, int, int, int]]:
        return self._pixels

    def getpixel(self, point: tuple[int, int]) -> tuple[int, int, int, int]:
        x, y = point
        return self._pixels[y * self.width + x]


class HelperScoringTests(unittest.TestCase):
    @staticmethod
    def _panel_pixels(*, shift: int = 0, fill: tuple[int, int, int] = (220, 220, 220),
                      border_width: int = 2, rounded: bool = False) -> FakeImage:
        """Build a deterministic panel for strict planted-negative controls."""
        width, height = 64, 48
        background = (24, 28, 36)
        pixels = [background] * (width * height)
        left, top, right, bottom = 14 + shift, 10, 49 + shift, 37
        for y in range(top, bottom + 1):
            for x in range(left, right + 1):
                corner = rounded and (
                    (x - left < 4 and y - top < 4)
                    or (right - x < 4 and y - top < 4)
                    or (x - left < 4 and bottom - y < 4)
                    or (right - x < 4 and bottom - y < 4)
                )
                if corner:
                    continue
                edge = (x - left < border_width or right - x < border_width
                        or y - top < border_width or bottom - y < border_width)
                pixels[y * width + x] = (120, 130, 145) if edge else fill
        return FakeImage(pixels, size=(width, height))

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

    def test_strict_floor_controls_keep_healthy_edge_variance_advisory(self) -> None:
        reference = self._panel_pixels()
        healthy = list(reference._pixels)
        # A small edge-only tonal change models a renderer AA disagreement. It
        # must remain inside the strict floor while still being visible to the
        # residual profiler; it never relaxes the production threshold.
        for x in range(14, 50):
            for y in (10, 37):
                index = y * 64 + x
                red, green, blue = healthy[index]
                healthy[index] = (min(255, red + 12), min(255, green + 12), min(255, blue + 12))
        healthy_image = FakeImage(healthy, size=(64, 48))
        self.assertGreaterEqual(diff.similarity_score(reference, healthy_image), 0.995)
        self.assertGreater(diff.residual_profile(reference, healthy_image)["differing_pixels"], 0)

    def test_all_gate_planted_negatives_fail_strict_floor(self) -> None:
        reference = self._panel_pixels()
        controls = {
            "one-pixel-shift": self._panel_pixels(shift=1),
            "color-change": self._panel_pixels(fill=(90, 220, 160)),
            "missing-border": self._panel_pixels(border_width=0),
            "wrong-weight": self._panel_pixels(border_width=4),
            "corner-radius": self._panel_pixels(rounded=True),
        }
        for name, candidate in controls.items():
            with self.subTest(control=name):
                self.assertLess(diff.similarity_score(reference, candidate), 0.995)
                profile = diff.residual_profile(reference, candidate)
                self.assertGreater(profile["differing_pixels"], 0)

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

    def test_residual_profile_detects_alpha_only_changes(self) -> None:
        reference = FakeRGBAImage([(30, 40, 50, 255)] * 4, (2, 2))
        candidate = FakeRGBAImage([(30, 40, 50, 128)] * 4, (2, 2))
        result = diff.residual_profile(reference, candidate)
        self.assertEqual(result["classification"], "alpha-only")
        self.assertEqual(result["differing_pixels"], 4)
        self.assertEqual(result["alpha_only_pixels"], 4)

    def test_residual_profile_detects_small_translation(self) -> None:
        white = (255, 255, 255, 255)
        black = (0, 0, 0, 255)
        reference = [white] * 32
        candidate = [white] * 32
        for y in range(4):
            reference[y * 8 + 2] = black
            candidate[y * 8 + 3] = black
        result = diff.residual_profile(
            FakeRGBAImage(reference, (8, 4)), FakeRGBAImage(candidate, (8, 4)), shift_radius=2)
        self.assertEqual(result["classification"], "placement-drift")
        self.assertEqual(result["shift_probe"]["best_shift"]["dx"], 1)
        self.assertGreater(result["shift_probe"]["improvement"], 0.5)

    def test_residual_profile_identity_and_rgb_inputs_are_supported(self) -> None:
        image = FakeRGBAImage([(30, 40, 50, 255)] * 4, (2, 2))
        result = diff.residual_profile(image, image)
        self.assertEqual(result["classification"], "identical")
        self.assertEqual(result["differing_fraction"], 0.0)

    def test_residual_profile_identifies_edge_concentrated_raster_difference(self) -> None:
        white = (255, 255, 255, 255)
        reference = [white] * 32
        candidate = [white] * 32
        for y in range(4):
            reference[y * 8 + 2] = (0, 0, 0, 255)
            candidate[y * 8 + 2] = (96, 96, 96, 255)
        result = diff.residual_profile(
            FakeRGBAImage(reference, (8, 4)), FakeRGBAImage(candidate, (8, 4)))
        self.assertEqual(result["classification"], "text-edge/rasterization")
        self.assertEqual(result["edge_band_pixels"], result["differing_pixels"])

    def test_viewport_background_diagnostic_records_outside_root_pixels(self) -> None:
        reference = FakeViewportImage([(10, 20, 30, 255)] * 16, (4, 4))
        candidate_pixels = [(10, 20, 30, 255)] * 16
        candidate_pixels[0] = candidate_pixels[15] = (99, 88, 77, 255)
        candidate = FakeViewportImage(candidate_pixels, (4, 4))
        result = background.measure(reference, candidate, (1, 1, 2, 2))
        self.assertEqual(result["schema"], "pulp-viewport-background-diagnostic-v1")
        self.assertEqual(result["corners"]["top_left"]["candidate"], [99, 88, 77])
        self.assertEqual(result["outside_root"]["differing_pixels"], 2)

    def test_viewport_background_diagnostic_excludes_inside_root_pixels(self) -> None:
        reference = FakeViewportImage([(10, 20, 30, 255)] * 16, (4, 4))
        candidate_pixels = [(10, 20, 30, 255)] * 16
        # (1, 1) is inside the declared root (1, 1, 2, 2).  This planted
        # negative proves the probe does not classify every mismatch as
        # outside-root background registration.
        candidate_pixels[1 + 1 * 4] = (99, 88, 77, 255)
        candidate = FakeViewportImage(candidate_pixels, (4, 4))
        result = background.measure(reference, candidate, (1, 1, 2, 2))
        self.assertEqual(result["outside_root"]["differing_pixels"], 0)

    def test_viewport_background_diagnostic_rejects_size_mismatch(self) -> None:
        reference = FakeViewportImage([(0, 0, 0, 255)] * 16, (4, 4))
        candidate = FakeViewportImage([(0, 0, 0, 255)] * 15, (3, 5))
        with self.assertRaisesRegex(ValueError, "image dimensions differ"):
            background.measure(reference, candidate, (0, 0, 2, 2))

    def test_viewport_background_diagnostic_rejects_out_of_bounds_root(self) -> None:
        image = FakeViewportImage([(0, 0, 0, 255)] * 16, (4, 4))
        with self.assertRaises(ValueError):
            background.measure(image, image, (0, 0, 5, 4))


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
        written = None
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

    def test_main_writes_residual_profile_without_changing_score(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        score_ref = FakeImage([(255, 255, 255)])
        score_cand = FakeImage([(255, 255, 255)])
        residual_ref = FakeRGBAImage([(30, 40, 50, 255)] * 4, (2, 2))
        residual_cand = FakeRGBAImage([(30, 40, 50, 128)] * 4, (2, 2))
        with tempfile.TemporaryDirectory() as tdir:
            root = Path(tdir)
            ref_path = root / "ref.png"
            cand_path = root / "cand.png"
            residual_path = root / "residual.json"
            ref_path.write_text("ref", encoding="utf-8")
            cand_path.write_text("cand", encoding="utf-8")
            with mock.patch.object(
                sys,
                "argv",
                [str(SCRIPT), str(ref_path), str(cand_path), "--json", "--residual-json", str(residual_path)],
            ), mock.patch.object(
                diff,
                "load_normalized",
                side_effect=[score_ref, score_cand, residual_ref, residual_cand],
            ), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = diff.main()
            written = json.loads(residual_path.read_text(encoding="utf-8"))
        self.assertEqual(rc, 0)
        self.assertEqual(stderr.getvalue(), "")
        payload = json.loads(stdout.getvalue())
        self.assertEqual(payload["score"], 1.0)
        self.assertEqual(payload["residual_profile"]["classification"], "alpha-only")
        assert written is not None
        self.assertEqual(written["alpha_only_pixels"], 4)

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
