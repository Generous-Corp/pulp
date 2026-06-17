#!/usr/bin/env python3
"""gallery_visual_diff.py — tolerance-based PNG visual-regression diff for the
widget gallery (Design-System-Import-Plan Phase 6a).

The existing tools/import-design/fidelity_diff.py serves the figma-plugin import
lane (it requires a scene.pulp.json + asset_manifest). This is the sibling for
the native gallery / flat reference PNGs the plan calls for: render the gallery
(pulp-widget-gallery), then diff the render against a committed golden with a
per-channel pixel tolerance and an overall changed-fraction threshold.

Why a tolerance and not an exact match: HarfBuzz/Skia/CoreGraphics text
anti-aliasing differs slightly across machines (plan §5), so an exact-pixel
golden would be flaky. We count a pixel as "changed" only when a channel differs
by more than `channel_tol` (0-255), and fail only when the changed fraction
exceeds `--fail-fraction`.

The diff CORE (`changed_fraction`) is pure Python over RGBA tuples and has no
third-party dependency, so it is unit-testable anywhere. PIL is imported lazily
only when actually loading PNG files from disk.
"""
from __future__ import annotations

import argparse
import sys
from typing import List, Sequence, Tuple

RGBA = Tuple[int, int, int, int]


def changed_fraction(a: Sequence[RGBA], b: Sequence[RGBA], channel_tol: int = 8) -> float:
    """Fraction (0..1) of pixels that differ beyond `channel_tol` on any channel.

    Sizes must match; raises ValueError otherwise. Empty input → 0.0.
    """
    if len(a) != len(b):
        raise ValueError(f"pixel count mismatch: {len(a)} vs {len(b)}")
    if not a:
        return 0.0
    changed = 0
    for pa, pb in zip(a, b):
        if (abs(pa[0] - pb[0]) > channel_tol or abs(pa[1] - pb[1]) > channel_tol
                or abs(pa[2] - pb[2]) > channel_tol or abs(pa[3] - pb[3]) > channel_tol):
            changed += 1
    return changed / len(a)


def _load_rgba(path: str) -> Tuple[List[RGBA], Tuple[int, int]]:
    try:
        from PIL import Image  # lazy — only the CLI/file path needs Pillow
    except ImportError:
        sys.stderr.write("gallery_visual_diff: Pillow (PIL) required to load PNGs; "
                         "`pip install pillow`.\n")
        raise SystemExit(3)
    img = Image.open(path).convert("RGBA")
    return list(img.getdata()), img.size


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--render", required=True, help="freshly rendered PNG to evaluate")
    ap.add_argument("--golden", required=True, help="committed golden PNG to compare against")
    ap.add_argument("--channel-tol", type=int, default=8,
                    help="per-channel delta (0-255) below which a pixel is unchanged")
    ap.add_argument("--fail-fraction", type=float, default=0.02,
                    help="fail if more than this fraction of pixels changed (default 2%%)")
    args = ap.parse_args()

    render, rsize = _load_rgba(args.render)
    golden, gsize = _load_rgba(args.golden)
    if rsize != gsize:
        sys.stderr.write(f"gallery_visual_diff: size mismatch render={rsize} golden={gsize}\n")
        return 1
    frac = changed_fraction(render, golden, args.channel_tol)
    pct = frac * 100.0
    if frac > args.fail_fraction:
        sys.stderr.write(f"gallery_visual_diff: DRIFT {pct:.2f}% changed "
                         f"(> {args.fail_fraction * 100:.1f}% threshold) — "
                         f"render={args.render} golden={args.golden}\n")
        return 1
    print(f"gallery_visual_diff: ok — {pct:.2f}% changed (<= {args.fail_fraction * 100:.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
