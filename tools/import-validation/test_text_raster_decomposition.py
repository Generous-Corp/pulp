#!/usr/bin/env python3
"""Unit tests for text_raster_decomposition.py.

Two layers:
  - synthetic ink maps whose answer is known by construction (a pure shift, a
    uniform coverage gain, a one-pixel dilation, a dropped glyph), so each
    reported number is pinned to the mechanism it claims to name;
  - the recorded renders in test/fixtures/browser-capture-text-raster/, which
    pin the measured result, drawn through SkiaCanvas::fill_text: Pulp's default
    glyph rasterization is Chromium's `-webkit-font-smoothing: auto` and carries
    about 30% more ink than Chromium's `antialiased`, and hinting none plus a
    linear text gamma (SkSurfaceProps contrast 0, gamma 1) reproduces
    `antialiased` exactly.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError:  # pragma: no cover - environment guard
    print("SKIP: numpy and Pillow are required")
    sys.exit(77)

THIS_DIR = Path(__file__).resolve().parent
# Keep repository-relative dependencies explicit so the ctest input generator
# records the helper and fixture directory even though the test resolves them
# from its own location at runtime.
SCRIPT_REPO_PATH = "tools/import-validation/text_raster_decomposition.py"
FIXTURE_REPO_PATH = "test/fixtures/browser-capture-text-raster"
SCRIPT = THIS_DIR / Path(SCRIPT_REPO_PATH).name
FIXTURE = THIS_DIR.parent.parent / Path(FIXTURE_REPO_PATH)
assert SCRIPT.resolve() == (THIS_DIR.parent.parent / SCRIPT_REPO_PATH).resolve()
SPEC = importlib.util.spec_from_file_location("text_raster_decomposition", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
trd = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = trd
SPEC.loader.exec_module(trd)

BG = 14.0


def glyph_field(h: int = 40, w: int = 60) -> np.ndarray:
    """A few antialiased vertical and horizontal stems on a flat background."""
    img = np.full((h, w), BG)
    for x0 in (10, 22, 34):
        img[8:30, x0:x0 + 2] = 220.0
        img[8:30, x0 + 2] = 120.0
        img[8:30, x0 - 1] = 60.0
    img[18:20, 10:40] = 220.0
    return img


def shifted(img: np.ndarray, dx: int, dy: int) -> np.ndarray:
    out = np.full_like(img, BG)
    h, w = img.shape
    out[max(0, dy):h + min(0, dy), max(0, dx):w + min(0, dx)] = \
        img[max(0, -dy):h - max(0, dy), max(0, -dx):w - max(0, dx)]
    return out


BOX = (4, 4, 56, 36)


class SyntheticMechanisms(unittest.TestCase):
    def test_identical_render_is_neutral(self):
        ref = glyph_field()
        row = trd.decompose(ref, ref.copy(), BOX)
        self.assertEqual(row["ink_ratio"], 1.0)
        self.assertEqual((row["dx"], row["dy"]), (0.0, 0.0))
        self.assertEqual(row["shift"], [0, 0])

    def test_pure_shift_moves_centroid_not_ink(self):
        ref = glyph_field()
        row = trd.decompose(ref, shifted(ref, 1, 0), BOX)
        self.assertAlmostEqual(row["ink_ratio"], 1.0, places=3)
        self.assertAlmostEqual(row["dx"], 1.0, places=2)
        self.assertAlmostEqual(row["dy"], 0.0, places=2)
        self.assertEqual(row["shift"], [1, 0])

    def test_coverage_gain_is_ink_with_a_monotone_transfer(self):
        ref = glyph_field()
        cand = BG + (ref - BG) * 1.25
        row = trd.decompose(ref, cand, BOX)
        self.assertAlmostEqual(row["ink_ratio"], 1.25, places=2)
        self.assertEqual(row["shift"], [0, 0])
        self.assertTrue(row["transfer"]["monotone"])
        self.assertLess(row["transfer"]["mean_spread"], 0.05)

    def test_dilation_lights_more_pixels(self):
        ref = glyph_field()
        cand = np.maximum(ref, shifted(ref, 1, 0))
        row = trd.decompose(ref, cand, BOX)
        self.assertGreater(row["ink_ratio"], 1.1)
        self.assertGreater(row["count_ratio"], 1.0)

    def test_dropped_glyph_reads_as_missing_ink(self):
        ref = glyph_field()
        cand = ref.copy()
        cand[8:30, 33:37] = BG
        row = trd.decompose(ref, cand, BOX)
        self.assertLess(row["ink_ratio"], 0.85)

    def test_empty_reference_is_an_error_not_a_ratio(self):
        flat = np.full((40, 60), BG)
        row = trd.decompose(flat, glyph_field(), BOX)
        self.assertIn("error", row)

    def test_dark_text_on_a_light_background_measures_like_light_on_dark(self):
        # The same stems, drawn dark on a light panel.
        light = 240.0
        ref = light - (glyph_field() - BG)
        gained = light - (light - ref) * 1.25
        row = trd.decompose(ref, gained, BOX)
        self.assertAlmostEqual(row["ink_ratio"], 1.25, places=2)
        self.assertEqual(row["shift"], [0, 0])
        moved = np.full_like(ref, light)
        moved[:, 1:] = ref[:, :-1]
        row = trd.decompose(ref, moved, BOX)
        self.assertAlmostEqual(row["ink_ratio"], 1.0, places=3)
        self.assertAlmostEqual(row["dx"], 1.0, places=2)
        self.assertEqual(row["shift"], [1, 0])

    def test_summary_counts_only_measured_labels(self):
        ref = glyph_field()
        rows = [dict(id="a", **trd.decompose(ref, shifted(ref, 1, 0), BOX)),
                dict(id="b", **trd.decompose(np.full((40, 60), BG), ref, BOX))]
        summary = trd.summarize(rows)
        self.assertEqual(summary["labels"], 1)
        self.assertEqual(summary["nonzero_shift"], 1)


class CommandLine(unittest.TestCase):
    def run_cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(SCRIPT), *args],
                              capture_output=True, text=True, encoding="utf-8", check=False)

    def test_html_carries_smoothing_and_every_label(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "index.html"
            proc = self.run_cli("html", str(FIXTURE / "labels.json"), "--font-url", "face.ttf",
                                "--smoothing", "antialiased", "--out", str(out))
            self.assertEqual(proc.returncode, 0, proc.stderr)
            page = out.read_text(encoding="utf-8")
            labels = json.loads((FIXTURE / "labels.json").read_text(encoding="utf-8"))["labels"]
            self.assertIn("-webkit-font-smoothing:antialiased", page)
            for lab in labels:
                self.assertIn(f'id="{lab["id"]}"', page)

    def test_spec_refuses_a_missing_baseline(self):
        with tempfile.TemporaryDirectory() as tmp:
            dump = Path(tmp) / "b.json"
            dump.write_text(json.dumps({"live": {"x": 12, "baseline": 30}}), encoding="utf-8")
            proc = self.run_cli("spec", str(FIXTURE / "labels.json"), str(dump),
                                "--out", str(Path(tmp) / "spec.tsv"))
            self.assertEqual(proc.returncode, trd.EX_INPUT)
            self.assertIn("no baseline", proc.stderr)

    def test_spec_rows_follow_measured_baselines(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "spec.tsv"
            proc = self.run_cli("spec", str(FIXTURE / "labels.json"),
                                str(FIXTURE / "chromium-antialiased.baselines.json"), "--out", str(out))
            self.assertEqual(proc.returncode, 0, proc.stderr)
            first = out.read_text(encoding="utf-8").splitlines()[0].split("\t")
            self.assertEqual(first[6], "SPECTR")
            self.assertEqual(first[5], "ffffffff")


class RecordedMatrix(unittest.TestCase):
    """The experiment's verdict, pinned on the renders it was measured from."""

    def analyze(self, ref: str, cand: str) -> dict:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "r.json"
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "analyze", "--labels", str(FIXTURE / "labels.json"),
                 "--baselines", str(FIXTURE / "chromium-antialiased.baselines.json"),
                 "--ref", str(FIXTURE / ref), "--cand", str(FIXTURE / cand), "--json", str(out)],
                capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            return json.loads(out.read_text(encoding="utf-8"))

    def test_default_pulp_raster_is_heavier_than_antialiased_chromium(self):
        summary = self.analyze("chromium-antialiased.png", "pulp-default.png")["summary"]
        self.assertGreaterEqual(summary["ink_ratio_min"], 1.2)

    def test_default_pulp_raster_matches_chromium_auto_smoothing(self):
        summary = self.analyze("chromium-auto.png", "pulp-default.png")["summary"]
        self.assertAlmostEqual(summary["ink_ratio_median"], 1.0, delta=0.02)

    def test_hinting_none_and_linear_gamma_reproduce_antialiased_ink(self):
        report = self.analyze("chromium-antialiased.png", "pulp-hinting-none-gamma-1.png")
        for row in report["labels"]:
            self.assertAlmostEqual(row["ink_ratio"], 1.0, delta=0.02, msg=row["id"])
            self.assertLess(abs(row["dy"]), 0.25, msg=row["id"])

    def test_ltr_runs_land_on_chromium_and_rtl_runs_keep_half_the_spacing(self):
        labels = {lab["id"]: lab for lab in json.loads((FIXTURE / "labels.json").read_text(encoding="utf-8"))["labels"]}
        report = self.analyze("chromium-antialiased.png", "pulp-hinting-none-gamma-1.png")
        for row in report["labels"]:
            lab = labels[row["id"]]
            # Half the CSS letter-spacing at device scale 2 is `letter_spacing` device px.
            expected = lab["letter_spacing"] if lab.get("direction") == "rtl" else 0.0
            self.assertAlmostEqual(row["dx"], expected, delta=0.1, msg=row["id"])

    def test_offset_control_measures_half_a_css_pixel(self):
        summary = self.analyze("chromium-antialiased.png", "chromium-antialiased-offset.png")["summary"]
        self.assertEqual(summary["ink_ratio_median"], 1.0)
        self.assertAlmostEqual(summary["centroid_median"], 1.0, delta=0.05)


if __name__ == "__main__":
    unittest.main()
