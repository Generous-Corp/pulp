#!/usr/bin/env python3
"""Tests for tools/import-design/fidelity_diff.py.

Two layers:

* Unit tests exercise each named heuristic helper on small *synthetic* images
  (a known narrow bar, a known gradient, a known disc) so the building blocks
  are verified without the large smoke fixture.
* One integration test runs the whole tool on tiny checked-in fixtures under
  ``test/fixtures/import-fidelity/`` and asserts the report structure, that a
  proportion-matched render passes tolerance, and that an obviously-distorted
  render fails.

The harness depends on Pillow (PIL). When PIL is unavailable the module raises
``unittest.SkipTest`` at import time so CI on a PIL-less interpreter skips
rather than errors (mirrors the SKIP_RETURN_CODE 77 contract in CMake).
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest

try:  # PIL is a hard dependency of the tool.
    from PIL import Image, ImageDraw  # noqa: F401
except ImportError:  # pragma: no cover - only on a PIL-less interpreter
    # Exit 77 so CTest's SKIP_RETURN_CODE treats this as skipped, not failed.
    print("SKIP: Pillow (PIL) not installed", file=sys.stderr)
    sys.exit(77)

REPO = pathlib.Path(__file__).resolve().parent.parent
TOOL = REPO / "tools" / "import-design" / "fidelity_diff.py"
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures" / "import-fidelity"

# Import the tool as a module (it lives outside any package). Register it in
# sys.modules before exec so dataclasses' type resolution (Python 3.12+) can
# find the module by __module__ name.
_spec = importlib.util.spec_from_file_location("fidelity_diff", TOOL)
assert _spec and _spec.loader
fd = importlib.util.module_from_spec(_spec)
sys.modules["fidelity_diff"] = fd
_spec.loader.exec_module(fd)


# --------------------------------------------------------------------------- #
# Synthetic image builders
# --------------------------------------------------------------------------- #

BG = (20, 23, 28, 255)


def solid_bg(w: int, h: int):
    return Image.new("RGBA", (w, h), BG)


def vertical_bar(w: int, h: int, *, bar_w: int, color):
    """A centered vertical bar of width ``bar_w`` on the dark background."""
    im = solid_bg(w, h)
    d = ImageDraw.Draw(im)
    x0 = (w - bar_w) // 2
    d.rectangle([x0, 0, x0 + bar_w - 1, h - 1], fill=color)
    return im


def red_to_green_gradient(w: int, h: int):
    """A full-height red(top)→green(bottom) gradient bar (meter signature)."""
    im = solid_bg(w, h)
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        r = int(220 * (1 - t) + 60 * t)
        g = int(90 * (1 - t) + 200 * t)
        d.line([0, y, w - 1, y], fill=(r, g, 70, 255))
    return im


# --------------------------------------------------------------------------- #
# Heuristic-helper unit tests
# --------------------------------------------------------------------------- #


class TestImageHelpers(unittest.TestCase):
    def test_background_color_from_corners(self):
        im = vertical_bar(40, 40, bar_w=8, color=(70, 130, 230, 255))
        self.assertEqual(fd.background_color(im), BG[:3])

    def test_color_distance(self):
        self.assertEqual(fd.color_distance((0, 0, 0), (0, 0, 0)), 0.0)
        self.assertAlmostEqual(
            fd.color_distance((0, 0, 0), (255, 255, 255)), 441.673, places=2
        )

    def test_is_foreground_alpha_and_color(self):
        # Opaque + distinct from bg -> foreground.
        self.assertTrue(fd.is_foreground((70, 130, 230, 255), BG[:3]))
        # Transparent -> not foreground regardless of color.
        self.assertFalse(fd.is_foreground((70, 130, 230, 0), BG[:3]))
        # Opaque but ~= bg -> not foreground.
        self.assertFalse(fd.is_foreground((21, 24, 29, 255), BG[:3]))

    def test_art_bounds_of_known_bar(self):
        # A 6px-wide bar centered in a 40-wide image, full height.
        im = vertical_bar(40, 50, bar_w=6, color=(70, 130, 230, 255))
        b = fd.art_bounds(im)
        self.assertIsNotNone(b)
        # Width should be ~6px (allow AA slop of +/-1).
        self.assertTrue(5 <= b.width <= 7, f"width={b.width}")
        self.assertEqual(b.height, 50)
        # Aspect (h/w) should be tall.
        self.assertGreater(b.aspect, 6.0)

    def test_art_bounds_blank_returns_none(self):
        self.assertIsNone(fd.art_bounds(solid_bg(10, 10)))

    def test_sample_column_gradient_orders_red_to_green(self):
        im = red_to_green_gradient(20, 100)
        stops = fd.sample_column_gradient(im, BG[:3], stops=5)
        self.assertEqual(len(stops), 5)
        # Top stop is red-dominant, bottom stop is green-dominant.
        self.assertGreater(stops[0][0], stops[0][1], "top should be red>green")
        self.assertGreater(stops[-1][1], stops[-1][0], "bottom should be green>red")
        # Red channel decreases monotonically top->bottom.
        reds = [s[0] for s in stops]
        self.assertEqual(reds, sorted(reds, reverse=True))

    def test_dominant_colors_excludes_background(self):
        im = vertical_bar(40, 40, bar_w=20, color=(70, 130, 230, 255))
        palette = fd.dominant_colors(im, BG[:3])
        self.assertTrue(palette)
        top_rgb, _frac = palette[0]
        # Dominant foreground is the blue bar, not the dark bg.
        self.assertGreater(top_rgb[2], top_rgb[0])  # blue > red

    def test_downscale_for_scan_preserves_aspect_and_caps_dim(self):
        big = solid_bg(2000, 1000)
        small = fd.downscale_for_scan(big, max_dim=480)
        self.assertEqual(max(small.size), 480)
        # 2:1 aspect preserved.
        self.assertAlmostEqual(small.size[0] / small.size[1], 2.0, places=1)
        # Already-small images are returned unchanged.
        tiny = solid_bg(100, 50)
        self.assertIs(fd.downscale_for_scan(tiny, max_dim=480), tiny)


class TestWidgetDetection(unittest.TestCase):
    def test_detect_knob_disc(self):
        im = solid_bg(120, 120)
        ImageDraw.Draw(im).ellipse(
            [20, 20, 100, 100], fill=(200, 200, 205, 255)
        )
        region = fd.detect_widget_region(im, "knob")
        self.assertIsNotNone(region)
        # Disc is roughly square.
        self.assertAlmostEqual(region.aspect, 1.0, delta=0.2)

    def test_detect_fader_track(self):
        im = vertical_bar(60, 200, bar_w=8, color=(72, 132, 232, 255))
        region = fd.detect_widget_region(im, "fader")
        self.assertIsNotNone(region)
        self.assertGreater(region.aspect, 5.0)  # tall and thin

    def test_detect_meter_gradient_is_single_blob(self):
        # The full red->green ramp must detect as ONE connected component
        # (regression guard for the mid-yellow gap that split the blob).
        im = solid_bg(36, 140)
        grad = red_to_green_gradient(20, 100)
        im.alpha_composite(grad, (8, 20))
        region = fd.detect_widget_region(im, "meter")
        self.assertIsNotNone(region)
        # Should span close to the full 100px gradient height, not half.
        self.assertGreater(region.height, 80, f"height={region.height}")

    def test_detect_absent_signature_returns_none(self):
        # A purely blue image has no knob (silver) signature.
        im = vertical_bar(40, 40, bar_w=30, color=(60, 90, 230, 255))
        self.assertIsNone(fd.detect_widget_region(im, "knob"))


class TestSceneParsing(unittest.TestCase):
    def test_parses_audio_widgets_and_resolves_assets(self):
        import json

        scene = json.loads((FIXTURES / "scene.pulp.json").read_text())
        widgets = fd.parse_widgets(scene, str(FIXTURES / "assets"))
        kinds = sorted(w.kind for w in widgets)
        self.assertEqual(kinds, ["fader", "knob", "meter"])
        by_kind = {w.kind: w for w in widgets}
        # asset_ref resolves to an on-disk PNG.
        self.assertTrue(pathlib.Path(by_kind["knob"].asset_path).exists())
        # Declared style + binding are lifted.
        self.assertEqual(by_kind["knob"].binding, "filter.cutoff_hz")
        self.assertEqual(by_kind["knob"].declared_width, 80)


# --------------------------------------------------------------------------- #
# Integration tests — full tool on checked-in fixtures
# --------------------------------------------------------------------------- #


class TestIntegration(unittest.TestCase):
    def _report(self, render_name: str) -> dict:
        return fd.build_report(
            str(FIXTURES / render_name),
            str(FIXTURES / "scene.pulp.json"),
            str(FIXTURES / "assets"),
            tolerance=0.15,
        )

    def test_report_structure(self):
        report = self._report("render_good.png")
        for key in ("render", "scene", "tolerance", "widgets", "summary", "results"):
            self.assertIn(key, report)
        self.assertEqual(sorted(report["widgets"]), ["fader", "knob", "meter"])
        s = report["summary"]
        for key in ("pass", "fail", "skip", "total", "ok"):
            self.assertIn(key, s)
        # Each result carries the documented fields.
        for r in report["results"]:
            for key in ("heuristic", "subject", "metric", "measured", "status"):
                self.assertIn(key, r)
            self.assertIn(
                r["status"], ("pass", "fail", "info", "skip"), r
            )
        # Every registered heuristic is represented.
        names = {r["heuristic"] for r in report["results"]}
        for expected in (
            "art_bounds",
            "declared_geometry",
            "colors",
        ):
            self.assertIn(expected, names)

    def test_matching_render_passes_tolerance(self):
        report = self._report("render_good.png")
        self.assertTrue(
            report["summary"]["ok"],
            f"good render should pass; fails={[r for r in report['results'] if r['status']=='fail']}",
        )
        self.assertEqual(report["summary"]["fail"], 0)
        # The knob (art ≈ box) passes both geometry heuristics.
        knob_aspect = [
            r
            for r in report["results"]
            if r["subject"] == "Cutoff" and r["metric"] == "aspect"
        ]
        self.assertTrue(knob_aspect)
        self.assertTrue(all(r["status"] == "pass" for r in knob_aspect))

    def test_distorted_render_fails_tolerance(self):
        report = self._report("render_bad.png")
        self.assertFalse(report["summary"]["ok"])
        self.assertGreater(report["summary"]["fail"], 0)
        # The squashed knob's aspect must be among the failures.
        knob_fail = [
            r
            for r in report["results"]
            if r["subject"] == "Cutoff"
            and r["metric"] == "aspect"
            and r["status"] == "fail"
        ]
        self.assertTrue(knob_fail, "squashed knob aspect should fail")

    def test_frame_overlay_skips_without_reference(self):
        report = self._report("render_good.png")
        overlay = [r for r in report["results"] if r["heuristic"] == "frame_overlay"]
        self.assertTrue(overlay)
        self.assertTrue(all(r["status"] == "skip" for r in overlay))

    def test_artifacts_written_with_out_dir(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            fd.build_report(
                str(FIXTURES / "render_good.png"),
                str(FIXTURES / "scene.pulp.json"),
                str(FIXTURES / "assets"),
                out_dir=tmp,
                tolerance=0.15,
            )
            written = list(pathlib.Path(tmp).glob("widget-*.png"))
            self.assertTrue(written, "side-by-side comparison images expected")


if __name__ == "__main__":
    unittest.main()
