#!/usr/bin/env python3
"""Unit tests for diff_against_reference_regions.py — schema validation and
--strict flag semantics.

Pins the Codex P2 contract on PR #1871:
  - load_regions() validates region type/coord values upfront and exits
    2 with a clear message before any scoring runs.
  - --strict flag is actually consulted; without it region misses are
    informational and the script exits 0.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

THIS_DIR = Path(__file__).resolve().parent
SCRIPT = THIS_DIR / "diff_against_reference_regions.py"
SPEC = importlib.util.spec_from_file_location("diff_against_reference_regions", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
regions_mod = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = regions_mod
SPEC.loader.exec_module(regions_mod)


def _run(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def _make_png(path: Path, color: tuple[int, int, int]) -> None:
    """Tiny PNG via PIL — keeps tests self-contained."""
    from PIL import Image  # type: ignore

    img = Image.new("RGB", (32, 32), color)
    img.save(path)


try:
    import PIL  # noqa: F401

    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


class TestSchemaValidation(unittest.TestCase):
    """load_regions() upfront schema validation."""

    def _write_regions(self, tmp: Path, data: object) -> Path:
        p = tmp / "regions.json"
        p.write_text(json.dumps(data))
        return p

    def test_non_dict_region_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(tmp, {"r1": "not a dict"})
            ref = tmp / "ref.png"
            cand = tmp / "cand.png"
            ref.write_text("placeholder")
            cand.write_text("placeholder")
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertIn("r1", result.stderr)
            self.assertIn("object", result.stderr)

    def test_non_numeric_coord_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(
                tmp,
                {"r1": {"x": "0.1", "y": 0.0, "w": 0.5, "h": 0.5}},
            )
            ref = tmp / "ref.png"
            cand = tmp / "cand.png"
            ref.write_text("placeholder")
            cand.write_text("placeholder")
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertIn("non-numeric", result.stderr)

    def test_bool_coord_rejected(self) -> None:
        # bool is an int subclass; remote fix rejects it explicitly so
        # `"x": true` doesn't silently become 1.
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(
                tmp,
                {"r1": {"x": True, "y": 0.0, "w": 0.5, "h": 0.5}},
            )
            ref = tmp / "ref.png"
            cand = tmp / "cand.png"
            ref.write_text("placeholder")
            cand.write_text("placeholder")
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertIn("non-numeric", result.stderr)

    def test_invalid_threshold_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(
                tmp,
                {
                    "r1": {
                        "x": 0.0,
                        "y": 0.0,
                        "w": 0.5,
                        "h": 0.5,
                        "threshold": "tight",
                    }
                },
            )
            ref = tmp / "ref.png"
            cand = tmp / "cand.png"
            ref.write_text("placeholder")
            cand.write_text("placeholder")
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertIn("threshold", result.stderr)

    def test_valid_regions_pass_schema_check(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(
                tmp,
                {"r1": {"x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0, "threshold": 0.5}},
            )
            # Missing PNGs still cause exit 2 (file-not-found), but the
            # error message comes AFTER schema validation. Asserting the
            # *absence* of a schema-error substring locks the ordering.
            ref = tmp / "missing-ref.png"
            cand = tmp / "missing-cand.png"
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("non-numeric", result.stderr)
            self.assertNotIn("must be an object", result.stderr)


@unittest.skipUnless(HAVE_PIL, "PIL/Pillow required for image-based tests")
class TestStrictFlag(unittest.TestCase):
    """--strict flag changes exit code semantics."""

    def _setup_mismatched(self, tmp: Path) -> tuple[Path, Path, Path]:
        ref = tmp / "ref.png"
        cand = tmp / "cand.png"
        _make_png(ref, color=(255, 255, 255))
        _make_png(cand, color=(0, 0, 0))
        regions = tmp / "regions.json"
        regions.write_text(
            json.dumps(
                {
                    "full": {
                        "x": 0.0,
                        "y": 0.0,
                        "w": 1.0,
                        "h": 1.0,
                        "threshold": 0.99,
                    }
                }
            )
        )
        return ref, cand, regions

    def test_without_strict_exits_zero_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            ref, cand, regions = self._setup_mismatched(tmp)
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(
                result.returncode,
                0,
                f"expected exit 0 without --strict; got {result.returncode}\n"
                f"stdout={result.stdout}\nstderr={result.stderr}",
            )
            # Failure still reported on stdout.
            self.assertIn("FAIL", result.stdout)

    def test_with_strict_exits_one_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            ref, cand, regions = self._setup_mismatched(tmp)
            result = _run([str(ref), str(cand), "--regions", str(regions), "--strict"])
            self.assertEqual(
                result.returncode,
                1,
                f"expected exit 1 with --strict; got {result.returncode}\n"
                f"stdout={result.stdout}\nstderr={result.stderr}",
            )


class FakeImage:
    def __init__(
        self,
        pixels: list[tuple[int, int, int]],
        size: tuple[int, int] = (10, 10),
        histogram: list[int] | None = None,
    ) -> None:
        self._pixels = pixels
        self.size = size
        self._histogram = histogram
        self.crop_box: tuple[int, int, int, int] | None = None
        self.resized_to: tuple[int, int] | None = None

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

    def crop(self, box: tuple[int, int, int, int]) -> "FakeImage":
        cropped = FakeImage(self._pixels, size=(box[2] - box[0], box[3] - box[1]))
        cropped.crop_box = box
        return cropped

    def resize(self, target: tuple[int, int], _resampling: object = None) -> "FakeImage":
        self.resized_to = target
        self.size = target
        return self


class TestRegionHelpers(unittest.TestCase):
    def test_image_module_imports_pillow_or_reports_install_hint(self) -> None:
        def fake_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "PIL":
                return mock.Mock(Image="fake-image-module")
            return __import__(name, *args, **kwargs)

        with mock.patch("builtins.__import__", side_effect=fake_import):
            self.assertEqual(regions_mod._image_module(), "fake-image-module")
        with mock.patch("builtins.__import__", side_effect=ImportError):
            with self.assertRaisesRegex(RuntimeError, "PIL/Pillow required"):
                regions_mod._image_module()

    def test_crop_region_maps_percent_rects_to_pixel_bounds(self) -> None:
        img = FakeImage([(1, 2, 3)], size=(200, 100))
        cropped = regions_mod.crop_region(img, {"x": 0.25, "y": 0.10, "w": 0.50, "h": 0.20})
        self.assertEqual(cropped.crop_box, (50, 10, 150, 30))
        self.assertEqual(cropped.size, (100, 20))

    def test_crop_region_clamps_empty_percent_dimensions_to_one_pixel(self) -> None:
        img = FakeImage([(1, 2, 3)], size=(200, 100))
        cropped = regions_mod.crop_region(img, {"x": 0.0, "y": 0.0, "w": 0.0, "h": 0.0})
        self.assertEqual(cropped.crop_box, (0, 0, 1, 1))

    def test_mean_pixel_distance_resizes_candidate_region_before_compare(self) -> None:
        ref = FakeImage([(255, 255, 255)], size=(2, 2))
        cand = FakeImage([(255, 255, 255)], size=(1, 1))
        fake_module = mock.Mock()
        fake_module.Resampling.LANCZOS = object()
        with mock.patch.object(regions_mod, "_image_module", return_value=fake_module):
            self.assertEqual(regions_mod.mean_pixel_distance(ref, cand), 1.0)
        self.assertEqual(cand.resized_to, (2, 2))

    def test_histogram_and_pixel_distance_zero_guards(self) -> None:
        empty = FakeImage([], histogram=[])
        full = FakeImage([(255, 255, 255)], histogram=[1])
        self.assertEqual(regions_mod.histogram_similarity(empty, full), 0.0)
        self.assertEqual(regions_mod.mean_pixel_distance(full, FakeImage([])), 0.0)

    def test_is_blank_treats_empty_or_mostly_black_regions_as_blank(self) -> None:
        self.assertTrue(regions_mod.is_blank(FakeImage([])))
        self.assertTrue(regions_mod.is_blank(FakeImage([(0, 0, 0)] * 96 + [(255, 255, 255)] * 4)))
        self.assertFalse(regions_mod.is_blank(FakeImage([(0, 0, 0)] * 94 + [(255, 255, 255)] * 6)))

    def test_region_score_uses_default_threshold_and_blank_candidate_gate(self) -> None:
        ref = FakeImage([(0, 0, 0)] * 100)
        cand = FakeImage([(0, 0, 0)] * 100)
        result = regions_mod.region_score(ref, cand, {"x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0})
        self.assertEqual(result["threshold"], 0.75)
        self.assertFalse(result["passed"])
        self.assertEqual(result["score"], 1.0)
        self.assertTrue(result["blank_candidate"])
        self.assertTrue(result["blank_reference"])
        self.assertEqual(result["rect_pct"], {"x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0})

    def test_region_score_passes_non_blank_matching_region_with_notes(self) -> None:
        img = FakeImage([(255, 255, 255)] * 100)
        result = regions_mod.region_score(
            img,
            img,
            {"x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0, "threshold": 0.99, "notes": "full"},
        )
        self.assertTrue(result["passed"])
        self.assertEqual(result["score"], 1.0)
        self.assertEqual(result["notes"], "full")
        self.assertFalse(result["blank_candidate"])
        self.assertFalse(result["blank_reference"])

    def test_load_regions_defaults_to_spectr_regions(self) -> None:
        self.assertIs(regions_mod.load_regions(None), regions_mod.SPECTR_REGIONS)
        self.assertIn("central_canvas", regions_mod.load_regions(None))

    def test_load_regions_reports_missing_malformed_and_empty_files(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as missing:
            regions_mod.load_regions(Path("missing-regions.json"))
        self.assertIn("regions JSON not found", stderr.getvalue())

        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            malformed = tmp / "bad.json"
            malformed.write_text("{", encoding="utf-8")
            empty = tmp / "empty.json"
            empty.write_text("{}", encoding="utf-8")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as bad_json:
                regions_mod.load_regions(malformed)
            self.assertIn("malformed regions JSON", stderr.getvalue())
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as empty_json:
                regions_mod.load_regions(empty)
            self.assertEqual(empty_json.exception.code, 2)
            self.assertIn("non-empty object", stderr.getvalue())

    def test_load_normalized_uses_pillow_module_and_skips_matching_resize(self) -> None:
        opened = mock.Mock()
        opened.size = (16, 8)
        opened.convert.return_value = opened
        fake_module = mock.Mock()
        fake_module.open.return_value = opened
        with mock.patch.object(regions_mod, "_image_module", return_value=fake_module):
            result = regions_mod.load_normalized(Path("ref.png"), (16, 8))
        self.assertIs(result, opened)
        opened.convert.assert_called_once_with("RGB")
        opened.resize.assert_not_called()

    def test_main_rejects_malformed_size_before_image_loading(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            ref_path = tmp / "ref.png"
            cand_path = tmp / "cand.png"
            ref_path.write_text("ref", encoding="utf-8")
            cand_path.write_text("cand", encoding="utf-8")
            with mock.patch.object(sys, "argv", [str(SCRIPT), str(ref_path), str(cand_path), "--size", "bad"]), \
                 mock.patch.object(regions_mod, "load_normalized") as load_normalized, \
                 contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                rc = regions_mod.main()
        self.assertEqual(rc, 2)
        self.assertIn("malformed --size", stderr.getvalue())
        self.assertEqual(stdout.getvalue(), "")
        load_normalized.assert_not_called()

    def test_main_json_without_strict_reports_failures_but_exits_zero(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            ref_path = tmp / "ref.png"
            cand_path = tmp / "cand.png"
            region_path = tmp / "regions.json"
            ref_path.write_text("ref", encoding="utf-8")
            cand_path.write_text("cand", encoding="utf-8")
            region_path.write_text(
                json.dumps({"full": {"x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0, "threshold": 0.99}}),
                encoding="utf-8",
            )
            with mock.patch.object(sys, "argv", [str(SCRIPT), str(ref_path), str(cand_path), "--regions", str(region_path), "--json"]), \
                 mock.patch.object(regions_mod, "load_normalized", side_effect=[
                     FakeImage([(255, 255, 255)] * 100),
                     FakeImage([(0, 0, 0)] * 100),
                 ]), \
                 contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                rc = regions_mod.main()
        payload = json.loads(stdout.getvalue())
        self.assertEqual(rc, 0)
        self.assertFalse(payload["all_passed"])
        self.assertEqual(payload["failed_regions"], ["full"])
        self.assertEqual(payload["size_normalized_to"], "1320x860")
        self.assertEqual(stderr.getvalue(), "")
        self.assertIn("full", payload["regions"])
        self.assertTrue(payload["regions"]["full"]["blank_candidate"])


if __name__ == "__main__":
    unittest.main()
