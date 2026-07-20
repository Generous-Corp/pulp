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

Heuristics (each a named row family in the report)
--------------------------------------------------
* ``art_bounds`` — per-widget signature-blob aspect (render vs captured asset),
  plus an info ``full_aspect`` row (housing+fill+thumb extent).
* ``declared_geometry`` — render art aspect vs the scene's declared box.
* ``colors`` — knob/fader dominant-palette match + meter green→red gradient
  stops, both sampled over matched (full-widget / fill) crops.
* ``completeness`` — every scene text + widget must render (presence), and no
  text may overflow its declared width / be clipped at the panel edge (no-wrap).
* ``padding`` — panel edge → first child gap vs the scene's ``layout.padding``;
  flags content hugging the wall.
* ``widget_detail`` — fader track/housing stroke presence, knob indicator
  angle vs the reference, meter housing + warm→cool gradient ramp.
* ``text_style`` — per-line glyph-height (size proxy) and stroke density
  (weight proxy) vs ``font_size`` / ``font_weight``.
* ``frame_overlay`` — content-aware whole-frame alignment: detect the rounded
  panel in BOTH render and reference, crop to it, scale to a common size, then
  diff + score (a real similarity gate, not a naive whole-image resize-diff).
* ``side_by_side`` — per-widget ``reference | render`` comparison artifacts.

Trustworthiness
---------------
The gate is calibrated against the ground truth: feeding the original frame
reference back in as the render scores 0 fails. A faithful import should land
there; the measured fails on a real import are then trustworthy gaps to fix.

Design notes
------------
* Dependency-light: standard library + Pillow (PIL) only. No network.
* Heuristics live in a small registry (``HEURISTICS``) so new ones can be
  added without touching the driver. Each heuristic is a small, documented,
  individually unit-testable function operating on plain values + PIL images.
* Missing optional inputs degrade gracefully (e.g. no ``--frame-reference``
  skips the whole-frame heuristic rather than erroring; a scene with no text
  nodes / no declared padding skips those heuristics).
* Two coordinate helpers make whole-frame heuristics robust: ``detect_panel``
  finds the rounded UI panel (dark-blob on a light page margin, OR border-ring
  + content on flush dark dead-space), and ``interior_background`` samples the
  modal interior color (not the rounded corners, which leak the page color).
* Known-approximate signals (documented at their call sites): the knob
  ``indicator_angle`` is a coarse ±~15° estimate and the fader ``track_stroke``
  is presence-only (not thickness).

CLI
---
::

    python3 tools/import-design/fidelity_diff.py \\
      --render <render.png> --scene <scene.pulp.json> \\
      --assets-dir <dir with captured asset_ref PNGs> \\
      [--frame-reference <whole-frame.png>] [--out-dir <dir>] \\
      [--json <report.json>] [--tolerance 0.15]

Exit code is ``0`` when every measured heuristic is within tolerance, ``1``
when at least one fails, ``2`` on bad usage / missing inputs, and ``3`` when
NOTHING was measured. Heuristics that could not run (skipped) do not fail the
suite — but a run where *every* heuristic skipped is reported as ``SKIPPED``,
not ``OK``: a suite that measured nothing has no opinion, and printing a pass
over zero measurements is how a dead lens hides in a green pipeline. (The
``.fig`` lane hits this shape today: it stamps ``audio_widget: "none"`` on
every node and carries no ``type == "text"`` nodes, so the widget/text
heuristics have nothing to parse.)
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


def interior_background(img: "Image.Image") -> RGB:
    """Estimate the dominant background by the modal color of an interior grid.

    Unlike :func:`background_color` (which samples the four corners) this samples
    a grid across the whole image and returns the most common color. It is the
    right choice for a ROUNDED panel crop, whose corners are the page showing
    through the corner radius — there the corner sample would wrongly report the
    page color, not the panel fill.
    """
    img = img.convert("RGBA")
    w, h = img.size
    if w == 0 or h == 0:
        return (0, 0, 0)
    px = img.load()
    counts: dict[RGB, int] = {}
    step = max(1, min(w, h) // 40)
    for y in range(0, h, step):
        for x in range(0, w, step):
            r, g, b, a = px[x, y]
            if a < 16:
                continue
            key = (r, g, b)
            counts[key] = counts.get(key, 0) + 1
    if not counts:
        return (0, 0, 0)
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
        # "none" is an explicit OPT-OUT, not a widget kind. A .fig import stamps
        # it on every instance-expanded node so the name heuristic can't paint
        # Pulp's stock knob over the designer's own art. As a plain truthiness
        # test this read the sentinel as a kind and then hunted the render for a
        # pixel signature named "none", which no signature can ever match — so a
        # near-perfect import reported 832 failures, one per opted-out node, and
        # the tool was blind precisely in the mode it most needs to audit.
        if kind and kind != "none":
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


@dataclasses.dataclass
class TextSpec:
    """A single text node lifted from the scene export."""

    content: str
    declared_width: Optional[float]
    declared_height: Optional[float]
    font_size: Optional[float]
    font_weight: Optional[float]
    font_family: Optional[str]
    color: Optional[str]
    node_id: Optional[str]
    # Absolute top-left in Figma frame coordinates (for ordering / locating).
    abs_x: Optional[float]
    abs_y: Optional[float]


def _abs_xy(node: dict) -> tuple[Optional[float], Optional[float]]:
    """Extract absolute (x, y) from a node's figma.absolute_transform.

    The transform is ``[[a, b, e], [c, d, f]]`` (affine); the translation is
    ``(e, f)``."""
    fig = node.get("figma") or {}
    t = fig.get("absolute_transform")
    if not t or len(t) < 2 or len(t[0]) < 3 or len(t[1]) < 3:
        return (None, None)
    try:
        return (float(t[0][2]), float(t[1][2]))
    except (TypeError, ValueError):
        return (None, None)


def parse_texts(scene: dict) -> list[TextSpec]:
    """Collect every ``type == "text"`` node with content + declared style."""
    texts: list[TextSpec] = []

    def visit(node: dict) -> None:
        if not isinstance(node, dict):
            return
        if node.get("type") == "text" and (node.get("content") or node.get("name")):
            style = node.get("style") or {}
            ax, ay = _abs_xy(node)
            texts.append(
                TextSpec(
                    content=node.get("content") or node.get("name") or "",
                    declared_width=style.get("width"),
                    declared_height=style.get("height"),
                    font_size=style.get("font_size"),
                    font_weight=style.get("font_weight"),
                    font_family=style.get("font_family"),
                    color=style.get("color"),
                    node_id=node.get("figma_node_id"),
                    abs_x=ax,
                    abs_y=ay,
                )
            )
        for child in node.get("children", []) or []:
            visit(child)

    root = scene.get("root") or scene
    visit(root)
    return texts


def root_panel_spec(scene: dict) -> tuple[Optional[float], Optional[float], Optional[dict]]:
    """Return the root frame's (width, height, layout.padding) for the
    padding/alignment heuristics."""
    root = scene.get("root") or scene
    style = root.get("style") or {}
    layout = root.get("layout") or {}
    return (style.get("width"), style.get("height"), layout.get("padding"))


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
    (different color, not connected to the art blob) and neighboring widgets
    (different signature)."""
    mask, w, h = _signature_mask(img, kind)
    return _largest_component_bounds(mask, w, h)


# --------------------------------------------------------------------------- #
# Full-widget detection (housing + thumb + fill / whole disc)
# --------------------------------------------------------------------------- #
#
# The *signature* detector above intentionally finds only the kind's colored
# blob: the meter's green→red fill, the fader's blue track, the knob's silver
# disc. That is the right tool when we want a tight, kind-specific anchor — but
# it is the WRONG crop to compare against the captured reference, because the
# reference art for a fader/meter also includes a *dark housing* (the recessed
# slot around the fill) and a thumb cap, and those are NOT in the colored blob.
# Comparing "fill-only render" against "housing+fill reference" is the
# apples-to-oranges bug that both over- and under-reports fidelity.
#
# detect_full_widget fixes this: it anchors on the signature blob, then grows
# the box to include every adjacent non-background pixel (housing, thumb, the
# rest of the disc). The growth is bounded by the widget's DECLARED box from the
# scene (seed_bounds) so it cannot bleed into a neighboring widget or a label.


def _foreground_mask(
    img: "Image.Image", bg: RGB, *, alpha_threshold: int = 16, color_threshold: float = 40.0
) -> tuple[list[bool], int, int]:
    """Flat row-major foreground mask (anything distinct from ``bg``)."""
    img = img.convert("RGBA")
    w, h = img.size
    data = _flat_pixels(img)
    bg_r, bg_g, bg_b = bg
    ct2 = color_threshold * color_threshold
    flat: list[bool] = [False] * (w * h)
    for i, (r, g, b, a) in enumerate(data):
        if a < alpha_threshold:
            continue
        dr, dg, db = r - bg_r, g - bg_g, b - bg_b
        if dr * dr + dg * dg + db * db > ct2:
            flat[i] = True
    return flat, w, h


def detect_full_widget(
    img: "Image.Image",
    kind: str,
    bg: Optional[RGB] = None,
    *,
    seed_bounds: Optional[Bounds] = None,
) -> Optional[Bounds]:
    """Detect a widget's FULL extent (housing + thumb + fill, or whole disc).

    Strategy:
      1. Find the kind's signature blob (the colored part) as an anchor.
      2. Flood-fill the *foreground* mask (everything distinct from the panel
         background) starting from the anchor, so the dark housing slot and the
         silver thumb — which are connected to the fill — are absorbed.
      3. Clip the result to ``seed_bounds`` (the declared box, in the same
         coordinate space) when provided, so growth can't escape into a
         neighboring widget or a label row.

    Falls back to the signature blob when no foreground anchor is found.
    """
    if bg is None:
        bg = background_color(img)
    anchor = detect_widget_region(img, kind, bg)
    if anchor is None:
        return None
    fg, w, h = _foreground_mask(img, bg)
    # Seed flood-fill from a few interior points of the anchor blob.
    ax = (anchor.left + anchor.right) // 2
    seeds = [
        (ax, (anchor.top + anchor.bottom) // 2),
        (ax, anchor.top + 1),
        (ax, anchor.bottom - 1),
    ]
    seen = bytearray(w * h)
    stack: list[tuple[int, int]] = []
    for sx, sy in seeds:
        if 0 <= sx < w and 0 <= sy < h and fg[sy * w + sx] and not seen[sy * w + sx]:
            seen[sy * w + sx] = 1
            stack.append((sx, sy))
    if not stack:
        return anchor
    # Clip bounds: declared box if given, else the whole image.
    if seed_bounds is not None:
        cl, ct, cr, cb = (
            max(0, seed_bounds.left),
            max(0, seed_bounds.top),
            min(w, seed_bounds.right),
            min(h, seed_bounds.bottom),
        )
    else:
        cl, ct, cr, cb = 0, 0, w, h
    left = right = ax
    top = bottom = seeds[0][1]
    while stack:
        x, y = stack.pop()
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
            if not (cl <= nx < cr and ct <= ny < cb):
                continue
            idx = ny * w + nx
            if fg[idx] and not seen[idx]:
                seen[idx] = 1
                stack.append((nx, ny))
    return Bounds(left, top, right + 1, bottom + 1)


# --------------------------------------------------------------------------- #
# Rounded-panel detection (for content-aware whole-frame alignment)
# --------------------------------------------------------------------------- #
#
# Both the render and the frame reference contain the same rounded dark panel,
# but at different offsets/scales and on different outer backgrounds (the
# reference has a light page margin around the panel; the render is flush). A
# naive whole-image resize-and-diff therefore compares panel-against-margin and
# scores garbage. detect_panel finds the largest dark rounded rectangle so both
# images can be cropped to JUST the panel before diffing — a real alignment.


def detect_panel(img: "Image.Image", *, dark_max: int = 90) -> Optional[Bounds]:
    """Bounding box of the rounded UI panel in ``img``.

    Two layouts must work:

      * **Frame reference** — dark panel on a LIGHT page margin. Here the panel
        is the largest dark connected component; the margin is excluded by
        luminance.
      * **Flush render** — the panel sits on dead-space of the *same* dark
        color, so a dark-blob detector would return the whole canvas. But the
        panel is drawn with a faint border RING a few luma steps above its own
        fill (Pulp renders ``border #2e333d`` on ``#14171c``). We detect the
        ring + interior content and take their combined bounding box.

    Strategy: take the dark-blob box (good for the light-margin case) AND a
    "non-empty content/border" box that excludes uniform dead-space, then
    intersect-prefer the tighter, content-bearing one. This makes the crop the
    real panel in both layouts.
    """
    img = img.convert("RGBA")
    w, h = img.size
    if w == 0 or h == 0:
        return None
    data = _flat_pixels(img)
    bg = background_color(img)
    bg_luma = sum(bg) / 3.0
    # --- Dark-blob box (light-margin case). ---
    dark_mask = [[False] * w for _ in range(h)]
    # --- Feature box: border ring + interior content (flush-margin case). ---
    # A feature pixel is one that differs from the dominant background by either
    # being clearly brighter (border ring / glyphs / silver / fill) — the panel
    # interior dead-space is exactly bg, so it is correctly excluded.
    fl = ft = None
    fr = fb = 0
    i = 0
    for y in range(h):
        drow = dark_mask[y]
        for x in range(w):
            r, g, b, a = data[i]
            i += 1
            if a < 16:
                continue
            luma = (r + g + b) / 3.0
            if luma <= dark_max:
                drow[x] = True
            # Feature: distinct from background (border ring is +~10 luma, art is
            # far off). Use a small luma OR color delta so the faint ring counts.
            if (
                abs(luma - bg_luma) > 6
                or color_distance((r, g, b), bg) > 16
            ):
                if fl is None or x < fl:
                    fl = x
                if ft is None or y < ft:
                    ft = y
                if x + 1 > fr:
                    fr = x + 1
                if y + 1 > fb:
                    fb = y + 1
    dark_box = _largest_component_bounds(
        dark_mask, w, h, min_pixels=max(64, (w * h) // 200)
    )
    feature_box = (
        Bounds(fl, ft, fr, fb) if fl is not None and ft is not None else None
    )
    # Prefer whichever is the tighter, sensible panel. When the dark blob spans
    # ~the whole canvas (flush case) the feature box is the real panel; when the
    # margin is light the dark blob already is the panel and the feature box may
    # include faint page texture — take their intersection's tighter side.
    if dark_box is None:
        return feature_box
    if feature_box is None:
        return dark_box
    dark_area = dark_box.width * dark_box.height
    canvas_area = w * h
    if dark_area >= 0.9 * canvas_area:
        # Dark blob is basically the whole canvas -> trust the feature box.
        return feature_box
    return dark_box


# --------------------------------------------------------------------------- #
# Text-region detection (completeness + overflow + weight proxy)
# --------------------------------------------------------------------------- #
#
# We don't OCR. Instead we detect *light glyph pixels on the dark panel* — the
# title is near-white, labels/subtitle are muted gray, all clearly brighter
# than the #14171c panel. A text node's rendered extent is the bounding box of
# the bright-pixel run on the row(s) where the scene says the text lives, and
# its stroke density (lit fraction of that box) is a usable weight proxy.


def _bright_mask(
    img: "Image.Image",
    *,
    min_luma: int = 110,
    bg: Optional[RGB] = None,
    rel_luma: int = 45,
    bg_band: float = 28.0,
) -> tuple[list[bool], int, int]:
    """Row-major mask of glyph pixels.

    A glyph pixel is a low-saturation pixel that stands out from the local
    panel background ``bg``: clearly brighter than it (``rel_luma`` above) while
    NOT being the background itself (``bg_band``). When ``bg`` is omitted we fall
    back to an absolute luminance threshold (``min_luma``).

    The background-relative mode is what lets text detection work on a DARK panel
    (white/grey glyphs on ``#14171c``) without also lighting up a surrounding
    LIGHT page margin — the margin is far from the dark panel bg in luma but is
    *outside* the panel crop the caller passes, and inside the panel the only
    bright-relative pixels are the glyphs and the faint border ring (filtered by
    the run/snap logic).
    """
    img = img.convert("RGBA")
    w, h = img.size
    data = _flat_pixels(img)
    flat = [False] * (w * h)
    if bg is not None:
        bg_luma = (bg[0] * 299 + bg[1] * 587 + bg[2] * 114) // 1000
        for i, (r, g, b, a) in enumerate(data):
            if a < 64:
                continue
            if (max(r, g, b) - min(r, g, b)) >= 60:
                continue  # saturated widget art, not text
            luma = (r * 299 + g * 587 + b * 114) // 1000
            if luma - bg_luma >= rel_luma and color_distance((r, g, b), bg) > bg_band:
                flat[i] = True
        return flat, w, h
    for i, (r, g, b, a) in enumerate(data):
        if a < 64:
            continue
        luma = (r * 299 + g * 587 + b * 114) // 1000
        if luma >= min_luma and (max(r, g, b) - min(r, g, b)) < 60:
            flat[i] = True
    return flat, w, h


def text_run_in_band(
    img: "Image.Image",
    y0: int,
    y1: int,
    *,
    min_luma: int = 110,
    bg: Optional[RGB] = None,
    min_run_px: int = 3,
    snap: bool = True,
    prefer_y: Optional[int] = None,
) -> Optional[Bounds]:
    """Bounding box of the bright-glyph run within the horizontal band
    ``[y0, y1)``. Returns ``None`` when the band is essentially blank (a missing
    text node). ``min_run_px`` filters single-pixel noise.

    When ``snap`` is set the band is treated as a *search window*: we find the
    contiguous cluster of glyph-bearing rows inside it (so a slightly mispredicted
    text Y still locks onto the real glyph run rather than catching only an edge
    row or returning ``None``). This makes presence/overflow robust to the coarse
    scene→render coordinate mapping and to downscale rounding.
    """
    img = img.convert("RGBA")
    w, h = img.size
    y0 = max(0, y0)
    y1 = min(h, y1)
    if y1 <= y0 or w == 0:
        return None
    mask, mw, _mh = _bright_mask(img, min_luma=min_luma, bg=bg)
    # Per-row lit counts within the window.
    row_lit = [
        sum(1 for x in range(w) if mask[y * mw + x]) for y in range(y0, y1)
    ]
    if snap:
        # Enumerate contiguous glyph-row clusters (1-row gaps tolerated). When
        # ``prefer_y`` is given, pick the cluster whose center is NEAREST the
        # predicted text Y — so a tall search window that also overlaps the
        # neighboring text line locks onto the right line rather than the one
        # with the most pixels. Otherwise pick the highest-lit cluster.
        clusters: list[tuple[int, int, int]] = []  # (start, end, lit_sum)
        i = 0
        n = len(row_lit)
        while i < n:
            if row_lit[i] == 0:
                i += 1
                continue
            j = i
            cur = 0
            gap = 0
            while j < n:
                if row_lit[j] == 0:
                    gap += 1
                    if gap > 1:
                        break
                else:
                    gap = 0
                cur += row_lit[j]
                j += 1
            clusters.append((i, j, cur))
            i = j + 1
        if not clusters:
            return None
        if prefer_y is not None:
            target = prefer_y - y0
            best = min(
                clusters,
                key=lambda c: abs((c[0] + c[1]) / 2.0 - target),
            )
        else:
            best = max(clusters, key=lambda c: c[2])
        scan_y0, scan_y1 = y0 + best[0], y0 + best[1]
    else:
        scan_y0, scan_y1 = y0, y1
    left = top = None
    right = bottom = 0
    lit = 0
    for y in range(scan_y0, scan_y1):
        base = y * mw
        for x in range(w):
            if not mask[base + x]:
                continue
            lit += 1
            if left is None or x < left:
                left = x
            if top is None or y < top:
                top = y
            if x + 1 > right:
                right = x + 1
            if y + 1 > bottom:
                bottom = y + 1
    if left is None or (right - left) < min_run_px or lit < min_run_px:
        return None
    return Bounds(left, top, right, bottom)


def stroke_density(
    img: "Image.Image", bounds: Bounds, *, min_luma: int = 110, bg: Optional[RGB] = None
) -> float:
    """Fraction of pixels inside ``bounds`` that are glyph-bright. Heavier
    (bolder / larger) text lights a larger fraction of its box — a coarse but
    useful font-weight / size proxy."""
    if bounds.width == 0 or bounds.height == 0:
        return 0.0
    crop = img.convert("RGBA").crop(bounds.as_tuple())
    mask, _w, _h = _bright_mask(crop, min_luma=min_luma, bg=bg)
    return sum(1 for b in mask if b) / float(len(mask))


def estimate_indicator_angle(img: "Image.Image", bg: RGB) -> Optional[float]:
    """Estimate a knob's pointer angle (degrees, 0°=up, +clockwise).

    The pointer is the lone dark notch drawn ON the silver disc, running from
    near the center outward. We:

      1. Locate the silver disc and its center.
      2. Crop the disc and upscale it to a fixed working size, so the estimate
         is stable whether the knob is rendered at 30px (downscaled whole frame)
         or 170px (captured asset).
      3. Within an INNER annulus (radius 0.2–0.78, avoiding the dark rim) take
         the darkest pixels and compute their angular centroid — the notch
         direction.

    Coarse (±15°) but enough to flag a grossly wrong indicator (e.g. pointing
    down-right vs straight up).
    """
    disc = detect_widget_region(img, "knob", bg)
    if disc is None:
        return None
    radius = min(disc.width, disc.height) / 2.0
    if radius <= 2:
        return None
    # Upscale only very small discs (tiny whole-frame knob) so the notch survives;
    # use nearest-neighbor to preserve the dark notch pixels (bicubic would blur
    # them above the darkness threshold).
    crop = img.convert("RGBA").crop(disc.as_tuple())
    if max(crop.size) < 80:
        s = 80 / max(crop.size)
        crop = crop.resize(
            (max(1, round(crop.width * s)), max(1, round(crop.height * s))),
            Image.NEAREST,
        )
    w, h = crop.size
    px = crop.load()
    cx, cy = w / 2.0, h / 2.0
    rr = min(w, h) / 2.0
    inner2 = (rr * 0.18) ** 2
    outer2 = (rr * 0.82) ** 2
    # Disc-relative darkness: the notch is markedly darker than the silver face.
    # Sample the disc's median luma and treat clearly-below-median as notch.
    samples: list[float] = []
    for y in range(0, h, max(1, h // 30)):
        for x in range(0, w, max(1, w // 30)):
            r, g, b, a = px[x, y]
            if a >= 16:
                samples.append((r + g + b) / 3.0)
    if not samples:
        return None
    samples.sort()
    median = samples[len(samples) // 2]
    dark_max = median * 0.55   # a DARK notch (reference knob style)
    light_min = median + (255 - median) * 0.45  # a LIGHT notch (some skins)
    # Some knob skins draw a DARK notch on the silver face, others a LIGHT one.
    # Vector-sum whichever set of off-median pixels is present in the inner
    # annulus, weighted by how far each is from the disc median. This way the
    # angle is recovered for both styles (and the directional difference between
    # a dark-up notch and a light-down indicator still shows up as a big delta).
    def _centroid(predicate) -> Optional[tuple[float, float, float]]:
        sx = sy = wsum = 0.0
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[x, y]
                if a < 16:
                    continue
                dx, dy = x - cx, y - cy
                d2 = dx * dx + dy * dy
                if d2 < inner2 or d2 > outer2:
                    continue
                luma = (r + g + b) / 3.0
                wgt = predicate(luma)
                if wgt <= 0:
                    continue
                sx += dx * wgt
                sy += dy * wgt
                wsum += wgt
        return (sx, sy, wsum) if wsum > 0 else None

    dark = _centroid(lambda lu: (dark_max - lu + 1.0) if lu <= dark_max else 0.0)
    light = _centroid(lambda lu: (lu - light_min + 1.0) if lu >= light_min else 0.0)
    chosen = max(
        (c for c in (dark, light) if c is not None),
        key=lambda c: c[2],
        default=None,
    )
    if chosen is None:
        return None
    sx, sy, _w = chosen
    # Angle of the weighted notch vector: 0° = up, clockwise positive.
    return math.degrees(math.atan2(sx, -sy))


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
    texts: list[TextSpec]
    assets_dir: str
    frame_reference: Optional["Image.Image"]
    out_dir: Optional[str]
    tolerance: float  # fractional dimension tolerance, e.g. 0.15
    # Root frame geometry (declared) for scene→render coordinate mapping.
    root_width: Optional[float] = None
    root_height: Optional[float] = None
    root_padding: Optional[dict] = None

    def render_panel(self) -> Optional[Bounds]:
        return detect_panel(self.render)

    def scene_to_render(self, fx_px: float, fy_px: float) -> Optional[tuple[float, float]]:
        """Map a *frame-relative* scene coordinate (px from the root frame's
        top-left, as the Figma-plugin export gives child node transforms) to a
        render pixel coordinate, using the detected render panel as the frame
        anchor and the declared root width/height as the frame extent.

        Child ``absolute_transform`` translations in this export are already
        relative to the enclosing frame, so we divide by the root width/height
        directly (no page-origin subtraction). Returns ``None`` when the panel
        or root geometry is unavailable.
        """
        panel = self.render_panel()
        if panel is None or not self.root_width or not self.root_height:
            return None
        fx = fx_px / self.root_width
        fy = fy_px / self.root_height
        return (panel.left + fx * panel.width, panel.top + fy * panel.height)


def _ratio_status(measured: float, expected: float, tol: float) -> tuple[float, str]:
    """Return (fractional_delta, pass/fail) for a measured-vs-expected pair."""
    if expected == 0:
        return (0.0, "info")
    delta = (measured - expected) / expected
    return (delta, "pass" if abs(delta) <= tol else "fail")


def _render_full_widget(ctx: Context, w: WidgetSpec) -> Optional[Bounds]:
    """Detect a widget's FULL extent in the render, seeding the flood-fill clip
    with the widget's DECLARED box (mapped into the render panel) so growth
    cannot bleed into a neighbor. Falls back to an unseeded detection when the
    declared box or panel mapping is unavailable."""
    anchor = detect_widget_region(ctx.render, w.kind, ctx.render_bg)
    if anchor is None:
        return None
    seed = None
    panel = ctx.render_panel()
    if w.declared_width and w.declared_height and panel is not None and ctx.root_width:
        sx = panel.width / ctx.root_width
        cx = (anchor.left + anchor.right) // 2
        cy = (anchor.top + anchor.bottom) // 2
        hw = int(w.declared_width * sx)
        hh = int(w.declared_height * sx)
        # Generous clip box around the anchor center (declared box, padded).
        seed = Bounds(cx - hw, cy - hh, cx + hw, cy + hh)
    return detect_full_widget(ctx.render, w.kind, ctx.render_bg, seed_bounds=seed)


# -- Heuristic 1: per-widget art bounds (reference vs rendered) ------------- #


def heuristic_art_bounds(ctx: Context) -> list[HeuristicResult]:
    """Compare the visible-art bounding box of each rendered widget against its
    captured ``asset_ref`` reference.

    The render and the captured asset PNG are at *different* canvas
    resolutions, so absolute pixel widths/heights are not directly comparable
    and are reported as ``info`` only. The pass/fail signal is the
    scale-invariant **aspect ratio** (height/width of the isolated art blob),
    which should match regardless of render scale. The pass/fail signal uses the
    SIGNATURE blob (the kind's colored part) on both sides — it is the most
    scale- and layout-stable anchor. The FULL-widget extent (housing+fill+thumb)
    is also reported as an ``info`` row so a fill-only-vs-full mismatch is
    visible without making the gate noisy."""
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
        # Same signature detector on both sides for an apples-to-apples blob
        # aspect (not the reference's whole bounding box, which also contains the
        # value-label text below the art).
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
                "render vs reference signature-blob extent (different scales)",
            )
        )
        # Full-widget aspect (housing+fill+thumb / whole disc) — info: a large
        # gap vs the signature aspect flags fill-only-vs-full-art differences
        # (e.g. a fader rendered as a bare track without its housing).
        ren_full = _render_full_widget(ctx, w)
        ref_full = detect_full_widget(ref_img, w.kind)
        if ren_full is not None and ref_full is not None:
            results.append(
                HeuristicResult(
                    "art_bounds", w.label, "full_aspect",
                    round(ren_full.aspect, 3), round(ref_full.aspect, 3),
                    None, "info",
                    "full-widget (housing+fill+thumb) aspect, render vs reference",
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
        region = _render_full_widget(ctx, w)
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
            # Sample the gradient over the FILL region on BOTH sides (the
            # saturated ramp), not the housing — otherwise the asset's dark
            # housing top skews stop 0 and the comparison is apples-to-oranges.
            ren_fill = detect_widget_region(ctx.render, "meter", ctx.render_bg)
            ref_fill = detect_widget_region(ref_img, "meter")
            ren_src = ctx.render.crop(ren_fill.as_tuple()) if ren_fill else rendered_crop
            ref_src = ref_img.crop(ref_fill.as_tuple()) if ref_fill else ref_img
            ren_stops = sample_column_gradient(ren_src, ctx.render_bg)
            ref_stops = sample_column_gradient(ref_src, ref_bg)
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
        # reference palette, report worst (largest) nearest distance. Sample the
        # reference over its FULL widget extent (not the whole asset, whose faint
        # value-label text would pollute the palette).
        ref_full = detect_full_widget(ref_img, w.kind)
        ref_src = ref_img.crop(ref_full.as_tuple()) if ref_full else ref_img
        ren_palette = dominant_colors(rendered_crop, ctx.render_bg)
        ref_palette = [c for c, _f in dominant_colors(ref_src, ref_bg)]
        if not ren_palette or not ref_palette:
            results.append(
                HeuristicResult(
                    "colors", w.label, "palette", None,
                    status="skip", note="could not sample palette",
                )
            )
            continue
        # Drop trace colors (< MIN_PALETTE_FRAC of the crop) before scoring. A
        # thin anti-aliased rim — e.g. the 1px transition row where the silver
        # thumb cap meets the dark track — quantizes to a mid-grey that is not
        # part of the widget's identity palette, yet a single such pixel run
        # would otherwise dominate the worst-nearest-distance and flip the gate.
        # The reference palette is already trace-free (dominant_colors caps at
        # the top buckets), so this makes the comparison symmetric. Keep at least
        # the single most-dominant rendered color so the check never empties out.
        MIN_PALETTE_FRAC = 0.08
        scored = [(c, f) for c, f in ren_palette if f >= MIN_PALETTE_FRAC]
        if not scored:
            scored = ren_palette[:1]
        worst = 0.0
        for color, _frac in scored:
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
    """Content-aware whole-frame alignment + similarity.

    The old version resized the whole render to the whole reference and diffed
    pixel-for-pixel. That scored garbage because the reference has a light page
    margin around the panel while the render is flush — so it compared panel
    against margin. This version:

      1. Detects the rounded dark panel in BOTH images (largest dark blob).
      2. Crops each image to its panel.
      3. Scales both panels to a common size.
      4. Diffs and scores on the aligned panels only.

    That makes ``frame_overlay`` a real gate: a faithful import lands a high
    similarity; a misaligned / missing-content import drops it. Writes an
    aligned side-by-side and a diff heatmap. Skipped without ``--frame-reference``.
    """
    if ctx.frame_reference is None:
        return [
            HeuristicResult(
                "frame_overlay", "frame", "similarity", None,
                status="skip", note="no --frame-reference supplied",
            )
        ]
    ref_full = ctx.frame_reference.convert("RGB")
    ren_full = ctx.render.convert("RGB")
    ref_panel_box = detect_panel(ctx.frame_reference)
    ren_panel_box = detect_panel(ctx.render)
    results: list[HeuristicResult] = []
    if ref_panel_box is None or ren_panel_box is None:
        results.append(
            HeuristicResult(
                "frame_overlay", "frame", "panel_detect", None,
                status="skip",
                note="could not detect rounded panel in render or reference",
            )
        )
        return results
    ref = ref_full.crop(ref_panel_box.as_tuple())
    ren = ren_full.crop(ren_panel_box.as_tuple())
    # Report the detected panel aspect agreement (a quick alignment sanity gate).
    pa_delta, pa_status = _ratio_status(
        ren_panel_box.aspect, ref_panel_box.aspect, ctx.tolerance
    )
    results.append(
        HeuristicResult(
            "frame_overlay", "frame", "panel_aspect",
            round(ren_panel_box.aspect, 3), round(ref_panel_box.aspect, 3),
            pa_delta, pa_status,
            "detected-panel aspect (render vs reference); aligns the crop",
        )
    )
    # Scale both cropped panels to a common comparison size.
    target = (min(ref.width, ren.width), min(ref.height, ren.height))
    target = (max(1, target[0]), max(1, target[1]))
    ref_r = ref.resize(target)
    ren_r = ren.resize(target)
    ref_px = ref_r.load()
    ren_px = ren_r.load()
    w, h = target
    step = max(1, min(w, h) // 200)
    total = 0
    acc = 0.0
    for y in range(0, h, step):
        for x in range(0, w, step):
            acc += color_distance(ref_px[x, y], ren_px[x, y])
            total += 1
    mean_dist = acc / total if total else 0.0
    similarity = max(0.0, 1.0 - mean_dist / 441.67)  # 0..1
    results.append(
        HeuristicResult(
            "frame_overlay", "frame", "similarity",
            round(similarity, 4), None, None,
            # Aligned panels of a faithful import score high; this threshold is
            # intentionally a coarse gate (content differences pull it down).
            "pass" if similarity >= 0.85 else "fail",
            f"aligned-panel mean RGB distance {mean_dist:.1f} over {total} samples",
        )
    )
    if ctx.out_dir:
        os.makedirs(ctx.out_dir, exist_ok=True)
        sbs = Image.new("RGB", (w * 2, h), (20, 20, 24))
        sbs.paste(ref_r, (0, 0))
        sbs.paste(ren_r, (w, 0))
        sbs_path = os.path.join(ctx.out_dir, "frame-side-by-side.png")
        sbs.save(sbs_path)
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
                status="info", note="aligned overlay images written",
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
        # Full-widget crops on both sides so the side-by-side shows comparable
        # art (housing+fill+thumb), not a fill-only sliver next to full art.
        region = _render_full_widget(ctx, w)
        if region is None or not w.asset_path:
            results.append(
                HeuristicResult(
                    "side_by_side", w.label, "artifacts", None,
                    status="skip",
                    note="missing render region or reference asset",
                )
            )
            continue
        ref_src = load_rgba(w.asset_path)
        ref_full = detect_full_widget(ref_src, w.kind)
        ref_img = (
            ref_src.crop(ref_full.as_tuple()) if ref_full else ref_src
        ).convert("RGB")
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


# -- Heuristic 6: completeness — node presence + text overflow -------------- #


def heuristic_completeness(ctx: Context) -> list[HeuristicResult]:
    """Every text node and widget the SCENE declares must actually render, and
    no text may overflow its declared width.

    Presence: for each scene text node we map its declared position into the
    render panel and look for a bright-glyph run in that horizontal band. An
    empty band = a missing label (e.g. the value labels the importer drops).
    For widgets we reuse the signature detector.

    Overflow: a text whose rendered run is wider than its declared
    ``style.width`` (beyond tolerance) means it did not wrap — the no-wrap
    failure the user saw on the subtitle.
    """
    results: list[HeuristicResult] = []
    panel = ctx.render_panel()
    scale_x = (
        panel.width / ctx.root_width
        if panel is not None and ctx.root_width
        else None
    )
    # Operate inside the panel crop using the PANEL's background, so a light page
    # margin around the panel can't masquerade as glyph pixels.
    panel_crop = ctx.render.crop(panel.as_tuple()) if panel is not None else ctx.render
    panel_bg = interior_background(panel_crop) if panel is not None else ctx.render_bg
    for t in ctx.texts:
        snippet = (t.content[:18] + "…") if len(t.content) > 18 else t.content
        mapped = (
            ctx.scene_to_render(t.abs_x, t.abs_y)
            if t.abs_x is not None and t.abs_y is not None
            else None
        )
        if mapped is None or scale_x is None:
            results.append(
                HeuristicResult(
                    "completeness", snippet, "presence", None,
                    status="skip",
                    note="no position / panel mapping for this text",
                )
            )
            continue
        _mx, my = mapped
        my_local = my - panel.top  # panel-local Y
        # Search window: declared text height scaled, plus generous slack on
        # both sides so the coarse coordinate mapping + downscale rounding still
        # lock onto the real glyph line (text_run_in_band snaps within it).
        sy = panel.height / ctx.root_height if ctx.root_height else 1.0
        dh = (t.declared_height or 16) * sy
        slack = int(max(12, dh))
        run = text_run_in_band(
            panel_crop, int(my_local) - slack, int(my_local) + int(dh) + slack,
            bg=panel_bg, prefer_y=int(my_local),
        )
        present = run is not None
        results.append(
            HeuristicResult(
                "completeness", snippet, "presence",
                "found" if present else "MISSING", "expected", None,
                "pass" if present else "fail",
                "scene declares this text; detected glyph run in its band"
                if present else "scene text absent from render (dropped node)",
            )
        )
        if present and t.declared_width:
            declared_px = t.declared_width * scale_x
            ratio = run.width / declared_px if declared_px else 0.0
            # Overflow signal 1: rendered run wider than declared box.
            wider = ratio > (1.0 + max(ctx.tolerance, 0.15))
            # Overflow signal 2 (clip): the rendered run runs to within 2px of
            # the panel's right edge (run is panel-local, so compare to panel
            # width). Text that wrapped would stop short; text that overflowed
            # gets clipped at the container edge. Catches the near-full-width
            # subtitle the user saw bleed off-frame, which a width-ratio alone
            # under-reports (declared ≈ full width).
            right_edge_px = panel.width if panel is not None else ctx.render.width
            clipped = run.right >= right_edge_px - 2 and run.width > declared_px * 0.9
            overflow = wider or clipped
            note = "rendered glyph-run width vs declared style.width (>1 ⇒ no-wrap)"
            if clipped and not wider:
                note += "; run reaches panel right edge ⇒ clipped/overflowed"
            results.append(
                HeuristicResult(
                    "completeness", snippet, "text_width",
                    round(run.width, 0), round(declared_px, 0),
                    round(ratio - 1.0, 3),
                    "fail" if overflow else "pass", note,
                )
            )
    # Widget presence.
    for w in ctx.widgets:
        region = detect_widget_region(ctx.render, w.kind, ctx.render_bg)
        results.append(
            HeuristicResult(
                "completeness", w.label, "widget_presence",
                "found" if region is not None else "MISSING", "expected", None,
                "pass" if region is not None else "fail",
                f"scene declares a {w.kind}; located in render"
                if region is not None else f"{w.kind} not found in render",
            )
        )
    return results


# -- Heuristic 7: padding — panel edge to first child content --------------- #


def heuristic_padding(ctx: Context) -> list[HeuristicResult]:
    """Measure the gap from the panel's left/top edge to its first content and
    compare to the scene's ``layout.padding``. Flags content hugging the edge
    (the knob touching the left wall the user spotted)."""
    panel = ctx.render_panel()
    if panel is None or not ctx.root_padding or not ctx.root_width:
        return [
            HeuristicResult(
                "padding", "panel", "inset", None,
                status="skip",
                note="no panel / declared padding for inset measurement",
            )
        ]
    scale_x = panel.width / ctx.root_width
    scale_y = (
        panel.height / ctx.root_height if ctx.root_height else scale_x
    )
    # Find the first foreground content inside the panel from the left and top.
    # Use the PANEL's own background (sampled from the crop's corners), not the
    # page background — otherwise a dark panel on a light page reads as all
    # foreground and the inset collapses to ~0. Also skip a border-ring margin
    # so the recessed rounded border isn't mistaken for content.
    crop = ctx.render.crop(panel.as_tuple())
    panel_bg = interior_background(crop)
    fg, w, h = _foreground_mask(crop, panel_bg)
    margin = max(2, min(w, h) // 40)  # skip the panel border ring
    # A content column/row must have a small *run* of foreground pixels, so a
    # 1px anti-aliased border speckle doesn't register as content.
    min_run = max(2, min(w, h) // 60)
    left_inset = None
    for x in range(margin, w - margin):
        if sum(1 for y in range(margin, h - margin) if fg[y * w + x]) >= min_run:
            left_inset = x - margin
            break
    top_inset = None
    for y in range(margin, h - margin):
        if sum(1 for x in range(margin, w - margin) if fg[y * w + x]) >= min_run:
            top_inset = y - margin
            break
    results: list[HeuristicResult] = []
    for axis, measured_px, declared, scale in (
        ("left", left_inset, ctx.root_padding.get("left"), scale_x),
        ("top", top_inset, ctx.root_padding.get("top"), scale_y),
    ):
        if measured_px is None or declared is None:
            results.append(
                HeuristicResult(
                    "padding", "panel", f"{axis}_inset", None,
                    status="skip", note="missing measurement or declared value",
                )
            )
            continue
        declared_px = declared * scale
        delta = (
            (measured_px - declared_px) / declared_px if declared_px else 0.0
        )
        # This heuristic targets the HUG bug (content jammed against the panel
        # wall), not exact padding parity — exact parity is noisy because of the
        # rounded border ring and label-baseline offsets. So:
        #   * declared padding >= 8px but content within ~40% of it (or <=4px
        #     absolute) ⇒ FAIL (hugging / padding collapsed).
        #   * otherwise (meaningful padding present) ⇒ PASS.
        if declared_px >= 8 and (measured_px <= 4 or measured_px < declared_px * 0.4):
            status = "fail"
        else:
            status = "pass"
        results.append(
            HeuristicResult(
                "padding", "panel", f"{axis}_inset",
                round(measured_px, 0), round(declared_px, 0), round(delta, 3),
                status,
                f"panel {axis} edge → first content vs declared "
                f"padding.{axis}={declared} (FAIL ⇒ content hugs the edge)",
            )
        )
    return results


# -- Heuristic 8: per-widget visual detail ---------------------------------- #


def _has_track_stroke_above_thumb(crop: "Image.Image", bg: RGB) -> bool:
    """A vertical fader/meter has a dark recessed housing stroke that extends
    ABOVE the colored fill/thumb. Detect it: in the top quarter of the crop,
    is there a vertical run of non-background pixels that is NOT the saturated
    fill color (i.e. the dark housing track)?

    The recessed housing channel is intentionally subtle — it can read either as
    a slightly LIGHTER edge (a drawn stroke) OR as a near-bg DARK slot (a recess
    a hair darker/equal to the panel interior, with a faint low-sat edge). Both
    are "the dark housing track above the thumb" and both should count, so the
    test accepts a low-saturation pixel that is meaningfully distinct from the
    panel bg in EITHER luma direction (not just lighter): keying only on
    "lighter than bg + 12" missed the dark recessed slot that the
    captured Pulp fader/meter draws (track ≈ panel interior luma)."""
    crop = crop.convert("RGBA")
    w, h = crop.size
    if w == 0 or h == 0:
        return False
    px = crop.load()
    top_band = max(1, h // 4)
    cx = w // 2
    bg_luma = sum(bg) / 3.0
    housing = 0
    for y in range(top_band):
        # Scan a few central columns for a dark-but-distinct housing pixel.
        for x in range(max(0, cx - w // 4), min(w, cx + w // 4 + 1)):
            r, g, b, a = px[x, y]
            if a < 64:
                continue
            luma = (r + g + b) / 3.0
            sat = max(r, g, b) - min(r, g, b)
            # Housing: low saturation, not the bright thumb cap, and not the
            # saturated blue/green fill. Distinct from the panel bg in either
            # direction — a lighter drawn edge (luma > bg+12) OR a darker
            # recessed slot / its subtle low-sat rim (|luma-bg| >= 6).
            if sat < 50 and luma < 180 and abs(luma - bg_luma) >= 6:
                housing += 1
                break
    return housing >= top_band // 3


def heuristic_widget_detail(ctx: Context) -> list[HeuristicResult]:
    """Per-widget visual-detail checks beyond color:

      * fader: presence of a dark track/housing stroke above the thumb (the
        missing-fader-stroke gap). Measured on the FULL-widget crop so the
        housing is in frame.
      * knob: indicator-angle estimate vs the captured reference knob.
      * meter: housing + fill-level + gradient presence (full extent vs
        fill-only).
    """
    results: list[HeuristicResult] = []
    for w in ctx.widgets:
        # Full-widget crop (housing + fill + thumb), seeded by the declared box.
        region = _render_full_widget(ctx, w)
        if region is None:
            results.append(
                HeuristicResult(
                    "widget_detail", w.label, "detect", None,
                    status="skip", note=f"could not locate {w.kind}",
                )
            )
            continue
        crop = ctx.render.crop(region.as_tuple())

        if w.kind == "fader":
            # The full-widget region anchors on the colored fill + thumb and
            # stops where the recessed dark track (≈ panel bg) starts — so the
            # track ABOVE the thumb is not in `crop`. Extend the crop UPWARD to
            # the widget's declared-box top (mapped into the render panel) so the
            # housing-above-thumb is in frame for the stroke test. Bounded by the
            # declared height so it can't run into a neighbor / the title row.
            track_crop = crop
            panel = ctx.render_panel()
            if (w.declared_height and panel is not None and ctx.root_width):
                sx = panel.width / ctx.root_width
                hh = int(w.declared_height * sx)
                ext_top = max(0, region.bottom - hh)
                if ext_top < region.top:
                    track_crop = ctx.render.crop(
                        (region.left, ext_top, region.right, region.bottom)
                    )
            ren_track = _has_track_stroke_above_thumb(track_crop, ctx.render_bg)
            ref_track = None
            if w.asset_path:
                ref_img = load_rgba(w.asset_path)
                ref_full = detect_full_widget(ref_img, "fader")
                if ref_full is not None:
                    ref_track = _has_track_stroke_above_thumb(
                        ref_img.crop(ref_full.as_tuple()), background_color(ref_img)
                    )
            status = "info" if ref_track is None else (
                "pass" if ren_track == ref_track else "fail"
            )
            results.append(
                HeuristicResult(
                    "widget_detail", w.label, "track_stroke",
                    "present" if ren_track else "absent",
                    None if ref_track is None else ("present" if ref_track else "absent"),
                    None, status,
                    "dark housing/track stroke above the fader thumb",
                    delta_is_ratio=False,
                )
            )

        elif w.kind == "knob":
            ren_ang = estimate_indicator_angle(ctx.render, ctx.render_bg)
            ref_ang = None
            if w.asset_path:
                ref_img = load_rgba(w.asset_path)
                ref_ang = estimate_indicator_angle(ref_img, background_color(ref_img))
            if ren_ang is None or ref_ang is None:
                results.append(
                    HeuristicResult(
                        "widget_detail", w.label, "indicator_angle",
                        None if ren_ang is None else round(ren_ang, 0),
                        None if ref_ang is None else round(ref_ang, 0),
                        None, "skip",
                        "could not estimate one or both indicator angles",
                        delta_is_ratio=False,
                    )
                )
            else:
                diff = abs(((ren_ang - ref_ang + 180) % 360) - 180)
                results.append(
                    HeuristicResult(
                        "widget_detail", w.label, "indicator_angle",
                        round(ren_ang, 0), round(ref_ang, 0), round(diff, 0),
                        "pass" if diff <= 30 else "fail",
                        "knob pointer angle (deg, 0=up); delta=angular error",
                        delta_is_ratio=False,
                    )
                )

        elif w.kind == "meter":
            # Housing presence: full-widget extent should be meaningfully taller
            # than the colored fill blob alone (housing extends above the fill).
            fill = detect_widget_region(ctx.render, "meter", ctx.render_bg)
            housing_ratio = (
                region.height / fill.height if fill and fill.height else 1.0
            )
            results.append(
                HeuristicResult(
                    "widget_detail", w.label, "housing",
                    round(housing_ratio, 2), None, None, "info",
                    "full-meter height / fill-only height (>1 ⇒ housing above fill)",
                    delta_is_ratio=False,
                )
            )
            # Gradient presence: sample stops over the FILL region (not the full
            # crop, whose dark housing would dilute the color stops). A meter
            # ramps when the TOP is warmer than the BOTTOM — quantified by the
            # red-minus-green balance decreasing top→bottom (warm/red at the top,
            # cool/green at the bottom). A flat single-color fill does not.
            fill_crop = (
                ctx.render.crop(fill.as_tuple()) if fill is not None else crop
            )
            stops = sample_column_gradient(fill_crop, ctx.render_bg)
            warm_top = stops[0][0] - stops[0][1] if stops else 0
            warm_bot = stops[-1][0] - stops[-1][1] if stops else 0
            ramps = len(stops) >= 2 and (warm_top - warm_bot) > 25
            results.append(
                HeuristicResult(
                    "widget_detail", w.label, "gradient_ramp",
                    f"warm Δ {warm_top - warm_bot:+d}" if stops else "n/a",
                    "warm Δ > 25", None,
                    "pass" if ramps else "fail",
                    "meter top warmer than bottom (red→green ramp present)",
                    delta_is_ratio=False,
                )
            )
    return results


# -- Heuristic 9: text style — height / weight proxy ------------------------ #


def heuristic_text_style(ctx: Context) -> list[HeuristicResult]:
    """Compare a rendered-text size/weight proxy against the scene's
    ``font_size`` / ``font_weight``. We use glyph-band height (size proxy) and
    stroke density (weight proxy). Coarse — flags gross mismatches only (e.g. a
    600-weight title rendered as thin 400, or an 18px title rendered tiny)."""
    panel = ctx.render_panel()
    scale_y = (
        panel.height / ctx.root_height
        if panel is not None and ctx.root_height
        else None
    )
    panel_crop = ctx.render.crop(panel.as_tuple()) if panel is not None else ctx.render
    panel_bg = interior_background(panel_crop) if panel is not None else ctx.render_bg
    results: list[HeuristicResult] = []
    # Sort texts top→bottom so the title (largest font) is checkable.
    texts = [t for t in ctx.texts if t.abs_y is not None]
    if not texts:
        return [
            HeuristicResult(
                "text_style", "all", "size", None,
                status="skip",
                note="no positioned text nodes in scene",
            )
        ]
    for t in sorted(texts, key=lambda t: t.abs_y):
        snippet = (t.content[:18] + "…") if len(t.content) > 18 else t.content
        mapped = (
            ctx.scene_to_render(t.abs_x or 0.0, t.abs_y)
            if t.abs_y is not None
            else None
        )
        if mapped is None or scale_y is None or not t.font_size:
            results.append(
                HeuristicResult(
                    "text_style", snippet, "size", None,
                    status="skip", note="no mapping / font_size for this text",
                )
            )
            continue
        _mx, my = mapped
        my_local = my - panel.top if panel is not None else my
        # Search window tall enough to lock onto the glyph line despite coarse
        # mapping; text_run_in_band snaps to the actual glyph rows inside it.
        cap = (t.font_size * 1.6) * scale_y
        slack = int(max(12, cap))
        run = text_run_in_band(
            panel_crop, int(my_local) - slack, int(my_local) + int(cap) + slack,
            bg=panel_bg, prefer_y=int(my_local),
        )
        if run is None:
            results.append(
                HeuristicResult(
                    "text_style", snippet, "size", None,
                    status="skip", note="no glyph run to measure",
                )
            )
            continue
        measured_h = run.height
        expected_h = t.font_size * scale_y  # rough cap+ascender proxy per line
        # Text may legitimately wrap to multiple lines, so the measured band can
        # be an integer multiple of one line. Reduce to a per-line height before
        # comparing (a 2-line subtitle should NOT read as 2x-too-tall).
        lines = max(1, round(measured_h / expected_h)) if expected_h else 1
        per_line = measured_h / lines
        delta, status = _ratio_status(per_line, expected_h, max(ctx.tolerance, 0.4))
        results.append(
            HeuristicResult(
                "text_style", snippet, "glyph_height",
                round(per_line, 0), round(expected_h, 0), delta, status,
                f"per-line glyph height vs font_size {t.font_size}px "
                f"(coarse size proxy; ~{lines} line(s) detected)",
            )
        )
        # Weight proxy: stroke density. Only flag a gross under-weight (a bold
        # title rendered as thin). Reported as info for normal weights.
        density = stroke_density(panel_crop, run, bg=panel_bg)
        if t.font_weight and float(t.font_weight) >= 600:
            wstatus = "pass" if density >= 0.12 else "fail"
            note = "bold text (weight>=600) should light a denser stroke fraction"
        else:
            wstatus = "info"
            note = "stroke density (weight proxy)"
        results.append(
            HeuristicResult(
                "text_style", snippet, "weight_proxy",
                round(density, 3),
                None if not t.font_weight else float(t.font_weight),
                None, wstatus, note, delta_is_ratio=False,
            )
        )
    return results


# The registry. Append new heuristics here — the driver iterates this list.
HEURISTICS: list[tuple[str, Callable[[Context], list[HeuristicResult]]]] = [
    ("art_bounds", heuristic_art_bounds),
    ("declared_geometry", heuristic_declared_geometry),
    ("colors", heuristic_colors),
    ("completeness", heuristic_completeness),
    ("padding", heuristic_padding),
    ("widget_detail", heuristic_widget_detail),
    ("text_style", heuristic_text_style),
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
    texts = parse_texts(scene)
    root_w, root_h, root_pad = root_panel_spec(scene)
    frame_img = (
        downscale_for_scan(load_rgba(frame_reference)) if frame_reference else None
    )
    ctx = Context(
        render=render,
        # Interior modal background, robust to a light page margin around the
        # panel (the corner sampler would mistake that margin for the bg).
        render_bg=interior_background(render),
        scene=scene,
        widgets=widgets,
        texts=texts,
        assets_dir=assets_dir,
        frame_reference=frame_img,
        out_dir=out_dir,
        tolerance=tolerance,
        root_width=root_w,
        root_height=root_h,
        root_padding=root_pad,
    )
    results = run_all(ctx)
    passes = sum(1 for r in results if r.status == "pass")
    fails = sum(1 for r in results if r.status == "fail")
    skips = sum(1 for r in results if r.status == "skip")
    measured = passes + fails
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
            # How many heuristics actually produced a comparison. Zero means
            # every heuristic skipped (or produced only info rows) — the tool
            # measured NOTHING and must not be read as a pass.
            "measured": measured,
            "ok": fails == 0 and measured > 0,
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
    measured = s.get("measured", s["pass"] + s["fail"])
    if s["fail"]:
        verdict = "FIDELITY REGRESSION"
    elif measured:
        verdict = "OK"
    else:
        # Nothing produced a comparison: every heuristic skipped. Say so
        # loudly — an "OK" here would be a lens that lies green while
        # contributing nothing (the fig lane's widget-less/text-less scene
        # shape is exactly this).
        verdict = "SKIPPED — no applicable heuristics for this scene shape"
    rows.append(
        f"Summary: {s['pass']} pass / {s['fail']} fail / {s['skip']} skip "
        f"({s['total']} checks)  ->  {verdict}"
    )
    if not s["fail"] and not measured:
        rows.append("Nothing was measured; every heuristic skipped:")
        seen: set[str] = set()
        for r in report["results"]:
            if r["status"] == "skip" and r.get("note") and r["note"] not in seen:
                seen.add(r["note"])
                rows.append(f"  - {r['heuristic']}: {r['note']}")
        rows.append(
            "Needs a scene with audio_widget/text nodes (the widget, geometry, "
            "color, and text heuristics), a declared root layout.padding, or "
            "--frame-reference/--out-dir for the whole-frame heuristics."
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
    summary = report["summary"]
    if summary["fail"]:
        return 1
    if not summary.get("measured", summary["pass"]):
        # Distinct from both pass (0) and fail (1) and usage error (2): the
        # tool ran but measured nothing, so it has no verdict to offer. A
        # caller must never mistake this for a green result.
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
