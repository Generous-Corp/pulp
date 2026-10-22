#!/usr/bin/env python3
"""Decode frame-identity stamps from a capture sequence.

A frame stamp (test/support/frame_stamp.hpp) paints the update index into
the rendered frame as one row of bit cells and a row holding their
complement. This tool reads that index back out of every captured PNG and
reports whether the screen showed the update that was due at each capture:

    exact      captures showing exactly the due update (and its percentage)
    held       captures repeating the previous capture's update
    skipped    due updates that never appeared, at readable capture slots
    unknown    due updates that never appeared, at unreadable capture slots
    judder     index changes whose interval is more than 0.75 of the refresh
               period off the interval the index advance implies
    regressed  captures showing an older update than the previous capture
    unreadable captures whose stamp could not be read (index -1)

Decoding never guesses. A cell whose luminance is neither clearly black nor
clearly white, or a bit that agrees with its complement, makes the whole
capture unreadable (-1); an unreadable capture is never counted exact and
never contributes a held or skipped verdict.

The due index of capture i is `start + i * step`. `--start-index` defaults
to the value implied by the first readable capture. Without
`--timestamps-json` the captures are taken as one refresh period apart.

Usage:
    python3 -m tools.motion.visual.frame_stamp_decode \\
        --frames-dir ./captures --scale 2 [--json report.json]

Exit codes:
    0  report produced (and --min-exact-pct met, when given)
    1  --min-exact-pct given and not met
    2  bad input (no frames, unreadable file, bad timestamps)
    3  Pillow missing (ctest SKIP_RETURN_CODE)
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence

REPORT_SCHEMA_VERSION = 1

# Luminance thresholds on 0..255, shared with test/support/frame_stamp.cpp.
BLACK_MAX = 64.0
WHITE_MIN = 191.0

# An index change is judder when its interval is this many refresh periods off.
JUDDER_PERIODS = 0.75


@dataclass(frozen=True)
class StampLayout:
    """Grid origin and cell side in logical pixels; defaults match the C++ side."""

    x: float = 0.0
    y: float = 0.0
    cell: float = 8.0
    bits: int = 16


def _cell_luminance(rgba: bytes, width: int, height: int, scale: float,
                    layout: StampLayout, col: int, row: int) -> float:
    """Mean luminance over the central half of one cell, or -1 if outside."""
    import math

    side = layout.cell * scale
    x0 = (layout.x + col * layout.cell) * scale + side * 0.25
    y0 = (layout.y + row * layout.cell) * scale + side * 0.25
    left, top = math.floor(x0), math.floor(y0)
    right, bottom = math.ceil(x0 + side * 0.5), math.ceil(y0 + side * 0.5)
    if left < 0 or top < 0 or right > width or bottom > height:
        return -1.0
    if right <= left or bottom <= top:
        return -1.0
    total = 0.0
    count = 0
    for y in range(top, bottom):
        base = y * width * 4
        for x in range(left, right):
            p = base + x * 4
            total += 0.2126 * rgba[p] + 0.7152 * rgba[p + 1] + 0.0722 * rgba[p + 2]
            count += 1
    return total / count


def decode_rgba(rgba: bytes, width: int, height: int, scale: float,
                layout: StampLayout = StampLayout()) -> int:
    """Decode one frame's stamp from tightly packed RGBA8; -1 when unreadable."""
    if layout.bits <= 0 or layout.bits > 30 or layout.cell <= 0 or scale <= 0:
        return -1
    if len(rgba) < width * height * 4:
        return -1
    value = 0
    for col in range(layout.bits):
        rows = []
        for row in (0, 1):
            lum = _cell_luminance(rgba, width, height, scale, layout, col, row)
            if lum < 0:
                return -1
            if lum >= WHITE_MIN:
                rows.append(True)
            elif lum <= BLACK_MAX:
                rows.append(False)
            else:
                return -1
        if rows[0] == rows[1]:
            return -1
        value = (value << 1) | (1 if rows[0] else 0)
    return value


@dataclass
class StampReport:
    schema: int = REPORT_SCHEMA_VERSION
    captures: int = 0
    readable: int = 0
    unreadable: int = 0
    start_index: Optional[int] = None
    step: int = 1
    exact: int = 0
    exact_pct: float = 0.0
    held: int = 0
    skipped: int = 0
    unknown: int = 0
    judder: int = 0
    regressed: int = 0
    distinct: int = 0
    indices: List[int] = field(default_factory=list)
    problems: List[str] = field(default_factory=list)


def analyze(indices: Sequence[int], step: int = 1,
            start_index: Optional[int] = None,
            timestamps_ms: Optional[Sequence[float]] = None,
            refresh_ms: Optional[float] = None) -> StampReport:
    """Score a decoded index sequence against the due schedule."""
    if step < 1:
        raise ValueError("step must be >= 1")
    if timestamps_ms is not None and len(timestamps_ms) != len(indices):
        raise ValueError("timestamps and captures differ in length")

    report = StampReport(captures=len(indices), step=step, indices=list(indices))
    readable = [(i, v) for i, v in enumerate(indices) if v >= 0]
    report.readable = len(readable)
    report.unreadable = len(indices) - len(readable)
    if not readable:
        report.problems = ["unreadable"] if indices else []
        return report

    if start_index is None:
        first_pos, first_val = readable[0]
        start_index = first_val - first_pos * step
    report.start_index = start_index

    def due(i: int) -> int:
        return start_index + i * step

    report.exact = sum(1 for i, v in readable if v == due(i))
    report.exact_pct = round(100.0 * report.exact / len(indices), 3)
    report.distinct = len({v for _, v in readable})

    for i in range(1, len(indices)):
        prev, cur = indices[i - 1], indices[i]
        if prev < 0 or cur < 0:
            continue
        if cur == prev:
            report.held += 1
        elif cur < prev:
            report.regressed += 1

    shown = {v for _, v in readable}
    for i, v in enumerate(indices):
        if due(i) in shown:
            continue
        if v >= 0:
            report.skipped += 1
        else:
            report.unknown += 1

    # Without timestamps, time is measured in captures and the period is one.
    if timestamps_ms is None:
        times = [float(i) for i in range(len(indices))]
        period = 1.0
    else:
        times = [float(t) for t in timestamps_ms]
        deltas = sorted(b - a for a, b in zip(times, times[1:]))
        median = deltas[len(deltas) // 2] if deltas else 0.0
        period = refresh_ms if refresh_ms is not None else median
    if period > 0:
        last_pos, last_val = readable[0]
        for pos, val in readable[1:]:
            if val == last_val:
                continue
            if val > last_val:
                expected = (val - last_val) / step * period
                actual = times[pos] - times[last_pos]
                if abs(actual - expected) > JUDDER_PERIODS * period:
                    report.judder += 1
            last_pos, last_val = pos, val

    for name in ("held", "skipped", "unknown", "judder", "regressed", "unreadable"):
        if getattr(report, name):
            report.problems.append(name)
    return report


def _load_rgba(path: Path):
    from PIL import Image

    with Image.open(path) as img:
        rgba = img.convert("RGBA")
        return rgba.tobytes(), rgba.width, rgba.height


def decode_files(paths: Sequence[Path], scale: float,
                 layout: StampLayout = StampLayout()) -> List[int]:
    out = []
    for path in paths:
        rgba, width, height = _load_rgba(path)
        out.append(decode_rgba(rgba, width, height, scale, layout))
    return out


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--frames-dir", required=True, type=Path)
    parser.add_argument("--pattern", default="frame_*.png")
    parser.add_argument("--scale", type=float, default=1.0,
                        help="device pixels per logical pixel in the captures")
    parser.add_argument("--x", type=float, default=StampLayout.x)
    parser.add_argument("--y", type=float, default=StampLayout.y)
    parser.add_argument("--cell", type=float, default=StampLayout.cell)
    parser.add_argument("--bits", type=int, default=StampLayout.bits)
    parser.add_argument("--step", type=int, default=1,
                        help="updates the producer advances per capture")
    parser.add_argument("--start-index", type=int, default=None)
    parser.add_argument("--timestamps-json", type=Path, default=None,
                        help="JSON list of capture times in ms, one per frame")
    parser.add_argument("--refresh-ms", type=float, default=None)
    parser.add_argument("--json", type=Path, default=None,
                        help="write the report here (stdout otherwise)")
    parser.add_argument("--min-exact-pct", type=float, default=None)
    args = parser.parse_args(argv)

    try:
        import PIL  # noqa: F401
    except ImportError as exc:
        print(f"frame_stamp_decode: Pillow missing ({exc})", file=sys.stderr)
        return 3

    paths = sorted(args.frames_dir.glob(args.pattern))
    if not paths:
        print(f"frame_stamp_decode: no frames match {args.pattern} in {args.frames_dir}",
              file=sys.stderr)
        return 2
    timestamps = None
    if args.timestamps_json is not None:
        try:
            timestamps = json.loads(args.timestamps_json.read_text())
        except (OSError, ValueError) as exc:
            print(f"frame_stamp_decode: bad timestamps: {exc}", file=sys.stderr)
            return 2
    layout = StampLayout(args.x, args.y, args.cell, args.bits)
    try:
        indices = decode_files(paths, args.scale, layout)
        report = analyze(indices, args.step, args.start_index, timestamps, args.refresh_ms)
    except (OSError, ValueError) as exc:
        print(f"frame_stamp_decode: {exc}", file=sys.stderr)
        return 2

    text = json.dumps(asdict(report), indent=2)
    if args.json is not None:
        args.json.write_text(text + "\n")
    else:
        print(text)
    if args.min_exact_pct is not None and report.exact_pct < args.min_exact_pct:
        print(f"frame_stamp_decode: exact {report.exact_pct}% < {args.min_exact_pct}%",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
