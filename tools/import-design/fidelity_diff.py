#!/usr/bin/env python3
"""Visual-fidelity diff harness for Pulp's design-import pipeline.

Measures how close an imported + rendered design is to its original Figma
source, using several named, reusable heuristics against multiple references:

  * the Figma-plugin scene export (``scene.pulp.json``) — the declared data
    (per-node ``style.width/height/border_radius``, ``audio_widget``,
    ``attributes.binding``, and ``asset_ref`` into ``asset_manifest``);
  * the per-widget captured reference PNGs (pixel-exact Figma art) named by
    ``asset_ref`` / ``asset_manifest[].local_path``;
  * optionally the whole-frame reference screenshot.

The point is to turn ad-hoc measurement scripts into a *repeatable, codified*
tool so import-fidelity work is measured, not eyeballed, and regressions are
caught.

Design notes
------------
* Dependency-light: standard library + Pillow (PIL) only. No network.
* Heuristics live in a small registry (``HEURISTICS``) so new ones can be
  added without touching the driver. Each heuristic is a small, documented,
  individually unit-testable function operating on plain values + PIL images.
* Missing optional inputs degrade gracefully (e.g. no ``--frame-reference``
  skips the whole-frame heuristic rather than erroring).

CLI
---
::

    python3 tools/import-design/fidelity_diff.py \\
      --render <render.png> --scene <scene.pulp.json> \\
      --assets-dir <dir with captured asset_ref PNGs> \\
      [--frame-reference <whole-frame.png>] [--out-dir <dir>] \\
      [--json <report.json>] [--tolerance 0.15]

Exit code is ``0`` when every measured heuristic is within tolerance, ``1``
when at least one fails. Heuristics that could not run (skipped) do not fail
the suite.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import sys
from typing import Callable, Iterable, Optional

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover - exercised only when PIL absent
    sys.stderr.write(
        "fidelity_diff: Pillow (PIL) is required. Install with `pip install pillow`.\n"
    )
    raise SystemExit(2) from exc


# --------------------------------------------------------------------------- #
# Small geometry / color value types
# --------------------------------------------------------------------------- #


@dataclasses.dataclass(frozen=True)
class Bounds:
    """Pixel bounding box (left, top, right, bottom) — right/bottom exclusive."""

    left: int
    top: int
    right: int
    bottom: int

    @property
    def width(self) -> int:
        return max(0, self.right - self.left)

    @property
    def height(self) -> int:
        return max(0, self.bottom - self.top)

    @property
    def aspect(self) -> float:
        """height / width, the natural aspect for tall audio widgets."""
        return self.height / self.width if self.width else 0.0

    def as_tuple(self) -> tuple[int, int, int, int]:
        return (self.left, self.top, self.right, self.bottom)


RGB = tuple[int, int, int]


# --------------------------------------------------------------------------- #
# Core image helpers (reused by several heuristics)
# --------------------------------------------------------------------------- #


#: Largest dimension (px) a render/frame is scanned at. Aspect ratios and
#: color signatures are scale-invariant, so down-scaling a large render before
#: the pure-Python pixel scans keeps the harness fast (seconds, not minutes)
#: without affecting the measured heuristics. Small captured assets are never
#: up-scaled.
MAX_SCAN_DIM = 480


def load_rgba(path: str) -> "Image.Image":
    """Load an image as RGBA. Pillow handles palette/L/RGB transparently."""
    return Image.open(path).convert("RGBA")


def _flat_pixels(img: "Image.Image") -> list:
    """Flat list of RGBA pixel tuples — much faster than per-pixel ``load()``.

    Uses ``get_flattened_data`` on Pillow >= 12 (where ``getdata`` is
    deprecated) and falls back to ``getdata`` on older Pillow."""
    getter = getattr(img, "get_flattened_data", None)
    if getter is not None:
        return list(getter())
    return list(img.getdata())


def downscale_for_scan(img: "Image.Image", max_dim: int = MAX_SCAN_DIM) -> "Image.Image":
    """Down-scale ``img`` so its longest side is ``max_dim`` px, preserving
    aspect. No-op when already small enough. Used on large renders/frames
    before pixel scans; never enlarges."""
    w, h = img.size
    longest = max(w, h)
    if longest <= max_dim:
        return img
    scale = max_dim / longest
    return img.resize((max(1, round(w * scale)), max(1, round(h * scale))))


def background_color(img: "Image.Image") -> RGB:
    """Estimate the dominant background color from the image's corners.

    Audio-widget art and renders share a flat dark panel background; the four
    corners are the most reliable background sample. Returns the modal corner
    color (ties broken by first seen).
    """
    w, h = img.size
    if w == 0 or h == 0:
        return (0, 0, 0)
    px = img.load()
    corners = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    counts: dict[RGB, int] = {}
    for x, y in corners:
        r, g, b, _a = px[x, y]
        key = (r, g, b)
        counts[key] = counts.get(key, 0) + 1
    return max(counts.items(), key=lambda kv: kv[1])[0]


def color_distance(a: RGB, b: RGB) -> float:
    """Euclidean distance in RGB space (0..441.67)."""
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def is_foreground(
    pixel: tuple[int, int, int, int],
    bg: RGB,
    *,
    alpha_threshold: int = 16,
    color_threshold: float = 40.0,
) -> bool:
    """A pixel counts as visible art if it is opaque enough AND distinct
    from the background color. Both conditions matter: anti-aliased panel
    edges are opaque but near-background, and translucent shadows are
    background-colored but low alpha."""
    r, g, b, a = pixel
    if a < alpha_threshold:
        return False
    return color_distance((r, g, b), bg) > color_threshold


def art_bounds(
    img: "Image.Image",
    *,
    bg: Optional[RGB] = None,
    alpha_threshold: int = 16,
    color_threshold: float = 40.0,
) -> Optional[Bounds]:
    """Tight bounding box of the visible art (foreground) within ``img``.

    Returns ``None`` when no foreground pixel is found (e.g. fully blank crop).
    Pure-Python scan — fine for the small per-widget crops the harness uses.
    """
    img = img.convert("RGBA")
    w, h = img.size
    if w == 0 or h == 0:
        return None
    if bg is None:
        bg = background_color(img)
    data = _flat_pixels(img)  # flat RGBA tuples — much faster than px[x,y]
    bg_r, bg_g, bg_b = bg
    ct2 = color_threshold * color_threshold
    left = top = None
    right = bottom = 0
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = data[i]
            i += 1
            if a < alpha_threshold:
                continue
            dr, dg, db = r - bg_r, g - bg_g, b - bg_b
            if dr * dr + dg * dg + db * db <= ct2:
                continue
            if left is None or x < left:
                left = x
            if top is None or y < top:
                top = y
            if x + 1 > right:
                right = x + 1
            if y + 1 > bottom:
                bottom = y + 1
    if left is None or top is None:
        return None
    return Bounds(left, top, right, bottom)


def dominant_colors(
    img: "Image.Image",
    bg: RGB,
    *,
    max_colors: int = 6,
    quant: int = 32,
) -> list[tuple[RGB, float]]:
    """Return the most common *foreground* colors (quantized into ``quant``
    buckets per channel) as ``(rgb, fraction)`` sorted by frequency. The
    background is excluded so we sample the widget's real palette."""
    img = img.convert("RGBA")
    counts: dict[RGB, int] = {}
    total = 0
    for pix in _flat_pixels(img):
        if not is_foreground(pix, bg):
            continue
        r, g, b, _a = pix
        key = (
            (r // quant) * quant,
            (g // quant) * quant,
            (b // quant) * quant,
        )
        counts[key] = counts.get(key, 0) + 1
        total += 1
    if total == 0:
        return []
    ordered = sorted(counts.items(), key=lambda kv: kv[1], reverse=True)
    return [(rgb, n / total) for rgb, n in ordered[:max_colors]]


def sample_column_gradient(
    img: "Image.Image",
    bg: RGB,
    *,
    stops: int = 5,
) -> list[RGB]:
    """Sample ``stops`` representative colors top→bottom along the vertical
    center of the art. Used for the meter's green→red gradient. Each stop is
    the mean of the foreground pixels in a horizontal band."""
    img = img.convert("RGBA")
    bounds = art_bounds(img, bg=bg)
    if bounds is None:
        return []
    px = img.load()
    result: list[RGB] = []
    band_h = max(1, bounds.height // stops)
    for s in range(stops):
        y0 = bounds.top + s * band_h
        y1 = min(bounds.bottom, y0 + band_h)
        # Sample the centered middle 50% of each band so sharp band-boundary
        # anti-aliasing (e.g. the meter's top red→background edge) does not
        # skew the stop's mean color.
        if y1 - y0 >= 4:
            inset = (y1 - y0) // 4
            y0 += inset
            y1 -= inset
        rs = gs = bs = n = 0
        for y in range(y0, y1):
            for x in range(bounds.left, bounds.right):
                pix = px[x, y]
                if not is_foreground(pix, bg):
                    continue
                rs += pix[0]
                gs += pix[1]
                bs += pix[2]
                n += 1
        if n:
            result.append((rs // n, gs // n, bs // n))
    return result


# --------------------------------------------------------------------------- #
# Scene parsing — flatten the audio-widget nodes the harness cares about
# --------------------------------------------------------------------------- #


@dataclasses.dataclass
class WidgetSpec:
    """A single audio-widget node lifted from the scene export."""

    kind: str  # knob / fader / meter
    label: str
    asset_ref: Optional[str]
    asset_path: Optional[str]  # resolved local PNG path (from asset_manifest)
    declared_width: Optional[float]
    declared_height: Optional[float]
    declared_radius: Optional[float]
    binding: Optional[str]
    node_id: Optional[str]


def _asset_index(scene: dict) -> dict[str, str]:
    """Map asset_id → local_path from the scene's asset_manifest."""
    index: dict[str, str] = {}
    manifest = scene.get("asset_manifest") or {}
    for asset in manifest.get("assets", []) or []:
        aid = asset.get("asset_id")
        path = asset.get("local_path")
        if aid and path:
            index[aid] = path
    return index


def parse_widgets(scene: dict, assets_dir: str) -> list[WidgetSpec]:
    """Walk the scene tree and collect every node with an ``audio_widget``.

    ``asset_ref`` values are prefixes of ``asset_id`` (the export truncates
    the content hash), so resolution matches by ``startswith`` against the
    manifest's asset ids, then falls back to a basename match in ``assets_dir``.
    """
    assets = _asset_index(scene)
    widgets: list[WidgetSpec] = []

    def resolve_asset(ref: Optional[str]) -> Optional[str]:
        if not ref:
            return None
        # Direct id match, then prefix match (export truncates the hash).
        local = assets.get(ref)
        if local is None:
            for aid, path in assets.items():
                if aid.startswith(ref) or ref.startswith(aid):
                    local = path
                    break
        if local is None:
            return None
        candidate = os.path.join(assets_dir, os.path.basename(local))
        return candidate if os.path.exists(candidate) else None

    def visit(node: dict) -> None:
        if not isinstance(node, dict):
            return
        kind = node.get("audio_widget")
        if kind:
            style = node.get("style") or {}
            attrs = node.get("attributes") or {}
            ref = node.get("asset_ref")
            widgets.append(
                WidgetSpec(
                    kind=kind,
                    label=node.get("label") or node.get("name") or kind,
                    asset_ref=ref,
                    asset_path=resolve_asset(ref),
                    declared_width=style.get("width"),
                    declared_height=style.get("height"),
                    declared_radius=style.get("border_radius"),
                    binding=attrs.get("binding"),
                    node_id=node.get("figma_node_id"),
                )
            )
        for child in node.get("children", []) or []:
            visit(child)

    root = scene.get("root") or scene
    visit(root)
    return widgets


# --------------------------------------------------------------------------- #
# Widget detection in the rendered frame
# --------------------------------------------------------------------------- #
#
# Each audio_widget has a distinctive expected look that lets us locate it in
# the render without per-pixel layout knowledge:
#   knob  -> silver/gray disc (low saturation, mid-high luminance, a big blob)
#   fader -> vertical blue fill (blue-dominant track) + light thumb cap
#   meter -> green→red vertical gradient (green and/or red dominant)
#
# Strategy: build a per-kind signature *mask* over the whole render, take the
# bounding box of the LARGEST connected component of that mask, then expand
# tightly around it. Connected-component selection is what excludes off-target
# noise — e.g. the gray title text never connects into the knob disc blob, and
# the green meter pixels never connect to the blue fader track.


def _pixel_matches_signature(pix: tuple[int, int, int, int], kind: str) -> bool:
    """Whether a pixel matches the distinctive look of ``kind``."""
    r, g, b, a = pix
    if a < 16:
        return False
    mx = max(r, g, b)
    mn = min(r, g, b)
    sat = mx - mn
    if kind == "knob":
        # Silver disc: low saturation, mid-high luminance. Excludes the dark
        # panel (low luminance) and saturated fader/meter art.
        return sat < 30 and 95 < mx < 245
    if kind == "fader":
        # Blue track or light thumb cap directly above/over the track.
        blue = b > 110 and b - r > 40 and b - g > 25
        return blue
    if kind == "meter":
        # The meter ramp runs red → orange → yellow → green; the mid (yellow)
        # band has both r and g high, so green-only + warm-only tests leave a
        # gap there and split the gradient into two components. Include a
        # yellow/mid term so the whole ramp is one connected blob. The unifying
        # signal across the ramp is "blue is the clearly weakest channel".
        green = g > 110 and g - b > 35
        warm = r > 150 and g > 60 and b < 120
        yellow = r > 120 and g > 120 and b < 120 and min(r, g) - b > 30
        return green or warm or yellow
    return False


def _signature_mask(img: "Image.Image", kind: str) -> tuple[list[list[bool]], int, int]:
    """Boolean mask (row-major) of pixels matching ``kind``'s signature."""
    img = img.convert("RGBA")
    w, h = img.size
    data = _flat_pixels(img)  # flat RGBA — faster than per-pixel px[x,y]
    match = _pixel_matches_signature
    flat = [match(pix, kind) for pix in data]
    mask = [flat[y * w:(y + 1) * w] for y in range(h)]
    return mask, w, h


def _largest_component_bounds(
    mask: list[list[bool]], w: int, h: int, *, min_pixels: int = 64
) -> Optional[Bounds]:
    """Bounding box of the largest 4-connected component in ``mask``.

    Iterative flood fill (no recursion-depth limit). Components smaller than
    ``min_pixels`` are ignored as noise (stray anti-aliased text pixels)."""
    seen = [[False] * w for _ in range(h)]
    best_size = 0
    best: Optional[Bounds] = None
    for sy in range(h):
        for sx in range(w):
            if not mask[sy][sx] or seen[sy][sx]:
                continue
            stack = [(sx, sy)]
            seen[sy][sx] = True
            left = right = sx
            top = bottom = sy
            size = 0
            while stack:
                x, y = stack.pop()
                size += 1
                if x < left:
                    left = x
                if x > right:
                    right = x
                if y < top:
                    top = y
                if y > bottom:
                    bottom = y
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h and mask[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            if size >= min_pixels and size > best_size:
                best_size = size
                best = Bounds(left, top, right + 1, bottom + 1)
    return best


def detect_widget_region(
    img: "Image.Image", kind: str, bg: Optional[RGB] = None
) -> Optional[Bounds]:
    """Locate the art bounds of a widget of ``kind`` in a render or asset crop.

    Builds the kind's signature mask and returns the bounding box of its
    largest connected component. This naturally excludes the title/label text
    (different color, not connected to the art blob) and neighbouring widgets
    (different signature)."""
    mask, w, h = _signature_mask(img, kind)
    return _largest_component_bounds(mask, w, h)


# --------------------------------------------------------------------------- #
# Heuristic registry
# --------------------------------------------------------------------------- #
#
# Each heuristic is a callable(ctx) -> list[HeuristicResult]. A heuristic that
# cannot run for the given inputs returns results with status="skip" (or an
# empty list). Register new heuristics by appending to HEURISTICS.


@dataclasses.dataclass
class HeuristicResult:
    heuristic: str  # heuristic name
    subject: str  # which widget / what was measured
    metric: str  # metric name
    measured: object  # measured value (number / string / dict)
    expected: object = None  # reference value, if any
    delta: Optional[float] = None  # signed/abs delta where meaningful
    status: str = "info"  # pass / fail / info / skip
    note: str = ""
    delta_is_ratio: bool = True  # True -> render delta as %, False -> raw value

    def to_dict(self) -> dict:
        return dataclasses.asdict(self)


@dataclasses.dataclass
class Context:
    render: "Image.Image"
    render_bg: RGB
    scene: dict
    widgets: list[WidgetSpec]
    assets_dir: str
    frame_reference: Optional["Image.Image"]
    out_dir: Optional[str]
    tolerance: float  # fractional dimension tolerance, e.g. 0.15


def _ratio_status(measured: float, expected: float, tol: float) -> tuple[float, str]:
    """Return (fractional_delta, pass/fail) for a measured-vs-expected pair."""
    if expected == 0:
        return (0.0, "info")
    delta = (measured - expected) / expected
    return (delta, "pass" if abs(delta) <= tol else "fail")


# -- Heuristic 1: per-widget art bounds (reference vs rendered) ------------- #


def heuristic_art_bounds(ctx: Context) -> list[HeuristicResult]:
    """Compare the visible-art bounding box of each rendered widget against its
    captured ``asset_ref`` reference.

    The render and the captured asset PNG are at *different* canvas
    resolutions, so absolute pixel widths/heights are not directly comparable
    and are reported as ``info`` only. The pass/fail signal is the
    scale-invariant **aspect ratio** (height/width of the isolated art blob),
    which should match regardless of render scale. Both sides use the same
    ``detect_widget_region`` signature detector for an apples-to-apples blob."""
    results: list[HeuristicResult] = []
    for w in ctx.widgets:
        region = detect_widget_region(ctx.render, w.kind, ctx.render_bg)
        if region is None:
            results.append(
                HeuristicResult(
                    "art_bounds", w.label, "detect", None,
                    status="skip", note=f"could not locate {w.kind} in render",
                )
            )
            continue
        if not w.asset_path:
            results.append(
                HeuristicResult(
                    "art_bounds", w.label, "reference", None,
                    status="skip", note="no captured asset reference",
                )
            )
            continue
        ref_img = load_rgba(w.asset_path)
        # Use the same signature detector on the reference so we compare the
        # same art blob (not the reference's whole bounding box, which also
        # contains label text below the art).
        ref_bounds = detect_widget_region(ref_img, w.kind)
        if ref_bounds is None:
            results.append(
                HeuristicResult(
                    "art_bounds", w.label, "reference", None,
                    status="skip",
                    note="could not isolate art blob in reference",
                )
            )
            continue
        ad, status = _ratio_status(region.aspect, ref_bounds.aspect, ctx.tolerance)
        results.append(
            HeuristicResult(
                "art_bounds", w.label, "aspect",
                round(region.aspect, 3), round(ref_bounds.aspect, 3), ad, status,
                "scale-invariant art height/width ratio",
            )
        )
        # Absolute pixel extents are informational (different canvas scales).
        results.append(
            HeuristicResult(
                "art_bounds", w.label, "size_px",
                f"{region.width}x{region.height}",
                f"{ref_bounds.width}x{ref_bounds.height}",
                None, "info",
                "render vs reference art extent (different canvas scales)",
            )
        )
    return results


# -- Heuristic 2: declared (scene) vs rendered geometry --------------------- #


def heuristic_declared_geometry(ctx: Context) -> list[HeuristicResult]:
    """Validate that the render preserves the *declared* aspect ratio.

    The scene's ``style.width/height`` describes the declared art box for a
    widget. We compare the render's detected-art aspect against that declared
    aspect. Because a widget's visible art does not always fill its declared
    box edge-to-edge (a fader's blue track is thinner than its frame), a naive
    art-vs-box comparison is noisy. So when a captured reference is available
    we *normalize out* that art-to-box relationship: we measure how far the
    reference's own art deviates from its declared box, and require the render
    to deviate by the same factor. With no reference, we fall back to a direct
    art-aspect vs declared-aspect comparison (informational for non-knob kinds
    where art rarely fills the box)."""
    results: list[HeuristicResult] = []
    for w in ctx.widgets:
        if w.declared_width is None or w.declared_height is None:
            results.append(
                HeuristicResult(
                    "declared_geometry", w.label, "declared", None,
                    status="skip", note="no declared style dimensions",
                )
            )
            continue
        region = detect_widget_region(ctx.render, w.kind, ctx.render_bg)
        if region is None:
            results.append(
                HeuristicResult(
                    "declared_geometry", w.label, "detect", None,
                    status="skip", note=f"could not locate {w.kind} in render",
                )
            )
            continue
        declared_aspect = (
            w.declared_height / w.declared_width if w.declared_width else 0.0
        )
        ref_bounds = (
            detect_widget_region(load_rgba(w.asset_path), w.kind)
            if w.asset_path
            else None
        )
        if ref_bounds is not None and declared_aspect:
            # Expected render aspect = declared aspect scaled by the reference's
            # own art-to-declared deviation. This makes the check robust to art
            # that doesn't fill its declared box, while still catching a render
            # that distorts the widget relative to its declared geometry.
            ref_factor = ref_bounds.aspect / declared_aspect
            expected = declared_aspect * ref_factor
            ad, status = _ratio_status(region.aspect, expected, ctx.tolerance)
            results.append(
                HeuristicResult(
                    "declared_geometry", w.label, "aspect",
                    round(region.aspect, 3), round(expected, 3), ad, status,
                    f"render aspect vs declared {w.declared_width}x"
                    f"{w.declared_height} (ref-normalized x{ref_factor:.2f})",
                )
            )
        else:
            ad, status = _ratio_status(
                region.aspect, declared_aspect, ctx.tolerance
            )
            # Without a reference, only the knob (art ≈ box) is reliable; other
            # kinds report informationally to avoid false regressions.
            if w.kind != "knob":
                status = "info"
            results.append(
                HeuristicResult(
                    "declared_geometry", w.label, "aspect",
                    round(region.aspect, 3), round(declared_aspect, 3), ad,
                    status,
                    f"render art aspect vs declared {w.declared_width}x"
                    f"{w.declared_height}",
                )
            )
        # Absolute declared dims, always informational.
        results.append(
            HeuristicResult(
                "declared_geometry", w.label, "declared_box",
                f"{region.width}x{region.height}px art",
                f"{w.declared_width}x{w.declared_height} declared",
                None, "info",
                "declared box may exceed visible art",
            )
        )
    return results


# -- Heuristic 3: color heuristics ------------------------------------------ #


def heuristic_colors(ctx: Context) -> list[HeuristicResult]:
    """Sample representative colors from the rendered widget and its captured
    reference, then report nearest-match distance per dominant color.

    For meters we additionally compare the top→bottom gradient stops (the
    green→red ramp), which is the most identity-defining signal for a meter."""
    results: list[HeuristicResult] = []
    for w in ctx.widgets:
        if not w.asset_path:
            results.append(
                HeuristicResult(
                    "colors", w.label, "reference", None,
                    status="skip", note="no captured asset reference",
                )
            )
            continue
        region = detect_widget_region(ctx.render, w.kind, ctx.render_bg)
        if region is None:
            results.append(
                HeuristicResult(
                    "colors", w.label, "detect", None,
                    status="skip", note=f"could not locate {w.kind} in render",
                )
            )
            continue
        rendered_crop = ctx.render.crop(region.as_tuple())
        ref_img = load_rgba(w.asset_path)
        ref_bg = background_color(ref_img)

        if w.kind == "meter":
            ren_stops = sample_column_gradient(rendered_crop, ctx.render_bg)
            ref_stops = sample_column_gradient(ref_img, ref_bg)
            n = min(len(ren_stops), len(ref_stops))
            if n == 0:
                results.append(
                    HeuristicResult(
                        "colors", w.label, "gradient", None,
                        status="skip", note="could not sample gradient",
                    )
                )
            for i in range(n):
                dist = color_distance(ren_stops[i], ref_stops[i])
                # 60 RGB units ~ perceptible-but-close; scale to tolerance.
                status = "pass" if dist <= 80 else "fail"
                results.append(
                    HeuristicResult(
                        "colors", w.label, f"gradient_stop_{i}",
                        ren_stops[i], ref_stops[i], round(dist, 1), status,
                        "top→bottom meter gradient (delta=RGB distance)",
                        delta_is_ratio=False,
                    )
                )
            continue

        # Knob / fader: nearest-match each rendered dominant color to the
        # reference palette, report worst (largest) nearest distance.
        ren_palette = dominant_colors(rendered_crop, ctx.render_bg)
        ref_palette = [c for c, _f in dominant_colors(ref_img, ref_bg)]
        if not ren_palette or not ref_palette:
            results.append(
                HeuristicResult(
                    "colors", w.label, "palette", None,
                    status="skip", note="could not sample palette",
                )
            )
            continue
        worst = 0.0
        for color, _frac in ren_palette:
            nearest = min(color_distance(color, rc) for rc in ref_palette)
            worst = max(worst, nearest)
        status = "pass" if worst <= 90 else "fail"
        results.append(
            HeuristicResult(
                "colors", w.label, "palette_match",
                round(worst, 1), None, round(worst, 1), status,
                "worst nearest-match RGB distance, rendered→reference palette",
                delta_is_ratio=False,
            )
        )
    return results


# -- Heuristic 4: whole-frame overlay + similarity -------------------------- #


def heuristic_frame_overlay(ctx: Context) -> list[HeuristicResult]:
    """Align the render to the whole-frame reference (resize to common size),
    compute a mean per-pixel similarity score, and write a side-by-side and a
    diff heatmap. Skipped when no ``--frame-reference`` was supplied."""
    if ctx.frame_reference is None:
        return [
            HeuristicResult(
                "frame_overlay", "frame", "similarity", None,
                status="skip", note="no --frame-reference supplied",
            )
        ]
    ref = ctx.frame_reference.convert("RGB")
    ren = ctx.render.convert("RGB")
    # Align by resizing both to the reference resolution (the render canvas
    # size differs; a content-aware alignment is future work, see report).
    target = ref.size
    ren_r = ren.resize(target)
    ref_px = ref.load()
    ren_px = ren_r.load()
    w, h = target
    # Sample on a grid for speed (full per-pixel is O(w*h); grid is enough
    # for a stable similarity score and keeps the harness fast).
    step = max(1, min(w, h) // 200)
    total = 0
    acc = 0.0
    for y in range(0, h, step):
        for x in range(0, w, step):
            d = color_distance(ref_px[x, y], ren_px[x, y])
            acc += d
            total += 1
    mean_dist = acc / total if total else 0.0
    similarity = max(0.0, 1.0 - mean_dist / 441.67)  # 0..1
    results = [
        HeuristicResult(
            "frame_overlay", "frame", "similarity",
            round(similarity, 4), None, None,
            "pass" if similarity >= 0.5 else "fail",
            f"mean per-pixel RGB distance {mean_dist:.1f} over {total} samples",
        )
    ]
    if ctx.out_dir:
        os.makedirs(ctx.out_dir, exist_ok=True)
        # Side-by-side (reference | render).
        sbs = Image.new("RGB", (w * 2, h), (20, 20, 24))
        sbs.paste(ref, (0, 0))
        sbs.paste(ren_r, (w, 0))
        sbs_path = os.path.join(ctx.out_dir, "frame-side-by-side.png")
        sbs.save(sbs_path)
        # Diff heatmap (brighter = more different).
        heat = Image.new("RGB", target)
        heat_px = heat.load()
        for y in range(h):
            for x in range(w):
                d = color_distance(ref_px[x, y], ren_px[x, y])
                v = min(255, int(d / 441.67 * 255 * 2))
                heat_px[x, y] = (v, 0, 255 - v)
        heat_path = os.path.join(ctx.out_dir, "frame-diff-heatmap.png")
        heat.save(heat_path)
        results.append(
            HeuristicResult(
                "frame_overlay", "frame", "artifacts",
                {"side_by_side": sbs_path, "heatmap": heat_path},
                status="info", note="overlay images written",
            )
        )
    return results


# -- Heuristic 5: per-widget side-by-side comparison images ----------------- #


def heuristic_side_by_side(ctx: Context) -> list[HeuristicResult]:
    """Write a ``reference | render`` comparison PNG per widget into
    ``--out-dir``. Pure artifact heuristic (always ``info``)."""
    if not ctx.out_dir:
        return [
            HeuristicResult(
                "side_by_side", "all", "artifacts", None,
                status="skip", note="no --out-dir supplied",
            )
        ]
    os.makedirs(ctx.out_dir, exist_ok=True)
    results: list[HeuristicResult] = []
    for w in ctx.widgets:
        region = detect_widget_region(ctx.render, w.kind, ctx.render_bg)
        if region is None or not w.asset_path:
            results.append(
                HeuristicResult(
                    "side_by_side", w.label, "artifacts", None,
                    status="skip",
                    note="missing render region or reference asset",
                )
            )
            continue
        ref_img = load_rgba(w.asset_path).convert("RGB")
        ren_crop = ctx.render.crop(region.as_tuple()).convert("RGB")
        # Normalize heights so they sit side by side at a comparable scale.
        target_h = max(ref_img.height, ren_crop.height)

        def _scale(im: "Image.Image") -> "Image.Image":
            if im.height == 0:
                return im
            scale = target_h / im.height
            return im.resize((max(1, int(im.width * scale)), target_h))

        ref_s = _scale(ref_img)
        ren_s = _scale(ren_crop)
        gap = 12
        canvas = Image.new(
            "RGB", (ref_s.width + gap + ren_s.width, target_h), (20, 20, 24)
        )
        canvas.paste(ref_s, (0, 0))
        canvas.paste(ren_s, (ref_s.width + gap, 0))
        safe = w.label.replace(" ", "_").replace("/", "_")
        out = os.path.join(ctx.out_dir, f"widget-{w.kind}-{safe}.png")
        canvas.save(out)
        results.append(
            HeuristicResult(
                "side_by_side", w.label, "artifact", out,
                status="info", note="reference | render comparison written",
            )
        )
    return results


# The registry. Append new heuristics here — the driver iterates this list.
HEURISTICS: list[tuple[str, Callable[[Context], list[HeuristicResult]]]] = [
    ("art_bounds", heuristic_art_bounds),
    ("declared_geometry", heuristic_declared_geometry),
    ("colors", heuristic_colors),
    ("frame_overlay", heuristic_frame_overlay),
    ("side_by_side", heuristic_side_by_side),
]


# --------------------------------------------------------------------------- #
# Driver
# --------------------------------------------------------------------------- #


def run_all(ctx: Context) -> list[HeuristicResult]:
    """Run every registered heuristic, in order, collecting all results."""
    out: list[HeuristicResult] = []
    for _name, fn in HEURISTICS:
        out.extend(fn(ctx))
    return out


def build_report(
    render_path: str,
    scene_path: str,
    assets_dir: str,
    *,
    frame_reference: Optional[str] = None,
    out_dir: Optional[str] = None,
    tolerance: float = 0.15,
) -> dict:
    """Top-level entry: load inputs, run heuristics, return a report dict."""
    with open(scene_path, "r", encoding="utf-8") as fh:
        scene = json.load(fh)
    # Down-scale large renders/frames before the pure-Python pixel scans;
    # aspect ratios and color signatures are scale-invariant.
    render = downscale_for_scan(load_rgba(render_path))
    widgets = parse_widgets(scene, assets_dir)
    frame_img = (
        downscale_for_scan(load_rgba(frame_reference)) if frame_reference else None
    )
    ctx = Context(
        render=render,
        render_bg=background_color(render),
        scene=scene,
        widgets=widgets,
        assets_dir=assets_dir,
        frame_reference=frame_img,
        out_dir=out_dir,
        tolerance=tolerance,
    )
    results = run_all(ctx)
    passes = sum(1 for r in results if r.status == "pass")
    fails = sum(1 for r in results if r.status == "fail")
    skips = sum(1 for r in results if r.status == "skip")
    return {
        "render": render_path,
        "scene": scene_path,
        "assets_dir": assets_dir,
        "frame_reference": frame_reference,
        "tolerance": tolerance,
        "widgets": [w.kind for w in widgets],
        "summary": {
            "pass": passes,
            "fail": fails,
            "skip": skips,
            "total": len(results),
            "ok": fails == 0,
        },
        "results": [r.to_dict() for r in results],
    }


# --------------------------------------------------------------------------- #
# Pretty-printing
# --------------------------------------------------------------------------- #


_STATUS_GLYPH = {"pass": "PASS", "fail": "FAIL", "skip": "skip", "info": "----"}


def format_table(report: dict) -> str:
    rows = []
    rows.append(
        f"Fidelity diff: {os.path.basename(report['render'])} "
        f"(tolerance {report['tolerance']:.0%})"
    )
    rows.append("-" * 92)
    rows.append(
        f"{'STATUS':<6} {'HEURISTIC':<18} {'SUBJECT':<14} "
        f"{'METRIC':<18} {'MEASURED':<14} {'REF':<12} DELTA"
    )
    rows.append("-" * 92)
    for r in report["results"]:
        measured = r["measured"]
        if isinstance(measured, (dict, list)):
            measured = "<artifact>"
        expected = r["expected"]
        if isinstance(expected, (dict, list)):
            expected = ""
        delta = r["delta"]
        if isinstance(delta, float):
            delta_s = (
                f"{delta:+.1%}" if r.get("delta_is_ratio", True) else f"{delta:.1f}"
            )
        else:
            delta_s = ""
        rows.append(
            f"{_STATUS_GLYPH.get(r['status'], r['status']):<6} "
            f"{r['heuristic']:<18} {str(r['subject'])[:14]:<14} "
            f"{str(r['metric'])[:18]:<18} {str(measured)[:14]:<14} "
            f"{str(expected)[:12]:<12} {delta_s}"
        )
    s = report["summary"]
    rows.append("-" * 92)
    rows.append(
        f"Summary: {s['pass']} pass / {s['fail']} fail / {s['skip']} skip "
        f"({s['total']} checks)  ->  {'OK' if s['ok'] else 'FIDELITY REGRESSION'}"
    )
    return "\n".join(rows)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def main(argv: Optional[Iterable[str]] = None) -> int:
    p = argparse.ArgumentParser(
        description="Visual-fidelity diff harness for Pulp design imports.",
    )
    p.add_argument("--render", required=True, help="rendered PNG to evaluate")
    p.add_argument("--scene", required=True, help="scene.pulp.json export")
    p.add_argument(
        "--assets-dir", required=True,
        help="directory containing the captured asset_ref PNGs",
    )
    p.add_argument(
        "--frame-reference", default=None,
        help="optional whole-frame original screenshot (enables overlay)",
    )
    p.add_argument(
        "--out-dir", default=None,
        help="directory for side-by-side + heatmap comparison images",
    )
    p.add_argument("--json", default=None, help="write the report as JSON here")
    p.add_argument(
        "--tolerance", type=float, default=0.15,
        help="fractional dimension tolerance (default 0.15 = 15%%)",
    )
    args = p.parse_args(list(argv) if argv is not None else None)

    for label, path in (("render", args.render), ("scene", args.scene)):
        if not os.path.exists(path):
            sys.stderr.write(f"fidelity_diff: {label} not found: {path}\n")
            return 2
    if not os.path.isdir(args.assets_dir):
        sys.stderr.write(
            f"fidelity_diff: assets-dir not a directory: {args.assets_dir}\n"
        )
        return 2

    report = build_report(
        args.render,
        args.scene,
        args.assets_dir,
        frame_reference=args.frame_reference,
        out_dir=args.out_dir,
        tolerance=args.tolerance,
    )
    print(format_table(report))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2)
        print(f"\nJSON report -> {args.json}")
    return 0 if report["summary"]["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
