#!/usr/bin/env python3
"""A family split into unicode-range subsets renders where Chromium put it,
through the native tree and through generated ui.js.

The fixture serves one family as two `@font-face` subsets, the first covering
only Latin Extended and the second basic Latin, and draws Latin text. Both
renderers must register each subset under its own alias, last-defined first,
so every glyph comes from the second subset. Registered under the plain
family, the first subset stands in for it and the text falls back to a host
face. The check is per label, against the Chromium capture (`chromium.png`,
DPR 2):
  - ink ratio (native / Chromium, background-subtracted luminance) near 1.0;
  - ink-centroid dx and dy under 0.1 CSS px.

The oracle is a macOS Chromium raster, so the check runs on macOS only.

Usage: test_design_ir_observe_font_subset_label.py --observer <pulp-design-ir-observe>
           --screenshot <pulp-screenshot>
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

FIXTURE = Path(__file__).resolve().parent / "fixtures/design-ir-observe/font-subset-label"
OBSERVER: Path | None = None
SCREENSHOT: Path | None = None
DPR = 2.0
PAD = 4  # device px around the captured paint box


def decode_png(path: Path) -> tuple[int, int, list[float]]:
    """Width, height and per-pixel luminance of an 8-bit RGB/RGBA PNG."""
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError(f"{path} is not a PNG")
    offset, packed = 8, bytearray()
    width = height = channels = 0
    while offset + 8 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", payload)
            if depth != 8 or interlace != 0 or color not in (2, 6):
                raise ValueError(f"{path}: unsupported PNG format")
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            packed.extend(payload)
        elif kind == b"IEND":
            break
        offset += 12 + length
    raw = zlib.decompress(bytes(packed))
    stride = width * channels
    rows: list[bytearray] = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = prev[i]
            up_left = prev[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif kind == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                line[i] = (line[i] + pred) & 0xFF
        rows.append(line)
        prev = line
    luma = []
    for line in rows:
        for x in range(width):
            r, g, b = line[x * channels:x * channels + 3]
            luma.append(0.2126 * r + 0.7152 * g + 0.0722 * b)
    return width, height, luma


def label_boxes(ir: dict) -> dict[str, tuple[int, int, int, int]]:
    """Device-px paint boxes of the fixture's captured text labels, by anchor."""
    boxes: dict[str, tuple[int, int, int, int]] = {}

    def walk(node: object) -> None:
        if isinstance(node, dict):
            attrs = node.get("attributes", {})
            if node.get("type") == "text" and "browser_box_paint_left_px" in attrs:
                boxes[node["stable_anchor_id"]] = tuple(
                    int(round(float(attrs[f"browser_box_paint_{edge}_px"])))
                    for edge in ("left", "top", "right", "bottom"))
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(ir["root"])
    return boxes


def ink(image: tuple[int, int, list[float]], box: tuple[int, int, int, int]):
    """Background-subtracted ink mass and centroid (device px) inside `box`."""
    width, height, luma = image
    x0, y0, x1, y1 = box
    x0, y0, x1, y1 = max(0, x0 - PAD), max(0, y0 - PAD), min(width, x1 + PAD), min(height, y1 + PAD)
    border = [luma[y * width + x] for x in range(x0, x1) for y in (y0, y1 - 1)]
    border += [luma[y * width + x] for y in range(y0, y1) for x in (x0, x1 - 1)]
    border.sort()
    background = border[len(border) // 2]
    mass = cx = cy = 0.0
    for y in range(y0, y1):
        for x in range(x0, x1):
            value = luma[y * width + x] - background
            if value > 0:
                mass += value
                cx += value * x
                cy += value * y
    return mass, cx / mass if mass else float("nan"), cy / mass if mass else float("nan")


class FontSubsetLabel(unittest.TestCase):
    def check_render(self, render: Path) -> None:
        ir = json.loads((FIXTURE / "design.ir.json").read_text(encoding="utf-8"))
        boxes = label_boxes(ir)
        self.assertEqual(len(boxes), 4, "fixture must carry four captured labels")
        native = decode_png(render)
        chromium = decode_png(FIXTURE / "chromium.png")
        for anchor, box in sorted(boxes.items()):
            with self.subTest(label=anchor.rsplit("/", 2)[-2]):
                c_mass, c_x, c_y = ink(chromium, box)
                n_mass, n_x, n_y = ink(native, box)
                self.assertGreater(c_mass, 0.0)
                self.assertAlmostEqual(n_mass / c_mass, 1.0, delta=0.02)
                self.assertLess(abs(n_x - c_x) / DPR, 0.1)
                self.assertLess(abs(n_y - c_y) / DPR, 0.1)

    def test_native_tree(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            render = Path(tmp) / "render.png"
            proc = subprocess.run(
                [str(OBSERVER), "--input", str(FIXTURE / "design.ir.json"),
                 "--render", str(render), "--layout", str(Path(tmp) / "layout.json"),
                 "--width", "420", "--height", "120", "--scale", "2",
                 "--backend", "skia", "--origin", "0.0,0.0"],
                capture_output=True, text=True, encoding="utf-8", timeout=120)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.check_render(render)

    def test_generated_ui_js(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            render = Path(tmp) / "render.png"
            proc = subprocess.run(
                [str(SCREENSHOT), "--script", str(FIXTURE / "ui.js"), "--output", str(render),
                 "--width", "420", "--height", "120", "--scale", "2", "--backend", "skia"],
                capture_output=True, text=True, encoding="utf-8", timeout=120)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.check_render(render)


def main() -> int:
    global OBSERVER, SCREENSHOT
    parser = argparse.ArgumentParser()
    parser.add_argument("--observer", required=True, type=Path)
    parser.add_argument("--screenshot", required=True, type=Path)
    args, rest = parser.parse_known_args()
    OBSERVER = args.observer
    SCREENSHOT = args.screenshot
    if sys.platform != "darwin":
        print("SKIP: the Chromium oracle is a macOS raster")
        return 77
    result = unittest.main(argv=[sys.argv[0], *rest], exit=False).result
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
