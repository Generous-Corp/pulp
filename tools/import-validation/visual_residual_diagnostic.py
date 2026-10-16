#!/usr/bin/env python3
"""Decompose a Chromium/native image residual into diagnostic regions.

This tool is deliberately advisory.  It does not change the importer score or
make a parity decision.  It classifies differing pixels as an edge-band
residual, shared-foreground interior color differences, mask/placement
disagreement, or outside-root registration differences.  The labels are
heuristics, not causal proof that a renderer setting is responsible.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Iterable

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover
    raise SystemExit("visual_residual_diagnostic: Pillow is required") from exc


Pixel = tuple[int, int, int, int]
Point = tuple[int, int]


def _pixels(image: Image.Image) -> list[Pixel]:
    image = image.convert("RGBA")
    getter = getattr(image, "get_flattened_data", None)
    return list(getter() if getter is not None else image.getdata())


def _modal_rgb(pixels: Iterable[Pixel]) -> tuple[int, int, int]:
    counts: dict[tuple[int, int, int], int] = {}
    for red, green, blue, alpha in pixels:
        if alpha < 16:
            continue
        rgb = (red, green, blue)
        counts[rgb] = counts.get(rgb, 0) + 1
    return max(counts.items(), key=lambda item: item[1])[0] if counts else (0, 0, 0)


def _distance(a: tuple[int, int, int], b: tuple[int, int, int]) -> float:
    return math.sqrt(sum((a[index] - b[index]) ** 2 for index in range(3)))


def _foreground_mask(
    pixels: list[Pixel], width: int, background: tuple[int, int, int], threshold: float
) -> set[Point]:
    return {
        (index % width, index // width)
        for index, (red, green, blue, alpha) in enumerate(pixels)
        if alpha >= 16 and _distance((red, green, blue), background) > threshold
    }


def _edge_band(mask: set[Point], width: int) -> set[Point]:
    if not mask:
        return set()
    boundary = {
        (x, y)
        for x, y in mask
        if any((x + dx, y + dy) not in mask
               for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)))
    }
    band: set[Point] = set()
    for x, y in boundary:
        for dx in range(-width, width + 1):
            for dy in range(-width, width + 1):
                band.add((x + dx, y + dy))
    return band


def _root_predicate(root: tuple[int, int, int, int] | None):
    if root is None:
        return lambda _x, _y: True
    x, y, width, height = root
    return lambda px, py: x <= px < x + width and y <= py < y + height


def measure(
    reference: Image.Image,
    candidate: Image.Image,
    *,
    root: tuple[int, int, int, int] | None = None,
    edge_width: int = 1,
    foreground_threshold: float = 12.0,
    difference_threshold: int = 0,
) -> dict[str, object]:
    """Return a stable residual decomposition for equal-sized RGBA images."""
    if edge_width < 0 or difference_threshold < 0:
        raise ValueError("edge_width and difference_threshold must be non-negative")
    ref = reference.convert("RGBA")
    cand = candidate.convert("RGBA")
    if ref.size != cand.size:
        raise ValueError(f"image dimensions differ: {ref.size} vs {cand.size}")
    if root is not None:
        x, y, width, height = root
        if width <= 0 or height <= 0 or x < 0 or y < 0 or x + width > ref.width or y + height > ref.height:
            raise ValueError(f"root bounds {root} lie outside image {ref.size}")

    ref_pixels, cand_pixels = _pixels(ref), _pixels(cand)
    ref_background = _modal_rgb(ref_pixels)
    cand_background = _modal_rgb(cand_pixels)
    ref_mask = _foreground_mask(ref_pixels, ref.width, ref_background, foreground_threshold)
    cand_mask = _foreground_mask(cand_pixels, ref.width, cand_background, foreground_threshold)
    edge = _edge_band(ref_mask | cand_mask, edge_width)
    in_root = _root_predicate(root)

    categories = {
        "background_residual": {"pixels": 0, "differing_pixels": 0, "mean_channel_error": 0.0},
        "edge_band_residual": {"pixels": 0, "differing_pixels": 0, "mean_channel_error": 0.0},
        "shared_foreground_interior": {"pixels": 0, "differing_pixels": 0, "mean_channel_error": 0.0},
        "mask_or_placement_interior": {"pixels": 0, "differing_pixels": 0, "mean_channel_error": 0.0},
        "outside_root_registration": {"pixels": 0, "differing_pixels": 0, "mean_channel_error": 0.0},
    }
    error_sums = {name: 0 for name in categories}
    for index, (ref_pixel, cand_pixel) in enumerate(zip(ref_pixels, cand_pixels)):
        px, py = index % ref.width, index // ref.width
        delta = sum(abs(ref_pixel[channel] - cand_pixel[channel]) for channel in range(4)) / 4
        alpha_only = (ref_pixel[:3] == cand_pixel[:3] and ref_pixel[3] != cand_pixel[3])
        differs = delta > difference_threshold
        if not in_root(px, py):
            category = "outside_root_registration"
        elif (_distance(ref_pixel[:3], ref_background) <= foreground_threshold
              and _distance(cand_pixel[:3], cand_background) <= foreground_threshold):
            category = "background_residual"
        elif (px, py) in edge:
            category = "edge_band_residual"
        elif (px, py) in ref_mask and (px, py) in cand_mask:
            category = "shared_foreground_interior"
        else:
            category = "mask_or_placement_interior"
        row = categories[category]
        row["pixels"] += 1
        if differs:
            row["differing_pixels"] += 1
            error_sums[category] += delta
        if alpha_only:
            row.setdefault("alpha_only_differing_pixels", 0)
            row["alpha_only_differing_pixels"] += 1

    for name, row in categories.items():
        differing = int(row["differing_pixels"])
        row["mean_channel_error"] = round(error_sums[name] / differing, 6) if differing else 0.0
        row["difference_fraction"] = round(differing / int(row["pixels"]), 6) if row["pixels"] else 0.0
        row.setdefault("alpha_only_differing_pixels", 0)

    return {
        "schema": "pulp-visual-residual-diagnostic-v1",
        "image": {"width": ref.width, "height": ref.height},
        "root_bounds": None if root is None else {"x": root[0], "y": root[1], "width": root[2], "height": root[3]},
        "parameters": {"edge_width": edge_width, "foreground_threshold": foreground_threshold, "difference_threshold": difference_threshold},
        "background_rgb": {"reference": list(ref_background), "candidate": list(cand_background)},
        "categories": categories,
        "mask": {"reference_foreground_pixels": len(ref_mask), "candidate_foreground_pixels": len(cand_mask)},
        "classification_note": "heuristic regions; not causal renderer attribution",
        "diagnostic_only": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--root-bounds", nargs=4, type=int, metavar=("X", "Y", "WIDTH", "HEIGHT"))
    parser.add_argument("--edge-width", type=int, default=1)
    parser.add_argument("--foreground-threshold", type=float, default=12.0)
    parser.add_argument("--difference-threshold", type=int, default=0)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    root = tuple(args.root_bounds) if args.root_bounds else None
    try:
        result = measure(Image.open(args.reference), Image.open(args.candidate), root=root,
                         edge_width=args.edge_width, foreground_threshold=args.foreground_threshold,
                         difference_threshold=args.difference_threshold)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    rendered = json.dumps(result, indent=2) + "\n"
    if args.json:
        args.json.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
