#!/usr/bin/env python3
"""Measure viewport/background registration for importer differential captures.

This is diagnostic only. It records corner and outside-root pixels so a visual
mismatch can be distinguished from missing html/body/full-size background
propagation. It never changes a score or promotion decision.
"""
from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Iterable

from PIL import Image


def _pixels(image: Image.Image) -> list[tuple[int, int, int, int]]:
    image = image.convert("RGBA")
    getter = getattr(image, "get_flattened_data", None)
    return list(getter() if getter is not None else image.getdata())


def _rgb(pixel: tuple[int, int, int, int]) -> list[int]:
    return list(pixel[:3])


def _mean(pixels: Iterable[tuple[int, int, int, int]]) -> list[float] | None:
    values = list(pixels)
    if not values:
        return None
    return [round(sum(pixel[channel] for pixel in values) / len(values), 3)
            for channel in range(3)]


def measure(reference: Image.Image, candidate: Image.Image,
            root: tuple[int, int, int, int]) -> dict[str, object]:
    ref = reference.convert("RGBA")
    cand = candidate.convert("RGBA")
    if ref.size != cand.size:
        raise ValueError(f"image dimensions differ: {ref.size} vs {cand.size}")
    x, y, width, height = root
    if width <= 0 or height <= 0 or x < 0 or y < 0 or x + width > ref.width or y + height > ref.height:
        raise ValueError(f"root bounds {root} lie outside image {ref.size}")
    ref_px = _pixels(ref)
    cand_px = _pixels(cand)
    outside_indices = [
        index for index in range(len(ref_px))
        if not (x <= index % ref.width < x + width
                and y <= index // ref.width < y + height)
    ]
    corners = {
        name: (cx, cy)
        for name, (cx, cy) in {
            "top_left": (0, 0), "top_right": (ref.width - 1, 0),
            "bottom_left": (0, ref.height - 1),
            "bottom_right": (ref.width - 1, ref.height - 1),
        }.items()
    }
    corner_report = {
        name: {"reference": _rgb(ref.getpixel((cx, cy))),
               "candidate": _rgb(cand.getpixel((cx, cy)))}
        for name, (cx, cy) in corners.items()
    }
    ref_outside = [ref_px[index] for index in outside_indices]
    cand_outside = [cand_px[index] for index in outside_indices]
    return {
        "schema": "pulp-viewport-background-diagnostic-v1",
        "image": {"width": ref.width, "height": ref.height},
        "root_bounds": {"x": x, "y": y, "width": width, "height": height},
        "corners": corner_report,
        "outside_root": {
            "pixel_count": len(outside_indices),
            "reference_modal_rgb": _rgb(Counter(ref_outside).most_common(1)[0][0]) if ref_outside else None,
            "candidate_modal_rgb": _rgb(Counter(cand_outside).most_common(1)[0][0]) if cand_outside else None,
            "reference_mean_rgb": _mean(ref_outside),
            "candidate_mean_rgb": _mean(cand_outside),
            "differing_pixels": sum(a != b for a, b in zip(ref_outside, cand_outside)),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--root-bounds", nargs=4, type=int, required=True,
                        metavar=("X", "Y", "WIDTH", "HEIGHT"))
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    try:
        result = measure(Image.open(args.reference), Image.open(args.candidate), tuple(args.root_bounds))
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
