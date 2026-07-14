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


def rounded_panel_on_margin(
    canvas, panel_box, *, margin_color, panel_color=(20, 23, 28, 255),
    border_color=(46, 51, 61, 255), radius=12,
):
    """A rounded dark panel with a border ring, on a ``margin_color`` page."""
    im = Image.new("RGBA", canvas, margin_color)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle(
        list(panel_box), radius=radius, fill=panel_color, outline=border_color, width=2
    )
    return im


def text_line(im, *, y, x0, x1, color=(235, 235, 240, 255), thickness=3):
    """Paint a horizontal glyph-like bar to stand in for a line of text."""
    d = ImageDraw.Draw(im)
    d.rectangle([x0, y, x1, y + thickness], fill=color)
    return im


def silver_knob_with_notch(size, *, notch_angle_deg):
    """A silver disc with a dark radial notch at ``notch_angle_deg`` (0=up,
    clockwise positive). Used to test indicator-angle estimation."""
    import math as _m

    im = solid_bg(size, size)
    d = ImageDraw.Draw(im)
    pad = size // 8
    d.ellipse([pad, pad, size - pad, size - pad], fill=(200, 200, 205, 255))
    cx = cy = size / 2.0
    r = (size / 2.0 - pad) * 0.62
    ang = _m.radians(notch_angle_deg)
    ex = cx + r * _m.sin(ang)
    ey = cy - r * _m.cos(ang)
    d.line([cx, cy, ex, ey], fill=(25, 25, 28, 255), width=max(2, size // 24))
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
# New-heuristic helper unit tests
# --------------------------------------------------------------------------- #


class TestPanelDetection(unittest.TestCase):
    def test_panel_on_light_margin(self):
        # Dark rounded panel on a LIGHT page -> dark-blob detection.
        im = rounded_panel_on_margin(
            (200, 140), (20, 15, 179, 124), margin_color=(245, 245, 245, 255)
        )
        b = fd.detect_panel(im)
        self.assertIsNotNone(b)
        # Detected box hugs the panel, not the full canvas.
        self.assertLess(b.left, 30)
        self.assertGreater(b.left, 10)
        self.assertLess(b.right, 195)
        self.assertGreater(b.right, 165)

    def test_panel_flush_on_dark_deadspace(self):
        # Panel border ring on dead-space of the SAME dark color: the dark-blob
        # detector would return the whole canvas, so the border/feature box must
        # win and exclude the right/bottom dead-space.
        im = rounded_panel_on_margin(
            (240, 160), (0, 0, 159, 119), margin_color=(20, 23, 28, 255)
        )
        b = fd.detect_panel(im)
        self.assertIsNotNone(b)
        # Right dead-space (x>160) and bottom dead-space (y>120) excluded.
        self.assertLess(b.right, 200, f"right={b.right} should exclude dead-space")
        self.assertLess(b.bottom, 150, f"bottom={b.bottom} should exclude dead-space")


class TestFullWidgetDetection(unittest.TestCase):
    def test_full_widget_includes_housing_above_fill(self):
        # A meter: dark housing slot (top) + colored fill (bottom). The signature
        # blob is only the fill; the FULL widget must include the housing, so it
        # is taller than the fill-only blob.
        im = solid_bg(40, 160)
        d = ImageDraw.Draw(im)
        # Housing slot: a dark-but-distinct vertical bar, full height.
        d.rectangle([15, 10, 25, 150], fill=(52, 57, 66, 255))
        # Fill: colored gradient over the bottom 60%.
        grad = red_to_green_gradient(11, 84)
        im.alpha_composite(grad, (15, 66))
        fill = fd.detect_widget_region(im, "meter")
        full = fd.detect_full_widget(im, "meter")
        self.assertIsNotNone(fill)
        self.assertIsNotNone(full)
        # Full extent reaches above the fill (absorbs the housing).
        self.assertLess(full.top, fill.top, f"full.top={full.top} fill.top={fill.top}")
        self.assertGreater(full.height, fill.height)

    def test_full_widget_clipped_to_seed_box(self):
        # Two side-by-side fills; seeding to the left declared box must keep the
        # full-widget detection from absorbing the right neighbor.
        im = solid_bg(120, 80)
        d = ImageDraw.Draw(im)
        d.rectangle([10, 20, 30, 60], fill=(72, 132, 232, 255))   # left fader
        d.rectangle([90, 20, 110, 60], fill=(72, 132, 232, 255))  # right fader
        seed = fd.Bounds(0, 0, 55, 80)
        full = fd.detect_full_widget(im, "fader", seed_bounds=seed)
        self.assertIsNotNone(full)
        self.assertLessEqual(full.right, 55, "must not bleed into the right widget")


class TestHousingStrokeDetection(unittest.TestCase):
    def test_track_stroke_detects_near_bg_recessed_channel(self):
        # The captured Pulp fader draws a dark recessed track channel above the
        # thumb that is intentionally close to the panel bg (a subtle slot). The
        # detector must count it as housing even though it is NOT lighter than
        # the bg — keying only on "lighter than bg" missed the dark slot.
        im = solid_bg(20, 80)
        d = ImageDraw.Draw(im)
        # Recessed channel in the TOP band: a hair darker than the bg, low-sat.
        d.rectangle([8, 0, 11, 30], fill=(12, 14, 18, 255))
        self.assertTrue(
            fd._has_track_stroke_above_thumb(im, BG[:3]),
            "near-bg recessed dark channel should read as housing",
        )

    def test_track_stroke_detects_lighter_edge(self):
        # A lighter drawn track edge (the other valid housing style) still counts.
        im = solid_bg(20, 80)
        d = ImageDraw.Draw(im)
        d.rectangle([8, 0, 11, 30], fill=(61, 63, 71, 255))
        self.assertTrue(fd._has_track_stroke_above_thumb(im, BG[:3]))

    def test_track_stroke_absent_on_plain_background(self):
        # A crop that is ALL panel background (no recessed channel, no edge) must
        # NOT be mistaken for a housing stroke — keeps the calibration honest.
        im = solid_bg(20, 80)
        self.assertFalse(fd._has_track_stroke_above_thumb(im, BG[:3]))


class TestPaletteTraceFilter(unittest.TestCase):
    def test_palette_match_ignores_thin_aa_rim(self):
        # A thin anti-aliased rim (a single mid-grey pixel run << 8% of the crop)
        # is not part of a widget's identity palette and must not dominate the
        # worst-nearest-distance. Model the dominant_colors output directly: a
        # dominant blue fill plus a trace mid-grey. Mirrors the palette_match
        # trace-filter logic so a 1px thumb/track AA edge can't flip the gate.
        # Dominant fill (blue) ~92%, trace AA grey ~5%, near-bg ~3%.
        ren_palette = [
            ((64, 128, 224), 0.92),
            ((128, 128, 128), 0.05),  # trace AA rim — must be ignored
            ((32, 32, 64), 0.03),
        ]
        ref_palette = [(64, 128, 224), (192, 192, 192), (32, 32, 64)]
        MIN = 0.08
        scored = [(c, f) for c, f in ren_palette if f >= MIN] or ren_palette[:1]
        worst = max(
            min(fd.color_distance(c, rc) for rc in ref_palette) for c, _f in scored
        )
        # The trace grey (128,128,128) — distance ~110 to the nearest ref — is
        # excluded, so the worst is the well-matched blue (≈0).
        self.assertLess(worst, 30.0)
        # Sanity: without the filter the trace grey WOULD dominate.
        worst_unfiltered = max(
            min(fd.color_distance(c, rc) for rc in ref_palette) for c, _f in ren_palette
        )
        self.assertGreater(worst_unfiltered, 90.0)


class TestTextDetection(unittest.TestCase):
    def test_text_run_found_in_band(self):
        im = solid_bg(200, 60)
        text_line(im, y=25, x0=20, x1=120)
        run = fd.text_run_in_band(im, 10, 45, bg=BG[:3])
        self.assertIsNotNone(run)
        self.assertAlmostEqual(run.width, 100, delta=6)

    def test_missing_text_band_returns_none(self):
        # A blank band -> no glyph run -> None (the "missing label" signal).
        im = solid_bg(200, 60)
        run = fd.text_run_in_band(im, 10, 45, bg=BG[:3])
        self.assertIsNone(run)

    def test_overflow_run_spans_full_width(self):
        # A line that runs to the edge models an overflowing / no-wrap text.
        im = solid_bg(200, 60)
        text_line(im, y=25, x0=2, x1=198)
        run = fd.text_run_in_band(im, 10, 45, bg=BG[:3])
        self.assertIsNotNone(run)
        self.assertGreater(run.width, 180)

    def test_prefer_y_locks_onto_nearer_line(self):
        # Two text lines in one window: the SHORT upper line and a LONG (more
        # pixels) lower line. prefer_y near the upper line must select it, even
        # though the lower line has more lit pixels.
        im = solid_bg(220, 80)
        text_line(im, y=12, x0=20, x1=70)    # short upper line
        text_line(im, y=55, x0=5, x1=215)    # long lower line
        run = fd.text_run_in_band(im, 0, 80, bg=BG[:3], prefer_y=13)
        self.assertIsNotNone(run)
        self.assertLess(run.width, 90, "should lock onto the short upper line")

    def test_bright_mask_glyph_vs_panel_bg(self):
        # On a DARK panel crop, a grey glyph is bright-relative to the panel bg
        # while the dark panel fill is not. (The harness crops to the panel
        # before calling, so the surrounding page margin is out of frame.)
        im = Image.new("RGBA", (60, 30), (20, 23, 28, 255))  # dark panel crop
        d = ImageDraw.Draw(im)
        d.rectangle([10, 13, 45, 16], fill=(235, 235, 240, 255))  # glyph
        mask, w, h = fd._bright_mask(im, bg=(20, 23, 28))
        lit = [(i % w, i // w) for i, v in enumerate(mask) if v]
        self.assertTrue(lit, "glyph should register")
        # All lit pixels are within the glyph row band, not the panel fill.
        self.assertTrue(all(12 <= y <= 17 for _x, y in lit))


class TestIndicatorAngle(unittest.TestCase):
    def test_estimates_up_notch(self):
        im = silver_knob_with_notch(120, notch_angle_deg=0)
        ang = fd.estimate_indicator_angle(im, fd.background_color(im))
        self.assertIsNotNone(ang)
        self.assertLess(abs(ang), 20, f"angle={ang} should be ~0 (up)")

    def test_estimates_down_right_notch(self):
        im = silver_knob_with_notch(120, notch_angle_deg=120)
        ang = fd.estimate_indicator_angle(im, fd.background_color(im))
        self.assertIsNotNone(ang)
        # Within ±35° of the painted 120°.
        self.assertLess(abs(ang - 120), 35, f"angle={ang} should be ~120")


class TestInteriorBackground(unittest.TestCase):
    def test_interior_ignores_rounded_corners(self):
        # A rounded dark panel cropped tight: the corners show the light page,
        # but the modal interior color must be the dark panel fill.
        im = rounded_panel_on_margin(
            (100, 80), (0, 0, 99, 79), margin_color=(245, 245, 245, 255), radius=18
        )
        bg = fd.interior_background(im)
        self.assertLess(sum(bg) / 3.0, 60, f"interior bg should be dark, got {bg}")


class TestNewHeuristics(unittest.TestCase):
    """Drive the named heuristics on a synthetic scene+render so each produces
    the right PASS/FAIL signal in isolation."""

    def _ctx(self, render, *, texts=None, widgets=None, root_padding=None,
             root_w=300, root_h=200):
        return fd.Context(
            render=render,
            render_bg=fd.interior_background(render),
            scene={},
            widgets=widgets or [],
            texts=texts or [],
            assets_dir="",
            frame_reference=None,
            out_dir=None,
            tolerance=0.15,
            root_width=root_w,
            root_height=root_h,
            root_padding=root_padding,
        )

    def _text(self, content, *, x, y, w=None, h=16, fs=12, fw=400):
        return fd.TextSpec(
            content=content, declared_width=w, declared_height=h, font_size=fs,
            font_weight=fw, font_family="Inter", color="#fff",
            node_id="n", abs_x=x, abs_y=y,
        )

    def test_completeness_flags_overflowing_text(self):
        # Panel filling the canvas; a text declared narrow (40px) but rendered
        # to the panel edge => overflow / no-wrap FAIL.
        render = rounded_panel_on_margin(
            (300, 200), (0, 0, 299, 199), margin_color=(20, 23, 28, 255), radius=10
        )
        text_line(render, y=30, x0=10, x1=290)  # spans nearly full width
        t = self._text("overflowing label", x=10, y=30, w=40, h=16)
        ctx = self._ctx(render, texts=[t])
        results = fd.heuristic_completeness(ctx)
        widths = [r for r in results if r.metric == "text_width"]
        self.assertTrue(widths)
        self.assertEqual(widths[0].status, "fail", "wide text should flag overflow")

    def test_completeness_flags_missing_text(self):
        # A declared text with NO glyphs rendered in its band => MISSING FAIL.
        render = rounded_panel_on_margin(
            (300, 200), (0, 0, 299, 199), margin_color=(20, 23, 28, 255), radius=10
        )
        t = self._text("dropped label", x=20, y=100, w=120, h=16)
        ctx = self._ctx(render, texts=[t])
        results = fd.heuristic_completeness(ctx)
        pres = [r for r in results if r.metric == "presence"]
        self.assertTrue(pres)
        self.assertEqual(pres[0].status, "fail")
        self.assertEqual(pres[0].measured, "MISSING")

    def test_completeness_passes_fitting_text(self):
        render = rounded_panel_on_margin(
            (300, 200), (0, 0, 299, 199), margin_color=(20, 23, 28, 255), radius=10
        )
        text_line(render, y=30, x0=20, x1=110)  # ~90px, fits declared 160
        t = self._text("fits fine", x=20, y=30, w=160, h=16)
        ctx = self._ctx(render, texts=[t])
        results = fd.heuristic_completeness(ctx)
        pres = [r for r in results if r.metric == "presence"][0]
        width = [r for r in results if r.metric == "text_width"][0]
        self.assertEqual(pres.status, "pass")
        self.assertEqual(width.status, "pass")

    def test_padding_flags_hugging_content(self):
        # Content jammed against the panel's top-left corner with declared
        # padding 24 => HUG FAIL.
        render = rounded_panel_on_margin(
            (300, 200), (0, 0, 299, 199), margin_color=(20, 23, 28, 255), radius=8
        )
        d = ImageDraw.Draw(render)
        d.rectangle([4, 4, 60, 60], fill=(200, 200, 205, 255))  # hugs corner
        ctx = self._ctx(
            render, root_padding={"top": 24, "right": 24, "bottom": 24, "left": 24}
        )
        results = fd.heuristic_padding(ctx)
        statuses = {r.metric: r.status for r in results}
        self.assertEqual(statuses.get("left_inset"), "fail")
        self.assertEqual(statuses.get("top_inset"), "fail")

    def test_padding_passes_with_real_padding(self):
        # Content inset well inside the panel respects declared padding => PASS.
        render = rounded_panel_on_margin(
            (300, 200), (0, 0, 299, 199), margin_color=(20, 23, 28, 255), radius=8
        )
        d = ImageDraw.Draw(render)
        d.rectangle([40, 30, 120, 120], fill=(200, 200, 205, 255))
        ctx = self._ctx(
            render, root_padding={"top": 24, "right": 24, "bottom": 24, "left": 36}
        )
        results = fd.heuristic_padding(ctx)
        statuses = {r.metric: r.status for r in results}
        self.assertEqual(statuses.get("left_inset"), "pass")
        self.assertEqual(statuses.get("top_inset"), "pass")

    def test_text_style_size_proxy(self):
        # A title rendered at roughly its declared size should pass the size
        # proxy; tolerance is intentionally coarse.
        render = rounded_panel_on_margin(
            (300, 200), (0, 0, 299, 199), margin_color=(20, 23, 28, 255), radius=8
        )
        # font_size 18, root 200 tall, panel ~200 tall => scale ~1; paint a glyph
        # band ~14px tall (cap height) near the predicted Y.
        d = ImageDraw.Draw(render)
        d.rectangle([20, 18, 160, 32], fill=(235, 235, 240, 255))
        t = self._text("Title", x=20, y=20, w=200, h=22, fs=18, fw=600)
        ctx = self._ctx(render, texts=[t])
        results = fd.heuristic_text_style(ctx)
        gh = [r for r in results if r.metric == "glyph_height"]
        self.assertTrue(gh)
        self.assertEqual(gh[0].status, "pass")


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
        # Every registered heuristic is represented (incl. the hardened set).
        names = {r["heuristic"] for r in report["results"]}
        for expected in (
            "art_bounds",
            "declared_geometry",
            "colors",
            "completeness",
            "padding",
            "widget_detail",
            "text_style",
            "frame_overlay",
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

    def test_frame_overlay_aligned_similarity_high_for_identical(self):
        # Content-aware alignment: when the render IS the reference frame, the
        # aligned-panel similarity must be very high and the heuristic PASS.
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            report = fd.build_report(
                str(FIXTURES / "render_good.png"),
                str(FIXTURES / "scene.pulp.json"),
                str(FIXTURES / "assets"),
                frame_reference=str(FIXTURES / "render_good.png"),
                out_dir=tmp,
                tolerance=0.15,
            )
            sim = [
                r for r in report["results"]
                if r["heuristic"] == "frame_overlay" and r["metric"] == "similarity"
            ]
            self.assertTrue(sim)
            self.assertGreaterEqual(sim[0]["measured"], 0.85)
            self.assertEqual(sim[0]["status"], "pass")
            # Aligned overlay artifacts written.
            self.assertTrue(
                (pathlib.Path(tmp) / "frame-side-by-side.png").exists()
            )
            self.assertTrue(
                (pathlib.Path(tmp) / "frame-diff-heatmap.png").exists()
            )

    def test_graceful_skip_without_optional_inputs(self):
        # The fixture scene has no text nodes / no padding / no abs transforms,
        # so completeness/padding/text_style degrade to skips (or widget-only),
        # never crash. This guards the graceful-degradation contract.
        report = self._report("render_good.png")
        # No text nodes -> no text presence rows, only widget_presence rows.
        text_pres = [
            r for r in report["results"]
            if r["heuristic"] == "completeness" and r["metric"] == "presence"
        ]
        self.assertEqual(text_pres, [])
        wid_pres = [
            r for r in report["results"]
            if r["heuristic"] == "completeness" and r["metric"] == "widget_presence"
        ]
        self.assertEqual(len(wid_pres), 3)
        # No declared padding -> padding heuristic skips.
        pad = [r for r in report["results"] if r["heuristic"] == "padding"]
        self.assertTrue(all(r["status"] == "skip" for r in pad))


if __name__ == "__main__":
    unittest.main()
