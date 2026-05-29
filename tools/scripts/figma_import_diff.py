#!/usr/bin/env python3
"""figma-import-diff — surface visual deltas between a Figma reference PNG
and a Pulp render PNG, so iterating on the importer doesn't require the
user to eyeball Preview.

Usage:
  figma_import_diff.py <reference.png> <render.png> [--out <diff.png>]
                       [--grid 4x3] [--threshold 16]

Output is a single PNG containing four panels:
   ┌─────────────┬─────────────┐
   │  reference  │   render    │
   ├─────────────┼─────────────┤
   │ pixel-diff  │ region-grid │
   │  heatmap    │   scores    │
   └─────────────┴─────────────┘

Plus a stdout summary:
  - overall MSE + max region delta
  - top-K regions sorted by delta (so you see WHERE to look)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def load_rgb(path: Path) -> np.ndarray:
    img = Image.open(path).convert("RGB")
    return np.asarray(img, dtype=np.uint8)


def fit_to(arr: np.ndarray, target_hw: tuple[int, int]) -> np.ndarray:
    """Resize an RGB ndarray to (h, w) using PIL's high-quality resampling."""
    img = Image.fromarray(arr)
    img = img.resize((target_hw[1], target_hw[0]), Image.LANCZOS)
    return np.asarray(img, dtype=np.uint8)


def pixel_delta(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Per-pixel mean absolute difference, returns uint8 [0..255]."""
    diff = np.mean(np.abs(a.astype(np.int16) - b.astype(np.int16)), axis=2)
    return np.clip(diff, 0, 255).astype(np.uint8)


def heatmap(delta: np.ndarray) -> np.ndarray:
    """Black→red→yellow→white heatmap from a uint8 delta map."""
    norm = delta.astype(np.float32) / 255.0
    h, w = norm.shape
    rgb = np.zeros((h, w, 3), dtype=np.uint8)
    # ramp: 0..0.33 black->red, 0.33..0.66 red->yellow, 0.66..1 yellow->white
    r = np.clip(norm * 3.0, 0, 1)
    g = np.clip((norm - 0.33) * 3.0, 0, 1)
    bch = np.clip((norm - 0.66) * 3.0, 0, 1)
    rgb[..., 0] = (r * 255).astype(np.uint8)
    rgb[..., 1] = (g * 255).astype(np.uint8)
    rgb[..., 2] = (bch * 255).astype(np.uint8)
    return rgb


def region_scores(delta: np.ndarray, grid: tuple[int, int]) -> np.ndarray:
    """Compute mean delta per grid cell. Returns array shaped (rows, cols)."""
    rows, cols = grid
    h, w = delta.shape
    out = np.zeros((rows, cols), dtype=np.float32)
    for r in range(rows):
        for c in range(cols):
            y0 = (r * h) // rows
            y1 = ((r + 1) * h) // rows
            x0 = (c * w) // cols
            x1 = ((c + 1) * w) // cols
            out[r, c] = float(np.mean(delta[y0:y1, x0:x1]))
    return out


def annotate_grid(
    base: np.ndarray, scores: np.ndarray, grid: tuple[int, int]
) -> np.ndarray:
    """Overlay region scores onto a copy of `base`."""
    img = Image.fromarray(base.copy())
    draw = ImageDraw.Draw(img, "RGBA")
    rows, cols = grid
    h, w = base.shape[:2]
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 18)
    except OSError:
        font = ImageFont.load_default()
    smax = max(float(scores.max()), 1.0)
    for r in range(rows):
        for c in range(cols):
            y0 = (r * h) // rows
            y1 = ((r + 1) * h) // rows
            x0 = (c * w) // cols
            x1 = ((c + 1) * w) // cols
            s = float(scores[r, c])
            alpha = int((s / smax) * 180)
            draw.rectangle([x0, y0, x1 - 1, y1 - 1], outline=(255, 0, 0, 200), width=2)
            draw.rectangle(
                [x0, y0, x1 - 1, y1 - 1], fill=(255, 0, 0, alpha)
            )
            txt = f"{s:.0f}"
            draw.text((x0 + 6, y0 + 4), txt, fill=(255, 255, 255, 255), font=font)
    return np.asarray(img, dtype=np.uint8)


def compose_quadrants(panels: list[np.ndarray]) -> np.ndarray:
    """Lay out four equal-size RGB panels in a 2×2 grid with thin separators."""
    assert len(panels) == 4
    h, w = panels[0].shape[:2]
    sep = 4
    out = np.full((h * 2 + sep, w * 2 + sep, 3), 32, dtype=np.uint8)
    out[0:h, 0:w] = panels[0]
    out[0:h, w + sep : 2 * w + sep] = panels[1]
    out[h + sep : 2 * h + sep, 0:w] = panels[2]
    out[h + sep : 2 * h + sep, w + sep : 2 * w + sep] = panels[3]
    return out


def label_panel(arr: np.ndarray, text: str) -> np.ndarray:
    """Draw a black bar at the top of `arr` with the panel label."""
    img = Image.fromarray(arr.copy())
    draw = ImageDraw.Draw(img, "RGBA")
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 22)
    except OSError:
        font = ImageFont.load_default()
    bar_h = 36
    draw.rectangle([0, 0, arr.shape[1], bar_h], fill=(0, 0, 0, 200))
    draw.text((10, 6), text, fill=(255, 255, 255, 255), font=font)
    return np.asarray(img, dtype=np.uint8)


def parse_grid(s: str) -> tuple[int, int]:
    parts = s.lower().split("x")
    if len(parts) != 2:
        raise argparse.ArgumentTypeError(f"--grid must be like 4x3, got {s!r}")
    return int(parts[0]), int(parts[1])


def main() -> int:
    ap = argparse.ArgumentParser(description="figma-import visual diff")
    ap.add_argument("reference", type=Path)
    ap.add_argument("render", type=Path)
    ap.add_argument(
        "--out",
        type=Path,
        default=Path("/tmp/figma-import-diff.png"),
        help="output composite PNG (default /tmp/figma-import-diff.png)",
    )
    ap.add_argument(
        "--grid",
        type=parse_grid,
        default=(4, 4),
        help="region grid rows x cols (default 4x4)",
    )
    ap.add_argument("--threshold", type=float, default=16.0,
                    help="region delta threshold to flag (default 16)")
    ap.add_argument("--top", type=int, default=6,
                    help="print top-K offending regions (default 6)")
    args = ap.parse_args()

    if not args.reference.exists():
        print(f"reference not found: {args.reference}", file=sys.stderr)
        return 2
    if not args.render.exists():
        print(f"render not found: {args.render}", file=sys.stderr)
        return 2

    ref = load_rgb(args.reference)
    rnd = load_rgb(args.render)

    # Normalize both to the SMALLER of the two — downscaling preserves
    # alignment better than upscaling and avoids interpolation halos.
    target_h = min(ref.shape[0], rnd.shape[0])
    target_w = min(ref.shape[1], rnd.shape[1])
    ref_n = fit_to(ref, (target_h, target_w))
    rnd_n = fit_to(rnd, (target_h, target_w))

    delta = pixel_delta(ref_n, rnd_n)
    overall = float(delta.mean())
    overall_max = float(delta.max())

    scores = region_scores(delta, args.grid)

    rows, cols = args.grid
    flat = [
        (float(scores[r, c]), r, c)
        for r in range(rows)
        for c in range(cols)
    ]
    flat.sort(reverse=True)

    print(f"figma-import-diff: {args.reference.name} vs {args.render.name}")
    print(f"  normalized to {target_w}x{target_h}")
    print(f"  overall mean delta: {overall:.2f}  max: {overall_max:.0f}")
    print(f"  region grid {rows}x{cols}  threshold: {args.threshold}")
    print(f"  top {args.top} offending regions (row,col → mean delta):")
    for s, r, c in flat[: args.top]:
        marker = "‼" if s > args.threshold else " "
        print(f"   {marker} ({r},{c}) → {s:6.2f}")

    panels = [
        label_panel(ref_n, "reference (figma)"),
        label_panel(rnd_n, "render (pulp)"),
        label_panel(heatmap(delta), f"pixel-diff (mean={overall:.1f})"),
        label_panel(annotate_grid(rnd_n, scores, args.grid),
                    f"region scores ({rows}x{cols})"),
    ]
    out = compose_quadrants(panels)
    Image.fromarray(out).save(args.out)
    print(f"  wrote {args.out}")

    return 0 if overall <= args.threshold else 1


if __name__ == "__main__":
    sys.exit(main())
